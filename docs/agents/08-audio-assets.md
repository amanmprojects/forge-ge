# 08 — Audio and Assets

Headers: `forge/audio/audio.hpp`, `forge/assets/assets.hpp`.

## AudioEngine

miniaudio-backed mixer: file/memory decode (WAV, MP3, OGG, FLAC), a 32-voice
pool, master/music/sfx buses, per-voice volume/pan/pitch/loop, and voice
stealing. Owned by `Engine`.

```cpp
struct SoundHandle { uint32_t id = 0; bool valid() const; };  // value type, copy freely

class AudioEngine {
    bool init();       // opens the default playback device; soft-fails
    void shutdown();   bool valid() const;      // deviceReady_

    // Decode (fully, to s16 PCM) — shared, cached by AssetManager
    SoundHandle loadFile(const std::string& path);
    SoundHandle loadMemory(const void* data, size_t len);   // encoded file bytes
    bool reloadFile(SoundHandle, const std::string& path);  // hot-reload in place
    void unload(SoundHandle);

    // Playback
    uint32_t play(SoundHandle, float volume = 1.0f, float pan = 0.0f,
                  float pitch = 1.0f, bool loop = false);   // 0 on failure
    void stop(uint32_t voice);
    void stopAll();
    void setVoiceVolume(uint32_t voice, float v);
    bool voicePlaying(uint32_t voice) const;

    // Buses (volume clamped 0..2)
    void setMasterVolume(float);  void setMusicVolume(float);  void setSfxVolume(float);
    float masterVolume() const;

    // Music convenience: one looped voice, replaced on each call
    void playMusic(SoundHandle, float volume = 0.8f);
    void stopMusic();

    int voiceCount() const;
};
```

Semantics that save debugging time:

- **Init is soft failure.** No audio device → `valid() == false`, everything
  else no-ops. Games must run muted. Never gate logic on a sound playing.
- **Handles are ids, not object lifetimes**: `unload` invalidates playback of
  that sound (active voices stop).
- **Voice stealing**: `play` prefers a free slot; if all 32 are busy it steals
  the *oldest non-music* voice. **Music voices are never stolen**, so 32
  simultaneous music tracks could starve SFX — realistically never; but if
  `playMusic` returns a busy mix, that's the rule.
- **Pitch** is a playback-rate ratio (`1.0` = original); values `<= 0.05`
  fall back to 1.0 (no silent/reversed voices). Useful range ~0.5–2.0.
- **Pan**: -1 left … +1 right (equal-power-ish linear split).
- **Volume**: clamped to 0..2 per voice; bus gains multiply on top.
- Mixing is clamped to [-1, 1] to avoid int16 wraparound.

### In-memory sounds (no asset files)

`loadMemory` expects **encoded file bytes**, not raw PCM — the same path a
decoded asset takes. Synthesizing a WAV header + PCM in memory is the
zero-asset pattern used by `04_audio.cpp` and `08_snake.cpp`:

```cpp
// Compact chiptune example: notes -> 16-bit mono WAV bytes -> SoundHandle.
struct Note { float f0, f1, dur, vol; int wave; float noise; };  // wave: 0 sine 1 square 2 tri

std::vector<uint8_t> synthWav(const std::vector<Note>& notes, int rate = 22050) {
    std::vector<int16_t> pcm;
    float phase = 0.0f;
    forge::Random rng{0x51EED5};
    for (const Note& n : notes) {
        int frames = std::max(1, (int)(n.dur * rate));
        for (int i = 0; i < frames; ++i) {
            float u = float(i) / float(frames);
            float f = forge::lerp(n.f0, n.f1, u);
            phase += f / float(rate);
            float s = (n.wave == 1) ? (std::sin(forge::TAU * phase) >= 0 ? 1.0f : -1.0f)
                    : (n.wave == 2) ? std::asin(std::sin(forge::TAU * phase)) * (2.0f / forge::PI)
                                    : std::sin(forge::TAU * phase);
            s = s * (1 - n.noise) + (rng.nextFloat() * 2 - 1) * n.noise;
            float env = std::min(1.0f, float(i) / (0.0025f * rate)) * std::exp(-3.2f * u);
            pcm.push_back((int16_t)(forge::clamp(s * env * n.vol, -1.0f, 1.0f) * 32767.0f));
        }
    }
    const int bytes = (int)pcm.size() * 2;
    std::vector<uint8_t> wav(size_t(44 + bytes));
    auto w32 = [&](int o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
    auto w16 = [&](int o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);          w32(4, 36 + bytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1);      // PCM, mono
    w32(24, rate); w32(28, rate * 2); w16(32, 2); w16(34, 16);
    std::memcpy(&wav[36], "data", 4);         w32(40, bytes);
    std::memcpy(&wav[44], pcm.data(), bytes);
    return wav;
}

// usage
auto blip = synthWav({{880, 1760, 0.08f, 0.5f, 1, 0.0f}});
forge::SoundHandle h = engine.audio().loadMemory(blip.data(), blip.size());
engine.audio().play(h, 0.9f, 0.0f, 1.0f);      // volume, pan, pitch
```

Generate each SFX once at startup (decoding/resynthesis is not cheap per
hit). For pitch variation without resynthesis, pass a random `pitch` in
0.9..1.1 to `play`.

## AssetManager

A path-keyed cache with mtime-based hot reload. Owned by `Engine`
(`engine.assets()`, audio wired automatically).

```cpp
explicit AssetManager(AudioEngine* audio = nullptr);

Texture2D* loadTexture(const std::string& path, TextureFilter = Linear);  // cached
Font*      loadFont(const std::string& path, float pxHeight = 32.0f);     // cached per (path, px)
SoundHandle loadSound(const std::string& path);                            // cached
Shader*    loadShader(const std::string& vertPath, const std::string& fragPath); // cached

void addSearchPath(const std::string& dir);
void clearSearchPaths();
const std::vector<std::string>& searchPaths() const;
std::string resolve(const std::string& path) const;  // first existing candidate

void unloadTexture(const std::string& path);
void clear();                         // unload everything
int  hotReload();                     // reload assets whose mtime changed; returns count
void setHotReload(bool on);
void update();                        // throttled to ~2 Hz; called by run* loops
size_t textureCount() const;
```

- **Pointers are stable** (heap nodes) until `unloadTexture`/`clear`: textures
  and fonts are mutated in place on hot reload, so held pointers stay valid —
  convenient for game objects that cache `const Texture2D*`.
- `loadTexture` failure returns `nullptr` (logged); `loadFont` likewise;
  `loadSound` returns an invalid handle when there's no audio engine or the
  decode fails.
- **Search paths**: `addSearchPath("assets")` makes every relative load try
  `assets/<path>` as well as `<path>` (in order, first existing file wins).
  Absolute paths bypass the list. This is what lets the same binary run from
  the repo root, from `build/`, or from an installed bundle. The cache key
  stays the *logical* path, so `loadTexture("hero.png")` is the same entry no
  matter which directory resolved it.
- Hot reload covers textures, fonts **and sounds** (sounds re-decode into the
  same handle; playing voices keep their cursor).
- **Shader hot reload**: `loadShader(vs, fs)` caches by the pair of paths and
  reloads when either file's mtime changes. A failed recompile is logged and
  the previous program keeps running — a typo never blanks a live game. This
  is folded into the same 2 Hz `update()`.
- `update()` stats files at most twice per second — cheap enough to leave
  enabled in shipping if you want live-reload; disable for zero syscalls:
  `engine.assets().setHotReload(false)`.
- Retry behavior: a failed load is not cached, so the next `loadTexture` call
  tries again (nice while an artist is saving files).

```cpp
forge::Texture2D* hero = engine.assets().loadTexture("assets/hero.png");
if (!hero) hero = &forge::Texture2D::white();     // degrade gracefully

forge::Font* hud = engine.assets().loadFont("assets/ui.ttf", 24.0f);
if (hud) hud->draw(engine.batch(), "READY", {16, 40}, 1.0f, forge::Color::white());
```

Hot reload assumes assets live on a real filesystem; it is a dev convenience,
not part of the deterministic simulation (don't depend on it in tests).
