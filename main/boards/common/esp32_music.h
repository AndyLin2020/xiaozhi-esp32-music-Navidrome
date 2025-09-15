#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <utility>
#include "music.h"  // 假设存在基础 Music 类
#include "board.h"  // 假设存在 Board 类
#include "http.h"   // 假设存在 Http 类
#include "mp3dec.h" // MP3 解码器头文件

// 音频数据块结构（修复：添加移动语义，避免拷贝）
struct AudioChunk {
    uint8_t* data;
    size_t size;

    // 默认构造
    AudioChunk() : data(nullptr), size(0) {}
    // 带参构造
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}

    // 移动构造（关键：解决拷贝禁用问题）
    AudioChunk(AudioChunk&& other) noexcept : data(other.data), size(other.size) {
        other.data = nullptr; // 转移所有权后置空原指针
        other.size = 0;
    }

    // 移动赋值（关键：解决赋值禁用问题）
    AudioChunk& operator=(AudioChunk&& other) noexcept {
        if (this != &other) {
            // 释放当前对象资源
            if (data) heap_caps_free(data);
            // 转移其他对象资源
            data = other.data;
            size = other.size;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }

    // 禁用拷贝（保持原逻辑）
    AudioChunk(const AudioChunk&) = delete;
    AudioChunk& operator=(const AudioChunk&) = delete;

    // 析构函数（避免内存泄漏）
    ~AudioChunk() {
        if (data) {
            heap_caps_free(data);
            data = nullptr;
        }
    }
};

// 歌词项结构体（时间+内容）
struct LyricItem {
    int64_t time_ms;  // 歌词对应的播放时间（毫秒）
    std::string content;  // 歌词内容
};

class Esp32Music : public Music {
public:
    Esp32Music();
    ~Esp32Music();

    // 重写基础接口
    virtual bool Download(const std::string& song_name) override;
    virtual bool Play() override;
    virtual bool Stop() override;
    virtual std::string GetDownloadResult() override;
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }

    // 播放列表接口
    bool Next();                       // 下一首
    bool Prev();                       // 上一首
    bool PlayTrackAt(int index);       // 播放指定索引曲目
    size_t GetPlaylistSize() const;    // 获取播放列表大小
    void ClearPlaylist();
    bool SetPlaylistFromJson(const std::string& json);  // 从JSON构建播放列表
    void NextPlaylistTrack();          // 播放列表下一首（已声明，解决未定义错误）

	// 返回当前播放的 playlist index（0-based）。若没有播放/队列为空，返回 -1
	int GetCurrentPlaylistIndex() const;

	// 单例访问（如果你愿意使用单例方式）；可选：如果你已有其它获取实例方法则不必添加
	static Esp32Music& GetInstance();
	
    // 播放列表轨道结构体
    struct PlaylistTrack {
        std::string id;
        std::string title;
        std::string artist;
        std::string album;
        std::string audio_url;
        std::string lyric_url;
    };

private:
    // 基础状态变量
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;

    // 歌词相关变量
    std::string current_lyric_url_;
    std::vector<std::pair<int64_t, std::string>> lyrics_;  // 时间戳（毫秒）+ 歌词
    std::mutex lyrics_mutex_;                              // 歌词线程安全锁（修复：专用锁）
    std::atomic<int> current_lyric_index_;                 // 当前歌词索引
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;

    // 播放/下载状态
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间（毫秒）
    int64_t last_frame_time_ms_;    // 上一帧时间戳
    int total_frames_decoded_;      // 已解码帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 128 * 1024;  // 256KB 最大缓冲区
    static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024;   // 32KB 最小播放缓冲区

    // MP3解码器
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;

    // 歌词HTTP下载（修复：使用 unique_ptr 管理）
    std::mutex lyric_http_mutex_;
    std::unique_ptr<Http> current_lyric_http_;

    // 播放列表变量
    mutable std::mutex playlist_mutex_;  // mutable 允许const方法加锁
    std::vector<PlaylistTrack> playlist_;
    int current_playlist_index_;
    bool is_playlist_mode_;
    bool playlist_random_;
    std::string playlist_type_;
	
	// ---- 新增：持久化的歌词 worker 线程相关 ----
	std::thread lyric_worker_thread_;              // 长期运行的歌词 worker 线程
	std::atomic<bool> lyric_worker_running_{false}; // worker 生存标志
	std::mutex lyric_worker_mutex_;
	std::condition_variable lyric_worker_cv_;
	std::string lyric_worker_req_url_;             // 待处理的歌词 URL（一次只处理一个）
	std::atomic<bool> lyric_worker_busy_{false};   // worker 是否正忙

    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();
    size_t SkipId3Tag(uint8_t* data, size_t size);
    std::string ExtractLyricsFromId3(const uint8_t* data, size_t size);

    // 歌词相关方法（修复：函数名统一为 DownloadLyricsSync） 
	bool DownloadLyricsSync(const std::string& lyric_url);// 最新歌词下载函数
    bool ParseLyrics(const std::string& lyric_content); // 自动解析LRC/JSON歌词
    void ParseLrcLyrics(const std::string& lyric_data); // 单独解析LRC歌词
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    void AbortCurrentLyricHttp();  // 中止歌词下载（适配 unique_ptr）

    // 播放列表内部方法
    bool PlayNextInPlaylist();
    bool PlayPrevInPlaylist();
    bool PlayTrackInternal(int index);
    void OnPlaybackFinished();      // 播放结束回调（自动切歌）
};

#endif // ESP32_MUSIC_H