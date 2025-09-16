#include "wifi_station.h"
#include <cstring>
#include <algorithm>

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
#include <lwip/inet.h>
#include <arpa/inet.h>  

#define TAG "wifi"
#define WIFI_EVENT_CONNECTED BIT0
#define MAX_RECONNECT_COUNT 5

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %d", err);
    }
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string& ssid, const std::string& password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    
    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_));
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_));
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
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
    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // [信号优化] -----------------------------------------------------
    // 1. 关闭 Wi-Fi 节能模式
    ESP_LOGI(TAG, "Disabling WiFi power save mode for best performance.");
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // 2. 设置国家代码为中国
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    esp_err_t err_country = esp_wifi_set_country(&country);
    if (err_country != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set country code: %s", esp_err_to_name(err_country));
    }

    // 3. 将发射功率设置为最大值
    ESP_LOGI(TAG, "Setting WiFi TX power to maximum (82).");
    esp_err_t err_power = esp_wifi_set_max_tx_power(82);
    if (err_power != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set max TX power: %s", esp_err_to_name(err_power));
    }
    // [信号优化 END] -------------------------------------------------

    // 保留您原有的 NVS 配置逻辑
    if (max_tx_power_ != 0) {
        ESP_LOGI(TAG, "Overriding with NVS TX power setting: %d", max_tx_power_);
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            // [优化] 修改扫描配置以更稳定地发现隐藏网络（增加 active 探测时间）
            wifi_scan_config_t scan_config = {};
            scan_config.ssid = NULL;
            scan_config.bssid = NULL;
            scan_config.channel = 0;
            scan_config.show_hidden = true;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            // 增加 active 探测时间以提高发现隐藏 SSID 的概率
            scan_config.scan_time.active.min = 300; // ms per channel
            scan_config.scan_time.active.max = 600;

            esp_err_t rc = esp_wifi_scan_start(&scan_config, false);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(rc));
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_err_t rc = esp_wifi_scan_get_ap_num(&ap_num);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(rc));
        ap_num = 0;
    }

    if (ap_num == 0) {
        ESP_LOGI(TAG, "No AP found in scan results.");
        // 如果 scan 没发现任何 AP，则尝试直接使用我们已保存的 SSID 发起连接（对隐藏 SSID 很有用）
        auto& ssid_manager = SsidManager::GetInstance();
        auto ssid_list = ssid_manager.GetSsidList();
        for (const auto& item : ssid_list) {
            WifiApRecord record = {};
            record.ssid = item.ssid;
            record.password = item.password;
            record.channel = 0; // unknown
            record.authmode = WIFI_AUTH_WPA2_PSK; // best-effort guess; router will reject if different
            memset(record.bssid, 0, sizeof(record.bssid));
            connect_queue_.push_back(record);
            ESP_LOGI(TAG, "Fallback: queued saved SSID '%s' for direct connect (hidden AP support).", item.ssid.c_str());
        }

        if (connect_queue_.empty()) {
            ESP_LOGI(TAG, "Wait for next scan");
            esp_timer_start_once(timer_handle_, 10 * 1000);
            return;
        } else {
            StartConnect();
            return;
        }
    }

    wifi_ap_record_t *ap_records = (wifi_ap_record_t*)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for ap_records");
        esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }

    rc = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(rc));
        free(ap_records);
        esp_timer_start_once(timer_handle_, 10 * 1000);
        return;
    }

    std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();

    for (int i = 0; i < ap_num; i++) {
        auto& ap_record = ap_records[i];
        std::string found_ssid(reinterpret_cast<const char*>(ap_record.ssid), strnlen(reinterpret_cast<const char*>(ap_record.ssid), sizeof(ap_record.ssid)));
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&found_ssid](const SsidItem& item) {
            return item.ssid == found_ssid;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                found_ssid.c_str(),
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);

            WifiApRecord record = {};
            record.ssid = it->ssid;
            record.password = it->password;
            record.channel = ap_record.primary;
            record.authmode = ap_record.authmode;
            memcpy(record.bssid, ap_record.bssid, sizeof(record.bssid));
            connect_queue_.push_back(record);
        }
    }

    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No known SSID found in scan results; will try saved SSIDs next scan.");
        auto ssid_list2 = ssid_manager.GetSsidList();
        for (const auto& item : ssid_list2) {
            WifiApRecord record = {};
            record.ssid = item.ssid;
            record.password = item.password;
            record.channel = 0;
            record.authmode = WIFI_AUTH_WPA2_PSK;
            memset(record.bssid, 0, sizeof(record.bssid));
            connect_queue_.push_back(record);
        }
        if (connect_queue_.empty()) {
            esp_timer_start_once(timer_handle_, 10 * 1000);
            return;
        }
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    if (connect_queue_.empty()) {
        ESP_LOGW(TAG, "StartConnect called but connect_queue_ empty");
        return;
    }

    if (connecting_.load()) {
        ESP_LOGI(TAG, "StartConnect: already connecting, skipping");
        return;
    }

    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());

    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));

    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ap_record.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password), ap_record.password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    bool has_bssid = false;
    for (int i = 0; i < 6; ++i) {
        if (ap_record.bssid[i] != 0) { has_bssid = true; break; }
    }

    if (remember_bssid_ && has_bssid) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, sizeof(wifi_config.sta.bssid));
        wifi_config.sta.bssid_set = true;
        ESP_LOGI(TAG, "StartConnect: using remembered BSSID for SSID=%s", ap_record.ssid.c_str());
    } else {
        wifi_config.sta.bssid_set = false;
    }

    // 标记正在连接，避免并发
    connecting_.store(true);

    esp_err_t rc = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(rc));
        connecting_.store(false); // 清理
        esp_timer_start_once(timer_handle_, 5000);
        return;
    }

    reconnect_count_ = 0;
    rc = esp_wifi_connect();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(rc));
        connecting_.store(false); // 清理
        esp_timer_start_once(timer_handle_, 5000);
    } else {
        ESP_LOGI(TAG, "Attempting connect to SSID: %s", ssid_.c_str());
        // keep connecting_ == true 直到连接成功或断开事件清理
    }
}

int8_t WifiStation::GetRssi() {
    wifi_ap_record_t ap_info;
    esp_err_t rc = esp_wifi_sta_get_ap_info(&ap_info);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "GetRssi: esp_wifi_sta_get_ap_info failed: %s", esp_err_to_name(rc));
        return 0; 
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    wifi_ap_record_t ap_info;
    esp_err_t rc = esp_wifi_sta_get_ap_info(&ap_info);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "GetChannel: esp_wifi_sta_get_ap_info failed: %s", esp_err_to_name(rc));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    ESP_ERROR_CHECK(esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}

void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);

    // 只处理 WIFI_EVENT 基类的事件
    if (event_base != WIFI_EVENT) return;

    switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START: starting initial scan");
            // 触发一次扫描（发现隐藏网络）
            wifi_scan_config_t scan_config = {
                .ssid = NULL,
                .bssid = NULL,
                .channel = 0,
                .show_hidden = true,
                .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                .scan_time = {
                    .active = { .min = 120, .max = 300 }
                }
            };
            esp_err_t rc = esp_wifi_scan_start(&scan_config, false);
            if (rc != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(rc));
            }
            if (this_->on_scan_begin_) this_->on_scan_begin_();
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE");
            // 处理扫描结果（你的函数内部会决定重试或发起连接）
            this_->HandleScanResult();
            break;
        }

        case WIFI_EVENT_STA_CONNECTED: {
            // 驱动已建立链路（但未必获取到 IP）
            ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
            // 清除 connecting_ 标志，表示驱动已完成连接尝试
            this_->connecting_.store(false);
            // reset reconnect counter (we connected to AP)
            this_->reconnect_count_ = 0;
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            // 获取断开原因（可选用于 debug）
            wifi_event_sta_disconnected_t* dis = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            int reason = dis ? dis->reason : -1;
            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED, reason=%d", reason);

            // 清掉连接标志位
            xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
            // 无论何时断开，都要把 connecting_ 清掉，允许后续重试
            this_->connecting_.store(false);

            // 如果还有重连次数，优先尝试驱动层 reconnect（但先确保不并发）
            if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
                esp_err_t rc = ESP_OK;
                if (!this_->connecting_.load()) {
                    rc = esp_wifi_connect();
                    if (rc == ESP_OK) {
                        this_->connecting_.store(true);
                        this_->reconnect_count_++;
                        ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
                        break;
                    } else {
                        ESP_LOGW(TAG, "esp_wifi_connect failed in reconnect: %s", esp_err_to_name(rc));
                    }
                } else {
                    ESP_LOGI(TAG, "Already connecting, skipping immediate reconnect");
                }
            }

            // 如果连接队列里还有候选 AP，就用 StartConnect 发起下一候选的连接（但避免并发）
            if (!this_->connect_queue_.empty()) {
                if (!this_->connecting_.load()) {
                    // 小延时以避免立刻冲突（给驱动一点缓冲时间）
                    esp_timer_start_once(this_->timer_handle_, 300);
                    // StartConnect 会有自己的 connecting_ 检查
                    // 这里我们直接 schedule，StartConnect 将在 timer 回调触发时运行（如果你没有 timer 回调触发 StartConnect，则直接调用 StartConnect）
                    // 为简单直接：调用 StartConnect，但加保护
                    this_->StartConnect();
                } else {
                    ESP_LOGI(TAG, "connect_queue_ available but already connecting; will try later");
                    esp_timer_start_once(this_->timer_handle_, 500);
                }
                break;
            }

            // 如果没有候选 AP，启动下次周期扫描
            ESP_LOGI(TAG, "No more AP to connect, schedule next scan");
            esp_timer_start_once(this_->timer_handle_, 10 * 1000);
            break;
        }

        default: {
            // ignore other wifi events
            break;
        }
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    // 保存并打印 IPv4 地址
    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

    // ----------------- DNS 诊断 -----------------
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        ESP_LOGW(TAG, "Failed to get netif handle for WIFI_STA_DEF; skipping DNS diagnostics.");
    } else {
        esp_netif_dns_info_t dnsinfo;
        bool main_dns_ok = false;

        // 读取 MAIN DNS（IPv4）
        if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dnsinfo) == ESP_OK) {
            if (dnsinfo.ip.type == ESP_IPADDR_TYPE_V4 && dnsinfo.ip.u_addr.ip4.addr != 0) {
                esp_ip4_addr_t tmp; tmp.addr = dnsinfo.ip.u_addr.ip4.addr;
                char dnsbuf[16];
                esp_ip4addr_ntoa(&tmp, dnsbuf, sizeof(dnsbuf));
                ESP_LOGI(TAG, "Current DNS (MAIN): %s", dnsbuf);
                main_dns_ok = true;
            } else {
                ESP_LOGI(TAG, "Current DNS (MAIN) not set or not IPv4");
            }
        } else {
            ESP_LOGI(TAG, "esp_netif_get_dns_info(MAIN) returned error");
        }

        // 读取 BACKUP DNS（IPv4）用于诊断
        if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dnsinfo) == ESP_OK) {
            if (dnsinfo.ip.type == ESP_IPADDR_TYPE_V4 && dnsinfo.ip.u_addr.ip4.addr != 0) {
                esp_ip4_addr_t tmp2; tmp2.addr = dnsinfo.ip.u_addr.ip4.addr;
                char dnsbuf2[16];
                esp_ip4addr_ntoa(&tmp2, dnsbuf2, sizeof(dnsbuf2));
                ESP_LOGI(TAG, "Current DNS (BACKUP): %s", dnsbuf2);
            } else {
                ESP_LOGI(TAG, "Current DNS (BACKUP) not set or not IPv4");
            }
        } else {
            ESP_LOGI(TAG, "esp_netif_get_dns_info(BACKUP) returned error");
        }

        // 如果 DHCPv4 没下发主 DNS，就设置一个更可靠的公共 DNS（优先使用 223.5.5.5），并记录日志
        if (!main_dns_ok) {
            esp_netif_dns_info_t new_main;
            new_main.ip.type = ESP_IPADDR_TYPE_V4;
            // 直接使用公共 DNS（223.5.5.5）而不是 gateway，许多路由器会阻断对 gateway 的 DNS 转发
            new_main.ip.u_addr.ip4.addr = inet_addr("223.5.5.5"); // 注意 inet_addr 返回网络字节序

            esp_err_t rc = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &new_main);
            if (rc == ESP_OK) {
                char buf[16];
                esp_ip4addr_ntoa(&new_main.ip.u_addr.ip4, buf, sizeof(buf));
                ESP_LOGI(TAG, "Set DNS MAIN to public: %s", buf);
            } else {
                ESP_LOGW(TAG, "Failed to set DNS MAIN: %s", esp_err_to_name(rc));
            }

            esp_netif_dns_info_t backup;
            backup.ip.type = ESP_IPADDR_TYPE_V4;
            backup.ip.u_addr.ip4.addr = inet_addr("223.6.6.6");
            rc = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &backup);
            if (rc == ESP_OK) {
                ESP_LOGI(TAG, "Set DNS BACKUP to 223.6.6.6");
            } else {
                ESP_LOGW(TAG, "Failed to set DNS BACKUP: %s", esp_err_to_name(rc));
            }

            ESP_LOGI(TAG, "DNS fix applied (main was missing or not IPv4).");
        }
    }
    // ----------------- DNS 诊断 -----------------

    // 通知应用已经连上（原有逻辑）
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}

