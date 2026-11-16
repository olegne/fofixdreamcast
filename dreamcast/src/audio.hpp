// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

namespace dc {

class Audio {
public:
    ~Audio() { stop(); }
    bool start(const char *path);
    void poll();
    void stop();

    int position_ms() const;
    bool playing() const { return active_; }
    const char *error() const { return error_; }

private:
    static constexpr unsigned block_frames = 2048;
    static constexpr unsigned blocks = 8;
    static constexpr unsigned ring_frames = block_frames * blocks;

    bool fill(unsigned block);
    void queue_start();
    void fail(const char *message);

    OggVorbis_File decoder_{};
    alignas(32) int16_t decoded_[block_frames * 2];
    alignas(32) int16_t split_[2][block_frames];
    int channel_[2] = {-1, -1};
    uint32_t address_[2] = {};
    bool opened_ = false;
    bool active_ = false;
    bool clock_ready_ = false;
    unsigned channels_ = 0;
    unsigned rate_ = 0;
    unsigned length_frames_ = 0;
    unsigned decoded_frames_ = 0;
    unsigned played_frames_ = 0;
    unsigned last_cursor_ = 0;
    uint64_t last_poll_us_ = 0;
    const char *error_ = nullptr;
};

} // namespace dc
