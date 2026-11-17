#pragma once
#include <cstdint>
constexpr int MAPLE_PORT_COUNT = 4;
constexpr int MAPLE_UNIT_COUNT = 6;
constexpr uint32_t MAPLE_FUNC_LCD = 0x04000000;
constexpr int MAPLE_FRAME_VACANT = 0;
constexpr int MAPLE_FRAME_UNSENT = 1;
constexpr int MAPLE_FRAME_SENT = 2;
constexpr int MAPLE_FRAME_RESPONDED = 3;
constexpr int MAPLE_RESPONSE_OK = 7;
constexpr int MAPLE_COMMAND_BWRITE = 12;
struct maple_state_t {};
struct maple_frame_t {
    bool queued = false;
    int state = 0, cmd = -1, dst_port = 0, dst_unit = 0, length = 0;
    void *send_buf = nullptr;
    void *recv_buf = nullptr;
    uint8_t recv[16]{};
    void (*callback)(maple_state_t *, maple_frame_t *) = nullptr;
};
struct maple_response_t { int response; };
struct maple_device_t {
    struct { uint32_t functions = 0; } info;
    maple_frame_t frame;
    int port = 0, unit = 0;
};
maple_device_t *maple_enum_type(int, uint32_t);
maple_device_t *maple_enum_dev(int, int);
void maple_detach_callback(uint32_t, void (*)(maple_device_t *));
int maple_queue_frame(maple_frame_t *);
void maple_queue_remove(maple_frame_t *);
int maple_frame_lock(maple_frame_t *);
void maple_frame_init(maple_frame_t *);
void maple_frame_unlock(maple_frame_t *);
