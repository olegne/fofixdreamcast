// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dc/maple.h>
#include <dc/maple/keyboard.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <memory>

namespace dc {
namespace {

using S = Setting;
using K = KeyAction;
using P = SettingsPage;

constexpr SettingRow categories[] = {
    {S::game, "Game Settings"}, {S::keys, "Key Settings"},
    {S::video, "Video Settings"}, {S::audio, "Audio Settings"},
};
constexpr SettingRow gameplay[] = {
    {S::mods, "Mod settings"}, {S::language, "Language"}, {S::lefty, "Lefty mode"},
    {S::tapping, "Tappable notes"}, {S::upload, "Upload Highscores"}, {S::apply, "Apply New Settings"},
};
constexpr SettingRow mods[] = {
    {S::chilly, "Chilly"}, {S::light_graphics, "LightGraphics"}, {S::apply, "Apply New Settings"},
};
constexpr SettingRow keys[] = {
    {S::test_keys, "Test Keys"}, {S::binding, "Pick", K::pick},
    {S::binding, "Secondary Pick", K::secondary_pick},
    {S::binding, "Fret #1", K::fret1}, {S::binding, "Fret #2", K::fret2},
    {S::binding, "Fret #3", K::fret3}, {S::binding, "Fret #4", K::fret4},
    {S::binding, "Fret #5", K::fret5}, {S::binding, "Move left", K::left},
    {S::binding, "Move right", K::right}, {S::binding, "Move up", K::up},
    {S::binding, "Move down", K::down}, {S::binding, "Cancel", K::cancel},
};
constexpr SettingRow video[] = {
    {S::resolution, "Video Resolution"}, {S::fullscreen, "Fullscreen"},
    {S::fps, "Frames per Second"}, {S::antialiasing, "Antialiasing"},
    {S::text_scale, "Text scale"}, {S::apply, "Apply New Settings"},
};
constexpr SettingRow audio[] = {
    {S::volume, "Volume Settings"}, {S::delay, "A/V delay"},
    {S::frequency, "Sample Frequency"}, {S::sample_bits, "Sample Bits"},
    {S::buffer, "Buffer Size"}, {S::apply, "Apply New Settings"},
};
constexpr SettingRow volume[] = {
    {S::guitar, "Guitar Volume"}, {S::song, "Song Volume"},
    {S::rhythm, "Rhythm Volume"}, {S::miss, "Screw Up Sounds"}, {S::apply, "Apply New Settings"},
};

template <unsigned N>
constexpr auto rows(const SettingRow (&table)[N]) {
    return std::make_pair(table, N);
}

auto page_rows(P page) {
    switch (page) {
        case P::categories: return rows(categories);
        case P::game: return rows(gameplay);
        case P::mods: return rows(mods);
        case P::keys: return rows(keys);
        case P::video: return rows(video);
        case P::audio: return rows(audio);
        case P::volume: return rows(volume);
        case P::pause_volume: return std::make_pair(&volume[0], 4u);
        case P::key_test: return rows(keys);
    }
    return rows(categories);
}

// Two alternating packages preserve the previous valid configuration if a write fails.
constexpr const char *filenames[] = {"FOFSET0", "FOFSET1"};
constexpr char app_id[16] = "FRETS ON FIRE";
constexpr uint8_t signature[] = {'F', 'O', 'S', 1};
constexpr unsigned payload_size = 24 + unsigned(K::count);

struct Snapshot {
    Settings settings;
    uint32_t generation = 0;
    int slot = -1;
};

std::array<uint8_t, payload_size> encode(const Settings &settings, uint32_t generation) {
    std::array<uint8_t, payload_size> bytes{};
    std::copy(std::begin(signature), std::end(signature), bytes.begin());
    for (unsigned i = 0; i < 4; ++i) {
        bytes[4 + i] = generation >> (i * 8);
    }
    bytes[8] = settings.delay_ms;
    bytes[9] = settings.delay_ms >> 8;
    bytes[10] = settings.text_scale;
    bytes[11] = settings.guitar_level;
    bytes[12] = settings.song_level;
    bytes[13] = settings.rhythm_level;
    bytes[14] = settings.miss_level;
    bytes[15] = settings.difficulty;
    bytes[16] = settings.language;
    bytes[17] = settings.lefty | (settings.tapping << 1);
    bytes[18] = settings.mods;
    bytes[19] = settings.upload_scores;
    bytes[20] = settings.video.low_resolution;
    bytes[21] = settings.video.half_refresh;
    bytes[22] = settings.video.horizontal_aa;
    std::copy(settings.keys.begin(), settings.keys.end(), bytes.begin() + 24);
    return bytes;
}

bool decode(const uint8_t *bytes, Settings &settings) {
    if (std::memcmp(bytes, signature, sizeof(signature)) != 0) {
        return false;
    }
    settings.delay_ms = bytes[8] | (unsigned(bytes[9]) << 8);
    settings.text_scale = bytes[10];
    settings.guitar_level = bytes[11];
    settings.song_level = bytes[12];
    settings.rhythm_level = bytes[13];
    settings.miss_level = bytes[14];
    settings.difficulty = bytes[15];
    settings.language = bytes[16];
    settings.lefty = bytes[17] & 1;
    settings.tapping = bytes[17] & 2;
    settings.mods = bytes[18];
    settings.upload_scores = bytes[19] != 0;
    settings.video = {bytes[20] != 0, bytes[21] != 0, bytes[22] != 0};
    std::copy_n(bytes + 24, settings.keys.size(), settings.keys.begin());
    const auto valid_volume = [](unsigned level) { return level <= 100 && level % 10 == 0; };
    return settings.delay_ms <= 300 && settings.text_scale >= 50 && settings.text_scale <= 250 &&
           settings.text_scale % 10 == 0 && valid_volume(settings.guitar_level) &&
           valid_volume(settings.song_level) && valid_volume(settings.rhythm_level) &&
           (settings.miss_level == 0 || settings.miss_level == 25 ||
            settings.miss_level == 50 || settings.miss_level == 100) &&
           settings.difficulty < 4 && bytes[17] <= 3 && settings.mods <= 3 && bytes[19] <= 1 &&
           bytes[20] <= 1 && bytes[21] <= 1 && bytes[22] <= 1 &&
           std::all_of(settings.keys.begin(), settings.keys.end(), valid_key);
}

Snapshot read_slot(maple_device_t *device, int slot) {
    void *raw = nullptr;
    int size = 0;
    const int result = vmufs_read(device, filenames[slot], &raw, &size);
    const std::unique_ptr<void, decltype(&std::free)> file(raw, &std::free);
    if (result || size < int(sizeof(vmu_hdr_t) + payload_size)) {
        return {};
    }
    const auto &header = *static_cast<const vmu_hdr_t *>(raw);
    if (std::memcmp(header.app_id, app_id, sizeof(app_id)) != 0 ||
        header.icon_cnt || header.eyecatch_type || header.data_len != payload_size) {
        return {};
    }
    vmu_pkg_t package{};
    Snapshot snapshot;
    if (vmu_pkg_parse(static_cast<uint8_t *>(raw), size, &package) ||
        !decode(package.data, snapshot.settings)) {
        return {};
    }
    for (unsigned i = 0; i < 4; ++i) {
        snapshot.generation |= uint32_t(package.data[4 + i]) << (i * 8);
    }
    snapshot.slot = slot;
    return snapshot;
}

Snapshot read_newest(maple_device_t *device) {
    const auto first = read_slot(device, 0);
    const auto second = read_slot(device, 1);
    if (!first.generation || (second.generation && int32_t(second.generation - first.generation) > 0)) {
        return second;
    }
    return first;
}

void log_settings(const char *action, const Settings &settings, uint32_t generation) {
    std::printf("FoFDC: settings %s generation=%lu delay=%u text=%u lefty=%u tapping=%u "
                "guitar=%u song=%u rhythm=%u miss=%u difficulty=%u language=%u mods=%u keys=",
                action, static_cast<unsigned long>(generation), settings.delay_ms, settings.text_scale,
                settings.lefty, settings.tapping, settings.guitar_level, settings.song_level,
                settings.rhythm_level, settings.miss_level, settings.difficulty, settings.language,
                settings.mods);
    for (const auto key : settings.keys) {
        std::printf("%02x", key);
    }
    std::printf(" upload_scores=%u resolution=%s half_refresh=%u horizontal_aa=%u\n",
                unsigned(settings.upload_scores), settings.video.low_resolution ? "320x240" : "640x480",
                unsigned(settings.video.half_refresh), unsigned(settings.video.supersampled()));
}

template <typename Value>
bool nudge(Value &value, int amount, int minimum, int maximum) {
    const int next_value = int(value) + amount;
    const auto next = static_cast<Value>(next_value < minimum ? maximum : next_value > maximum ? minimum : next_value);
    const bool changed = next != value;
    value = next;
    return changed;
}

} // namespace

Settings::Settings()
    : keys{KBD_KEY_ENTER, 0xe5, KBD_KEY_F1, KBD_KEY_F2, KBD_KEY_F3, KBD_KEY_F4, KBD_KEY_F5,
           KBD_KEY_LEFT, KBD_KEY_RIGHT, KBD_KEY_UP, KBD_KEY_DOWN, KBD_KEY_ESCAPE} {}

void Settings::load() {
    for (int index = 0; auto *card = maple_enum_type(index, MAPLE_FUNC_MEMCARD); ++index) {
        const auto snapshot = read_newest(card);
        if (snapshot.generation) {
            *this = snapshot.settings;
            log_settings("loaded", *this, snapshot.generation);
            return;
        }
    }
    log_settings("defaults", *this, 0);
}

void Settings::save() const {
    maple_device_t *card = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (!card) {
        std::puts("FoFDC: settings in RAM (no VMU)");
        return;
    }
    // Match the card discovered by load even if an empty card precedes it.
    Snapshot old = read_newest(card);
    for (int index = 1; !old.generation; ++index) {
        auto *next = maple_enum_type(index, MAPLE_FUNC_MEMCARD);
        if (!next) {
            break;
        }
        const auto snapshot = read_newest(next);
        if (snapshot.generation) {
            card = next;
            old = snapshot;
        }
    }
    if (old.generation && encode(old.settings, old.generation) == encode(*this, old.generation)) {
        return;
    }
    const uint32_t generation = old.generation + 1 ? old.generation + 1 : 1;
    auto data = encode(*this, generation);
    vmu_pkg_t package{};
    uint8_t dummy = 0;
    std::strcpy(package.desc_short, "FoF Settings");
    std::strcpy(package.desc_long, "Frets on Fire 1.3.110 settings");
    std::memcpy(package.app_id, app_id, sizeof(app_id));
    package.icon_data = &dummy;
    package.eyecatch_data = &dummy;
    package.data_len = data.size();
    package.data = data.data();

    uint8_t *raw = nullptr;
    int size = 0;
    const int status = vmu_pkg_build(&package, &raw, &size);
    const std::unique_ptr<uint8_t, decltype(&std::free)> file(raw, &std::free);
    std::array<uint8_t, 512> block{};
    if (status || size <= 0 || size > int(block.size())) {
        std::puts("FoFDC: settings in RAM (VMU package error)");
        return;
    }
    std::copy_n(raw, size, block.begin());
    const int slot = old.slot == 0 ? 1 : 0;
    const int result = vmufs_write(card, filenames[slot], block.data(), block.size(), VMUFS_OVERWRITE);
    if (result) {
        std::printf("FoFDC: settings in RAM (VMU write error %d)\n", result);
        return;
    }
    log_settings("saved", *this, generation);
}

void SettingsWriter::queue(const Settings &settings) {
    next_ = settings;
    queued_ = true;
    poll();
}

void SettingsWriter::poll() {
    if (thread_ && done_.load(std::memory_order_acquire)) {
        thd_join(thread_, nullptr);
        thread_ = nullptr;
    }
    if (!thread_ && queued_) {
        start();
    }
}

void SettingsWriter::flush() {
    while (thread_ || queued_) {
        poll();
        if (thread_) {
            thd_join(thread_, nullptr);
            thread_ = nullptr;
        }
    }
}

void SettingsWriter::start() {
    writing_ = next_;
    queued_ = false;
    done_.store(false, std::memory_order_relaxed);
    thread_ = thd_create(false, write, this);
    if (!thread_) {
        std::puts("FoFDC: settings in RAM (VMU worker error)");
    }
}

void *SettingsWriter::write(void *context) {
    auto &writer = *static_cast<SettingsWriter *>(context);
    writer.writing_.save();
    writer.done_.store(true, std::memory_order_release);
    return nullptr;
}

void SettingsMenu::open(P new_page, unsigned row) {
    page = new_page;
    selected = row;
    offset = 0;
    capture = false;
    test_held = 0;
}

void SettingsMenu::move(int direction) {
    const auto count = row_count();
    selected = (selected + count + direction) % count;
    if (selected < offset) {
        offset = selected;
    } else if (selected >= offset + 6) {
        offset = selected - 5;
    }
}

bool SettingsMenu::back() {
    switch (page) {
        case P::game: open(P::categories, 0); break;
        case P::mods: open(P::game); break;
        case P::keys: open(P::categories, 1); break;
        case P::video: open(P::categories, 2); break;
        case P::audio: open(P::categories, 3); break;
        case P::volume: open(P::audio); break;
        case P::key_test: open(P::keys); break;
        default: return false;
    }
    return true;
}

unsigned SettingsMenu::row_count() const { return page_rows(page).second; }
const SettingRow &SettingsMenu::row(unsigned index) const { return page_rows(page).first[index]; }

bool valid_key(unsigned key) {
    return (key >= KBD_KEY_A && key <= KBD_KEY_S3) || (key >= 0xe0 && key <= 0xe7);
}

std::string key_name(unsigned key) {
    if (key >= KBD_KEY_A && key <= KBD_KEY_Z) {
        return std::string(1, char('A' + key - KBD_KEY_A));
    }
    if (key >= KBD_KEY_1 && key <= KBD_KEY_0) {
        return std::string(1, char('0' + (1 + key - KBD_KEY_1) % 10));
    }
    if (key >= KBD_KEY_F1 && key <= KBD_KEY_F12) {
        return "F" + std::to_string(1 + key - KBD_KEY_F1);
    }
    if (key >= 0xe0 && key <= 0xe7) {
        constexpr const char *modifiers[] = {"Left Ctrl", "Left Shift", "Left Alt", "Left Meta",
                                             "Right Ctrl", "Right Shift", "Right Alt", "Right Meta"};
        return modifiers[key - 0xe0];
    }
    if (key >= KBD_KEY_PAD_1 && key <= KBD_KEY_PAD_0) {
        return "Keypad " + std::to_string((1 + key - KBD_KEY_PAD_1) % 10);
    }
    switch (key) {
        case KBD_KEY_ENTER: return "Return";
        case KBD_KEY_ESCAPE: return "Escape";
        case KBD_KEY_BACKSPACE: return "Backspace";
        case KBD_KEY_TAB: return "Tab";
        case KBD_KEY_SPACE: return "Space";
        case KBD_KEY_MINUS: return "-";
        case KBD_KEY_PLUS: return "=";
        case KBD_KEY_LBRACKET: return "[";
        case KBD_KEY_RBRACKET: return "]";
        case KBD_KEY_BACKSLASH: return "Backslash";
        case KBD_KEY_SEMICOLON: return ";";
        case KBD_KEY_QUOTE: return "Quote";
        case KBD_KEY_TILDE: return "Backquote";
        case KBD_KEY_COMMA: return ",";
        case KBD_KEY_PERIOD: return ".";
        case KBD_KEY_SLASH: return "/";
        case KBD_KEY_CAPSLOCK: return "Caps lock";
        case KBD_KEY_PRINT: return "Print screen";
        case KBD_KEY_SCRLOCK: return "Scroll lock";
        case KBD_KEY_PAUSE: return "Pause";
        case KBD_KEY_INSERT: return "Insert";
        case KBD_KEY_HOME: return "Home";
        case KBD_KEY_PGUP: return "Page up";
        case KBD_KEY_DEL: return "Delete";
        case KBD_KEY_END: return "End";
        case KBD_KEY_PGDOWN: return "Page down";
        case KBD_KEY_RIGHT: return "Right";
        case KBD_KEY_LEFT: return "Left";
        case KBD_KEY_DOWN: return "Down";
        case KBD_KEY_UP: return "Up";
        case KBD_KEY_PAD_NUMLOCK: return "Numlock";
        case KBD_KEY_PAD_DIVIDE: return "Keypad /";
        case KBD_KEY_PAD_MULTIPLY: return "Keypad *";
        case KBD_KEY_PAD_MINUS: return "Keypad -";
        case KBD_KEY_PAD_PLUS: return "Keypad +";
        case KBD_KEY_PAD_ENTER: return "Keypad Enter";
        case KBD_KEY_PAD_PERIOD: return "Keypad .";
        default: return "Key " + std::to_string(key);
    }
}

bool setting_is_volume(S setting) {
    return setting == S::guitar || setting == S::song || setting == S::rhythm || setting == S::miss;
}

bool setting_adjustable(const Settings &settings, S setting) {
    switch (setting) {
        case S::antialiasing: return settings.video.low_resolution;
        case S::binding:
        case S::fullscreen:
        case S::frequency:
        case S::sample_bits:
        case S::buffer: return false;
        default: return true;
    }
}

bool adjust_setting(Settings &settings, S setting, int direction, unsigned languages) {
    if (!direction || !setting_adjustable(settings, setting)) {
        return false;
    }
    switch (setting) {
        case S::language: return languages && nudge(settings.language, direction, 0, languages - 1);
        case S::lefty: return nudge(settings.lefty, direction, 0, 1);
        case S::tapping: return nudge(settings.tapping, direction, 0, 1);
        case S::upload: return nudge(settings.upload_scores, direction, 0, 1);
        case S::chilly: settings.mods ^= 1; return true;
        case S::light_graphics: settings.mods ^= 2; return true;
        case S::delay: return nudge(settings.delay_ms, direction, 0, 300);
        case S::resolution: return nudge(settings.video.low_resolution, direction, 0, 1);
        case S::fps: return nudge(settings.video.half_refresh, direction, 0, 1);
        case S::antialiasing: return nudge(settings.video.horizontal_aa, direction, 0, 1);
        case S::text_scale: return nudge(settings.text_scale, 10 * direction, 50, 250);
        case S::guitar: return nudge(settings.guitar_level, 10 * direction, 0, 100);
        case S::song: return nudge(settings.song_level, 10 * direction, 0, 100);
        case S::rhythm: return nudge(settings.rhythm_level, 10 * direction, 0, 100);
        case S::miss: {
            constexpr uint8_t levels[] = {50, 0, 100, 25}; // Loud, Off, Painful, Quiet
            unsigned index = std::find(std::begin(levels), std::end(levels), settings.miss_level) - levels;
            if (!nudge(index, direction, 0, 3)) {
                return false;
            }
            settings.miss_level = levels[index];
            return true;
        }
        default: return false;
    }
}

std::string setting_value(const Settings &settings, const SettingRow &row) {
    unsigned level;
    switch (row.setting) {
        case S::chilly: return settings.mods & 1 ? "Yes" : "No";
        case S::light_graphics: return settings.mods & 2 ? "Yes" : "No";
        case S::lefty: return settings.lefty ? "Yes" : "No";
        case S::tapping: return settings.tapping ? "Yes" : "No";
        case S::upload: return settings.upload_scores ? "Yes" : "No";
        case S::resolution: return settings.video.low_resolution ? "320x240" : "640x480";
        case S::fullscreen: return "Yes";
        case S::fps: return settings.video.half_refresh ? "Half refresh" : "Video refresh";
        case S::antialiasing:
            if (!settings.video.low_resolution) {
                return "320x240 only";
            }
            return settings.video.supersampled() ? "2x horizontal" : "Off";
        case S::text_scale: return std::to_string(settings.text_scale) + "%";
        case S::delay: return std::to_string(settings.delay_ms);
        case S::frequency: return "44100 Hz";
        case S::sample_bits: return "16";
        case S::buffer: return "AICA";
        case S::binding: return key_name(settings.key(row.action));
        case S::guitar: level = settings.guitar_level; break;
        case S::song: level = settings.song_level; break;
        case S::rhythm: level = settings.rhythm_level; break;
        case S::miss:
            switch (settings.miss_level) {
                case 0: return "Off";
                case 25: return "Quiet";
                case 50: return "Loud";
                default: return "Painful";
            }
        default: return {};
    }
    std::string value = level == 100 ? "" : "0";
    value += std::to_string(level / 10) + "/10";
    return value;
}

} // namespace dc
