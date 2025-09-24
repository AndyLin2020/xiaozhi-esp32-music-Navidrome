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

    ESP_ERROR_CHECK(esp_wifi_start());
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    return (xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    static std::vector<CandidateAp> candidates;
    static bool last_scan_was_targeted = false;
    static std::string last_targeted_ssid; 

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    std::vector<wifi_ap_record_t> scanned_aps(ap_num);
    if (ap_num > 0) {
        esp_wifi_scan_get_ap_records(&ap_num, scanned_aps.data());
    }

    ESP_LOGI(TAG, "HandleScanResult: scanned %u AP records (targeted_scan=%d)", ap_num, last_scan_was_targeted ? 1 : 0);

    // 如果是新的全信道扫描，清空并重建候选列表
    if (!last_scan_was_targeted) {
        candidates.clear();
        auto saved_list = SsidManager::GetInstance().GetSsidList();
        if (saved_list.empty()) {
            ESP_LOGW(TAG, "No saved SSIDs.");
            if (timer_handle_) esp_timer_start_once(timer_handle_, 10000 * 1000);
            return;
        }
        for (const SsidItem &si : saved_list) {
            CandidateAp cand;
            cand.record.ssid = si.ssid;
            cand.record.password = si.password;
            cand.rssi = RSSI_FOR_UNSCANNED_AP;
            memset(cand.record.bssid, 0, sizeof(cand.record.bssid));
            cand.record.channel = 0;
            cand.record.authmode = WIFI_AUTH_WPA2_PSK;
            candidates.push_back(cand);
        }

        // 用全信道扫描结果填充RSSI值（对能直接匹配 SSID 的 AP 更新真实 RSSI）
        for (const auto &ap : scanned_aps) {
            std::string ap_ssid(reinterpret_cast<const char*>(ap.ssid));
            for (auto &cand : candidates) {
                if (cand.record.ssid == ap_ssid) {
                    cand.rssi = ap.rssi;
                    memcpy(cand.record.bssid, ap.bssid, 6);
                    cand.record.channel = ap.primary;
                    cand.record.authmode = ap.authmode;
                }
            }
        }
    } else { 
        for (const auto &ap : scanned_aps) {
            std::string ap_ssid(reinterpret_cast<const char*>(ap.ssid));
            for (auto &cand : candidates) {
                bool match = false;
                if (!ap_ssid.empty()) {
                    if (cand.record.ssid == ap_ssid) match = true;
                } else {
                    if (!last_targeted_ssid.empty() && cand.record.ssid == last_targeted_ssid) match = true;
                }

                if (match) {
                    cand.rssi = ap.rssi;
                    ESP_LOGI(TAG, "Targeted scan: Hidden/targeted SSID '%s' found. RSSI=%d.",
                             cand.record.ssid.c_str(), cand.rssi);

                    memcpy(cand.record.bssid, ap.bssid, 6);
                    cand.record.channel = ap.primary;
                    cand.record.authmode = ap.authmode;
                }
            }
        }
    }

    if (!last_scan_was_targeted) {
        std::string target_ssid;
        for (const auto& cand : candidates) {
            if (cand.rssi == RSSI_FOR_UNSCANNED_AP) {
                target_ssid = cand.record.ssid;
                break;
            }
        }

        if (!target_ssid.empty() && !connecting_.load() && !IsConnected()) {
            ESP_LOGI(TAG, "Performing targeted scan for hidden SSID '%s'", target_ssid.c_str());
            last_scan_was_targeted = true;
            last_targeted_ssid = target_ssid; 

            wifi_scan_config_t scan_cfg = {};
            scan_cfg.ssid = reinterpret_cast<uint8_t*>(const_cast<char*>(target_ssid.c_str()));
            scan_cfg.show_hidden = true;
            scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_cfg.scan_time.active.min = 300;
            scan_cfg.scan_time.active.max = 600;

            if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
                last_scan_was_targeted = false;
                last_targeted_ssid.clear();
                if (timer_handle_) esp_timer_start_once(timer_handle_, 5000 * 1000);
            }
            return;
        }
    }

    last_scan_was_targeted = false;
    last_targeted_ssid.clear();

    std::sort(candidates.begin(), candidates.end(), [](const CandidateAp &a, const CandidateAp &b) {
        return a.rssi > b.rssi;
    });

    connect_queue_.clear();
    ESP_LOGI(TAG, "--- Connection Candidates (best signal first) ---");
    for (const auto &cand : candidates) {
        ESP_LOGI(TAG, "  - SSID: %-20s RSSI: %4d %s",
                 cand.record.ssid.c_str(), cand.rssi,
                 (cand.rssi == RSSI_FOR_UNSCANNED_AP ? "(Unscanned/Hidden & Not Found)" : ""));
        connect_queue_.push_back(cand.record);
    }
    ESP_LOGI(TAG, "-------------------------------------------------");

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
        if (!connect_queue_.empty()) StartConnect(); 
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

            if (self->timer_handle_) {
                esp_timer_start_once(self->timer_handle_, 500 * 1000);
            } else {
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
            if (!self->IsConnected()) {
                self->HandleScanResult();
            } else {
                ESP_LOGI(TAG, "Scan done while connected — ignoring to avoid disrupting connection.");
            }
            break;
        }

        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED (link established).");
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            auto* dis = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            int reason = dis ? dis->reason : -1;
            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED, reason=%d", reason);

            xEventGroupClearBits(self->event_group_, WIFI_EVENT_CONNECTED);
            self->connecting_.store(false);

            if (self->reconnect_count_ < MAX_RECONNECT_COUNT) {
                self->reconnect_count_++;
                ESP_LOGI(TAG, "Driver reconnect attempt %d/%d to '%s'", self->reconnect_count_, MAX_RECONNECT_COUNT, self->ssid_.c_str());

                vTaskDelay(pdMS_TO_TICKS(500 * self->reconnect_count_));
                self->connecting_.store(true);
                if (esp_wifi_connect() != ESP_OK) {
                    self->connecting_.store(false);
                }
            } else {
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