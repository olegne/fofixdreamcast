// SPDX-License-Identifier: GPL-2.0-or-later
#include "view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <arch/timer.h>
#include <dc/video.h>
#include <malloc.h>

namespace dc {
namespace {

constexpr uint32_t white = 0xffffffff;
constexpr uint32_t yellow = 0xffffff80;
constexpr uint32_t gold = 0xffffbf00;
constexpr uint32_t fret_colors[] = {0xff22ff22, 0xffff2222, 0xffffff22, 0xff3333ff, 0xffff22ff};
constexpr const char *difficulties[] = {"Amazing", "Medium", "Easy", "Supaeasy"};
constexpr float board_length = 12.0f;
constexpr float lane_width = 0.8f;
constexpr float left_edge = 2.0f;
constexpr float right_edge = -2.0f;
constexpr float focus = 415.6922f;
constexpr float menu_depth = 5.0f;
constexpr float main_font = 1.1264f;  // default.ttf at 22 px times .002 * 640 / atlas size
constexpr float big_font = 5.5296f;   // title.ttf at 108 px times .002 * 640 / atlas size
constexpr float menu_y = 198.4f;      // Classic menu.pos.y is 0.31 of the logical width
constexpr float menu_line = 45.0f;    // Classic default.ttf's line pitch at 640 × 480
constexpr float matrix_pi = 3.14159265358979323846f;

float lane_x(unsigned lane) {
    return (2.0f - lane) * lane_width;
}

float board_z(int offset_ms, float period_ms) {
    return offset_ms / period_ms * board_length / 5.0f;
}

uint32_t tint(uint32_t color, float opacity) {
    return (color & 0x00ffffff) | (static_cast<uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f) << 24);
}

uint32_t shade(uint32_t color, float level, float light = 0.0f) {
    const auto channel = [level, light](uint32_t c) {
        return static_cast<uint32_t>(std::clamp(level * c + light * 255.0f, 0.0f, 255.0f));
    };
    return (color & 0xff000000) | (channel((color >> 16) & 255) << 16) |
           (channel((color >> 8) & 255) << 8) | channel(color & 255);
}

uint32_t lane_color(float lane) {
    const float position = std::clamp(lane, 0.0f, 4.0f);
    const auto index = static_cast<unsigned>(position);
    const auto low = fret_colors[index];
    const auto high = fret_colors[std::min(index + 1, 4u)];
    const float fraction = position - index;
    const auto channel = [low, high, fraction](unsigned shift) {
        return static_cast<uint32_t>(((low >> shift) & 255) * (1 - fraction) +
                                     ((high >> shift) & 255) * fraction);
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// Classic GuitarSceneClient aims from (0, 3, -3) at (0, 0, 4) with Scene's 60-degree FOV.
pvr_vertex_t project(float x, float y, float z, float u, float v, uint32_t color, bool last,
                     float bias = 0.0f) {
    constexpr float forward_y = -0.3939193f;
    constexpr float forward_z = 0.9191450f;
    const auto ry = y - 3.0f;
    const auto rz = z + 3.0f;
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
    pvr_set_bg_color(0.2f, 0, 0);
    constexpr const char *names[] = {
        "keyboard", "pose", "logo", "cassette-bg", "editor-bg", "cassette-front", "cassette-above", "cassette-below",
        "label-bangbang", "label-defy", "label-twibmpg", "stage",
        "stage-lights-left", "stage-lights-right", "stage-drums", "stage-bass",
        "stage-speakers", "stage-cones", "stage-crowd-left", "stage-crowd-right", "stage-beam",
        "neck", "string", "bar", "tail", "glow", "gem", "tap-gem", "ball-on", "ball-off",
        "star", "star-off", "mult2", "mult3", "mult4", "font", "title-font",
        "tutorial-pose", "tutorial-keyboard", "tutorial-escape"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == texture_count);
    for (unsigned i = 0; i < texture_count; ++i) {
        const auto id = static_cast<TextureId>(i);
        const bool additive = id == glow || id == stage_beam || (id >= mult2 && id <= mult4);
        if (!load_texture(id, names[i], id == stage, id == neck, additive)) {
            return false;
        }
    }
    pvr_poly_cxt_t context;
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&mesh_header_, &context);
    pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&overlay_header_, &context);
    context.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&glow_header_, &context);
    return file_bytes("/rd/theme/font.map", widths_[0].data(), widths_[0].size()) &&
           file_bytes("/rd/theme/title-font.map", widths_[1].data(), widths_[1].size()) &&
           load_mesh("/rd/theme/notes.mesh", regular_note_) &&
           load_mesh("/rd/theme/hopo.mesh", tap_marker_) &&
           load_mesh("/rd/theme/keys.mesh", fret_key_);
}

void View::reset_effects() {
    recent_lanes_.fill(0);
    beat_lanes_.fill(0);
    recent_pick_count_ = 0;
    beat_lane_count_ = 1;
    stage_beat_period_ = 500.0f;
    stage_beat_ = 0;
    last_stage_beat_ms_ = -1;
    last_multiplier_ms_ = -1;
}

void View::record_pick(const Round &round) {
    const auto frets = round.last_hit_frets;
    if (!frets) {
        return;
    }
    update_stage(round.last_hit_ms, stage_beat_period_);
    unsigned lane_total = 0;
    for (unsigned lane = 0; lane < 5; ++lane) {
        if (frets & (1u << lane)) {
            lane_total += lane;
        }
    }
    if (recent_pick_count_ < recent_lanes_.size()) {
        ++recent_pick_count_;
    } else {
        std::rotate(recent_lanes_.begin(), recent_lanes_.begin() + 1, recent_lanes_.end());
    }
    recent_lanes_[recent_pick_count_ - 1] = float(lane_total) / fret_count(frets);
    beat_lanes_[beat_lane_count_ - 1] =
        std::accumulate(recent_lanes_.begin(), recent_lanes_.begin() + recent_pick_count_, 0.0f) /
        recent_pick_count_;
    if (round.streak && round.streak % 10 == 0) {
        last_multiplier_ms_ = round.last_hit_ms;
    }
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

void View::rotated_sprite(TextureId id, float x, float y, float w, float h, float angle,
                          float depth, uint32_t color) {
    const auto &texture = textures_[id];
    pvr_prim(&texture.header, sizeof(texture.header));
    const float cos_a = std::cos(angle);
    const float sin_a = std::sin(angle);
    pvr_vertex_t quad[4];
    for (unsigned i = 0; i < 4; ++i) {
        const float u = (i & 1) ? 1.0f : 0.0f;
        const float v = i >= 2 ? 1.0f : 0.0f;
        const float dx = (u - 0.5f) * w;
        const float dy = (v - 0.5f) * h;
        quad[i] = {i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                   x + dx * cos_a - dy * sin_a, y + dx * sin_a + dy * cos_a,
                   depth, u, v, color, 0};
    }
    pvr_prim(quad, sizeof(quad));
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
                bool title, float depth_offset) {
    const auto &mapping = widths_[title ? 1 : 0];
    const auto get_index = [](unsigned char c) { return c >= 32 && c < 127 ? c - 32 : '?' - 32; };
    if (centered) {
        for (auto *character = line; *character; ++character) {
            x -= 0.5f * size * mapping[get_index(*character)];
        }
    }

    const float depth = menu_depth + 1 + depth_offset;
    text_line(x + 1.9f, y + 1.9f, line, size, color & 0xff000000, title, depth);
    text_line(x, y, line, size, color, title, depth);
}

void View::text_line(float x, float y, const char *line, float size, uint32_t color, bool title, float depth) {
    const auto &mapping = widths_[title ? 1 : 0];
    const auto get_index = [](unsigned char c) { return c >= 32 && c < 127 ? c - 32 : '?' - 32; };
    const auto &header = textures_[title ? title_font : font].header;
    pvr_prim(&header, sizeof(header));
    x -= 2.0f * size;  // left padding inside each atlas cell
    for (; *line; ++line) {
        const auto index = get_index(*line);
        const float u = (index % 16) / 16.0f;
        const float v = (index / 16) * (36.0f / 256.0f);
        const float right = x + 32.0f * size;
        const float bottom = y + 36.0f * size;
        if (*line != ' ') {
            const pvr_vertex_t glyph[] = {
                {PVR_CMD_VERTEX, x, y, depth, u, v, color, 0},
                {PVR_CMD_VERTEX, right, y, depth, u + 1.0f / 16, v, color, 0},
                {PVR_CMD_VERTEX, x, bottom, depth, u, v + 36.0f / 256, color, 0},
                {PVR_CMD_VERTEX_EOL, right, bottom, depth, u + 1.0f / 16, v + 36.0f / 256, color, 0}
            };
            pvr_prim(glyph, sizeof(glyph));
        }
        x += size * mapping[index];
    }
}

void View::mesh(const Mesh &shape, float x, float y, float z, uint32_t color) {
    pvr_prim(&mesh_header_, sizeof(mesh_header_));
    for (const auto &face : shape) {
        pvr_vertex_t strip[4];
        for (unsigned i = 0; i < face.count; ++i) {
            const auto &point = face.vertices[i];
            strip[i] = project(x + point.x, y + point.y, z + point.z, 0, 0,
                               shade(color, point.shade), i + 1 == face.count);
        }
        pvr_prim(strip, face.count * sizeof(pvr_vertex_t));
    }
}

void View::draw_tail_wave(unsigned lane, float end, int time) {
    const float x = lane_x(lane);
    const auto color = fret_colors[lane];
    pvr_prim(&glow_header_, sizeof(glow_header_));
    constexpr unsigned samples = 28;
    pvr_vertex_t edge[2][2];
    for (unsigned i = 0; i <= samples; ++i) {
        const float ratio = float(i) / samples;
        const float z = ratio * end;
        const float phase = time * 0.014f + z * 5.0f + lane;
        const float width = (0.095f + 0.083f * std::sin(phase) + 0.052f * std::cos(phase * 0.43f)) *
                            std::min(1.0f, (1.0f - ratio) * 5.0f);
        const auto inner = project(x, 0.018f, z, 0, 0, 0xbfffffff, false);
        const pvr_vertex_t outer[] = {
            project(x + width, 0.018f, z, 0, 0, tint(color, 0.5f), false),
            project(x - width, 0.018f, z, 0, 0, tint(color, 0.5f), false),
        };
        for (unsigned side = 0; side < 2; ++side) {
            if (i) {
                pvr_vertex_t quad[] = {edge[side][0], edge[side][1], outer[side], inner};
                quad[3].flags = PVR_CMD_VERTEX_EOL;
                pvr_prim(quad, sizeof(quad));
            }
            edge[side][0] = outer[side];
            edge[side][1] = inner;
        }
    }
}

void View::world(const Frame &f, const SongChart &song, const Round &round, const Chart &chart,
                 float period_ms) {
    const float scroll = std::fmod(-0.5f * board_z(f.time_ms, period_ms), 1.0f);
    const auto surface = [&](float start, float end, uint32_t back, uint32_t front) {
        board_quad(neck, left_edge, right_edge, 0.0f, start, end,
                   1, scroll - 0.5f * end, 0, scroll - 0.5f * start, back, front);
    };
    surface(-2.0f, -1.0f, white, 0x00ffffff);
    surface(-1.0f, 8.4f, white, white);
    surface(8.4f, board_length, 0x00ffffff, white);
    for (unsigned lane = 0; lane < 5; ++lane) {
        const auto x = lane_x(lane);
        board_quad(strings, x + 0.035f, x - 0.035f, 0.003f, -2.0f, board_length,
                   0, 0, 1, 1, yellow, yellow);
    }
    const auto start = std::lower_bound(song.beats.begin(), song.beats.end(), f.time_ms - int(period_ms),
                                        [](const Beat &b, int time) { return b.time_ms < time; });
    for (auto it = start; it != song.beats.end(); ++it) {
        const float z = board_z(it->time_ms - f.time_ms, period_ms);
        if (z > board_length) {
            break;
        }
        if (!it->strength || z < -1.0f) {
            continue;
        }
        const auto alpha = 0.75f * std::min({1.0f, 1.0f + z, (board_length - z) / 2.4f});
        board_quad(bars, left_edge, right_edge, 0.005f, z - 0.04f, z + 0.04f, 0, 0, 1, 1,
                   tint(yellow, alpha), tint(yellow, alpha));
    }
    board_quad(bars, left_edge, right_edge, 0.006f, -0.04f, 0.04f, 0, 0, 1, 1,
               tint(gold, 0.5f), tint(gold, 0.5f));

    const auto past = std::lower_bound(chart.begin(), chart.end(), f.time_ms - int(period_ms * 2),
                                       [](const Chord &c, int time) { return c.time_ms < time; });
    unsigned precise = 0;
    for (size_t index = past - chart.begin(); index < chart.size(); ++index) {
        const auto &chord = chart[index];
        const float z = board_z(chord.time_ms - f.time_ms, period_ms);
        if (z > board_length) {
            break;
        }
        const bool taken = round.was_hit(index);
        for (unsigned lane = 0; lane < 5; ++lane) {
            if (!(chord.frets & (1u << lane))) {
                continue;
            }
            const auto x = lane_x(lane);
            const auto end = board_z(chord.ends[lane] - f.time_ms, period_ms);
            const auto color = shade(fret_colors[lane], 0.8f, 0.1f);
            const auto fade = z < 0 ? std::clamp(1.0f + end, 0.0f, 1.0f)
                                    : std::min(1.0f, (board_length - z) / 2.4f);
            if (end > -1.0f && end > z) {
                const auto tail_color = tint(z < 0 && !taken ? 0xff999999 : color,
                                             fade * (z < 0 && !taken ? 0.5f : 1.0f));
                const auto begin = std::max(taken ? 0.0f : -1.0f, z);
                if (end > begin) {
                    board_quad(tail, x + .1f, x - .1f, .008f, begin, std::min(board_length, end),
                               0, 0, 1, 1, tail_color, tail_color);
                }
            }
            if (taken || z < -1.0f) {
                continue;
            }
            const bool mesh_note = z >= 0.0f && z <= 9.6f && precise++ < 14;
            if (mesh_note) {
                continue;  // The opaque geometry was submitted before the translucent board layers.
            }
            const auto p = project(x, 0.0f, z, 0, 0, white, false);
            // The baked sprite covers a 128-pixel crop in note_preview's 896-pixel focal length.
            const float side = 234.0f * p.z;
            sprite(chord.hopo ? tap_gem : gem, p.x - side / 2,
                   p.y - side * (z < 0.0f ? 0.16f : 0.50f), side,
                   side * (z < 0.0f ? 0.30f : 1.0f), p.z + 0.004f, 0, 0, 1, 1,
                   tint(z < 0 ? 0xffaaaaaa : color, fade * (z < 0 ? 0.5f : 1.0f)));
        }
    }

    const auto held_end = std::min(board_length, board_z(round.sustained_until() - f.time_ms, period_ms));
    const auto recent = f.time_ms - round.last_hit_ms;
    for (unsigned lane = 0; lane < 5; ++lane) {
        const bool sustained = (round.sustain_frets & (1u << lane)) && held_end > 0;
        if (sustained) {
            draw_tail_wave(lane, held_end, f.time_ms);
        }
        if (sustained || ((round.last_hit_frets & (1u << lane)) && recent < 170)) {
            const auto p = project(lane_x(lane), 0.10f, 0.0f, 0, 0, white, false, 0.01f);
            const float strength = sustained ? 0.8f : 0.9f * (1.0f - std::max(0, recent) / 170.0f);
            rotated_sprite(glow, p.x, p.y, 104, 78, f.time_ms * 0.01f, p.z,
                           tint(fret_colors[lane], strength));
        }
    }
}

void View::update_stage(int time_ms, float beat_period) {
    stage_beat_period_ = beat_period;
    const int beat = int(time_ms / beat_period);
    if (beat <= stage_beat_) {
        return;
    }
    stage_beat_ = beat;
    last_stage_beat_ms_ = time_ms;
    if (beat_lane_count_ < beat_lanes_.size()) {
        ++beat_lane_count_;
    } else {
        std::rotate(beat_lanes_.begin(), beat_lanes_.begin() + 1, beat_lanes_.end());
    }
    // Stage.triggerBeat copies the previous average into the next beat slot.
    beat_lanes_[beat_lane_count_ - 1] = beat_lanes_[beat_lane_count_ - 2];
}

void View::stage_decoration(int time_ms) {
    const auto pulse = [&](float delay) {
        if (last_stage_beat_ms_ < 0) {
            return 0.0f;
        }
        const float phase = (time_ms - last_stage_beat_ms_) / stage_beat_period_ - delay;
        return 1.0f - std::clamp(phase, 0.0f, 1.0f);
    };
    const auto layer = [&](TextureId id, float px, float py, float w, float h) {
        sprite(id, 320 + px * 320 - w / 2, 240 + py * 240 - h / 2, w, h, 0.015f);
    };
    const float beat_phase = pulse(0.0f);
    const auto beat_scale = [beat_phase](float magnitude) { return 1.0f + magnitude * beat_phase; };
    const float speaker_scale = 1.0f + 0.03f * pulse(0.5f);
    layer(stage_lights_left, -0.65f, -0.8f, 241, 135);
    layer(stage_lights_right, 0.65f, -0.8f, 239, 141);
    layer(stage_drums, -0.7f, 0.0f, 294 * beat_scale(0.01f), 218 * beat_scale(0.01f));
    layer(stage_bass, -0.65f, 0.13f, 91 * beat_scale(0.06f), 94 * beat_scale(0.06f));
    layer(stage_speakers, 0.7f, 0.0f, 213 * speaker_scale, 179 * speaker_scale);
    layer(stage_cones, 0.67f, -0.03f, 118 * beat_scale(0.07f), 125 * beat_scale(0.07f));
    const float right_phase = pulse(0.25f);
    layer(stage_crowd_left, -0.8f + 0.04f * beat_phase * std::sin(matrix_pi * beat_phase),
          0.4f + 0.08f * beat_phase * std::cos(matrix_pi * beat_phase), 278, 248);
    layer(stage_crowd_right, 0.8f + 0.02f * right_phase * std::sin(2 * matrix_pi * right_phase),
          0.4f + 0.06f * right_phase * std::cos(2 * matrix_pi * right_phase), 172, 223);
}

void View::stage_lights(const Frame &frame, const Round &round) {
    // Positions and rotations are the six additive light layers in the default stage.ini.
    constexpr float light_x[] = {-0.87f, -0.69f, -0.48f, 0.83f, 0.64f, 0.46f};
    constexpr float light_y[] = {-0.75f, -0.8f, -0.83f, -0.75f, -0.84f, -0.9f};
    constexpr float light_angle[] = {0.0f, -12.0f, -16.0f, 0.0f, 12.0f, 19.0f};
    constexpr float miss_angle[] = {10.0f, -10.0f, 7.0f, 12.0f, -5.0f, -5.0f};
    constexpr float miss_period[] = {75.0f, 100.0f, 150.0f, 250.0f, 200.0f, 150.0f};
    const float recent = recent_pick_count_ ? 1.0f - std::clamp(
        (frame.time_ms - round.last_hit_ms) / 500.0f, 0.0f, 1.0f) : 0.0f;
    const auto miss = frame.time_ms - round.last_miss_ms;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned channel = i % 3;
        if (beat_lane_count_ < channel + 2) {
            continue;
        }
        const auto color = tint(shade(lane_color(beat_lanes_[channel]), 0.50f + 0.35f * recent), 0.7f);
        const float phase = std::clamp(miss / miss_period[i], 0.0f, 1.0f);
        const float impulse = i == 5 ? 1.0f - phase : 1.0f - std::cos(matrix_pi * (1.0f - phase));
        rotated_sprite(stage_beam, 320 + 320 * light_x[i], 240 + 240 * light_y[i],
                       294 * 3, 263 * 3, (light_angle[i] + miss_angle[i] * impulse) * matrix_pi / 180,
                       0.95f, color);
    }
}

void View::multiplier_effect(const Frame &frame, const Round &round, float song_period) {
    const int elapsed = frame.time_ms - last_multiplier_ms_;
    if (last_multiplier_ms_ < 0 || elapsed <= 0 || elapsed >= 2 * song_period) {
        return;
    }
    const auto multiplier = round.multiplier();
    const bool show_icon = round.streak < 40 && multiplier >= 2;
    constexpr uint32_t flash_colors[] = {white, 0xffff8080, 0xffffff80, 0xff8080ff};
    const auto color = show_icon ? flash_colors[multiplier - 1] : white;
    const float ratio = elapsed / song_period;
    const float pulse = 1.0f - std::abs(1.0f - ratio);
    const float alpha = pulse * pulse - 0.5f;
    if (alpha > 0.0f) {
        pvr_prim(&overlay_header_, sizeof(overlay_header_));
        const auto top = tint(color, alpha);
        // The original flash ends at normalized y=1.0, below the 480px viewport.
        const auto bottom = tint(color, alpha * 0.4375f);
        constexpr float depth = menu_depth + 1.2f;
        const pvr_vertex_t flash[] = {
            {PVR_CMD_VERTEX, 0, 0, depth, 0, 0, top, 0},
            {PVR_CMD_VERTEX, 640, 0, depth, 0, 0, top, 0},
            {PVR_CMD_VERTEX, 0, 480, depth, 0, 0, bottom, 0},
            {PVR_CMD_VERTEX_EOL, 640, 480, depth, 0, 0, bottom, 0}
        };
        pvr_prim(flash, sizeof(flash));
    }
    if (show_icon) {
        const float scale = 0.5f + 0.5f * ratio * ratio * ratio;
        if (scale < 2.0f) {
            const auto icon = static_cast<TextureId>(mult2 + multiplier - 2);
            rotated_sprite(icon, 320, 96, 112 * 2.56f * scale, 117 * 2.56f * scale,
                           0, menu_depth + 1.3f, tint(white, 2.0f - scale));
        }
    }
}

void View::draw_stats(const Frame &frame, const Round &round, const Rules &rules) {
    char value[48];
    std::snprintf(value, sizeof(value), "%u", round.display_score(frame.time_ms, rules));
    text(384, 9, value, main_font, gold);
    std::snprintf(value, sizeof(value), "%ux", round.multiplier());
    text(384, 54, value, main_font, gold);

    constexpr uint32_t balls[] = {0xff80ff80, 0xffff8080, 0xffffff80, 0xff8080ff};
    const unsigned streak = std::min(39u, round.streak);
    const unsigned lit = streak % 10 + 1;
    for (unsigned i = 0; i < 10; ++i) {
        sprite(i < lit ? ball_on : ball_off, 429 + i * 18.5f, 67, 19.5f, 19.5f,
               menu_depth, 0, 0, 1, 1, balls[streak / 10]);
    }
    if (round.streak) {
        std::snprintf(value, sizeof(value), "%u hit", round.streak);
        const float age = std::max(0, frame.time_ms - round.last_hit_ms) / 1000.0f;
        const float size = main_font * (1.0f + 0.25f * std::pow(std::max(0.0f, 1.0f - age), 2));
        text(102, 8, value, size, gold, true);
    } else if (frame.screen == Screen::play) {
        const auto age = frame.time_ms - round.last_miss_ms;
        if (age >= 0 && age < 180) {
            text(64, 12 + 0.0032f * age * age, "Missed!", main_font,
                 tint(gold, 1.0f - age * 0.005f));
        }
    }
}

void View::menu_backdrop(const Frame &frame) {
    const float phase = (timer_ms_gettime64() % 251327) / 5000.0f;
    const float zoom = std::sin(phase / 8) + 2;
    if (frame.screen == Screen::setlist) {
        rotated_sprite(cassette_bg, 320 + 160 * std::sin(phase / 2), 240 - 120 * std::cos(phase),
                       350 * zoom, 223 * zoom, phase, 0.01f);
    } else if (frame.screen == Screen::difficulty) {
        rotated_sprite(editor_bg, 320 + 160 * std::sin(phase / 2), 240 - 120 * std::cos(phase),
                       1457 * zoom, 402 * zoom, phase, 0.01f);
    } else {
        rotated_sprite(keyboard, 320 + 160 * std::cos(phase / 2), 240 - 120 * std::sin(phase),
                       958 * zoom, 272 * zoom, phase, 0.01f);
    }
}

void View::setlist(const Frame &frame, const std::vector<SongEntry> &library) {
    if (library.empty() || frame.selected >= library.size()) {
        return;
    }
    const auto style = [](const SongEntry &song) -> std::pair<uint32_t, TextureId> {
        if (song.midi.find("/bangbang/") != std::string::npos) {
            return {0xffff4411u, label_bangbang};
        }
        if (song.midi.find("/defy/") != std::string::npos) {
            return {white, label_defy};
        }
        if (song.midi.find("/twibmpg/") != std::string::npos) {
            return {0xff555555u, label_twibmpg};
        }
        return {0xffbbbb88u, texture_count};
    };
    const size_t first = frame.selected ? frame.selected - 1 : 0;
    const size_t last = std::min(library.size(), size_t(frame.selected) + 2);
    for (size_t i = first; i < last; ++i) {
        const auto [color, label] = style(library[i]);
        if (i == frame.selected) {
            sprite(cassette_front, 0, 0, 384, 480, menu_depth, 0, 0, 1, 1, color);
            if (label != texture_count) {
                sprite(label, 127, 171, 192, 106, menu_depth + 0.01f);
            }
        } else {
            sprite(i < frame.selected ? cassette_above : cassette_below, 0, 0, 384, 480,
                   menu_depth, 0, 0, 1, 1, color);
        }
    }
    text(384, 32, "Choose a Song", main_font, yellow);
    const auto wrapped = [&](const std::string &label, float y, uint32_t color) {
        std::string line;
        size_t from = 0;
        float width = 0;
        const float scale = main_font * 0.84f;
        while (from < label.size()) {
            const auto next = label.find(' ', from);
            const auto word = label.substr(from, next - from);
            float word_width = 0;
            for (const unsigned char ch : word) {
                word_width += widths_[0][ch >= 32 && ch < 127 ? ch - 32 : '?' - 32] * scale;
            }
            constexpr float gap = 6.0f;
            if (!line.empty() && width + gap + word_width > 217) {
                text(384, y, line.c_str(), scale, color);
                y += 36;
                line.clear();
                width = 0;
            }
            if (!line.empty()) {
                line += ' ';
                width += gap;
            }
            line += word;
            width += word_width;
            from = next == std::string::npos ? label.size() : next + 1;
        }
        if (!line.empty()) {
            text(384, y, line.c_str(), scale, color);
            y += 36;
        }
        return y;
    };
    const auto &song = library[frame.selected];
    const auto after_title = wrapped(song.title, 96, gold);
    wrapped(song.artist, after_title, yellow);
    if (style(song).second != texture_count) {
        for (unsigned i = 0; i < 3; ++i) {
            const float y = 320 + i * 49;
            text(384, y, difficulties[2 - i], 0.63f * main_font, yellow);
            text(480, y, "---", 0.63f * main_font, gold);
            for (unsigned j = 0; j < 5; ++j) {
                sprite(star_off, 384 + j * 16, y + 24, 16, 16, menu_depth, 0, 0, 1, 1, yellow);
            }
            text(480, y + 20, "---", 0.63f * main_font, gold);
        }
    }
}

void View::menu(const Frame &frame, const Round &round,
                const std::vector<SongEntry> &library) {
    const auto item = [&](float x, float y, const char *label, unsigned index) {
        text(x, y, label, main_font, frame.selected == index ? gold : yellow);
    };
    const auto center = [&](float y, const char *label) { text(320, y, label, main_font, yellow, true); };
    const auto dim = [&](float visibility) {
        pvr_prim(&overlay_header_, sizeof(overlay_header_));
        const auto top = tint(0, 0.3f * visibility);
        const auto bottom = tint(0, 0.9f * visibility);
        constexpr float depth = menu_depth + 1.5f;
        const pvr_vertex_t vertices[] = {
            {PVR_CMD_VERTEX, 0, 0, depth, 0, 0, top, 0},
            {PVR_CMD_VERTEX, 640, 0, depth, 0, 0, top, 0},
            {PVR_CMD_VERTEX, 0, 480, depth, 0, 0, bottom, 0},
            {PVR_CMD_VERTEX_EOL, 640, 480, depth, 0, 0, bottom, 0}
        };
        pvr_prim(vertices, sizeof(vertices));
    };
    char line[128];
    if (frame.countdown > 0.0f && (frame.screen == Screen::countdown || frame.screen == Screen::pause)) {
        const float title_alpha = 4.0f - std::abs(4.0f - frame.countdown);
        text(32, 12, frame.title, 0.75f * main_font, tint(yellow, title_alpha));
        text(32, 38, frame.artist, 0.75f * main_font, tint(yellow, title_alpha));
        if (frame.countdown > 1.0f) {
            const float prompt_alpha = 3.0f - std::abs(4.0f - frame.countdown);
            text(320, 192, "Get Ready to Rock", main_font, tint(yellow, prompt_alpha), true);
            if (frame.countdown < 6.0f) {
                const int beat = static_cast<int>(frame.countdown);
                const float beat_fraction = frame.countdown - beat;
                const float scale = big_font * (1.0f + 0.25f * beat_fraction * beat_fraction * beat_fraction);
                std::snprintf(line, sizeof(line), "%d", beat);
                text(320, 288 - 18 * scale, line, scale, gold, true, true);
            }
        }
    }
    switch (frame.screen) {
        case Screen::menu: {
            const float phase = (timer_ms_gettime64() % 251327) / 5000.0f;
            rotated_sprite(logo, 320, 96, 572 * (1 + 0.025f * std::sin(phase * 16)),
                           138 * (1 - 0.025f * std::cos(phase * 17)), 0, menu_depth);
            rotated_sprite(menu_pose, 480, 312, 302 * 0.9f, 413 * 0.9f, 0, menu_depth);
            constexpr const char *options[] = {"Play Game", "Tutorial", "Song Editor >", "Settings >", "Credits", "Quit"};
            for (unsigned i = 0; i < 6; ++i) {
                item(128, menu_y + i * menu_line, options[i], i);
            }
            if (frame.input == InputDevice::none) {
                text(128, 410, "Connect a keyboard", .8f * main_font, yellow);
            }
            break;
        }
        case Screen::settings:
            center(48, "Settings");
            std::snprintf(line, sizeof(line), "A/V Delay: %d ms", frame.delay_ms);
            item(128, 196, line, 0);
            std::snprintf(line, sizeof(line), "Board Speed: %d%%", frame.board_speed);
            item(128, 228, line, 1);
            item(128, 260, "Back", 2);
            break;
        case Screen::setlist:
            setlist(frame, library);
            break;
        case Screen::difficulty:
            text(64, 32, "Choose a difficulty:", main_font, yellow);
            for (int i = 3, row = 0; i >= 0; --i) {
                if (frame.available & (1u << i)) {
                    item(128, menu_y + row * menu_line, difficulties[i], i);
                    ++row;
                }
            }
            break;
        case Screen::countdown:
            break;
        case Screen::pause: {
            const float hidden = 1.0f - frame.pause_visibility;
            dim(1.0f - hidden * hidden);
            constexpr const char *options[] = {
                "Return to Song", "Restart Song", "Change Song", "Settings >", "Quit to Main Menu"
            };
            float transition = hidden * hidden;
            for (unsigned i = 0; i < 5; ++i) {
                const bool selected = frame.selected == i;
                const auto color = tint(selected ? gold : yellow, 1.0f - transition * (selected ? 2 : 1));
                text(128 - 160 * transition, menu_y + i * menu_line, options[i], main_font,
                     color, false, false, 1.0f);
                transition *= 2.0f;
            }
            break;
        }
        case Screen::results: {
            center(24, "Song Finished!");
            std::snprintf(line, sizeof(line), "%u", round.score);
            text(320, 51, line, big_font * 1.1f, gold, true, true);
            const auto rating = round.rating(frame.gems);
            for (unsigned i = 0; i < 5; ++i) {
                sprite(i < rating ? star : star_off, 132 + i * 76, 203, 70, 68, menu_depth,
                       0, 0, 1, 1, yellow);
            }
            const auto accuracy = frame.gems ? (round.gem_hits * 100 / frame.gems) : 0;
            std::snprintf(line, sizeof(line), "Accuracy: %u%%", accuracy);
            center(275, line);
            std::snprintf(line, sizeof(line), "Longest note streak: %u", round.best_streak);
            center(310, line);
            item(128, 350, "Replay", 0);
            item(128, 382, "Change Song", 1);
            item(128, 414, "Quit to Main Menu", 2);
            break;
        }
        case Screen::failed:
            center(132, "Song Finished!");
            item(128, 196, "Replay", 0);
            item(128, 228, "Change Song", 1);
            break;
        case Screen::error:
            center(121, "Load Error");
            text(320, 204, frame.error, 0.72f * main_font, yellow, true);
            center(321, "Esc Back");
            break;
        case Screen::play:
            if (frame.tutorial_picture >= 0 && frame.tutorial_picture < 3) {
                constexpr float widths[] = {302, 509, 265};
                constexpr float heights[] = {413, 424, 274};
                const auto picture = frame.tutorial_picture;
                rotated_sprite(static_cast<TextureId>(tutorial_pose + picture), 320,
                               240 + frame.picture_fade * 480, widths[picture], heights[picture],
                               0, menu_depth);
            }
            if (frame.caption) {
                text(320, 422, frame.caption, main_font, yellow, true);
            }
            break;
    }
}

void View::draw(const Frame &frame, const SongChart &song, const Round &round,
                const std::vector<SongEntry> &library) {
    const bool playing = frame.screen == Screen::play || frame.screen == Screen::pause ||
                         frame.screen == Screen::countdown;
    const float period = song.us_per_beat(frame.time_ms) * 0.001f;
    const float period_ms = period * 100.0f / frame.board_speed;
    const auto &chart = song.guitar[frame.difficulty];

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    if (playing) {
        sprite(stage, 0, 0, 640, 480, 0.01f);
        unsigned precise = 0;
        for (size_t i = round.next; i < chart.size() && precise < 14; ++i) {
            const auto &chord = chart[i];
            const float z = board_z(chord.time_ms - frame.time_ms, period_ms);
            if (z > 9.6f) {
                break;
            }
            if (z < 0.0f || round.was_hit(i)) {
                continue;
            }
            for (unsigned lane = 0; lane < 5 && precise < 14; ++lane) {
                if (chord.frets & (1u << lane)) {
                    const auto color = shade(fret_colors[lane], 0.8f, 0.1f);
                    mesh(regular_note_, lane_x(lane), 0, z, color);
                    if (chord.hopo) {
                        mesh(tap_marker_, lane_x(lane), 0, z, color);
                    }
                    ++precise;
                }
            }
        }
        for (unsigned lane = 0; lane < 5; ++lane) {
            const float depressed = frame.held & (1u << lane) ? 0.5f : 0.0f;
            const auto color = shade(fret_colors[lane], 0.86f, 0.08f + depressed);
            mesh(fret_key_, lane_x(lane), depressed / 6, 0, color);
        }
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    if (playing) {
        update_stage(frame.time_ms, period);
        stage_decoration(frame.time_ms);
        world(frame, song, round, chart, period_ms);
        stage_lights(frame, round);
        draw_stats(frame, round, Rules{60000000.0f / song.initial_us_per_beat});
        multiplier_effect(frame, round, song.initial_us_per_beat * 0.001f);
    } else {
        menu_backdrop(frame);
    }
    menu(frame, round, library);
    pvr_list_finish();
    pvr_scene_finish();
}

} // namespace dc
