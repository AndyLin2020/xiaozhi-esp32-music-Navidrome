#include "dns_server.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "DnsServer";

DnsServer::DnsServer() {
    // header 中已经对 port_ 和 fd_ 有默认值 (port_ = 53, fd_ = -1)
    // 这里不在初始化列表中重排序，直接保持空构造体
}

DnsServer::~DnsServer() {
    Stop();
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    ESP_LOGI(TAG, "Starting DNS server");
    gateway_ = gateway;

    // 如果 socket 已打开，说明已经在运行
    if (fd_ >= 0) {
        ESP_LOGW(TAG, "DNS server already started (fd=%d)", fd_);
        return;
    }

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket, errno=%d", errno);
        fd_ = -1;
        return;
    }

    // allow reuse
    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if (bind(fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "failed to bind port %d, errno=%d", port_, errno);
        close(fd_);
        fd_ = -1;
        return;
    }

    BaseType_t r = xTaskCreate([](void* arg) {
        DnsServer* dns = static_cast<DnsServer*>(arg);
        dns->Run();
    }, "DnsServerTask", 4096, this, 5, nullptr);

    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS task");
        close(fd_);
        fd_ = -1;
        return;
    }

    ESP_LOGI(TAG, "DNS server UDP/%d started (fd=%d)", port_, fd_);
}

void DnsServer::Stop() {
    ESP_LOGI(TAG, "Stopping DNS server");
    if (fd_ >= 0) {
        // close socket -> recvfrom will fail and task will exit
        close(fd_);
        fd_ = -1;
    } else {
        ESP_LOGI(TAG, "DNS server socket already closed");
    }
    // We don't explicitly delete the task here; Run() will vTaskDelete(NULL) on exit
}

void DnsServer::Run() {
    const int BUF_SIZE = 1024;
    uint8_t buffer[BUF_SIZE];

    while (true) {
        if (fd_ < 0) {
            ESP_LOGI(TAG, "Socket closed, exiting DnsServer task");
            break;
        }

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        int len = recvfrom(fd_, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            // If socket closed, errno may be EBADF or EINTR etc.
            ESP_LOGW(TAG, "recvfrom returned error: errno=%d", errno);
            // If socket closed (fd_ < 0) then exit
            if (fd_ < 0 || errno == EBADF) break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (len < 12) {
            ESP_LOGW(TAG, "Received too short DNS packet (%d bytes), ignore", len);
            continue;
        }

        // Log the client IP
        char client_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        // Prepare response:
        // Set QR=1 (response) by setting bit7 of byte2, set RA in byte3
        buffer[2] |= 0x80; // QR = 1
        buffer[3] |= 0x80; // RA = 1

        // Set ANCOUNT = 1 (two bytes)
        buffer[6] = 0x00;
        buffer[7] = 0x01;

        int pos = len;
        // Ensure enough room for answer: name ptr (2) + type/class/ttl/len (10) + rdata(4) = 16
        if (pos + 16 > BUF_SIZE) {
            ESP_LOGE(TAG, "Not enough buffer to append DNS answer");
            continue;
        }

        // NAME pointer to offset 12 (0xC)
        buffer[pos++] = 0xC0;
        buffer[pos++] = 0x0C;

        // TYPE A (0x0001)
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x01;
        // CLASS IN (0x0001)
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x01;
        // TTL 28s
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x1C;
        // RDLENGTH = 4
        buffer[pos++] = 0x00;
        buffer[pos++] = 0x04;

        // RDATA: use gateway_.addr (ip4 addr in network byte order as esp stores it)
        struct in_addr gw;
        gw.s_addr = gateway_.addr;
        memcpy(&buffer[pos], &gw.s_addr, 4);
        pos += 4;

        //ESP_LOGI(TAG, "DNS query from %s -> answer %s", client_ip, inet_ntoa(gw));

        int sent = sendto(fd_, buffer, pos, 0, (struct sockaddr *)&client_addr, client_addr_len);
        if (sent < 0) {
            ESP_LOGE(TAG, "sendto failed, errno=%d", errno);
        }
    }

    ESP_LOGI(TAG, "DnsServer task exiting");
    // delete self
    vTaskDelete(NULL);
}