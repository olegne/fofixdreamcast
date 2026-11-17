#include "vmu_lcd.hpp"
#include <dc/maple.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

maple_device_t devices[4][6];
bool attached[4][6]{};
void (*on_detach)(maple_device_t *) = nullptr;
std::vector<maple_frame_t *> pending;
std::vector<int> ordering;
struct Packet { int port, unit; dc::VmuBitmap bitmap; };
std::vector<Packet> sent;

maple_device_t *maple_enum_dev(int p, int u) {
    assert(p >= 0 && p < 4 && u >= 0 && u < 6);
    return attached[p][u] ? &devices[p][u] : nullptr;
}
maple_device_t *maple_enum_type(int n, uint32_t function) {
    for (int p = 0; p < 4; ++p) for (int u = 0; u < 6; ++u) {
        auto *dev = maple_enum_dev(p, u);
        if (dev && (dev->info.functions & function) && !n--) return dev;
    }
    return nullptr;
}
void maple_detach_callback(uint32_t, void (*cb)(maple_device_t *)) { on_detach = cb; }
int maple_queue_frame(maple_frame_t *f) {
    assert(!f->queued && f->state == MAPLE_FRAME_UNSENT);
    f->queued = true;
    pending.push_back(f);
    ordering.push_back(2);
    return 0;
}
void maple_queue_remove(maple_frame_t *f) {
    for (auto i = pending.begin(); i != pending.end(); ++i) if (*i == f) {
        pending.erase(i); break;
    }
    f->queued = false;
}
int maple_frame_lock(maple_frame_t *f) {
    if (f->queued || f->state != MAPLE_FRAME_VACANT) return -1;
    f->state = MAPLE_FRAME_UNSENT;
    return 0;
}
void maple_frame_init(maple_frame_t *f) { assert(f->state == 1); f->recv_buf = f->recv; }
void maple_frame_unlock(maple_frame_t *f) { assert(f->state == 3); f->state = 0; }
namespace dc { void rumble_after_polling() { ordering.push_back(1); } }
extern "C" void __wrap_maple_queue_flush();
extern "C" void __real_maple_queue_flush() {
    ordering.push_back(3);
    for (auto *f : pending) if (f->state == 1) {
        assert(f->cmd == MAPLE_COMMAND_BWRITE && f->length == 50);
        const auto *words = static_cast<const uint32_t *>(f->send_buf);
        assert(words[0] == MAPLE_FUNC_LCD && words[1] == 0);
        Packet packet{f->dst_port, f->dst_unit, {}};
        std::memcpy(packet.bitmap.data(), words + 2, packet.bitmap.size());
        sent.push_back(packet);
        f->state = 2;
    }
}
void reply() {
    assert(pending.size() == 1);
    auto *f = pending.front();
    maple_queue_remove(f);
    f->state = 3;
    static_cast<maple_response_t *>(f->recv_buf)->response = MAPLE_RESPONSE_OK;
    f->callback(nullptr, f);
}
void attach(int p, int u) {
    devices[p][u].port = p; devices[p][u].unit = u;
    devices[p][u].info.functions = MAPLE_FUNC_LCD;
    attached[p][u] = true;
}
void detach(int p, int u) {
    assert(on_detach);
    on_detach(&devices[p][u]);
    attached[p][u] = false;
}

int main() {
    attach(0, 1); attach(1, 1);
    dc::VmuLcd lcd;
    dc::VmuBitmap image{};
    image[0] = 0x81; image[191] = 0x42;
    assert(lcd.ready(0, -1)); lcd.present(image);
    assert(pending.empty()); // No immediate transfer before input polling.
    __wrap_maple_queue_flush();
    assert((ordering == std::vector<int>{1,2,3}));
    assert(sent.back().port == 0 && sent.back().unit == 1 && sent.back().bitmap == image);
    reply();

    assert(lcd.ready(1, 1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.back().port == 1 && sent.back().unit == 1); reply();
    const auto before_duplicate = sent.size();
    assert(lcd.ready(68, 1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.size() == before_duplicate && pending.empty());

    detach(1, 1);
    assert(!lcd.ready(135, 1)); // Never fall back to A in keyboard mode.
    __wrap_maple_queue_flush(); assert(sent.size() == before_duplicate);
    attach(1, 2);
    assert(lcd.ready(202, 1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.back().port == 1 && sent.back().unit == 2); reply();

    image[1] = 7;
    assert(lcd.ready(269, 1)); lcd.present(image); // B pending, not submitted.
    assert(lcd.ready(270, -1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.back().port == 0); reply();

    assert(lcd.ready(271, 1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(!lcd.ready(272, -1)); // In-flight B buffer is retained until response.
    reply();
    assert(lcd.ready(339, -1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.back().port == 0 && sent.back().bitmap == image); reply();

    assert(lcd.ready(340, 1)); lcd.present(image);
    const auto before_detach = sent.size(); detach(1, 2);
    __wrap_maple_queue_flush(); assert(sent.size() == before_detach && pending.empty());
    attach(1, 1);
    assert(lcd.ready(407, 1)); lcd.present(image);
    lcd.suspend(); __wrap_maple_queue_flush(); assert(sent.size() == before_detach);
    assert(lcd.ready(474, 1)); lcd.present(image); __wrap_maple_queue_flush();
    assert(sent.back().port == 1 && sent.back().unit == 1); reply();

    std::puts("PASS: port B selection, no fallback, both slots, mode change, in-flight lifetime, detach, save suspension, deduplication and queue order");
}
