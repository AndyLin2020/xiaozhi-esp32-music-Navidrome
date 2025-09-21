#include "http_client.h"
#include "network_interface.h"
#include <esp_log.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cctype>

static const char *TAG = "HttpClient";

// 限制：所有 body_chunks 合计不超过这个值（避免内存耗尽）
static constexpr size_t MAX_TOTAL_BUFFER_BYTES = 512 * 1024; // 128 KB
static constexpr size_t MAX_RX_BUFFER_BYTES = 128 * 1024; // 128 KB 上限，按需调整
static constexpr size_t MAX_ONTCP_WAIT_BYTES = 64 * 1024; // OnTcpData 等待中使用的阈值（保持原意）

HttpClient::HttpClient(NetworkInterface* network, int connect_id) : network_(network), connect_id_(connect_id) {
    event_group_handle_ = xEventGroupCreate();
}

HttpClient::~HttpClient() {
    if (connected_) {
        Close();
    }
    vEventGroupDelete(event_group_handle_);
}

void HttpClient::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void HttpClient::SetHeader(const std::string& key, const std::string& value) {
    // 转换key为小写用于存储和查找，但保留原始key用于输出
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    headers_[lower_key] = HeaderEntry(key, value);
}

void HttpClient::SetContent(std::string&& content) {
    content_ = std::move(content);
}

bool HttpClient::ParseUrl(const std::string& url) {
    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        ESP_LOGE(TAG, "Invalid URL format: %s", url.c_str());
        return false;
    }

    protocol_ = url.substr(0, protocol_end);
    std::transform(protocol_.begin(), protocol_.end(), protocol_.begin(), ::tolower);

    size_t host_start = protocol_end + 3;
    size_t path_start = url.find("/", host_start);
    size_t port_start = url.find(":", host_start);

    if (protocol_ == "https") {
        port_ = 443;
    } else {
        port_ = 80;
    }

    if (path_start == std::string::npos) {
        path_ = "/";
        if (port_start != std::string::npos) {
            host_ = url.substr(host_start, port_start - host_start);
            std::string port_str = url.substr(port_start + 1);
            char* endptr;
            long port = strtol(port_str.c_str(), &endptr, 10);
            if (endptr != port_str.c_str() && *endptr == '\0' && port > 0 && port <= 65535) {
                port_ = static_cast<int>(port);
            } else {
                ESP_LOGE(TAG, "Invalid port: %s", port_str.c_str());
                return false;
            }
        } else {
            host_ = url.substr(host_start);
        }
    } else {
        path_ = url.substr(path_start);
        if (port_start != std::string::npos && port_start < path_start) {
            host_ = url.substr(host_start, port_start - host_start);
            std::string port_str = url.substr(port_start + 1, path_start - port_start - 1);
            char* endptr;
            long port = strtol(port_str.c_str(), &endptr, 10);
            if (endptr != port_str.c_str() && *endptr == '\0' && port > 0 && port <= 65535) {
                port_ = static_cast<int>(port);
            } else {
                ESP_LOGE(TAG, "Invalid port: %s", port_str.c_str());
                return false;
            }
        } else {
            host_ = url.substr(host_start, path_start - host_start);
        }
    }

    ESP_LOGD(TAG, "Parsed URL: protocol=%s, host=%s, port=%d, path=%s",
             protocol_.c_str(), host_.c_str(), port_, path_.c_str());
    return true;
}

std::string HttpClient::BuildHttpRequest() {
    std::ostringstream request;

    request << method_ << " " << path_ << " HTTP/1.1\r\n";

    request << "Host: " << host_;
    if ((protocol_ == "http" && port_ != 80) || (protocol_ == "https" && port_ != 443)) {
        request << ":" << port_;
    }
    request << "\r\n";

    for (const auto& [lower_key, header_entry] : headers_) {
        request << header_entry.original_key << ": " << header_entry.value << "\r\n";
    }

    bool user_set_content_length = headers_.find("content-length") != headers_.end();
    bool user_set_transfer_encoding = headers_.find("transfer-encoding") != headers_.end();
    bool has_content = content_.has_value() && !content_->empty();
    if (has_content && !user_set_content_length) {
        request << "Content-Length: " << content_->size() << "\r\n";
    } else if ((method_ == "POST" || method_ == "PUT") && !user_set_content_length && !user_set_transfer_encoding) {
        if (request_chunked_) {
            request << "Transfer-Encoding: chunked\r\n";
        } else {
            request << "Content-Length: 0\r\n";
        }
    }

    if (headers_.find("connection") == headers_.end()) {
        request << "Connection: close\r\n";
    }

    request << "\r\n";
    ESP_LOGD(TAG, "HTTP request headers:\n%s", request.str().c_str());

    if (has_content) {
        request << *content_;
    }

    return request.str();
}

bool HttpClient::Open(const std::string& method, const std::string& url) {
    method_ = method;
    url_ = url;

    status_code_ = -1;
    response_headers_.clear();
    {
        std::lock_guard<std::mutex> read_lock(read_mutex_);
        body_chunks_.clear();
    }
    body_offset_ = 0;
    content_length_ = 0;
    total_body_received_ = 0;
    eof_ = false;
    headers_received_ = false;
    response_chunked_ = false;
    connection_error_ = false;  // 重置连接错误状态
    parse_state_ = ParseState::STATUS_LINE;
    chunk_size_ = 0;
    chunk_received_ = 0;
    rx_buffer_.clear();

    xEventGroupClearBits(event_group_handle_,
                         EC801E_HTTP_EVENT_HEADERS_RECEIVED |
                         EC801E_HTTP_EVENT_BODY_RECEIVED |
                         EC801E_HTTP_EVENT_ERROR |
                         EC801E_HTTP_EVENT_COMPLETE);

    if (!ParseUrl(url)) {
        return false;
    }

    if (protocol_ == "https") {
        tcp_ = network_->CreateSsl(connect_id_);
    } else {
        tcp_ = network_->CreateTcp(connect_id_);
    }

    tcp_->OnStream([this](const std::string& data) {
        OnTcpData(data);
    });

    tcp_->OnDisconnected([this]() {
        OnTcpDisconnected();
    });
    if (!tcp_->Connect(host_, port_)) {
        ESP_LOGE(TAG, "TCP connection failed");
        return false;
    }

    connected_ = true;
    request_chunked_ = (method_ == "POST" || method_ == "PUT") && !content_.has_value();

    std::string http_request = BuildHttpRequest();
    if (tcp_->Send(http_request) <= 0) {
        ESP_LOGE(TAG, "Send HTTP request failed");
        tcp_->Disconnect();
        connected_ = false;
        return false;
    }

    return true;
}

void HttpClient::Close() {
    if (!connected_) {
        return;
    }
    connected_ = false;
    write_cv_.notify_all();
    tcp_->Disconnect();

    eof_ = true;
    cv_.notify_all();
    ESP_LOGD(TAG, "HTTP connection closed");
}

void HttpClient::OnTcpData(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        std::unique_lock<std::mutex> read_lock(read_mutex_);
        write_cv_.wait(read_lock, [this, incoming = data.size()] {
            size_t chunks_total = 0;
            for (const auto& chunk : body_chunks_) chunks_total += chunk.data.size();

            size_t total_size = incoming + chunks_total + rx_buffer_.size();

            if (total_size > MAX_RX_BUFFER_BYTES) {
                return true; 
            }

            return total_size < MAX_ONTCP_WAIT_BYTES || !connected_;
        });
    }

    if (rx_buffer_.size() + data.size() > MAX_RX_BUFFER_BYTES) {
        size_t overflow = (rx_buffer_.size() + data.size()) - MAX_RX_BUFFER_BYTES;
        ESP_LOGW(TAG, "rx_buffer_ overflow: current=%u incoming=%u max=%u -> drop %u bytes",
                 (unsigned)rx_buffer_.size(), (unsigned)data.size(), (unsigned)MAX_RX_BUFFER_BYTES, (unsigned)overflow);
        if (overflow >= rx_buffer_.size()) {
            rx_buffer_.clear();
        } else {
            rx_buffer_.erase(0, overflow);
        }
    }

    rx_buffer_.append(data);

    ProcessReceivedData();

    cv_.notify_one();
}

void HttpClient::OnTcpDisconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;

    if (headers_received_ && !IsDataComplete()) {
        connection_error_ = true;
        //ESP_LOGI(TAG, "连接过早关闭，预期接收 %u 字节，但实际只接收到 %u 字节",
        //         content_length_, total_body_received_);
    } else {
        eof_ = true;
    }

    cv_.notify_all(); 
}

void HttpClient::ProcessReceivedData() {
    while (!rx_buffer_.empty() && parse_state_ != ParseState::COMPLETE) {
        try {
            switch (parse_state_) {
                case ParseState::STATUS_LINE: {
                    if (!HasCompleteLine(rx_buffer_)) return;
                    std::string line = GetNextLine(rx_buffer_);
                    if (!ParseStatusLine(line)) { SetError(); return; }
                    parse_state_ = ParseState::HEADERS;
                    break;
                }

                case ParseState::HEADERS: {
                    if (!HasCompleteLine(rx_buffer_)) return;
                    std::string line = GetNextLine(rx_buffer_);
                    if (line.empty()) {
                        auto te_it = response_headers_.find("transfer-encoding");
                        if (te_it != response_headers_.end() &&
                            te_it->second.value.find("chunked") != std::string::npos) {
                            response_chunked_ = true;
                            parse_state_ = ParseState::CHUNK_SIZE;
                        } else {
                            parse_state_ = ParseState::BODY;
                            content_length_ = 0;
                            auto cl_it = response_headers_.find("content-length");
                            if (cl_it != response_headers_.end()) {
                                char* endptr;
                                unsigned long length = strtoul(cl_it->second.value.c_str(), &endptr, 10);
                                if (endptr != cl_it->second.value.c_str() && *endptr == '\0') {
                                    content_length_ = static_cast<size_t>(length);
                                } else {
                                    ESP_LOGE(TAG, "Invalid Content-Length: %s", cl_it->second.value.c_str());
                                    content_length_ = 0;
                                }
                            }

                            auto cr_it = response_headers_.find("content-range");
                            if (cr_it != response_headers_.end() && content_length_ == 0) {
                                std::string cr = cr_it->second.value;
                                size_t slash = cr.find('/');
                                if (slash != std::string::npos) {
                                    std::string total_str = cr.substr(slash + 1);
                                    if (!total_str.empty() && total_str != "*") {
                                        char* endptr2;
                                        unsigned long total = strtoul(total_str.c_str(), &endptr2, 10);
                                        if (endptr2 != total_str.c_str() && *endptr2 == '\0') {
                                            content_length_ = static_cast<size_t>(total);
                                            ESP_LOGD(TAG, "Parsed Content-Range total: %u", content_length_);
                                        }
                                    }
                                }
                            }
                        }

                        headers_received_ = true;
                        xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_HEADERS_RECEIVED);
                    } else {
                        if (!ParseHeaderLine(line)) { SetError(); return; }
                    }
                    break;
                }

                case ParseState::BODY: {
                    if (rx_buffer_.empty()) return;

                    size_t need = (content_length_ > 0) ? (content_length_ - total_body_received_) : rx_buffer_.size();
                    size_t to_consume = std::min(need, rx_buffer_.size());

                    if (to_consume > 0) {
                        const size_t MAX_CHUNK_STEP = 16 * 1024;
                        size_t step = std::min(to_consume, MAX_CHUNK_STEP);
                        std::string chunk;
                        chunk.assign(rx_buffer_.data(), step);
                        AddBodyData(std::move(chunk));
                        total_body_received_ += step;
                        rx_buffer_.erase(0, step);
                    }

                    if ((content_length_ > 0 && total_body_received_ >= content_length_) ||
                        (content_length_ == 0 && eof_)) {
                        parse_state_ = ParseState::COMPLETE;
                        xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_COMPLETE);
                    }
                    break;
                }

                case ParseState::CHUNK_SIZE: {
                    if (!HasCompleteLine(rx_buffer_)) return;
                    std::string line = GetNextLine(rx_buffer_);
                    chunk_size_ = ParseChunkSize(line);
                    chunk_received_ = 0;
                    if (chunk_size_ == 0) {
                        parse_state_ = ParseState::CHUNK_TRAILER;
                    } else {
                        parse_state_ = ParseState::CHUNK_DATA;
                    }
                    break;
                }

                case ParseState::CHUNK_DATA: {
                    size_t need = chunk_size_ - chunk_received_;
                    size_t avail = std::min(rx_buffer_.size(), need);
                    if (avail > 0) {
                        const size_t MAX_CHUNK_STEP = 16 * 1024;
                        size_t step = std::min(avail, MAX_CHUNK_STEP);
                        std::string chunk;
                        chunk.assign(rx_buffer_.data(), step);
                        AddBodyData(std::move(chunk));
                        total_body_received_ += step;
                        rx_buffer_.erase(0, step);
                        chunk_received_ += step;
                    } else {
                        return; 
                    }

                    if (chunk_received_ == chunk_size_) {
                        if (rx_buffer_.size() >= 2 && rx_buffer_.substr(0,2) == "\r\n") {
                            rx_buffer_.erase(0,2);
                        }
                        parse_state_ = ParseState::CHUNK_SIZE;
                    }
                    break;
                }

                case ParseState::CHUNK_TRAILER: {
                    if (!HasCompleteLine(rx_buffer_)) return;
                    std::string line = GetNextLine(rx_buffer_);
                    if (line.empty()) {
                        parse_state_ = ParseState::COMPLETE;
                        eof_ = true;
                        xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_COMPLETE);
                    } else {
                    }
                    break;
                }

                case ParseState::COMPLETE:
                    return;
            } // switch
        } catch (const std::bad_alloc& e) {
            ESP_LOGE(TAG, "ProcessReceivedData: std::bad_alloc caught (%s). Marking connection_error_.", e.what());
            connection_error_ = true;
            xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_ERROR);
            rx_buffer_.clear();
            return;
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "ProcessReceivedData: unexpected exception: %s", e.what());
            connection_error_ = true;
            xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_ERROR);
            rx_buffer_.clear();
            return;
        }
    } 

    if (parse_state_ == ParseState::BODY && !response_chunked_ &&
            content_length_ == 0 && eof_) {
        parse_state_ = ParseState::COMPLETE;
        xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_COMPLETE);
        ESP_LOGD(TAG, "HTTP stream ended (no content-length)");
    }
}


bool HttpClient::ParseStatusLine(const std::string& line) {
    std::istringstream iss(line);
    std::string version, status_str, reason;

    if (!(iss >> version >> status_str)) {
        ESP_LOGE(TAG, "Invalid status line: %s", line.c_str());
        return false;
    }

    std::getline(iss, reason); 

    char* endptr;
    long status = strtol(status_str.c_str(), &endptr, 10);

    if (endptr == status_str.c_str() || *endptr != '\0' || status < 100 || status > 999) {
        ESP_LOGE(TAG, "Parse status code failed: %s", status_str.c_str());
        return false;
    }

    status_code_ = static_cast<int>(status);
    ESP_LOGD(TAG, "HTTP status code: %d", status_code_);
    return true;
}

bool HttpClient::ParseHeaderLine(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        ESP_LOGE(TAG, "Invalid header line: %s", line.c_str());
        return false;
    }

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t\r\n") + 1);

    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);

    response_headers_[lower_key] = HeaderEntry(key, value);

    return true;
}

void HttpClient::ParseChunkedBody(const std::string& /*data*/) {
    // 这个函数仅用于兼容接口，解析逻辑已经写在 ProcessReceivedData 的 CHUNK_* 状态中。
    // 保持空实现以避免重复实现。
}

void HttpClient::ParseRegularBody(const std::string& data) {
    if (data.empty()) return;

    size_t to_take = data.size();
    if (content_length_ > 0) {
        if (total_body_received_ >= content_length_) {
            return;
        }
        size_t remain = content_length_ - total_body_received_;
        if (to_take > remain) to_take = remain;
    }

    std::string chunk = data.substr(0, to_take);
    AddBodyData(std::move(chunk));
    total_body_received_ += to_take;

    if (rx_buffer_.size() >= to_take) {
        rx_buffer_.erase(0, to_take);
    } else {
        rx_buffer_.clear();
    }
}

size_t HttpClient::ParseChunkSize(const std::string& line) {
    std::string s = line;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t pos = s.find(';');
    if (pos != std::string::npos) s = s.substr(0, pos);

    if (s.length() == 0) return (size_t)-1;
    if (s.length() > 32) {
        ESP_LOGW(TAG, "ParseChunkSize: line too long (%u), truncating", (unsigned)s.length());
        s = s.substr(0, 32);
    }

    size_t chunk = 0;
    for (char c : s) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else {
            break;
        }
        if (chunk > (SIZE_MAX >> 4)) {
            ESP_LOGE(TAG, "ParseChunkSize: overflow while parsing chunk size");
            return (size_t)-1;
        }
        chunk = (chunk << 4) | (size_t)v;

        // 保护：限制 chunk 的最大值（例如 4MB）
        const size_t MAX_CHUNK_LIMIT = 4 * 1024 * 1024;
        if (chunk > MAX_CHUNK_LIMIT) {
            ESP_LOGW(TAG, "ParseChunkSize: chunk size %u exceeds limit %u", (unsigned)chunk, (unsigned)MAX_CHUNK_LIMIT);
            return (size_t)-1;
        }
    }

    return chunk;
}

std::string HttpClient::GetNextLine(std::string& buffer) {
    size_t pos = buffer.find('\n');
    if (pos == std::string::npos) {
        return "";
    }
    size_t line_len = pos;
    if (line_len > 4096) line_len = 4096; // 限长，防止恶意行
    std::string line = buffer.substr(0, line_len);
    buffer.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

bool HttpClient::HasCompleteLine(const std::string& buffer) {
    return buffer.find('\n') != std::string::npos;
}

void HttpClient::SetError() {
    ESP_LOGE(TAG, "HTTP parse error");
    xEventGroupSetBits(event_group_handle_, EC801E_HTTP_EVENT_ERROR);
}

int HttpClient::Read(char* buffer, size_t buffer_size) {
    std::unique_lock<std::mutex> read_lock(read_mutex_);

    if (connection_error_) {
        return -1;
    }

    if (eof_ && body_chunks_.empty()) {
        return 0;
    }

    while (!body_chunks_.empty()) {
        auto& front_chunk = body_chunks_.front();
        size_t bytes_read = front_chunk.read(buffer, buffer_size);

        if (bytes_read > 0) {
            if (front_chunk.empty()) {
                body_chunks_.pop_front();
            }
            write_cv_.notify_one();
            return static_cast<int>(bytes_read);
        }

        body_chunks_.pop_front();
    }

    if (!connected_) {
        if (connection_error_) {
            return -1;
        }
        return 0;
    }

    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool received = cv_.wait_for(read_lock, timeout, [this] {
        return !body_chunks_.empty() || eof_ || !connected_ || connection_error_;
    });

    if (!received) {
        ESP_LOGE(TAG, "等待HTTP内容接收超时");
        return -1;
    }

    if (connection_error_) {
        return -1;
    }

    while (!body_chunks_.empty()) {
        auto& front_chunk = body_chunks_.front();
        size_t bytes_read = front_chunk.read(buffer, buffer_size);

        if (bytes_read > 0) {
            if (front_chunk.empty()) {
                body_chunks_.pop_front();
            }
            write_cv_.notify_one();
            return static_cast<int>(bytes_read);
        }

        body_chunks_.pop_front();
    }

    return 0;
}

int HttpClient::Write(const char* buffer, size_t buffer_size) {
    if (!connected_ || !request_chunked_) {
        ESP_LOGE(TAG, "Cannot write: connection closed or not chunked mode");
        return -1;
    }

    if (buffer_size == 0) {
        std::string end_chunk = "0\r\n\r\n";
        return tcp_->Send(end_chunk);
    }

    std::ostringstream chunk;
    chunk << std::hex << buffer_size << "\r\n";
    chunk.write(buffer, buffer_size);
    chunk << "\r\n";

    std::string chunk_data = chunk.str();
    return tcp_->Send(chunk_data);
}

int HttpClient::GetStatusCode() {
    if (!headers_received_) {
        auto bits = xEventGroupWaitBits(event_group_handle_,
                                        EC801E_HTTP_EVENT_HEADERS_RECEIVED | EC801E_HTTP_EVENT_ERROR,
                                        pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms_));

        if (bits & EC801E_HTTP_EVENT_ERROR) {
            return -1;
        }
        if (!(bits & EC801E_HTTP_EVENT_HEADERS_RECEIVED)) {
            ESP_LOGE(TAG, "Wait for HTTP headers receive timeout");
            return -1;
        }
    }

    return status_code_;
}

std::string HttpClient::GetResponseHeader(const std::string& key) const {
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    
    auto it = response_headers_.find(lower_key);
    if (it != response_headers_.end()) {
        return it->second.value;
    }
    return "";
}

size_t HttpClient::GetBodyLength() {
    if (!headers_received_) {
        GetStatusCode(); 
    }

    if (response_chunked_) {
        return 0; 
    }

    return content_length_;
}

void HttpClient::AddBodyData(const std::string& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> read_lock(read_mutex_);

	size_t current_total = 0;
    for (const auto& c : body_chunks_) current_total += c.data.size();
    if (current_total + data.size() > MAX_TOTAL_BUFFER_BYTES) {
        ESP_LOGW(TAG, "Body buffer overflow: current=%u, adding=%u, max=%u. Dropping oldest chunks.",
                 (unsigned)current_total, (unsigned)data.size(), (unsigned)MAX_TOTAL_BUFFER_BYTES);
        while (!body_chunks_.empty() && current_total + data.size() > MAX_TOTAL_BUFFER_BYTES) {
            current_total -= body_chunks_.front().data.size();
            body_chunks_.pop_front();
        }
    }

    body_chunks_.emplace_back(data);  // 使用构造函数，避免额外的拷贝
    cv_.notify_one();  // 通知有新数据
    write_cv_.notify_one();  // 通知写入操作
}

void HttpClient::AddBodyData(std::string&& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> read_lock(read_mutex_);

    size_t current_total = 0;
    for (const auto& c : body_chunks_) current_total += c.data.size();
    if (current_total + data.size() > MAX_TOTAL_BUFFER_BYTES) {
        ESP_LOGW(TAG, "Body buffer overflow (move): current=%u, adding=%u, max=%u. Dropping oldest chunks.",
                 (unsigned)current_total, (unsigned)data.size(), (unsigned)MAX_TOTAL_BUFFER_BYTES);
        while (!body_chunks_.empty() && current_total + data.size() > MAX_TOTAL_BUFFER_BYTES) {
            current_total -= body_chunks_.front().data.size();
            body_chunks_.pop_front();
        }
    }

    body_chunks_.emplace_back(std::move(data)); 
    cv_.notify_one();  
    write_cv_.notify_one(); 
}

std::string HttpClient::ReadAll() {
    std::unique_lock<std::mutex> lock(mutex_);

    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool completed = cv_.wait_for(lock, timeout, [this] {
        return eof_ || connection_error_;
    });

    if (!completed) {
        ESP_LOGE(TAG, "Wait for HTTP content receive complete timeout");
        return ""; 
    }

    if (connection_error_) {
        ESP_LOGE(TAG, "Cannot read all data: connection closed prematurely");
        return "";
    }

    std::string result;
    std::lock_guard<std::mutex> read_lock(read_mutex_);
    for (const auto& chunk : body_chunks_) {
        result.append(chunk.data);
    }

    return result;
}

bool HttpClient::IsDataComplete() const {
    if (response_chunked_) {
        return parse_state_ == ParseState::COMPLETE;
    }

    if (content_length_ > 0) {
        return total_body_received_ >= content_length_;
    }

    // 如果没有content-length且不是chunked，当连接关闭时认为完整
    // 这种情况通常用于HTTP/1.0或者content-length为0的响应
    return true;
}
