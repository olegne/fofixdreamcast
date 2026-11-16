// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "song_library.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

enum class Screen {
    menu, settings, setlist, difficulty, countdown, play, pause, resume_countdown, results, failed, error
};
enum class InputDevice { none, keyboard, controller };

struct Frame {
    Screen screen = Screen::menu;
    int time_ms = 0;
    int duration_ms = 0;
    int delay_ms = 0;
    int board_speed = 100;
    int countdown = 3;
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

    bool load_texture(TextureId id, const char *name, bool opaque, bool repeat = false,
                      bool additive = false);
    bool load_mesh(const char *name, Mesh &mesh);
    void sprite(TextureId id, float left, float top, float width, float height, float depth,
                float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1,
                uint32_t color = 0xffffffff);
    void rotated_sprite(TextureId id, float x, float y, float w, float h, float angle,
                        float depth, uint32_t color = 0xffffffff);
    void board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                    float u0, float v0, float u1, float v1,
                    uint32_t back = 0xffffffff, uint32_t front = 0xffffffff);
    void text(float x, float y, const char *line, float size, uint32_t color,
              bool centered = false, bool title = false);
    void text_line(float x, float y, const char *line, float size, uint32_t color, bool title);
    void mesh(const Mesh &shape, float x, float y, float z, uint32_t color);
    void draw_tail_wave(unsigned lane, float end, int time);
    void world(const Frame &frame, const SongChart &song, const Round &round,
               const Chart &chart, float period_ms);
    void stage_decoration(float beat_phase);
    void stage_lights(const Frame &frame, const Round &round);
    void draw_stats(const Frame &frame, const Round &round, const Rules &rules);
    void menu_backdrop(const Frame &frame);
    void setlist(const Frame &frame, const std::vector<SongEntry> &library);
    void menu(const Frame &frame, const Round &round,
              const std::vector<SongEntry> &library);

    std::array<Texture, texture_count> textures_{};
    std::array<std::array<uint8_t, 95>, 2> widths_{};
    pvr_poly_hdr_t mesh_header_{}, overlay_header_{}, glow_header_{};
    Mesh regular_note_, tap_marker_, fret_key_;
};

} // namespace dc
