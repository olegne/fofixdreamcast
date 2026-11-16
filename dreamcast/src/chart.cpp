// SPDX-License-Identifier: GPL-2.0-or-later
#include "chart.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace dc {
namespace {

constexpr size_t max_chart_bytes = 2 * 1024 * 1024;
constexpr size_t max_events = 65536;
constexpr int max_song_ms = 60 * 60 * 1000;
constexpr uint32_t unheld = std::numeric_limits<uint32_t>::max();

struct Reader {
    const uint8_t *current;
    const uint8_t *end;

    size_t remaining() const { return static_cast<size_t>(end - current); }

    const uint8_t *take(size_t count) {
        if (count > remaining()) {
            return nullptr;
        }
        const auto *start = current;
        current += count;
        return start;
    }

    bool varint(uint32_t &value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto *byte = take(1);
            if (!byte) {
                return false;
            }
            value = (value << 7) | (*byte & 0x7f);
            if (!(*byte & 0x80)) {
                return true;
            }
        }
        return false;
    }
};

uint16_t be16(const uint8_t *data) {
    return (uint16_t(data[0]) << 8) | data[1];
}

uint32_t be32(const uint8_t *data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) | data[3];
}

struct Tempo {
    uint32_t tick;
    unsigned us_per_beat;
    uint64_t scaled_time = 0;
};

struct Gem {
    uint32_t tick;
    uint32_t end;
    uint8_t pitch;
    unsigned tempo_at_off;
    bool tappable = false;
};

bool is_gem(uint8_t pitch) {
    return pitch >= 60 && pitch <= 100 && (pitch - 60) % 12 < 5;
}

bool read_track(Reader track, std::vector<Tempo> &tempos,
                std::vector<Gem> &gems, bool has_notes) {
    uint32_t tick = 0;
    uint8_t running_status = 0;
    std::array<uint32_t, 16 * 128> held;
    held.fill(unheld);
    while (track.remaining()) {
        uint32_t delta;
        if (!track.varint(delta) || delta >= unheld - tick || !track.remaining()) {
            return false;
        }
        tick += delta;

        uint8_t status = *track.current;
        if (status & 0x80) {
            track.take(1);
        } else {
            status = running_status;
            if (!status) {
                return false;
            }
        }

        if (status == 0xff) {
            const auto *type = track.take(1);
            uint32_t length;
            if (!type || !track.varint(length)) {
                return false;
            }
            const auto *payload = track.take(length);
            if (!payload) {
                return false;
            }
            if (*type == 0x2f) {
                return length == 0 && !track.remaining();
            }
            if (*type == 0x51) {
                if (length != 3 || tempos.size() == max_events) {
                    return false;
                }
                const auto us_per_beat = (unsigned(payload[0]) << 16) |
                                         (unsigned(payload[1]) << 8) | payload[2];
                if (!us_per_beat) {
                    return false;
                }
                tempos.push_back({tick, us_per_beat});
            }
            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            running_status = 0;
            uint32_t length;
            if (!track.varint(length) || !track.take(length)) {
                return false;
            }
            continue;
        }
        if (status >= 0xf0) {
            return false;
        }
        const auto command = status & 0xf0;
        const size_t length = command == 0xc0 || command == 0xd0 ? 1 : 2;
        const auto *data = track.take(length);
        if (!data || (data[0] & 0x80) || (data[length - 1] & 0x80)) {
            return false;
        }
        running_status = status;
        const auto pitch = data[0];
        if (!has_notes || (command != 0x80 && command != 0x90) || !is_gem(pitch)) {
            continue;
        }
        auto &start = held[(status & 0x0f) * 128 + pitch];
        if (command == 0x90 && data[1]) {
            start = tick;
        } else if (start != unheld) {
            if (gems.size() == max_events) {
                return false;
            }
            gems.push_back({start, tick, pitch, tempos.empty() ? 500000u : tempos.back().us_per_beat});
            start = unheld;
        }
    }
    return false;
}

bool matches(uint8_t frets, uint8_t held) {
    const auto highest_required = 32u - __builtin_clz(unsigned(frets));
    // Every required fret must be down. Extra frets below the highest required
    // fret are legal even when the target is a chord.
    return (held & frets) == frets && !(held >> highest_required);
}

int64_t start_us(const Chord &chord) {
    return int64_t(chord.time_ms) * 1000 + chord.start_us_remainder;
}

bool before_last_bucket(const Chord &chord, int64_t window_end_us) {
    // Track.getEvents omits the 50 ms bucket containing its upper bound.
    return chord.time_ms / 50 < window_end_us / 50000;
}

bool parse_chart(const std::vector<uint8_t> &bytes, SongChart &song, std::string &error) {
    Reader file{bytes.data(), bytes.data() + bytes.size()};
    auto *chunk = file.take(8);
    if (!chunk || std::memcmp(chunk, "MThd", 4) != 0) {
        error = "Invalid MIDI header.";
        return false;
    }
    const auto header_size = be32(chunk + 4);
    const auto *header = file.take(header_size);
    if (!header || header_size < 6) {
        error = "Incomplete MIDI header.";
        return false;
    }
    const auto format = be16(header);
    const auto tracks = be16(header + 2);
    const auto ppqn = be16(header + 4);
    if (format > 1 || !tracks || (format == 0 && tracks != 1) || !ppqn || (ppqn & 0x8000)) {
        error = "Use a type 0 or 1 MIDI with PPQN timing.";
        return false;
    }

    std::vector<Gem> guitar;
    std::vector<Tempo> tempos;
    for (unsigned i = 0; i < tracks; ++i) {
        chunk = file.take(8);
        if (!chunk || std::memcmp(chunk, "MTrk", 4) != 0) {
            error = "Invalid MIDI track.";
            return false;
        }
        const auto track_size = be32(chunk + 4);
        const auto *start = file.take(track_size);
        // The 1.3.110 MidiReader looks at the first two MIDI tracks, regardless
        // of their names. Later tracks can still supply tempo metadata.
        if (!start || !read_track({start, start + track_size}, tempos, guitar, i < 2)) {
            error = "Invalid MIDI event.";
            return false;
        }
    }
    if (guitar.empty()) {
        error = "No five-fret guitar chart in this MIDI.";
        return false;
    }

    SongChart parsed;
    if (!tempos.empty()) {
        parsed.initial_us_per_beat = tempos.front().us_per_beat;
    }
    tempos.insert(tempos.begin(), {0, parsed.initial_us_per_beat});
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
    for (size_t i = 0; i < tempos.size(); ++i) {
        auto &tempo = tempos[i];
        if (i) {
            const auto &prev = tempos[i - 1];
            tempo.scaled_time = prev.scaled_time + uint64_t(tempo.tick - prev.tick) * prev.us_per_beat;
        }
        const auto time_ms = tempo.scaled_time / ppqn / 1000;
        if (time_ms <= max_song_ms) {
            parsed.tempos.push_back({static_cast<int>(time_ms), tempo.us_per_beat});
        }
    }

    const auto time_of = [&](uint32_t tick) {
        const auto it = std::upper_bound(tempos.begin(), tempos.end(), tick,
                                         [](uint32_t value, const Tempo &tempo) {
                                             return value < tempo.tick;
                                         });
        const auto &tempo = *(it - 1);
        return double(tempo.scaled_time + uint64_t(tick - tempo.tick) * tempo.us_per_beat) / ppqn;
    };

    struct TapGroup {
        std::vector<Gem *> gems;
        double ticks = 0.0;
    };
    std::array<TapGroup, 4> previous_groups;
    std::array<TapGroup, 4> current_groups;
    // Track.update visits NoteOff insertion order before sorting for display.
    // It classifies each pending group when a new group begins.
    for (auto &gem : guitar) {
        const auto difficulty = 3 - (gem.pitch - 60) / 12;
        auto &previous = previous_groups[difficulty];
        auto &current = current_groups[difficulty];
        const double ticks = time_of(gem.tick) * 480 / gem.tempo_at_off;
        if (ticks >= current.ticks + 1e-3) {
            if (previous.gems.size() == 1) {
                const auto &prior = *previous.gems.front();
                const double length = time_of(prior.end) - time_of(prior.tick);
                const double previous_end = previous.ticks + length * 480 / gem.tempo_at_off;
                if (current.ticks - previous_end <= 161 &&
                    std::none_of(current.gems.begin(), current.gems.end(),
                                 [&](const Gem *note) { return note->pitch == prior.pitch; })) {
                    for (auto *note : current.gems) {
                        note->tappable = true;
                    }
                }
            }
            std::swap(previous, current);
            current.gems.clear();
            current.ticks = ticks;
        }
        current.gems.push_back(&gem);
    }

    std::stable_sort(guitar.begin(), guitar.end(),
                     [](const Gem &a, const Gem &b) { return a.tick < b.tick; });
    std::array<uint32_t, 4> last_tick_by_difficulty{unheld, unheld, unheld, unheld};
    uint32_t last_tick = 0;
    for (const auto &gem : guitar) {
        const auto time_us = uint64_t(time_of(gem.tick));
        const auto end_us = uint64_t(time_of(gem.end));
        if (end_us / 1000 > max_song_ms) {
            error = "The chart exceeds the maximum playback length.";
            return false;
        }
        const auto difficulty = 3 - (gem.pitch - 60) / 12;
        auto &track = parsed.guitar[difficulty];
        const auto fret = (gem.pitch - 60) % 12;
        if (last_tick_by_difficulty[difficulty] != gem.tick) {
            track.push_back({static_cast<int>(time_us / 1000)});
            track.back().start_us_remainder = time_us % 1000;
            track.back().hopo = true;
            last_tick_by_difficulty[difficulty] = gem.tick;
        }
        auto &target = track.back();
        target.frets |= 1u << fret;
        target.hopo &= gem.tappable;
        if (end_us > uint64_t(target.ends[fret]) * 1000 + target.end_us_remainders[fret]) {
            target.ends[fret] = static_cast<int>(end_us / 1000);
            target.end_us_remainders[fret] = end_us % 1000;
        }
        last_tick = std::max(last_tick, gem.end);
    }

    const auto final_tick = std::min<uint64_t>(uint64_t(last_tick) + 2u * ppqn, unheld - 1);
    for (uint64_t eighth = 0; eighth * ppqn / 2 <= final_tick; ++eighth) {
        if (parsed.beats.size() == max_events) {
            error = "Too many beats in this MIDI.";
            return false;
        }
        const auto tick = static_cast<uint32_t>(eighth * ppqn / 2);
        const auto time = time_of(tick);
        parsed.beats.push_back({static_cast<int>(time / 1000), eighth % 8 == 0 ? 2u : unsigned(eighth % 2 == 0)});
    }
    song = std::move(parsed);
    return true;
}

} // namespace

bool load_song_chart(const char *path, SongChart &song, std::string &error) {
    auto *file = std::fopen(path, "rb");
    if (!file) {
        error = "Could not open notes.mid.";
        return false;
    }
    const long size = std::fseek(file, 0, SEEK_END) == 0 ? std::ftell(file) : -1;
    if (size < 14 || static_cast<size_t>(size) > max_chart_bytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "Invalid MIDI file length.";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        error = "Could not read notes.mid.";
        return false;
    }
    return parse_chart(bytes, song, error);
}

unsigned SongChart::us_per_beat(int time_ms) const {
    const auto it = std::upper_bound(tempos.begin(), tempos.end(), time_ms,
                                     [](int time, const TempoPoint &tempo) {
                                         return time < tempo.time_ms;
                                     });
    return it == tempos.begin() ? 500000u : (it - 1)->us_per_beat;
}

Rules::Rules(float bpm, int delay)
    : early_ms(60000.0f / bpm / 3.5f),
      late_ms(early_ms),
      release_ms(60000.0f / bpm / 2.0f),
      sustain_threshold_ms(1.1f * 60000.0f / bpm / 4.0f),
      window_us(static_cast<int>(early_ms * 1000)),
      release_us(static_cast<int>(release_ms * 1000)),
      sustain_threshold_us(static_cast<int>(sustain_threshold_ms * 1000)),
      delay_ms(delay) {}

unsigned Round::multiplier() const {
    return 1 + std::min(streak / 10, 3u);
}

unsigned Round::sustain_points(int time_ms, const Rules &rules) const {
    const auto duration = std::min(int64_t(time_ms) * 1000 - sustain_start_us_,
                                   sustain_length_us_);
    return sustain_frets && duration > rules.sustain_threshold_us
               ? static_cast<unsigned>(duration * fret_count(sustain_frets) / 10000)
               : 0u;
}

unsigned Round::display_score(int time_ms, const Rules &rules) const {
    return score + sustain_points(time_ms, rules);
}

unsigned Round::rating(unsigned gems) const {
    // The original uses floor(5 * (notesHit / totalNotes + 0.05)).
    return gems ? (20 * gem_hits + gems) / (4 * gems) : 0u;
}

void Round::end_sustain(int time_ms, const Rules &rules) {
    if (sustain_frets) {
        score += sustain_points(time_ms, rules) * multiplier();
        // Guitar.endPick uses the uncorrected audio time only for muting.
        if ((int64_t(time_ms) + rules.delay_ms) * 1000 + rules.release_us < last_gem_end_us_) {
            guitar_muted = true;
        }
        sustain_frets = 0;
    }
}

void Round::miss(const Chord &chord) {
    ++misses;
    gem_misses += fret_count(chord.frets);
}

size_t Round::required_target(const Chart &chart, int time_ms, const Rules &rules) const {
    const auto pos = int64_t(time_ms) * 1000;
    size_t index = next;
    while (index < chart.size() && start_us(chart[index]) < pos - rules.window_us) {
        ++index;
    }
    if (index < chart.size() && before_last_bucket(chart[index], pos + rules.window_us)) {
        return index;
    }
    return chart.size();
}

Pick Round::advance(const Chart &chart, int time_ms, uint8_t held, const Rules &rules) {
    const auto pos = int64_t(time_ms) * 1000;
    if (sustain_frets && pos > sustain_end_us_) {
        end_sustain(time_ms, rules);
    }
    while (next < chart.size() && start_us(chart[next]) < pos - rules.window_us) {
        miss(chart[next++]);
    }

    if (!sustain_frets) {
        const auto first = std::lower_bound(chart.begin(), chart.end(), pos - 2 * rules.window_us,
                                            [](const Chord &chord, int64_t time_us) {
                                                return start_us(chord) < time_us;
                                            });
        for (auto it = first; it != chart.end() && before_last_bucket(*it, pos - rules.window_us); ++it) {
            if (!was_hit(static_cast<size_t>(it - chart.begin()))) {
                guitar_muted = true;
                streak = 0;
                break;
            }
        }
    }

    if (pending_pick_ms_ && time_ms > *pending_pick_ms_) {
        pending_pick_ms_.reset();
        const auto target = required_target(chart, time_ms, rules);
        if (target < chart.size() && matches(chart[target].frets, held)) {
            return pick(chart, time_ms, held, rules, false);
        }
    }
    return Pick::neutral;
}

Pick Round::pick(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool tap) {
    if (!tap && !held) {
        pending_pick_ms_ = time_ms + 30;
        return Pick::neutral;
    }
    if (!tap) {
        pending_pick_ms_.reset();
    }

    const auto target = required_target(chart, time_ms, rules);
    const bool matched = target < chart.size() && matches(chart[target].frets, held);
    if (tap && (!streak || !matched || !chart[target].hopo)) {
        return Pick::neutral;
    }
    if (sustain_frets && sustain_tappable_ && target == chart.size() && last_pick_ms_ &&
        (int64_t(time_ms) - *last_pick_ms_) * 1000 <= rules.release_us) {
        return Pick::neutral;
    }

    end_sustain(time_ms, rules);
    last_pick_ms_ = time_ms;
    if (!matched) {
        ++overstrums;
        streak = 0;
        guitar_muted = true;
        last_miss_ms = time_ms;
        return Pick::bad;
    }

    while (next < target) {
        miss(chart[next++]);
    }
    const auto &chord = chart[next++];
    if (played_.empty()) {
        played_.resize(chart.size());
    }
    played_[target] = true;

    ++hits;
    const auto gem_count = fret_count(chord.frets);
    gem_hits += gem_count;
    ++streak;
    best_streak = std::max(streak, best_streak);
    score += 50 * gem_count * multiplier();
    guitar_muted = false;
    last_hit_frets = chord.frets;
    last_hit_ms = time_ms;
    last_hit_offset_ms = time_ms - chord.time_ms;

    const auto note_start = start_us(chord);
    sustain_start_us_ = std::max(int64_t(time_ms) * 1000, note_start);
    sustain_end_us_ = std::numeric_limits<int64_t>::max();
    last_gem_end_us_ = note_start;
    for (unsigned i = 0; i < chord.ends.size(); ++i) {
        if (chord.frets & (1u << i)) {
            const auto note_end = int64_t(chord.ends[i]) * 1000 + chord.end_us_remainders[i];
            sustain_end_us_ = std::min(sustain_end_us_, note_end);
            last_gem_end_us_ = std::max(last_gem_end_us_, note_end);
        }
    }
    sustain_end_ms_ = static_cast<int>(sustain_end_us_ / 1000);
    sustain_length_us_ = sustain_end_us_ - note_start;
    sustain_frets = chord.frets;
    sustain_tappable_ = chord.hopo;
    return Pick::hit;
}

Pick Round::fret_edge(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool released) {
    const auto result = pick(chart, time_ms, held, rules, true);
    if (result == Pick::neutral && released && last_pick_ms_ &&
        (int64_t(time_ms) - *last_pick_ms_) * 1000 > rules.release_us) {
        end_sustain(time_ms, rules);
    }
    return result;
}

void Round::finish(const Chart &chart, int, const Rules &) {
    // GuitarScene goes straight to results before processing another frame.
    sustain_frets = 0;
    while (next < chart.size()) {
        miss(chart[next++]);
    }
}

} // namespace dc
