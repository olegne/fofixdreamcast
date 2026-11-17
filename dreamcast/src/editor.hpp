// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"
#include "song_library.hpp"
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct maple_device;

namespace dc {

struct EditorLabel {
    std::string key;
    std::string title;
};

inline constexpr std::array<std::string_view, 3> stock_label_keys{"@bangbang", "@defy", "@twibmpg"};
bool valid_label_key(std::string_view key);
std::string editor_label_path(std::string_view key);
std::vector<EditorLabel> editor_labels();

class SongEditor {
public:
    void open(const SongEntry &entry, SongChart chart);
    void move_lane(int step);
    void move_quarter(int step);
    void change_difficulty(int step);
    void follow_audio(int time_ms);
    void update(unsigned elapsed_ms, int held_direction, bool notes_held, bool playing);
    void begin_note(uint8_t frets);
    void end_note();
    void remove_note();
    bool set_field(unsigned field, const std::string &text);
    bool set_tempo(double bpm);
    void set_label(const std::string &label);
    std::string field(unsigned field) const;
    bool has_changes() const { return modified_; }
    bool chart_changed() const { return chart_modified_; }
    void mark_saved() { modified_ = chart_modified_ = false; }
    const SongChart &chart() const { return chart_; }
    const SongEntry &entry() const { return entry_; }
    unsigned difficulty() const { return difficulty_; }
    unsigned lane() const { return lane_; }
    int position_ms() const { return int(position_us_ / 1000.0); }
    float scroll_ms() const { return float(scroll_us_ / 1000.0); }
    std::vector<uint8_t> midi() const;

private:
    double quarter_us() const { return chart_.initial_us_per_beat / 4.0; }
    int64_t snap_position() const;
    void write_note();

    SongEntry entry_;
    SongChart chart_;
    unsigned difficulty_ = 0;
    unsigned lane_ = 0;
    double position_us_ = 0.0;
    double scroll_us_ = 0.0;
    float speed_ = 0.0f;
    int64_t note_start_ = 0;
    int64_t note_end_ = -1;
    uint8_t note_frets_ = 0;
    bool modified_ = false;
    bool chart_modified_ = false;
};

class SongEdits {
public:
    enum class SaveResult { saved, in_memory, failed };

    void load(std::vector<SongEntry> &songs);
    bool open(const SongEntry &entry, SongChart &chart, std::string &error,
              bool allow_empty = false) const;
    const std::vector<uint8_t> *midi_override(const SongEntry &entry) const;
    bool has_playable_edit(const SongEntry &entry) const;
    void update_metadata(SongEntry &entry) const;
    SaveResult save(const SongEditor &editor, std::string &message);

private:
    struct Edit {
        std::vector<uint8_t> midi;
        std::string title;
        std::string artist;
        std::string label;
        int delay_ms = 0;
        uint32_t color = 0;
        uint32_t generation = 0;
        uint16_t crc = 0;
        int device_port = -1;
        int device_unit = 0;
        int slot = -1;
        bool playable = false;
    };

    static bool read_slot(maple_device *device, const std::string &path, int slot, Edit &edit);
    static bool persist(const std::string &path, Edit &edit, std::string &message);
    std::map<std::string, Edit> entries_;
};

} // namespace dc
