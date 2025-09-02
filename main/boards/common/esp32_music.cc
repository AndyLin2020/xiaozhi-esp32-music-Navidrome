#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Music"

// forward declare Http class type (implementation provided by board/network)
class Http;

// ========== Helper functions (authentication, url encode, etc.) ==========
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

static std::string get_device_chip_id() {
    std::string mac = SystemInfo::GetMacAddress();
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

static std::string generate_dynamic_key(int64_t timestamp) {
    const std::string secret_key = "your-esp32-secret-key-2024";
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    return key;
}

static void add_auth_headers(Http* http) {
    int64_t timestamp = esp_timer_get_time() / 1000000;
    std::string dynamic_key = generate_dynamic_key(timestamp);
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %lld", mac.c_str(), chip_id.c_str(), timestamp);
    }
}

static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        pos = amp_pos + 1;
    }
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    return result_url;
}

// Constructor / Destructor
Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Music player initialized");
    InitializeMp3Decoder();
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }
            is_downloading_ = false;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }
            is_playing_ = false;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    
    // abort any outstanding lyric HTTP
    AbortCurrentLyricHttp();
    
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

// Download (request metadata)
bool Esp32Music::Download(const std::string& song_name) {
    ESP_LOGI(TAG, "148514");
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());
    
    last_downloaded_data_.clear();
    current_song_name_ = song_name;
    
    std::string api_url = "http://61.145.168.125:8088/stream_pcm";
    std::string full_url = api_url + "?song=" + url_encode(song_name);
    
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to music API");
        return false;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return false;
    }
    
    last_downloaded_data_ = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, last_downloaded_data_.length());
    ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    if (!last_downloaded_data_.empty()) {
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            cJSON* artist = cJSON_GetObjectItem(response_json, "artist");
            cJSON* title = cJSON_GetObjectItem(response_json, "title");
            cJSON* audio_url = cJSON_GetObjectItem(response_json, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(response_json, "lyric_url");
            
            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Artist: %s", artist->valuestring);
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Title: %s", title->valuestring);
            }
            
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
                ESP_LOGI(TAG, "Audio URL path: %s", audio_url->valuestring);
                
                current_music_url_ = audio_url->valuestring;
                                
                ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
                song_name_displayed_ = false;
                StartStreaming(current_music_url_);
                
                bool has_external_lyrics = false;
                if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    has_external_lyrics = true;
                    current_lyric_url_ = lyric_url->valuestring;
                    ESP_LOGI(TAG, "Found external lyrics URL: %s", current_lyric_url_.c_str());
                }

                if (is_lyric_running_) {
                    is_lyric_running_ = false;
                    if (lyric_thread_.joinable()) {
                        lyric_thread_.join();
                    }
                }
                
                is_lyric_running_ = true;
                current_lyric_index_ = -1;
                lyrics_.clear();
                
                lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
                
                if (!has_external_lyrics) {
                    ESP_LOGI(TAG, "No external lyric URL found, will attempt to parse embedded lyrics.");
                }

                cJSON_Delete(response_json);
                return true;
            } else {
                ESP_LOGE(TAG, "Audio URL not found or empty in response");
                cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON response");
        }
    } else {
        ESP_LOGE(TAG, "Empty response from music API");
    }
    
    return false;
}

bool Esp32Music::Play() {
    if (is_playing_.load()) {
        ESP_LOGW(TAG, "Music is already playing");
        return true;
    }
    
    if (last_downloaded_data_.empty()) {
        ESP_LOGE(TAG, "No music data to play");
        return false;
    }
    
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    
    return StartStreaming(current_music_url_);
}

bool Esp32Music::Stop() {
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "Music is not playing or downloading");
        return true;
    }
    
    ESP_LOGI(TAG, "Stopping music playback and download");
    
    is_downloading_ = false;
    is_playing_ = false;
    
    ResetSampleRate();
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    
    ClearAudioBuffer();
    
    ESP_LOGI(TAG, "Music stopped successfully");
    return true;
}

std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGD(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    is_downloading_ = false;
    is_playing_ = false;
    
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        play_thread_.join();
    }
    
    ClearAudioBuffer();
    
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;
    cfg.prio = 5;
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);
    
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    return true;
}

bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    ResetSampleRate();
    
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }
    
    is_downloading_ = false;
    is_playing_ = false;
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");
        ESP_LOGI(TAG, "Cleared song name display");
    }
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// Extract embedded USLT lyrics (from ID3v2)
std::string ExtractLyricsFromId3(const uint8_t* data, size_t size) {
    const char* uslt_tag = "USLT";
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (memcmp(data + i, uslt_tag, 4) == 0) {
            if (i + 10 < size) {
                uint32_t frame_size = (data[i + 4] << 24) | (data[i + 5] << 16) | (data[i + 6] << 8) | data[i + 7];
                if (frame_size > 0 && i + 10 + frame_size <= size) {
                    const uint8_t* lyric_start = data + i + 10;
                    size_t lyric_max_size = frame_size;
                    size_t offset = 1 + 3;
                    while (offset < lyric_max_size && lyric_start[offset] != 0) {
                        offset++;
                    }
                    if (offset < lyric_max_size) {
                        offset++;
                        std::string lyrics(reinterpret_cast<const char*>(lyric_start + offset), lyric_max_size - offset);
                        ESP_LOGI("Esp32Music", "Found and extracted embedded USLT lyrics from ID3 tag!");
                        return lyrics;
                    }
                }
            }
        }
    }
    return "";
}

// Download audio stream
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    
    if (!http->Open("GET", music_url)) {
        ESP_LOGE(TAG, "Failed to connect to music stream URL");
        is_downloading_ = false;
        return;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        is_downloading_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
    
    const size_t chunk_size = 2048;
    char* buffer = (char*)heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        http->Close();
        is_downloading_ = false;
        return;
    }

    size_t total_downloaded = 0;
    bool lyrics_found_and_parsed = false;
    
    while (is_downloading_.load()) {
        int bytes_read = http->Read(buffer, chunk_size);
        
        if (bytes_read <= 0) {
            if (bytes_read < 0) ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            else ESP_LOGI(TAG, "Audio stream download completed, total: %zu bytes", total_downloaded);
            break;
        }

        if (!lyrics_found_and_parsed && total_downloaded < (128 * 1024)) {
            std::string embedded_lyrics = ExtractLyricsFromId3(reinterpret_cast<const uint8_t*>(buffer), bytes_read);
            if (!embedded_lyrics.empty()) {
                ParseLyrics(embedded_lyrics);
                lyrics_found_and_parsed = true;
            }
        }
        
        uint8_t* chunk_data_copy = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data_copy) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk copy");
            break;
        }
        memcpy(chunk_data_copy, buffer, bytes_read);
        
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (!buffer_cv_.wait_for(lock, std::chrono::seconds(5), [this] { 
                return buffer_size_ < MAX_BUFFER_SIZE || !is_playing_.load(); 
            })) {
                ESP_LOGW(TAG, "DownloadAudioStream: wait for buffer space timeout");
                heap_caps_free(chunk_data_copy);
                break;
            }

            if (!is_playing_.load() || !is_downloading_.load()) {
                heap_caps_free(chunk_data_copy);
                break;
            }
            
            audio_buffer_.push(AudioChunk(chunk_data_copy, bytes_read));
            buffer_size_ += bytes_read;
            
            buffer_cv_.notify_one();
        }

        total_downloaded += bytes_read;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    heap_caps_free(buffer);
    http->Close();
    
    is_downloading_ = false;
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// Play audio stream
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        ESP_LOGE(TAG, "Audio codec not available or not enabled");
        is_playing_ = false;
        return;
    }
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }
    
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        if (!buffer_cv_.wait_for(lock, std::chrono::seconds(10), [this] {
            return (buffer_size_ >= MIN_BUFFER_SIZE) || (!is_downloading_.load() && !audio_buffer_.empty()) || !is_playing_.load();
        })) {
             ESP_LOGE(TAG, "Timeout waiting for initial audio buffer. Aborting playback.");
             is_playing_ = false;
             return;
        }
    }
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %zu", buffer_size_);
    
    size_t total_played = 0;
    const size_t MP3_INPUT_BUFFER_SIZE = 8192;
    uint8_t* mp3_input_buffer = (uint8_t*)heap_caps_malloc(MP3_INPUT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }
    int bytes_left = 0;
    uint8_t* read_ptr = mp3_input_buffer;
    bool id3_processed = false;

    int loop_counter = 0;
    
    while (is_playing_.load()) {
        if (++loop_counter >= 20) {
            vTaskDelay(pdMS_TO_TICKS(1));
            loop_counter = 0;
        }

        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        if (current_state == kDeviceStateListening) {
            ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            app.ToggleChatState();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) {
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }
        }
        
        if (bytes_left < (MP3_INPUT_BUFFER_SIZE / 2)) {
            AudioChunk chunk;
            bool got_chunk = false;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (is_downloading_.load()) {
                        buffer_cv_.wait_for(lock, std::chrono::milliseconds(200));
                    } else {
                        ESP_LOGI(TAG, "Playback buffer empty and download finished.");
                        break;
                    }
                }
                
                if (!audio_buffer_.empty()) {
                    chunk = audio_buffer_.front();
                    audio_buffer_.pop();
                    buffer_size_ -= chunk.size;
                    got_chunk = true;
                    buffer_cv_.notify_one();
                }
            }

            if (got_chunk && chunk.data && chunk.size > 0) {
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                read_ptr = mp3_input_buffer;

                size_t space_available = MP3_INPUT_BUFFER_SIZE - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                if (copy_size < chunk.size) {
                    ESP_LOGW(TAG, "MP3 input buffer is full, discarding %zu bytes", chunk.size - copy_size);
                }
                
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                
                heap_caps_free(chunk.data);
            }
        }

        if (bytes_left == 0) {
            if (!is_downloading_.load()) {
                 ESP_LOGI(TAG, "No more data in buffer and download is finished.");
                 break; 
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!id3_processed && bytes_left >= 10) {
            size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
            if (id3_skip > 0) {
                read_ptr += id3_skip;
                bytes_left -= id3_skip;
                ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
            }
            id3_processed = true;
        }
        
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGD(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            if (mp3_frame_info_.samprate <= 0 || mp3_frame_info_.nChans <= 0) {
                ESP_LOGD(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            current_play_time_ms_ += frame_duration_ms;
            
            int buffer_latency_ms = 600;
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                if (mp3_frame_info_.nChans == 2) {
                    int mono_samples = mp3_frame_info_.outputSamps / 2;
                    mono_buffer.resize(mono_samples);
                    for (int i = 0; i < mono_samples; ++i) {
                        mono_buffer[i] = (int16_t)(((int)pcm_buffer[i * 2] + (int)pcm_buffer[i * 2 + 1]) / 2);
                    }
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;
                }
                
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.channels = 1;
                packet.frame_duration = 60;
                packet.timestamp = 0;
                
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);
                
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
            }
            
        } else {
            ESP_LOGD(TAG, "MP3 decode failed with error: %d", decode_result);
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            }
        }
    }
    
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
    }
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");
        ESP_LOGI(TAG, "Cleared song name display on playback end");
    }

    ResetSampleRate();
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %zu bytes", total_played);
    
    is_playing_ = false;
}

// Clear audio buffer
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// Initialize / cleanup MP3 decoder
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// Reset sample rate
void Esp32Music::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// Skip ID3 tag
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    size_t total_skip = 10 + tag_size;
    
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// Abort current lyric HTTP (thread-safe)
void Esp32Music::AbortCurrentLyricHttp() {
    std::lock_guard<std::mutex> lock(lyric_http_mutex_);
    if (current_lyric_http_) {
        try {
            current_lyric_http_->Close();
        } catch (...) {}
        current_lyric_http_.reset();
    }
}

// Download lyrics (enhanced)
bool Esp32Music::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());
    
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }
    
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    
    while (retry_count < max_retries && !success) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        auto network = Board::GetInstance().GetNetwork();
        {
            std::lock_guard<std::mutex> lock(lyric_http_mutex_);
            current_lyric_http_ = network->CreateHttp(0);
        }
        if (!current_lyric_http_) {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }

        Http* http = nullptr;
        {
            std::lock_guard<std::mutex> lock(lyric_http_mutex_);
            http = current_lyric_http_.get();
        }
        if (!http) {
            ESP_LOGE(TAG, "Lyric http pointer invalid");
            AbortCurrentLyricHttp();
            retry_count++;
            continue;
        }

        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        add_auth_headers(http);

        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            AbortCurrentLyricHttp();
            retry_count++;
            continue;
        }

        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);

        if (status_code == 204) {
            ESP_LOGI(TAG, "Lyric URL returned 204 No Content - treating as no lyrics");
            http->Close();
            AbortCurrentLyricHttp();
            return false;
        }

        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            AbortCurrentLyricHttp();
            retry_count++;
            continue;
        }

        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;

        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
            } else if (bytes_read == 0) {
                success = true;
                break;
            } else {
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }

            if (!is_lyric_running_.load()) {
                ESP_LOGI(TAG, "Lyric download aborted by stop flag");
                read_error = true;
                break;
            }
        }

        http->Close();
        AbortCurrentLyricHttp();

        if (read_error) {
            retry_count++;
            continue;
        }

        if (success) break;
    }

    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }

    if (lyric_content.empty()) {
        ESP_LOGI(TAG, "Downloaded lyric content is empty");
        return false;
    }

    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", (int)lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 替换版 ParseLyrics：同时支持 LRC 和 JSON(line数组) 两种格式
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content (auto-detect LRC or JSON)");
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    lyrics_.clear();

    if (lyric_content.empty()) {
        ESP_LOGW(TAG, "ParseLyrics: empty content");
        return false;
    }

    // Helper to push a lyric line safely
    auto push_line = [&](int timestamp_ms, const std::string &text) {
        std::string trimmed = text;
        // trim both ends
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n')) trimmed.pop_back();
        size_t i = 0;
        while (i < trimmed.size() && (trimmed[i] == ' ' || trimmed[i] == '\t')) ++i;
        if (i > 0) trimmed.erase(0, i);
        lyrics_.push_back(std::make_pair(timestamp_ms, trimmed));
    };

    // 1) Try JSON parse if content likely JSON (starts with [ or {)
    const char *p = lyric_content.c_str();
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p == '[' || *p == '{') {
        cJSON *root = cJSON_Parse(lyric_content.c_str());
        if (root) {
            // root could be array or object containing array
            cJSON *entries = nullptr;
            if (cJSON_IsArray(root)) {
                entries = root;
            } else if (cJSON_IsObject(root)) {
                // try to find top-level array: "lyrics" or "line" or similar
                entries = cJSON_GetObjectItem(root, "lyrics");
                if (!entries) entries = cJSON_GetObjectItem(root, "lines");
                if (!entries) {
                    // maybe object itself contains an array named "line"
                    cJSON *maybeLine = cJSON_GetObjectItem(root, "line");
                    if (maybeLine && cJSON_IsArray(maybeLine)) entries = maybeLine;
                }
                // if still null, treat root as single entry wrapped in object, so try "line" inside root
                if (!entries) entries = root;
            }

            if (entries && cJSON_IsArray(entries)) {
                int count = cJSON_GetArraySize(entries);
                for (int i = 0; i < count; ++i) {
                    cJSON *entry = cJSON_GetArrayItem(entries, i);
                    if (!entry) continue;
                    // entry may have "line" array
                    cJSON *lines = cJSON_GetObjectItem(entry, "line");
                    if (lines && cJSON_IsArray(lines)) {
                        int ln = cJSON_GetArraySize(lines);
                        for (int j = 0; j < ln; ++j) {
                            cJSON *lineObj = cJSON_GetArrayItem(lines, j);
                            if (!lineObj) continue;
                            cJSON *start = cJSON_GetObjectItem(lineObj, "start");
                            cJSON *value = cJSON_GetObjectItem(lineObj, "value");
                            if (start && value && (cJSON_IsNumber(start) || cJSON_IsString(start)) && cJSON_IsString(value)) {
                                int start_ms = 0;
                                if (cJSON_IsNumber(start)) start_ms = (int)start->valuedouble;
                                else start_ms = atoi(start->valuestring);
                                push_line(start_ms, value->valuestring);
                            }
                        }
                    } else {
                        // entry might itself be a line object {start, value}
                        cJSON *start = cJSON_GetObjectItem(entry, "start");
                        cJSON *value = cJSON_GetObjectItem(entry, "value");
                        if (start && value && cJSON_IsString(value)) {
                            int start_ms = 0;
                            if (cJSON_IsNumber(start)) start_ms = (int)start->valuedouble;
                            else start_ms = atoi(start->valuestring);
                            push_line(start_ms, value->valuestring);
                        }
                    }
                }
            } else if (cJSON_IsObject(root)) {
                // try root->line array
                cJSON *lineArr = cJSON_GetObjectItem(root, "line");
                if (lineArr && cJSON_IsArray(lineArr)) {
                    int ln = cJSON_GetArraySize(lineArr);
                    for (int j = 0; j < ln; ++j) {
                        cJSON *lineObj = cJSON_GetArrayItem(lineArr, j);
                        if (!lineObj) continue;
                        cJSON *start = cJSON_GetObjectItem(lineObj, "start");
                        cJSON *value = cJSON_GetObjectItem(lineObj, "value");
                        if (start && value && (cJSON_IsNumber(start) || cJSON_IsString(start)) && cJSON_IsString(value)) {
                            int start_ms = cJSON_IsNumber(start) ? (int)start->valuedouble : atoi(start->valuestring);
                            push_line(start_ms, value->valuestring);
                        }
                    }
                }
            }
            cJSON_Delete(root);
            std::sort(lyrics_.begin(), lyrics_.end());
            ESP_LOGI(TAG, "Parsed %d lyric lines (from JSON)", (int)lyrics_.size());
            return !lyrics_.empty();
        } else {
            ESP_LOGW(TAG, "ParseLyrics: JSON parse failed, will try LRC");
        }
    }

    // 2) Fallback to LRC parsing: [mm:ss.xx]text
    std::istringstream stream(lyric_content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t pos = 0;
        while (pos < line.size()) {
            if (line[pos] != '[') break;
            size_t close = line.find(']', pos);
            if (close == std::string::npos) break;
            std::string tag = line.substr(pos + 1, close - pos - 1);
            std::string rest = line.substr(close + 1);
            // parse mm:ss.xx or mm:ss
            size_t colon = tag.find(':');
            if (colon != std::string::npos) {
                std::string mm = tag.substr(0, colon);
                std::string ss = tag.substr(colon + 1);
                try {
                    int minutes = std::stoi(mm);
                    float seconds = std::stof(ss);
                    int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000.0f);
                    push_line(timestamp_ms, rest);
                } catch (...) {
                    // skip malformed tag
                }
            }
            pos = close + 1;
            // if there are multiple [..][..]timestamp tags, loop to next tag in same line
        }
    }

    std::sort(lyrics_.begin(), lyrics_.end());
    ESP_LOGI(TAG, "Parsed %d lyric lines (from LRC)", (int)lyrics_.size());
    return !lyrics_.empty();
}


// Lyric display thread
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started.");
    
    if (!current_lyric_url_.empty()) {
        ESP_LOGI(TAG, "Downloading external lyrics...");
        if (!DownloadLyrics(current_lyric_url_)) {
            ESP_LOGW(TAG, "Failed to download or parse external lyrics.");
        }
        current_lyric_url_.clear();
    } else {
        ESP_LOGI(TAG, "Waiting for embedded lyrics to be parsed...");
    }

    while (is_lyric_running_.load() && is_playing_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished.");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    int new_lyric_index = -1;
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;
        }
    }
    
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            display->SetChatMessage("lyric", lyric_text.c_str());
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", current_time_ms, lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}
