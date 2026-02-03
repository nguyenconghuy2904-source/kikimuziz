#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <cstdint>

/**
 * @brief Base interface for music player implementations
 */
class MusicPlayer {
public:
    virtual ~MusicPlayer() = default;
    
    // Playback control
    virtual bool Play(const std::string& url) = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    
    // Volume control
    virtual void SetVolume(int volume) = 0;
    virtual int GetVolume() const = 0;
    
    // State
    virtual bool IsPlaying() const = 0;
    virtual bool IsPaused() const = 0;
    virtual bool IsDownloading() const { return false; }
    
    // Buffer info
    virtual size_t GetBufferSize() const { return 0; }
    virtual int16_t* GetAudioData() { return nullptr; }
    
    // Progress (in seconds)
    virtual float GetProgress() const { return 0.0f; }
    virtual float GetDuration() const { return 0.0f; }
    virtual bool SeekTo(float seconds) { return false; }
    
    // Metadata
    virtual std::string GetTitle() const { return ""; }
    virtual std::string GetArtist() const { return ""; }
};

/**
 * @brief Music interface for streaming music functionality
 * This is the base class that Esp32Music inherits from
 */
class Music {
public:
    virtual ~Music() = default;
    
    // Download and streaming
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming(bool send_notification = true) = 0;  // send_notification: send MCP notification when stopping
    
    // State
    virtual bool IsPlaying() const = 0;
    virtual bool IsDownloading() const { return false; }
    
    // Buffer info for FFT visualization
    virtual size_t GetBufferSize() const { return 0; }
    virtual int16_t* GetAudioData() { return nullptr; }
};

#endif // MUSIC_H
