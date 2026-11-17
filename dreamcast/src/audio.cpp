// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <arch/timer.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <kos/thread.h>
#include <malloc.h>

namespace dc {
namespace {

constexpr unsigned cd_read_buffer_size = 16 * 1024;
constexpr unsigned resample_bits = 14;
constexpr int resample_unit = 1 << resample_bits;

bool read_pcm(OggVorbis_File &decoder, int16_t *samples, unsigned bytes, bool last_read) {
    auto *output = reinterpret_cast<char *>(samples);
    unsigned offset = 0;
    while (offset < bytes) {
        int section = 0;
        const auto got = ov_read(&decoder, output + offset, bytes - offset, 0, 2, 1, &section);
        if (got == 0 && last_read) {
            // A clean EOF can precede the declared final granule by a few samples.
            std::memset(output + offset, 0, bytes - offset);
            return true;
        }
        if (got <= 0) {
            return false;
        }
        offset += static_cast<unsigned>(got);
    }
    return true;
}

struct AicaCommand {
    alignas(32) uint32_t packet[AICA_CMDSTR_CHANNEL_SIZE]{};

    aica_cmd_t *command() { return reinterpret_cast<aica_cmd_t *>(packet); }
    aica_channel_t *voice() { return reinterpret_cast<aica_channel_t *>(command()->cmd_data); }

    void send(uint64_t id, uint32_t flags) {
        command()->cmd = AICA_CMD_CHAN;
        command()->size = AICA_CMDSTR_CHANNEL_SIZE;
        command()->cmd_id = id;
        voice()->cmd = flags;
        snd_sh4_to_aica(packet, command()->size);
    }
};

} // namespace

bool AudioClip::load(const char *path) {
    auto *file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    std::unique_ptr<OggVorbis_File> decoder(new (std::nothrow) OggVorbis_File{});
    if (!decoder || ov_open(file, decoder.get(), nullptr, 0) != 0) {
        std::fclose(file);
        return false;
    }
    const auto *info = ov_info(decoder.get(), 0);
    const auto frames = ov_pcm_total(decoder.get(), 0);
    if (ov_streams(decoder.get()) != 1 || !info || info->channels != 2 || info->rate != Audio::sample_rate ||
        frames <= 0 || frames >= std::numeric_limits<unsigned>::max() / 4 - Audio::block_frames) {
        ov_clear(decoder.get());
        return false;
    }
    const auto stride = (unsigned(frames) + Audio::block_frames - 1) & ~(Audio::block_frames - 1);
    decltype(pcm_) pcm(static_cast<int16_t *>(memalign(32, stride * 2 * sizeof(int16_t))), &std::free);
    if (!pcm) {
        ov_clear(decoder.get());
        return false;
    }
    int16_t interleaved[Audio::block_frames * 2];
    auto *left = pcm.get();
    auto *right = left + stride;
    for (unsigned offset = 0; offset < frames; offset += Audio::block_frames) {
        const auto count = std::min(Audio::block_frames, unsigned(frames) - offset);
        if (!read_pcm(*decoder, interleaved, count * 2 * sizeof(int16_t), offset + count == frames)) {
            ov_clear(decoder.get());
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            left[offset + i] = interleaved[2 * i];
            right[offset + i] = interleaved[2 * i + 1];
        }
    }
    std::fill(left + frames, left + stride, 0);
    std::fill(right + frames, right + stride, 0);
    ov_clear(decoder.get());
    pcm_ = std::move(pcm);
    stride_ = stride;
    frames_ = unsigned(frames);
    return true;
}

bool Audio::start(const char *backing, const char *guitar, const char *rhythm) {
    return prepare(backing, guitar, rhythm) && play_prepared();
}

bool Audio::start_clip(const AudioClip &clip, unsigned volume) {
    stop();
    if (!clip.pcm_) {
        error_ = "Audio effect not loaded.";
        return false;
    }
    if (snd_init() < 0) {
        error_ = "AICA initialization failed.";
        return false;
    }
    error_ = nullptr;
    clip_gain_ = std::min(volume, 100u) * 128 / 100;
    auto &stem = stems_[0];
    stem.clip = &clip;
    stem.channels = 2;
    stem.frames = length_frames_ = clip.frames_;
    if (!allocate_channels(stem)) {
        stop();
        return false;
    }
    for (unsigned block = 0; block < blocks; ++block) {
        fill(stem, block);
    }
    begin_stream(0);
    return true;
}

bool Audio::prepare(const char *backing, const char *guitar, const char *rhythm, PrepareService service,
                    unsigned start_ms) {
    stop();
    error_ = nullptr;
    played_frames_ = 0;
    guitar_gate_ = 1.0f;
    guitar_controls_backing_ = false;
    if (snd_init() < 0) {
        fail("AICA initialization failed.");
        return false;
    }
    if (!open(stems_[0], backing, service)) {
        stop();
        return false;
    }
    length_frames_ = stems_[0].frames;
    played_frames_ = unsigned(std::min(uint64_t(length_frames_ - 1), uint64_t(start_ms) * sample_rate / 1000));
    const auto prefill = [&](Stem &stem) {
        if (!seek(stem, played_frames_)) {
            error_ = "Could not seek in the audio track.";
            return false;
        }
        for (unsigned block = 0; block < blocks; ++block) {
            if (service) {
                service();
            }
            if (!fill(stem, block)) {
                error_ = "Error decoding the audio track.";
                return false;
            }
        }
        stem.service = nullptr;
        return true;
    };
    if (!prefill(stems_[0])) {
        stop();
        return false;
    }
    // Song.py continues with the accompaniment if a companion fails to load.
    const char *companions[] = {guitar, rhythm};
    const char *names[] = {"guitar", "rhythm"};
    for (unsigned i = 0; i < 2; ++i) {
        auto &stem = stems_[i + 1];
        if (companions[i] && (!open(stem, companions[i], service) || !prefill(stem))) {
            std::printf("FoFDC: %s skipped: %s\n", names[i], error_);
            close_stem(stem);
            error_ = nullptr;
        }
    }
    prepared_ = true;
    return true;
}

bool Audio::play_prepared() {
    if (!prepared_) {
        error_ = "Audio was not prepared.";
        return false;
    }
    prepared_ = false;
    begin_stream(played_frames_);
    std::printf("FoFDC: audio %u Hz, backing=%u ch guitar=%u ch samples=%u",
                sample_rate, stems_[0].channels, stems_[1].channels, length_frames_);
    if (stems_[2].open) {
        std::printf(" rhythm=%u ch", stems_[2].channels);
    }
    std::puts("");
    return true;
}

bool Audio::open(Stem &stem, const char *path, PrepareService service) {
    if (service) {
        service();
    }
    auto *file = std::fopen(path, "rb");
    if (!file) {
        error_ = "Could not open an audio track.";
        return false;
    }
    if (std::strncmp(path, "/cd/", 4) == 0) {
        // KOS can DMA whole ISO9660 sectors into a 32-byte-aligned buffer.
        auto *buffer = static_cast<char *>(memalign(32, cd_read_buffer_size));
        if (buffer && std::setvbuf(file, buffer, _IOFBF, cd_read_buffer_size) == 0) {
            stem.read_buffer = buffer;
        } else {
            std::free(buffer);
        }
    }
    stem.file = file;
    stem.service = service;
    // A result voice can still be playing when Replay loads two stems. Let
    // it refill its own AICA ring between the file operations during prefill.
    constexpr ov_callbacks serviced_file{
        [](void *dst, size_t size, size_t count, void *user) {
            const auto &input = *static_cast<Stem *>(user);
            if (input.service) {
                input.service();
            }
            return std::fread(dst, size, count, input.file);
        },
        [](void *user, ogg_int64_t offset, int whence) {
            const auto &input = *static_cast<Stem *>(user);
            if (input.service) {
                input.service();
            }
            return std::fseek(input.file, offset, whence);
        },
        [](void *user) { return std::fclose(static_cast<Stem *>(user)->file); },
        [](void *user) { return std::ftell(static_cast<Stem *>(user)->file); }
    };
    const int opened = service ? ov_open_callbacks(&stem, &stem.decoder, nullptr, 0, serviced_file) :
                                 ov_open(file, &stem.decoder, nullptr, 0);
    if (opened != 0) {
        std::fclose(file);
        error_ = "Invalid Ogg Vorbis audio track.";
        return false;
    }
    stem.open = true;
    const auto *info = ov_info(&stem.decoder, 0);
    const auto frames = ov_pcm_total(&stem.decoder, 0);
    if (ov_streams(&stem.decoder) != 1 || !info || info->channels < 1 || info->channels > 2 ||
        info->rate < 8000 || info->rate > long(max_source_rate) || frames <= 0 || frames > info->rate * 3600) {
        error_ = "Unsupported Ogg Vorbis track.";
        return false;
    }
    const auto rate = static_cast<unsigned>(info->rate);
    if (rate != sample_rate) {
        stem.resampler.reset(new (std::nothrow) Resampler(rate, unsigned(frames)));
        if (!stem.resampler) {
            error_ = "Not enough memory to resample the audio track.";
            return false;
        }
    }
    stem.channels = static_cast<unsigned>(info->channels);
    stem.frames = static_cast<unsigned>((frames * sample_rate + rate - 1) / rate);
    stem.decoded = 0;
    return allocate_channels(stem);
}

bool Audio::allocate_channels(Stem &stem) {
    for (unsigned i = 0; i < stem.channels; ++i) {
        stem.channel[i] = snd_sfx_chn_alloc();
        stem.address[i] = snd_mem_malloc(ring_frames * sizeof(int16_t));
        if (stem.channel[i] < 0 || !stem.address[i]) {
            error_ = "Not enough AICA memory.";
            return false;
        }
    }
    return true;
}

Audio::Resampler::Resampler(unsigned source_rate, unsigned frames)
    : rate(source_rate), source_frames(frames),
      step(rate / sample_rate), step_remainder(rate % sample_rate) {
    constexpr double pi = 3.14159265358979323846;
    const double cutoff = std::min(1.0, double(sample_rate) / rate);
    for (unsigned phase_no = 0; phase_no <= phases; ++phase_no) {
        const double fraction = double(phase_no) / phases;
        double weights[taps];
        double sum = 0.0;
        for (unsigned i = 0; i < taps; ++i) {
            const double time = double(i) - left - fraction;
            const double angle = pi * time / (taps / 2);
            const double sinc = time == 0.0 ? cutoff : std::sin(pi * cutoff * time) / (pi * time);
            weights[i] = sinc * (0.42 + 0.5 * std::cos(angle) + 0.08 * std::cos(2 * angle));
            sum += weights[i];
        }
        int quantized_sum = 0;
        for (unsigned i = 0; i < taps; ++i) {
            coefficients[phase_no][i] = static_cast<int16_t>(std::lround(weights[i] * resample_unit / sum));
            quantized_sum += coefficients[phase_no][i];
        }
        coefficients[phase_no][left + (phase_no > phases / 2)] += resample_unit - quantized_sum;
    }
}

bool Audio::seek(Stem &stem, unsigned frame) {
    stem.decoded = std::min(frame, stem.frames);
    if (stem.decoded >= stem.frames) {
        return true;
    }
    if (stem.resampler) {
        auto &resampler = *stem.resampler;
        const auto scaled = uint64_t(frame) * resampler.rate;
        resampler.position = static_cast<unsigned>(scaled / sample_rate);
        resampler.phase = static_cast<unsigned>(scaled % sample_rate);
        resampler.window_start = int(resampler.position) - int(Resampler::left);
        resampler.buffered = 0;
        // Retain the filter's source history when starting mid-track.
        const int first = std::max(0, resampler.window_start);
        return first == 0 || ov_pcm_seek(&stem.decoder, first) == 0;
    }
    return frame == 0 || ov_pcm_seek(&stem.decoder, frame) == 0;
}

bool Audio::resample(Stem &stem, unsigned count) {
    auto &resampler = *stem.resampler;
    const auto channels = stem.channels;
    const int first = int(resampler.position) - int(Resampler::left);
    const auto source_span = (resampler.phase + (count - 1) * resampler.rate) / sample_rate + Resampler::taps;
    const auto discard = unsigned(first - resampler.window_start);
    auto buffered = std::min(source_span, resampler.buffered - std::min(discard, resampler.buffered));
    if (buffered && discard) {
        std::memmove(resampler.source, resampler.source + discard * channels,
                     buffered * channels * sizeof(int16_t));
    }
    const auto leading = std::min(source_span - buffered, unsigned(std::max(0, -first - int(buffered))));
    std::fill_n(resampler.source + buffered * channels, leading * channels, 0);
    buffered += leading;
    const auto source_position = unsigned(first + int(buffered));
    const auto read = std::min(source_span - buffered,
                               resampler.source_frames - std::min(source_position, resampler.source_frames));
    if (!read_pcm(stem.decoder, resampler.source + buffered * channels, read * channels * sizeof(int16_t),
                  source_position + read >= resampler.source_frames)) {
        return false;
    }
    buffered += read;
    std::fill(resampler.source + buffered * channels, resampler.source + source_span * channels, 0);
    resampler.window_start = first;
    resampler.buffered = source_span;

    unsigned offset = 0;
    unsigned phase = resampler.phase;
    // All AICA voices advance at 44.1 kHz. The integer phase advances by the
    // exact source/output ratio and is continuous through block refills.
    for (unsigned i = 0; i < count; ++i) {
        const auto phase_no = (phase * Resampler::phases + sample_rate / 2) / sample_rate;
        const auto *filter = resampler.coefficients[phase_no];
        const auto *source = resampler.source + offset * channels;
        for (unsigned channel = 0; channel < channels; ++channel) {
            int sum = 0;
            for (unsigned tap = 0; tap < Resampler::taps; ++tap) {
                sum += source[tap * channels + channel] * filter[tap];
            }
            decoded_[i * channels + channel] =
                int16_t(std::clamp((sum + resample_unit / 2) >> resample_bits, -32768, 32767));
        }
        offset += resampler.step;
        phase += resampler.step_remainder;
        if (phase >= sample_rate) {
            phase -= sample_rate;
            ++offset;
        }
    }
    resampler.position += offset;
    resampler.phase = phase;
    return true;
}

bool Audio::decode(Stem &stem, unsigned count) {
    auto frames = std::min(count, stem.frames - stem.decoded);
    if (frames) {
        const bool decoded = stem.resampler ? resample(stem, frames) :
                             read_pcm(stem.decoder, decoded_, frames * stem.channels * sizeof(int16_t),
                                      stem.decoded + frames == stem.frames);
        if (!decoded) {
            if (&stem == &stems_[0] || (!active_ && !paused_)) {
                return false;
            }
            std::printf("FoFDC: %s muted after an audio decoding error.\n", &stem == &stems_[1] ? "guitar" : "rhythm");
            stem.decoded = stem.frames;
            frames = 0;
        }
    }
    stem.decoded += frames;
    std::memset(decoded_ + frames * stem.channels, 0, (count - frames) * stem.channels * sizeof(int16_t));
    return true;
}

bool Audio::fill(Stem &stem, unsigned block) {
    const auto block_bytes = block_frames * sizeof(int16_t);
    if (stem.clip) {
        const auto *clip = stem.clip;
        const unsigned frames = std::min(block_frames, clip->frames_ - stem.decoded);
        for (unsigned channel = 0; channel < stem.channels; ++channel) {
            auto *samples = clip->pcm_.get() + channel * clip->stride_ + stem.decoded;
            // Resuming an effect can start the ring at a sample not aligned for store queues.
            if (clip_gain_ != 128 || frames != block_frames || ((stem.decoded * sizeof(int16_t)) & 31)) {
                if (clip_gain_ == 128) {
                    std::copy_n(samples, frames, split_[channel]);
                } else {
                    const auto gain = int(clip_gain_);
                    for (unsigned i = 0; i < frames; ++i) {
                        split_[channel][i] = samples[i] * gain / 128;
                    }
                }
                std::fill(split_[channel] + frames, split_[channel] + block_frames, 0);
                samples = split_[channel];
            }
            spu_memload_sq(stem.address[channel] + block * block_bytes, samples, block_bytes);
        }
        stem.decoded += frames;
        return true;
    }
    if (!decode(stem, block_frames)) {
        return false;
    }
    if (stem.channels == 1) {
        spu_memload_sq(stem.address[0] + block * block_bytes, decoded_, block_bytes);
    } else {
        for (unsigned i = 0; i < block_frames; ++i) {
            split_[0][i] = decoded_[2 * i];
            split_[1][i] = decoded_[2 * i + 1];
        }
        for (unsigned i = 0; i < stem.channels; ++i) {
            spu_memload_sq(stem.address[i] + block * block_bytes, split_[i], block_bytes);
        }
    }
    return true;
}

void Audio::begin_stream(unsigned position) {
    ring_origin_ = played_frames_ = position;
    last_cursor_ = 0;
    clock_ready_ = false;
    queue_start();
    active_ = true;
    paused_ = false;
    last_poll_us_ = timer_us_gettime64();
}

void Audio::queue_start() {
    AicaCommand cmd;
    auto *voice = cmd.voice();
    voice->type = AICA_SM_16BIT;
    voice->length = ring_frames;
    voice->loop = 1;
    voice->loopend = ring_frames;
    voice->freq = sample_rate;
    uint64_t mask = 0;

    snd_sh4_to_aica_stop();
    for (unsigned index = 0; index < 3; ++index) {
        auto &stem = stems_[index];
        stem.volume = stem_volume(index);
        for (unsigned i = 0; i < stem.channels; ++i) {
            voice->base = stem.address[i];
            voice->pan = stem.channels == 1 ? 128 : (i == 0 ? 0 : 255);
            voice->vol = stem.volume;
            cmd.send(stem.channel[i], AICA_CH_CMD_START | AICA_CH_START_DELAY);
            mask |= uint64_t{1} << stem.channel[i];
        }
    }
    cmd.send(mask, AICA_CH_CMD_START | AICA_CH_START_SYNC);
    snd_sh4_to_aica_start();
}

void Audio::poll() {
    if (!active_) {
        return;
    }
    if (clip_stop_pending_) {
        if (!clip_channels_playing()) {
            active_ = false;
            stop();
        }
        return;
    }
    const auto now = timer_us_gettime64();
    constexpr auto safe_poll_interval = uint64_t(ring_frames - block_frames) * 1000000 / sample_rate;
    if (now - last_poll_us_ >= safe_poll_interval) {
        fail("The audio buffer lost synchronization.");
        return;
    }
    const auto primary_channel = stems_[0].channel[0];
    if (!clock_ready_) {
        if (!snd_is_playing(primary_channel)) {
            return;
        }
        clock_ready_ = true;
    }

    // The ARM publishes the actual playback position of the backing voice.
    const auto cursor = unsigned(snd_get_pos(primary_channel)) & (ring_frames - 1);
    const auto advance = (cursor - last_cursor_) & (ring_frames - 1);
    if (advance >= ring_frames - block_frames) {
        fail("Invalid AICA playback position.");
        return;
    }
    const auto old_block = (played_frames_ - ring_origin_) / block_frames;
    played_frames_ += advance;
    last_cursor_ = cursor;
    last_poll_us_ = now;
    if (played_frames_ >= length_frames_) {
        played_frames_ = length_frames_;
        stop();
        return;
    }
    for (auto block = old_block; block < (played_frames_ - ring_origin_) / block_frames; ++block) {
        for (auto &stem : stems_) {
            if (stem.channels && !fill(stem, block % blocks)) {
                fail("Error decoding the audio track.");
                return;
            }
        }
    }
    if (fade_duration_ms_) {
        const auto elapsed_ms = now / 1000 - fade_begin_ms_;
        if (elapsed_ms >= fade_duration_ms_) {
            stop();
        } else {
            fade_volume_ = fade_begin_volume_ * (fade_duration_ms_ - elapsed_ms) / fade_duration_ms_;
            update_volume();
        }
    }
}

bool Audio::clip_channels_playing() const {
    const auto &clip = stems_[0];
    return snd_is_playing(clip.channel[0]) || snd_is_playing(clip.channel[1]);
}

void Audio::pause_clips(const ClipVoices &voices) {
    struct Latch {
        Audio *voice = nullptr;
        uint64_t published_at = 0;
        bool tracking = false;
    };
    std::array<Latch, std::tuple_size_v<ClipVoices>> latches{};
    const auto capture_cursor = [](Latch &latch, bool measure_time) {
        auto &voice = *latch.voice;
        const auto cursor = unsigned(snd_get_pos(voice.stems_[0].channel[0])) & (ring_frames - 1);
        const auto advance = (cursor - voice.last_cursor_) & (ring_frames - 1);
        if (cursor && advance && advance < ring_frames - block_frames) {
            if (measure_time) {
                latch.published_at = timer_us_gettime64();
            }
            voice.last_cursor_ = cursor;
            voice.played_frames_ = std::min(voice.length_frames_, voice.played_frames_ + advance);
        }
    };
    unsigned count = 0;
    for (auto *voice : voices) {
        if (!voice || !voice->active_ || !voice->stems_[0].clip || voice->clip_stop_pending_) {
            continue;
        }
        voice->poll();
        if (voice->active_) {
            auto &latch = latches[count++];
            latch.voice = voice;
            latch.tracking = voice->clock_ready_;
            if (latch.tracking) {
                capture_cursor(latch, false);
            }
        }
    }
    if (!count) {
        return;
    }

    // The ARM samples all channels before it drains the command queue.
    // Establish a fresh sample timestamp with a single window for the batch.
    const auto latch_deadline = timer_us_gettime64() + 5200;
    while (timer_us_gettime64() < latch_deadline) {
        bool waiting = false;
        for (unsigned i = 0; i < count; ++i) {
            auto &latch = latches[i];
            if (latch.tracking) {
                capture_cursor(latch, true);
                waiting |= !latch.published_at;
            }
        }
        if (!waiting) {
            break;
        }
        timer_spin_delay_us(16);
    }

    AicaCommand cmd;
    snd_sh4_to_aica_stop();
    for (unsigned i = 0; i < count; ++i) {
        for (const auto channel : latches[i].voice->stems_[0].channel) {
            cmd.send(channel, AICA_CH_CMD_STOP);
        }
    }
    snd_sh4_to_aica_start();

    const auto stop_started = timer_us_gettime64();
    for (;;) {
        bool waiting = false;
        bool tracking = false;
        const bool precise = timer_us_gettime64() - stop_started < 12000;
        for (unsigned i = 0; i < count; ++i) {
            auto &latch = latches[i];
            if (!latch.voice) {
                continue;
            }
            auto &voice = *latch.voice;
            const auto &channels = voice.stems_[0].channel;
            const bool playing = snd_is_playing(channels[0]);
            if (precise && latch.tracking) {
                if (playing) {
                    capture_cursor(latch, true);
                    tracking = true;
                } else {
                    const auto since_publish = timer_us_gettime64() - latch.published_at;
                    if (latch.published_at && since_publish < 4000) {
                        const auto advance = unsigned(since_publish * sample_rate / 1000000);
                        voice.played_frames_ = std::min(voice.length_frames_, voice.played_frames_ + advance);
                    }
                    latch.tracking = false;
                }
            }
            if (playing || snd_is_playing(channels[1])) {
                waiting = true;
            } else {
                voice.active_ = false;
                voice.paused_ = true;
                latch.voice = nullptr;
            }
        }
        if (!waiting) {
            return;
        }
        if (timer_us_gettime64() - stop_started >= 50000) {
            // Keep ownership of unacknowledged channels. poll() releases them
            // once the pending KEYOFF takes effect.
            for (auto &latch : latches) {
                if (latch.voice) {
                    latch.voice->clip_stop_pending_ = true;
                    latch.voice->error_ = "The AICA did not stop.";
                }
            }
            std::puts("FoFiXDC: The AICA did not stop all effects.");
            return;
        }
        if (tracking) {
            timer_spin_delay_us(16);
        } else {
            thd_sleep(1);
        }
    }
}

bool Audio::halt() {
    if (!active_) {
        return true;
    }
    AicaCommand cmd;
    for (const auto &stem : stems_) {
        for (unsigned i = 0; i < stem.channels; ++i) {
            cmd.send(stem.channel[i], AICA_CH_CMD_STOP);
        }
    }
    active_ = false;
    const auto deadline = timer_us_gettime64() + 50000;
    for (const auto &stem : stems_) {
        for (unsigned i = 0; i < stem.channels; ++i) {
            while (snd_is_playing(stem.channel[i])) {
                if (timer_us_gettime64() >= deadline) {
                    snd_shutdown();
                    for (auto &invalid : stems_) {
                        invalid.channel[0] = invalid.channel[1] = -1;
                        invalid.address[0] = invalid.address[1] = 0;
                    }
                    return false;
                }
                thd_sleep(1);
            }
        }
    }
    return true;
}

bool Audio::pause() {
    if (!active_) {
        return false;
    }
    if (stems_[0].clip) {
        pause_clips({this});
        return paused_;
    }
    if (!halt()) {
        fail("The AICA did not stop.");
        return false;
    }
    paused_ = true;
    return true;
}

bool Audio::resume() {
    if (!paused_) {
        return false;
    }

    auto &clip = stems_[0];
    if (clip.clip) {
        clip.decoded = played_frames_;
        for (unsigned block = 0; block < blocks; ++block) {
            fill(clip, block);
        }
        begin_stream(played_frames_);
        return true;
    }

    // Completed blocks have already been refilled. The unread PCM wraps from
    // last_cursor_ to the start of its block. Rotate it to sample zero, then
    // decode ahead to fill the space reclaimed from the current block.
    if (last_cursor_) {
        const auto consumed = last_cursor_ % block_frames;
        for (auto &stem : stems_) {
            if (!stem.open) {
                continue;
            }
            if (!decode(stem, consumed)) {
                fail("Error decoding the audio track.");
                return false;
            }
            for (unsigned channel = 0; channel < stem.channels; ++channel) {
                spu_memread(resume_pcm_, stem.address[channel], sizeof(resume_pcm_));
                std::rotate(resume_pcm_, resume_pcm_ + last_cursor_, resume_pcm_ + ring_frames);
                auto *tail = resume_pcm_ + ring_frames - consumed;
                for (unsigned i = 0; i < consumed; ++i) {
                    tail[i] = decoded_[i * stem.channels + channel];
                }
                spu_memload_sq(stem.address[channel], resume_pcm_, sizeof(resume_pcm_));
            }
        }
    }
    begin_stream(played_frames_);
    return true;
}

unsigned Audio::stem_volume(unsigned index) const {
    if (stems_[index].clip) {
        return fade_volume_; // SDL clip gain is already applied to its PCM.
    }
    const bool guitar = index == 1 || (index == 0 && guitar_controls_backing_);
    const unsigned level = guitar ? guitar_level_ : index == 0 ? song_level_ : rhythm_level_;
    // The reference quantizes the configured level to SDL_mixer's 0–128 scale.
    const auto sdl_level = level * 128 / 100;
    const float linear = sdl_level * fade_volume_ * (guitar ? guitar_gate_ : 1.0f) / 128.0f;
    // KOS truncates its logarithmic AICA attenuation. Bias by half a hardware
    // step (2^(-1/32)) so intermediate gains round towards the requested level.
    return linear >= 255.0f ? 255u : static_cast<unsigned>(linear * 0.97857206f + 0.5f);
}

void Audio::update_volume() {
    if (!active_) {
        return;
    }
    AicaCommand cmd;
    for (unsigned index = 0; index < 3; ++index) {
        auto &stem = stems_[index];
        const auto volume = stem_volume(index);
        if (!stem.channels || volume == stem.volume) {
            continue;
        }
        stem.volume = volume;
        cmd.voice()->vol = volume;
        for (unsigned i = 0; i < stem.channels; ++i) {
            cmd.send(stem.channel[i], AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL);
        }
    }
}

void Audio::set_mix(unsigned guitar, unsigned song, unsigned rhythm) {
    guitar_level_ = std::min(guitar, 100u);
    song_level_ = std::min(song, 100u);
    rhythm_level_ = std::min(rhythm, 100u);
    update_volume();
}

void Audio::guitar_gain(float gate, bool picked) {
    const float value = std::clamp(gate, 0.0f, 1.0f);
    const bool use_backing = !has_guitar() && (picked || value != guitar_gate_ || guitar_controls_backing_);
    if (value != guitar_gate_ || use_backing != guitar_controls_backing_) {
        guitar_gate_ = value;
        guitar_controls_backing_ = use_backing;
        update_volume();
    }
}

void Audio::restore_backing_gain() {
    if (guitar_controls_backing_) {
        guitar_controls_backing_ = false;
        update_volume();
    }
}

unsigned Audio::backing_level() const {
    return guitar_controls_backing_ ? unsigned(guitar_level_ * guitar_gate_) : song_level_;
}

void Audio::fade_out(unsigned duration_ms) {
    if (!active_ || fade_duration_ms_) {
        return;
    }
    if (!duration_ms) {
        stop();
        return;
    }
    fade_begin_ms_ = timer_ms_gettime64();
    fade_begin_volume_ = fade_volume_;
    fade_duration_ms_ = duration_ms;
}

int Audio::position_ms() const {
    return static_cast<int>(uint64_t(played_frames_) * 1000 / sample_rate);
}

void Audio::stop() {
    halt();
    clip_stop_pending_ = false;
    paused_ = false;
    prepared_ = false;
    fade_volume_ = 255;
    fade_duration_ms_ = 0;
    for (auto &stem : stems_) {
        close_stem(stem);
    }
}

void Audio::close_stem(Stem &stem) {
    for (unsigned i = 0; i < 2; ++i) {
        if (stem.channel[i] >= 0) {
            snd_sfx_chn_free(stem.channel[i]);
            stem.channel[i] = -1;
        }
        if (stem.address[i]) {
            snd_mem_free(stem.address[i]);
            stem.address[i] = 0;
        }
    }
    if (stem.open) {
        ov_clear(&stem.decoder);
        stem.open = false;
    }
    stem.file = nullptr;
    stem.service = nullptr;
    stem.clip = nullptr;
    std::free(stem.read_buffer);
    stem.read_buffer = nullptr;
    stem.resampler.reset();
    stem.channels = stem.frames = stem.decoded = 0;
}

void Audio::fail(const char *message) {
    error_ = message;
    std::printf("FoFiXDC: %s\n", message);
    stop();
}

void ResultAudio::schedule(const char *path) {
    pending_ = path;
    scheduled_ms_ = timer_ms_gettime64();
}

void ResultAudio::set_volume(unsigned guitar) {
    volume_ = guitar;
    if (audio_) {
        audio_->set_mix(guitar, guitar, 100);
    }
}

void ResultAudio::pause() {
    if (playing()) {
        audio_->poll();
        audio_->pause();
    }
}

void ResultAudio::resume() {
    if (audio_ && audio_->paused()) {
        audio_->resume();
    }
}

void ResultAudio::poll(bool results_visible) {
    if (audio_) {
        audio_->poll();
        if (!audio_->playing() && !audio_->paused()) {
            audio_.reset();
        }
    }
    if (!results_visible) {
        pending_ = nullptr;
    }
    // GameResultsScene plays a taunt after five seconds. Once started, the
    // original SDL mixer lets it continue independently of the results scene.
    if (!pending_ || timer_ms_gettime64() - scheduled_ms_ <= 5000) {
        return;
    }
    const auto *path = pending_;
    pending_ = nullptr;
    if (!audio_) {
        audio_.reset(new (std::nothrow) Audio);
        if (audio_) {
            audio_->set_mix(volume_, volume_, 100);
        }
    }
    if (!audio_) {
        std::printf("FoFDC: could not allocate result audio for %s\n", path);
    } else if (!audio_->start(path)) {
        std::printf("FoFDC: could not start result audio %s: %s\n", path, audio_->error());
        audio_.reset();
    } else {
        std::printf("FoFDC: result audio %s\n", path);
    }
}

} // namespace dc
