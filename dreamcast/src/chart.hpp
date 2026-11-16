// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dc {

struct Chord {
    int time_ms;
    uint8_t frets;
};

using Chart = std::vector<Chord>;

// Standard MIDI format 0 or 1, five-fret Expert guitar.
bool load_chart(const char *path, Chart &chart, std::string &error);

class Round {
public:
    static constexpr int hit_window_ms = 100;

    void advance(const Chart &chart, int time_ms);
    bool strum(const Chart &chart, int time_ms, uint8_t frets);
    void finish(const Chart &chart);

    size_t next = 0;
    unsigned score = 0;
    unsigned hits = 0;
    unsigned misses = 0;
    unsigned overstrums = 0;
    unsigned streak = 0;
    unsigned best_streak = 0;
    unsigned multiplier() const;
};

} // namespace dc
