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

#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 3 // 减少对同一AP的重连次数，更快切换到下一个候选
#define RSSI_FOR_UNSCANNED_AP -100 // 为未扫描到的（可能是隐藏的）AP设置一个低的RSSI值用于排序

// 辅助结构体，用于在 HandleScanResult 中临时存储RSSI以方便排序
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
    
    // 创建定时器，但不立即启动
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

/**
 * @brief [重构] 扫描完成后的核心处理函数
 * 策略: 扫描 -> 评估 -> 排序 -> 连接
 */
void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    auto& ssid_manager = SsidManager::GetInstance();
    auto saved_list = ssid_manager.GetSsidList();
    if (saved_list.empty()) {
        ESP_LOGW(TAG, "No saved SSIDs. Waiting for configuration.");
        return;
    }

    std::vector<wifi_ap_record_t> scanned_aps;
    if (ap_num > 0) {
        scanned_aps.resize(ap_num);
        if (esp_wifi_scan_get_ap_records(&ap_num, scanned_aps.data()) != ESP_OK) {
            scanned_aps.clear();
        }
    }

    // 步骤1: 创建一个包含所有已保存SSID的候选列表
    std::vector<CandidateAp> candidates;
    for (const auto& saved : saved_list) {
        CandidateAp cand;
        cand.record.ssid = saved.ssid;
        cand.record.password = saved.password;
        cand.rssi = RSSI_FOR_UNSCANNED_AP; // 先假设所有都未扫描到
        cand.record.channel = 0;
        cand.record.authmode = WIFI_AUTH_WPA2_PSK;
        memset(cand.record.bssid, 0, 6);
        candidates.push_back(cand);
    }

    // 步骤2: 用扫描结果更新候选列表的真实信号强度 (RSSI) 和其他信息
    for (const auto& scanned_ap : scanned_aps) {
        std::string ssid_str(reinterpret_cast<const char*>(scanned_ap.ssid));
        if (ssid_str.empty()) continue;

        for (auto& cand : candidates) {
            if (cand.record.ssid == ssid_str) {
                // 如果找到匹配的，并且信号比之前记录的强，则更新
                // （这处理了同名SSID有多个AP的情况）
                if (scanned_ap.rssi > cand.rssi) {
                    cand.rssi = scanned_ap.rssi;
                    cand.record.channel = scanned_ap.primary;
                    cand.record.authmode = scanned_ap.authmode;
                    memcpy(cand.record.bssid, scanned_ap.bssid, 6);
                }
            }
        }
    }
    
    // 步骤3: 对所有候选（无论是否扫描到）进行统一排序
    std::sort(candidates.begin(), candidates.end(), 
              [](const CandidateAp& a, const CandidateAp& b){ 
                  return a.rssi > b.rssi; 
              });

    if (candidates.empty()) {
        ESP_LOGW(TAG, "No connectable APs found. Retrying scan later.");
        if (timer_handle_) esp_timer_start_once(timer_handle_, 10000);
        return;
    }

    // 步骤4: 生成连接队列
    connect_queue_.clear();
    ESP_LOGI(TAG, "--- Connection Candidates (best signal first) ---");
    for (const auto& cand : candidates) {
        ESP_LOGI(TAG, "  - SSID: %-20s, RSSI: %d %s", 
                 cand.record.ssid.c_str(), 
                 cand.rssi,
                 (cand.rssi == RSSI_FOR_UNSCANNED_AP ? "(Unscanned/Hidden)" : ""));
        connect_queue_.push_back(cand.record);
    }
    ESP_LOGI(TAG, "-------------------------------------------------");

    // 步骤5: 开始连接
    StartConnect();
}

/**
 * @brief [重构] 尝试连接队列中的下一个候选 AP
 */
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
    auto* self = static_cast<WifiStation*>(arg);

    switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START received. Applying runtime configurations.");

            // 1. 设置节能模式
            ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

            // 2. 设置国家代码
            wifi_country_t country = {.cc = "CN", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_AUTO};
            if (esp_wifi_set_country(&country) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set country code.");
            }

            // 3. 设置发射功率
            int8_t power = 80;
            if (self->max_tx_power_ != 0) {
                power = self->max_tx_power_;
            }
            ESP_LOGI(TAG, "Setting WiFi TX power to %d.", power);
            if (esp_wifi_set_max_tx_power(power) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set max TX power."); 
            }

            // 4. 延迟后启动首次扫描 (保留原有优化)
            ESP_LOGI(TAG, "Scheduling initial scan after a 500ms delay for RF stability.");
            if (self->timer_handle_ != nullptr) {
                esp_timer_start_once(self->timer_handle_, pdMS_TO_TICKS(500));
            }
            if (self->on_scan_begin_) {
                self->on_scan_begin_();
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE.");
            // 只有在未连接且非连接中状态时才处理扫描结果
            if (!self->IsConnected() && !self->connecting_.load()) {
                self->HandleScanResult();
            }
            break;
        }
        
        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED to '%s'.", self->ssid_.c_str());
            self->reconnect_count_ = 0;
            // connecting_ 标志在获取到IP后由 IpEventHandler 清除
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            auto* dis_event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED from '%s', reason: %d", self->ssid_.c_str(), dis_event->reason);
            
            xEventGroupClearBits(self->event_group_, WIFI_EVENT_CONNECTED);
            self->connecting_.store(false);

            // 优先尝试对当前失败的AP进行几次重连
            if (self->reconnect_count_ < MAX_RECONNECT_COUNT) {
                self->reconnect_count_++;
                ESP_LOGI(TAG, "Reconnecting to '%s' (attempt %d/%d)", self->ssid_.c_str(), self->reconnect_count_, MAX_RECONNECT_COUNT);
                vTaskDelay(pdMS_TO_TICKS(1000 * self->reconnect_count_)); // 增加重连的等待时间
                
                self->connecting_.store(true);
                if (esp_wifi_connect() != ESP_OK) {
                    self->connecting_.store(false); // 如果connect调用本身就失败了，立即清除标志
                }
            } else {
                // 如果重连次数耗尽，则尝试连接队列中的下一个候选项
                ESP_LOGW(TAG, "Failed to connect to '%s' after max retries.", self->ssid_.c_str());
                if (!self->connect_queue_.empty()) {
                    ESP_LOGI(TAG, "Trying next candidate in the queue.");
                    self->StartConnect();
                } else {
                    // 如果连接队列也空了，说明所有候选项都失败了，安排一次新的全面扫描
                    ESP_LOGI(TAG, "Connection queue is empty. Triggering a new scan in 5 seconds.");
                    if (self->timer_handle_ != nullptr) {
                        esp_timer_start_once(self->timer_handle_, pdMS_TO_TICKS(5000));
                    }
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

    // [关键修复] 获取IP后，检查并设置DNS服务器
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