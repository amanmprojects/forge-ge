#include "forge/audio/audio.hpp"
#include "forge/core/logger.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstring>
#include <cmath>

namespace forge {

static float clampVol(float v) { return v < 0 ? 0 : (v > 2 ? 2 : v); }

struct DecodedSound {
    std::vector<int16_t> pcm; // mono or stereo interleaved s16
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t frames = 0;
};

struct Voice {
    uint32_t id = 0;
    uint32_t soundId = 0;
    double cursor = 0;   // fractional frame position
    double step = 1;     // pitch ratio
    float volume = 1;
    float pan = 0;
    bool loop = false;
    bool active = false;
    bool isMusic = false;
};

// Bus gains, read by the mixer thread. Float stores are atomic in practice;
// API setters write them directly (benign race, no tearing on x86/ARM32+).
static float gMaster = 1.0f, gMusic = 0.8f, gSfx = 1.0f;

struct AudioEngine::Impl {
    ma_device device{};
    bool deviceOpen = false;
    std::unordered_map<uint32_t, DecodedSound> sounds;
    Voice voices[32]{};
    uint32_t nextSoundId = 1;
    uint32_t nextVoiceId = 1;
    uint32_t deviceChannels = 2;
    uint32_t deviceRate = 48000;
    std::mutex mutex;
};

static void dataCallback(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
    (void)in;
    AudioEngine::Impl* im = (AudioEngine::Impl*)dev->pUserData;
    int16_t* dst = (int16_t*)out;
    uint32_t ch = im->deviceChannels;
    std::lock_guard<std::mutex> lock(im->mutex);

    // ducking gains read without lock by API setters (floats, benign races)
    for (ma_uint32 f = 0; f < frames; ++f) {
        float mixL = 0, mixR = 0;
        for (auto& v : im->voices) {
            if (!v.active) continue;
            auto it = im->sounds.find(v.soundId);
            if (it == im->sounds.end()) { v.active = false; continue; }
            const DecodedSound& s = it->second;
            if (s.frames == 0) { v.active = false; continue; }

            uint64_t idx = (uint64_t)v.cursor;
            if (idx >= s.frames) {
                if (v.loop) { v.cursor = 0; idx = 0; }
                else { v.active = false; continue; }
            }
            float sl = 0, sr = 0;
            if (s.channels == 1) {
                sl = sr = s.pcm[idx] / 32768.0f;
            } else {
                sl = s.pcm[idx * s.channels + 0] / 32768.0f;
                sr = s.pcm[idx * s.channels + (s.channels > 1 ? 1 : 0)] / 32768.0f;
            }
            // resample cursor advance (device rate vs sound rate * pitch)
            v.cursor += v.step * (double)s.sampleRate / (double)im->deviceRate;

            float bus = v.isMusic ? gMusic : gSfx;
            float g = v.volume * bus * gMaster;
            float l = g * (v.pan <= 0 ? 1.0f : 1.0f - v.pan);
            float r = g * (v.pan >= 0 ? 1.0f : 1.0f + v.pan);
            mixL += sl * l; mixR += sr * r;
        }
        float l = mixL < -1 ? -1 : (mixL > 1 ? 1 : mixL);
        float r = mixR < -1 ? -1 : (mixR > 1 ? 1 : mixR);
        if (ch == 1) {
            dst[f] = (int16_t)((l + r) * 0.5f * 32767.0f);
        } else {
            dst[f * ch + 0] = (int16_t)(l * 32767.0f);
            dst[f * ch + 1] = (int16_t)(r * 32767.0f);
            for (uint32_t c = 2; c < ch; ++c) dst[f * ch + c] = 0;
        }
    }
}

AudioEngine::AudioEngine() : impl_(new Impl{}) {}
AudioEngine::~AudioEngine() { shutdown(); delete impl_; }

bool AudioEngine::init() {
    if (deviceReady_) return true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.dataCallback = dataCallback;
    cfg.pUserData = impl_;
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        FORGE_LOG_ERROR("Audio: no playback device (running muted)");
        return false;
    }
    impl_->deviceOpen = true;
    impl_->deviceChannels = impl_->device.playback.channels;
    impl_->deviceRate = impl_->device.sampleRate;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        FORGE_LOG_ERROR("Audio: device start failed");
        ma_device_uninit(&impl_->device);
        impl_->deviceOpen = false;
        return false;
    }
    deviceReady_ = true;
    gMaster = masterVol_; gMusic = musicVol_; gSfx = sfxVol_;
    FORGE_LOG_INFO("Audio ready: %u ch @ %u Hz", impl_->deviceChannels, impl_->deviceRate);
    return true;
}

void AudioEngine::shutdown() {
    if (impl_ && impl_->deviceOpen) {
        ma_device_uninit(&impl_->device);
        impl_->deviceOpen = false;
    }
    deviceReady_ = false;
    stopAll();
}

static bool decodeToS16(const void* data, size_t len, DecodedSound& out, bool isFile) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder dec;
    ma_result r = isFile
        ? ma_decoder_init_file((const char*)data, &cfg, &dec)
        : ma_decoder_init_memory(data, len, &cfg, &dec);
    if (r != MA_SUCCESS) return false;
    out.channels = dec.outputChannels;
    out.sampleRate = dec.outputSampleRate;
    ma_uint64 frames = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &frames);
    size_t cap = frames > 0 ? (size_t)frames : 4096;
    out.pcm.reserve(cap * (out.channels ? out.channels : 2));
    int16_t tmp[4096 * 2];
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, tmp, 4096, &got);
        if (got == 0) break;
        size_t base = out.pcm.size();
        out.pcm.resize(base + (size_t)got * out.channels);
        std::memcpy(out.pcm.data() + base, tmp, (size_t)got * out.channels * sizeof(int16_t));
    }
    out.frames = out.pcm.size() / (out.channels ? out.channels : 1);
    ma_decoder_uninit(&dec);
    return out.frames > 0;
}

SoundHandle AudioEngine::loadFile(const std::string& path) {
    DecodedSound s;
    if (!decodeToS16(path.c_str(), 0, s, true)) {
        FORGE_LOG_ERROR("Audio decode failed: %s", path.c_str());
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    uint32_t id = impl_->nextSoundId++;
    impl_->sounds[id] = std::move(s);
    return {id};
}

SoundHandle AudioEngine::loadMemory(const void* data, size_t len) {
    DecodedSound s;
    if (!decodeToS16(data, len, s, false)) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    uint32_t id = impl_->nextSoundId++;
    impl_->sounds[id] = std::move(s);
    return {id};
}

void AudioEngine::unload(SoundHandle h) {
    if (!h.valid()) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sounds.erase(h.id);
    for (auto& v : impl_->voices)
        if (v.soundId == h.id) v.active = false;
}

uint32_t AudioEngine::play(SoundHandle h, float volume, float pan, float pitch, bool loop) {
    if (!h.valid()) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sounds.find(h.id) == impl_->sounds.end()) return 0;
    // steal oldest inactive, else quietest active non-music
    Voice* slot = nullptr;
    for (auto& v : impl_->voices)
        if (!v.active) { slot = &v; break; }
    if (!slot) {
        slot = &impl_->voices[0];
        for (auto& v : impl_->voices)
            if (!v.isMusic && v.volume < slot->volume) slot = &v;
    }
    slot->id = impl_->nextVoiceId++;
    slot->soundId = h.id;
    slot->cursor = 0;
    slot->step = pitch > 0.05 ? pitch : 1.0;
    slot->volume = clampVol(volume);
    slot->pan = pan < -1 ? -1 : (pan > 1 ? 1 : pan);
    slot->loop = loop;
    slot->active = true;
    slot->isMusic = false;
    return slot->id;
}

void AudioEngine::stop(uint32_t voice) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& v : impl_->voices)
        if (v.id == voice) { v.active = false; break; }
}

void AudioEngine::stopAll() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& v : impl_->voices) v.active = false;
    musicVoice_ = 0;
}

void AudioEngine::setVoiceVolume(uint32_t voice, float vol) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& v : impl_->voices)
        if (v.id == voice) { v.volume = clampVol(vol); break; }
}

bool AudioEngine::voicePlaying(uint32_t voice) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& v : impl_->voices)
        if (v.id == voice) return v.active;
    return false;
}

void AudioEngine::setMasterVolume(float v) { masterVol_ = clampVol(v); gMaster = masterVol_; }
void AudioEngine::setMusicVolume(float v) { musicVol_ = clampVol(v); gMusic = musicVol_; }
void AudioEngine::setSfxVolume(float v) { sfxVol_ = clampVol(v); gSfx = sfxVol_; }

void AudioEngine::playMusic(SoundHandle h, float volume) {
    if (musicVoice_) stop(musicVoice_);
    musicVoice_ = play(h, volume, 0, 1.0f, true);
    if (musicVoice_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& v : impl_->voices)
            if (v.id == musicVoice_) { v.isMusic = true; break; }
    }
}

void AudioEngine::stopMusic() {
    if (musicVoice_) { stop(musicVoice_); musicVoice_ = 0; }
}

int AudioEngine::voiceCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    int n = 0;
    for (auto& v : impl_->voices) if (v.active) ++n;
    return n;
}

} // namespace forge
