// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

namespace dc {

class AudioClip {
public:
    bool load(const char *path, unsigned volume);
    bool loaded() const { return bool(pcm_); }

private:
    friend class Audio;
    std::unique_ptr<int16_t, decltype(&std::free)> pcm_{nullptr, &std::free};
    unsigned frames_ = 0;
    unsigned stride_ = 0;
};

class Audio {
public:
    using PrepareService = void (*)();

    ~Audio() { stop(); }
    bool start(const char *backing, const char *guitar = nullptr);
    bool start_clip(const AudioClip &clip);
    bool prepare(const char *backing, const char *guitar = nullptr, PrepareService service = nullptr);
    bool play_prepared();
    void poll();
    bool pause();
    bool resume();
    void stop();
    void guitar_gain(float volume);

    int position_ms() const;
    uint64_t position_sampled_at_us() const { return last_poll_us_; }
    bool playing() const { return active_; }
    bool paused() const { return paused_; }
    bool has_guitar() const { return stems_[1].open; }
    const char *error() const { return error_; }

private:
    static constexpr unsigned block_frames = 2048;
    static constexpr unsigned blocks = 8;
    static constexpr unsigned ring_frames = block_frames * blocks;
    friend class AudioClip;

    struct Stem {
        OggVorbis_File decoder{};
        std::FILE *file = nullptr;
        PrepareService service = nullptr;
        char *read_buffer = nullptr;
        const AudioClip *clip = nullptr;
        int channel[2] = {-1, -1};
        uint32_t address[2] = {};
        unsigned channels = 0;
        unsigned frames = 0;
        unsigned decoded = 0;
        bool open = false;
    };

    bool open(Stem &stem, const char *path, PrepareService service);
    bool allocate_channels(Stem &stem);
    bool decode(Stem &stem, unsigned count);
    bool fill(Stem &stem, unsigned block);
    void begin_stream(unsigned position);
    void queue_start();
    bool halt();
    void fail(const char *message);

    Stem stems_[2];
    alignas(32) int16_t decoded_[block_frames * 2];
    alignas(32) int16_t split_[2][block_frames];
    alignas(32) int16_t resume_pcm_[ring_frames];
    bool active_ = false;
    bool paused_ = false;
    bool clock_ready_ = false;
    bool prepared_ = false;
    unsigned rate_ = 0;
    unsigned length_frames_ = 0;
    unsigned played_frames_ = 0;
    unsigned ring_origin_ = 0;
    unsigned last_cursor_ = 0;
    unsigned guitar_volume_ = 255;
    uint64_t last_poll_us_ = 0;
    const char *error_ = nullptr;
};

class ResultAudio {
public:
    void schedule(const char *path);
    void poll(bool results_visible);
    void pause();
    void resume();
    bool playing() const { return audio_ && audio_->playing(); }

private:
    std::unique_ptr<Audio> audio_;
    const char *pending_ = nullptr;
    uint64_t scheduled_ms_ = 0;
};

} // namespace dc
