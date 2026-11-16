// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dc {

struct Chord {
    int time_ms;
    std::array<int, 5> ends{};
    uint8_t frets = 0;
    bool hopo = false;
    // The renderer uses milliseconds. Judgement and sustain scoring retain fractions.
    uint16_t start_us_remainder = 0;
    std::array<uint16_t, 5> end_us_remainders{};
    uint8_t special_frets = 0;
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
    // Frets on Fire difficulty numbers: Amazing, Medium, Easy, Supaeasy.
    std::array<Chart, 4> guitar;
    std::vector<TempoPoint> tempos;
    std::vector<Beat> beats;
    unsigned initial_us_per_beat = 500000;
    unsigned us_per_beat(int time_ms) const;
};

bool load_song_chart(const char *path, SongChart &song, std::string &error,
                     bool allow_empty = false);
bool parse_song_midi(const std::vector<uint8_t> &bytes, SongChart &song, std::string &error,
                     bool allow_empty = false);

inline unsigned fret_count(uint8_t mask) {
    return static_cast<unsigned>(__builtin_popcount(mask));
}

struct Rules {
    explicit Rules(float bpm = 120, int delay_ms = 0, bool tapping_enabled = true);

    float early_ms;
    float late_ms;
    float release_ms;
    float sustain_threshold_ms;
    int window_us;
    int release_us;
    int sustain_threshold_us;
    int delay_ms;
    bool tapping;
};

enum class Pick { neutral, hit, bad };

class Round {
public:
    Pick advance(const Chart &chart, int time_ms, uint8_t held, const Rules &rules);
    Pick pick(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool tap);
    Pick fret_edge(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool released);
    void finish(const Chart &chart, int time_ms, const Rules &rules);
    uint8_t required_frets(const Chart &chart, int time_ms, const Rules &rules) const;
    unsigned multiplier() const;
    unsigned display_score(int time_ms, const Rules &rules) const;
    unsigned rating(unsigned gems) const;
    int sustained_until() const { return sustain_end_ms_; }
    bool was_hit(size_t target) const { return target < played_.size() && played_[target]; }
    const Chord *sustaining_chord(const Chart &chart) const {
        return sustain_frets && last_hit_target_ < chart.size() ? &chart[last_hit_target_] : nullptr;
    }

    size_t next = 0;
    unsigned score = 0;
    unsigned hits = 0;
    unsigned misses = 0;
    unsigned gem_hits = 0;
    unsigned gem_misses = 0;
    unsigned overstrums = 0;
    unsigned streak = 0;
    unsigned best_streak = 0;
    bool guitar_muted = false;
    uint8_t sustain_frets = 0;
    uint8_t last_hit_frets = 0;
    int last_hit_ms = -1000;
    int last_hit_offset_ms = 0;
    int last_miss_ms = -1000;

private:
    void miss(const Chord &chord);
    void end_sustain(int time_ms, const Rules &rules);
    unsigned sustain_points(int time_ms, const Rules &rules) const;
    size_t required_target(const Chart &chart, int time_ms, const Rules &rules) const;

    int64_t sustain_start_us_ = 0;
    int64_t sustain_length_us_ = 0;
    int64_t sustain_end_us_ = 0;
    int64_t last_gem_end_us_ = 0;
    int sustain_end_ms_ = 0;
    std::optional<int> last_pick_ms_;
    std::optional<int> pending_pick_ms_;
    bool sustain_tappable_ = false;
    size_t last_hit_target_ = 0;
    std::vector<bool> played_;
};

} // namespace dc
