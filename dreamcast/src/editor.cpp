// SPDX-License-Identifier: GPL-2.0-or-later
#include "editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <utility>

namespace dc {
namespace {

constexpr int64_t max_position_us = 3600ll * 1000000;
constexpr int64_t track_bucket_us = 50000;
constexpr unsigned midi_ppqn = 480;
constexpr size_t max_field_length = 60;
constexpr size_t max_labels = 256;
constexpr char label_dir[] = "/cd/labels";

int64_t starts(const Chord &chord) {
    return int64_t(chord.time_ms) * 1000 + chord.start_us_remainder;
}

int64_t ends(const Chord &chord, unsigned lane) {
    return int64_t(chord.ends[lane]) * 1000 + chord.end_us_remainders[lane];
}

void set_end(Chord &chord, unsigned lane, int64_t time) {
    chord.ends[lane] = int(time / 1000);
    chord.end_us_remainders[lane] = uint16_t(time % 1000);
}

bool contains_event(const Chord &chord, unsigned lane, int64_t first, int64_t last) {
    // The editor uses the same half-open range of 50 ms buckets as Track.getEvents.
    first /= track_bucket_us;
    last /= track_bucket_us;
    return first < last && starts(chord) / track_bucket_us < last &&
           ends(chord, lane) / track_bucket_us >= first;
}

void keep_in_range(double &time) {
    time = std::clamp(time, 0.0, double(max_position_us));
}

void midi_delta(std::vector<uint8_t> &output, uint32_t ticks) {
    uint8_t data[4];
    unsigned count = 0;
    data[count++] = uint8_t(ticks & 127);
    while ((ticks >>= 7)) {
        data[count++] = uint8_t((ticks & 127) | 128);
    }
    while (count) {
        output.push_back(data[--count]);
    }
}

bool has_guitar_notes(const SongChart &chart) {
    return std::any_of(chart.guitar.begin(), chart.guitar.end(), [](const Chart &difficulty) {
        return !difficulty.empty();
    });
}

} // namespace

bool valid_label_key(std::string_view key) {
    if (key.empty() || std::find(stock_label_keys.begin(), stock_label_keys.end(), key) != stock_label_keys.end()) {
        return true;
    }
    // The disc authoring tool limits Joliet basenames. Never let a save select an
    // arbitrary path or a hidden file through the label extension.
    return key.size() <= 48 && key.front() != '.' &&
           std::all_of(key.begin(), key.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_';
           });
}

std::string editor_label_path(std::string_view key) {
    if (key.empty() || key.front() == '@' || !valid_label_key(key)) {
        return {};
    }
    return std::string(label_dir) + "/" + std::string(key);
}

std::vector<EditorLabel> editor_labels() {
    std::vector<EditorLabel> labels{
        {"", "Original cassette label"},
        {std::string(stock_label_keys[0]), "Bang Bang, Mystery Man"},
        {std::string(stock_label_keys[1]), "Defy The Machine"},
        {std::string(stock_label_keys[2]), "This Week I've Been Mostly Playing Guitar"},
    };
    auto *dir = opendir(label_dir);
    if (!dir) {
        return labels;
    }
    const size_t first = labels.size();
    while (auto *file = readdir(dir)) {
        const std::string_view name(file->d_name);
        constexpr std::string_view extension = ".texz";
        if (name.size() <= extension.size() ||
            name.substr(name.size() - extension.size()) != extension) {
            continue;
        }
        const auto key = name.substr(0, name.size() - extension.size());
        if (!editor_label_path(key).empty()) {
            labels.push_back({std::string(key), std::string(key)});
        }
        if (labels.size() == max_labels) {
            break;
        }
    }
    closedir(dir);
    std::sort(labels.begin() + first, labels.end(), [](const EditorLabel &a, const EditorLabel &b) {
        return a.title < b.title;
    });
    return labels;
}

void SongEditor::open(const SongEntry &entry, SongChart chart) {
    *this = {};
    entry_ = entry;
    chart_ = std::move(chart);
}

void SongEditor::move_lane(int step) {
    lane_ = unsigned((int(lane_) + 5 + step) % 5);
}

void SongEditor::move_quarter(int step) {
    position_us_ = snap_position() + step * quarter_us();
    keep_in_range(position_us_);
}

void SongEditor::change_difficulty(int step) {
    end_note();
    difficulty_ = unsigned((int(difficulty_) + 4 + step) % 4);
}

void SongEditor::follow_audio(int time_ms) {
    position_us_ = double(time_ms - entry_.delay_ms) * 1000.0;
    keep_in_range(position_us_);
}

int64_t SongEditor::snap_position() const {
    return int64_t(std::round(position_us_ / quarter_us()) * quarter_us() + 0.5);
}

void SongEditor::update(unsigned elapsed_ms, int held_direction, bool notes_held, bool playing) {
    if (!playing && held_direction) {
        position_us_ += held_direction * double(chart_.initial_us_per_beat) * speed_;
        keep_in_range(position_us_);
        speed_ = std::min(2.0f, speed_ + elapsed_ms / 4096.0f);
    } else {
        speed_ = 0.0f;
    }
    if (note_frets_) {
        if (!notes_held || snap_position() < note_start_) {
            end_note();
        } else {
            write_note();
        }
    }
    scroll_us_ = playing ? position_us_ : (scroll_us_ + snap_position()) * 0.5;
}

void SongEditor::begin_note(uint8_t frets) {
    note_frets_ = frets ? uint8_t(frets & 31u) : uint8_t(1u << lane_);
    note_start_ = snap_position();
    note_end_ = -1;
    write_note();
}

void SongEditor::end_note() {
    note_frets_ = 0;
}

void SongEditor::write_note() {
    const auto end = std::min(max_position_us, std::max(note_start_ + int64_t(quarter_us() + 0.5),
                                                       snap_position()));
    if (end == note_end_ || end <= note_start_) {
        return;
    }
    auto &track = chart_.guitar[difficulty_];
    note_end_ = end;
    for (auto &chord : track) {
        const auto start = starts(chord);
        for (unsigned lane = 0; lane < 5; ++lane) {
            const unsigned fret = 1u << lane;
            if (!(chord.frets & note_frets_ & fret) ||
                (start != note_start_ && !contains_event(chord, lane, note_start_, note_end_))) {
                continue;
            }
            if (start < note_start_) {
                set_end(chord, lane, note_start_);
            } else {
                chord.frets &= ~fret;
                chord.special_frets &= ~fret;
                set_end(chord, lane, 0);
            }
        }
    }
    track.erase(std::remove_if(track.begin(), track.end(), [](const Chord &chord) {
        return !chord.frets;
    }), track.end());

    auto chord = std::lower_bound(track.begin(), track.end(), note_start_,
        [](const Chord &item, int64_t time) { return starts(item) < time; });
    if (chord == track.end() || starts(*chord) != note_start_) {
        Chord created{int(note_start_ / 1000)};
        created.start_us_remainder = uint16_t(note_start_ % 1000);
        chord = track.insert(chord, created);
    }
    chord->frets |= note_frets_;
    chord->hopo = false;
    for (unsigned lane = 0; lane < 5; ++lane) {
        if (note_frets_ & (1u << lane)) {
            set_end(*chord, lane, note_end_);
        }
    }
    modified_ = chart_modified_ = true;
}

void SongEditor::remove_note() {
    end_note();
    auto &track = chart_.guitar[difficulty_];
    const auto start = snap_position();
    const auto end = start + int64_t(quarter_us() + 0.5);
    const auto fret = 1u << lane_;
    for (auto &chord : track) {
        if ((chord.frets & fret) && contains_event(chord, lane_, start, end)) {
            chord.frets &= ~fret;
            chord.special_frets &= ~fret;
            set_end(chord, lane_, 0);
            modified_ = chart_modified_ = true;
        }
    }
    track.erase(std::remove_if(track.begin(), track.end(), [](const Chord &chord) {
        return !chord.frets;
    }), track.end());
}

bool SongEditor::set_field(unsigned field, const std::string &text) {
    char *end;
    if (field == 1 || field == 2) {
        if (text.empty() || text.size() > max_field_length) {
            return false;
        }
        (field == 1 ? entry_.title : entry_.artist) = text;
    } else if (field == 3 || field == 4) {
        const double bpm = std::strtod(text.c_str(), &end);
        return end != text.c_str() && !*end && set_tempo(bpm);
    } else if (field == 5) {
        const long delay = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end || delay < -10000 || delay > 10000) {
            return false;
        }
        entry_.delay_ms = int(delay);
    } else if (field == 6) {
        if (text.size() != 7 || text[0] != '#') {
            return false;
        }
        const auto color = std::strtoul(text.c_str() + 1, &end, 16);
        if (*end || color > 0xffffffu || !std::all_of(text.begin() + 1, text.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
        })) {
            return false;
        }
        entry_.cassette_color = 0xff000000u | uint32_t(color);
    } else {
        return false;
    }
    modified_ = true;
    return true;
}

bool SongEditor::set_tempo(double bpm) {
    if (!std::isfinite(bpm) || bpm < 3.58 || bpm > 600.0) {
        return false;
    }
    const auto tempo = unsigned(60000000.0 / bpm);
    if (chart_.initial_us_per_beat != tempo) {
        chart_.initial_us_per_beat = tempo;
        chart_.tempos = {{0, tempo}};
        modified_ = chart_modified_ = true;
    }
    return true;
}

void SongEditor::set_label(const std::string &label) {
    if (valid_label_key(label) && entry_.cassette_label != label) {
        entry_.cassette_label = label;
        modified_ = true;
    }
}

std::string SongEditor::field(unsigned field) const {
    switch (field) {
        case 1: return entry_.title;
        case 2: return entry_.artist;
        case 3: {
            char bpm[24];
            std::snprintf(bpm, sizeof(bpm), "%.3f", 60000000.0 / chart_.initial_us_per_beat);
            return bpm;
        }
        case 5: return std::to_string(entry_.delay_ms);
        case 6: {
            char color[8];
            std::snprintf(color, sizeof(color), "#%06lX", static_cast<unsigned long>(entry_.cassette_color & 0xffffff));
            return color;
        }
        default: return {};
    }
}

std::vector<uint8_t> SongEditor::midi() const {
    struct Event {
        uint32_t tick;
        uint8_t pitch;
        uint8_t velocity;
    };
    std::vector<Event> events;
    const auto tempo = chart_.initial_us_per_beat;
    const auto tick = [tempo](int64_t usec) { return uint32_t(usec * midi_ppqn / tempo); };
    for (unsigned difficulty = 0; difficulty < chart_.guitar.size(); ++difficulty) {
        for (const auto &chord : chart_.guitar[difficulty]) {
            const auto start = starts(chord);
            const auto first = tick(start);
            for (unsigned lane = 0; lane < 5; ++lane) {
                if (chord.frets & (1u << lane)) {
                    const auto pitch = uint8_t(60 + 12 * (3 - difficulty) + lane);
                    const auto velocity = uint8_t((chord.special_frets & (1u << lane)) ? 127 : 100);
                    // The 1.3.110 writer quantizes onset and duration independently to PPQN 480.
                    events.push_back({first, pitch, velocity});
                    events.push_back({first + std::max<uint32_t>(1u, tick(ends(chord, lane) - start)), pitch, 0});
                }
            }
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.tick != b.tick) {
            return a.tick < b.tick;
        }
        return !a.velocity && b.velocity;
    });
    std::vector<uint8_t> output{
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(midi_ppqn >> 8), uint8_t(midi_ppqn),
        'M', 'T', 'r', 'k', 0, 0, 0, 0,
        0, 0xff, 0x51, 3, uint8_t(tempo >> 16), uint8_t(tempo >> 8), uint8_t(tempo),
    };
    uint32_t previous = 0;
    for (const auto &event : events) {
        midi_delta(output, event.tick - previous);
        output.insert(output.end(), {uint8_t(event.velocity ? 0x90 : 0x80), event.pitch, event.velocity});
        previous = event.tick;
    }
    output.insert(output.end(), {0, 0xff, 0x2f, 0});
    const auto size = uint32_t(output.size() - 22);
    for (unsigned i = 0; i < 4; ++i) {
        output[18 + i] = uint8_t(size >> (24 - 8 * i));
    }
    return output;
}

bool SongEdits::open(const SongEntry &entry, SongChart &chart, std::string &error, bool allow_empty) const {
    const auto edit = entries_.find(entry.midi);
    if (edit != entries_.end()) {
        return parse_song_midi(edit->second.midi, chart, error, allow_empty);
    }
    if (allow_empty && entry.blank_chart) {
        chart = {};
        chart.initial_us_per_beat = 60000000 / 122;
        chart.tempos = {{0, chart.initial_us_per_beat}};
        return true;
    }
    return load_song_chart(entry.midi.c_str(), chart, error, allow_empty);
}

const std::vector<uint8_t> *SongEdits::midi_override(const SongEntry &entry) const {
    const auto edit = entries_.find(entry.midi);
    return edit == entries_.end() ? nullptr : &edit->second.midi;
}

bool SongEdits::has_playable_edit(const SongEntry &entry) const {
    const auto edit = entries_.find(entry.midi);
    return edit != entries_.end() && edit->second.playable;
}

void SongEdits::update_metadata(SongEntry &entry) const {
    const auto it = entries_.find(entry.midi);
    if (it == entries_.end()) {
        return;
    }
    const auto &edit = it->second;
    entry.title = edit.title;
    entry.artist = edit.artist;
    entry.delay_ms = edit.delay_ms;
    entry.cassette_color = edit.color;
    entry.cassette_label = edit.label;
}

SongEdits::SaveResult SongEdits::save(const SongEditor &editor, std::string &message) {
    const auto &source = editor.entry();
    std::vector<uint8_t> midi;
    if (editor.chart_changed()) {
        midi = editor.midi();
    } else if (const auto previous = entries_.find(source.midi); previous != entries_.end()) {
        midi = previous->second.midi;
    } else if (source.blank_chart) {
        midi = editor.midi();
    } else if (!load_song_midi(source.midi.c_str(), midi, message)) {
        return SaveResult::failed;
    }
    SongChart saved_chart;
    if (!parse_song_midi(midi, saved_chart, message, true)) {
        return SaveResult::failed;
    }
    auto &edit = entries_[source.midi];
    edit.midi = std::move(midi);
    edit.title = source.title;
    edit.artist = source.artist;
    edit.label = source.cassette_label;
    edit.delay_ms = source.delay_ms;
    edit.color = source.cassette_color;
    edit.playable = has_guitar_notes(saved_chart);
    const auto saved = persist(source.midi, edit, message);
    return saved ? SaveResult::saved : SaveResult::in_memory;
}

} // namespace dc
