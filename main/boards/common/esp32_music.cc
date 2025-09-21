#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "assets/lang_config.h"

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
static Esp32Music* g_esp32_music_instance = nullptr;

class Http;

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

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         is_playing_(false), is_downloading_(false), force_stop_(false),
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false),
                         playlist_mutex_(), playlist_(), current_playlist_index_(0), is_playlist_mode_(false),
                         playlist_random_(false), playlist_type_(),current_stream_samplerate_locked_(false) {
	// 将实例指针指向 this（便于全局访问）
    g_esp32_music_instance = this;
    ESP_LOGI(TAG, "音乐播放器初始化完成");
    InitializeMp3Decoder();
	
	// 启动 lyric worker
	lyric_worker_running_.store(true);
	lyric_worker_thread_ = std::thread([this]() {
		while (lyric_worker_running_.load()) {
			std::unique_lock<std::mutex> lk(this->lyric_worker_mutex_);
			this->lyric_worker_cv_.wait(lk, [this]() {
				return !this->lyric_worker_req_url_.empty() || !this->lyric_worker_running_.load();
			});
			if (!this->lyric_worker_running_.load()) break;
			std::string url = std::move(this->lyric_worker_req_url_);
			this->lyric_worker_req_url_.clear();
			lk.unlock();

			this->lyric_worker_busy_.store(true);

			// 同步下载并解析歌词（DownloadLyricsSync 应该是阻塞式实现）
			try {
				(void) DownloadLyricsSync(url);
			} catch (const std::exception &e) {
				ESP_LOGE(TAG, "歌词工作线程 DownloadLyricsSync 异常: %s", e.what());
			} catch (...) {
				ESP_LOGE(TAG, "歌词工作线程 DownloadLyricsSync 未知异常");
			}

			this->lyric_worker_busy_.store(false);
		}
	});
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "正在销毁音乐播放器 - 停止所有操作");
    
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "等待下载线程完成（超时：5秒）");
        auto start_time = std::chrono::steady_clock::now();
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "下载线程在5秒后超时");
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
                ESP_LOGI(TAG, "仍在等待下载线程完成... (%ds)", (int)elapsed);
            }
        }
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "下载进程已完成");
    }
    
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "等待播放进程完成（超时：3秒）");
        auto start_time = std::chrono::steady_clock::now();
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "播放进程在3秒后超时");
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
        ESP_LOGI(TAG, "播放进程已完成");
    }
    
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "等待歌词进程完成");
        lyric_thread_.join();
        ESP_LOGI(TAG, "歌词进程已完成");
    }
    
    AbortCurrentLyricHttp();
    
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ClearPlaylist();
    
    g_esp32_music_instance = nullptr;
	
	// 停止 lyric worker
	lyric_worker_running_.store(false);
	{
		std::lock_guard<std::mutex> lk(lyric_worker_mutex_);
		lyric_worker_req_url_.clear();
	}
	lyric_worker_cv_.notify_all();
	if (lyric_worker_thread_.joinable()) {
		lyric_worker_thread_.join();
	}

    ESP_LOGI(TAG, "音乐播放器成功销毁");
}

// ---------- Playlist helpers ----------
int Esp32Music::GetCurrentPlaylistIndex() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (playlist_.empty()) return -1;
    return current_playlist_index_;
}

Esp32Music& Esp32Music::GetInstance() {
    if (!g_esp32_music_instance) {
        // 如果尚未初始化，抛异常或断言（Application 会捕获异常并安全返回）
        // 这里使用 abort 是最简单直接的方法，也可改为抛异常
        ESP_LOGE(TAG, "Esp32Music::GetInstance() 在实例构造之前被调用！");
        abort();
    }
    return *g_esp32_music_instance;
}

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

    cJSON* playlistArr = cJSON_GetObjectItem(root, "playlist");
    if (!playlistArr) {
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
        if (start_index < 0) start_index = 0;
        if (start_index >= (int)playlist_.size()) start_index = 0;
        current_playlist_index_ = start_index;

        if (playlist_random_) {
            std::mt19937 rng((unsigned)esp_timer_get_time());
            std::shuffle(playlist_.begin(), playlist_.end(), rng);
            current_playlist_index_ = 0;
        }
    }
    return true;
}

bool Esp32Music::PlayTrackInternal(int index) {
    PlaylistTrack track;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (index < 0 || index >= (int)playlist_.size()) {
            ESP_LOGE(TAG, "PlayTrackInternal: 无效索引 %d", index);
            return false;
        }
        current_playlist_index_ = index;
        track = playlist_[index];
    }

    StopStreaming();

    if (download_thread_.joinable()) download_thread_.join();
    if (play_thread_.joinable()) play_thread_.join();
    
    ClearAudioBuffer();

    if (!mp3_decoder_initialized_) {
        if (!InitializeMp3Decoder()) {
            ESP_LOGE(TAG, "PlayTrackInternal: MP3 解码器初始化失败");
            return false;
        }
    }

    current_music_url_ = track.audio_url;
    current_song_name_ = track.title;
    song_name_displayed_ = false;
    current_lyric_url_ = track.lyric_url;

    // [最终修复] 彻底移除旧的、会产生竞态的歌词线程逻辑
    // is_lyric_running_ = false;
    // if (lyric_thread_.joinable()) lyric_thread_.join();

    // 统一使用后台工作线程来处理歌词
    {
        std::lock_guard<std::mutex> l(lyrics_mutex_);
        lyrics_.clear();
    }
    current_lyric_index_ = -1;
    
    if (!current_lyric_url_.empty()) {
        ESP_LOGI(TAG, "向后台工作线程提交歌词下载任务: %s", current_lyric_url_.c_str());
        {
            std::lock_guard<std::mutex> lk(lyric_worker_mutex_);
            lyric_worker_req_url_ = current_lyric_url_;
        }
        lyric_worker_cv_.notify_one();
    } else {
        ESP_LOGI(TAG, "当前歌曲没有歌词链接");
    }

    return StartStreaming(current_music_url_);
}

bool Esp32Music::PlayNextInPlaylist() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (playlist_.empty()) return false;
    int next = current_playlist_index_ + 1;
    if (next >= (int)playlist_.size()) {
        return false;
    }
    current_playlist_index_ = next;
    int idx = current_playlist_index_;
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

// Download (request metadata)
bool Esp32Music::Download(const std::string& song_name) {
    ESP_LOGI(TAG, "开始获取音乐细节: %s", song_name.c_str());
    
    last_downloaded_data_.clear();
    
    std::string api_url = "http://61.145.168.125:8088/stream_pcm";
    std::string full_url = api_url + "?song=" + url_encode(song_name);
    ESP_LOGI(TAG, "请求网址: %s", full_url.c_str());
    
    const int max_retries = 3;
    int attempt = 0;
    bool success = false;

    while (attempt < max_retries && !success) {
        attempt++;
        if (attempt > 1) {
            uint32_t delay_ms = 500 * (1 << (attempt - 2));
            ESP_LOGI(TAG, "网络请求失败，将在 %dms 后重试 (尝试 %d/%d)...", (int)delay_ms, attempt, max_retries);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "创建 HTTP 客户端失败 (尝试 %d)", attempt);
            continue;
        }
        
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "application/json");
        add_auth_headers(http.get());
        
        if (!http->Open("GET", full_url)) {
            ESP_LOGE(TAG, "无法连接到音乐 API (尝试 %d)", attempt);
        } else {
            int status_code = http->GetStatusCode();
            if (status_code != 200) {
                ESP_LOGE(TAG, "HTTP GET 请求失败，状态码: %d (尝试 %d)", status_code, attempt);
            } else {
                last_downloaded_data_ = http->ReadAll(); 
                ESP_LOGI(TAG, "HTTP GET 成功, 状态 = %d, 内容长度 = %d", status_code, (int)last_downloaded_data_.length());
                success = true;
            }
            http->Close();
        }
    }

    if (!success || last_downloaded_data_.empty()) {
        ESP_LOGE(TAG, "经过 %d 次尝试后，获取音乐细节失败或内容为空。", max_retries);
        return false;
    }
    
    // [最终修复] 统一处理逻辑：无论是单曲还是列表，都构建一个播放列表
    if (SetPlaylistFromJson(last_downloaded_data_)) {
        ESP_LOGI(TAG, "播放列表已解析并获取，歌曲数量=%d", (int)GetPlaylistSize());
    } else {
        ESP_LOGI(TAG, "收到单曲信息，正在将其转换为单曲播放列表...");
        // 如果 SetPlaylistFromJson 失败，说明可能是单曲JSON
        // 我们将其手动转换为一个只包含一首歌的播放列表
        cJSON* root = cJSON_Parse(last_downloaded_data_.c_str());
        if (root) {
            PlaylistTrack track;
            cJSON* item = cJSON_GetObjectItem(root, "title");
            if (item) track.title = item->valuestring;
            item = cJSON_GetObjectItem(root, "artist");
            if (item) track.artist = item->valuestring;
            item = cJSON_GetObjectItem(root, "audio_url");
            if (item) track.audio_url = item->valuestring;
            item = cJSON_GetObjectItem(root, "lyric_url");
            if (item) track.lyric_url = item->valuestring;
            cJSON_Delete(root);

            if (!track.audio_url.empty()) {
                std::lock_guard<std::mutex> lock(playlist_mutex_);
                playlist_.clear();
                playlist_.push_back(track);
                current_playlist_index_ = 0;
                is_playlist_mode_ = true;
                ESP_LOGI(TAG, "单曲播放列表创建成功");
            } else {
                ESP_LOGE(TAG, "单曲JSON中未找到 audio_url");
                return false;
            }
        } else {
            ESP_LOGE(TAG, "无法将响应解析为播放列表或单曲JSON");
            return false;
        }
    }
    
    // 统一的播放入口
    int idx_to_play = GetCurrentPlaylistIndex();
    if (idx_to_play < 0) idx_to_play = 0;
    
    return PlayTrackInternal(idx_to_play);
}

bool Esp32Music::IsPlaying() const {
    return is_playing_.load();
}

bool Esp32Music::Play() {
    if (is_playing_.load()) {
        ESP_LOGW(TAG, "音乐已在播放");
        return true;
    }
    
    if (last_downloaded_data_.empty() && current_music_url_.empty()) {
        ESP_LOGE(TAG, "没有音乐数据可播放");
        return false;
    }
    
    if (play_thread_.joinable()) {
        if (play_thread_.get_id() != std::this_thread::get_id()) {
            play_thread_.join();
        }
    }
    
    return StartStreaming(current_music_url_);
}

bool Esp32Music::Stop() {
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "音乐未在播放或下载");
        return true;
    }
    
    ESP_LOGI(TAG, "停止音乐播放和下载");
    
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
    
    ESP_LOGI(TAG, "音乐成功停止");
    return true;
}

std::string Esp32Music::GetDownloadResult() {
    return last_downloaded_data_;
}

bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "音乐 URL 为空");
        return false;
    }
    
    ESP_LOGD(TAG, "正在启动流媒体 URL ：: %s", music_url.c_str());
    
    // --- 停止并清理旧线程的逻辑，与您的代码完全一致 ---
    is_downloading_ = false;
    is_playing_ = false;
    
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
    current_stream_samplerate_locked_ = false;
    force_stop_ = false; // 重置停止标志
	
    esp_pthread_cfg_t cfg;
    
    // 1. 配置下载线程
    cfg = esp_pthread_get_default_config(); // 获取默认配置
    cfg.stack_size = 8192;
    cfg.prio = 5;                   // 较低的优先级
    cfg.pin_to_core = 0;            // 绑定到 Core 0 (协议核)
    cfg.thread_name = "audio_download";
    esp_pthread_set_cfg(&cfg);
    
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    
    // 2. 配置播放线程
    cfg = esp_pthread_get_default_config(); // 再次获取默认配置
    cfg.stack_size = 8192;
    cfg.prio = 7;                   // 较高的优先级，确保实时性
    cfg.pin_to_core = 1;            // 绑定到 Core 1 (应用核)
    cfg.thread_name = "audio_play";
    esp_pthread_set_cfg(&cfg);
    
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "流式进程已成功启动 (下载 P5/C0, 播放 P7/C1)");
    return true;
}

bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG, "停止音乐流播放 - 当前状态：下载中=%d, 播放中=%d", 
            is_downloading_.load(), is_playing_.load());

    ResetSampleRate();
    
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "当前没有正在进行的流媒体播放");
        return true;
    }
     
    is_downloading_ = false;
    is_playing_ = false;
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");
        ESP_LOGI(TAG, "清除歌曲名称显示");
    }
    
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "音乐流播放停止信号已发送");
    return true;
}

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
                        ESP_LOGI("Esp32Music", "从 ID3 标签中找到并提取嵌入的 USLT 歌词!");
                        return lyrics;
                    }
                }
            }
        }
    }
    return "";
}

void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGI(TAG, "开始下载音频流: %s", music_url.c_str());

    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "无效 URL 格式: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }

    // 标记开始下载
    is_downloading_ = true;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");

    if (!http->Open("GET", music_url)) {
        ESP_LOGE(TAG, "连接音乐流 URL 失败");
        is_downloading_ = false;
        return;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "HTTP GET 请求失败，状态码：%d", status_code);
        http->Close();
        is_downloading_ = false;
        return;
    }

    ESP_LOGI(TAG, "开始下载音频流，状态: %d", status_code);

    const size_t chunk_size = 8192; // 单次读取增大以减少系统调用
    char* buffer = (char*)heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "分配下载缓冲区失败");
        http->Close();
        is_downloading_ = false;
        return;
    }

    size_t total_downloaded = 0;
    bool lyrics_found_and_parsed = false;

    while (is_downloading_.load()) {
        int bytes_read = http->Read(buffer, chunk_size);

        if (bytes_read < 0) {
            //ESP_LOGI(TAG, "读取音频流返回错误代码 %d，视为正常结束。", bytes_read);
            break; 
        }

        if (bytes_read == 0) {
            // 读取到 0 字节是标准的 EOF (End-of-File) 信号。
            ESP_LOGI(TAG, "音频流正常结束 (EOF)，总计下载: %u 字节", total_downloaded);
            break; 
        }
        // 只在前面一段数据查找嵌入式歌词
        if (!lyrics_found_and_parsed && total_downloaded < (128 * 1024)) {
            std::string embedded_lyrics = ExtractLyricsFromId3(reinterpret_cast<const uint8_t*>(buffer), bytes_read);
            if (!embedded_lyrics.empty()) {
                ParseLyrics(embedded_lyrics);
                lyrics_found_and_parsed = true;
            }
        }

        uint8_t* chunk_data_copy = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data_copy) {
            ESP_LOGE(TAG, "分配音频块复制的内存失败");
            // 遇到短期内存问题：短延迟后重试，而不是直接退出
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        memcpy(chunk_data_copy, buffer, bytes_read);

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            // 捕获 bytes_read（按值），以便在 predicate 中使用
            if (!buffer_cv_.wait_for(lock, std::chrono::seconds(10), [this, bytes_read] {
                return (buffer_size_ + (size_t)bytes_read) <= MAX_BUFFER_SIZE || !is_playing_.load() || !is_downloading_.load();
            })) {
                ESP_LOGW(TAG, "DownloadAudioStream: 等待缓冲区空间超时，将重试");
                heap_caps_free(chunk_data_copy);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // 播放已停止或下载被取消时退出
            if (!is_playing_.load() || !is_downloading_.load()) {
                heap_caps_free(chunk_data_copy);
                break;
            }

            audio_buffer_.push(AudioChunk(chunk_data_copy, bytes_read));
            buffer_size_ += bytes_read;
            buffer_cv_.notify_one();
        }

        total_downloaded += bytes_read;
        // 轻微 yield，避免耗尽 CPU，但不要太长
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    heap_caps_free(buffer);
    http->Close();

    is_downloading_ = false;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "音频流下载线程已完成");
}

void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "开始音频流播放 (运行于 Core %d)", xPortGetCoreID());

    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    size_t total_played = 0;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "音频编码器或MP3解码器未初始化");
        is_playing_ = false;
        return;
    }

    const size_t PREBUFFER_BYTES = 64 * 1024;
    const size_t MP3_INPUT_BUFFER_SIZE = 64 * 1024;

    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        if (!buffer_cv_.wait_for(lock, std::chrono::seconds(20), [this, PREBUFFER_BYTES] {
            return (buffer_size_ >= PREBUFFER_BYTES) || (!is_downloading_.load() && !audio_buffer_.empty()) || !is_playing_.load();
        })) {
            ESP_LOGE(TAG, "等待初始音频缓冲区超时。正在中止播放。");
            is_playing_ = false;
            return;
        }
    }

    ESP_LOGI(TAG, "开始播放，缓冲区大小为: %u", (unsigned)buffer_size_);

    uint8_t* mp3_input_buffer = (uint8_t*)heap_caps_malloc(MP3_INPUT_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "分配MP3输入缓冲区失败");
        is_playing_ = false;
        return;
    }

    int bytes_left = 0;
    uint8_t* read_ptr = mp3_input_buffer;
    bool id3_processed = false;
    int loop_counter = 0;
    current_stream_samplerate_locked_ = false;

    while (is_playing_.load()) {
        if (++loop_counter >= 20) {
            vTaskDelay(pdMS_TO_TICKS(1));
            loop_counter = 0;
        }

        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        if (current_state == kDeviceStateSpeaking || current_state == kDeviceStateListening) {
            ESP_LOGI(TAG, "检测到设备处于对话状态，正在请求切换到 Idle...");
            app.ToggleChatState();
            vTaskDelay(pdMS_TO_TICKS(300)); // 给予状态机切换的时间
            continue; // 继续下一次循环，重新检查状态
        } else if (current_state != kDeviceStateIdle) {
            ESP_LOGD(TAG, "设备不处于 Idle 状态 (state=%d)，等待...", current_state);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!codec->output_enabled()) {
            ESP_LOGW(TAG, "检测到 Codec 被禁用，正在重新启用...");
            codec->EnableOutput(true);
            vTaskDelay(pdMS_TO_TICKS(10));
            // 如果启用失败，下一轮循环会再次尝试
            if (!codec->output_enabled()) {
                 ESP_LOGE(TAG, "启用 Codec 失败，下一轮重试");
                 vTaskDelay(pdMS_TO_TICKS(100));
                 continue;
            }
        }
        
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "歌曲名称： %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }
        }
        while (bytes_left < (MP3_INPUT_BUFFER_SIZE / 2) && is_playing_.load()) {
			AudioChunk chunk;
			bool got_chunk = false;
			{
				std::unique_lock<std::mutex> lock(buffer_mutex_);
				
				// 如果缓冲区为空，进入等待逻辑
				if (audio_buffer_.empty()) {
					if (is_downloading_.load()) {
						buffer_cv_.wait_for(lock, std::chrono::milliseconds(50), [this]{
							return !audio_buffer_.empty() || !is_downloading_.load() || !is_playing_.load();
						});
					} else {
						// 下载已结束
						if (bytes_left == 0) {
							ESP_LOGI(TAG, "播放缓冲区为空，下载已完成.");
							goto playback_end;
						} else {
							// 还有剩余数据要处理，先退出内层循环去解码
							break;
						}
					}
				}

				// 再次检查缓冲区是否已有数据
				if (!audio_buffer_.empty()) {
					chunk = std::move(audio_buffer_.front());
					audio_buffer_.pop();
					buffer_size_ -= chunk.size;
					got_chunk = true;
					buffer_cv_.notify_one();
				}
			} 
            if (!got_chunk) break;
            if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                memmove(mp3_input_buffer, read_ptr, bytes_left);
            }
            read_ptr = mp3_input_buffer;
            size_t space_available = MP3_INPUT_BUFFER_SIZE - bytes_left;
            size_t copy_size = std::min((size_t)chunk.size, space_available);
            memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
            bytes_left += copy_size;
        }
        if (bytes_left == 0) {
            if (!is_downloading_.load()) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!id3_processed && bytes_left >= 10) {
            size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
            if (id3_skip > 0) {
                read_ptr += id3_skip;
                bytes_left -= id3_skip;
                ESP_LOGI(TAG, "跳过 ID3 标签: %u 字节", (unsigned int)id3_skip);
            }
            id3_processed = true;
        }
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGD(TAG, "未找到MP3同步字，跳过 %d 字节", bytes_left);
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
            if (mp3_frame_info_.samprate <= 0 || mp3_frame_info_.nChans <= 0 || mp3_frame_info_.outputSamps <= 0) {
                continue;
            }
            if (!current_stream_samplerate_locked_) {
                if (codec->output_sample_rate() != 44100) {
                     if (codec->SetOutputSampleRate(44100)) {
                         ESP_LOGI(TAG, "采样率已锁定为 44100 Hz");
                     } else {
                         ESP_LOGE(TAG, "设置硬件采样率 44100 Hz 失败!");
                     }
                } else {
                     ESP_LOGI(TAG, "硬件采样率已是 44100 Hz，直接锁定");
                }
                current_stream_samplerate_locked_ = true;
            }
            int samples_per_channel = mp3_frame_info_.outputSamps / mp3_frame_info_.nChans;
            int frame_duration_ms = (samples_per_channel * 1000) / mp3_frame_info_.samprate;
            current_play_time_ms_ += frame_duration_ms;
            UpdateLyricDisplay(current_play_time_ms_ + 600);
            if (mp3_frame_info_.outputSamps > 0) {
                std::vector<int16_t> mono_buffer;
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                if (mp3_frame_info_.nChans == 2) {
                    mono_buffer.resize(final_sample_count / 2);
                    for (int i = 0; i < final_sample_count / 2; ++i) {
                        mono_buffer[i] = (pcm_buffer[i * 2] + pcm_buffer[i * 2 + 1]) / 2;
                    }
                    final_pcm_data = mono_buffer.data();
                    final_sample_count /= 2;
                }
                AudioStreamPacket packet;
                packet.sample_rate = codec->output_sample_rate();
                packet.channels = 1;
                packet.frame_duration = frame_duration_ms;
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);
                app.AddAudioData(std::move(packet));
				total_played += pcm_size_bytes;
            }
        } else {
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            }
        }
    }

playback_end:
    if (mp3_input_buffer) heap_caps_free(mp3_input_buffer);

    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetMusicInfo("");
        ESP_LOGI(TAG, "播放结束时清除歌曲名称显示");
    }
    ResetSampleRate();
    ESP_LOGI(TAG, "音频流播放完成，总共播放: %u 字节", (unsigned)total_played);
    is_playing_ = false;

    // 只有在不是被强制停止的情况下，才自动播放下一首
    if (!force_stop_.load()) {
        try {
            OnPlaybackFinished();
        } catch (...) {
            ESP_LOGW(TAG, "OnPlaybackFinished 抛出异常");
        }
    } else {
        ESP_LOGI(TAG, "播放被强制停止，不自动切换到下一首。");
    }
}

void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        // 用const引用获取队列首元素（不触发拷贝）
        //const AudioChunk& chunk = audio_buffer_.front();
        // 先释放数据（引用有效，chunk.data可安全访问）
        //if (chunk.data) {
        //    heap_caps_free(chunk.data);
        //}
        // 再删除队列元素（避免引用失效后访问）
        audio_buffer_.pop();
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "音频缓冲区已清空");
}

bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "初始化MP3解码器失败");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3解码器成功初始化");
    return true;
}

void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3解码器已清理");
}

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
    
    ESP_LOGI(TAG, "发现 ID3v2 标签，跳过 %u 字节", (unsigned int)total_skip);
    return total_skip;
}
 
void Esp32Music::AbortCurrentLyricHttp() {
    std::lock_guard<std::mutex> lock(lyric_http_mutex_);
    if (current_lyric_http_) {
        // 修复：Http类无Abort()，用Close()替代（符合HTTP客户端常规逻辑）
        current_lyric_http_->Close();
        current_lyric_http_.reset();  // unique_ptr自动释放内存
        ESP_LOGI(TAG, "中止当前歌词下载");
    }
}

bool Esp32Music::DownloadLyricsSync(const std::string& lyric_url) {
    ESP_LOGI(TAG, "后台工作线程正在下载歌词: %s", lyric_url.c_str());

    // 立即清空旧歌词，防止显示上一首的内容
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
        current_lyric_index_ = -1;
    }

    if (lyric_url.empty()) {
        ESP_LOGW(TAG, "歌词URL为空，下载中止。");
        return false;
    }

    const int max_retries = 3;
    int retry_count = 0;
    std::string lyric_content;
    bool success = false;

    while (retry_count < max_retries && !success) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "重试歌词下载（第 %d 次）", retry_count + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        
        if (!http) {
            ESP_LOGE(TAG, "创建用于歌词下载的HTTP客户端失败");
            retry_count++;
            continue;
        }

        http->SetHeader("Accept", "text/plain");
        add_auth_headers(http.get());

        if (!http->Open("GET", lyric_url)) {
            ESP_LOGE(TAG, "打开歌词的HTTP连接失败");
            retry_count++;
            continue;
        }

        int status_code = http->GetStatusCode();
        if (status_code != 200) {
            ESP_LOGE(TAG, "歌词HTTP GET请求失败，状态码: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        lyric_content = http->ReadAll();
        http->Close();

        if (!lyric_content.empty()) {
            success = true;
        } else {
            ESP_LOGW(TAG, "下载的歌词内容为空，重试...");
            retry_count++;
        }
    } 

    if (!success) {
        ESP_LOGE(TAG, "经过 %d 次尝试后下载歌词失败", max_retries);
        return false;
    }

    ESP_LOGI(TAG, "歌词下载成功，大小: %d bytes", (int)lyric_content.length());
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

	ESP_LOGI(TAG, "解析了 %d 行 LRC 歌词", (int)lyrics_.size()); 
    // 补充返回值：歌词列表非空则视为解析成功
    return !lyrics_.empty();
}

void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "歌词显示进程已启动.");

    // 如果存在外部歌词 URL，则在这个线程内同步下载并解析（DownloadLyricsSync 应该填充 lyrics_）
    {
        std::string lyric_url_copy;
        {
            std::lock_guard<std::mutex> l(lyrics_mutex_);
            lyric_url_copy = current_lyric_url_;
        }

        if (!lyric_url_copy.empty()) {
            ESP_LOGI(TAG, "正在下载外部歌词...");
            // 调用你的同步版本下载/解析函数（如果你把名字定为 DownloadLyricsSync）
            bool ok = false;
            try {
                ok = DownloadLyricsSync(lyric_url_copy); // <- 确保头文件有声明
            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "DownloadLyricsSync 抛出异常: %s", e.what());
                ok = false;
            } catch (...) {
                ESP_LOGE(TAG, "DownloadLyricsSync 抛出未知异常");
                ok = false;
            }

            if (!ok) {
                ESP_LOGW(TAG, "无法下载或解析网址的外部歌词=%s", lyric_url_copy.c_str());
            } else {
                // 打日志显示解析到多少行（使用已有数据结构）
                {
                    std::lock_guard<std::mutex> l(lyrics_mutex_);
                    ESP_LOGI(TAG, "歌词已解析，行数=%u", (unsigned)lyrics_.size());
                }
            }

            // 清空 current_lyric_url_（防止重复下载）
            {
                std::lock_guard<std::mutex> l(lyrics_mutex_);
                current_lyric_url_.clear();
            }
        } else {
            ESP_LOGI(TAG, "没有外部歌词网址，将等待嵌入的歌词。");
        }
    }

    // 原有逻辑：循环等待播放状态改变（播放线程会周期性调用 UpdateLyricDisplay(current_play_time_ms)）
    // 当 is_lyric_running_ 或 is_playing_ 变为 false 时线程退出
    while (is_lyric_running_.load() && is_playing_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ESP_LOGI(TAG, "歌词显示进程已完成。");
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
            ESP_LOGD(TAG, "歌词更新于 %lldms: %s", current_time_ms, lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 当一首歌播放结束时调用（在 PlayAudioStream 线程结束后会触发）
void Esp32Music::OnPlaybackFinished() {
    bool playlist_mode = false;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        playlist_mode = is_playlist_mode_;
    }
    if (!playlist_mode) return;

    bool advanced = false;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (!playlist_.empty()) {
            int next = current_playlist_index_ + 1;
            if (next < (int)playlist_.size()) {
                current_playlist_index_ = next;
                advanced = true;
            } else {
                advanced = false;
            }
        }
    }

    if (advanced) {
        int idx;
        {
            std::lock_guard<std::mutex> lock(playlist_mutex_);
            idx = current_playlist_index_;
        }
        std::thread t([this, idx]() {
            PlayTrackInternal(idx);
        });
        t.detach();
    } else {
        //处理播放列表播放结束的情况
        ESP_LOGI(TAG, "播放列表完成。正在清理状态。");
        
        std::thread([this]() {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                display->SetMusicInfo("播放列表已结束");
            }

            // 延时5秒
            std::this_thread::sleep_for(std::chrono::seconds(5));

            display = board.GetDisplay();
            if (display) {
                //splay->SetMusicInfo(""); 

				display->SetStatus(Lang::Strings::STANDBY);
				display->SetEmotion("neutral");
				display->SetChatMessage("system", "");
            }

        // 清空内部播放列表数据
        ClearPlaylist();
		}).detach();
    }
}

void Esp32Music::NextPlaylistTrack() {
    // 保护 playlist 相关状态
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (!is_playlist_mode_ || playlist_.empty()) {
        ESP_LOGE(TAG, "不在播放列表模式或播放列表为空");
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

    ESP_LOGI(TAG, "NextPlaylistTrack: 准备曲目 %s（索引 %d）", current_song_name_.c_str(), current_playlist_index_);

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
    //lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
	
	// 通知 lyric worker 去下载/解析并更新歌词
	{
		std::lock_guard<std::mutex> lk(lyric_worker_mutex_);
		lyric_worker_req_url_ = current_lyric_url_; // 用 current_lyric_url_ 字符串
	}
	lyric_worker_cv_.notify_one();

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

    // 启动下载线程（传入当前音乐 URL）
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, current_music_url_);

    // 启动播放线程
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);

    ESP_LOGI(TAG, "NextPlaylistTrack: 开始播放曲目 %s (网址: %s)", current_song_name_.c_str(), current_music_url_.c_str());
}

void Esp32Music::ForceStopAndClear() {
    ESP_LOGI(TAG, "收到强制停止指令 (来自长按)");
    force_stop_ = true;
    StopStreaming();
    ClearPlaylist();
}