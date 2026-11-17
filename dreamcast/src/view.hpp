// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "guitar_visual_clock.hpp"
#include "credits.hpp"
#include "editor.hpp"
#include "ranking.hpp"
#include "records.hpp"
#include "settings.hpp"
#include "language.hpp"
#include "song_library.hpp"
#include "stage_transition.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

class SongChooser;

enum class Screen {
    menu, settings, setlist, difficulty, countdown, play, pause, results, failed, error, credits,
    editor_menu, editor_import, editing, editor_actions, editor_value, editor_tempo, editor_label,
    editor_saving, editor_message
};
enum class InputDevice { none, keyboard, controller };

struct Frame {
    Screen screen = Screen::menu;
    CreditsFrame credits;
    const SongEditor *editor = nullptr;
    const std::vector<SongEntry> *import_sources = nullptr;
    const std::vector<EditorLabel> *editor_labels = nullptr;
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
    UploadStatus upload_status = UploadStatus::idle;
    int upload_ordinal = 0;
    const ScoreBook *scores = nullptr;
    const SettingsMenu *settings_menu = nullptr;
    uint16_t text_scale = 100;
    bool lefty = false;
    bool tapping = true;
    unsigned selected = 0;
    unsigned language = 0;
    uint8_t mods = 0;
    bool library_item = false;
    const SongChooser *chooser = nullptr;
    std::string search;
    unsigned library_generation = 0;
    bool filter_match = true;
    const char *cheat_message = nullptr;
    unsigned difficulty = 0;
    unsigned gems = 0;
    uint8_t available = 0;
    uint8_t held = 0;
    bool strumming = false;
    bool sustain_expired = false;
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
    void reset_effects(bool same_scene = false);
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
        light_flame_one, light_flame_two, custom_label_a, custom_label_b, custom_label_c, stage_fade, texture_count
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
        bool shown = false;
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
    TextureId stock_cassette_label(std::string_view key) const;
    TextureId song_cassette_label(const SongEntry &song) const;
    void prepare_editor_label(const Frame &frame);
    void prepare_cassette_labels(const Frame &frame, const std::vector<SongEntry> &library);
    void update_cassettes(const Frame &frame, const std::vector<SongEntry> &library);
    std::array<size_t, 3> visible_cassettes(unsigned selected) const;
    void draw_cassettes(const Frame &frame, const std::vector<SongEntry> &library, bool stickers);
    bool prepare_note_mesh(const char *name, NoteMesh &mesh);
    bool prepare_frets();
    void fret_vertices(unsigned lane, float weight, std::vector<pvr_vertex_t> &vertices,
                       float visibility = 1.0f) const;
    void draw_frets(bool strumming, float visibility, bool transparent);
    void draw_tracks(float visibility);
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
    void draw_board_bar(float y, float z, float opacity, uint32_t color);
    pvr_vertex_t board_vertex(float x, float y, float z, float u, float v, uint32_t color,
                              bool last, float bias = 0.0f) const;
    void select_language(unsigned index);
    bool prepare_international_font();
    void apply_mods(unsigned flags);
    const char *tr(const char *source) const { return language_.translate(source); }
    Glyph glyph_for(uint32_t cp, bool &intl) const;
    float font_height() const;
    float big_font_height() const;
    unsigned text_width(const char *line) const;
    void text(float x, float y, const char *line, float size, uint32_t color,
              bool centered = false, float depth_offset = 0, float angle = 0);
    void text_line(float x, float y, const char *line, float size, uint32_t color, float depth,
                   float sin, float cos);
    void fit_text(float x, float y, const char *line, float width, float size, uint32_t color,
                  float depth_offset = 0);
    void score_stars(float x, float y, float size, unsigned count, uint32_t color);
    void title_number(float y, const char *digits, float size, uint32_t color, float centering_size = 0);
    void note_mesh(NoteMesh &mesh, unsigned lane, float z, float opacity, bool missed);
    void note_heads(const Frame &frame, const Chart &chart, const Round &round,
                    float period_ms, bool translucent);
    void draw_tail_wave(unsigned lane, const Chord &note, int position_ms, float period_ms,
                        float guitar_time_ms);
    void world(const Frame &frame, const Round &round, const Chart &chart,
               const GuitarVisualFrame &timing);
    void update_stage_beat(int time_ms);
    void stage_decoration();
    void stage_lights();
    bool prepare_light_stage();
    void draw_light_stage();
    void draw_stats(const Frame &frame, const Round &round, const Rules &rules);
    void multiplier_effect(const Frame &frame, const Round &round, float song_period);
    void update_menu_clock(const Frame &frame);
    void main_menu_art(float phase, float hidden);
    void menu_item(float x, float y, const char *label, float size, bool selected, float hidden,
                   float depth_offset = 0);
    float wrapped_text(float x, float y, const std::string &label, float size, uint32_t color,
                       float hidden, float right = 576.0f);
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
    TextureId editor_label_preview_ = texture_count;
    std::vector<CassettePosition> cassettes_;
    unsigned library_generation_ = 0;
    uint64_t cassette_enter_ms_ = 0;
    uint64_t cassette_frame_ms_ = 0;
    float cassette_camera_ = 0.0f;
    float cassette_target_ = 0.0f;
    Screen menu_screen_ = Screen::error;
    unsigned menu_selected_ = 0;
    bool menu_results_actions_ = false;
    SettingsPage menu_settings_page_{};
    uint64_t menu_enter_ms_ = 0;
    uint64_t menu_main_enter_ms_ = 0;
    uint64_t menu_selected_ms_ = 0;
    uint64_t menu_frame_ms_ = 0;
    unsigned requested_language_ = 0;
    unsigned active_mods_ = 0;
    uint32_t yellow = 0xffffff80;
    uint32_t gold = 0xffffbf00;
    Language language_;
    std::vector<Glyph> international_glyphs_;
    float text_scale_ = 1.0f;
    bool lefty_ = false;
    std::array<uint8_t, 10> international_digits_{};
    bool international_title_loaded_ = false;
    std::array<uint8_t, 95> widths_{};
    std::array<uint8_t, 10> digit_widths_{};
    std::array<float, 4> recent_lanes_{};
    std::array<float, 5> beat_lanes_{};
    size_t recent_pick_count_ = 0;
    size_t beat_lane_count_ = 1;
    GuitarVisualClock guitar_clock_;
    StageTransition stage_transition_;
    int stage_time_ms_ = 0;
    int stage_pick_ms_ = 0;
    int stage_miss_ms_ = 0;
    bool stage_strumming_ = false;
    float bar_coverage_ = 0.0f;
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
