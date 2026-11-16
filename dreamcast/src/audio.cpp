// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <arch/timer.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <kos/thread.h>

namespace dc {
namespace {

constexpr unsigned backing_volume = 204; // FoFiX's 80% background volume.

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

bool Audio::start(const char *backing, const char *guitar) {
    stop();
    error_ = nullptr;
    rate_ = played_frames_ = 0;
    guitar_volume_ = 255;
    if (snd_init() < 0) {
        fail("AICA initialization failed.");
        return false;
    }
    if (!open(stems_[0], backing) || (guitar && !open(stems_[1], guitar))) {
        stop();
        return false;
    }
    length_frames_ = std::max(stems_[0].frames, stems_[1].frames);
    if (!prepare(0)) {
        return false;
    }
    std::printf("FoFiXDC: audio %u Hz, backing=%u ch guitar=%u ch samples=%u\n",
                rate_, stems_[0].channels, stems_[1].channels, length_frames_);
    return true;
}

bool Audio::open(Stem &stem, const char *path) {
    auto *file = std::fopen(path, "rb");
    if (!file) {
        error_ = "Could not open an audio track.";
        return false;
    }
    if (ov_open(file, &stem.decoder, nullptr, 0) != 0) {
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

bool Audio::fill(Stem &stem, unsigned block) {
    const auto frames = std::min(block_frames, stem.frames - stem.decoded);
    const unsigned bytes = frames * stem.channels * sizeof(int16_t);
    auto *pcm = reinterpret_cast<char *>(decoded_);
    unsigned offset = 0;
    while (offset < bytes) {
        int section = 0;
        const auto got = ov_read(&stem.decoder, pcm + offset, bytes - offset, 0, 2, 1, &section);
        if (got <= 0) {
            return false;
        }
        offset += static_cast<unsigned>(got);
    }
    stem.decoded += frames;
    const auto block_bytes = block_frames * sizeof(int16_t);
    std::memset(pcm + offset, 0, block_bytes * stem.channels - offset);
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

bool Audio::prepare(unsigned position) {
    for (auto &stem : stems_) {
        if (!stem.open) {
            continue;
        }
        const auto offset = std::min(position, stem.frames);
        if (stem.decoded != offset) {
            stem.decoded = offset;
            if (stem.decoded < stem.frames && ov_pcm_seek(&stem.decoder, stem.decoded) != 0) {
                fail("Could not seek in the audio track.");
                return false;
            }
        }
        for (unsigned block = 0; block < blocks; ++block) {
            if (!fill(stem, block)) {
                fail("Error decoding the audio track.");
                return false;
            }
        }
    }

    ring_origin_ = played_frames_ = position;
    last_cursor_ = 0;
    clock_ready_ = false;
    queue_start();
    active_ = true;
    paused_ = false;
    last_poll_us_ = timer_us_gettime64();
    return true;
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
            if (stem.open && !fill(stem, block % blocks)) {
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
    return paused_ && prepare(played_frames_);
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
        stem.channels = stem.frames = stem.decoded = 0;
    }
}

void Audio::fail(const char *message) {
    error_ = message;
    std::printf("FoFiXDC: %s\n", message);
    stop();
}

} // namespace dc
