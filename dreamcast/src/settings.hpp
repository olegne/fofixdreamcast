// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <kos/thread.h>
#include <string>

namespace dc {

enum class KeyAction : unsigned {
    pick, secondary_pick, fret1, fret2, fret3, fret4, fret5,
    left, right, up, down, cancel, count
};

struct Settings {
    Settings();
    uint8_t key(KeyAction action) const { return keys[static_cast<unsigned>(action)]; }

    std::array<uint8_t, static_cast<unsigned>(KeyAction::count)> keys{};
    uint16_t delay_ms = 100;
    uint16_t text_scale = 100;
    uint8_t guitar_level = 100;
    uint8_t song_level = 100;
    uint8_t rhythm_level = 100;
    uint8_t miss_level = 25;
    uint8_t difficulty = 2;
    uint8_t language = 0;
    uint8_t mods = 0;
    bool lefty = false;
    bool tapping = true;
    bool upload_scores = false;

    void load();
    void save() const;
};

class SettingsWriter {
public:
    ~SettingsWriter() { flush(); }
    void queue(const Settings &settings);
    void poll();
    void flush();

private:
    void start();
    static void *write(void *context);

    Settings next_;
    Settings writing_;
    kthread_t *thread_ = nullptr;
    bool queued_ = false;
    std::atomic<bool> done_{false};
};

enum class SettingsPage : uint8_t { categories, game, mods, keys, video, audio, volume, pause_volume, key_test };
enum class Setting : uint8_t {
    game, keys, video, audio, mods, language, lefty, tapping, upload, apply, test_keys, binding,
    resolution, fullscreen, fps, antialiasing, text_scale, volume, delay, frequency, sample_bits, buffer,
    guitar, song, rhythm, miss, chilly, light_graphics
};

struct SettingRow {
    Setting setting;
    const char *name;
    KeyAction action = KeyAction::count;
};

struct SettingsMenu {
    SettingsPage page = SettingsPage::categories;
    Settings draft;
    unsigned selected = 0;
    unsigned offset = 0;
    unsigned test_held = 0;
    bool capture = false;

    void open(SettingsPage new_page, unsigned row = 0);
    void move(int direction);
    bool back();
    unsigned row_count() const;
    const SettingRow &row(unsigned index) const;
};

bool valid_key(unsigned key);
std::string key_name(unsigned key);
bool setting_is_volume(Setting setting);
bool adjust_setting(Settings &settings, Setting setting, int direction, unsigned languages);
std::string setting_value(const Settings &settings, const SettingRow &row);

} // namespace dc
