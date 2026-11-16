// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"

#include <algorithm>
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

constexpr unsigned backing_volume = 255; // Frets on Fire defaults songvol to 1.0.
constexpr unsigned cd_read_buffer_size = 16 * 1024;
constexpr unsigned clip_sample_rate = 44100;

bool read_pcm(OggVorbis_File &decoder, int16_t *samples, unsigned bytes) {
    auto *output = reinterpret_cast<char *>(samples);
    unsigned offset = 0;
    while (offset < bytes) {
        int section = 0;
        const auto got = ov_read(&decoder, output + offset, bytes - offset, 0, 2, 1, &section);
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

bool AudioClip::load(const char *path, unsigned volume) {
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
    if (ov_streams(decoder.get()) != 1 || !info || info->channels != 2 || info->rate != clip_sample_rate ||
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
    const int gain = std::min(volume, 128u);
    for (unsigned offset = 0; offset < frames; offset += Audio::block_frames) {
        const auto count = std::min(Audio::block_frames, unsigned(frames) - offset);
        if (!read_pcm(*decoder, interleaved, count * 2 * sizeof(int16_t))) {
            ov_clear(decoder.get());
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            left[offset + i] = interleaved[2 * i] * gain / 128;
            right[offset + i] = interleaved[2 * i + 1] * gain / 128;
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

bool Audio::start(const char *backing, const char *guitar) {
    return prepare(backing, guitar) && play_prepared();
}

bool Audio::start_clip(const AudioClip &clip) {
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
    rate_ = clip_sample_rate;
    guitar_volume_ = 255;
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

bool Audio::prepare(const char *backing, const char *guitar, PrepareService service) {
    stop();
    error_ = nullptr;
    rate_ = played_frames_ = 0;
    guitar_volume_ = 255;
    if (snd_init() < 0) {
        fail("AICA initialization failed.");
        return false;
    }
    if (!open(stems_[0], backing, service) || (guitar && !open(stems_[1], guitar, service))) {
        stop();
        return false;
    }
    length_frames_ = stems_[0].frames;
    for (auto &stem : stems_) {
        for (unsigned block = 0; stem.open && block < blocks; ++block) {
            if (service) {
                service();
            }
            if (!fill(stem, block)) {
                fail("Error decoding the audio track.");
                return false;
            }
        }
        stem.service = nullptr;
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
    begin_stream(0);
    std::printf("FoFDC: audio %u Hz, backing=%u ch guitar=%u ch samples=%u\n",
                rate_, stems_[0].channels, stems_[1].channels, length_frames_);
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
        info->rate < 8000 || info->rate > 48000 || frames <= 0 || frames > info->rate * 3600) {
        error_ = "Unsupported Ogg Vorbis track.";
        return false;
    }
    const auto rate = static_cast<unsigned>(info->rate);
    if (rate_ && rate_ != rate) {
        error_ = "The backing and guitar tracks must share a sample rate.";
        return false;
    }
    rate_ = rate;
    stem.channels = static_cast<unsigned>(info->channels);
    stem.frames = static_cast<unsigned>(frames);
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

bool Audio::decode(Stem &stem, unsigned count) {
    const auto frames = std::min(count, stem.frames - stem.decoded);
    const unsigned bytes = frames * stem.channels * sizeof(int16_t);
    if (!read_pcm(stem.decoder, decoded_, bytes)) {
        return false;
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
            if (frames != block_frames || ((stem.decoded * sizeof(int16_t)) & 31)) {
                std::copy_n(samples, frames, split_[channel]);
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
    voice->freq = rate_;
    uint64_t mask = 0;

    snd_sh4_to_aica_stop();
    for (const auto &stem : stems_) {
        for (unsigned i = 0; i < stem.channels; ++i) {
            voice->base = stem.address[i];
            voice->pan = stem.channels == 1 ? 128 : (i == 0 ? 0 : 255);
            voice->vol = &stem == &stems_[1] || !has_guitar() ? guitar_volume_ : backing_volume;
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
    const auto now = timer_us_gettime64();
    const auto safe_poll_interval = uint64_t(ring_frames - block_frames) * 1000000 / rate_;
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

void Audio::guitar_gain(float volume) {
    const unsigned value = std::clamp(static_cast<int>(volume * 255.0f + 0.5f), 0, 255);
    if (value == guitar_volume_) {
        return;
    }
    guitar_volume_ = value;
    if (active_) {
        const auto &voice = stems_[has_guitar() ? 1 : 0];
        AicaCommand cmd;
        cmd.voice()->vol = guitar_volume_;
        for (unsigned i = 0; i < voice.channels; ++i) {
            cmd.send(voice.channel[i], AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL);
        }
    }
}

int Audio::position_ms() const {
    return rate_ ? static_cast<int>(uint64_t(played_frames_) * 1000 / rate_) : 0;
}

void Audio::stop() {
    halt();
    paused_ = false;
    prepared_ = false;
    for (auto &stem : stems_) {
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
        stem.channels = stem.frames = stem.decoded = 0;
    }
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
