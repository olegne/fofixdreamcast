// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <kos/fs.h>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

namespace dc {

class AudioClip {
public:
    bool load(const char *path);
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
    using ClipVoices = std::array<Audio *, 8>;

    ~Audio() { stop(); }
    bool start(const char *backing, const char *guitar = nullptr, const char *rhythm = nullptr);
    bool start_clip(const AudioClip &clip, unsigned volume = 100);
    bool prepare(const char *backing, const char *guitar = nullptr, const char *rhythm = nullptr,
                 PrepareService service = nullptr, unsigned start_ms = 0);
    bool play_prepared();
    void poll();
    bool pause();
    static void pause_clips(const ClipVoices &voices);
    bool resume();
    void stop();
    void set_mix(unsigned guitar, unsigned song, unsigned rhythm);
    void guitar_gain(float gate, bool picked = false);
    void restore_backing_gain();
    unsigned backing_level() const;
    void fade_out(unsigned duration_ms);

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
    static constexpr unsigned sample_rate = 44100;
    static constexpr unsigned max_source_rate = 48000;
    friend class AudioClip;

    struct Resampler {
        static constexpr unsigned taps = 24;
        static constexpr unsigned left = taps / 2 - 1;
        static constexpr unsigned phases = 128;
        static constexpr unsigned source_capacity =
            ((block_frames - 1) * max_source_rate + sample_rate - 1) / sample_rate + taps;

        Resampler(unsigned source_rate, unsigned frames);

        const unsigned rate;
        const unsigned source_frames;
        const unsigned step;
        const unsigned step_remainder;
        unsigned position = 0;
        unsigned phase = 0;
        unsigned buffered = 0;
        int window_start = -static_cast<int>(left);
        int16_t coefficients[phases + 1][taps];
        int16_t source[source_capacity * 2];
    };

    struct Stem {
        OggVorbis_File decoder{};
        std::FILE *file = nullptr;
        file_t cd_file = FILEHND_INVALID;
        PrepareService service = nullptr;
        char *read_buffer = nullptr;
        size_t file_size = 0;
        size_t file_position = 0;
        size_t read_buffer_start = 0;
        size_t read_buffer_count = 0;
        const AudioClip *clip = nullptr;
        int channel[2] = {-1, -1};
        uint32_t address[2] = {};
        std::unique_ptr<Resampler> resampler;
        unsigned channels = 0;
        unsigned frames = 0;
        unsigned decoded = 0;
        unsigned volume = 255;
        bool open = false;
    };

    bool open(Stem &stem, const char *path, PrepareService service);
    static size_t read_file(Stem &stem, void *dst, size_t size, size_t count);
    static int close_file(Stem &stem);
    static int seek_file(Stem &stem, ogg_int64_t offset, int whence);
    bool allocate_channels(Stem &stem);
    static void close_stem(Stem &stem);
    static bool seek(Stem &stem, unsigned frame);
    bool resample(Stem &stem, unsigned count);
    bool decode(Stem &stem, unsigned count);
    bool fill(Stem &stem, unsigned block);
    void begin_stream(unsigned position);
    void queue_start();
    unsigned stem_volume(unsigned index) const;
    void update_volume();
    bool clip_channels_playing() const;
    bool halt();
    void fail(const char *message);

    Stem stems_[3];
    alignas(32) int16_t decoded_[block_frames * 2];
    alignas(32) int16_t split_[2][block_frames];
    alignas(32) int16_t resume_pcm_[ring_frames];
    bool active_ = false;
    bool paused_ = false;
    bool clock_ready_ = false;
    bool prepared_ = false;
    bool clip_stop_pending_ = false;
    unsigned length_frames_ = 0;
    unsigned played_frames_ = 0;
    unsigned ring_origin_ = 0;
    unsigned last_cursor_ = 0;
    unsigned guitar_level_ = 100;
    unsigned song_level_ = 100;
    unsigned rhythm_level_ = 100;
    unsigned clip_gain_ = 128;
    float guitar_gate_ = 1.0f;
    bool guitar_controls_backing_ = false;
    unsigned fade_volume_ = 255;
    unsigned fade_begin_volume_ = 255;
    unsigned fade_duration_ms_ = 0;
    uint64_t fade_begin_ms_ = 0;
    uint64_t last_poll_us_ = 0;
    const char *error_ = nullptr;
};

class ResultAudio {
public:
    void schedule(const char *path);
    void poll(bool results_visible);
    void set_volume(unsigned guitar);
    void pause();
    void resume();
    bool playing() const { return audio_ && audio_->playing(); }

private:
    std::unique_ptr<Audio> audio_;
    const char *pending_ = nullptr;
    uint64_t scheduled_ms_ = 0;
    unsigned volume_ = 100;
};

} // namespace dc
