#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"
#include <lwip/dns.h>
#include <unordered_set>

#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 3 // 减少对同一AP的重连次数，更快切换到下一个候选
#define RSSI_FOR_UNSCANNED_AP -100 // 为未扫描到的（可能是隐藏的）AP设置一个低的RSSI值用于排序

struct CandidateAp {
    WifiApRecord record;
    int8_t rssi;
};

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    event_group_ = xEventGroupCreate();
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string& ssid, const std::string& password) {
    SsidManager::GetInstance().AddSsid(ssid, password);
}

void WifiStation::Stop() {
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::WifiEventHandler, this, &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::IpEventHandler, this, &instance_got_ip_));

    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<WifiStation*>(arg);
            if (self->IsConnected() || self->connecting_.load()) {
                return;
            }
            
            ESP_LOGI(TAG, "Scan timer triggered: Starting a new WiFi scan.");
            wifi_scan_config_t scan_config = {};
            scan_config.show_hidden = true;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_config.scan_time.active.min = 300;
            scan_config.scan_time.active.max = 600;
            esp_err_t err = esp_wifi_scan_start(&scan_config, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_scan_start failed (%s), will retry later.", esp_err_to_name(err));
                if (self->timer_handle_ != nullptr) {
                    esp_timer_start_once(self->timer_handle_, pdMS_TO_TICKS(5000));
                }
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
		.skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

    // 将 esp_wifi_start() 放回函数末尾，以保持与上层代码的同步逻辑兼容
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    return (xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    // 静态变量：记录上次是否是定向扫描 & 已经尝试过的目标 SSID 列表
    static bool last_scan_was_targeted = false;
    static std::vector<std::string> tried_targeted;

    uint16_t ap_num = 0;
    esp_err_t rc = esp_wifi_scan_get_ap_num(&ap_num);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(rc));
        if (timer_handle_) esp_timer_start_once(timer_handle_, 5000 * 1000);
        // 不改 last_scan_was_targeted
        return;
    }

    std::vector<wifi_ap_record_t> scanned_aps;
    if (ap_num > 0) {
        scanned_aps.resize(ap_num);
        if (esp_wifi_scan_get_ap_records(&ap_num, scanned_aps.data()) != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed");
            scanned_aps.clear();
        }
    }

    ESP_LOGI(TAG, "HandleScanResult: scanned %d AP records (last_scan_was_targeted=%d)",
             (int)scanned_aps.size(), last_scan_was_targeted ? 1 : 0);

    // 如果这是一轮由“定时器/启动”触发的 full-scan（非定向）并且返回了结果，
    // 我们可以清空 tried_targeted，让下一轮重新尝试未命中的隐藏 SSID。
    if (!last_scan_was_targeted && !scanned_aps.empty()) {
        tried_targeted.clear();
    }

    // -------- 获取已保存的 SSID 列表 --------
    auto& ssid_manager = SsidManager::GetInstance();
    auto saved_list = ssid_manager.GetSsidList();
    if (saved_list.empty()) {
        ESP_LOGW(TAG, "No saved SSIDs.");
        if (timer_handle_) esp_timer_start_once(timer_handle_, 10000 * 1000);
        last_scan_was_targeted = false;
        return;
    }

    // 打印扫描到的 AP（便于诊断：能看出 ZG 是否出现及其 RSSI）
    for (const auto &ap : scanned_aps) {
        size_t ssid_len = strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid));
        std::string ap_ssid(reinterpret_cast<const char*>(ap.ssid), ssid_len);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        ESP_LOGD(TAG, " Scanned AP: SSID='%s' len=%d BSSID=%s chan=%d rssi=%d",
                 ap_ssid.c_str(), (int)ssid_len, mac, ap.primary, ap.rssi);
    }

    // -------- 建立候选列表（优先把所有保存的 SSID 都作为候选） --------
    const int8_t RSSI_FOR_UNSCANNED = -100;
    struct Candidate { WifiApRecord r; int8_t rssi; };
    std::vector<Candidate> candidates;
    candidates.reserve(saved_list.size());
    for (const auto &si : saved_list) {
        Candidate c;
        c.r.ssid = si.ssid;
        c.r.password = si.password;
        c.rssi = RSSI_FOR_UNSCANNED;
        c.r.channel = 0;
        c.r.authmode = WIFI_AUTH_WPA2_PSK;
        memset(c.r.bssid, 0, sizeof(c.r.bssid));
        candidates.push_back(std::move(c));
    }

    // 用扫描结果更新 candidates（同名 SSID 取最高 RSSI）
    for (const auto &ap : scanned_aps) {
        size_t ssid_len = strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid));
        std::string ap_ssid(reinterpret_cast<const char*>(ap.ssid), ssid_len);
        if (ap_ssid.empty()) {
            // 隐藏 SSID（空名字）——打印 BSSID/RSSI 供排查
            char mac[18];
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
            ESP_LOGD(TAG, "Scanned hidden AP BSSID=%s rssi=%d chan=%d", mac, ap.rssi, ap.primary);
            continue;
        }
        for (auto &cand : candidates) {
            if (cand.r.ssid == ap_ssid) {
                if (ap.rssi > cand.rssi) {
                    cand.rssi = ap.rssi;
                    cand.r.channel = ap.primary;
                    cand.r.authmode = ap.authmode;
                    memcpy(cand.r.bssid, ap.bssid, sizeof(ap.bssid));
                }
            }
        }
    }

    // 找到本次扫描中被识别出来的最强 RSSI（如果有）
    int8_t best_scanned_rssi = -127;
    for (const auto &c : candidates) {
        if (c.rssi != RSSI_FOR_UNSCANNED && c.rssi > best_scanned_rssi) {
            best_scanned_rssi = c.rssi;
        }
    }

    // 决定是否需要对未扫描到的保存 SSID 做定向扫描（更强的隐藏 AP）
    const int8_t FAST_CONNECT_THRESHOLD = -80; // 改为 -80，减少过度触发
    bool needs_targeted = false;
    if (best_scanned_rssi == -127) {
        // 未扫描到任何保存的 SSID
        needs_targeted = true;
    } else if (best_scanned_rssi < FAST_CONNECT_THRESHOLD) {
        // 扫描到的最强信号仍然较弱 -> 可能存在强的隐藏 SSID
        needs_targeted = true;
    }

    if (needs_targeted && !connecting_.load() && !IsConnected()) {
        // 收集未被扫描到的 SSID
        std::vector<std::string> unscanned;
        for (const auto &c : candidates) {
            if (c.rssi == RSSI_FOR_UNSCANNED) unscanned.push_back(c.r.ssid);
        }

        // 找第一个还没尝试过的
        std::string to_target;
        for (const auto &s : unscanned) {
            bool tried = false;
            for (const auto &t : tried_targeted) if (t == s) { tried = true; break; }
            if (!tried) { to_target = s; break; }
        }

        if (!to_target.empty()) {
            // 发起定向扫描（对单个 SSID 做 active probe across channels）
            tried_targeted.push_back(to_target);
            ESP_LOGI(TAG, "HandleScanResult: performing QUICK targeted scan for hidden SSID '%s'", to_target.c_str());

            // 标记本次扫描是定向扫描
            last_scan_was_targeted = true;

            wifi_scan_config_t scan_cfg = {};
            // 将 const char* 转成 SDK 需要的 uint8_t*（SDK 不会修改字符串）
            scan_cfg.ssid = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(to_target.c_str()));
            scan_cfg.show_hidden = true;
            scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            // 增加时间提升命中率（针对隐藏 SSID）
            scan_cfg.scan_time.active.min = 300; // ms per channel
            scan_cfg.scan_time.active.max = 600;
            esp_err_t r = esp_wifi_scan_start(&scan_cfg, false);
            if (r != ESP_OK) {
                ESP_LOGW(TAG, "targeted esp_wifi_scan_start failed for '%s': %s", to_target.c_str(), esp_err_to_name(r));
                if (timer_handle_) esp_timer_start_once(timer_handle_, 3000 * 1000);
                // 清除定向标记（下次 full-scan 时才真正清空 tried_targeted）
                last_scan_was_targeted = false;
            }
            return; // 等待定向扫描结果
        }
        // 如果所有 unscanned 都尝试过了，我们就按已有信息去排序连接
    }

    // 排序候选列表（RSSI 高者优先）
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b){
        return a.rssi > b.rssi;
    });

    // 填充 connect_queue_
    connect_queue_.clear();
    ESP_LOGI(TAG, "--- Connection Candidates (best signal first) ---");
    for (const auto &cand : candidates) {
        ESP_LOGI(TAG, "  - SSID: %-20s RSSI: %4d %s",
                 cand.r.ssid.c_str(), cand.rssi,
                 (cand.rssi == RSSI_FOR_UNSCANNED ? "(Unscanned/Hidden)" : ""));
        connect_queue_.push_back(cand.r);
    }
    ESP_LOGI(TAG, "-------------------------------------------------");

    // 清除定向标记（既然我们已处理这次扫描）
    last_scan_was_targeted = false;

    // 如果尚未连接并且没有正在连接，立即尝试最优者
    if (!connect_queue_.empty() && !connecting_.load() && !IsConnected()) {
        StartConnect();
    } else if (connect_queue_.empty()) {
        if (timer_handle_) esp_timer_start_once(timer_handle_, 10000 * 1000);
    }
}

void WifiStation::StartConnect() {
    if (connecting_.load() || connect_queue_.empty()) {
        return;
    }

    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());

    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ap_record.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, ap_record.password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    bool bssid_valid = false;
    for (int i = 0; i < 6; ++i) if (ap_record.bssid[i] != 0) { bssid_valid = true; break; }
    
    if (remember_bssid_ && bssid_valid) {
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
        wifi_config.sta.channel = ap_record.channel;
    }

    connecting_.store(true);
    reconnect_count_ = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Attempting to connect to SSID: '%s'", ssid_.c_str());
    if (esp_wifi_connect() != ESP_OK) {
        connecting_.store(false);
        if (!connect_queue_.empty()) StartConnect(); // 立即尝试下一个
    }
}

int8_t WifiStation::GetRssi() {
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : -127;
}

uint8_t WifiStation::GetChannel() {
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.primary : 0;
}

bool WifiStation::IsConnected() {
    return (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base != WIFI_EVENT) return;
    auto* self = static_cast<WifiStation*>(arg);

    switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START received. scheduling initial scan.");
            // 尝试应用一些运行时配置：关闭省电、设置国家/功率（不影响主线决定）
            ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
            if (self->max_tx_power_ != 0) {
                esp_err_t rc = esp_wifi_set_max_tx_power(self->max_tx_power_);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(rc));
                }
            }

            // 使用 timer 延迟一个短时（500ms）去触发首次扫描，以稳定RF
            if (self->timer_handle_) {
                esp_timer_start_once(self->timer_handle_, 500 * 1000);
            } else {
                // 兜底：直接触发一次快速主动扫描
                wifi_scan_config_t scan_cfg = {};
                scan_cfg.show_hidden = true;
                scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                scan_cfg.scan_time.active.min = 120;
                scan_cfg.scan_time.active.max = 200;
                esp_wifi_scan_start(&scan_cfg, false);
            }

            if (self->on_scan_begin_) self->on_scan_begin_();
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE.");
            // 当已连接时通常不处理全量扫描结果以避免打断；但如果是 targeted scan 的结果（该函数通过 tried_targeted 管理），
            // 也允许 HandleScanResult 去查看更新（HandleScanResult 内有保护）。
            if (!self->IsConnected()) {
                // 直接处理扫描结果（内部会决定是否需要 targeted 并发/返回）
                self->HandleScanResult();
            } else {
                ESP_LOGI(TAG, "Scan done while connected — ignoring to avoid disrupting connection.");
            }
            break;
        }

        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED (link established).");
            // 保持 connecting_ true 直到 IpEventHandler 获得 IP 并清理
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            auto* dis = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            int reason = dis ? dis->reason : -1;
            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED, reason=%d", reason);

            // 清除连接事件位（你的 WaitForConnected/IsConnected 使用该位）
            xEventGroupClearBits(self->event_group_, WIFI_EVENT_CONNECTED);
            self->connecting_.store(false);

            // 尝试少次数的驱动层 reconnect 优先（短等待）
            if (self->reconnect_count_ < MAX_RECONNECT_COUNT) {
                self->reconnect_count_++;
                ESP_LOGI(TAG, "Driver reconnect attempt %d/%d to '%s'", self->reconnect_count_, MAX_RECONNECT_COUNT, self->ssid_.c_str());
                // 小延时以稍微缓冲
                vTaskDelay(pdMS_TO_TICKS(500 * self->reconnect_count_));
                self->connecting_.store(true);
                if (esp_wifi_connect() != ESP_OK) {
                    self->connecting_.store(false);
                }
            } else {
                // 放弃当前 SSID 的自动驱动重连，尝试队列里的下一个候选或重新扫描
                ESP_LOGW(TAG, "Giving up reconnects to '%s' after %d tries.", self->ssid_.c_str(), self->reconnect_count_);
                if (!self->connect_queue_.empty()) {
                    ESP_LOGI(TAG, "Trying next candidate in queue.");
                    self->StartConnect();
                } else {
                    ESP_LOGI(TAG, "Connection queue empty — scheduling a full scan in 5s.");
                    if (self->timer_handle_) esp_timer_start_once(self->timer_handle_, 5000 * 1000);
                }
            }
            break;
        }

        default:
            break;
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    
    char ip_str[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
    self->ip_address_ = ip_str;

    ESP_LOGI(TAG, "Got IP: %s for SSID: %s", self->ip_address_.c_str(), self->ssid_.c_str());

    // 获取IP后，检查并设置DNS服务器
    esp_netif_t *netif = event->esp_netif;
    esp_netif_dns_info_t dns_info;

    // 检查主DNS服务器是否有效
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) != ESP_OK || dns_info.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "DHCP did not provide a primary DNS server. Setting a public DNS server.");
        
        // 设置主DNS为 223.5.5.5 (AliDNS)
        ip_addr_t dns_main;
        IP_ADDR4(&dns_main, 223, 5, 5, 5);
        dns_setserver(0, &dns_main);

        // 设置备用DNS为 114.114.114.114
        ip_addr_t dns_backup;
        IP_ADDR4(&dns_backup, 114, 114, 114, 114);
        dns_setserver(1, &dns_backup);

        ESP_LOGI(TAG, "Manually set DNS servers to 223.5.5.5 and 114.114.114.114");
    } else {
        char dns_ip_str[16];
        esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns_ip_str, sizeof(dns_ip_str));
        ESP_LOGI(TAG, "Primary DNS server provided by DHCP: %s", dns_ip_str);
    }

    xEventGroupSetBits(self->event_group_, WIFI_EVENT_CONNECTED);
    self->connecting_.store(false);
    self->connect_queue_.clear();
    
    if (self->on_connected_) {
        self->on_connected_(self->ssid_);
    }
}