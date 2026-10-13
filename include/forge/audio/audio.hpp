#pragma once
// Audio: miniaudio-powered mixer. Decodes WAV/MP3/OGG/FLAC from file or
// memory, 32-voice pool, master/music/sfx buses, per-sound volume/pan/pitch.

#include <string>
#include <cstdint>

namespace forge {

struct SoundHandle {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
};

class AudioEngine {
public:
    struct Impl; // defined in audio.cpp (needs to be named by the mixer callback)
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();       // opens default playback device
    void shutdown();
    bool valid() const { return deviceReady_; }

    // ---- decoded sound data (shared, cached by AssetManager) ----
    SoundHandle loadFile(const std::string& path);
    SoundHandle loadMemory(const void* data, size_t len);
    bool reloadFile(SoundHandle h, const std::string& path); // hot-reload
    void unload(SoundHandle h);

    // ---- playback ----
    // Returns voice id (>0) for stop/set control, 0 on failure.
    uint32_t play(SoundHandle h, float volume = 1.0f, float pan = 0.0f,
                  float pitch = 1.0f, bool loop = false);
    void stop(uint32_t voice);
    void stopAll();
    void setVoiceVolume(uint32_t voice, float v);
    bool voicePlaying(uint32_t voice) const;

    // ---- buses ----
    void setMasterVolume(float v);
    void setMusicVolume(float v);
    void setSfxVolume(float v);
    float masterVolume() const { return masterVol_; }

    // Convenience: music channel (single looped stream w/ crossfade-less swap)
    void playMusic(SoundHandle h, float volume = 0.8f);
    void stopMusic();

    int voiceCount() const;

private:
    Impl* impl_ = nullptr;
    bool deviceReady_ = false;
    float masterVol_ = 1.0f, musicVol_ = 0.8f, sfxVol_ = 1.0f;
    uint32_t musicVoice_ = 0;
};

} // namespace forge
