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
#include <sh4zam/shz_trig.h>

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
constexpr float title_font_height = 133.0f;  // SDL_ttf's title.ttf at 108 px
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
    return file_bytes("/rd/theme/font.map", widths_.data(), widths_.size()) &&
           file_bytes("/rd/theme/title-font.map", digit_widths_.data(), digit_widths_.size()) &&
           prepare_note_mesh("/rd/theme/notes.mesh", regular_note_) &&
           prepare_note_mesh("/rd/theme/hopo.mesh", tap_marker_) &&
           prepare_frets();
}

bool View::prepare_frets() {
    Mesh shape;
    if (!load_mesh("/rd/theme/keys.mesh", shape)) {
        return false;
    }
    const auto count = std::accumulate(shape.begin(), shape.end(), size_t{},
        [](size_t sum, const Face &face) { return sum + face.count; });
    for (unsigned lane = 0; lane < frets_.size(); ++lane) {
        for (unsigned held = 0; held < frets_[lane].size(); ++held) {
            auto &vertices = frets_[lane][held];
            vertices.reserve(count);
            const float depressed = held * 0.5f;
            const auto color = shade(fret_colors[lane], 0.86f, 0.08f + depressed);
            for (const auto &face : shape) {
                for (unsigned i = 0; i < face.count; ++i) {
                    vertices.push_back(mesh_vertex(face.vertices[i], lane_x(lane), depressed / 6, 0,
                                                   color, i + 1 == face.count));
                }
            }
        }
    }
    return true;
}

bool View::prepare_note_mesh(const char *name, NoteMesh &mesh) {
    Mesh shape;
    if (!load_mesh(name, shape)) {
        return false;
    }
    for (const auto &face : shape) {
        for (unsigned i = 0; i < face.count; ++i) {
            const auto &vertex = face.vertices[i];
            const std::array<float, 3> position{vertex.x, vertex.y, vertex.z};
            const auto point = std::find(mesh.points.begin(), mesh.points.end(), position);
            NoteCorner corner{size_t(point - mesh.points.begin()),
                              i + 1 == face.count ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, {}};
            if (point == mesh.points.end()) {
                mesh.points.push_back(position);
            }
            for (unsigned lane = 0; lane < corner.colors.size(); ++lane) {
                corner.colors[lane] = shade(shade(fret_colors[lane], 0.8f, 0.1f), vertex.shade);
            }
            mesh.corners.push_back(corner);
        }
    }
    mesh.projected.resize(mesh.points.size());
    return true;
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
    guitar_time_origin_ms_ = timer_ms_gettime64();
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

void View::material(const pvr_poly_hdr_t &header) {
    if (active_header_ != &header) {
        pvr_prim(&header, sizeof(header));
        active_header_ = &header;
    }
}

void View::sprite(TextureId id, float x, float y, float w, float h, float z,
                  float u0, float v0, float u1, float v1, uint32_t color) {
    const auto &texture = textures_[id];
    material(texture.header);
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
    material(texture.header);
    const auto rotation = shz_sincosf(angle);
    pvr_vertex_t quad[4];
    for (unsigned i = 0; i < 4; ++i) {
        const float u = (i & 1) ? 1.0f : 0.0f;
        const float v = i >= 2 ? 1.0f : 0.0f;
        const float dx = (u - 0.5f) * w;
        const float dy = (v - 0.5f) * h;
        quad[i] = {i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                   x + dx * rotation.cos - dy * rotation.sin, y + dx * rotation.sin + dy * rotation.cos,
                   depth, u, v, color, 0};
    }
    pvr_prim(quad, sizeof(quad));
}


void View::board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                      float u0, float v0, float u1, float v1, uint32_t back, uint32_t front) {
    const auto &texture = textures_[id];
    material(texture.header);
    const pvr_vertex_t vertices[] = {
        project(x0, y, z1, u0, v0, back, false),
        project(x1, y, z1, u1, v0, back, false),
        project(x0, y, z0, u0, v1, front, false),
        project(x1, y, z0, u1, v1, front, true)
    };
    pvr_prim(vertices, sizeof(vertices));
}

void View::text(float x, float y, const char *line, float size, uint32_t color, bool centered,
                float depth_offset) {
    const auto get_index = [](unsigned char c) { return c >= 32 && c < 127 ? c - 32 : '?' - 32; };
    if (centered) {
        for (auto *character = line; *character; ++character) {
            x -= 0.5f * size * widths_[get_index(*character)];
        }
    }

    const float depth = menu_depth + 1 + depth_offset;
    text_line(x + 1.9f, y + 1.9f, line, size, color & 0xff000000, depth);
    text_line(x, y, line, size, color, depth);
}

void View::text_line(float x, float y, const char *line, float size, uint32_t color, float depth) {
    const auto get_index = [](unsigned char c) { return c >= 32 && c < 127 ? c - 32 : '?' - 32; };
    const auto &header = textures_[font].header;
    material(header);
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
        x += size * widths_[index];
    }
}

void View::title_number(float y, const char *digits, float size, uint32_t color) {
    if (y >= 480.0f) {
        return;
    }
    float left = 320.0f;
    for (const char *digit = digits; *digit; ++digit) {
        left -= 0.5f * size * digit_widths_[*digit - '0'];
    }
    const auto line = [&](float x, float top, uint32_t ink) {
        for (const char *digit = digits; *digit; ++digit) {
            const unsigned index = *digit - '0';
            const float width = digit_widths_[index];
            const float u = (index * 48 + 2) / 512.0f;
            sprite(title_font, x, top, width * size, 128.0f * size, menu_depth + 1,
                   u, 0, u + width / 512.0f, 1, ink);
            x += width * size;
        }
    };
    line(left + 1.92f, y + 1.92f, color & 0xff000000);
    line(left, y, color);
}

pvr_vertex_t View::mesh_vertex(const MeshVertex &point, float x, float y, float z,
                               uint32_t color, bool last) {
    return project(x + point.x, y + point.y, z + point.z, 0, 0, shade(color, point.shade), last);
}

void View::note_mesh(NoteMesh &mesh, unsigned lane, float z) {
    material(mesh_header_);
    const float x = lane_x(lane);
    for (size_t i = 0; i < mesh.points.size(); ++i) {
        const auto &point = mesh.points[i];
        mesh.projected[i] = project(x + point[0], point[1], z + point[2], 0, 0, 0, false);
    }
    // Reuse the projected position across faces and preserve per-corner color and strip boundaries.
    for (const auto &corner : mesh.corners) {
        auto *vertex = pvr_dr_target(direct_);
        auto projected = mesh.projected[corner.point];
        projected.flags = corner.flags;
        projected.argb = corner.colors[lane];
        *vertex = projected;
        pvr_dr_commit(vertex);
    }
}

void View::draw_tail_wave(unsigned lane, const Chord &note, int position_ms, float period_ms,
                          float guitar_time_ms) {
    // Keep the local step smaller than a millisecond representable, even late in a song.
    const float start = (note.time_ms - position_ms) + 0.001f * note.start_us_remainder;
    const float remaining = (note.ends[lane] - position_ms) + 0.001f * note.end_us_remainders[lane];
    if (remaining < 0.001f) {
        return;
    }

    // Guitar.renderNotes samples from the tip toward the hit line.
    const float projection = board_length / (5.0f * period_ms);
    const float first_step = remaining * projection * 25.0f;
    const float step_gradient = (10.0f - first_step) / remaining;
    const float x = lane_x(lane);
    const auto color = tint(fret_colors[lane], 0.5f);
    const auto slice = [&](float time, float width) {
        const float z = time * projection;
        const auto center = project(x, 0.0f, z, 0, 0, 0xbfffffff, false);
        const float span = focus * width * center.z;
        auto first_edge = center;
        auto second_edge = center;
        first_edge.x += span;
        second_edge.x -= span;
        first_edge.argb = second_edge.argb = color;
        return std::array<pvr_vertex_t, 3>{first_edge, center, second_edge};
    };

    float time = remaining;
    float step = first_step;
    if (time - step <= 0.0f) {
        return;
    }
    material(glow_header_);
    auto previous = slice(time, 0.0f);
    const float sin_phase = lane - guitar_time_ms * 0.01f + position_ms * 0.03f;
    const float cos_phase = lane + guitar_time_ms * 0.01f + position_ms * 0.02f;
    for (unsigned segment = 0; segment < 512 && time > start; ++segment) {
        const float next_time = time - step;
        if (next_time <= 0.0f || next_time >= time) {
            break;
        }
        time = next_time;
        const float offset = ((time - start) * -0.1f - start) / 64.0f + 0.0001f;
        const float sinc = std::abs(offset) < 0.125f ? 1.0f - offset * offset / 6.0f
                                                     : shz_sinf(offset) / offset;
        const float width = 0.1f * (shz_sinf(sin_phase + time * 0.03f) +
                                    shz_cosf(cos_phase + time * 0.02f)) + 0.1f + 0.2f * sinc;
        auto next = slice(time, width);
        pvr_vertex_t strip[] = {previous[0], next[0], previous[1], next[1], previous[2], next[2]};
        strip[5].flags = PVR_CMD_VERTEX_EOL;
        pvr_prim(strip, sizeof(strip));
        previous = next;
        step = first_step + step_gradient * (remaining - time);
    }
}

void View::world(const Frame &f, const SongChart &song, const Round &round, const Chart &chart,
                 float period_ms, const NoteMeshRange &meshes) {
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
            // Guitar.renderNotes clips a played head only after it crosses the hit line.
            if (z < -1.0f || (taken && z < 0.0f) || meshes.contains(index, lane)) {
                continue;
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

    const auto *held = round.sustaining_chord(chart);
    const auto recent = f.time_ms - round.last_hit_ms;
    const float guitar_time_ms = held ? timer_ms_gettime64() - guitar_time_origin_ms_ : 0.0f;
    for (unsigned lane = 0; lane < 5; ++lane) {
        const bool sustained = held && (held->frets & (1u << lane)) &&
                               held->ends[lane] >= f.time_ms;
        if (sustained) {
            draw_tail_wave(lane, *held, f.time_ms, period_ms, guitar_time_ms);
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
    layer(stage_crowd_left, -0.8f + 0.04f * beat_phase * shz_sinf(matrix_pi * beat_phase),
          0.4f + 0.08f * beat_phase * shz_cosf(matrix_pi * beat_phase), 278, 248);
    layer(stage_crowd_right, 0.8f + 0.02f * right_phase * shz_sinf(2 * matrix_pi * right_phase),
          0.4f + 0.06f * right_phase * shz_cosf(2 * matrix_pi * right_phase), 172, 223);
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
        const float impulse = i == 5 ? 1.0f - phase : 1.0f - shz_cosf(matrix_pi * (1.0f - phase));
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
        material(overlay_header_);
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
    if (frame.screen == Screen::results) {
        const float hidden = 1.0f - std::min(1.0f, frame.results_age_ms / 512.0f);
        // GameResultsScene and SceneClient both increment the background clock.
        const float t = frame.results_age_ms / 2500.0f;
        const float zoom = shz_sinf(t / 8.0f) + 2.0f;
        rotated_sprite(keyboard, 320 + 160 * shz_cosf(t / 2) + 1280 * hidden * hidden,
                       240 - 120 * shz_sinf(t), 958 * zoom, 272 * zoom, t, 0.01f);
        return;
    }
    const float phase = (timer_ms_gettime64() % 251327) / 5000.0f;
    const float zoom = shz_sinf(phase / 8) + 2;
    if (frame.screen == Screen::setlist) {
        rotated_sprite(cassette_bg, 320 + 160 * shz_sinf(phase / 2), 240 - 120 * shz_cosf(phase),
                       350 * zoom, 223 * zoom, phase, 0.01f);
    } else if (frame.screen == Screen::difficulty) {
        rotated_sprite(editor_bg, 320 + 160 * shz_sinf(phase / 2), 240 - 120 * shz_cosf(phase),
                       1457 * zoom, 402 * zoom, phase, 0.01f);
    } else {
        rotated_sprite(keyboard, 320 + 160 * shz_cosf(phase / 2), 240 - 120 * shz_sinf(phase),
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
                word_width += widths_[ch >= 32 && ch < 127 ? ch - 32 : '?' - 32] * scale;
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
        material(overlay_header_);
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
                const float scale = 1.28f * (1.0f + 0.25f * beat_fraction * beat_fraction * beat_fraction);
                std::snprintf(line, sizeof(line), "%d", beat);
                title_number(288 - 0.5f * title_font_height * scale, line, scale, gold);
            }
        }
    }
    switch (frame.screen) {
        case Screen::menu: {
            const float phase = (timer_ms_gettime64() % 251327) / 5000.0f;
            rotated_sprite(logo, 320, 96, 572 * (1 + 0.025f * shz_sinf(phase * 16)),
                           138 * (1 - 0.025f * shz_cosf(phase * 17)), 0, menu_depth);
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
                     color, false, 1.0f);
                transition *= 2.0f;
            }
            break;
        }
        case Screen::results: {
            const unsigned age = frame.results_age_ms;
            const float hidden = 1.0f - std::min(1.0f, age / 512.0f);
            const float slide = hidden * hidden;
            const auto color = tint(yellow, 1.0f - slide);
            const auto anim = [age](unsigned start, unsigned end) {
                return std::min(1.0f, static_cast<float>(std::max(age, start)) / end);
            };
            text(320, (0.05f - slide) * 640, "Song Finished!", main_font, color, true);
            const float rise = anim(0, 1000);
            const auto score = age >= 2000 ? round.score : static_cast<unsigned>(round.score * double(anim(1000, 2000)));
            std::snprintf(line, sizeof(line), "%u", score);
            title_number((1.11f + slide - rise * rise * rise) * 640, line, 1.6f, color);
            if (age > 1000) {
                constexpr float star_width = 0.75f * title_font_height * 0.0017f * 640;
                const auto rating = round.rating(frame.gems);
                const float y = (0.35f + slide) * 640;
                for (unsigned i = 0; i < 5; ++i) {
                    const float travel = 1.0f - anim(1000 + i * 200, 1200 + i * 200);
                    const float x = 320 + star_width * (float(i) - 2.5f) + 64000 * travel * travel;
                    if (x < 640) {
                        const auto id = i < rating ? star : star_off;
                        const float height = star_width * 109.0f / (id == star ? 113.0f : 114.0f);
                        // Font._renderString reverses v for custom glyphs.
                        sprite(id, x + 1.92f, y + 1.92f, star_width, height, menu_depth,
                               0, 1, 1, 0, color & 0xff000000);
                        sprite(id, x, y, star_width, height, menu_depth, 0, 1, 1, 0, color);
                    }
                }
            }
            if (frame.results_actions) {
                item(128, 320, "Replay", 0);
                item(128, 365, "Change Song", 1);
                item(128, 410, "Quit to Main Menu", 2);
            } else if (age > 2500) {
                const auto accuracy = frame.gems ? round.gem_hits * 100 / frame.gems : 0;
                std::snprintf(line, sizeof(line), "Accuracy: %u%%", accuracy);
                text(320, 352, line, main_font, color, true);
                std::snprintf(line, sizeof(line), "Longest note streak: %u", round.best_streak);
                text(320, 396.8f, line, main_font, color, true);
            }
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
    NoteMeshRange meshes;

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    active_header_ = nullptr;
    pvr_dr_init(&direct_);
    if (playing) {
        sprite(stage, 0, 0, 640, 480, 0.01f);
        const auto first_note = std::lower_bound(chart.begin(), chart.end(), frame.time_ms,
            [](const Chord &chord, int time) { return chord.time_ms < time; });
        meshes.first = meshes.last = first_note - chart.begin();
        unsigned precise = 0;
        for (size_t i = meshes.first; i < chart.size() && precise < 14; ++i) {
            const auto &chord = chart[i];
            const float z = board_z(chord.time_ms - frame.time_ms, period_ms);
            if (z > 9.6f) {
                break;
            }
            for (unsigned lane = 0; lane < 5 && precise < 14; ++lane) {
                if (chord.frets & (1u << lane)) {
                    note_mesh(regular_note_, lane, z);
                    if (chord.hopo) {
                        note_mesh(tap_marker_, lane, z);
                    }
                    meshes.last = i;
                    meshes.last_lane_end = lane + 1;
                    ++precise;
                }
            }
        }
        material(mesh_header_);
        for (unsigned lane = 0; lane < frets_.size(); ++lane) {
            const auto &vertices = frets_[lane][(frame.held >> lane) & 1u];
            pvr_prim(vertices.data(), vertices.size() * sizeof(pvr_vertex_t));
        }
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    active_header_ = nullptr;
    if (playing) {
        update_stage(frame.time_ms, period);
        stage_decoration(frame.time_ms);
        world(frame, song, round, chart, period_ms, meshes);
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
