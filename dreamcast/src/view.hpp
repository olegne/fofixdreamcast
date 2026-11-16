// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "song_library.hpp"
#include <array>
#include <dc/pvr.h>
#include <vector>

namespace dc {

enum class Screen { menu, settings, setlist, difficulty, countdown, play, pause, results, failed, error };

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
    bool connected = false;
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
        main_bg, main_text, version, options_bg, stage, neck, power_neck, strings, bars, measure, beat,
        half, frets, notes, flame, glow, tail, hud, meter, star, star_off, pause_image, rock, fail,
        font, score_font, tutorial_pose, tutorial_keyboard, tutorial_escape, gem_a, gem_b, gem_c,
        gem_d, gem_e, power_a, power_b, power_c, power_d, power_e, texture_count
    };

    struct Texture {
        pvr_ptr_t memory = nullptr;
        pvr_poly_hdr_t header{};
    };

    struct MeshVertex {
        float x, y, z, u, v;
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
    void board_quad(TextureId id, float x0, float x1, float y, float z0, float z1,
                    float u0, float v0, float u1, float v1,
                    uint32_t back = 0xffffffff, uint32_t front = 0xffffffff);
    void text(float x, float y, const char *line, float size, uint32_t color,
              bool centered = false, bool numeric = false);
    void world(const Frame &frame, const SongChart &song, const Round &round,
               const Chart &chart, float period_ms);
    void draw_note_mesh(const Chord &chord, unsigned lane, float z);
    void draw_rockmeter(const Frame &frame, const Round &round, const Chart &chart,
                        const Rules &rules);
    void menu(const Frame &frame, const Chart &chart, const Round &round,
              const std::vector<SongEntry> &library);

    std::array<Texture, texture_count> textures_{};
    std::array<std::array<uint8_t, 95>, 2> widths_{};
    Mesh regular_note_, hopo_note_;
};

} // namespace dc
