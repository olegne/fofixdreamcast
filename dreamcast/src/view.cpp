// SPDX-License-Identifier: GPL-2.0-or-later
#include "view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dc/video.h>

namespace dc {
namespace {

constexpr uint32_t white = 0xfff0f2fb;
constexpr uint32_t muted = 0xff9da9c2;
constexpr uint32_t accent = 0xff6fe6dd;
constexpr uint32_t warning = 0xffff6882;
constexpr uint32_t border = 0xff263656;
constexpr uint32_t ink = 0xff101a31;
constexpr uint32_t frets[] = {0xff54e385, 0xfffb526d, 0xffffd95e, 0xff5ca9ff, 0xffff9551};
constexpr float target_y = 379.0f;
constexpr int approach_ms = 1700;
constexpr int max_visible_chords = 112;

constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./:%";
constexpr uint8_t glyphs[][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {0,4,4,31,4,4,0}, {0,0,0,31,0,0,0}, {0,0,0,0,0,0,4},
    {1,1,2,4,8,16,16}, {0,4,4,0,4,4,0}, {25,25,2,4,8,19,19}
};
static_assert(sizeof(alphabet) - 1 == sizeof(glyphs) / sizeof(glyphs[0]));

constexpr int font_width = 128;
constexpr int font_height = 32;

float lane_x(float boundary, float y) {
    const float half_width = 62.0f + 0.365f * (y - 94.0f);
    return 320.0f + half_width * (boundary * 0.4f - 1.0f);
}

float note_y(int delta_ms) {
    const float path = 1.0f - static_cast<float>(delta_ms) / approach_ms;
    return 94.0f + (target_y - 94.0f) * path * (0.54f + 0.46f * path);
}

uint32_t darken(uint32_t color) {
    return 0xff000000 | ((color & 0x00fcfcfc) >> 2);
}

} // namespace

bool View::init() {
    vid_set_mode(DM_640x480, PM_RGB565);
    if (pvr_init_defaults() < 0) {
        return false;
    }
    pvr_set_bg_color(0.028f, 0.043f, 0.084f);

    alignas(32) static uint16_t pixels[font_width * font_height]{};
    for (unsigned i = 0; i < sizeof(alphabet) - 1; ++i) {
        const auto x = (i % 16) * 8;
        const auto y = (i / 16) * 8;
        for (unsigned row = 0; row < 7; ++row) {
            for (unsigned col = 0; col < 5; ++col) {
                if (glyphs[i][row] & (1u << (4 - col))) {
                    pixels[(y + row) * font_width + x + col] = 0xffff;
                }
            }
        }
    }
    const auto font = pvr_mem_malloc(sizeof(pixels));
    if (!font) {
        return false;
    }
    pvr_txr_load(pixels, font, sizeof(pixels));

    pvr_poly_cxt_t ctx;
    pvr_poly_cxt_col(&ctx, PVR_LIST_OP_POLY);
    ctx.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&shape_header_, &ctx);
    pvr_poly_cxt_txr(&ctx, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     font_width, font_height, font, PVR_FILTER_NONE);
    ctx.gen.culling = PVR_CULLING_NONE;
    ctx.txr.env = PVR_TXRENV_MODULATEALPHA;
    pvr_poly_compile(&text_header_, &ctx);
    return true;
}

void View::quad(float left_top, float right_top, float top, float left_bottom,
                float right_bottom, float bottom, uint32_t color, uint32_t lower_color,
                float u1, float v1, float u2, float v2) {
    depth_ += 0.015625f;
    const pvr_vertex_t vertices[] = {
        {PVR_CMD_VERTEX, left_top, top, depth_, u1, v1, color, 0},
        {PVR_CMD_VERTEX, right_top, top, depth_, u2, v1, color, 0},
        {PVR_CMD_VERTEX, left_bottom, bottom, depth_, u1, v2, lower_color, 0},
        {PVR_CMD_VERTEX_EOL, right_bottom, bottom, depth_, u2, v2, lower_color, 0}
    };
    pvr_prim(vertices, sizeof(vertices));
}

void View::rect(float x, float y, float w, float h, uint32_t color) {
    quad(x, x + w, y, x, x + w, y + h, color, color);
}

void View::panel(float x, float y, float w, float h) {
    rect(x, y, w, h, border);
    rect(x + 1, y + 1, w - 2, h - 2, ink);
}

void View::text(float x, float y, const char *line, int scale, uint32_t color) {
    for (; *line; ++line, x += 6 * scale) {
        const char *glyph = std::strchr(alphabet, *line);
        if (!glyph) {
            continue;
        }
        const auto i = glyph - alphabet;
        const float u = static_cast<float>((i % 16) * 8);
        const float v = static_cast<float>((i / 16) * 8);
        quad(x, x + 5 * scale, y, x, x + 5 * scale, y + 7 * scale, color, color,
             u / font_width, v / font_height, (u + 5) / font_width, (v + 7) / font_height);
    }
}

void View::centered(float y, const char *line, int scale, uint32_t color) {
    text(320.0f - (static_cast<int>(std::strlen(line)) * 6 - 1) * scale * 0.5f,
         y, line, scale, color);
}

void View::highway(const Frame &frame, const Chart &chart, const Round &round) {
    constexpr float near_y = 407.0f;
    constexpr float far_y = 94.0f;
    for (unsigned lane = 0; lane < 5; ++lane) {
        const auto a = static_cast<float>(lane);
        const bool held = frame.held & (1u << lane);
        quad(lane_x(a, far_y), lane_x(a + 1, far_y) - 1, far_y,
             lane_x(a, near_y), lane_x(a + 1, near_y) - 2, near_y,
             0xff14213e, held ? darken(frets[lane]) : 0xff233559);
    }
    for (const auto edge : {0.0f, 5.0f}) {
        quad(lane_x(edge, far_y) - 1, lane_x(edge, far_y) + 1, far_y,
             lane_x(edge, near_y) - 2, lane_x(edge, near_y) + 2, near_y, border, accent);
    }
    rect(lane_x(0, target_y), target_y - 2, lane_x(5, target_y) - lane_x(0, target_y), 4, white);

    for (unsigned lane = 0; lane < 5; ++lane) {
        const float x = lane_x(static_cast<float>(lane) + 0.5f, target_y);
        const bool lit = (frame.held | frame.flash) & (1u << lane);
        rect(x - 23, target_y - 13, 46, 26, frets[lane]);
        rect(x - 21, target_y - 11, 42, 22, lit ? frets[lane] : ink);
        if (frame.flash & (1u << lane)) {
            rect(x - 19, target_y - 5, 38, 10, white);
        }
    }
    if (frame.screen != Screen::play) {
        return;
    }

    // Draw distant notes first so closer notes retain a clear outline.
    const auto first = chart.begin() + round.next;
    const auto draw_limit = first + std::min<ptrdiff_t>(chart.end() - first, max_visible_chords);
    auto last = std::upper_bound(first, draw_limit, frame.time_ms + approach_ms,
                                 [](int time, const Chord &chord) { return time < chord.time_ms; });
    while (last != first) {
        const auto &chord = *--last;
        const float y = note_y(chord.time_ms - frame.time_ms);
        const float width = 5.0f + (y - far_y) * 0.047f;
        const float height = 3.0f + (y - far_y) * 0.023f;
        for (unsigned lane = 0; lane < 5; ++lane) {
            if (chord.frets & (1u << lane)) {
                const auto x = lane_x(static_cast<float>(lane) + 0.5f, y);
                rect(x - width - 2, y - height - 2, width * 2 + 4, height * 2 + 4, 0xff080d1e);
                rect(x - width, y - height, width * 2, height * 2, frets[lane]);
                rect(x - width * 0.56f, y - height + 1, width * 1.12f, 2, white);
            }
        }
    }
}

void View::draw(const Frame &frame, const Chart &chart, const Round &round) {
    char line[64];
    depth_ = 1.0f;
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&shape_header_, sizeof(shape_header_));
    rect(28, 64, 584, 2, border);
    rect(28, 64, 78, 2, accent);
    highway(frame, chart, round);
    panel(28, 431, 584, 25);
    if (frame.screen == Screen::play) {
        panel(28, 146, 119, 142);
        panel(493, 146, 119, 142);
    } else {
        panel(105, 133, 430, 200);
        rect(106, 134, 428, 3, frame.screen == Screen::error ? warning : accent);
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&text_header_, sizeof(text_header_));
    text(28, 31, "FOFIX", 4, white);
    text(163, 42, "DREAMCAST", 2, accent);
    text(420, 40, "GUITARRA EXPERT", 2, muted);
    constexpr const char *labels[] = {"L", "X", "Y", "B", "R"};
    for (unsigned lane = 0; lane < 5; ++lane) {
        text(lane_x(static_cast<float>(lane) + 0.5f, target_y) - 5, 407, labels[lane], 2, frets[lane]);
    }
    centered(437, "D-PAD CIMA/BAIXO OU A PARA TOCAR", 2, muted);

    switch (frame.screen) {
        case Screen::ready:
            centered(160, "CIRCUITO 01", 3, white);
            centered(199, "FAIXA DE TESTE", 2, muted);
            centered(243, frame.connected ? "START PARA JOGAR" : "CONECTE UM CONTROLE", 2, accent);
            std::snprintf(line, sizeof(line), "SINCRONIA %+d MS", frame.delay_ms);
            centered(283, line, 2, white);
            centered(309, "ESQ/DIR AJUSTA", 2, muted);
            break;
        case Screen::play:
            text(40, 161, "PONTOS", 2, muted);
            std::snprintf(line, sizeof(line), "%u", round.score);
            text(40, 187, line, 2, white);
            std::snprintf(line, sizeof(line), "X%u", round.multiplier());
            text(40, 229, line, 3, accent);
            text(501, 161, "SEQUENCIA", 2, muted);
            std::snprintf(line, sizeof(line), "%u", round.streak);
            text(505, 182, line, 3, white);
            text(505, 224, "ACERTOS", 2, muted);
            std::snprintf(line, sizeof(line), "%u/%u", round.hits, static_cast<unsigned>(chart.size()));
            text(505, 248, line, 2, white);
            if (frame.bad_strum) {
                text(40, 261, "FORA", 2, warning);
            }
            if (!frame.connected) {
                centered(77, "CONECTE UM CONTROLE", 2, warning);
            }
            break;
        case Screen::results:
            centered(157, "FAIXA CONCLUIDA", 3, white);
            std::snprintf(line, sizeof(line), "%u PONTOS", round.score);
            centered(196, line, 2, accent);
            std::snprintf(line, sizeof(line), "%u/%u ACERTOS", round.hits,
                          static_cast<unsigned>(chart.size()));
            centered(225, line, 2, white);
            std::snprintf(line, sizeof(line), "SEQUENCIA %u / FORA %u", round.best_streak,
                          round.overstrums);
            centered(248, line, 2, muted);
            std::snprintf(line, sizeof(line), "%+d MS / ESQ/DIR AJUSTA", frame.delay_ms);
            centered(275, line, 2, muted);
            centered(302, frame.connected ? "START PARA REPETIR" : "CONECTE UM CONTROLE", 2, accent);
            break;
        case Screen::error:
            centered(173, "ERRO NA FAIXA", 3, warning);
            centered(225, frame.error, 1, white);
            if (frame.retry_audio) {
                centered(272, "START PARA TENTAR DE NOVO", 2, muted);
            }
            break;
    }
    pvr_list_finish();
    pvr_scene_finish();
}

} // namespace dc
