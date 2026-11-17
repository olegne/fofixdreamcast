// SPDX-License-Identifier: GPL-2.0-or-later
#include "view.hpp"
#include "chooser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string_view>
#include <arch/timer.h>
#include <dc/video.h>
#include <malloc.h>
#include <sh4zam/shz_trig.h>
#include <zlib.h>

namespace dc {
namespace {

constexpr uint32_t white = 0xffffffff;
constexpr uint32_t fret_colors[] = {0xff22ff22, 0xffff2222, 0xffffff22, 0xff3333ff, 0xffff22ff};
constexpr const char *difficulties[] = {"Amazing", "Medium", "Easy", "Supaeasy"};
constexpr float board_length = 12.0f;
constexpr float lane_width = 0.8f;
constexpr float left_edge = 2.0f;
constexpr float right_edge = -2.0f;
constexpr float focus = 415.6922f;
constexpr float menu_depth = 5.0f;
constexpr float main_font = 1.28f;    // Font.render default scale .002 at 640 logical pixels.
constexpr float menu_y = 198.4f;      // Classic menu.pos.y is 0.31 of the logical width
constexpr float menu_line = 45.0f;    // Classic default.ttf's line pitch at 640 × 480
constexpr float matrix_pi = 3.14159265358979323846f;

float lane_x(unsigned lane) {
    return (2.0f - lane) * lane_width;
}

float note_lift(float hidden, unsigned lane) {
    float lift = hidden;
    for (unsigned i = 0; i < lane; ++i) {
        lift *= hidden;
    }
    return lift;
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

uint32_t lane_color(float lane, float intensity) {
    const float position = std::clamp(lane, 0.0f, 4.0f);
    const auto index = static_cast<unsigned>(position);
    const auto low = fret_colors[index];
    const auto high = fret_colors[std::min(index + 1, 4u)];
    const float fraction = position - index;
    const auto channel = [low, high, fraction, intensity](unsigned shift) {
        const float value = ((low >> shift) & 255) * (1 - fraction) +
                            ((high >> shift) & 255) * fraction;
        return static_cast<uint32_t>(value * intensity + 0.5f);
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
    const bool custom_label = id >= custom_label_a && id <= custom_label_c;
    const auto stem = *name == '/' ? std::string(name) : std::string("/rd/theme/") + name;
    auto path = stem + ".tex";
    auto *file = std::fopen(path.c_str(), "rb");
    const bool compressed = !file;
    if (compressed) {
        path = stem + ".texz";
        file = std::fopen(path.c_str(), "rb");
    }
    if (!file) {
        if (!custom_label) {
            std::printf("FoFiXDC: missing texture %s\n", path.c_str());
        }
        return false;
    }

    uint16_t sizes[4]{};
    const bool head = std::fread(sizes, sizeof(uint16_t), 4, file) == 4;
    const auto suitable = [](unsigned n) { return n >= 8 && n <= 1024 && !(n & (n - 1)); };
    if (!head || !suitable(sizes[0]) || !suitable(sizes[1]) || sizes[2] > 1 ||
        (custom_label && (sizes[0] != 128 || sizes[1] != 128 || sizes[2] != 1))) {
        std::fclose(file);
        return false;
    }

    const auto bytes = unsigned(sizes[0]) * sizes[1] * sizeof(uint16_t);
    void *pixels = memalign(32, bytes);
    bool loaded = false;
    if (pixels && compressed && std::fseek(file, 0, SEEK_END) == 0) {
        const long total = std::ftell(file);
        const size_t header_bytes = sizeof(sizes);
        if (total > long(header_bytes) && total <= long(header_bytes + compressBound(bytes)) &&
            std::fseek(file, header_bytes, SEEK_SET) == 0) {
            std::vector<uint8_t> packed(total - header_bytes);
            uLongf actual = bytes;
            loaded = std::fread(packed.data(), 1, packed.size(), file) == packed.size() &&
                     uncompress(static_cast<Bytef *>(pixels), &actual, packed.data(), packed.size()) == Z_OK &&
                     actual == bytes;
        }
    } else if (pixels && !compressed) {
        loaded = std::fread(pixels, 1, bytes, file) == bytes;
    }
    std::fclose(file);

    auto &texture = textures_[id];
    if (loaded && !texture.memory) {
        texture.memory = pvr_mem_malloc(bytes);
        texture.bytes = bytes;
    }
    if (!loaded || !texture.memory || bytes != texture.bytes) {
        std::free(pixels);
        std::printf("FoFiXDC: texture allocation failed for %s\n", name);
        return false;
    }
    pvr_txr_load(pixels, texture.memory, bytes);
    if (id == bars) {
        const auto *texels = static_cast<const uint16_t *>(pixels);
        unsigned alpha = 0;
        for (unsigned row = 0; row < sizes[1]; ++row) {
            alpha += texels[row * sizes[0] + sizes[0] / 2] >> 12;
        }
        bar_coverage_ = float(alpha) / (15u * sizes[1]);
    }
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
        if (id == glow) {
            ctx.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        }
        ctx.txr.env = PVR_TXRENV_MODULATEALPHA;
        if (additive) {
            ctx.blend.dst = PVR_BLEND_ONE;
        }
    }
    pvr_poly_compile(&texture.header, &ctx);
    if (id == stage) {
        // Both headers refer to the same RGB565 backdrop in VRAM.
        pvr_poly_cxt_txr(&ctx, PVR_LIST_TR_POLY, format, sizes[0], sizes[1],
                         texture.memory, PVR_FILTER_BILINEAR);
        ctx.gen.culling = PVR_CULLING_NONE;
        ctx.depth.write = PVR_DEPTHWRITE_DISABLE;
        ctx.txr.env = PVR_TXRENV_MODULATEALPHA;
        ctx.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&textures_[stage_fade].header, &ctx);
    }
    return true;
}

template <typename Shape>
bool View::load_mesh(const char *name, Shape &mesh) {
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
            std::fread(face.vertices.data(), sizeof(face.vertices[0]), face.count, file) != face.count) {
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);
    return true;
}

bool View::init(const VideoSettings &video) {
    video_ = video;
    render_y_scale_ = video.low_resolution ? 0.5f : 1.0f;
    render_x_scale_ = video.supersampled() ? 1.0f : render_y_scale_;
    vid_set_mode(video.low_resolution ? DM_320x240 : DM_640x480, PM_RGB565);
    const pvr_init_params_t params{
        {PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0},
        // Draw translucent board layers in submission order so the neck cannot cover the tails.
        768 * 1024, 0, video.supersampled(), 1, 3, 1
    };
    if (pvr_init(&params) < 0) {
        return false;
    }
    pvr_set_bg_color(0.2f, 0, 0);
    constexpr const char *names[] = {
        "keyboard", "pose", "logo", "cassette-bg", "editor-bg",
        "label-bangbang", "label-defy", "label-twibmpg", "stage",
        "stage-lights-left", "stage-lights-right", "stage-drums", "stage-bass",
        "stage-speakers", "stage-cones", "stage-crowd-left", "stage-crowd-right", "stage-beam",
        "neck", "string", "bar", "tail", "glow", "ball-on", "ball-off",
        "star", "star-off", "mult2", "mult3", "mult4", "font", "title-font",
        "tutorial-pose", "tutorial-keyboard", "tutorial-escape"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == font_intl);
    for (unsigned i = 0; i < font_intl; ++i) {
        const auto id = static_cast<TextureId>(i);
        const bool additive = id == glow || id == stage_beam || (id >= mult2 && id <= mult4);
        const bool repeat = id == neck || (id >= label_bangbang && id <= label_twibmpg);
        if (!load_texture(id, names[i], id == stage, repeat, additive)) {
            return false;
        }
    }
    pvr_poly_cxt_t context;
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&mesh_header_, &context);
    pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&faded_note_header_, &context);
    context.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&overlay_header_, &context);
    context.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&glow_header_, &context);
    return file_bytes("/rd/theme/font.map", widths_.data(), widths_.size()) &&
           file_bytes("/rd/theme/title-font.map", digit_widths_.data(), digit_widths_.size()) &&
           prepare_international_font() &&
           prepare_cassette_mesh("/rd/theme/cassette-shell.mesh", cassette_shell_) &&
           prepare_cassette_mesh("/rd/theme/cassette-reels.mesh", cassette_reels_) &&
           prepare_cassette_mesh("/rd/theme/library-shell.mesh", library_shell_) &&
           prepare_cassette_mesh("/rd/theme/library-details.mesh", library_details_) &&
           load_mesh("/rd/theme/cassette-sticker.mesh", cassette_sticker_) &&
           load_mesh("/rd/theme/library-sticker.mesh", library_sticker_) &&
           prepare_note_mesh("/rd/theme/notes.mesh", regular_note_) &&
           prepare_note_mesh("/rd/theme/hopo.mesh", tap_marker_) &&
           prepare_frets();
}

bool View::configure_video(VideoSettings &video) {
    if (video.low_resolution == video_.low_resolution && video.supersampled() == video_.supersampled()) {
        return true;
    }
    auto previous = video_;
    previous.half_refresh = video.half_refresh;
    pvr_wait_ready();
    pvr_wait_render_done();
    pvr_shutdown();
    *this = View{};
    if (init(video)) {
        return true;
    }
    std::puts("FoFDC: restoring the previous video mode");
    pvr_shutdown();
    *this = View{};
    video = previous;
    return init(video);
}

bool View::prepare_frets() {
    if (!load_mesh("/rd/theme/keys.mesh", fret_model_)) {
        return false;
    }
    const auto count = std::accumulate(fret_model_.begin(), fret_model_.end(), size_t{},
        [](size_t sum, const Face &face) { return sum + face.count; });
    releasing_fret_.reserve(count);
    constexpr float weights[] = {0.0f, 0.5f, 0.75f};
    for (unsigned lane = 0; lane < fret_vertices_.size(); ++lane) {
        for (unsigned i = 0; i < std::size(weights); ++i) {
            auto &vertices = fret_vertices_[lane][i];
            vertices.reserve(count);
            fret_vertices(lane, weights[i], vertices);
        }
    }
    return true;
}

void View::fret_vertices(unsigned lane, float weight, std::vector<pvr_vertex_t> &vertices,
                         float visibility) const {
    const float x = lane_x(lane);
    constexpr float exits[] = {7.0f, 10.5f, 15.75f, 23.625f, 35.4375f};
    const float y = weight / 6.0f + exits[lane] * (1.0f - visibility);
    vertices.clear();
    for (const auto &face : fret_model_) {
        for (unsigned i = 0; i < face.count; ++i) {
            const auto &point = face.vertices[i];
            const float light = point.shade;
            const auto color = tint(shade(fret_colors[lane], 0.8f * light, (0.1f + weight) * light),
                                    visibility);
            vertices.push_back(board_vertex(x + point.x, y + point.y, point.z, 0, 0,
                                            color, i + 1 == face.count));
        }
    }
}

void View::draw_frets(bool strumming, float visibility, bool transparent) {
    const bool settled = visibility == 1.0f;
    material(transparent ? overlay_header_ : mesh_header_);
    for (unsigned lane = 0; lane < fret_vertices_.size(); ++lane) {
        const float weight = fret_weights_[lane];
        if (settled && !lefty_ && (weight == 0.0f || weight == 0.5f)) {
            const auto &vertices = fret_vertices_[lane][weight == 0.0f ? 0 : (strumming ? 2 : 1)];
            submit(vertices.data(), vertices.size());
        } else {
            const float depressed = weight + (strumming && weight > 0.0f ? 0.25f : 0.0f);
            fret_vertices(lane, depressed, releasing_fret_, visibility);
            submit(releasing_fret_.data(), releasing_fret_.size());
        }
    }
}

void View::update_frets(const Frame &frame, const Round &round) {
    const auto now = timer_ms_gettime64();
    const float ticks = static_cast<float>(now - fret_time_ms_);
    fret_time_ms_ = now;
    for (unsigned lane = 0; lane < fret_weights_.size(); ++lane) {
        const auto bit = 1u << lane;
        fret_weights_[lane] = frame.held & bit ? 0.5f :
                              std::max(0.0f, fret_weights_[lane] - ticks / 64.0f);
        auto &activity = fret_activities_[lane];
        activity = round.sustain_frets & bit ? std::min(1.0f, activity + ticks / 32.0f) :
                                             std::max(0.0f, activity - ticks / 64.0f);
    }
}

bool View::prepare_note_mesh(const char *name, NoteMesh &mesh) {
    auto *file = std::fopen(name, "rb");
    if (!file) {
        return false;
    }
    uint32_t count = 0;
    const bool header_ok = std::fread(&count, sizeof(count), 1, file) == 1 && count > 0 && count <= 1024;
    bool ok = header_ok;
    for (unsigned strip = 0; ok && strip < count; ++strip) {
        std::array<MeshVertex, 64> vertices;
        uint32_t length;
        ok = std::fread(&length, sizeof(length), 1, file) == 1 &&
             length >= 3 && length <= vertices.size() &&
             std::fread(vertices.data(), sizeof(MeshVertex), length, file) == length;
        if (!ok) {
            break;
        }
        for (unsigned i = 0; i < length; ++i) {
            const auto &vertex = vertices[i];
            const std::array<float, 3> position{vertex.x, vertex.y, vertex.z};
            const auto point = std::find(mesh.points.begin(), mesh.points.end(), position);
            NoteCorner corner{size_t(point - mesh.points.begin()),
                              i + 1 == length ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, {}};
            if (point == mesh.points.end()) {
                mesh.points.push_back(position);
            }
            for (unsigned lane = 0; lane < 5; ++lane) {
                corner.colors[lane] = shade(fret_colors[lane], 0.8f * vertex.shade, 0.1f * vertex.shade);
            }
            corner.colors[5] = shade(0xff999999, vertex.shade);
            mesh.corners.push_back(corner);
        }
    }
    std::fclose(file);
    mesh.projected.resize(mesh.points.size());
    return ok;
}

void View::reset_effects(bool same_scene) {
    recent_lanes_.fill(0);
    beat_lanes_.fill(0);
    recent_pick_count_ = 0;
    beat_lane_count_ = 1;
    guitar_clock_.restart(same_scene);
    last_multiplier_ms_ = -1;
    if (!same_scene) {
        stage_transition_ = {};
        guitar_time_origin_ms_ = fret_time_ms_ = timer_ms_gettime64();
        fret_weights_.fill(0);
        fret_activities_.fill(0);
    }
}

void View::record_pick(const Round &round, bool after_stage_update) {
    const auto frets = round.last_hit_frets;
    if (!frets) {
        return;
    }
    // Keyboard callbacks precede Stage.run. The delayed-pick timeout runs after it.
    if (after_stage_update) {
        update_stage_beat(round.last_hit_ms);
    }
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

void View::scale_vertex(pvr_vertex_t &vertex) const {
    if (video_.low_resolution) {
        vertex.x *= render_x_scale_;
        vertex.y *= render_y_scale_;
    }
}

void View::submit(const pvr_vertex_t *vertices, size_t count) {
    if (!video_.low_resolution) {
        pvr_prim(vertices, count * sizeof(*vertices));
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        auto vertex = vertices[i];
        scale_vertex(vertex);
        auto *target = pvr_dr_target(direct_);
        *target = vertex;
        pvr_dr_commit(target);
    }
}

bool View::render_slot(bool half_refresh) {
    if (!half_refresh) {
        render_capped_ = false;
        return true;
    }
    const auto vblank = static_cast<unsigned>(pvr_get_vbl_count());
    if (render_capped_ && vblank - render_vblank_ < 2) {
        do {
            thd_sleep(1);
        } while (static_cast<unsigned>(pvr_get_vbl_count()) == vblank);
        return false;
    }
    render_vblank_ = vblank;
    render_capped_ = true;
    return true;
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
    submit(vertices, std::size(vertices));
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
    submit(quad, std::size(quad));
}


void View::board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                      float u0, float v0, float u1, float v1, uint32_t back, uint32_t front) {
    const auto &texture = textures_[id];
    material(texture.header);
    const pvr_vertex_t vertices[] = {
        board_vertex(x0, y, z1, u0, v0, back, false),
        board_vertex(x1, y, z1, u1, v0, back, false),
        board_vertex(x0, y, z0, u0, v1, front, false),
        board_vertex(x1, y, z0, u1, v1, front, true)
    };
    submit(vertices, std::size(vertices));
}

void View::draw_board_bar(float y, float z, float angle, float opacity, uint32_t color) {
    if (opacity <= 0.0f) {
        return;
    }
    constexpr float half_length = 0.04f;
    const auto transform = shz_sincosf(angle);
    const auto corner = [&](unsigned side, float position) {
        const float x = side ? right_edge : left_edge;
        return board_vertex(x * transform.cos - y * transform.sin, x * transform.sin + y * transform.cos,
                            position, float(side), 0.5f, color, false);
    };
    const pvr_vertex_t near_edge[] = {corner(0, z - half_length), corner(1, z - half_length)};
    const pvr_vertex_t far_edge[] = {corner(0, z + half_length), corner(1, z + half_length)};
    float normal_x = 0.0f;
    float normal_y = 1.0f;
    if (angle != 0.0f) {
        const float dx = near_edge[1].x - near_edge[0].x + far_edge[1].x - far_edge[0].x;
        const float dy = near_edge[1].y - near_edge[0].y + far_edge[1].y - far_edge[0].y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0f) {
            return;
        }
        normal_x = -dy / length;
        normal_y = dx / length;
    }

    // Sample the classic texture across the neck. Vertically, a bar this
    // thin needs pixel coverage: point samples can fall entirely in its
    // transparent margins. Integrate a stripe with the texture's alpha area.
    const auto clear = color & 0x00ffffff;
    pvr_vertex_t vertices[8];
    for (unsigned side = 0; side < 2; ++side) {
        const auto &top = far_edge[side];
        const auto &bottom = near_edge[side];
        const float dx = bottom.x - top.x;
        const float dy = bottom.y - top.y;
        const float footprint = std::abs(dx * normal_x + dy * normal_y) * render_y_scale_;
        if (footprint < 0.001f) {
            return;
        }
        // Match the phase-averaged energy from the original renderer's
        // trilinear bar texture and 4× MSAA at 640×480. The coverage stays
        // continuous where the reference also varies with sample positions.
        const float filtered = footprint / (footprint + 0.73f);
        const float thickness = footprint * bar_coverage_;
        const float outer = (thickness + 1.0f) * 0.5f / footprint;
        const float inner = std::abs(thickness - 1.0f) * 0.5f / footprint;
        const float ratios[] = {0.5f - outer, 0.5f - inner, 0.5f + inner, 0.5f + outer};
        const auto solid = tint(color, opacity * filtered * std::min(1.0f, thickness));
        for (unsigned row = 0; row < 4; ++row) {
            const float ratio = ratios[row];
            vertices[2 * row + side] = {PVR_CMD_VERTEX, top.x + ratio * dx, top.y + ratio * dy,
                                        top.z + ratio * (bottom.z - top.z), float(side), 0.5f,
                                        row == 0 || row == 3 ? clear : solid, 0};
        }
    }
    vertices[7].flags = PVR_CMD_VERTEX_EOL;
    material(textures_[bars].header);
    submit(vertices, std::size(vertices));
}

pvr_vertex_t View::board_vertex(float x, float y, float z, float u, float v, uint32_t color,
                                bool last, float bias) const {
    return project(lefty_ ? -x : x, y, z, u, v, color, last, bias);
}

void View::apply_mods(unsigned flags) {
    flags &= 3;
    if (flags == active_mods_) {
        return;
    }
    const bool chilly = flags & 1;
    const bool light = flags & 2;
    const auto changed = flags ^ active_mods_;
    struct ModImage {
        TextureId id;
        const char *name;
        unsigned sources;
    };
    constexpr ModImage sources[] = {
        {logo, "logo", 3}, {neck, "neck", 3}, {stage, "stage", 1},
        {keyboard, "keyboard", 2}, {menu_pose, "pose", 2}, {cassette_bg, "cassette-bg", 2},
        {editor_bg, "editor-bg", 2}, {glow, "glow", 2}, {ball_on, "ball-on", 2},
        {ball_off, "ball-off", 2}, {star, "star", 2}, {star_off, "star-off", 2},
        {mult2, "mult2", 2}, {mult3, "mult3", 2}, {mult4, "mult4", 2},
    };
    for (const auto &image : sources) {
        if (!(changed & image.sources)) {
            continue;
        }
        const auto id = image.id;
        const char *prefix = light && (image.sources & 2) ? "light-" :
                             chilly && (image.sources & 1) ? "chilly-" : "";
        char filename[48];
        std::snprintf(filename, sizeof(filename), "%s%s", prefix, image.name);
        const bool additive = id == glow || (id >= mult2 && id <= mult4);
        load_texture(id, filename, id == stage, id == neck, additive);
    }
    yellow = chilly ? 0xff80ffff : 0xffffff80;
    gold = chilly ? 0xff00bfff : 0xffffbf00;
    pvr_set_bg_color(chilly ? 0.0f : 0.2f, 0.0f, chilly ? 0.2f : 0.0f);
    active_mods_ = flags;
}

bool View::prepare_international_font() {
    static_assert(sizeof(Glyph) == 12);
    auto *file = std::fopen("/rd/theme/font-intl.map", "rb");
    if (!file) {
        return false;
    }
    Glyph glyph;
    while (std::fread(&glyph, sizeof(glyph), 1, file) == 1) {
        international_glyphs_.push_back(glyph);
    }
    std::fclose(file);
    return !international_glyphs_.empty() && load_texture(font_intl, "font-intl", false);
}

void View::select_language(unsigned index) {
    language_.select(index);
    if (!language_.international() || international_title_loaded_) {
        return;
    }
    international_title_loaded_ = load_texture(title_intl, "title-intl", false) &&
        file_bytes("/rd/theme/title-intl.map", international_digits_.data(), international_digits_.size());
}

static uint32_t next_codepoint(const char *&text) {
    const auto head = static_cast<unsigned char>(*text++);
    if (head < 0x80) {
        return head;
    }
    const unsigned follow = head >= 0xf0 && head <= 0xf4 ? 3 :
                            head >= 0xe0 && head < 0xf0 ? 2 :
                            head >= 0xc2 && head < 0xe0 ? 1 : 0;
    if (!follow) {
        return '?';
    }
    uint32_t cp = head & (0x7f >> (follow + 1));
    for (unsigned i = 0; i < follow; ++i) {
        const auto byte = static_cast<unsigned char>(*text);
        if ((byte & 0xc0) != 0x80) {
            return '?';
        }
        cp = (cp << 6) | (byte & 0x3f);
        ++text;
    }
    return cp;
}

View::Glyph View::glyph_for(uint32_t cp, bool &intl) const {
    intl = language_.international() || cp >= 127;
    if (intl) {
        const auto find = [&](uint32_t value) {
            return std::lower_bound(international_glyphs_.begin(), international_glyphs_.end(), value,
                [](const Glyph &glyph, uint32_t target) { return glyph.codepoint < target; });
        };
        const auto glyph = find(cp);
        if (glyph != international_glyphs_.end() && glyph->codepoint == cp) {
            return *glyph;
        }
        if (language_.international()) {
            return *find('?');
        }
        intl = false;
    }
    const auto i = cp >= 32 && cp < 127 ? cp - 32 : '?' - 32;
    return {cp, static_cast<uint16_t>((i % 16) * 32), static_cast<uint16_t>((i / 16) * 40),
            static_cast<uint8_t>(widths_[i] + 4), widths_[i]};
}

float View::font_height() const {
    return language_.international() ? 27.0f : 35.0f;
}

float View::big_font_height() const {
    return language_.international() && international_title_loaded_ ? 127.0f : 133.0f;
}

unsigned View::text_width(const char *line) const {
    unsigned width = 0;
    while (*line) {
        bool intl;
        width += glyph_for(next_codepoint(line), intl).advance;
    }
    return width;
}

void View::text(float x, float y, const char *line, float size, uint32_t color, bool centered,
                float depth_offset, float angle) {
    size *= text_scale_;
    if (centered) {
        x -= 0.5f * size * text_width(line);
    }

    const float depth = menu_depth + 1 + depth_offset;
    const auto rotation = angle ? shz_sincosf(angle) : shz_sincos_t{0.0f, 1.0f};
    text_line(x + 1.92f, y + 1.92f, line, size, color & 0xff000000, depth,
              rotation.sin, rotation.cos);
    text_line(x, y, line, size, color, depth, rotation.sin, rotation.cos);
}

void View::text_line(float x, float y, const char *line, float size, uint32_t color,
                     float depth, float sin, float cos) {
    const auto render = [&](uint32_t cp) {
        bool intl;
        const auto glyph = glyph_for(cp, intl);
        if (cp != ' ') {
            const auto id = intl ? font_intl : font;
            const float row_size = intl ? 512.0f : 256.0f;
            const float glyph_height = intl ? 31.0f : 39.0f;
            const float left = x - 2.0f * size;
            // Accented text may use the international atlas inside an English line.
            const float top = y + (intl && !language_.international() ? 2.0f : -2.0f) * size;
            const float u0 = glyph.x / 512.0f;
            const float v0 = glyph.y / row_size;
            const float u1 = (glyph.x + glyph.width) / 512.0f;
            const float v1 = (glyph.y + glyph_height) / row_size;
            const float width = glyph.width * size;
            const float height = glyph_height * size;
            if (sin == 0.0f) {
                sprite(id, left, top, width, height, depth, u0, v0, u1, v1, color);
            } else {
                material(textures_[id].header);
                const auto vertex = [&](float a, float b, float u, float v, uint32_t flags) {
                    return pvr_vertex_t{flags, a * cos - b * sin, a * sin + b * cos,
                                        depth, u, v, color, 0};
                };
                const pvr_vertex_t quad[] = {
                    vertex(left, top, u0, v0, PVR_CMD_VERTEX),
                    vertex(left + width, top, u1, v0, PVR_CMD_VERTEX),
                    vertex(left, top + height, u0, v1, PVR_CMD_VERTEX),
                    vertex(left + width, top + height, u1, v1, PVR_CMD_VERTEX_EOL)
                };
                submit(quad, std::size(quad));
            }
        }
        x += size * glyph.advance;
    };
    if (language_.reversed()) {
        std::vector<uint32_t> letters;
        while (*line) {
            letters.push_back(next_codepoint(line));
        }
        for (auto it = letters.rbegin(); it != letters.rend(); ++it) {
            render(*it);
        }
    } else {
        while (*line) {
            render(next_codepoint(line));
        }
    }
}

void View::title_number(float y, const char *digits, float size, uint32_t color, float centering_size) {
    if (y >= 480.0f) {
        return;
    }
    const float center = (centering_size > 0 ? centering_size : size) * text_scale_;
    size *= text_scale_;
    const bool intl = language_.international() && international_title_loaded_;
    const auto &widths = intl ? international_digits_ : digit_widths_;
    const float atlas = intl ? 1024.0f : 512.0f;
    const unsigned cell = intl ? 96 : 48;
    float left = 320.0f;
    for (const char *digit = digits; *digit; ++digit) {
        left -= 0.5f * center * widths[*digit - '0'];
    }
    const auto line = [&](float x, float top, uint32_t ink) {
        for (const char *digit = digits; *digit; ++digit) {
            const unsigned index = *digit - '0';
            const float width = widths[index];
            const float u = (index * cell + 2) / atlas;
            sprite(intl ? title_intl : title_font, x, top, width * size, 128.0f * size, menu_depth + 1,
                   u, 0, u + width / atlas, 1, ink);
            x += width * size;
        }
    };
    line(left + 1.92f, y + 1.92f, color & 0xff000000);
    line(left, y, color);
}

void View::note_mesh(NoteMesh &mesh, unsigned lane, float z, float opacity, bool missed) {
    const float x = lane_x(lane);
    const float y = note_lift(1.0f - stage_transition_.visibility(), lane);
    const float height = missed ? 0.1f : 1.0f;
    const unsigned bank = missed ? 5 : lane;
    const uint32_t alpha = static_cast<uint32_t>(opacity * 255.0f) << 24;
    for (size_t i = 0; i < mesh.points.size(); ++i) {
        const auto &point = mesh.points[i];
        mesh.projected[i] = board_vertex(x + point[0], y + height * point[1], z + point[2], 0, 0, 0, false);
        scale_vertex(mesh.projected[i]);
    }
    // Reuse the projected position across faces and preserve per-corner color and strip boundaries.
    for (const auto &corner : mesh.corners) {
        auto *vertex = pvr_dr_target(direct_);
        auto projected = mesh.projected[corner.point];
        projected.flags = corner.flags;
        projected.argb = alpha | (corner.colors[bank] & 0xffffff);
        *vertex = projected;
        pvr_dr_commit(vertex);
    }
}

void View::note_heads(const Frame &frame, const Chart &chart, const Round &round,
                      float period_ms, bool translucent) {
    const float visibility = stage_transition_.visibility();
    if (visibility == 0.0f) {
        return;
    }
    const auto past = std::lower_bound(chart.begin(), chart.end(), frame.time_ms - int(period_ms),
                                       [](const Chord &chord, int time) { return chord.time_ms < time; });
    material(translucent ? faded_note_header_ : mesh_header_);
    for (auto it = past; it != chart.end(); ++it) {
        const auto &chord = *it;
        const float z = board_z(chord.time_ms - frame.time_ms, period_ms);
        if (z >= board_length) {
            break;
        }
        const bool missed = z < 0.0f;
        if (z < -1.0f || (missed && round.was_hit(it - chart.begin()))) {
            continue;
        }
        const float fade = std::min(1.0f, (board_length - z) / (board_length * 0.2f));
        if ((missed || visibility * fade <= 0.9f) != translucent) {
            continue;
        }
        for (unsigned lane = 0; lane < 5; ++lane) {
            if (!(chord.frets & (1u << lane))) {
                continue;
            }
            const float end = board_z(chord.ends[lane] - frame.time_ms, period_ms);
            const float opacity = missed ? 0.5f * std::clamp(1.0f + end, 0.0f, 1.0f) : fade;
            if (opacity <= 0.0f) {
                continue;
            }
            const float alpha = translucent ? visibility * opacity : 1.0f;
            note_mesh(regular_note_, lane, z, alpha, missed);
            if (chord.hopo && frame.tapping) {
                note_mesh(tap_marker_, lane, z, alpha, missed);
            }
        }
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
        const auto center = board_vertex(x, 0.0f, z, 0, 0, 0xbfffffff, false);
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
        submit(strip, std::size(strip));
        previous = next;
        step = first_step + step_gradient * (remaining - time);
    }
}

void View::world(const Frame &f, const Round &round, const Chart &chart,
                 const GuitarVisualFrame &timing) {
    const float visibility = stage_transition_.visibility();
    const float period_ms = timing.note_period_ms;
    const float scroll = std::fmod(-0.5f * timing.scroll_beat * board_length / 5.0f, 1.0f);
    const auto surface = [&](float start, float end, uint32_t back, uint32_t front) {
        board_quad(neck, left_edge, right_edge, 0.0f, start, end,
                   1, scroll - 0.5f * end, 0, scroll - 0.5f * start, back, front);
    };
    const auto neck_color = tint(white, visibility);
    surface(-2.0f, -1.0f, neck_color, 0x00ffffff);
    surface(-1.0f, 8.4f, neck_color, neck_color);
    surface(8.4f, board_length, 0x00ffffff, neck_color);
    draw_tracks(visibility);
    const float hidden = 1.0f - visibility;
    const float rotation = hidden * matrix_pi * 0.5f;
    float angle = rotation;
    for (int beat = static_cast<int>(timing.grid_beat); beat < timing.grid_beat + 5.0f;
         ++beat, angle += rotation) {
        const float z = (beat - timing.grid_beat) * board_length / 5.0f;
        if (z < -1.0f) {
            continue;
        }
        const auto alpha = 0.75f * std::min({1.0f, 1.0f + z, (board_length - z) / 2.4f});
        draw_board_bar(0.005f - hidden, z, angle, visibility * alpha, yellow);
    }
    draw_board_bar(0.006f, 0.0f, 0.0f, visibility * 0.5f, gold);

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
                const bool missed = z < 0 && !taken;
                const auto tail_color = tint(missed ? 0xff999999 : color,
                                             visibility * fade * (missed ? 0.5f : 1.0f));
                const auto begin = std::max(taken ? 0.0f : -1.0f, z);
                if (end > begin) {
                    board_quad(tail, x + .1f, x - .1f, note_lift(hidden, lane) + .008f,
                               begin, std::min(board_length, end),
                               0, 0, 1, 1, tail_color, tail_color);
                }
            }
        }
    }

    note_heads(f, chart, round, period_ms, true);

    const auto *held = round.sustaining_chord(chart);
    const auto guitar_time = static_cast<float>(fret_time_ms_ - guitar_time_origin_ms_);
    if (held) {
        for (unsigned lane = 0; lane < fret_activities_.size(); ++lane) {
            if (held->frets & (1u << lane)) {
                draw_tail_wave(lane, *held, f.time_ms, period_ms, guitar_time);
            }
        }
    }
    draw_fret_glows(f, held);
}

void View::draw_fret_glows(const Frame &frame, const Chord *held) {
    const float time = static_cast<float>(fret_time_ms_ - guitar_time_origin_ms_);
    const float pulse = shz_cosf(held ? matrix_pi + (time - held->time_ms) * 0.01f : time * 0.01f);
    constexpr float radians = matrix_pi / 180.0f;
    for (unsigned lane = 0; lane < fret_activities_.size(); ++lane) {
        const float activity = fret_activities_[lane];
        if (activity == 0.0f) {
            continue;
        }
        material(textures_[glow].header);
        const float x = lane_x(lane);
        const float weight = fret_weights_[lane];
        float y = (weight + (frame.strumming && weight > 0.0f ? 0.25f : 0.0f)) / 6.0f;
        float angle = (activity + 0.1f * time) * radians;
        float radius = 0.22f * (activity + 1.5f) * activity;
        for (unsigned layer = 0; layer < 3; ++layer) {
            const float strength = (1.0f - layer * 0.2f) * activity * pulse * 0.25f + 0.75f;
            const auto color = shade(fret_colors[lane], strength);
            const auto rotation = shz_sincosf(angle);
            pvr_vertex_t strip[4];
            for (unsigned i = 0; i < 4; ++i) {
                const float u = i & 1;
                const float v = i / 2;
                const float dx = u > 0 ? radius : -radius;
                const float dz = v > 0 ? radius : -radius;
                strip[i] = board_vertex(x + dx * rotation.cos + dz * rotation.sin, y,
                                   -dx * rotation.sin + dz * rotation.cos, u, 1.0f - v, color, i == 3);
            }
            submit(strip, std::size(strip));
            y += strength * 0.2f;
            radius *= 0.8f;
            angle += strength * 20.0f * radians;
        }
    }
}

void View::update_stage_beat(int time_ms) {
    if (!guitar_clock_.advance_stage(time_ms)) {
        return;
    }
    if (beat_lane_count_ < beat_lanes_.size()) {
        ++beat_lane_count_;
    } else {
        std::rotate(beat_lanes_.begin(), beat_lanes_.begin() + 1, beat_lanes_.end());
    }
    // Stage.triggerBeat copies the previous average into the next beat slot.
    beat_lanes_[beat_lane_count_ - 1] = beat_lanes_[beat_lane_count_ - 2];
}

void View::draw_tracks(float visibility) {
    const auto color = tint(yellow, visibility);
    float drop = 1.0f - visibility;
    for (unsigned lane = 0; lane < 5; ++lane) {
        const auto x = lane_x(lane);
        board_quad(strings, x + 0.035f, x - 0.035f, 0.003f - drop, -2.0f, board_length,
                   0, 0, 1, 1, color, color);
        drop *= 2.0f;
    }
}

void View::stage_decoration() {
    const auto pulse = [&](float delay) {
        if (!guitar_clock_.last_beat_ms()) {
            return 0.0f;
        }
        const float phase = (stage_time_ms_ - guitar_clock_.last_beat_ms()) /
                            guitar_clock_.stage_period_ms() - delay;
        return 1.0f - std::clamp(phase, 0.0f, 1.0f);
    };
    const auto layer = [&](TextureId id, float px, float py, float w, float h) {
        sprite(id, stage_transition_.x(px) - w / 2, 240 + py * 240 - h / 2,
               w, h, 0.015f, 0, 0, 1, 1, tint(white, stage_transition_.opacity()));
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
    const auto crowd_left = shz_sincosf(matrix_pi * beat_phase);
    const auto crowd_right = shz_sincosf(2 * matrix_pi * right_phase);
    layer(stage_crowd_left, -0.8f + 0.04f * beat_phase * crowd_left.sin,
          0.4f + 0.08f * beat_phase * crowd_left.cos, 278, 248);
    layer(stage_crowd_right, 0.8f + 0.02f * right_phase * crowd_right.sin,
          0.4f + 0.06f * right_phase * crowd_right.cos, 172, 223);
}

void View::stage_lights() {
    // Positions and rotations are the six additive light layers in the default stage.ini.
    constexpr float light_x[] = {-0.87f, -0.69f, -0.48f, 0.83f, 0.64f, 0.46f};
    constexpr float light_y[] = {-0.75f, -0.8f, -0.83f, -0.75f, -0.84f, -0.9f};
    constexpr float light_angle[] = {0.0f, -12.0f, -16.0f, 0.0f, 12.0f, 19.0f};
    constexpr float miss_angle[] = {10.0f, -10.0f, 7.0f, 12.0f, -5.0f, -5.0f};
    constexpr float miss_period[] = {75.0f, 100.0f, 150.0f, 250.0f, 200.0f, 150.0f};
    const float recent = stage_pick_ms_ ? 1.0f - std::clamp(
        (stage_time_ms_ - stage_pick_ms_) / 500.0f, 0.0f, 1.0f) : 0.0f;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned channel = i % 3;
        if (beat_lane_count_ < channel + 2) {
            continue;
        }
        const auto color = tint(lane_color(beat_lanes_[channel], 0.50f + 0.35f * recent), 0.7f);
        float impulse = 0.0f;
        if (stage_miss_ms_) {
            const float phase = std::clamp((stage_time_ms_ - stage_miss_ms_) / miss_period[i], 0.0f, 1.0f);
            impulse = i == 5 ? 1.0f - phase : 1.0f - shz_cosf(matrix_pi * (1.0f - phase));
        }
        rotated_sprite(stage_beam, stage_transition_.x(light_x[i]), 240 + 240 * light_y[i],
                       294 * 3, 263 * 3, (light_angle[i] + miss_angle[i] * impulse) * matrix_pi / 180,
                       0.95f, color);
    }
}

bool View::prepare_light_stage() {
    if (!light_stage_attempted_) {
        light_stage_attempted_ = true;
        if (load_texture(light_flame_one, "light-graphics-flame1", false) &&
            !load_texture(light_flame_two, "light-graphics-flame2", false)) {
            pvr_mem_free(textures_[light_flame_one].memory);
            textures_[light_flame_one].memory = nullptr;
        }
    }
    return textures_[light_flame_two].memory != nullptr;
}

void View::draw_light_stage() {
    const auto pulse = [this](int event_ms, float delay) {
        if (!event_ms) {
            return 0.0f;
        }
        return 1.0f - std::clamp((stage_time_ms_ - event_ms) / 500.0f - delay, 0.0f, 1.0f);
    };
    for (unsigned layer = 0; layer < 2; ++layer) {
        const float miss = pulse(stage_miss_ms_, layer ? 0.1f : 0.0f);
        const auto move = shz_sincosf(4.0f * matrix_pi * miss);
        const float pick = pulse(stage_pick_ms_, layer ? 0.0f : 0.25f);
        const float scale = 1.0f + (layer ? 0.1f : 0.05f) * pick;
        const float width = 704.0f * scale;
        const float height = 544.0f * scale;
        const float center_x = 320.0f + (0.02f * 640.0f) * miss * move.sin;
        const float center_y = 240.0f + (0.04f * 480.0f) * miss * move.cos;
        sprite(static_cast<TextureId>(light_flame_one + layer), center_x - 0.5f * width,
               center_y - 0.5f * height, width, height, 0.015f,
               0, 0, 1, 1, tint(white, stage_transition_.opacity()));
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
        submit(flash, std::size(flash));
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
    const float rise = 128.0f * (1.0f - stage_transition_.visibility());
    char value[48];
    std::snprintf(value, sizeof(value), "%u", round.display_score(frame.time_ms, rules));
    text(384, 9 - rise, value, main_font, gold);
    std::snprintf(value, sizeof(value), "%ux", round.multiplier());
    text(384, 54 - rise, value, main_font, gold);

    constexpr uint32_t balls[] = {0xff80ff80, 0xffff8080, 0xffffff80, 0xff8080ff};
    const unsigned streak = std::min(39u, round.streak);
    const unsigned lit = streak % 10 + 1;
    for (unsigned i = 0; i < 10; ++i) {
        sprite(i < lit ? ball_on : ball_off, 429 + i * 18.5f, 67 - rise, 19.5f, 19.5f,
               menu_depth, 0, 0, 1, 1, balls[streak / 10]);
    }
    if (round.streak) {
        const auto hit = language_.format("%d hit", std::to_string(round.streak));
        const float age = std::max(0, frame.time_ms - round.last_hit_ms) / 1000.0f;
        const float size = main_font * (1.0f + 0.25f * std::pow(std::max(0.0f, 1.0f - age), 2));
        text(102, 8 - rise, hit.c_str(), size, gold, true);
    } else if (frame.screen == Screen::play) {
        const auto age = frame.time_ms - round.last_miss_ms;
        if (age >= 0 && age < 180) {
            text(64, 12 + 0.0032f * age * age - rise, tr("Missed!"), main_font,
                 tint(gold, 1.0f - age * 0.005f));
        }
    }
}

void View::update_menu_clock(const Frame &frame) {
    menu_frame_ms_ = timer_ms_gettime64();
    const auto page = frame.settings_menu && frame.screen == Screen::settings ?
                      frame.settings_menu->page : SettingsPage::categories;
    const auto selected = frame.settings_menu && frame.screen == Screen::settings ?
                          frame.settings_menu->selected : frame.selected;
    const bool new_menu = frame.screen != menu_screen_ || !menu_enter_ms_ ||
                          menu_settings_page_ != page || menu_results_actions_ != frame.results_actions;
    if (new_menu) {
        menu_enter_ms_ = menu_frame_ms_;
        const bool main_menu = frame.screen == Screen::menu || frame.screen == Screen::editor_menu ||
                               (frame.screen == Screen::settings && page != SettingsPage::pause_volume);
        if (!main_menu) {
            menu_main_enter_ms_ = 0;
        } else if (!menu_main_enter_ms_) {
            menu_main_enter_ms_ = menu_frame_ms_;
        }
    }
    if (new_menu || selected != menu_selected_) {
        menu_selected_ms_ = menu_frame_ms_;
    }
    menu_screen_ = frame.screen;
    menu_results_actions_ = frame.results_actions;
    menu_settings_page_ = page;
    menu_selected_ = selected;
}

void View::main_menu_art(float phase, float hidden) {
    const float slide = hidden * hidden;
    const float cube = slide * slide * slide;
    rotated_sprite(logo, 320, 96, 572 * (1 + 0.025f * shz_sinf(phase * 16) + cube),
                   138 * (1 - 0.025f * shz_cosf(phase * 17) - cube), 0, menu_depth);
    rotated_sprite(menu_pose, 480 + 1280 * slide, 312, 302 * 0.9f, 413 * 0.9f, 0, menu_depth);
}

void View::menu_item(float x, float y, const char *label, float size, bool selected,
                     float hidden, float depth_offset) {
    const float phase = (menu_frame_ms_ - menu_selected_ms_) / 50.0f;
    const float base_alpha = 1.0f - hidden;
    const float alpha = selected ? (shz_sinf(phase) * 0.15f + 0.75f) * (1.0f - 2.0f * hidden) :
                                  base_alpha;
    if (alpha <= 0.0f) {
        return;
    }
    // Menu.py rotates around the normalized viewport origin, including the shadow.
    const float offset = selected ? -3.2f * alpha : 0.0f;
    const float angle = hidden * matrix_pi / 4.0f;
    const float max_size = (640.0f - x - 24.0f) /
                           (std::max(1u, text_width(label)) * text_scale_);
    text(x - 160.0f * hidden + offset, y + offset, label, std::min(size, max_size),
         tint(selected ? gold : yellow, alpha), false, depth_offset, angle);
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
    if (frame.screen == Screen::setlist) {
        const float age = cassette_frame_ms_ ? cassette_frame_ms_ - cassette_enter_ms_ : 0;
        const float t = age / 5000.0f;
        const float hidden = 1.0f - std::min(1.0f, age / 512.0f);
        const float scale = shz_sinf(t / 8) + 2;
        rotated_sprite(cassette_bg, 320 + 1280 * hidden * hidden + 160 * shz_sinf(t / 2),
                       240 - 120 * shz_cosf(t), 350 * scale, 223 * scale, t, 0.01f);
    } else if (frame.screen == Screen::difficulty || frame.screen >= Screen::editing) {
        const float age = menu_frame_ms_ - menu_enter_ms_;
        const bool choosing = frame.screen == Screen::difficulty;
        const float time = choosing ? age / 5000.0f : 34.0f + frame.editor_age_ms / 5000.0f;
        const float hidden = choosing ? 1.0f - std::min(1.0f, age / 512.0f) : 0.0f;
        const float scale = shz_sinf(time / 8) + 2;
        rotated_sprite(editor_bg, 320 + 1280 * hidden * hidden + 160 * shz_sinf(time / 2),
                       240 - 120 * shz_cosf(time), 1457 * scale, 402 * scale, time, 0.01f);
    } else {
        const float age = menu_frame_ms_ - (menu_main_enter_ms_ ? menu_main_enter_ms_ : menu_enter_ms_);
        const float phase = age / 5000.0f;
        const float zoom = shz_sinf(phase / 8) + 2;
        const float hidden = 1.0f - std::min(1.0f, age / 512.0f);
        rotated_sprite(keyboard, 320 + 1280 * hidden * hidden + 160 * shz_cosf(phase / 2),
                       240 - 120 * shz_sinf(phase),
                       958 * zoom, 272 * zoom, phase, 0.01f);
    }
}

void View::credits(const CreditsFrame &frame) {
    constexpr float view_width = 640.0f;
    constexpr float view_height = 480.0f;
    const float line_height = font_height() * main_font * text_scale_;
    const float scroll_progress = frame.elapsed_ms / 5000.0f;
    const float offset = 1.0f - scroll_progress;
    const float hidden = 1.0f - frame.visibility;
    const float slide = hidden * hidden * view_width;
    const float phase = scroll_progress + 34.0f;
    const float zoom = shz_sinf(phase / 8.0f) + 3.0f;
    const float x = 320.0f + 160.0f * shz_cosf(phase / 2.0f) + 2.0f * slide;
    const float y = 240.0f - 120.0f * shz_sinf(phase);
    constexpr TextureId backgrounds[] = {editor_bg, keyboard, cassette_bg};
    constexpr float widths[] = {1457, 958, 350};
    constexpr float heights[] = {402, 272, 223};
    for (unsigned i = 0; i < std::size(backgrounds); ++i) {
        const float cycle = offset + i * 2.0f;
        const float wrap = cycle - 6.0f * std::floor(cycle / 6.0f);
        rotated_sprite(backgrounds[i], x, y + view_height * (wrap - 3.0f), widths[i] * zoom,
                       heights[i] * zoom, -0.5f * shz_sinf(phase * 4.0f + i), 0.01f + i * 0.001f);
    }

    float top = offset * view_width;
    for (const auto &line : credits_lines()) {
        if (top >= view_height) {
            break;
        }
        const float height = line.text ? line_height * line.scale : 160.0f;
        if (top + height > 0.0f) {
            if (!line.text) {
                rotated_sprite(logo, 320.0f, top + height * 0.5f, 572, 138, 0, menu_depth);
            } else if (line.text[0] != ' ' || line.text[1] != '\0') {
                const float scale = line.scale * main_font;
                float left = 64.0f;
                if (line.align == CreditAlign::center) {
                    left = 320.0f;
                } else if (line.align == CreditAlign::right) {
                    left = 576.0f;
                    left -= scale * text_scale_ * text_width(line.text);
                }
                text(left - slide, top, line.text, scale, line.gold ? gold : yellow,
                     line.align == CreditAlign::center);
            }
        }
        top += height;
    }
}

void View::fit_text(float x, float y, const char *line, float width, float size, uint32_t color,
                    float depth_offset) {
    const unsigned advance = text_width(line);
    if (advance) {
        text(x, y, line, std::min(size, width / (advance * text_scale_)), color, false, depth_offset);
    }
}

void View::score_stars(float x, float y, float size, unsigned count, uint32_t color) {
    for (unsigned i = 0; i < 5; ++i) {
        const auto id = i < count ? star : star_off;
        const float left = x + i * size;
        const float height = size * 109.0f / (id == star ? 113.0f : 114.0f);
        sprite(id, left + 1.92f, y + 1.92f, size, height, menu_depth, 0, 1, 1, 0, color & 0xff000000);
        sprite(id, left, y, size, height, menu_depth, 0, 1, 1, 0, color);
    }
}

View::TextureId View::stock_cassette_label(std::string_view key) const {
    const auto found = std::find(stock_label_keys.begin(), stock_label_keys.end(), key);
    return found == stock_label_keys.end() ? texture_count :
        static_cast<TextureId>(label_bangbang + (found - stock_label_keys.begin()));
}

View::TextureId View::song_cassette_label(const SongEntry &song) const {
    if (song.midi == "/cd/songs/bangbang/notes.mid") return label_bangbang;
    if (song.midi == "/cd/songs/defy/notes.mid") return label_defy;
    if (song.midi == "/cd/songs/twibmpg/notes.mid") return label_twibmpg;
    return texture_count;
}

View::TextureId View::cassette_label(const SongEntry &song) const {
    const auto preset = stock_cassette_label(song.cassette_label);
    if (preset != texture_count) {
        return preset;
    }
    for (size_t slot = 0; slot < custom_labels_.size(); ++slot) {
        if (custom_labels_[slot].ready && custom_labels_[slot].song == song.midi) {
            return static_cast<TextureId>(custom_label_a + slot);
        }
    }
    return song_cassette_label(song);
}

static std::string original_label_path(const SongEntry &song) {
    const auto directory = song.library_count >= 0 ? song.midi : song.midi.substr(0, song.midi.rfind('/'));
    return directory + "/label";
}

void View::prepare_editor_label(const Frame &frame) {
    const auto &key = (*frame.editor_labels)[frame.selected].key;
    const auto &entry = frame.editor->entry();
    editor_label_preview_ = stock_cassette_label(key);
    if (editor_label_preview_ != texture_count) {
        return;
    }
    const auto path = key.empty() ? original_label_path(entry) : editor_label_path(key);
    auto &cached = custom_labels_[0];
    if (cached.song != path) {
        if (textures_[custom_label_a].memory && pvr_wait_render_done() < 0) {
            return;
        }
        cached.song = path;
        cached.ready = !path.empty() && load_texture(custom_label_a, path.c_str(), false);
    }
    editor_label_preview_ = cached.ready ? custom_label_a :
                            key.empty() ? song_cassette_label(entry) : texture_count;
}

void View::prepare_cassette_labels(const Frame &frame, const std::vector<SongEntry> &library) {
    if (frame.screen == Screen::editor_label) {
        prepare_editor_label(frame);
        return;
    }
    if (frame.screen != Screen::setlist || !frame.selected || frame.selected >= library.size()) {
        for (size_t slot = 0; slot < custom_labels_.size(); ++slot) {
            auto &texture = textures_[custom_label_a + slot];
            if (texture.memory) {
                if (pvr_wait_render_done() < 0) {
                    return;
                }
                pvr_mem_free(texture.memory);
                texture = {};
            }
            custom_labels_[slot] = {};
        }
        return;
    }

    const auto visible = visible_cassettes(frame.selected);
    std::array<std::string_view, 3> wanted{};
    for (size_t slot = 0; slot < visible.size(); ++slot) {
        const auto i = visible[slot];
        if (i && cassettes_[i].label_loaded) {
            wanted[slot] = library[i].midi;
        }
    }
    for (size_t slot = 0; slot < visible.size(); ++slot) {
        const auto name = wanted[slot];
        if (name.empty() || std::any_of(custom_labels_.begin(), custom_labels_.end(),
            [name](const CachedLabel &label) { return label.song == name; })) {
            continue;
        }
        const auto free = std::find_if(custom_labels_.begin(), custom_labels_.end(),
            [&](const CachedLabel &label) {
                return label.song.empty() ||
                       std::find(wanted.begin(), wanted.end(), label.song) == wanted.end();
            });
        if (free == custom_labels_.end()) {
            continue;
        }
        const auto id = static_cast<TextureId>(custom_label_a + (free - custom_labels_.begin()));
        if (textures_[id].memory && pvr_wait_render_done() < 0) {
            continue;
        }
        free->song = name;
        const auto &entry = library[visible[slot]];
        const auto override_path = editor_label_path(entry.cassette_label);
        free->ready = (!override_path.empty() && load_texture(id, override_path.c_str(), false, true)) ||
                      load_texture(id, original_label_path(entry).c_str(), false, true);
    }
}

bool View::prepare_cassette_mesh(const char *name, CassetteMesh &mesh) {
    Mesh shape;
    if (!load_mesh(name, shape)) {
        return false;
    }
    for (const auto &face : shape) {
        if (face.count != 3) {
            return false;  // The cassette mesh consists solely of triangles.
        }
        for (unsigned i = 0; i < face.count; ++i) {
            const auto &vertex = face.vertices[i];
            const auto shared = std::find_if(mesh.points.begin(), mesh.points.end(),
                [&](const MeshVertex &point) {
                    return point.x == vertex.x && point.y == vertex.y && point.z == vertex.z &&
                           point.shade == vertex.shade;
                });
            mesh.corners.push_back(static_cast<uint16_t>(shared - mesh.points.begin()));
            if (shared == mesh.points.end()) {
                mesh.points.push_back(vertex);
            }
        }
    }
    mesh.projected.resize(mesh.points.size());
    return true;
}

void View::update_cassettes(const Frame &frame, const std::vector<SongEntry> &library) {
    const size_t songs = library.size();
    if (frame.screen != Screen::setlist || !frame.selected || frame.selected >= songs) {
        cassette_frame_ms_ = 0;
        return;
    }
    const auto now = timer_ms_gettime64();
    if (!cassette_frame_ms_ || cassettes_.size() != songs || library_generation_ != frame.library_generation) {
        library_generation_ = frame.library_generation;
        cassette_frame_ms_ = cassette_enter_ms_ = now;
        cassette_camera_ = cassette_target_ = 0.0f;
        cassettes_.assign(songs, {});
    }
    const float dt = std::min<uint64_t>(now - cassette_frame_ms_, 192);
    cassette_frame_ms_ = now;
    cassette_camera_ += (cassette_target_ - cassette_camera_) * dt / 192.0f;

    float y = 0.0f;
    for (size_t i = 1; i < songs; ++i) {  // The Tutorial opens from the main menu.
        auto &item = cassettes_[i];
        item.angle = std::clamp(item.angle + (i == frame.selected ? dt : -dt) * 0.5f, 0.0f, 90.0f);
        item.shown = !frame.chooser || frame.chooser->matches(library[i]);
        if (!item.shown) {
            continue;
        }
        const bool folder = library[i].library_count >= 0;
        item.label_loaded |= folder || i == frame.selected;
        const float closed = folder ? 1.2f : 0.8f;
        const float height = closed + (4.0f - closed) * shz_sinf_deg(item.angle);
        item.center_y = y + height / 2;
        y += height;
    }
    if (cassettes_[frame.selected].shown) {
        cassette_target_ = cassettes_[frame.selected].center_y;
    }
}

std::array<size_t, 3> View::visible_cassettes(unsigned selected) const {
    // Keep the original three-cassette vertex budget when filtering a large collection.
    std::array<size_t, 3> visible{};
    for (size_t i = selected; i > 1;) {
        if (cassettes_[--i].shown) {
            visible[0] = i;
            break;
        }
    }
    visible[1] = cassettes_[selected].shown ? selected : 0;
    for (size_t i = selected + 1; i < cassettes_.size(); ++i) {
        if (cassettes_[i].shown) {
            visible[2] = i;
            break;
        }
    }
    return visible;
}

void View::draw_cassettes(const Frame &frame, const std::vector<SongEntry> &library, bool stickers) {
    if (!cassette_frame_ms_) {
        return;
    }
    const float age = cassette_frame_ms_ - cassette_enter_ms_;
    const float hidden = 1.0f - std::min(1.0f, age / 512.0f);
    const float camera_slide = 10.0f * hidden * hidden * hidden * hidden;
    if (camera_slide > 7.0f) {
        return;  // The group starts outside the left edge of the view.
    }
    for (const auto i : visible_cassettes(frame.selected)) {
        if (!i) {
            continue;
        }
        const auto &pose = cassettes_[i];
        const bool folder = library[i].library_count >= 0;
        const auto turn = shz_sincosf_deg(folder ? -pose.angle : pose.angle);
        const auto spin = shz_sincosf_deg(folder && i == frame.selected ? age * 0.08f : 0.0f);
        const float center = cassette_camera_ - pose.center_y;
        const auto project = [&](const MeshVertex &point, float u, float v, uint32_t color, bool end) {
            const float py = spin.cos * point.y - spin.sin * point.z;
            const float pz = spin.sin * point.y + spin.cos * point.z;
            const float rx = turn.cos * point.x - turn.sin * py + 10.0f - camera_slide;
            const float ry = turn.sin * point.x + turn.cos * py + center;
            const float rz = pz - 4.0f - camera_slide;
            // SongChooser's camera looks from (-10,y,4) toward (0,y,2.5) with a 60-degree FOV.
            constexpr float forward_x = 0.98893635f;
            constexpr float side_x = 0.14834045f;
            const float depth = 1.0f / (forward_x * rx - side_x * rz);
            const float sx = 320.0f + focus * (side_x * rx + forward_x * rz) * depth;
            return pvr_vertex_t{end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                                sx, 240.0f - focus * ry * depth, depth, u, v,
                                shade(color, point.shade), 0};
        };
        if (stickers) {
            const auto label = cassette_label(library[i]);
            if (label == texture_count || !pose.label_loaded) {
                continue;
            }
            material(textures_[label].header);
            const auto &sticker = folder ? library_sticker_ : cassette_sticker_;
            for (const auto &face : sticker) {
                pvr_vertex_t strip[4];
                for (unsigned k = 0; k < face.count; ++k) {
                    const auto &v = face.vertices[k];
                    strip[k] = project(v.point, v.u, v.v, white, k + 1 == face.count);
                }
                submit(strip, face.count);
            }
        } else {
            material(mesh_header_);
            const auto draw_shape = [&](CassetteMesh &shape, uint32_t color) {
                for (size_t p = 0; p < shape.points.size(); ++p) {
                    shape.projected[p] = project(shape.points[p], 0, 0, color, false);
                    scale_vertex(shape.projected[p]);
                }
                unsigned corner = 0;
                for (const auto point : shape.corners) {
                    auto *vertex = pvr_dr_target(direct_);
                    *vertex = shape.projected[point];
                    if (++corner == 3) {
                        vertex->flags = PVR_CMD_VERTEX_EOL;
                        corner = 0;
                    }
                    pvr_dr_commit(vertex);
                }
            };
            const auto color = library[i].cassette_color;
            draw_shape(folder ? library_shell_ : cassette_shell_, color ? color : (i == frame.selected ? gold : yellow));
            draw_shape(folder ? library_details_ : cassette_reels_, 0xff1a1a1au);
        }
    }
}

float View::wrapped_text(float x, float y, const std::string &label, float size,
                         uint32_t color, float hidden, float right) {
    const float space = text_width(" ") * size * text_scale_;
    const float height = font_height() * size * text_scale_;
    float left = x;
    size_t from = 0;
    for (unsigned index = 0; from < label.size(); ++index) {
        const auto next = label.find(' ', from);
        const auto word = label.substr(from, next - from);
        const float width = text_width(word.c_str()) * size * text_scale_;
        if (left + width > right || word == "\n") {
            left = x;
            y += height;
        }
        if (word != "\n") {
            const float drop = hidden * index * 640;
            const float angle = -hidden * (index + 1) * matrix_pi / 4;
            text(left, y + drop, word.c_str(), size, color, false, 0.0f, angle);
            left += width + space;
        }
        from = next == std::string::npos ? label.size() : next + 1;
    }
    return y;
}

void View::setlist(const Frame &frame, const std::vector<SongEntry> &library) {
    if (library.empty() || frame.selected >= library.size()) {
        return;
    }
    const float age = cassette_frame_ms_ ? cassette_frame_ms_ - cassette_enter_ms_ : 0;
    const float hidden = 1.0f - std::min(1.0f, age / 512.0f);
    const float slide = hidden * hidden;
    const auto unselected = tint(yellow, 1.0f - slide);
    const auto selected = tint(gold, 1.0f - slide);
    text(384, 32 - 640 * slide, tr("Choose a Song"), main_font, unselected);
    if (!frame.search.empty()) {
        auto filter = language_.format("Filter: %s", frame.search) + '|';
        if (!frame.filter_match) {
            filter += std::string(" (") + tr("Not found") + ')';
        }
        fit_text(32, 448 + 640 * slide, filter.c_str(), 580, 0.5f * main_font, unselected);
    }
    if (!frame.selected || !frame.filter_match) {
        return;
    }
    const float closed = 1.0f - cassettes_[frame.selected].angle / 90.0f;
    const float turn = closed * closed;
    constexpr float label_size = 0.8f * main_font;
    const float label_height = font_height() * label_size * text_scale_;
    const auto &song = library[frame.selected];
    const auto after_title = wrapped_text(384, 96, song.title, label_size, selected, turn);
    if (song.library_count >= 0) {
        const auto count = song.library_count == 1 ? std::string(tr("One song in this library")) :
                           language_.format("%d songs in this library", std::to_string(song.library_count));
        wrapped_text(384, after_title + 3 * label_height, count, label_size, unselected, turn);
        return;
    }
    wrapped_text(384, after_title + label_height, song.artist, label_size, unselected, turn);
    if (frame.scores) {
        float y = (frame.available == 0x0f ? 268.8f : 320.0f) + 320.0f * turn;
        constexpr float size = 0.55f * main_font;
        const float h = font_height() * size * text_scale_;
        for (int difficulty = 3; difficulty >= 0; --difficulty) {
            if (!(frame.available & (1u << difficulty))) {
                continue;
            }
            const auto &table = frame.scores->table(song.midi, difficulty);
            const auto *best = table.empty() ? nullptr : &table.front();
            char points[16] = "---";
            if (best) {
                std::snprintf(points, sizeof(points), "%lu", static_cast<unsigned long>(best->points));
            }
            text(384, y, tr(difficulties[difficulty]), size, unselected);
            fit_text(480, y, points, 150, size, selected);
            score_stars(384, y + h, h * 0.75f * 0.9f, best ? best->stars : 0, unselected);
            fit_text(480, y + h, best ? best->name.c_str() : "---", 150, size, selected);
            y += 2 * h + 160.0f * turn;
        }
    }
}

void View::draw_editor(const Frame &frame) {
    constexpr const char *options[] = {
        "Edit Existing Song", "Import New Song", "Import Guitar Hero(tm) Songs", "Load SD Card"
    };
    constexpr const char *actions[] = {
        "Save Song", "Set Song Name", "Set Artist Name", "Set Beats per Minute",
        "Estimate Beats per Minute", "Set A/V delay", "Set Cassette Color",
        "Set Cassette Label", "Editing Help", "Quit to Main Menu"
    };
    constexpr const char *prompts[] = {
        "", "Enter Song Name", "Enter Artist Name", "Enter Beats per Minute Value", "",
        "Enter A/V delay in milliseconds", "Enter cassette color in HTML (#RRGGBB) format."
    };
    constexpr float overlay_text = 2.0f;
    const auto items = [&](const char *const *names, unsigned count, float top, float space, float size) {
        for (unsigned i = 0; i < count; ++i) {
            text(128, top + i * space, tr(names[i]), size, frame.selected == i ? gold : yellow,
                 false, overlay_text);
        }
    };
    if (frame.screen == Screen::editor_menu) {
        rotated_sprite(logo, 320, 103, 512, 124, 0, menu_depth);
        items(options, std::size(options), menu_y, menu_line, main_font);
        if (!frame.editor_text.empty()) {
            fit_text(64, menu_y + 4 * menu_line, frame.editor_text.c_str(), 512, 0.8f * main_font, yellow);
        }
        return;
    }
    if (frame.screen == Screen::editor_import) {
        text(32, 32, tr("Importing Songs"), main_font, gold);
        if (!frame.editor_text.empty()) {
            fit_text(32, 90, frame.editor_text.c_str(), 576, 0.8f * main_font, yellow);
        }
        if (frame.import_sources->empty()) {
            text(32, 144, "No staged songs were found on disc or SD card.", 0.8f * main_font, yellow);
        } else {
            const size_t first = frame.selected >= 8 ? frame.selected - 7 : 0;
            const size_t last = std::min(frame.import_sources->size(), first + 8);
            for (size_t i = first; i < last; ++i) {
                const auto &song = (*frame.import_sources)[i];
                fit_text(64, 144 + (i - first) * 32, song.title.c_str(), 512, main_font,
                         frame.selected == i ? gold : yellow);
            }
        }
        return;
    }

    const auto &editor = *frame.editor;
    const auto &chart = editor.chart();
    const float position = editor.scroll_ms();
    const float period = chart.us_per_beat(int(position)) * 0.001f;
    const auto project = [&](float x, float y, float z, float bias, float u, float v,
                             uint32_t color, bool last) {
        // gluLookAt((-2, 9, 5.5), (2, 0, 5.5), (0, 1, 0)) and a 60 degree FOV.
        const float ax = (frame.lefty ? -x : x) + 2.0f;
        const float ay = y - 9.0f;
        constexpr float cos_pitch = 0.406138466f;
        constexpr float sin_pitch = 0.913811549f;
        const float depth = 1.0f / (cos_pitch * ax - sin_pitch * ay);
        return pvr_vertex_t{last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
                            320.0f + focus * (z - 5.5f) * depth,
                            240.0f - focus * (sin_pitch * ax + cos_pitch * ay) * depth,
                            depth + bias, u, v, color, 0};
    };
    const auto quad = [&](const pvr_poly_hdr_t &header, float x0, float x1, float z0, float z1,
                          float bias, uint32_t near_color, uint32_t far_color,
                          float v0 = 0, float v1 = 1) {
        material(header);
        const pvr_vertex_t vertices[] = {
            project(x0, 0, z0, bias, 0, v0, near_color, false),
            project(x1, 0, z0, bias, 1, v0, near_color, false),
            project(x0, 0, z1, bias, 0, v1, far_color, false),
            project(x1, 0, z1, bias, 1, v1, far_color, true)
        };
        submit(vertices, std::size(vertices));
    };
    const auto surface = [&](float start, float end, uint32_t a, uint32_t b) {
        const float offset = std::fmod(-0.5f * position / period * board_length / 5.0f, 1.0f);
        quad(textures_[neck].header, right_edge, left_edge, start, end, 0, a, b,
             offset - 0.5f * start, offset - 0.5f * end);
    };
    surface(-2, -1, 0x00ffffff, white);
    surface(-1, 8.4f, white, white);
    surface(8.4f, board_length, white, 0x00ffffff);
    const float selected = lane_x(editor.lane());
    quad(overlay_header_, selected - 0.32f, selected + 0.32f, -0.5f, board_length - 0.5f,
         0.001f, tint(white, 0.15f), tint(white, 0.15f));
    for (unsigned lane = 0; lane < 5; ++lane) {
        const float x = lane_x(lane);
        quad(textures_[strings].header, x - 0.035f, x + 0.035f, -2, board_length, 0.002f, yellow, yellow);
    }
    const float beat = position / period;
    for (int quarter = int(beat) * 4; quarter < (beat + 5) * 4; ++quarter) {
        const float z = (quarter * 0.25f - beat) * board_length / 5.0f;
        const float opacity = (quarter % 4 ? 0.5f : 0.75f) *
                              std::clamp(std::min(1.0f + z, (board_length - z) / 2.4f), 0.0f, 1.0f);
        quad(textures_[bars].header, right_edge, left_edge, z - 0.04f, z + 0.04f, 0.003f,
             tint(yellow, opacity), tint(yellow, opacity));
    }
    quad(textures_[bars].header, right_edge, left_edge, -0.04f, 0.04f, 0.004f,
         tint(gold, 0.5f), tint(gold, 0.5f));
    const auto &notes = chart.guitar[editor.difficulty()];
    const auto last = std::upper_bound(notes.begin(), notes.end(), position + period * 5,
                                       [](float time, const Chord &note) { return time < note.time_ms; });
    const auto head = [&](NoteMesh &mesh, unsigned lane, float z, float opacity) {
        material(faded_note_header_);
        const auto x = lane_x(lane);
        const unsigned color = z < 0 ? 5 : lane;
        for (size_t i = 0; i < mesh.points.size(); ++i) {
            const auto &point = mesh.points[i];
            const float height = z < 0 ? point[1] * 0.1f : point[1];
            mesh.projected[i] = project(x + point[0], height, z + point[2], 0.006f, 0, 0, white, false);
            scale_vertex(mesh.projected[i]);
        }
        for (const auto &corner : mesh.corners) {
            auto vertex = mesh.projected[corner.point];
            vertex.flags = corner.flags;
            vertex.argb = tint(corner.colors[color], opacity);
            auto *target = pvr_dr_target(direct_);
            *target = vertex;
            pvr_dr_commit(target);
        }
    };
    for (auto note = notes.begin(); note != last; ++note) {
        const float z = (note->time_ms - position) / period * board_length / 5.0f;
        for (unsigned lane = 0; lane < 5; ++lane) {
            if (!(note->frets & (1u << lane))) {
                continue;
            }
            const float x = lane_x(lane);
            const float end = (note->ends[lane] - position) / period * board_length / 5.0f;
            const float visible = std::clamp(z < 0 ? 1.0f + end : (board_length - z) / 2.4f, 0.0f, 1.0f);
            if (!visible) {
                continue;
            }
            const auto ink = tint(z < 0 ? 0xff999999 : shade(fret_colors[lane], 0.8f, 0.1f),
                                  visible * (z < 0 ? 0.5f : 1.0f));
            const float begin = std::max(-1.0f, z);
            if (end > begin) {
                quad(textures_[tail].header, x - 0.1f, x + 0.1f, begin,
                     std::min(board_length, end), 0.005f, ink, ink);
            }
            if (z >= -1.0f) {
                const float opacity = visible * (z < 0 ? 0.5f : 1.0f);
                head(regular_note_, lane, z, opacity);
                if (note->hopo && frame.tapping) {
                    head(tap_marker_, lane, z, opacity);
                }
            }
        }
    }
    material(faded_note_header_);
    for (unsigned lane = 0; lane < 5; ++lane) {
        const bool active = (frame.held & (1u << lane)) || lane == editor.lane();
        const float weight = active ? (frame.strumming ? 0.75f : 0.5f) : 0.0f;
        for (const auto &face : fret_model_) {
            for (unsigned i = 0; i < face.count; ++i) {
                const auto &point = face.vertices[i];
                const auto color = shade(fret_colors[lane], 0.8f * point.shade, (0.1f + weight) * point.shade);
                auto vertex = project(lane_x(lane) + point.x, point.y + weight / 6, point.z, 0.006f,
                                      0, 0, color, i + 1 == face.count);
                scale_vertex(vertex);
                auto *target = pvr_dr_target(direct_);
                *target = vertex;
                pvr_dr_commit(target);
            }
        }
    }

    char time[32];
    const unsigned ms = unsigned(editor.position_ms());
    std::snprintf(time, sizeof(time), "%u.%02u'%03u", ms / 60000, ms % 60000 / 1000, ms % 1000);
    text(32, 10, time, main_font, gold);
    text(32, 55, tr(frame.editor_playing ? "Playing" : "Stopped"), main_font, gold);
    text(32, 100, tr(difficulties[editor.difficulty()]), main_font, gold);
    const auto title = editor.entry().title + (editor.has_changes() ? "*" : "");
    fit_text(320, 10, title.c_str(), 288, main_font, yellow);
    if (frame.screen == Screen::editing) {
        return;
    }
    material(overlay_header_);
    const auto veil = tint(0, 0.7f);
    const pvr_vertex_t dim[] = {
        {PVR_CMD_VERTEX, 0, 0, menu_depth + 1.5f, 0, 0, veil, 0},
        {PVR_CMD_VERTEX, 640, 0, menu_depth + 1.5f, 0, 0, veil, 0},
        {PVR_CMD_VERTEX, 0, 480, menu_depth + 1.5f, 0, 0, veil, 0},
        {PVR_CMD_VERTEX_EOL, 640, 480, menu_depth + 1.5f, 0, 0, veil, 0}
    };
    submit(dim, std::size(dim));
    if (frame.screen == Screen::editor_actions) {
        items(actions, std::size(actions), 50, 37, 0.90f * main_font);
    } else if (frame.screen == Screen::editor_value && frame.selected < std::size(prompts)) {
        text(320, 155, tr(prompts[frame.selected]), main_font, gold, true, overlay_text);
        fit_text(48, 220, frame.editor_text.c_str(), 544, main_font, yellow, overlay_text);
    } else if (frame.screen == Screen::editor_tempo) {
        text(320, 155, tr("Estimate Beats per Minute"), main_font, gold, true, overlay_text);
        fit_text(64, 210, tr("Tap the Space bar to the beat of the song. Press Enter when done or Escape to cancel."),
                 512, main_font, yellow, overlay_text);
        if (!frame.editor_text.empty()) {
            text(320, 265, frame.editor_text.c_str(), main_font, gold, true, overlay_text);
        }
    } else if (frame.screen == Screen::editor_label) {
        fit_text(32, 44, tr("Choose a 256x128 PNG format label image."), 576, main_font,
                 gold, overlay_text);
        const auto &labels = *frame.editor_labels;
        const size_t first = frame.selected >= 8 ? frame.selected - 7 : 0;
        const size_t last = std::min(labels.size(), first + 8);
        for (size_t i = first; i < last; ++i) {
            const auto &item = labels[i];
            const auto *title = item.key.empty() ? tr(item.title.c_str()) : item.title.c_str();
            fit_text(32, 117 + (i - first) * 37, title, 344, main_font,
                     frame.selected == i ? gold : yellow, overlay_text);
        }
        if (editor_label_preview_ != texture_count) {
            sprite(editor_label_preview_, 394, 143, 222, 111, menu_depth + overlay_text);
        }
    } else if (frame.screen == Screen::editor_saving) {
        text(320, 215, tr("Saving..."), main_font, gold, true, overlay_text);
    } else if (frame.editor_text.empty()) {
        text(64, 110, tr("Editing keys: "), main_font, gold, false, overlay_text);
        text(64, 155, tr("Arrows - Move cursor"), main_font, yellow, false, overlay_text);
        text(64, 197, tr("Space - Play/pause song"), main_font, yellow, false, overlay_text);
        fit_text(64, 239, tr("Enter - Make note (hold and move for long notes)"), 512, main_font, yellow, overlay_text);
        text(64, 281, tr("Delete - Delete note"), main_font, yellow, false, overlay_text);
        text(64, 323, tr("Page Up/Down - Change difficulty"), main_font, yellow, false, overlay_text);
    } else {
        fit_text(48, 215, tr(frame.editor_text.c_str()), 544, main_font, yellow, overlay_text);
    }
}

void View::menu(const Frame &frame, const Round &round,
                const std::vector<SongEntry> &library) {
    const auto item = [&](float x, float y, const char *label, unsigned index) {
        text(x, y, tr(label), main_font, frame.selected == index ? gold : yellow);
    };
    const auto center = [&](float y, const char *label) { text(320, y, tr(label), main_font, yellow, true); };
    const float age_ms = menu_frame_ms_ - menu_enter_ms_;
    const float menu_hidden = 1.0f - std::min(1.0f, age_ms / 512.0f);
    const float menu_slide = menu_hidden * menu_hidden;
    const float spacing = font_height() * main_font * text_scale_;
    // On the fixed viewport all the visible menu entries still need to fit.
    const float row_spacing = std::min(spacing, (480.0f - menu_y) / 6.0f);
    const float row_font = main_font * row_spacing / spacing;
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
        submit(vertices, std::size(vertices));
    };
    char line[128];
    if (frame.countdown > 0.0f && (frame.screen == Screen::countdown || frame.screen == Screen::pause)) {
        const auto title_color = tint(yellow, 4.0f - std::abs(4.0f - frame.countdown));
        const float title_end = wrapped_text(32, 32 - 0.5f * spacing, frame.title,
                                            0.75f * main_font, title_color, 0.0f, 384.0f);
        wrapped_text(32, title_end + 0.75f * spacing, frame.artist,
                     0.75f * main_font, title_color, 0.0f, 384.0f);
        if (frame.countdown > 1.0f) {
            const float prompt_alpha = 3.0f - std::abs(4.0f - frame.countdown);
            text(320, 192, tr("Get Ready to Rock"), main_font, tint(yellow, prompt_alpha), true);
            if (frame.countdown < 6.0f) {
                const int beat = static_cast<int>(frame.countdown);
                const float beat_fraction = frame.countdown - beat;
                const float scale = 1.28f * (1.0f + 0.25f * beat_fraction * beat_fraction * beat_fraction);
                std::snprintf(line, sizeof(line), "%d", beat);
                title_number(288 - 0.5f * big_font_height() * scale * text_scale_, line, scale, gold);
            }
        }
    }
    if (frame.screen == Screen::play || frame.screen == Screen::pause) {
        if (frame.tutorial_picture >= 0 && frame.tutorial_picture < 3) {
            constexpr float widths[] = {302, 509, 265};
            constexpr float heights[] = {413, 424, 274};
            const auto picture = frame.tutorial_picture;
            sprite(static_cast<TextureId>(tutorial_pose + picture),
                   320.0f - widths[picture] * 0.5f,
                   (0.5f + frame.picture_fade) * 480.0f - heights[picture] * 0.5f,
                   widths[picture], heights[picture], menu_depth);
        }
        constexpr float caption_top = 0.67f * 640.0f;
        for (const auto *caption : frame.tutorial_captions) {
            if (caption) {
                text(320, caption_top, tr(caption), main_font, white, true);
            }
        }
    }

    if (frame.cheat_message && (frame.screen == Screen::play || frame.screen == Screen::countdown)) {
        dim(1.0f);
        fit_text(64, 192, tr(frame.cheat_message), 512, main_font, yellow, 1.0f);
        text(320, 270, tr("<OK>"), 0.5f * main_font, gold, true, 1.0f);
    }

    switch (frame.screen) {
        case Screen::menu: {
            const float art_age = menu_frame_ms_ - menu_main_enter_ms_;
            main_menu_art(art_age / 5000.0f, 1.0f - std::min(1.0f, art_age / 512.0f));
            const std::string editor = std::string(tr("Song Editor")) + " >";
            const char *options[] = {tr("Play Game"), tr("Tutorial"), editor.c_str(),
                                     tr("Settings >"), tr("Credits"), tr("Quit")};
            float row_slide = menu_slide;
            for (unsigned i = 0; i < std::size(options); ++i) {
                menu_item(128, menu_y + i * row_spacing, options[i], row_font,
                          i == frame.selected, row_slide);
                row_slide *= 2.0f;
            }
            if (frame.input == InputDevice::none) {
                text(128, 168, "Connect a keyboard or controller", .7f * row_font, yellow);
            }
            break;
        }
        case Screen::settings: {
            const auto *settings = frame.settings_menu;
            if (!settings) {
                break;
            }
            if (settings->page != SettingsPage::pause_volume) {
                const float art_age = menu_frame_ms_ - menu_main_enter_ms_;
                main_menu_art(art_age / 5000.0f, 1.0f - std::min(1.0f, art_age / 512.0f));
            }
            if (settings->page == SettingsPage::key_test) {
                dim(0.9f);
                const char *prompt = tr("Play with the keys and press Escape when you're done.");
                fit_text(64, 128, prompt, 512, main_font, yellow, 1.0f);
                for (unsigned i = 0; i < 5; ++i) {
                    std::snprintf(line, sizeof(line), "#%u", i + 1);
                    text(128 + 96 * i, 256, line, main_font,
                         settings->test_held & (1u << i) ? fret_colors[i] : 0xff666666, false, 1.0f);
                }
                text(288, 320, tr("Pick!"), main_font,
                     settings->test_held & (1u << 5) ? gold : 0xff666666, false, 1.0f);
                break;
            }
            if (settings->capture) {
                dim(0.9f);
                const auto &row = settings->row(settings->selected);
                const auto prompt = language_.format("Press a key for '%s' or Escape to cancel.", tr(row.name));
                fit_text(64, 211.2f, prompt.c_str(), 512, main_font, yellow, 1.0f);
                break;
            }
            const auto last = std::min(settings->row_count(), settings->offset + 6);
            const float depth = settings->page == SettingsPage::pause_volume ? 1.0f : 0.0f;
            float row_slide = menu_slide;
            for (unsigned i = settings->offset; i < last; ++i) {
                const auto &row = settings->row(i);
                const bool current = i == settings->selected;
                const auto y = menu_y + (i - settings->offset) * row_spacing;
                auto value = row.setting == Setting::language ? Language::name(settings->draft.language) :
                             setting_value(settings->draft, row);
                const bool submenu = row.setting == Setting::game || row.setting == Setting::mods ||
                                     row.setting == Setting::keys || row.setting == Setting::video ||
                                     row.setting == Setting::audio || row.setting == Setting::volume;
                std::string label = tr(row.name);
                if (submenu) {
                    label += " >";
                } else if (!value.empty()) {
                    const bool adjust = current && setting_adjustable(settings->draft, row.setting);
                    label += ": ";
                    if (adjust) {
                        label += "< ";
                    }
                    label += row.setting == Setting::language ? value : tr(value.c_str());
                    if (adjust) {
                        label += " >";
                    }
                }
                menu_item(128, y, label.c_str(), row_font, current, row_slide, depth);
                row_slide *= 2.0f;
            }
            if (settings->offset) {
                text(95, menu_y, "^", row_font, yellow, false, depth);
            }
            if (last < settings->row_count()) {
                text(95, menu_y + 5 * row_spacing, "v", row_font, yellow, false, depth);
            }
            break;
        }
        case Screen::credits:
            credits(frame.credits);
            break;
        case Screen::setlist:
            setlist(frame, library);
            break;
        case Screen::difficulty: {
            text(64, 32 - 640.0f * menu_slide, tr("Choose a difficulty:"), main_font,
                 tint(yellow, 1.0f - menu_slide));
            float row_slide = menu_slide;
            for (int i = 3, row = 0; i >= 0; --i) {
                if (frame.available & (1u << i)) {
                    menu_item(128, menu_y + row * row_spacing, tr(difficulties[i]), row_font,
                              frame.selected == unsigned(i), row_slide);
                    ++row;
                    row_slide *= 2.0f;
                }
            }
            break;
        }
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
                menu_item(128, menu_y + i * row_spacing, tr(options[i]), row_font,
                          i == frame.selected, transition, 1.0f);
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
            if (frame.results_actions) {
                const auto title = language_.format("Highest Scores (%s)", tr(difficulties[frame.difficulty]));
                text(320, (0.05f - slide) * 640, title.c_str(), main_font, color, true);
                std::string footer;
                if (frame.scores && frame.result_song < library.size()) {
                    const auto &table = frame.scores->table(library[frame.result_song].midi, frame.difficulty);
                    const float size = 0.85f * row_font;
                    float y = (0.15f + slide) * 640;
                    unsigned row = 0;
                    for (const auto &entry : table) {
                        const auto row_color = tint(int(row) == frame.highscore_rank && age % 500 < 250
                                                        ? gold : yellow, 1.0f - slide);
                        std::snprintf(line, sizeof(line), "%u.", row + 1);
                        text(64, y, line, size, row_color);
                        std::snprintf(line, sizeof(line), "%lu", static_cast<unsigned long>(entry.points));
                        fit_text(96, y, line, 120, size, row_color);
                        score_stars(224, y, row_spacing * 0.75f * 0.85f * 0.9f, entry.stars, row_color);
                        fit_text(384, y, entry.name.c_str(), 232, size, row_color);
                        ++row;
                        y += row_spacing;
                    }
                    const char *notice = nullptr;
                    switch (frame.scores->save_status()) {
                        case ScoreBook::SaveStatus::pending: notice = "Saving scores to the VMU..."; break;
                        case ScoreBook::SaveStatus::no_vmu: notice = "No VMU. Scores kept for this session."; break;
                        case ScoreBook::SaveStatus::full: notice = "VMU full. Scores kept for this session."; break;
                        case ScoreBook::SaveStatus::error: notice = "VMU error. Scores kept for this session."; break;
                        default: break;
                    }
                    footer = notice ? tr(notice) : "";
                }
                constexpr const char *actions[] = {"Replay", "Change Song", "Quit to Main Menu"};
                float row_slide = menu_slide;
                for (unsigned i = 0; i < std::size(actions); ++i) {
                    menu_item(128, 320 + i * row_spacing, tr(actions[i]), row_font,
                              frame.selected == i, row_slide);
                    row_slide *= 2.0f;
                }
                std::string upload_notice;
                if (frame.upload_status == UploadStatus::pending) {
                    upload_notice = tr("Uploading Scores...");
                } else if (frame.upload_status == UploadStatus::failed) {
                    upload_notice = tr("Score upload failed");
                } else if (frame.upload_status == UploadStatus::succeeded && frame.upload_ordinal > 0) {
                    upload_notice = language_.format("You're #%d on the world charts!",
                                                      std::to_string(frame.upload_ordinal));
                }
                if (!upload_notice.empty()) {
                    fit_text(32, (0.7f + slide) * 640, upload_notice.c_str(), 576,
                             0.5f * main_font, color);
                }
                fit_text(32, upload_notice.empty() ? 452.0f : 468.0f, footer.c_str(), 576,
                         0.45f * row_font, color);
                break;
            }
            text(320, (0.05f - slide) * 640, tr("Song Finished!"), main_font, color, true);
            const float rise = anim(0, 1000);
            const auto score = age >= 2000 ? round.score : static_cast<unsigned>(round.score * double(anim(1000, 2000)));
            std::snprintf(line, sizeof(line), "%u", score);
            title_number((1.11f + slide - rise * rise * rise) * 640, line, 1.6f, color, main_font);
            if (age > 1000) {
                const float star_width = 0.75f * big_font_height() * 0.0017f * 640 * text_scale_;
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
            if (age > 2500) {
                const auto accuracy = frame.gems ? round.gem_hits * 100 / frame.gems : 0;
                const auto accuracy_text = language_.format("Accuracy: %d%%", std::to_string(accuracy));
                const auto streak = language_.format("Longest note streak: %d", std::to_string(round.best_streak));
                const float y = 352 + 640.0f * slide;
                text(320, y, accuracy_text.c_str(), main_font, color, true);
                text(320, y + spacing, streak.c_str(), main_font, color, true);
            }
            if (frame.results_warning) {
                const float hidden_warning = 1.0f - frame.results_dialog_visibility;
                const float warning_slide = hidden_warning * hidden_warning;
                const float alpha = 1.0f - warning_slide;
                dim(alpha);
                const auto end = wrapped_text(64, (0.3f + 2.0f * warning_slide) * 640,
                                              tr("No highscores for cheaters!"), main_font,
                                              tint(yellow, alpha), warning_slide, 576);
                text(320, end + 1.5f * spacing + 1280 * warning_slide, tr("<OK>"),
                     0.5f * main_font, tint(gold, alpha), true, 1.0f);
            } else if (frame.results_name) {
                const float hidden_name = 1.0f - frame.results_dialog_visibility;
                const float name_slide = hidden_name * hidden_name;
                const auto prompt_color = tint(yellow, 1.0f - name_slide);
                const auto text_color = tint(gold, 1.0f - name_slide);
                dim(1.0f - name_slide);
                const float y = (0.33f - name_slide) * 640;
                const auto prompt = language_.format("%d points is a new high score! Please enter your name:",
                                                     std::to_string(round.score));
                const float prompt_end = wrapped_text(64, y, prompt, main_font, prompt_color, 0, 576);
                const float name_y = prompt_end + (0.08f + 1.25f * name_slide) * 640;
                const bool controller = frame.input == InputDevice::controller;
                const char *cursor = frame.name_cursor && frame.results_dialog_visibility > 0.9f ? "|" : "";
                if (controller) {
                    std::snprintf(line, sizeof(line), "%s <%c>", frame.score_name.c_str(), frame.name_wheel);
                } else {
                    std::snprintf(line, sizeof(line), "%s%s", frame.score_name.c_str(), cursor);
                }
                const float name_end = wrapped_text(64, name_y, line, main_font, text_color, 0, 576);
                if (controller) {
                    text(64, name_end + spacing, "Up/Down: letter   Right: add   Left: erase   A: OK   B: cancel",
                         0.48f * main_font, prompt_color, false, 1.0f);
                }
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
            center(321, frame.input == InputDevice::controller ? "B Back" : "Esc Back");
            break;
        case Screen::editor_menu:
        case Screen::editor_import:
        case Screen::editing:
        case Screen::editor_actions:
        case Screen::editor_value:
        case Screen::editor_tempo:
        case Screen::editor_label:
        case Screen::editor_saving:
        case Screen::editor_message:
            draw_editor(frame);
            break;
        case Screen::play:
            break;
    }
}

void View::draw(const Frame &frame, const SongChart &song, const Round &round,
                const std::vector<SongEntry> &library) {
    text_scale_ = frame.text_scale / 100.0f;
    lefty_ = frame.lefty;
    update_menu_clock(frame);
    const bool playing = frame.screen == Screen::play || frame.screen == Screen::pause ||
                         frame.screen == Screen::countdown ||
                         (frame.screen == Screen::settings && frame.settings_menu &&
                          frame.settings_menu->page == SettingsPage::pause_volume);
    const bool stage_active = stage_transition_.advance(playing, menu_frame_ms_);
    const bool finishing_stage = frame.screen == Screen::results && stage_active;
    const float stage_visibility = stage_transition_.visibility();
    const bool opaque_stage = playing && stage_visibility == 1.0f;
    const auto &chart = song.guitar[frame.difficulty];
    update_cassettes(frame, library);

    GuitarVisualFrame visual_timing{};
    if (playing) {
        stage_time_ms_ = frame.time_ms;
        stage_pick_ms_ = round.hits ? round.last_hit_ms : 0;
        stage_miss_ms_ = round.overstrums ? round.last_miss_ms : 0;
        stage_strumming_ = frame.strumming;
        update_stage_beat(frame.time_ms);
        visual_timing = guitar_clock_.render(frame.time_ms, song.tempos,
                                             frame.countdown > 0.0f, frame.sustain_expired);
        update_frets(frame, round);
    }
    if (!render_slot(frame.half_refresh)) {
        return;
    }
    pvr_wait_ready();
    prepare_cassette_labels(frame, library);
    const bool new_language = frame.language != requested_language_;
    if (new_language || frame.mods != active_mods_) {
        pvr_wait_render_done();
        if (new_language) {
            requested_language_ = frame.language;
            select_language(requested_language_);
        }
        apply_mods(frame.mods);
    }
    const bool light_stage = stage_active && (frame.mods & 2) && prepare_light_stage();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    active_header_ = nullptr;
    pvr_dr_init(&direct_);
    if (playing) {
        if (opaque_stage && !light_stage) {
            sprite(stage, 0, 0, 640, 480, 0.01f);
        }
        note_heads(frame, chart, round, visual_timing.note_period_ms, false);
        if (opaque_stage) {
            draw_frets(stage_strumming_, stage_visibility, false);
        }
    } else if (frame.screen == Screen::setlist) {
        draw_cassettes(frame, library, false);
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    active_header_ = nullptr;
    if (stage_active) {
        if (light_stage) {
            draw_light_stage();
        } else {
            if (!opaque_stage) {
                sprite(stage_fade, 0, 0, 640, 480, 0.01f,
                       0, 0, 1, 1, tint(white, stage_transition_.opacity()));
            }
            stage_decoration();
        }
        if (playing) {
            world(frame, round, chart, visual_timing);
        } else {
            // The Classic drops its song before popping the layer. Only
            // tracks and frets have geometry during the outgoing animation.
            draw_tracks(stage_visibility);
        }
        if (!opaque_stage && stage_visibility > 0.0f) {
            draw_frets(stage_strumming_, stage_visibility, true);
        }
        if (!light_stage) {
            stage_lights();
        }
        draw_stats(frame, round, Rules{60000000.0f / song.initial_us_per_beat});
    }
    if (playing) {
        multiplier_effect(frame, round, song.initial_us_per_beat * 0.001f);
    } else if (frame.screen != Screen::credits && !finishing_stage) {
        menu_backdrop(frame);
        if (frame.screen == Screen::setlist) {
            draw_cassettes(frame, library, true);
        }
    }
    if (!finishing_stage) {
        menu(frame, round, library);
    }
    pvr_list_finish();
    pvr_scene_finish();
}

} // namespace dc
