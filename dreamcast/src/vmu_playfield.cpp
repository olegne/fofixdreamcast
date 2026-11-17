// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmu_playfield.hpp"

#include <algorithm>

namespace dc {
namespace {

constexpr int width = 48;
constexpr int height = 32;
constexpr int hit_row = 26;
constexpr int far_row = 2;
constexpr int lookahead_ms = 1024;
constexpr size_t max_visible_chords = 64;
constexpr std::array<int, 5> lane_centers{5, 14, 23, 32, 41};

constexpr void pixel(VmuBitmap &bitmap, int x, int y) {
    const auto bit = (height - 1 - y) * width + (width - 1 - x);
    bitmap[bit / 8] |= 0x80 >> (bit % 8);
}

constexpr void box(VmuBitmap &bitmap, int x, int y, int w, int h) {
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) {
            pixel(bitmap, col, row);
        }
    }
}

constexpr void note(VmuBitmap &bitmap, int x, int y) {
    box(bitmap, x - 1, y - 1, 3, 3);
    box(bitmap, x - 2, y, 5, 1);
}

constexpr VmuBitmap make_board() {
    VmuBitmap bitmap{};
    box(bitmap, 1, hit_row, width - 2, 1);
    for (const int x : lane_centers) {
        for (int y = far_row; y < hit_row; y += 3) {
            pixel(bitmap, x, y);
        }
        box(bitmap, x - 2, 28, 5, 1);
        box(bitmap, x - 2, 31, 5, 1);
        box(bitmap, x - 2, 29, 1, 2);
        box(bitmap, x + 2, 29, 1, 2);
    }
    return bitmap;
}

constexpr auto board = make_board();

int note_row(int offset_ms) {
    // A fixed preview time keeps the LCD stable across tempo changes and
    // independent of the video renderer's frame rate.
    return hit_row - std::clamp(offset_ms, 0, lookahead_ms) * (hit_row - far_row) / lookahead_ms;
}

uint32_t tail_rows(int from, int to) {
    return ((1u << to) - 1u) & ~((1u << from) - 1u);
}

// 3 × 5 row-major pixel glyphs for the logo and port credit.
constexpr uint16_t letters[] = {
    0b010'101'111'101'101, // A
    0b110'101'110'101'110,
    0b011'100'100'100'011,
    0b110'101'101'101'110,
    0b111'100'110'100'111,
    0b111'100'110'100'100,
    0b011'100'111'101'011,
    0b101'101'111'101'101, // H
    0b111'010'010'010'111,
    0b001'001'001'101'010,
    0b101'101'110'101'101,
    0b100'100'100'100'111,
    0b101'111'101'101'101,
    0b101'111'111'111'101,
    0b010'101'101'101'010, // O
    0b110'101'110'100'100,
    0b010'101'101'110'011,
    0b110'101'110'101'101,
    0b011'100'010'001'110,
    0b111'010'010'010'010,
    0b101'101'101'101'111,
    0b101'101'101'101'010, // V
    0b101'101'101'111'101,
    0b101'101'010'101'101,
    0b101'101'010'010'010,
    0b111'001'010'100'111,
};

constexpr uint16_t glyph(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return letters[ch - 'A'];
    }
    switch (ch) {
        case '1': return 0b010'110'010'010'111;
        case '2': return 0b110'001'010'100'111;
        case '8': return 0b111'101'111'101'111;
        default: return 0;
    }
}

template <size_t Count>
constexpr void text(VmuBitmap &bitmap, const char (&value)[Count], int y, int scale = 1) {
    constexpr int count = Count - 1;
    const int start = (width - (4 * count - 1) * scale) / 2;
    for (int i = 0; i < count; ++i) {
        const auto bits = glyph(value[i]);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (bits & (1u << (14 - (3 * row + col)))) {
                    box(bitmap, start + (4 * i + col) * scale, y + row * scale, scale, scale);
                }
            }
        }
    }
}

constexpr VmuBitmap make_menu() {
    VmuBitmap bitmap{};
    text(bitmap, "FRETS", 4, 2);
    text(bitmap, "ON FIRE", 18);
    return bitmap;
}

constexpr VmuBitmap make_pause() {
    VmuBitmap bitmap{};
    box(bitmap, 17, 5, 5, 13);
    box(bitmap, 26, 5, 5, 13);
    text(bitmap, "PAUSED", 22);
    return bitmap;
}

constexpr VmuBitmap make_result() {
    VmuBitmap bitmap{};
    box(bitmap, 14, 5, 2, 14);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 6; ++col) {
            if ((row + col) % 2 == 0) {
                box(bitmap, 16 + col * 3, 5 + row * 3, 3, 3);
            }
        }
    }
    text(bitmap, "RESULT", 22);
    return bitmap;
}

constexpr VmuBitmap make_credits() {
    VmuBitmap bitmap{};
    text(bitmap, "DREAMCAST", 3);
    text(bitmap, "PORT BY", 10);
    text(bitmap, "ANGELO", 17);
    text(bitmap, "128BITS", 24);
    return bitmap;
}

constexpr auto menu = make_menu();
constexpr auto pause = make_pause();
constexpr auto result = make_result();
constexpr auto credits = make_credits();

constexpr void sparkle(VmuBitmap &bitmap, int x, int y) {
    box(bitmap, x - 1, y, 3, 1);
    box(bitmap, x, y - 1, 1, 3);
}

} // namespace

void render_vmu_playfield(VmuBitmap &bitmap, const Chart &chart, const Round &round,
                          int time_ms, uint8_t held_frets, bool lefty) {
    bitmap = board;
    std::array<uint32_t, lane_centers.size()> heads{};
    std::array<uint32_t, lane_centers.size()> tails{};
    // The closest unplayed chord is at Round::next. Draw at most a small
    // fixed number so very dense custom charts cannot stall the game.
    for (size_t i = round.next; i < chart.size() && i - round.next < max_visible_chords; ++i) {
        const auto &chord = chart[i];
        const int offset = chord.time_ms - time_ms;
        if (offset > lookahead_ms) {
            break;
        }
        const int row = note_row(offset);
        for (unsigned lane = 0; lane < lane_centers.size(); ++lane) {
            if (chord.frets & (1u << lane)) {
                heads[lane] |= 1u << row;
                tails[lane] |= tail_rows(note_row(chord.ends[lane] - time_ms), row);
            }
        }
    }
    const auto *sustain = round.sustaining_chord(chart);
    const bool hit_flash = round.hits && time_ms >= round.last_hit_ms &&
                           time_ms - round.last_hit_ms < 128;
    const unsigned active = held_frets | (hit_flash ? round.last_hit_frets : 0u);
    for (unsigned lane = 0; lane < lane_centers.size(); ++lane) {
        const int x = lane_centers[lefty ? lane_centers.size() - 1 - lane : lane];
        if (sustain && (round.sustain_frets & (1u << lane))) {
            tails[lane] |= tail_rows(note_row(sustain->ends[lane] - time_ms), hit_row);
        }
        for (auto rows = tails[lane]; rows; rows &= rows - 1) {
            pixel(bitmap, x, __builtin_ctz(rows));
        }
        for (auto rows = heads[lane]; rows; rows &= rows - 1) {
            note(bitmap, x, __builtin_ctz(rows));
        }
        if (active & (1u << lane)) {
            box(bitmap, x - 1, 29, 3, 2);
        }
    }
}

void render_vmu_status(VmuBitmap &bitmap, VmuStatus status, unsigned phase) {
    switch (status) {
        case VmuStatus::menu:
            bitmap = menu;
            for (unsigned lane = 0; lane < lane_centers.size(); ++lane) {
                note(bitmap, lane_centers[lane], lane == phase % lane_centers.size() ? 26 : 29);
            }
            break;
        case VmuStatus::paused:
            bitmap = pause;
            break;
        case VmuStatus::result:
            bitmap = result;
            break;
        case VmuStatus::credits:
            bitmap = credits;
            sparkle(bitmap, 4, phase % 2 ? 18 : 23);
            sparkle(bitmap, width - 5, phase % 2 ? 23 : 18);
            break;
    }
}

} // namespace dc
