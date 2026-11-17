// Host behavioral tests against the real rumble.cpp, with simulated Maple
// device removal, AGAIN/retry, in-flight DMA and a controllable hardware clock.
#include <dc/maple.h>
#include "rumble.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
extern volatile uint32_t dc_rumble_events;
extern volatile uint32_t dc_rumble_started;
extern volatile uint32_t dc_rumble_stopped;
extern volatile uint32_t dc_rumble_dropped;
extern volatile uint32_t dc_rumble_retries;
}

namespace {
uint64_t clock_ms = 1000;
maple_device_t devices[MAPLE_PORT_COUNT][MAPLE_UNIT_COUNT];
void (*detach_observer)(maple_device_t *) = nullptr;
std::vector<maple_frame_t *> queued;
struct Packet { uint64_t time; int port; int unit; uint32_t raw; };
std::vector<Packet> packets;
bool fail_queue_once = false;

void add(int port, int unit, uint32_t functions) {
    auto &dev = devices[port][unit];
    dev.port = port;
    dev.unit = unit;
    dev.info.functions = functions;
    dev.valid = true;
    dev.frame.state = MAPLE_FRAME_VACANT;
    dev.frame.queued = false;
}

void remove(int port, int unit) {
    auto &dev = devices[port][unit];
    dev.valid = false;
    if (detach_observer && (dev.info.functions & MAPLE_FUNC_PURUPURU)) {
        detach_observer(&dev);
    }
}

void finish_response(int response = MAPLE_RESPONSE_OK) {
    const auto copy = queued;
    for (auto *frame : copy) {
        assert(frame->state == MAPLE_FRAME_SENT);
        if (response == MAPLE_RESPONSE_AGAIN) {
            frame->state = MAPLE_FRAME_UNSENT;
        } else {
            frame->state = MAPLE_FRAME_RESPONDED;
            maple_queue_remove(frame);
            frame->recv_buf[0] = static_cast<uint8_t>(response);
            frame->callback(nullptr, frame);
        }
    }
}

void poll(bool reply = true, int response = MAPLE_RESPONSE_OK) {
    dc::rumble_after_polling();
    for (auto *frame : queued) {
        assert(frame->state == MAPLE_FRAME_UNSENT);
        frame->state = MAPLE_FRAME_SENT;
        const auto *words = static_cast<uint32_t *>(frame->send_buf);
        assert(frame->cmd == MAPLE_COMMAND_SETCOND && frame->length == 2);
        assert(words[0] == MAPLE_FUNC_PURUPURU);
        packets.push_back({clock_ms, frame->dst_port, frame->dst_unit, words[1]});
        // Valid motor, reserved bits clear, and NEVER continuous vibration.
        assert((words[1] & 0xff) == 0x10);
    }
    if (reply) {
        finish_response(response);
    }
}

void advance(uint64_t ms, int response = MAPLE_RESPONSE_OK) {
    clock_ms += ms;
    poll(true, response);
}

void hit(unsigned multiplier) { dc::rumble_observe(multiplier, true); }
bool stopping(const Packet &packet) { return packet.raw == 0x10; }

void setup(bool keyboard = false) {
    if (keyboard) {
        add(0, 0, MAPLE_FUNC_KEYBOARD);
        add(1, 0, MAPLE_FUNC_CONTROLLER);
        add(1, 2, MAPLE_FUNC_PURUPURU);
    } else {
        add(0, 0, MAPLE_FUNC_CONTROLLER);
        add(0, 2, MAPLE_FUNC_PURUPURU);
    }
    dc::rumble_context(true, keyboard);
    dc::rumble_reset();
}
} // namespace

uint64_t timer_ms_gettime64() { return clock_ms; }
maple_device_t *maple_enum_dev(int port, int unit) {
    assert(port >= 0 && port < MAPLE_PORT_COUNT && unit >= 0 && unit < MAPLE_UNIT_COUNT);
    return devices[port][unit].valid ? &devices[port][unit] : nullptr;
}
maple_device_t *maple_enum_type(int index, uint32_t functions) {
    for (auto &port : devices) {
        for (auto &dev : port) {
            if (dev.valid && (dev.info.functions & functions) && index-- == 0) {
                return &dev;
            }
        }
    }
    return nullptr;
}
void maple_detach_callback(uint32_t functions, void (*callback)(maple_device_t *)) {
    assert(functions == MAPLE_FUNC_PURUPURU);
    detach_observer = callback;
}
int maple_frame_lock(maple_frame_t *frame) {
    if (frame->queued || frame->state != MAPLE_FRAME_VACANT) {
        return -1;
    }
    frame->state = MAPLE_FRAME_UNSENT;
    return 0;
}
void maple_frame_init(maple_frame_t *frame) {
    assert(frame->state == MAPLE_FRAME_UNSENT && !frame->queued);
    frame->recv_buf = frame->recv_buf_arr;
    std::memset(frame->recv_buf, 0, 1024);
}
void maple_frame_unlock(maple_frame_t *frame) {
    assert(frame->state == MAPLE_FRAME_RESPONDED);
    frame->state = MAPLE_FRAME_VACANT;
}
int maple_queue_frame(maple_frame_t *frame) {
    if (fail_queue_once) {
        fail_queue_once = false;
        return -1;
    }
    assert(!frame->queued);
    assert(frame->state == MAPLE_FRAME_UNSENT);
    frame->queued = true;
    frame->dev = maple_enum_dev(frame->dst_port, frame->dst_unit);
    queued.push_back(frame);
    return 0;
}
int maple_queue_remove(maple_frame_t *frame) {
    assert(frame->queued);
    queued.erase(std::find(queued.begin(), queued.end(), frame));
    frame->queued = false;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "thresholds") {
        setup();
        for (unsigned m = 2; m <= 4; ++m) {
            hit(m);
            poll();
            for (int repeat = 0; repeat < 10; ++repeat) hit(m);
            advance(139);
            assert(!stopping(packets.back()));
            advance(1);
            assert(stopping(packets.back()));
            advance(33);
        }
        assert(dc_rumble_events == 3 && dc_rumble_started == 3);
        assert(packets.size() == 6 && packets[0].raw == 0x01075010);
        dc::rumble_observe(1, false);
        poll();
        assert(packets.size() == 6);
        hit(2);
        poll();
        assert(dc_rumble_started == 4);
    } else if (scenario == "keyboard_port") {
        setup(true);
        add(2, 0, MAPLE_FUNC_CONTROLLER);
        add(2, 1, MAPLE_FUNC_PURUPURU);
        hit(2);
        poll();
        assert(packets.size() == 1 && packets[0].port == 1 && packets[0].unit == 2);
        advance(140);
        remove(1, 2);
        dc::rumble_observe(1, false);
        hit(2);
        advance(50);
        assert(packets.size() == 2); // No unrelated-port fallback.
    } else if (scenario == "controller_port") {
        add(2, 0, MAPLE_FUNC_CONTROLLER);
        add(2, 1, MAPLE_FUNC_PURUPURU);
        add(3, 0, MAPLE_FUNC_CONTROLLER);
        add(3, 2, MAPLE_FUNC_PURUPURU);
        dc::rumble_context(true, false);
        hit(2);
        poll();
        assert(packets.size() == 1 && packets[0].port == 2);
    } else if (scenario == "pause_restart") {
        setup();
        hit(2);
        poll();
        hit(3); // Cancel a queued next pulse as well.
        dc::rumble_context(false, false);
        advance(20);
        assert(packets.size() == 2 && stopping(packets.back()));
        advance(1000);
        dc::rumble_reset();
        dc::rumble_context(true, false);
        hit(1);
        poll();
        assert(packets.size() == 2);
        hit(2);
        poll();
        assert(packets.size() == 3);
        dc::rumble_reset();
        advance(1);
        assert(stopping(packets.back()));
    } else if (scenario == "source_change") {
        setup();
        add(1, 0, MAPLE_FUNC_CONTROLLER);
        add(1, 2, MAPLE_FUNC_PURUPURU);
        hit(2);
        poll();
        add(3, 0, MAPLE_FUNC_KEYBOARD);
        advance(16); // Stop even before main loop notices keyboard hotplug.
        assert(packets.size() == 2 && packets.back().port == 0 && stopping(packets.back()));
        dc::rumble_context(true, true);
        hit(3);
        advance(33);
        assert(packets.size() == 3 && packets.back().port == 1 && !stopping(packets.back()));
    } else if (scenario == "lost_reply") {
        setup();
        hit(2);
        poll(false);
        const auto original_raw = static_cast<uint32_t *>(queued.front()->send_buf)[1];
        dc::rumble_context(false, false);
        clock_ms += 200;
        dc::rumble_after_polling();
        assert(queued.front()->state == MAPLE_FRAME_SENT);
        assert(static_cast<uint32_t *>(queued.front()->send_buf)[1] == original_raw);
        finish_response(-1);
        poll();
        assert(packets.size() == 2 && stopping(packets.back()));
    } else if (scenario == "again_bounded") {
        setup();
        hit(2);
        poll(true, MAPLE_RESPONSE_AGAIN);
        for (int i = 0; i < 30; ++i) advance(16, MAPLE_RESPONSE_AGAIN);
        assert(packets.size() == 11); // At most 3 ON + 8 STOP submissions.
        assert(dc_rumble_started == 1 && dc_rumble_stopped == 8);
        assert(queued.empty());
    } else if (scenario == "detach_inflight") {
        setup();
        hit(2);
        poll(false);
        remove(0, 2);
        add(0, 2, MAPLE_FUNC_PURUPURU); // Same address, new device generation.
        finish_response();
        advance(200);
        assert(packets.size() == 1); // Never send stale STOP to replacement.
        dc::rumble_observe(1, false);
        hit(2);
        advance(1);
        assert(packets.size() == 2 && !stopping(packets.back()));
    } else if (scenario == "busy_and_failure") {
        setup();
        devices[0][2].frame.queued = true;
        hit(2);
        poll();
        assert(packets.empty());
        devices[0][2].frame.queued = false;
        fail_queue_once = true;
        advance(10);
        assert(packets.empty());
        advance(10);
        assert(packets.size() == 1);
        devices[0][2].frame.queued = true;
        advance(140);
        assert(packets.size() == 1);
        devices[0][2].frame.queued = false;
        advance(16);
        assert(packets.size() == 2 && stopping(packets.back()));
    } else if (scenario == "rapid_crossings") {
        setup();
        hit(2);
        hit(3);
        hit(4);
        poll();
        for (int i = 0; i < 40; ++i) advance(16);
        assert(dc_rumble_events == 3 && dc_rumble_started == 3 && packets.size() == 6);
        for (unsigned i = 0; i < packets.size(); i += 2) {
            assert(!stopping(packets[i]) && stopping(packets[i + 1]));
            assert(packets[i + 1].time - packets[i].time <= 156);
        }
    } else if (scenario == "nonhit_and_absent") {
        dc::rumble_context(true, false);
        hit(2);
        poll();
        assert(packets.empty());
        setup();
        dc::rumble_observe(2, false);
        hit(2);
        poll();
        assert(packets.empty());
        dc::rumble_observe(1, false);
        hit(2);
        dc::rumble_context(false, false);
        poll();
        assert(packets.empty());
    } else {
        assert(false && "unknown scenario");
    }
    std::printf("PASS %s: events=%u starts=%u stop_packets=%u retries=%u dropped=%u\n", argv[1],
                unsigned(dc_rumble_events), unsigned(dc_rumble_started), unsigned(dc_rumble_stopped),
                unsigned(dc_rumble_retries), unsigned(dc_rumble_dropped));
}
