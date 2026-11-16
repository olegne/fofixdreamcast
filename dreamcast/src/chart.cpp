// SPDX-License-Identifier: GPL-2.0-or-later
#include "chart.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
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
};

bool is_guitar(std::string_view name) {
    return name == "PART GUITAR" || name == "T1 GEMS" || name == "Click" ||
           name == "Midi Out";
}

bool is_gem(uint8_t pitch) {
    return pitch >= 60 && pitch <= 100 && (pitch - 60) % 12 < 5;
}

bool is_phrase(uint8_t pitch) {
    return pitch == 103 || pitch == 116;
}

bool read_track(Reader track, std::vector<Tempo> &tempos,
                std::vector<Gem> &gems, std::string_view &name) {
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
            if (*type == 0x03 && name.empty()) {
                name = {reinterpret_cast<const char *>(payload), length};
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
        if ((command != 0x80 && command != 0x90) || (!is_gem(pitch) && !is_phrase(pitch))) {
            continue;
        }
        auto &start = held[(status & 0x0f) * 128 + pitch];
        if (command == 0x90 && data[1]) {
            start = tick;
        } else if (start != unheld) {
            if (gems.size() == max_events) {
                return false;
            }
            gems.push_back({start, tick, pitch});
            start = unheld;
        }
    }
    return false;
}

bool matches(uint8_t frets, uint8_t held) {
    if (fret_count(frets) > 1) {
        return frets == held;
    }
    // Holding lower frets is legal during a single note.
    return (held & ~(frets - 1)) == frets;
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
    std::vector<Tempo> tempos{{0, 500000}};
    for (unsigned i = 0; i < tracks; ++i) {
        chunk = file.take(8);
        if (!chunk || std::memcmp(chunk, "MTrk", 4) != 0) {
            error = "Invalid MIDI track.";
            return false;
        }
        const auto track_size = be32(chunk + 4);
        const auto *start = file.take(track_size);
        std::vector<Gem> gems;
        std::string_view name;
        if (!start || !read_track({start, start + track_size}, tempos, gems, name)) {
            error = "Invalid MIDI event.";
            return false;
        }
        if (guitar.empty() && (is_guitar(name) || (tracks == 1 && name.empty())) &&
            std::any_of(gems.begin(), gems.end(), [](const Gem &gem) { return is_gem(gem.pitch); })) {
            guitar = std::move(gems);
        }
    }
    if (guitar.empty()) {
        error = "No five-fret guitar chart in this MIDI.";
        return false;
    }

    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
    SongChart parsed;
    const unsigned divisor = unsigned(ppqn) * 1000;
    for (size_t i = 0; i < tempos.size(); ++i) {
        auto &tempo = tempos[i];
        if (i) {
            const auto &prev = tempos[i - 1];
            tempo.scaled_time = prev.scaled_time + uint64_t(tempo.tick - prev.tick) * prev.us_per_beat;
        }
        if (tempo.scaled_time / divisor <= max_song_ms) {
            parsed.tempos.push_back({static_cast<int>(tempo.scaled_time / divisor), tempo.us_per_beat});
        }
    }

    const auto time_of = [&](uint32_t tick) {
        const auto it = std::upper_bound(tempos.begin(), tempos.end(), tick,
                                         [](uint32_t value, const Tempo &tempo) {
                                             return value < tempo.tick;
                                         });
        const auto &tempo = *(it - 1);
        return (tempo.scaled_time + uint64_t(tick - tempo.tick) * tempo.us_per_beat) / divisor;
    };

    std::stable_sort(guitar.begin(), guitar.end(),
                     [](const Gem &a, const Gem &b) { return a.tick < b.tick; });
    std::vector<Gem> phrases;
    uint32_t last_tick = 0;
    for (const auto &gem : guitar) {
        if (is_phrase(gem.pitch)) {
            phrases.push_back(gem);
            continue;
        }
        const auto time = time_of(gem.tick);
        const auto end = time_of(gem.end);
        if (end > max_song_ms) {
            error = "The chart exceeds the maximum playback length.";
            return false;
        }
        auto &track = parsed.guitar[3 - (gem.pitch - 60) / 12];
        const auto fret = (gem.pitch - 60) % 12;
        if (track.empty() || track.back().time_ms != static_cast<int>(time)) {
            track.push_back({static_cast<int>(time)});
        }
        track.back().frets |= 1u << fret;
        track.back().ends[fret] = std::max(track.back().ends[fret], static_cast<int>(end));
        last_tick = std::max(last_tick, gem.end);
    }

    for (auto &track : parsed.guitar) {
        for (size_t i = 1; i < track.size(); ++i) {
            auto &chord = track[i];
            const auto &previous = track[i - 1];
            const auto quarter_ms = parsed.us_per_beat(chord.time_ms) / 1000.0f;
            chord.hopo = fret_count(chord.frets) == 1 && chord.frets != previous.frets &&
                         chord.time_ms - previous.time_ms <= quarter_ms * (170.0f / 480.0f);
        }
        if (phrases.size() < 2) {
            unsigned total = 0;
            for (const auto &target : track) {
                total += fret_count(target.frets);
            }
            const unsigned phrases_in_track = total / 120;
            if (phrases_in_track > 0) {
                const unsigned spacing = total / phrases_in_track;
                const unsigned offset = phrases_in_track / 4;
                unsigned ordinal = 0;
                for (auto &target : track) {
                    const auto end = ordinal + fret_count(target.frets);
                    for (auto gem = ordinal; gem < end; ++gem) {
                        if (gem >= offset && (gem - offset) / spacing < phrases_in_track &&
                            (gem - offset) % spacing <= 10) {
                            target.power_phrase = static_cast<int>((gem - offset) / spacing);
                            target.phrase_end |= (gem - offset) % spacing == 10;
                        }
                    }
                    ordinal = end;
                }
            }
            continue;
        }
        for (size_t i = 0; i < phrases.size(); ++i) {
            const auto start = time_of(phrases[i].tick);
            const auto end = time_of(phrases[i].end);
            if (start >= end || end > max_song_ms) {
                continue;
            }
            const auto at = [](const Chord &chord, int time_ms) { return chord.time_ms < time_ms; };
            const auto first = std::lower_bound(track.begin(), track.end(), static_cast<int>(start), at);
            const auto last = std::lower_bound(first, track.end(), static_cast<int>(end), at);
            for (auto it = first; it != last; ++it) {
                it->power_phrase = static_cast<int>(i);
                it->phrase_end = it + 1 == last;
            }
        }
    }
    // FoFiX draws the bar, beat and half-beat lines independently of the notes.
    const auto final_tick = std::min<uint64_t>(uint64_t(last_tick) + 2u * ppqn, unheld - 1);
    for (uint64_t eighth = 0; eighth * ppqn / 2 <= final_tick; ++eighth) {
        if (parsed.beats.size() == max_events) {
            error = "Too many beats in this MIDI.";
            return false;
        }
        const auto tick = static_cast<uint32_t>(eighth * ppqn / 2);
        const auto time = time_of(tick);
        parsed.beats.push_back({static_cast<int>(time), eighth % 8 == 0 ? 2u : unsigned(eighth % 2 == 0)});
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

Rules::Rules(float bpm) {
    const auto capped = std::min(bpm, 200.0f);
    late_ms = 250.0f - capped / 5.0f - 70.0f * 1.2f;
    early_ms = late_ms;
    release_ms = 200.0f - capped / 5.0f - 70.0f * 0.5f;
    sustain_threshold_ms = 1.1f * 60000.0f / bpm / 4.0f;
}

unsigned Round::multiplier() const {
    return (1 + std::min(streak / 10, 3u)) * (power_active ? 2 : 1);
}

unsigned Round::sustain_points(int time_ms, const Rules &rules) const {
    const int duration = std::min(time_ms, sustain_end_ms_) - sustain_start_ms_;
    return sustain_frets && duration > rules.sustain_threshold_ms
               ? static_cast<unsigned>(0.1f * duration * fret_count(sustain_frets))
               : 0u;
}

unsigned Round::display_score(int time_ms, const Rules &rules) const {
    return score + sustain_points(time_ms, rules) * multiplier();
}

unsigned Round::rating(unsigned gems, size_t targets, int time_ms, const Rules &rules) const {
    if (!gems || !targets) {
        return 0;
    }
    // The default FoFiX RB+GH thresholds use average score per gem.
    const float average = display_score(time_ms, rules) / (gems * 50.0f);
    if (average > 5.3f) {
        return 6;
    }
    constexpr float thresholds[] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f};
    return std::upper_bound(std::begin(thresholds), std::end(thresholds), average) -
           std::begin(thresholds);
}

void Round::end_sustain(int time_ms, const Rules &rules) {
    score += sustain_points(time_ms, rules) * multiplier();
    sustain_frets = 0;
}

bool Round::activate_power() {
    if (power_active || power < 50.0f) {
        return false;
    }
    power_active = true;
    return true;
}

void Round::miss(const Chord &chord) {
    rock_loss_ += 2.0f / (power_active ? 2.0f : 1.0f);
    rock = std::max(0.0f, rock - rock_loss_ / (power_active ? 2.0f : 1.0f));
    rock_gain_ = std::max(15.0f, rock_gain_ - 14.0f);
    ++misses;
    streak = 0;
    chain_ = last_was_tap_ = false;
    guitar_muted = true;
    if (chord.power_phrase >= 0) {
        broken_phrase_ = chord.power_phrase;
    }
}

void Round::advance(const Chart &chart, int time_ms, uint8_t held, const Rules &rules) {
    if (previous_time_ms_ >= 0 && power_active) {
        power = std::max(0.0f, power - std::max(0, time_ms - previous_time_ms_) / 200.0f);
        power_active = power > 0.0f;
    }
    previous_time_ms_ = time_ms;

    if (sustain_frets && (time_ms >= sustain_end_ms_ || (held & sustain_frets) != sustain_frets)) {
        if (time_ms + rules.release_ms < sustain_end_ms_) {
            guitar_muted = true;
        }
        end_sustain(time_ms, rules);
    }
    while (next < chart.size() && time_ms > chart[next].time_ms + rules.late_ms) {
        miss(chart[next++]);
    }
}

Pick Round::pick(const Chart &chart, int time_ms, uint8_t held, const Rules &rules, bool tap) {
    if (!held || time_ms < 0 || (tap && !chain_)) {
        return Pick::neutral;
    }
    size_t target = next;
    while (target < chart.size() && chart[target].time_ms <= time_ms + rules.early_ms) {
        const auto &chord = chart[target];
        if (matches(chord.frets, held) && (!tap || chord.hopo)) {
            break;
        }
        if (tap) {
            return Pick::neutral;
        }
        ++target;
    }
    if (target == chart.size() || chart[target].time_ms > time_ms + rules.early_ms) {
        if (tap || (last_was_tap_ && time_ms - last_hit_ms <= rules.late_ms &&
                    matches(last_hit_frets, held))) {
            return Pick::neutral;
        }
        end_sustain(time_ms, rules);
        ++overstrums;
        streak = 0;
        chain_ = last_was_tap_ = false;
        rock_loss_ += 0.4f;
        rock = std::max(0.0f, rock - rock_loss_ / 5.0f);
        rock_gain_ = std::max(15.0f, rock_gain_ - 2.8f);
        guitar_muted = true;
        return Pick::bad;
    }

    end_sustain(time_ms, rules);
    while (next < target) {
        miss(chart[next++]);
    }
    const auto &chord = chart[next++];
    ++hits;
    ++streak;
    best_streak = std::max(streak, best_streak);
    score += 50 * fret_count(chord.frets) * multiplier();
    rock_gain_ += 7.0f;
    rock = std::min(30000.0f, rock + rock_gain_ * (power_active ? 2.0f : 1.0f));
    if (chord.phrase_end && chord.power_phrase != broken_phrase_) {
        power = std::min(100.0f, power + 25.0f);
    }
    chain_ = true;
    last_was_tap_ = tap;
    guitar_muted = false;
    last_hit_frets = chord.frets;
    last_hit_ms = time_ms;
    last_hit_offset_ms = time_ms - chord.time_ms;

    sustain_start_ms_ = std::max(time_ms, chord.time_ms);
    sustain_end_ms_ = std::numeric_limits<int>::max();
    for (unsigned i = 0; i < chord.ends.size(); ++i) {
        if (chord.frets & (1u << i)) {
            sustain_end_ms_ = std::min(sustain_end_ms_, chord.ends[i]);
        }
    }
    if (sustain_end_ms_ - sustain_start_ms_ > rules.sustain_threshold_ms) {
        sustain_frets = chord.frets;
    }
    return Pick::hit;
}

void Round::finish(const Chart &chart, int time_ms, const Rules &rules) {
    end_sustain(time_ms, rules);
    misses += static_cast<unsigned>(chart.size() - next);
    next = chart.size();
}

} // namespace dc
