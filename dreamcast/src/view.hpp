// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "credits.hpp"
#include "editor.hpp"
#include "records.hpp"
#include "settings.hpp"
#include "language.hpp"
#include "song_library.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

enum class Screen {
    menu, settings, setlist, difficulty, countdown, play, pause, results, failed, error, credits,
    editor_menu, editor_import, editing, editor_actions, editor_value, editor_tempo, editor_message
};
enum class InputDevice { none, keyboard, controller };

struct Frame {
    Screen screen = Screen::menu;
    CreditsFrame credits;
    const SongEditor *editor = nullptr;
    const std::vector<SongEntry> *import_sources = nullptr;
    std::string editor_text;
    bool editor_playing = false;
    unsigned editor_age_ms = 0;
    int time_ms = 0;
    int duration_ms = 0;
    int delay_ms = 0;
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
    const SettingsMenu *settings_menu = nullptr;
    uint16_t text_scale = 100;
    bool lefty = false;
    bool tapping = true;
    unsigned selected = 0;
    unsigned language = 0;
    uint8_t mods = 0;
    bool library_item = false;
    std::string search;
    unsigned library_generation = 0;
    bool filter_match = true;
    const char *cheat_message = nullptr;
    unsigned difficulty = 0;
    unsigned gems = 0;
    uint8_t available = 0;
    uint8_t held = 0;
    bool strumming = false;
    InputDevice input = InputDevice::none;
    const char *title = "";
    const char *artist = "";
    const char *error = "";
    std::array<const char *, 2> tutorial_captions{};
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
        neck, strings, bars, tail, glow, ball_on, ball_off,
        star, star_off, mult2, mult3, mult4, font, title_font,
        tutorial_pose, tutorial_keyboard, tutorial_escape, font_intl, title_intl,
        light_flame_one, light_flame_two, custom_label_a, custom_label_b, custom_label_c, texture_count
    };

    struct Texture {
        pvr_ptr_t memory = nullptr;
        size_t bytes = 0;
        pvr_poly_hdr_t header{};
    };

    struct Glyph {
        uint32_t codepoint;
        uint16_t x, y;
        uint8_t width, advance;
        int16_t left;
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

    struct CachedLabel {
        std::string song;
        bool ready = false;
    };

    struct CassetteMesh {
        std::vector<MeshVertex> points;
        std::vector<uint16_t> corners;
        std::vector<pvr_vertex_t> projected;
    };

    struct NoteCorner {
        size_t point;
        uint32_t flags;
        std::array<uint32_t, 6> colors;
    };

    struct NoteMesh {
        std::vector<std::array<float, 3>> points;
        std::vector<NoteCorner> corners;
        std::vector<pvr_vertex_t> projected;
    };

    bool load_texture(TextureId id, const char *name, bool opaque, bool repeat = false,
                      bool additive = false);
    template <typename Shape>
    bool load_mesh(const char *name, Shape &mesh);
    bool prepare_cassette_mesh(const char *name, CassetteMesh &mesh);
    TextureId cassette_label(const SongEntry &song) const;
    void prepare_cassette_labels(const Frame &frame, const std::vector<SongEntry> &library);
    void update_cassettes(const Frame &frame, const std::vector<SongEntry> &library);
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
    pvr_vertex_t board_vertex(float x, float y, float z, float u, float v, uint32_t color,
                              bool last, float bias = 0.0f) const;
    void select_language(unsigned index);
    void apply_mods(unsigned flags);
    const char *tr(const char *source) const { return language_.translate(source); }
    Glyph glyph_for(uint32_t cp) const;
    unsigned text_width(const char *line) const;
    void text(float x, float y, const char *line, float size, uint32_t color,
              bool centered = false, float depth_offset = 0);
    void text_line(float x, float y, const char *line, float size, uint32_t color, float depth);
    void fit_text(float x, float y, const char *line, float width, float size, uint32_t color,
                  float depth_offset = 0);
    void score_stars(float x, float y, float size, unsigned count, uint32_t color);
    void title_number(float y, const char *digits, float size, uint32_t color);
    void note_mesh(NoteMesh &mesh, unsigned lane, float z, float opacity, bool missed);
    void note_heads(const Frame &frame, const Chart &chart, const Round &round,
                    float period_ms, bool translucent);
    void draw_tail_wave(unsigned lane, const Chord &note, int position_ms, float period_ms,
                        float guitar_time_ms);
    void world(const Frame &frame, const SongChart &song, const Round &round,
               const Chart &chart, float period_ms);
    void update_stage_beat(int time_ms);
    void update_stage(int time_ms, const SongChart &song);
    void stage_decoration(int time_ms);
    void stage_lights(const Frame &frame, const Round &round);
    bool prepare_light_stage();
    void draw_light_stage(int time_ms, const Round &round);
    void draw_stats(const Frame &frame, const Round &round, const Rules &rules);
    void multiplier_effect(const Frame &frame, const Round &round, float song_period);
    void menu_backdrop(const Frame &frame);
    void credits(const CreditsFrame &frame);
    void draw_editor(const Frame &frame);
    void setlist(const Frame &frame, const std::vector<SongEntry> &library);
    void menu(const Frame &frame, const Round &round,
              const std::vector<SongEntry> &library);

    std::array<Texture, texture_count> textures_{};
    CassetteMesh cassette_shell_, cassette_reels_, library_shell_, library_details_;
    std::vector<LabelFace> cassette_sticker_, library_sticker_;
    std::array<CachedLabel, 3> custom_labels_{};
    std::vector<CassettePosition> cassettes_;
    unsigned library_generation_ = 0;
    uint64_t cassette_enter_ms_ = 0;
    uint64_t cassette_frame_ms_ = 0;
    float cassette_camera_ = 0.0f;
    float cassette_target_ = 0.0f;
    unsigned requested_language_ = 0;
    unsigned active_mods_ = 0;
    uint32_t yellow = 0xffffff80;
    uint32_t gold = 0xffffbf00;
    Language language_;
    std::vector<Glyph> international_glyphs_;
    float text_scale_ = 1.0f;
    bool lefty_ = false;
    std::array<uint8_t, 10> international_digits_{};
    bool international_font_loaded_ = false;
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
    bool light_stage_attempted_ = false;
    int last_multiplier_ms_ = -1;
    uint64_t guitar_time_origin_ms_ = 0;
    pvr_poly_hdr_t mesh_header_{}, faded_note_header_{}, overlay_header_{}, glow_header_{};
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
