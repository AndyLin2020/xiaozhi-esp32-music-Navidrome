#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <memory>

#include "music.h"

// forward declare Http (实现由 Board::GetInstance().GetNetwork() 提供)
class Http;

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    Esp32Music();
    ~Esp32Music();

    virtual bool Download(const std::string& song_name) override;
    virtual bool Play() override;
    virtual bool Stop() override;
    virtual std::string GetDownloadResult() override;
    
    // 新增：流式控制
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }

    // 播放列表 / 切歌 接口
    bool Next();                       // 播放下一首（若playlist模式）
    bool Prev();                       // 播放上一首（若playlist模式）
    bool PlayTrackAt(int index);       // 播放播放列表中的指定索引
    size_t GetPlaylistSize() const;    // 返回 playlist_ 的大小（线程安全）
    void ClearPlaylist();
    bool SetPlaylistFromJson(const std::string& json); // 从返回的 JSON 构建 playlist

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB最小播放缓冲（降低以减少brownout风险）
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    
    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    // 新增: 管理歌词下载时的 HTTP 客户端指针与互斥
    std::mutex lyric_http_mutex_;
    std::unique_ptr<Http> current_lyric_http_;
    void AbortCurrentLyricHttp(); // 关闭并释放当前 lyric http

    // ========== Playlist 支持 ==========
public:
    struct PlaylistTrack {
        std::string id;
        std::string title;
        std::string artist;
        std::string album;
        std::string audio_url;
        std::string lyric_url;
    };

private:
    mutable std::mutex playlist_mutex_; // mutable so const methods can lock
    std::vector<PlaylistTrack> playlist_;
    int current_playlist_index_;    // 当前在 playlist 中的索引
    bool is_playlist_mode_;         // 是否在播放 playlist
    bool playlist_random_;          // 是否随机播放
    std::string playlist_type_;     // 如 "artist_temp" / "album" 等

    // playlist 操作的内部实现
    bool PlayNextInPlaylist();      // 移动并播放下一首（返回是否成功）
    bool PlayPrevInPlaylist();      // 播放上一首
    bool PlayTrackInternal(int index); // 内部切歌（线程安全）
    void OnPlaybackFinished();      // 播放完后回调（检查 playlist 并自动切歌）

};

#endif // ESP32_MUSIC_H
