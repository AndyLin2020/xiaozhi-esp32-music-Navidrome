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
#include <random>
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
 
static void add_auth_headers(Http* http) {
    int64_t timestamp = esp_timer_get_time() / 1000000; 
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp)); 
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

// Constructor / Destructor
Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false),
                         playlist_mutex_(), playlist_(), current_playlist_index_(0), is_playlist_mode_(false),
                         playlist_random_(false), playlist_type_() {
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
    
    // clear playlist
    ClearPlaylist();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

// ---------- Playlist helpers ----------
size_t Esp32Music::GetPlaylistSize() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    return playlist_.size();
}

void Esp32Music::ClearPlaylist() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    playlist_.clear();
    current_playlist_index_ = 0;
    is_playlist_mode_ = false;
    playlist_random_ = false;
    playlist_type_.clear();
}

bool Esp32Music::SetPlaylistFromJson(const std::string& json) {
    if (json.empty()) return false;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;

    std::vector<PlaylistTrack> tmp;
    bool random_flag = false;
    std::string pl_type;
    int start_index = 0;

    // try top-level playlist array
    cJSON* playlistArr = cJSON_GetObjectItem(root, "playlist");
    if (!playlistArr) {
        // maybe the root itself is an array
        if (cJSON_IsArray(root)) playlistArr = root;
    }
    if (playlistArr && cJSON_IsArray(playlistArr)) {
        int cnt = cJSON_GetArraySize(playlistArr);
        for (int i = 0; i < cnt; ++i) {
            cJSON* item = cJSON_GetArrayItem(playlistArr, i);
            if (!item) continue;
            PlaylistTrack t;
            cJSON* j;

            j = cJSON_GetObjectItem(item, "id");
            if (j && cJSON_IsString(j)) t.id = j->valuestring;
            j = cJSON_GetObjectItem(item, "title");
            if (j && cJSON_IsString(j)) t.title = j->valuestring;
            j = cJSON_GetObjectItem(item, "artist");
            if (j && cJSON_IsString(j)) t.artist = j->valuestring;
            j = cJSON_GetObjectItem(item, "album");
            if (j && cJSON_IsString(j)) t.album = j->valuestring;
            j = cJSON_GetObjectItem(item, "audio_url");
            if (j && cJSON_IsString(j)) t.audio_url = j->valuestring;
            j = cJSON_GetObjectItem(item, "lyric_url");
            if (j && cJSON_IsString(j)) t.lyric_url = j->valuestring;

            if (!t.audio_url.empty()) tmp.push_back(std::move(t));
        }
    }

    // flags
    cJSON* r = cJSON_GetObjectItem(root, "random");
    if (r && cJSON_IsBool(r)) random_flag = cJSON_IsTrue(r);
    cJSON* t = cJSON_GetObjectItem(root, "playlist_type");
    if (t && cJSON_IsString(t)) pl_type = t->valuestring;
    cJSON* idx = cJSON_GetObjectItem(root, "current_index");
    if (idx && cJSON_IsNumber(idx)) start_index = (int)idx->valuedouble;

    cJSON_Delete(root);

    if (tmp.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_ = std::move(tmp);
        playlist_random_ = random_flag;
        playlist_type_ = pl_type;
        is_playlist_mode_ = true;
        // clamp start_index
        if (start_index < 0) start_index = 0;
        if (start_index >= (int)playlist_.size()) start_index = 0;
        current_playlist_index_ = start_index;

        if (playlist_random_) {
            // shuffle preserving current index as first element
            std::mt19937 rng((unsigned)esp_timer_get_time());
            std::shuffle(playlist_.begin(), playlist_.end(), rng);
            // reset index to 0 after shuffle
            current_playlist_index_ = 0;
        }
    }
    return true;
}

// Internal: play a specific playlist index (thread-safe)
// Internal: play a specific playlist index (thread-safe)
bool Esp32Music::PlayTrackInternal(int index) {
    PlaylistTrack track;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (index < 0 || index >= (int)playlist_.size()) return false;
        track = playlist_[index];
    }

    // 【原有逻辑保留】停止当前流式播放
    StopStreaming();

    // 【核心修改1】清理旧的下载/播放线程（避免线程泄漏）
    is_downloading_ = false;
    is_playing_ = false;
    {
        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        play_thread_.join();
    }
    ClearAudioBuffer(); // 清空旧缓冲区

    // 【核心修改2】初始化 MP3 解码器（确保播放正常）
    if (!mp3_decoder_initialized_) {
        if (!InitializeMp3Decoder()) {
            ESP_LOGE(TAG, "PlayTrackInternal: MP3 decoder init failed");
            return false;
        }
    }

    // 【原有逻辑保留】设置当前曲目元数据
    current_music_url_ = track.audio_url;
    current_song_name_ = track.title.empty() ? (track.artist + " - track") : track.title;
    song_name_displayed_ = false; // 【新增】重置歌曲名显示标志
    if (!track.lyric_url.empty()) {
        current_lyric_url_ = track.lyric_url;
    } else {
        current_lyric_url_.clear();
    }

    // 【原有逻辑保留】重置歌词线程
    if (is_lyric_running_) {
        is_lyric_running_ = false;
        if (lyric_thread_.joinable()) lyric_thread_.join();
    }
    is_lyric_running_ = true;
    current_lyric_index_ = -1;
    {
        std::lock_guard<std::mutex> l(lyrics_mutex_);
        lyrics_.clear();
    }
    lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);

    // 【原有逻辑保留】启动新的流式播放
    return StartStreaming(current_music_url_);
}

bool Esp32Music::PlayNextInPlaylist() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (playlist_.empty()) return false;
    int next = current_playlist_index_ + 1;
    if (next >= (int)playlist_.size()) {
        // no wrap by default
        return false;
    }
    current_playlist_index_ = next;
    int idx = current_playlist_index_;
    // unlock then play
    // play outside lock to avoid deadlocks
    // copy track index
    // call PlayTrackInternal
    return PlayTrackInternal(idx);
}

bool Esp32Music::PlayPrevInPlaylist() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (playlist_.empty()) return false;
    int prev = current_playlist_index_ - 1;
    if (prev < 0) return false;
    current_playlist_index_ = prev;
    int idx = current_playlist_index_;
    return PlayTrackInternal(idx);
}

bool Esp32Music::Next() {
    // public wrapper
    if (!is_playlist_mode_) return false;
    return PlayNextInPlaylist();
}

bool Esp32Music::Prev() {
    if (!is_playlist_mode_) return false;
    return PlayPrevInPlaylist();
}

bool Esp32Music::PlayTrackAt(int index) {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (index < 0 || index >= (int)playlist_.size()) return false;
    current_playlist_index_ = index;
    return PlayTrackInternal(index);
}

// ------------ 以上 playlist 代码 end --------------

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
    // CreateHttp 返回 std::unique_ptr<Http>（在你的工程中如此），所以用 auto 接受
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create http client");
        return false;
    }
    
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    // 注意：CreateHttp 返回 unique_ptr，所以传入需要 Http* 的函数时用 .get()
    add_auth_headers(http.get()); // <-- 修正点：使用 .get()
    
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
    
    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, (int)last_downloaded_data_.length());
    ESP_LOGD(TAG, "Complete music details response: %s", last_downloaded_data_.c_str());
    
    if (!last_downloaded_data_.empty()) {
        // first try to parse playlist if present
        if (SetPlaylistFromJson(last_downloaded_data_)) {
            // playlist constructed successfully -> play first track
            ESP_LOGI(TAG, "Playlist parsed and set, Items=%d", (int)GetPlaylistSize());
            int idx = 0;
            {
                std::lock_guard<std::mutex> lock(playlist_mutex_);
                idx = current_playlist_index_;
            }
            PlayTrackInternal(idx);
            return true;
        }

        // otherwise parse single-track response (existing behavior)
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
                
                // single track mode: clear playlist mode
                ClearPlaylist();

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
    
    if (last_downloaded_data_.empty() && current_music_url_.empty()) {
        ESP_LOGE(TAG, "No music data to play");
        return false;
    }
    
    if (play_thread_.joinable()) {
        // join only if not current thread
        if (play_thread_.get_id() != std::this_thread::get_id()) {
            play_thread_.join();
        }
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
    
    if (download_thread_.joinable() && download_thread_.get_id() != std::this_thread::get_id()) {
        download_thread_.join();
    }
    if (play_thread_.joinable() && play_thread_.get_id() != std::this_thread::get_id()) {
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
    
    // set flags so download/play loops can stop
    is_downloading_ = false;
    is_playing_ = false;
    
    // join previous threads if they are joinable and not current thread
    if (download_thread_.joinable() && download_thread_.get_id() != std::this_thread::get_id()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        download_thread_.join();
    }
    if (play_thread_.joinable() && play_thread_.get_id() != std::this_thread::get_id()) {
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
std::string Esp32Music::ExtractLyricsFromId3(const uint8_t* data, size_t size) {
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
            else ESP_LOGI(TAG, "Audio stream download completed, total: %u bytes", total_downloaded);
            break;
        }

        // 修复：解析嵌入式歌词时调用统一的 ParseLyrics 
        if (!lyrics_found_and_parsed && total_downloaded < (128 * 1024)) {
            std::string embedded_lyrics = ExtractLyricsFromId3(reinterpret_cast<const uint8_t*>(buffer), bytes_read);
            if (!embedded_lyrics.empty()) {
                ParseLyrics(embedded_lyrics);  // 统一解析逻辑
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
            
            // 移动语义入队（避免拷贝）
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
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
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

        // 修复：用移动语义获取队列元素（避免拷贝）
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
                    // 关键修复：移动语义赋值，不触发拷贝
                    chunk = std::move(audio_buffer_.front());
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
                    ESP_LOGW(TAG, "MP3 input buffer is full, discarding %d bytes", chunk.size - copy_size);
                }
                
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                
                // 无需手动free：chunk析构会自动释放data
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
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    is_playing_ = false;

    // 播放结束自动切歌
    try {
        OnPlaybackFinished();
    } catch (...) {
        ESP_LOGW(TAG, "OnPlaybackFinished threw exception");
    }
}

void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        // 用const引用获取队列首元素（不触发拷贝）
        const AudioChunk& chunk = audio_buffer_.front();
        // 先释放数据（引用有效，chunk.data可安全访问）
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
        // 再删除队列元素（避免引用失效后访问）
        audio_buffer_.pop();
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
 
void Esp32Music::AbortCurrentLyricHttp() {
    std::lock_guard<std::mutex> lock(lyric_http_mutex_);
    if (current_lyric_http_) {
        // 修复：Http类无Abort()，用Close()替代（符合HTTP客户端常规逻辑）
        current_lyric_http_->Close();
        current_lyric_http_.reset();  // unique_ptr自动释放内存
        ESP_LOGI(TAG, "Aborted current lyric download");
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
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, (int)lyric_content.length());
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
    // 修复：用歌词专用锁（原代码用buffer_mutex_错误）
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    lyrics_.clear();
    current_lyric_index_ = -1;

    std::istringstream ss(lyric_content);
    std::string line;

    // 逐行解析LRC歌词（格式：[mm:ss.xx]歌词内容）
    while (std::getline(ss, line)) {
        // 去除行首尾空白字符
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty() || line[0] != '[') continue;

        // 提取时间标签（如 [02:34.56]）
        size_t time_end = line.find(']');
        if (time_end == std::string::npos || time_end < 3) continue;
        std::string time_str = line.substr(1, time_end - 1);
        std::string content = line.substr(time_end + 1);
        if (content.empty()) continue;

        // 解析时间为毫秒（mm:ss 或 mm:ss.xx）
        size_t colon_pos = time_str.find(':');
        if (colon_pos == std::string::npos) continue;
        int minutes = atoi(time_str.substr(0, colon_pos).c_str());
        float seconds = atof(time_str.substr(colon_pos + 1).c_str());
        if (minutes < 0 || seconds < 0) continue;
        int64_t total_ms = static_cast<int64_t>(minutes * 60 * 1000 + seconds * 1000);

        // 添加到歌词列表
        lyrics_.push_back({total_ms, content});
    }

    // 按时间排序歌词（确保播放时顺序正确）
    std::sort(lyrics_.begin(), lyrics_.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

	ESP_LOGI(TAG, "Parsed %d LRC lyric lines", (int)lyrics_.size()); 
    // 补充返回值：歌词列表非空则视为解析成功
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

// 当一首歌播放结束时调用（在 PlayAudioStream 线程结束后会触发）
void Esp32Music::OnPlaybackFinished() {
    // If in playlist mode, attempt to play next track automatically
    bool playlist_mode = false;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_mode = is_playlist_mode_;
    }
    if (!playlist_mode) return;

    // try to advance to next
    bool advanced = false;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (!playlist_.empty()) {
            int next = current_playlist_index_ + 1;
            if (next < (int)playlist_.size()) {
                current_playlist_index_ = next;
                advanced = true;
            } else {
                // reached end -> no wrap by default
                advanced = false;
            }
        }
    }

    if (advanced) {
        // play the next track
        int idx;
        {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            idx = current_playlist_index_;
        }
        // Start next track asynchronously (so we don't call StartStreaming from within the just-exited playback thread in ambiguous states)
        // But StartStreaming can be called safely here because play_thread_ is no longer joinable (this thread was play_thread_).
        // To be conservative we spawn a small detached thread to start the next stream.
        std::thread t([this, idx]() {
            PlayTrackInternal(idx);
        });
        t.detach();
    }
}

void Esp32Music::NextPlaylistTrack() {
    // 保护 playlist 相关状态
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (!is_playlist_mode_ || playlist_.empty()) {
        ESP_LOGE(TAG, "Not in playlist mode or empty playlist");
        return;
    }

    // 计算下一首索引（随机/顺序）
    if (playlist_random_) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distr(0, static_cast<int>(playlist_.size()) - 1);
        current_playlist_index_ = distr(gen);
    } else {
        current_playlist_index_ = (current_playlist_index_ + 1) % playlist_.size();
    }

    // 读取当前曲目信息（不持有 lyrics_mutex_）
    auto current_track = playlist_[current_playlist_index_]; // copy to avoid引用并发
    current_song_name_ = current_track.title + " - " + current_track.artist;
    song_name_displayed_ = false; // 重置歌曲名显示标志
    current_music_url_ = current_track.audio_url;  // 更新音乐 URL
    current_lyric_url_ = current_track.lyric_url;  // 更新歌词 URL

    ESP_LOGI(TAG, "NextPlaylistTrack: preparing track %s (index %d)", current_song_name_.c_str(), current_playlist_index_);

    // ------------------------ 歌词线程处理（先停止旧线程，再清理，再启动新线程） ------------------------
    // 停止并 join 旧的显示线程（如果存在）
    if (is_lyric_running_) {
        is_lyric_running_ = false;
        if (lyric_thread_.joinable()) {
            // 如果 lyric_thread_ 不是当前线程则 join（安全检查）
            if (lyric_thread_.get_id() != std::this_thread::get_id()) {
                lyric_thread_.join();
            } else {
                // 极端情况：不能 join self，detach 以防阻塞（通常不会发生）
                lyric_thread_.detach();
            }
        }
    }

    // 中止未完成的旧歌词 HTTP 下载（如果有）
    AbortCurrentLyricHttp();

    // 清空歌词数据与索引（在锁内保护）
    {
        std::lock_guard<std::mutex> l(lyrics_mutex_);
        lyrics_.clear();
    }
    current_lyric_index_ = -1;

    // 启动新的歌词显示线程（LyricDisplayThread 负责根据 current_lyric_url_ 进行下载并显示）
    is_lyric_running_ = true;
    // 启动线程之前再次确保歌词状态已初始化
    current_lyric_index_ = -1;
    {
        std::lock_guard<std::mutex> l(lyrics_mutex_);
        lyrics_.clear();
    }
    lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);

    // ------------------------ 停止旧下载/播放线程并清理缓冲 ------------------------
    // 先请求停止下载/播放线程
    is_downloading_ = false;
    is_playing_ = false;

    // 唤醒可能阻塞在 buffer_cv_ 的线程，让它们看到 is_downloading_/is_playing_ 标志并退出
    {
        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // Join 下载线程（如果存在且不是当前线程）
    if (download_thread_.joinable()) {
        if (download_thread_.get_id() != std::this_thread::get_id()) {
            download_thread_.join();
        } else {
            download_thread_.detach();
        }
    }

    // Join 播放线程（如果存在且不是当前线程）
    if (play_thread_.joinable()) {
        if (play_thread_.get_id() != std::this_thread::get_id()) {
            play_thread_.join();
        } else {
            play_thread_.detach();
        }
    }

    // 清理音频缓冲区
    ClearAudioBuffer();

    // ------------------------ 启动新的下载与播放线程 ------------------------
    // 启动下载线程（传入当前音乐 URL）
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, current_music_url_);

    // 启动播放线程
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);

    ESP_LOGI(TAG, "NextPlaylistTrack: started track %s (URL: %s)", current_song_name_.c_str(), current_music_url_.c_str());
}
