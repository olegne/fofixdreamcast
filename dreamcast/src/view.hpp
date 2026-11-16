// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "song_library.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

enum class Screen {
    menu, settings, setlist, difficulty, countdown, play, pause, results, failed, error
};
enum class InputDevice { none, keyboard, controller };

struct Frame {
    Screen screen = Screen::menu;
    int time_ms = 0;
    int duration_ms = 0;
    int delay_ms = 0;
    int board_speed = 100;
    float countdown = 0.0f;
    float pause_visibility = 0.0f;
    unsigned results_age_ms = 0;
    bool results_actions = false;
    unsigned selected = 0;
    unsigned difficulty = 0;
    unsigned gems = 0;
    uint8_t available = 0;
    uint8_t held = 0;
    InputDevice input = InputDevice::none;
    const char *title = "";
    const char *artist = "";
    const char *error = "";
    const char *caption = nullptr;
    int tutorial_picture = -1;
    float picture_fade = 0.0f;
};

class View {
public:
    bool init();
    void reset_effects();
    void record_pick(const Round &round);
    void draw(const Frame &frame, const SongChart &song, const Round &round,
              const std::vector<SongEntry> &library);

private:
    enum TextureId {
        keyboard, menu_pose, logo, cassette_bg, editor_bg, cassette_front, cassette_above, cassette_below,
        label_bangbang, label_defy, label_twibmpg, stage,
        stage_lights_left, stage_lights_right, stage_drums, stage_bass, stage_speakers,
        stage_cones, stage_crowd_left, stage_crowd_right, stage_beam,
        neck, strings, bars, tail, glow, gem, tap_gem, ball_on, ball_off,
        star, star_off, mult2, mult3, mult4, font, title_font,
        tutorial_pose, tutorial_keyboard, tutorial_escape, texture_count
    };

    struct Texture {
        pvr_ptr_t memory = nullptr;
        pvr_poly_hdr_t header{};
    };

    struct MeshVertex {
        float x, y, z, shade;
    };

    struct Face {
        unsigned count;
        std::array<MeshVertex, 4> vertices;
    };

    using Mesh = std::vector<Face>;

    struct NoteCorner {
        size_t point;
        uint32_t flags;
        std::array<uint32_t, 5> colors;
    };

    struct NoteMesh {
        std::vector<std::array<float, 3>> points;
        std::vector<NoteCorner> corners;
        std::vector<pvr_vertex_t> projected;
    };

    struct NoteMeshRange {
        size_t first = 0;
        size_t last = 0;
        unsigned last_lane_end = 0;

        bool contains(size_t target, unsigned lane) const {
            return target >= first && (target < last || (target == last && lane < last_lane_end));
        }
    };

    bool load_texture(TextureId id, const char *name, bool opaque, bool repeat = false,
                      bool additive = false);
    bool load_mesh(const char *name, Mesh &mesh);
    bool prepare_note_mesh(const char *name, NoteMesh &mesh);
    bool prepare_frets();
    static pvr_vertex_t mesh_vertex(const MeshVertex &point, float x, float y, float z,
                                    uint32_t color, bool last);
    void material(const pvr_poly_hdr_t &header);
    void sprite(TextureId id, float left, float top, float width, float height, float depth,
                float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1,
                uint32_t color = 0xffffffff);
    void rotated_sprite(TextureId id, float x, float y, float w, float h, float angle,
                        float depth, uint32_t color = 0xffffffff);
    void board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                    float u0, float v0, float u1, float v1,
                    uint32_t back = 0xffffffff, uint32_t front = 0xffffffff);
    void text(float x, float y, const char *line, float size, uint32_t color,
              bool centered = false, float depth_offset = 0);
    void text_line(float x, float y, const char *line, float size, uint32_t color, float depth);
    void title_number(float y, const char *digits, float size, uint32_t color);
    void note_mesh(NoteMesh &mesh, unsigned lane, float z);
    void draw_tail_wave(unsigned lane, const Chord &note, int position_ms, float period_ms,
                        float guitar_time_ms);
    void world(const Frame &frame, const SongChart &song, const Round &round,
               const Chart &chart, float period_ms, const NoteMeshRange &meshes);
    void update_stage(int time_ms, float beat_period);
    void stage_decoration(int time_ms);
    void stage_lights(const Frame &frame, const Round &round);
    void draw_stats(const Frame &frame, const Round &round, const Rules &rules);
    void multiplier_effect(const Frame &frame, const Round &round, float song_period);
    void menu_backdrop(const Frame &frame);
    void setlist(const Frame &frame, const std::vector<SongEntry> &library);
    void menu(const Frame &frame, const Round &round,
              const std::vector<SongEntry> &library);

    std::array<Texture, texture_count> textures_{};
    std::array<uint8_t, 95> widths_{};
    std::array<uint8_t, 10> digit_widths_{};
    std::array<float, 4> recent_lanes_{};
    std::array<float, 5> beat_lanes_{};
    size_t recent_pick_count_ = 0;
    size_t beat_lane_count_ = 1;
    float stage_beat_period_ = 500.0f;
    int stage_beat_ = 0;
    int last_stage_beat_ms_ = -1;
    int last_multiplier_ms_ = -1;
    uint64_t guitar_time_origin_ms_ = 0;
    pvr_poly_hdr_t mesh_header_{}, overlay_header_{}, glow_header_{};
    const pvr_poly_hdr_t *active_header_ = nullptr;
    pvr_dr_state_t direct_{};
    NoteMesh regular_note_, tap_marker_;
    std::array<std::array<std::vector<pvr_vertex_t>, 2>, 5> frets_;
};

} // namespace dc
