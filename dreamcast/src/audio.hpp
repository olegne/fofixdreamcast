// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

namespace dc {

class Audio {
public:
    ~Audio() { stop(); }
    bool start(const char *backing, const char *guitar = nullptr);
    void poll();
    bool pause();
    bool resume();
    void stop();
    void guitar_gain(float volume);

    int position_ms() const;
    bool playing() const { return active_; }
    bool has_guitar() const { return stems_[1].open; }
    const char *error() const { return error_; }

private:
    static constexpr unsigned block_frames = 2048;
    static constexpr unsigned blocks = 8;
    static constexpr unsigned ring_frames = block_frames * blocks;

    struct Stem {
        OggVorbis_File decoder{};
        int channel[2] = {-1, -1};
        uint32_t address[2] = {};
        unsigned channels = 0;
        unsigned frames = 0;
        unsigned decoded = 0;
        bool open = false;
    };

    bool open(Stem &stem, const char *path);
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
    unsigned rate_ = 0;
    unsigned length_frames_ = 0;
    unsigned played_frames_ = 0;
    unsigned ring_origin_ = 0;
    unsigned last_cursor_ = 0;
    unsigned guitar_volume_ = 255;
    uint64_t last_poll_us_ = 0;
    const char *error_ = nullptr;
};

} // namespace dc
