// SPDX-License-Identifier: GPL-2.0-or-later
#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dc/video.h>
#include <malloc.h>

namespace dc {
namespace {

constexpr uint32_t white = 0xffffffff;
constexpr uint32_t gray = 0xffaaaaaa;
constexpr uint32_t gold = 0xffffbf00;
constexpr uint32_t fret_colors[] = {0xff732ab0, 0xff4bced3, 0xffe10d1d, 0xff84ed67, 0xffdbea62};
constexpr const char *difficulties[] = {"Expert", "Hard", "Medium", "Easy"};
constexpr float board_length = 9.0f;
constexpr float lane_width = 0.6f;
constexpr float left_edge = 1.5f;
constexpr float right_edge = -1.5f;
constexpr float focus = 415.6922f;
constexpr auto menu_depth = 5.0f;

float lane_x(unsigned lane) {
    return (2.5f - lane) * lane_width;
}

float board_z(int offset_ms, float period_ms) {
    return offset_ms / period_ms * board_length / 5.0f;
}

uint32_t tint(uint32_t color, float opacity) {
    return (color & 0x00ffffff) | (static_cast<uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f) << 24);
}

// MegaLight V4's Rock Band camera in GuitarScene.updateCamera and Scene.render.
pvr_vertex_t project(float x, float y, float z, float u, float v, uint32_t color, bool last,
                     float bias = 0.0f) {
    constexpr float forward_y = -0.4022737145f;
    constexpr float forward_z = 0.9155194759f;
    const auto ry = y - 2.9f;
    const auto rz = z + 2.9f;
    const auto depth = forward_y * ry + forward_z * rz;
    const auto vertical = forward_z * ry - forward_y * rz;
    const auto reciprocal = 1.0f / std::max(0.1f, depth);
    return {last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
            320.0f - focus * x * reciprocal, 240.0f - focus * vertical * reciprocal,
            reciprocal + bias, u, v, color, 0};
}

bool file_bytes(const char *path, void *out, size_t bytes) {
    auto *f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    const bool ok = std::fread(out, 1, bytes, f) == bytes;
    std::fclose(f);
    return ok;
}

} // namespace

bool View::load_texture(TextureId id, const char *name, bool opaque, bool repeat, bool additive) {
    char path[64];
    std::snprintf(path, sizeof(path), "/rd/theme/%s.tex", name);
    auto *file = std::fopen(path, "rb");
    if (!file) {
        std::printf("FoFiXDC: missing texture %s\n", path);
        return false;
    }

    uint16_t sizes[4]{};
    const bool head = std::fread(sizes, sizeof(uint16_t), 4, file) == 4;
    const auto suitable = [](unsigned n) { return n >= 8 && n <= 1024 && !(n & (n - 1)); };
    if (!head || !suitable(sizes[0]) || !suitable(sizes[1]) || sizes[2] > 1) {
        std::fclose(file);
        return false;
    }

    const auto bytes = unsigned(sizes[0]) * sizes[1] * sizeof(uint16_t);
    void *pixels = memalign(32, bytes);
    const bool loaded = pixels && std::fread(pixels, 1, bytes, file) == bytes;
    std::fclose(file);

    auto &texture = textures_[id];
    if (loaded) {
        texture.memory = pvr_mem_malloc(bytes);
    }
    if (!texture.memory) {
        std::free(pixels);
        std::printf("FoFiXDC: texture allocation failed for %s\n", name);
        return false;
    }
    pvr_txr_load(pixels, texture.memory, bytes);
    std::free(pixels);

    pvr_poly_cxt_t ctx;
    const auto format = (sizes[2] ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565) |
                        PVR_TXRFMT_NONTWIDDLED;
    pvr_poly_cxt_txr(&ctx, opaque ? PVR_LIST_OP_POLY : PVR_LIST_TR_POLY, format,
                     sizes[0], sizes[1], texture.memory, PVR_FILTER_BILINEAR);
    ctx.gen.culling = PVR_CULLING_NONE;
    ctx.txr.uv_clamp = repeat ? PVR_UVCLAMP_NONE : PVR_UVCLAMP_UV;
    if (!opaque) {
        ctx.depth.write = PVR_DEPTHWRITE_DISABLE;
        ctx.txr.env = PVR_TXRENV_MODULATEALPHA;
        if (additive) {
            ctx.blend.dst = PVR_BLEND_ONE;
        }
    }
    pvr_poly_compile(&texture.header, &ctx);
    return true;
}

bool View::load_mesh(const char *name, Mesh &mesh) {
    auto *file = std::fopen(name, "rb");
    if (!file) {
        return false;
    }
    uint32_t count;
    if (std::fread(&count, 4, 1, file) != 1 || count > 1024) {
        std::fclose(file);
        return false;
    }
    mesh.resize(count);
    for (auto &face : mesh) {
        if (std::fread(&face.count, 4, 1, file) != 1 || face.count < 3 || face.count > 4 ||
            std::fread(face.vertices.data(), sizeof(MeshVertex), face.count, file) != face.count) {
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);
    return true;
}

bool View::init() {
    vid_set_mode(DM_640x480, PM_RGB565);
    const pvr_init_params_t params{
        {PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0},
        // Draw translucent board layers in submission order so the neck cannot cover the tails.
        768 * 1024, 0, 0, 1, 3, 1
    };
    if (pvr_init(&params) < 0) {
        return false;
    }
    pvr_set_bg_color(0, 0, 0);
    constexpr const char *names[] = {
        "main", "maintext", "version", "options", "stage", "neck", "neck-power", "strings", "bars", "measure",
        "beat", "half", "frets", "notes", "flame", "glow", "tail", "tail-end",
        "tail-held", "tail-held-end", "hud", "meter", "star",
        "star-off", "pause", "rock", "fail", "font", "score-font", "tutorial-pose",
        "tutorial-keyboard", "tutorial-escape", "gem-a", "gem-b", "gem-c",
        "gem-d", "gem-e", "power-a", "power-b", "power-c", "power-d", "power-e"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == texture_count);
    for (unsigned i = 0; i < texture_count; ++i) {
        const auto id = static_cast<TextureId>(i);
        const bool opaque = id == main_bg || id == options_bg || id == stage || id >= gem_a;
        const bool repeat = id == neck || id == power_neck || id == strings || id == bars ||
                            id == tail || id == tail_held;
        if (!load_texture(id, names[i], opaque, repeat, id == flame || id == glow)) {
            return false;
        }
    }
    return file_bytes("/rd/theme/font.map", widths_[0].data(), widths_[0].size()) &&
           file_bytes("/rd/theme/score-font.map", widths_[1].data(), widths_[1].size()) &&
           load_mesh("/rd/theme/notes.mesh", regular_note_) &&
           load_mesh("/rd/theme/hopo.mesh", hopo_note_);
}

void View::sprite(TextureId id, float x, float y, float w, float h, float z,
                  float u0, float v0, float u1, float v1, uint32_t color) {
    const auto &texture = textures_[id];
    pvr_prim(&texture.header, sizeof(texture.header));
    const pvr_vertex_t vertices[] = {
        {PVR_CMD_VERTEX, x,     y,     z, u0, v0, color, 0},
        {PVR_CMD_VERTEX, x + w, y,     z, u1, v0, color, 0},
        {PVR_CMD_VERTEX, x,     y + h, z, u0, v1, color, 0},
        {PVR_CMD_VERTEX_EOL, x + w, y + h, z, u1, v1, color, 0}
    };
    pvr_prim(vertices, sizeof(vertices));
}

void View::board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                      float u0, float v0, float u1, float v1, uint32_t back, uint32_t front) {
    const auto &texture = textures_[id];
    pvr_prim(&texture.header, sizeof(texture.header));
    const pvr_vertex_t vertices[] = {
        project(x0, y, z1, u0, v0, back, false),
        project(x1, y, z1, u1, v0, back, false),
        project(x0, y, z0, u0, v1, front, false),
        project(x1, y, z0, u1, v1, front, true)
    };
    pvr_prim(vertices, sizeof(vertices));
}

void View::text(float x, float y, const char *line, float size, uint32_t color, bool centered,
                bool numeric) {
    const auto &mapping = widths_[numeric ? 1 : 0];
    const auto get_index = [](unsigned char c) { return c >= 32 && c < 127 ? c - 32 : '?' - 32; };
    if (centered) {
        for (auto *character = line; *character; ++character) {
            x -= 0.5f * size * mapping[get_index(*character)];
        }
    }

    const auto &header = textures_[numeric ? score_font : font].header;
    pvr_prim(&header, sizeof(header));
    for (; *line; ++line) {
        const auto index = get_index(*line);
        const float u = (index % 16) / 16.0f;
        const float v = (index / 16) * (36.0f / 256.0f);
        const float right = x + 32.0f * size;
        const float bottom = y + 36.0f * size;
        if (*line != ' ') {
            const pvr_vertex_t glyph[] = {
                {PVR_CMD_VERTEX, x, y, menu_depth + 1, u, v, color, 0},
                {PVR_CMD_VERTEX, right, y, menu_depth + 1, u + 1.0f / 16, v, color, 0},
                {PVR_CMD_VERTEX, x, bottom, menu_depth + 1, u, v + 36.0f / 256, color, 0},
                {PVR_CMD_VERTEX_EOL, right, bottom, menu_depth + 1, u + 1.0f / 16, v + 36.0f / 256, color, 0}
            };
            pvr_prim(glyph, sizeof(glyph));
        }
        x += size * mapping[index];
    }
}

void View::draw_note_mesh(const Chord &chord, unsigned lane, float z) {
    const auto &mesh = chord.hopo ? hopo_note_ : regular_note_;
    const auto bank = chord.power_phrase >= 0 ? power_a : gem_a;
    const auto &header = textures_[bank + lane].header;
    pvr_prim(&header, sizeof(header));
    for (const auto &face : mesh) {
        pvr_vertex_t strip[4];
        for (unsigned i = 0; i < face.count; ++i) {
            const auto &point = face.vertices[i];
            strip[i] = project(lane_x(lane) + point.x, point.y, z + point.z,
                               point.u, point.v, white, i + 1 == face.count);
        }
        pvr_prim(strip, face.count * sizeof(pvr_vertex_t));
    }
}

void View::draw_tail(float x, float start, float end, float scroll, uint32_t color, bool held) {
    constexpr float half_width = 0.4f;
    const auto body = held ? tail_held : tail;
    board_quad(body, x + half_width, x - half_width, 0.027f, start, end,
               1, scroll + (end - start) * 0.375f, 0, scroll, color, color);
    if (end < board_length) {
        const auto cap = held ? tail_held_end : tail_end;
        board_quad(cap, x + half_width, x - half_width, 0.027f, end, end + 0.6f,
                   1, 1, 0, 0, color, color);
    }
}

void View::world(const Frame &f, const SongChart &song, const Round &round, const Chart &chart,
                 float period_ms) {
    const auto distance = board_z(f.time_ms, period_ms);
    const float scroll = std::fmod(distance * 0.125f, 1.0f);
    const float tail_scroll = std::fmod(distance * 0.375f, 1.0f);
    const auto draw_surface = [&](TextureId texture, float y) {
        board_quad(texture, left_edge, right_edge, y, -2.0f, -1.0f,
                    1, scroll - 0.125f, 0, scroll - 0.25f, white, 0x00ffffff);
        board_quad(texture, left_edge, right_edge, y, -1.0f, 6.3f,
                    1, scroll + 0.7875f, 0, scroll - 0.125f);
        board_quad(texture, left_edge, right_edge, y, 6.3f, 9.0f,
                    1, scroll + 1.125f, 0, scroll + 0.7875f, 0x00ffffff, white);
    };
    draw_surface(neck, 0.0f);
    if (round.power_active) {
        draw_surface(power_neck, 0.003f);
    }
    draw_surface(strings, 0.006f);
    for (const float x : {left_edge, right_edge}) {
        board_quad(bars, x + 0.018f, x - 0.018f, 0.008f, -2.0f, board_length, 0, 0, 1, 1,
                    0x00ffffff, white);
    }

    const auto start = std::lower_bound(song.beats.begin(), song.beats.end(), f.time_ms - int(period_ms),
                                         [](const Beat &b, int time) { return b.time_ms < time; });
    unsigned visible_beats = 0;
    for (auto it = start; it != song.beats.end() && visible_beats < 192; ++it, ++visible_beats) {
        const float z = board_z(it->time_ms - f.time_ms, period_ms);
        if (z > board_length) {
            break;
        }
        const auto texture = it->strength == 2 ? measure : (it->strength == 1 ? beat : half);
        board_quad(texture, left_edge, right_edge, 0.012f, z - 0.10f, z + 0.10f, 0, 0, 1, 1,
                    tint(white, (board_length - z) / 1.8f), tint(white, (board_length - z) / 1.8f));
    }

    const auto past = std::lower_bound(chart.begin(), chart.end(), f.time_ms - int(period_ms),
                                        [](const Chord &c, int time) { return c.time_ms < time; });
    const size_t first = std::min(static_cast<size_t>(past - chart.begin()), round.next);
    unsigned visible_gems = 0;
    for (size_t number = first; number < chart.size() && visible_gems < 96; ++number) {
        const auto &chord = chart[number];
        const float z = board_z(chord.time_ms - f.time_ms, period_ms);
        if (z > board_length) {
            break;
        }
        for (unsigned n = 0; n < 5; ++n) {
            if (!(chord.frets & (1u << n))) {
                continue;
            }
            const auto x = lane_x(n);
            const auto end = board_z(chord.ends[n] - 50 - f.time_ms, period_ms);
            const float minimum_tail_ms = song.us_per_beat(chord.time_ms) * 0.00035f;
            if (chord.ends[n] - chord.time_ms > minimum_tail_ms && number >= round.next && end > -1.3f) {
                draw_tail(x, std::max(-1.3f, z), std::min(board_length, end), tail_scroll,
                          tint(fret_colors[n], (board_length - z) / 1.8f), false);
            }
            // The hardware vertex budget reserves the detailed mesh for notes near the strike line.
            if (number >= round.next && (z > 7.4f || visible_gems >= 17)) {
                const float row = (chord.power_phrase >= 0 ? 2.0f : 0.0f) + (chord.hopo ? 1.0f : 0.0f);
                auto p = project(x, 0.08f, z, 0, 0, white, false);
                const float size = 250.0f * p.z;
                sprite(notes, p.x - size / 2, p.y - size * 0.26f, size, size * 0.52f,
                       p.z + 0.002f, n * 0.2f, row / 6, (n + 1) * 0.2f, (row + 1) / 6,
                       tint(white, (board_length - z) / 1.8f));
            }
            ++visible_gems;
        }
    }
    if (round.sustain_frets && round.sustained_until() > f.time_ms + 50) {
        const auto end = std::clamp(board_z(round.sustained_until() - f.time_ms - 50, period_ms),
                                    0.0f, board_length);
        for (unsigned n = 0; n < 5; ++n) {
            if (round.sustain_frets & (1u << n)) {
                draw_tail(lane_x(n), 0.0f, end, tail_scroll, fret_colors[n], true);
            }
        }
    }

    const int since_hit = f.time_ms - round.last_hit_ms;
    for (unsigned n = 0; n < 5; ++n) {
        const auto x = lane_x(n);
        const bool lit = ((round.last_hit_frets & (1u << n)) && since_hit < 170) ||
                         (round.sustain_frets & (1u << n));
        const float row = f.held & (1u << n) ? 1.0f : (lit ? 2.0f : 0.0f);
        board_quad(frets, x + 0.3f, x - 0.3f, 0.0f, -0.25f, 0.25f,
                    n * 0.2f, row / 3.0f, (n + 1) * 0.2f, (row + 1) / 3.0f);
        if (lit) {
            auto p = project(x, 0.05f, 0, 0, 0, white, false);
            sprite(glow, p.x - 44, p.y - 36, 88, 72, p.z + 0.04f, 0, 0, 1, 1,
                   tint(fret_colors[n], 0.86f));
            if (since_hit < 170) {
                const float opacity = 1.0f - since_hit / 170.0f;
                sprite(flame, p.x - 43, p.y - 64, 86, 74, p.z + 0.05f, 0, 0, 1, 1,
                       tint(fret_colors[n], opacity));
            }
        }
    }
}

void View::draw_rockmeter(const Frame &frame, const Round &round, const Chart &chart,
                          const Rules &rules) {
    sprite(hud, 0, 431, 220, 50, menu_depth);
    sprite(hud, 420, 431, 220, 50, menu_depth, 1, 0, 0, 1);
    sprite(hud, 506, 385, 220, 50, menu_depth, 1, 0, 0, 1);
    const float rock_fill = round.rock / 30000.0f;
    const float power_fill = round.power / 100.0f;
    sprite(meter, 0, 165, 30, 267, menu_depth, 0, 0, 1.0f / 3, 1);
    sprite(meter, 0, 165 + 267 * (1 - rock_fill), 30, 267 * rock_fill, menu_depth + 0.01f,
           1.0f / 3, 1 - rock_fill, 2.0f / 3, 1);
    sprite(meter, 25, 214, 28, 218, menu_depth, 0, 0, 1.0f / 3, 1);
    sprite(meter, 25, 214 + 218 * (1 - power_fill), 28, 218 * power_fill, menu_depth + 0.01f,
           2.0f / 3, 1 - power_fill, 1, 1);

    char value[64];
    const auto draw_number = [&](float x, unsigned number, int width) {
        constexpr float scale = 0.79f;
        std::snprintf(value, sizeof(value), "%0*u", width, number);
        const char *first = value;
        while (*first == '0' && first[1]) {
            ++first;
        }
        text(x, 442, value, scale, 0xff999999, false, true);
        text(x + (first - value) * widths_[1]['0' - 32] * scale, 442, first, scale, white,
             false, true);
    };
    text(16, 442, "Score", 0.75f, white, false, true);
    draw_number(80, round.score, 7);
    text(474, 442, "Streak", 0.76f, white, false, true);
    draw_number(575, round.streak, 4);
    const int time = std::max(0, frame.time_ms / 1000);
    std::snprintf(value, sizeof(value), "%d:%02d/%d:%02d", time / 60, time % 60,
                  frame.duration_ms / 60000, (frame.duration_ms / 1000) % 60);
    text(320, 460, value, 0.68f, white, true);
    const unsigned stars = round.rating(frame.gems, chart.size(), frame.time_ms, rules);
    sprite(stars ? star : star_off, 585, 389, 45, 43, menu_depth + 0.1f);
    std::snprintf(value, sizeof(value), "%u", stars);
    text(562, 397, value, 0.8f, white, true, true);
    if (frame.screen == Screen::play && round.streak) {
        constexpr const char *late[] = {"-Excellent!", "Slightly Late", "Late", "Very Late"};
        constexpr const char *early[] = {"+Excellent!", "Slightly Early", "Early", "Very Early"};
        constexpr uint32_t colors[] = {0xff00ff00, 0xffffff00, 0xffffff00, 0xffff0000};
        const auto offset = round.last_hit_offset_ms;
        const auto band = std::min(3u, static_cast<unsigned>(std::abs(offset) * 4 / rules.late_ms));
        const auto *label = offset == 0 ? "Perfect!!" : (offset > 0 ? late[band] : early[band]);
        text(572, 171, label, 0.65f, offset == 0 ? 0xff00ffff : colors[band], true);
    }
}

void View::menu(const Frame &f, const Chart &chart, const Round &round,
                const std::vector<SongEntry> &library) {
    const auto title = [&](const char *name) { text(320, 121, name, 1.35f, white, true); };
    const auto item = [&](float y, const char *label, unsigned index) {
        text(320, y, label, 0.94f, f.selected == index ? gold : white, true);
    };
    const bool keyboard = f.input == InputDevice::keyboard;
    const char *navigation = keyboard ? "Arrows     Enter Select     Esc Back" :
                                       "Up / Down     A Select     B Back";
    char line[80];
    switch (f.screen) {
        case Screen::menu:
            // The original atlas stores an entire menu for each option.
            sprite(main_text, 240, 296, 160, 128, menu_depth, 0.5f, f.selected / 5.0f,
                   1.0f, (f.selected + 1) / 5.0f);
            sprite(version, 160, 448, 320, 17.2f, menu_depth);
            if (f.input == InputDevice::none) {
                text(320, 425, "Connect a keyboard", 0.71f, gray, true);
            }
            break;
        case Screen::settings:
            title("Settings");
            std::snprintf(line, sizeof(line), "A/V Delay   %+d ms", f.delay_ms);
            item(202, line, 0);
            std::snprintf(line, sizeof(line), "Board Speed   %d%%", f.board_speed);
            item(244, line, 1);
            item(286, "Back", 2);
            break;
        case Screen::setlist: {
            title("Choose a Song");
            const auto end = std::min<size_t>(library.size(), std::max(5u, f.selected + 3));
            const auto first = end > 5 ? end - 5 : 0;
            for (size_t i = first; i < end; ++i) {
                std::snprintf(line, sizeof(line), "%.42s", library[i].title.c_str());
                item(185 + (i - first) * 37, line, i);
            }
            const auto &current = library[f.selected];
            text(320, 377, current.artist.c_str(), 0.77f, gray, true);
            text(320, 415, navigation, 0.75f, gray, true);
            break;
        }
        case Screen::difficulty:
            title(f.title);
            for (unsigned i = 0; i < 4; ++i) {
                if (f.available & (1u << i)) {
                    item(194 + (3 - i) * 35, difficulties[i], i);
                }
            }
            text(320, 410, navigation, 0.75f, gray, true);
            break;
        case Screen::countdown:
            text(38, 65, f.title, 0.95f, white);
            text(38, 96, f.artist, 0.72f, gray);
            std::snprintf(line, sizeof(line), "%d", f.countdown);
            text(320, 162, line, 2.55f, white, true, true);
            break;
        case Screen::pause:
            sprite(pause_image, 302, 100, 36, 36, menu_depth + 0.1f);
            title("Paused");
            item(183, "Resume", 0);
            item(218, "Restart Song", 1);
            item(253, "Change Song", 2);
            std::snprintf(line, sizeof(line), "A/V delay   %+d ms", f.delay_ms);
            text(320, 355, line, 0.85f, gray, true);
            break;
        case Screen::results:
            std::snprintf(line, sizeof(line), "%s Finished!", f.title);
            text(32, 23, line, 0.88f, white);
            std::snprintf(line, sizeof(line), "%u", round.score);
            text(320, 72, line, 2.0f, white, true, true);
            for (unsigned i = 0; i < 5; ++i) {
                const bool filled = i < round.rating(f.gems, chart.size(), f.time_ms, Rules{});
                sprite(filled ? star : star_off, 145 + i * 70, 136, 63, 60, menu_depth);
            }
            std::snprintf(line, sizeof(line), "%u / %u Notes Hit", round.hits,
                          static_cast<unsigned>(chart.size()));
            text(320, 233, line, 1.0f, white, true);
            std::snprintf(line, sizeof(line), "%u Note Streak", round.best_streak);
            text(320, 271, line, 1.0f, white, true);
            item(338, "Replay", 0);
            item(373, "Change Song", 1);
            if (round.hits && round.hits == chart.size()) {
                sprite(rock, 241, 416, 158, 39.5f, menu_depth);
            }
            break;
        case Screen::failed:
            sprite(fail, 156, 108, 328, 82, menu_depth);
            item(225, "Retry Song", 0);
            item(265, "Change Song", 1);
            break;
        case Screen::error:
            title("Load Error");
            text(320, 208, f.error, 0.76f, white, true);
            text(320, 321, keyboard ? "Esc Back" : "B Back", 0.82f, gold, true);
            break;
        case Screen::play:
            if (f.tutorial_picture >= 0) {
                constexpr float widths[] = {302, 509, 265};
                constexpr float heights[] = {413, 424, 274};
                const auto picture = f.tutorial_picture;
                const float y = 240 + f.picture_fade * 480;
                sprite(static_cast<TextureId>(tutorial_pose + picture), 320 - widths[picture] / 2,
                       y - heights[picture] / 2, widths[picture], heights[picture], menu_depth + 0.2f);
            }
            if (f.caption) {
                text(321, 447, f.caption, 0.73f, 0xff000000, true);
                text(320, 446, f.caption, 0.73f, white, true);
            }
            break;
    }
}

void View::draw(const Frame &f, const SongChart &song, const Round &round,
                const std::vector<SongEntry> &library) {
    const bool playing = f.screen == Screen::play || f.screen == Screen::pause ||
                         f.screen == Screen::countdown;
    const float bpm = 60000000.0f / song.us_per_beat(f.time_ms);
    constexpr float speeds[] = {226.0f, 256.0f, 286.0f, 306.0f};
    const float period_ms = (speeds[f.difficulty] - std::min(200.0f, bpm) / 10.0f) *
                            100.0f / f.board_speed;
    const auto &chart = song.guitar[f.difficulty];

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    const auto background = playing ? stage : (f.screen == Screen::menu ? main_bg : options_bg);
    sprite(background, 0, 0, 640, 480, 0.01f);
    if (playing) {
        const float limit = 7.4f;
        unsigned precise_notes = 0;
        for (size_t i = round.next; i < chart.size() && precise_notes < 17; ++i) {
            const auto &chord = chart[i];
            const auto z = board_z(chord.time_ms - f.time_ms, period_ms);
            if (z > limit) {
                break;
            }
            for (unsigned lane = 0; lane < 5 && precise_notes < 17; ++lane) {
                if (chord.frets & (1u << lane)) {
                    draw_note_mesh(chord, lane, z);
                    ++precise_notes;
                }
            }
        }
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    if (playing) {
        world(f, song, round, chart, period_ms);
        draw_rockmeter(f, round, chart, Rules{bpm});
    }
    menu(f, chart, round, library);
    pvr_list_finish();
    pvr_scene_finish();
}

} // namespace dc
