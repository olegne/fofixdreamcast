// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include <dc/pvr.h>

namespace dc {

enum class Screen { ready, play, results, error };

struct Frame {
    Screen screen = Screen::ready;
    int time_ms = 0;
    int delay_ms = 0;
    uint8_t held = 0;
    uint8_t flash = 0;
    bool connected = false;
    bool bad_strum = false;
    bool retry_audio = false;
    const char *error = nullptr;
};

class View {
public:
    bool init();
    void draw(const Frame &frame, const Chart &chart, const Round &round);

private:
    void quad(float left_top, float right_top, float top, float left_bottom,
              float right_bottom, float bottom, uint32_t color, uint32_t lower_color,
              float u1 = 0, float v1 = 0, float u2 = 0, float v2 = 0);
    void rect(float x, float y, float w, float h, uint32_t color);
    void panel(float x, float y, float w, float h);
    void text(float x, float y, const char *line, int scale, uint32_t color);
    void centered(float y, const char *line, int scale, uint32_t color);
    void highway(const Frame &frame, const Chart &chart, const Round &round);

    pvr_poly_hdr_t shape_header_{};
    pvr_poly_hdr_t text_header_{};
    float depth_ = 1.0f;
};

} // namespace dc
