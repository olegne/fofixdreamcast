// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dc {

struct Chord {
    int time_ms;
    std::array<int, 5> ends{};
    uint8_t frets = 0;
    bool hopo = false;
    int power_phrase = -1;
    bool phrase_end = false;
};

using Chart = std::vector<Chord>;

struct Beat {
    int time_ms;
    unsigned strength;
};

struct TempoPoint {
    int time_ms;
    unsigned us_per_beat;
};

struct SongChart {
    // FoFiX difficulty numbers: Expert, Hard, Medium, Easy.
    std::array<Chart, 4> guitar;
    std::vector<TempoPoint> tempos;
    std::vector<Beat> beats;
    unsigned us_per_beat(int time_ms) const;
};

bool load_song_chart(const char *path, SongChart &song, std::string &error);

inline unsigned fret_count(uint8_t mask) {
    return static_cast<unsigned>(__builtin_popcount(mask));
}

struct Rules {
    explicit Rules(float bpm = 120);

    float early_ms;
    float late_ms;
    float release_ms;
    float sustain_threshold_ms;
};

enum class Pick { neutral, hit, bad };

class Round {
public:
    void advance(const Chart &chart, int time_ms, uint8_t held, const Rules &rules);
    Pick pick(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool tap);
    bool activate_power();
    void finish(const Chart &chart, int time_ms, const Rules &rules);
    unsigned multiplier() const;
    unsigned display_score(int time_ms, const Rules &rules) const;
    unsigned rating(unsigned gems, size_t targets, int time_ms, const Rules &rules) const;
    int sustained_until() const { return sustain_end_ms_; }

    size_t next = 0;
    unsigned score = 0;
    unsigned hits = 0;
    unsigned misses = 0;
    unsigned overstrums = 0;
    unsigned streak = 0;
    unsigned best_streak = 0;
    float rock = 15000.0f;
    float power = 0.0f;
    bool power_active = false;
    bool guitar_muted = false;
    uint8_t sustain_frets = 0;
    uint8_t last_hit_frets = 0;
    int last_hit_ms = -1000;
    int last_hit_offset_ms = 0;

private:
    void miss(const Chord &chord);
    void end_sustain(int time_ms, const Rules &rules);
    unsigned sustain_points(int time_ms, const Rules &rules) const;

    float rock_gain_ = 15.0f;
    float rock_loss_ = 400.0f;
    int sustain_start_ms_ = 0;
    int sustain_end_ms_ = 0;
    bool chain_ = false;
    bool last_was_tap_ = false;
    int broken_phrase_ = -1;
    int previous_time_ms_ = -1;
};

} // namespace dc
