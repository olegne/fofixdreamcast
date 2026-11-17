#pragma once
#include <cstdint>

constexpr int MAPLE_PORT_COUNT = 4;
constexpr int MAPLE_UNIT_COUNT = 6;
constexpr uint32_t MAPLE_FUNC_PURUPURU = 0x00010000;
constexpr uint32_t MAPLE_FUNC_CONTROLLER = 0x01000000;
constexpr uint32_t MAPLE_FUNC_MEMCARD = 0x02000000;
constexpr uint32_t MAPLE_FUNC_LCD = 0x04000000;
constexpr uint32_t MAPLE_FUNC_KEYBOARD = 0x40000000;
constexpr int MAPLE_FRAME_VACANT = 0;
constexpr int MAPLE_FRAME_UNSENT = 1;
constexpr int MAPLE_FRAME_SENT = 2;
constexpr int MAPLE_FRAME_RESPONDED = 3;
constexpr int MAPLE_COMMAND_SETCOND = 14;
constexpr int MAPLE_COMMAND_BWRITE = 12;
constexpr int MAPLE_RESPONSE_OK = 7;
constexpr int MAPLE_RESPONSE_AGAIN = -4;

struct maple_state_t {};
struct maple_device_t;
struct maple_frame_t {
    int state = MAPLE_FRAME_VACANT;
    int queued = 0;
    int cmd = 0;
    int dst_port = 0;
    int dst_unit = 0;
    int length = 0;
    void *send_buf = nullptr;
    uint8_t *recv_buf = nullptr;
    alignas(32) uint8_t recv_buf_arr[1088]{};
    maple_device_t *dev = nullptr;
    void (*callback)(maple_state_t *, maple_frame_t *) = nullptr;
};
struct maple_device_t {
    int port = 0;
    int unit = 0;
    bool valid = false;
    struct { uint32_t functions = 0; } info;
    maple_frame_t frame;
};
struct maple_response_t { int8_t response; };
maple_device_t *maple_enum_dev(int, int);
maple_device_t *maple_enum_type(int, uint32_t);
void maple_detach_callback(uint32_t, void (*)(maple_device_t *));
int maple_frame_lock(maple_frame_t *);
void maple_frame_init(maple_frame_t *);
void maple_frame_unlock(maple_frame_t *);
int maple_queue_frame(maple_frame_t *);
int maple_queue_remove(maple_frame_t *);
