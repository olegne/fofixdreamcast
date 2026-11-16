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

bool Audio::start(const char *path) {
    stop();
    error_ = nullptr;
    played_frames_ = decoded_frames_ = last_cursor_ = 0;
    clock_ready_ = false;
    if (snd_init() < 0) {
        fail("FALHA AO INICIAR AICA");
        return false;
    }

    auto *file = std::fopen(path, "rb");
    if (!file) {
        fail("NAO FOI POSSIVEL ABRIR SONG.OGG");
        return false;
    }
    if (ov_open(file, &decoder_, nullptr, 0) != 0) {
        std::fclose(file);
        fail("ARQUIVO OGG VORBIS INVALIDO");
        return false;
    }
    opened_ = true;
    const auto *info = ov_info(&decoder_, 0);
    const auto length = ov_pcm_total(&decoder_, 0);
    if (ov_streams(&decoder_) != 1 || !info || info->channels < 1 || info->channels > 2 ||
        info->rate < 8000 || info->rate > 48000 || length <= 0 ||
        length > info->rate * 60 * 60) {
        fail("USE OGG VORBIS MONO OU ESTEREO");
        return false;
    }
    channels_ = static_cast<unsigned>(info->channels);
    rate_ = static_cast<unsigned>(info->rate);
    length_frames_ = static_cast<unsigned>(length);
    for (unsigned i = 0; i < channels_; ++i) {
        channel_[i] = snd_sfx_chn_alloc();
        address_[i] = snd_mem_malloc(ring_frames * sizeof(int16_t));
        if (channel_[i] < 0 || !address_[i]) {
            fail("SEM MEMORIA DE AUDIO AICA");
            return false;
        }
    }
    for (unsigned block = 0; block < blocks; ++block) {
        if (!fill(block)) {
            fail("ERRO DURANTE A LEITURA DO OGG");
            return false;
        }
    }

    queue_start();
    active_ = true;
    last_poll_us_ = timer_us_gettime64();
    std::printf("FoFiXDC: audio %u Hz, %u channel(s), %u frames\n",
                rate_, channels_, length_frames_);
    return true;
}

bool Audio::fill(unsigned block) {
    const auto frames = std::min(block_frames, length_frames_ - decoded_frames_);
    const unsigned bytes = frames * channels_ * sizeof(int16_t);
    auto *pcm = reinterpret_cast<char *>(decoded_);
    unsigned offset = 0;
    while (offset < bytes) {
        int section = 0;
        const auto got = ov_read(&decoder_, pcm + offset, bytes - offset, 0, 2, 1, &section);
        if (got <= 0) {
            return false;
        }
        offset += static_cast<unsigned>(got);
    }
    decoded_frames_ += frames;
    const auto block_bytes = block_frames * sizeof(int16_t);
    std::memset(pcm + offset, 0, block_bytes * channels_ - offset);
    if (channels_ == 1) {
        spu_memload_sq(address_[0] + block * block_bytes, decoded_, block_bytes);
    } else {
        snd_pcm16_split(reinterpret_cast<uint32_t *>(decoded_),
                        reinterpret_cast<uint32_t *>(split_[0]),
                        reinterpret_cast<uint32_t *>(split_[1]), block_bytes * 2);
        for (unsigned i = 0; i < channels_; ++i) {
            spu_memload_sq(address_[i] + block * block_bytes, split_[i], block_bytes);
        }
    }
    return true;
}

void Audio::queue_start() {
    AICA_CMDSTR_CHANNEL(packet, command, voice);
    std::memset(packet, 0, sizeof(packet));
    command->cmd = AICA_CMD_CHAN;
    command->size = AICA_CMDSTR_CHANNEL_SIZE;
    voice->cmd = AICA_CH_CMD_START | AICA_CH_START_DELAY;
    voice->type = AICA_SM_16BIT;
    voice->length = ring_frames;
    voice->loop = 1;
    voice->loopend = ring_frames;
    voice->freq = rate_;
    voice->vol = 255;
    uint64_t mask = 0;

    snd_sh4_to_aica_stop();
    for (unsigned i = 0; i < channels_; ++i) {
        command->cmd_id = channel_[i];
        voice->base = address_[i];
        voice->pan = channels_ == 1 ? 128 : (i == 0 ? 0 : 255);
        snd_sh4_to_aica(packet, command->size);
        mask |= uint64_t{1} << channel_[i];
    }
    command->cmd_id = mask;
    voice->cmd = AICA_CH_CMD_START | AICA_CH_START_SYNC;
    snd_sh4_to_aica(packet, command->size);
    snd_sh4_to_aica_start();
}

void Audio::poll() {
    if (!active_) {
        return;
    }
    const auto now = timer_us_gettime64();
    const auto safe_poll_interval = uint64_t(ring_frames - block_frames) * 1000000 / rate_;
    if (now - last_poll_us_ >= safe_poll_interval) {
        fail("AICA PERDEU A SINCRONIA");
        return;
    }
    if (!clock_ready_) {
        if (!snd_is_playing(channel_[0])) {
            return;
        }
        clock_ready_ = true;
    }

    // Only a consumed block is rewritten. The cursor is measured on the playing AICA voice.
    const auto cursor = unsigned(snd_get_pos(channel_[0])) & (ring_frames - 1);
    const auto advance = (cursor - last_cursor_) & (ring_frames - 1);
    if (advance >= ring_frames - block_frames) {
        fail("CURSOR DE AUDIO INVALIDO");
        return;
    }
    const auto old_block = played_frames_ / block_frames;
    played_frames_ += advance;
    last_cursor_ = cursor;
    last_poll_us_ = now;
    if (played_frames_ >= length_frames_) {
        played_frames_ = length_frames_;
        stop();
        return;
    }
    for (auto block = old_block; block < played_frames_ / block_frames; ++block) {
        if (!fill(block % blocks)) {
            fail("ERRO DURANTE A LEITURA DO OGG");
            return;
        }
    }
}

int Audio::position_ms() const {
    return rate_ ? static_cast<int>(uint64_t(played_frames_) * 1000 / rate_) : 0;
}

void Audio::stop() {
    if (active_) {
        AICA_CMDSTR_CHANNEL(packet, command, voice);
        std::memset(packet, 0, sizeof(packet));
        command->cmd = AICA_CMD_CHAN;
        command->size = AICA_CMDSTR_CHANNEL_SIZE;
        voice->cmd = AICA_CH_CMD_STOP;
        for (unsigned i = 0; i < channels_; ++i) {
            command->cmd_id = channel_[i];
            snd_sh4_to_aica(packet, command->size);
        }
        // Wait for the ARM to apply the stops before returning SPU buffers to the allocator.
        for (unsigned i = 0; i < channels_; ++i) {
            const auto deadline = timer_us_gettime64() + 50000;
            while (snd_is_playing(channel_[i]) && timer_us_gettime64() < deadline) {
                thd_sleep(1);
            }
            if (snd_is_playing(channel_[i])) {
                snd_shutdown();
                channel_[0] = channel_[1] = -1;
                address_[0] = address_[1] = 0;
                break;
            }
        }
        active_ = false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (channel_[i] >= 0) {
            snd_sfx_chn_free(channel_[i]);
            channel_[i] = -1;
        }
        if (address_[i]) {
            snd_mem_free(address_[i]);
            address_[i] = 0;
        }
    }
    if (opened_) {
        ov_clear(&decoder_);
        opened_ = false;
    }
}

void Audio::fail(const char *message) {
    error_ = message;
    std::printf("FoFiXDC: %s\n", message);
    stop();
}

} // namespace dc
