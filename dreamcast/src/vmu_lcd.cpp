// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmu_lcd.hpp"

#include <arch/irq.h>
#include <dc/maple.h>

#include <array>
#include <cstring>

extern "C" void __real_maple_queue_flush();

#ifdef FRETSONFIRE_INPUT_TRACE
extern "C" {
volatile uint32_t dc_vmu_lcd_submitted = 0;
volatile uint32_t dc_vmu_lcd_acknowledged = 0;
}
#endif

namespace {

constexpr uint64_t refresh_ms = 67;
constexpr uint64_t retry_ms = 500;

// Keep the DMA buffers alive across scene changes and device removal. The
// device's own frame remains available to KOS for memory card operations.
struct LcdTransfer {
    maple_frame_t frame{};
    std::array<uint32_t, 2 + sizeof(dc::VmuBitmap) / sizeof(uint32_t)> words{MAPLE_FUNC_LCD, 0};
    uint32_t generation = 0;
    bool pending = false;
    bool acknowledged = false;
    bool retry_later = false;
};

LcdTransfer lcd;
uint32_t generation = 0;
bool detach_observer_installed = false;

bool lcd_idle(const maple_device_t *dev) {
    return dev && (dev->info.functions & MAPLE_FUNC_LCD) &&
           !dev->frame.queued && dev->frame.state == MAPLE_FRAME_VACANT;
}

void lcd_detached(maple_device_t *dev) {
    if (dev->info.functions & MAPLE_FUNC_LCD) {
        ++generation;
    }
}

void lcd_reply(maple_state_t *, maple_frame_t *frame) {
    const auto *response = reinterpret_cast<const maple_response_t *>(frame->recv_buf);
    lcd.acknowledged = response->response == MAPLE_RESPONSE_OK && lcd.generation == generation;
    lcd.retry_later = response->response != MAPLE_RESPONSE_OK;
#ifdef FRETSONFIRE_INPUT_TRACE
    if (lcd.acknowledged) {
        ++dc_vmu_lcd_acknowledged;
    }
#endif
    maple_frame_unlock(frame);
}

void discard_unsent() {
    if (lcd.frame.queued) {
        maple_queue_remove(&lcd.frame);
    }
    lcd.frame.state = MAPLE_FRAME_VACANT;
    lcd.pending = false;
}

void queue_lcd_after_polling() {
    if (lcd.frame.queued && lcd.frame.state == MAPLE_FRAME_UNSENT) {
        // KOS can automatically retry an AGAIN response. Drop optional LCD
        // traffic instead of retransmitting it ahead of the next input poll.
        discard_unsent();
        lcd.retry_later = true;
    }
    if (lcd.pending) {
        lcd.pending = false;
        if (lcd.generation != generation ||
            !lcd_idle(maple_enum_dev(lcd.frame.dst_port, lcd.frame.dst_unit)) ||
            maple_queue_frame(&lcd.frame) != 0) {
            discard_unsent();
            return;
        }
#ifdef FRETSONFIRE_INPUT_TRACE
        ++dc_vmu_lcd_submitted;
#endif
    }
}

} // namespace

extern "C" void __wrap_maple_queue_flush() {
    // KOS has already queued device detection, keyboard and controller input
    // here. The optional LCD packet goes at the tail of their DMA batch.
    queue_lcd_after_polling();
    __real_maple_queue_flush();
}

namespace dc {

bool VmuLcd::ready(uint64_t now_ms) {
    active_ = true;
    port_ = -1;
    if (now_ms < next_refresh_ms_) {
        return false;
    }
    next_refresh_ms_ = now_ms + refresh_ms;
    irq_disable_scoped();
    if (!detach_observer_installed) {
        maple_detach_callback(MAPLE_FUNC_LCD, lcd_detached);
        detach_observer_installed = true;
    }
    if (lcd.retry_later) {
        lcd.retry_later = false;
        next_refresh_ms_ = now_ms + retry_ms;
        return false;
    }
    if (lcd.frame.queued || lcd.frame.state != MAPLE_FRAME_VACANT) {
        return false;
    }
    const auto *device = maple_enum_type(0, MAPLE_FUNC_LCD);
    if (!lcd_idle(device)) {
        return false;
    }
    port_ = device->port;
    unit_ = device->unit;
    generation_ = generation;
    return true;
}

void VmuLcd::present(const VmuBitmap &bitmap) {
    const int port = port_;
    port_ = -1;
    if (port < 0) {
        return;
    }
    irq_disable_scoped();
    if (generation_ != generation || !lcd_idle(maple_enum_dev(port, unit_))) {
        return;
    }
    auto *pixels = lcd.words.data() + 2;
    if (lcd.acknowledged && lcd.generation == generation && lcd.frame.dst_port == port &&
        lcd.frame.dst_unit == unit_ && std::memcmp(pixels, bitmap.data(), bitmap.size()) == 0) {
        return;
    }
    if (maple_frame_lock(&lcd.frame) != 0) {
        return;
    }
    maple_frame_init(&lcd.frame);
    std::memcpy(pixels, bitmap.data(), bitmap.size());
    lcd.frame.cmd = MAPLE_COMMAND_BWRITE;
    lcd.frame.dst_port = port;
    lcd.frame.dst_unit = unit_;
    lcd.frame.length = static_cast<int>(lcd.words.size());
    lcd.frame.send_buf = lcd.words.data();
    lcd.frame.callback = lcd_reply;
    lcd.generation = generation;
    lcd.acknowledged = false;
    lcd.pending = true;
}

void VmuLcd::suspend() {
    if (!active_) {
        return;
    }
    active_ = false;
    port_ = -1;
    irq_disable_scoped();
    ++generation;
    if (lcd.frame.state == MAPLE_FRAME_UNSENT) {
        discard_unsent();
    }
}

} // namespace dc
