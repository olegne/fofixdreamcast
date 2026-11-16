// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "credits.hpp"
#include "records.hpp"
#include "song_library.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

enum class Screen {
    menu, settings, setlist, difficulty, countdown, play, pause, results, failed, error, credits
};
enum class InputDevice { none, keyboard, controller };

struct Frame {
    Screen screen = Screen::menu;
    CreditsFrame credits;
    int time_ms = 0;
    int duration_ms = 0;
    int delay_ms = 0;
    int board_speed = 100;
    float countdown = 0.0f;
    float pause_visibility = 0.0f;
    unsigned results_age_ms = 0;
    bool results_actions = false;
    bool results_name = false;
    float name_visibility = 0.0f;
    bool name_cursor = false;
    char name_wheel = 'A';
    std::string score_name;
    size_t result_song = 0;
    int highscore_rank = -1;
    const ScoreBook *scores = nullptr;
    unsigned selected = 0;
    unsigned difficulty = 0;
    unsigned gems = 0;
    uint8_t available = 0;
    uint8_t held = 0;
    bool strumming = false;
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
    void record_pick(const Round &round, bool after_stage_update = false);
    void draw(const Frame &frame, const SongChart &song, const Round &round,
              const std::vector<SongEntry> &library);

private:
    enum TextureId {
        keyboard, menu_pose, logo, cassette_bg, editor_bg,
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

    struct LabelVertex {
        MeshVertex point;
        float u, v;
    };

    struct LabelFace {
        unsigned count;
        std::array<LabelVertex, 4> vertices;
    };

    struct CassettePosition {
        float angle = 0.0f;
        float center_y = 0.0f;
        bool label_loaded = false;
    };

    struct CassetteMesh {
        std::vector<MeshVertex> points;
        std::vector<uint16_t> corners;
        std::vector<pvr_vertex_t> projected;
    };

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
    template <typename Shape>
    bool load_mesh(const char *name, Shape &mesh);
    bool prepare_cassette_mesh(const char *name, CassetteMesh &mesh);
    TextureId cassette_label(const SongEntry &song) const;
    void update_cassettes(const Frame &frame, size_t songs);
    void draw_cassettes(const Frame &frame, const std::vector<SongEntry> &library, bool stickers);
    bool prepare_note_mesh(const char *name, NoteMesh &mesh);
    bool prepare_frets();
    void fret_vertices(unsigned lane, float weight, std::vector<pvr_vertex_t> &vertices) const;
    void update_frets(const Frame &frame, const Round &round);
    void draw_fret_glows(const Frame &frame, const Chord *held);
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
    void fit_text(float x, float y, const char *line, float width, float size, uint32_t color,
                  float depth_offset = 0);
    void score_stars(float x, float y, float size, unsigned count, uint32_t color);
    void title_number(float y, const char *digits, float size, uint32_t color);
    void note_mesh(NoteMesh &mesh, unsigned lane, float z);
    void draw_tail_wave(unsigned lane, const Chord &note, int position_ms, float period_ms,
                        float guitar_time_ms);
    void world(const Frame &frame, const SongChart &song, const Round &round,
               const Chart &chart, float period_ms, const NoteMeshRange &meshes);
    void update_stage_beat(int time_ms);
    void update_stage(int time_ms, const SongChart &song);
    void stage_decoration(int time_ms);
    void stage_lights(const Frame &frame, const Round &round);
    void draw_stats(const Frame &frame, const Round &round, const Rules &rules);
    void multiplier_effect(const Frame &frame, const Round &round, float song_period);
    void menu_backdrop(const Frame &frame);
    void credits(const CreditsFrame &frame);
    void setlist(const Frame &frame, const std::vector<SongEntry> &library);
    void menu(const Frame &frame, const Round &round,
              const std::vector<SongEntry> &library);

    std::array<Texture, texture_count> textures_{};
    CassetteMesh cassette_shell_, cassette_reels_;
    std::vector<LabelFace> cassette_sticker_;
    std::vector<CassettePosition> cassettes_;
    uint64_t cassette_enter_ms_ = 0;
    uint64_t cassette_frame_ms_ = 0;
    float cassette_camera_ = 0.0f;
    float cassette_target_ = 0.0f;
    std::array<uint8_t, 95> widths_{};
    std::array<uint8_t, 10> digit_widths_{};
    std::array<float, 4> recent_lanes_{};
    std::array<float, 5> beat_lanes_{};
    size_t recent_pick_count_ = 0;
    size_t beat_lane_count_ = 1;
    float stage_bpm_ = 50.0f;
    float stage_target_bpm_ = 50.0f;
    float stage_beat_period_ = 1200.0f;
    size_t stage_tempo_ = 0;
    int stage_beat_ = 0;
    int last_stage_beat_ms_ = -1;
    int last_multiplier_ms_ = -1;
    uint64_t guitar_time_origin_ms_ = 0;
    pvr_poly_hdr_t mesh_header_{}, overlay_header_{}, glow_header_{};
    const pvr_poly_hdr_t *active_header_ = nullptr;
    pvr_dr_state_t direct_{};
    NoteMesh regular_note_, tap_marker_;
    Mesh fret_model_;
    std::array<std::array<std::vector<pvr_vertex_t>, 3>, 5> fret_vertices_;
    std::vector<pvr_vertex_t> releasing_fret_;
    std::array<float, 5> fret_weights_{};
    std::array<float, 5> fret_activities_{};
    uint64_t fret_time_ms_ = 0;
};

} // namespace dc
