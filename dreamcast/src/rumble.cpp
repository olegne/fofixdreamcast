// SPDX-License-Identifier: GPL-2.0-or-later
#include "rumble.hpp"

#include <arch/irq.h>
#include <arch/timer.h>
#include <dc/maple.h>

#include <array>
#include <cstdint>

#ifdef FRETSONFIRE_INPUT_TRACE
extern "C" {
volatile uint32_t dc_rumble_events = 0;
volatile uint32_t dc_rumble_started = 0;
volatile uint32_t dc_rumble_stopped = 0;
volatile uint32_t dc_rumble_acknowledged = 0;
volatile uint32_t dc_rumble_retries = 0;
volatile uint32_t dc_rumble_dropped = 0;
volatile uint32_t dc_rumble_last_raw = 0;
volatile int32_t dc_rumble_last_port = -1;
volatile int32_t dc_rumble_last_unit = -1;
volatile uint32_t dc_rumble_trace_count = 0;
volatile uint32_t dc_rumble_trace[64][5]{};
}
#define RUMBLE_COUNT(name) (++dc_rumble_##name)
#else
#define RUMBLE_COUNT(name) ((void)0)
#endif

namespace {

constexpr uint64_t pulse_ms = 140;
constexpr uint64_t gap_ms = 33;
constexpr uint64_t event_lifetime_ms = 700;
constexpr uint64_t stop_retry_ms = 300;
constexpr unsigned max_on_attempts = 3;
constexpr unsigned max_stop_attempts = 8;
constexpr uint32_t stop_raw = 0x00000010; // motor 1; both powers zero

struct Target {
    int port = -1;
    int unit = -1;
    uint32_t generation = 0;
};

bool same_target(const Target &a, const Target &b) {
    return a.port == b.port && a.unit == b.unit && a.generation == b.generation;
}

struct Event {
    Target target;
    uint32_t raw = 0;
    uint64_t expires = 0;
    unsigned attempts = 0;
};

enum class Command { none, start, stop };

// This transfer owns its DMA buffers for the process lifetime. A removal does
// not invalidate them, and the device's own frame remains available to KOS.
struct Transfer {
    maple_frame_t frame{};
    std::array<uint32_t, 2> words{MAPLE_FUNC_PURUPURU, stop_raw};
    Target target;
    Command command = Command::none;
};

Transfer transfer;
std::array<std::array<uint32_t, MAPLE_UNIT_COUNT>, MAPLE_PORT_COUNT> generations{};
std::array<Event, 4> events{};
unsigned event_head = 0;
unsigned event_count = 0;
unsigned previous_multiplier = 1;
bool observer_installed = false;
bool playing = false;
bool keyboard_mode = false;

Target active;
uint32_t active_raw = 0;
bool may_be_running = false;
bool stop_requested = false;
bool retry_start = false;
unsigned on_attempts = 0;
unsigned stop_attempts = 0;
uint64_t stop_at = 0;
uint64_t stop_expires = 0;
uint64_t next_start_at = 0;

bool current_generation(const Target &target) {
    return target.port >= 0 && target.port < MAPLE_PORT_COUNT &&
           target.unit >= 0 && target.unit < MAPLE_UNIT_COUNT &&
           target.generation == generations[target.port][target.unit];
}

maple_device_t *target_device(const Target &target) {
    if (!current_generation(target)) {
        return nullptr;
    }
    auto *dev = maple_enum_dev(target.port, target.unit);
    return dev && (dev->info.functions & MAPLE_FUNC_PURUPURU) ? dev : nullptr;
}

bool device_idle(const Target &target) {
    const auto *dev = target_device(target);
    return dev && !dev->frame.queued && dev->frame.state == MAPLE_FRAME_VACANT;
}

int selected_port() {
    // Stop promptly if hotplug changes the input source before the main loop
    // can publish its next snapshot. Never redirect an old hit to another pad.
    const bool keyboard_present = maple_enum_type(0, MAPLE_FUNC_KEYBOARD) != nullptr;
    if (keyboard_present != keyboard_mode) {
        return -1;
    }
    if (keyboard_mode) {
        return 1;
    }
    const auto *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    return controller ? controller->port : -1;
}

Target find_target(int port) {
    if (port >= 0 && port < MAPLE_PORT_COUNT) {
        for (int unit = 0; unit < MAPLE_UNIT_COUNT; ++unit) {
            const auto *dev = maple_enum_dev(port, unit);
            if (dev && (dev->info.functions & MAPLE_FUNC_PURUPURU)) {
                return {port, unit, generations[port][unit]};
            }
        }
    }
    return {};
}

void forget_active() {
    active = {};
    may_be_running = false;
    stop_requested = false;
    retry_start = false;
}

void request_stop(uint64_t now) {
    retry_start = false;
    if (may_be_running && !stop_requested) {
        stop_requested = true;
        stop_expires = now + stop_retry_ms;
        stop_attempts = 0;
    }
}

void cancel_events(uint64_t now) {
    event_count = 0;
    event_head = 0;
    request_stop(now);
}

void detached(maple_device_t *dev) {
    if (dev->port >= 0 && dev->port < MAPLE_PORT_COUNT &&
        dev->unit >= 0 && dev->unit < MAPLE_UNIT_COUNT) {
        ++generations[dev->port][dev->unit];
    }
}

void ensure_observer() {
    if (!observer_installed) {
        // This is the rumble driver only; preserve the input/LCD observers.
        maple_detach_callback(MAPLE_FUNC_PURUPURU, detached);
        observer_installed = true;
    }
}

void reply(maple_state_t *, maple_frame_t *frame) {
    const auto *response = reinterpret_cast<const maple_response_t *>(frame->recv_buf);
    const bool accepted = response->response == MAPLE_RESPONSE_OK &&
                          current_generation(transfer.target);
    if (accepted) {
        RUMBLE_COUNT(acknowledged);
    }
    if (same_target(transfer.target, active) && may_be_running) {
        if (transfer.command == Command::stop && accepted) {
            forget_active();
            next_start_at = timer_ms_gettime64() + gap_ms;
        } else if (!accepted) {
            // A missing reply can be ambiguous: prefer a bounded STOP over
            // replaying an optional pulse that may already have happened.
            request_stop(timer_ms_gettime64());
        }
    }
    transfer.command = Command::none;
    maple_frame_unlock(frame);
}

bool submit(const Target &target, uint32_t raw, Command command, uint64_t now) {
    if (!device_idle(target) || maple_frame_lock(&transfer.frame) != 0) {
        return false;
    }
    maple_frame_init(&transfer.frame);
    transfer.words[1] = raw;
    transfer.target = target;
    transfer.command = command;
    transfer.frame.cmd = MAPLE_COMMAND_SETCOND;
    transfer.frame.dst_port = target.port;
    transfer.frame.dst_unit = target.unit;
    transfer.frame.length = static_cast<int>(transfer.words.size());
    transfer.frame.send_buf = transfer.words.data();
    transfer.frame.callback = reply;
    if (maple_queue_frame(&transfer.frame) != 0) {
        transfer.frame.state = MAPLE_FRAME_VACANT;
        transfer.command = Command::none;
        return false;
    }
#ifdef FRETSONFIRE_INPUT_TRACE
    dc_rumble_last_raw = raw;
    dc_rumble_last_port = target.port;
    dc_rumble_last_unit = target.unit;
    const auto index = dc_rumble_trace_count;
    auto &entry = dc_rumble_trace[index % 64];
    entry[0] = static_cast<uint32_t>(now);
    entry[1] = static_cast<uint32_t>(target.port);
    entry[2] = static_cast<uint32_t>(target.unit);
    entry[3] = raw;
    entry[4] = command == Command::stop;
    dc_rumble_trace_count = index + 1;
#else
    (void)now;
#endif
    return true;
}

void pop_event() {
    event_head = (event_head + 1) % events.size();
    --event_count;
}

} // namespace

namespace dc {

void rumble_context(bool active_gameplay, bool keyboard) {
    irq_disable_scoped();
    ensure_observer();
    if (!active_gameplay || keyboard_mode != keyboard) {
        cancel_events(timer_ms_gettime64());
    }
    playing = active_gameplay;
    keyboard_mode = keyboard;
}

void rumble_reset(unsigned multiplier) {
    irq_disable_scoped();
    ensure_observer();
    previous_multiplier = multiplier ? multiplier : 1;
    cancel_events(timer_ms_gettime64());
}

void rumble_observe(unsigned multiplier, bool hit) {
    irq_disable_scoped();
    const unsigned previous = previous_multiplier;
    previous_multiplier = multiplier ? multiplier : 1;
    if (!playing || !hit || previous_multiplier <= previous) {
        return;
    }
    RUMBLE_COUNT(events);
    const auto target = find_target(selected_port());
    if (target.port < 0 || event_count == events.size()) {
        RUMBLE_COUNT(dropped);
        return;
    }
    // Use KOS's "Basic Thud" shape: noncontinuous, motor 1, forward power
    // and inclination 1. Frequency 7 also gives DreamColor's finite-effect
    // decoder a 1000/7 ~= 142 ms lifetime, matching our explicit 140 ms STOP.
    // Retain the pack's autonomous stop as a fallback for a lost STOP packet.
    // Never set cont, div or conv; packs disagree on the ramping modes.
    const unsigned power = previous_multiplier >= 4 ? 7 : previous_multiplier + 3;
    const uint32_t raw = (1u << 24) | (7u << 16) | (power << 12) | stop_raw;
    events[(event_head + event_count) % events.size()] =
        {target, raw, timer_ms_gettime64() + event_lifetime_ms, 0};
    ++event_count;
}

void rumble_after_polling() {
    irq_disable_scoped();
    if (!observer_installed) {
        return;
    }
    const auto now = timer_ms_gettime64();
    const auto port = selected_port();

    if (may_be_running && !target_device(active)) {
        forget_active();
    }
    if (may_be_running && (!playing || port != active.port || now >= stop_at)) {
        request_stop(now);
    }

    if (transfer.frame.queued && transfer.frame.state == MAPLE_FRAME_UNSENT) {
        // KOS retains an AGAIN response at the front of its queue. Remove it
        // so each bounded retry goes AFTER the next fresh keyboard/pad poll.
        const auto command = transfer.command;
        maple_queue_remove(&transfer.frame);
        transfer.frame.state = MAPLE_FRAME_VACANT;
        transfer.command = Command::none;
        if (command == Command::start && may_be_running) {
            retry_start = !stop_requested && on_attempts < max_on_attempts;
            if (!retry_start) {
                request_stop(now);
            }
        }
    }
    if (transfer.frame.queued || transfer.frame.state != MAPLE_FRAME_VACANT) {
        return; // Never overwrite a DMA transfer which is already in flight.
    }

    if (may_be_running && stop_requested) {
        if (now >= stop_expires || stop_attempts >= max_stop_attempts) {
            // No infinite Maple retry and no continuous motor command: give
            // the pack's finite effect time to expire before any new pulse.
            forget_active();
            next_start_at = now + 500;
            RUMBLE_COUNT(dropped);
        } else if (submit(active, stop_raw, Command::stop, now)) {
            if (stop_attempts++) {
                RUMBLE_COUNT(retries);
            }
            RUMBLE_COUNT(stopped);
        }
        return;
    }
    if (may_be_running) {
        if (retry_start && submit(active, active_raw, Command::start, now)) {
            ++on_attempts;
            retry_start = false;
            RUMBLE_COUNT(retries);
        }
        return;
    }

    while (event_count) {
        const auto &event = events[event_head];
        if (playing && event.target.port == port && now < event.expires &&
            target_device(event.target) && event.attempts < max_on_attempts) {
            break;
        }
        pop_event();
        RUMBLE_COUNT(dropped);
    }
    if (!event_count || now < next_start_at) {
        return;
    }
    auto &event = events[event_head];
    if (!device_idle(event.target)) {
        return;
    }
    ++event.attempts;
    if (submit(event.target, event.raw, Command::start, now)) {
        active = event.target;
        active_raw = event.raw;
        may_be_running = true;
        stop_requested = false;
        retry_start = false;
        on_attempts = 1;
        stop_attempts = 0;
        stop_at = now + pulse_ms;
        RUMBLE_COUNT(started);
        pop_event();
    }
}

} // namespace dc
