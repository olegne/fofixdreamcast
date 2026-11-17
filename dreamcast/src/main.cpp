// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"
#include "chart.hpp"
#include "chooser.hpp"
#include "language.hpp"
#include "ranking.hpp"
#include "settings.hpp"
#include "song_library.hpp"
#include "view.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <arch/arch.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/pvr.h>
#include <dc/sound/sound.h>
#include <kos/init.h>
#include <kos/thread.h>

extern "C" int __real_maple_queue_frame(maple_frame_t *frame);

extern "C" {
volatile uint8_t dc_gameplay_active = 0;
volatile uint32_t dc_gems_hit = 0;
volatile uint32_t dc_targets_hit = 0;
volatile uint32_t dc_targets_missed = 0;
volatile uint32_t dc_overstrums = 0;
volatile uint32_t dc_score = 0;
volatile int32_t dc_event_time_ms = 0;
volatile int32_t dc_last_hit_chart_ms = -1;
#ifdef FRETSONFIRE_INPUT_TRACE
volatile uint32_t dc_input_trace_count = 0;
volatile uint32_t dc_input_trace[128][8]{};
#endif
}

namespace {

void record_round(const dc::Round &round, int time_ms) {
    dc_gems_hit = round.gem_hits;
    dc_targets_hit = round.hits;
    dc_targets_missed = round.misses;
    dc_overstrums = round.overstrums;
    dc_score = round.score;
    dc_event_time_ms = time_ms;
    dc_last_hit_chart_ms = round.hits ? round.last_hit_ms - round.last_hit_offset_ms : -1;
}

enum class InputTrace : uint32_t { report = 1, fret, strum, advance, hit };

#ifdef FRETSONFIRE_INPUT_TRACE
void trace_input(InputTrace kind, int time_ms, unsigned detail, uint8_t frets, const dc::Round &round) {
    const auto index = dc_input_trace_count;
    auto &entry = dc_input_trace[index % 128];
    entry[0] = static_cast<uint32_t>(kind);
    entry[1] = static_cast<uint32_t>(time_ms);
    entry[2] = detail;
    entry[3] = frets;
    entry[4] = round.hits;
    entry[5] = round.misses;
    entry[6] = round.overstrums;
    entry[7] = round.score;
    dc_input_trace_count = index + 1;
}
#else
inline void trace_input(InputTrace, int, unsigned, uint8_t, const dc::Round &) {}
#endif

enum Effect : unsigned {
    accept, back, select1, select2, select3, start_song, miss1, miss2, miss3, miss4, miss5, miss6,
    effect_count
};

class SoundEffects {
public:
    void load() {
        constexpr const char *names[] = {
            "in", "out", "crunch1", "crunch2", "crunch3", "start",
            "fiba1", "fiba2", "fiba3", "fiba4", "fiba5", "fiba6",
        };
        static_assert(sizeof(names) / sizeof(names[0]) == effect_count);
        for (unsigned i = 0; i < effect_count; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "/rd/fx/%s.ogg", names[i]);
            if (!samples_[i].load(path)) {
                std::printf("FoFDC: could not load sound %s\n", path);
            }
        }
    }

    void play(Effect effect) {
        play_at_volume(effect, effect >= miss1 ? miss_level_ : guitar_level_, effect >= miss1);
    }

    void preview(unsigned volume) {
        play_at_volume(miss1, volume, false);
    }

    void set_mix(unsigned guitar, unsigned miss) {
        guitar_level_ = guitar;
        miss_level_ = miss;
    }

private:
    void play_at_volume(Effect effect, unsigned volume, bool replace_miss) {
        const auto &clip = samples_[effect];
        if (!clip.loaded()) {
            return;
        }
        // GuitarScene replaces the last mixer channel. Other effects take the first free channel.
        const auto target = replace_miss ? voices_.end() - 1 :
            std::find_if(voices_.begin(), voices_.end(), [](const auto &voice) {
                return !voice || (!voice->playing() && !voice->paused());
            });
        if (target == voices_.end()) {
            return;
        }
        auto &voice = *target;
        if (!voice) {
            voice.reset(new (std::nothrow) dc::Audio);
        }
        if (!voice || !voice->start_clip(clip, volume)) {
            std::printf("FoFDC: could not play sound %u\n", effect);
        }
    }

public:
    void poll() {
        for (const auto &voice : voices_) {
            if (voice && voice->playing()) {
                voice->poll();
            }
        }
    }

    void pause() {
        poll();
        for (const auto &voice : voices_) {
            if (voice && voice->playing()) {
                voice->pause();
            }
        }
    }

    void resume() {
        for (const auto &voice : voices_) {
            if (voice && voice->paused()) {
                voice->resume();
            }
        }
    }

private:
    unsigned guitar_level_ = 100;
    unsigned miss_level_ = 25;
    std::array<dc::AudioClip, effect_count> samples_;
    std::array<std::unique_ptr<dc::Audio>, 8> voices_;
};

struct Cue {
    int start, duration, picture;
    std::string text;
};

std::vector<Cue> read_tutorial_cues() {
    std::vector<Cue> cues;
    auto *script = std::fopen("/rd/tutorial/script.txt", "r");
    if (!script) {
        return cues;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), script)) {
        int start, duration, offset;
        char type[8];
        if (std::sscanf(line, "%d %d %7s %n", &start, &duration, type, &offset) != 3) {
            continue;
        }
        auto *content = line + offset;
        content[std::strcspn(content, "\r\n")] = '\0';
        if (std::strcmp(type, "text") == 0) {
            cues.push_back({start, duration, -1, content});
        } else if (std::strcmp(type, "pic") == 0) {
            constexpr const char *names[] = {"pose.svg", "keyboard.svg", "esc.svg"};
            for (int i = 0; i < 3; ++i) {
                if (std::strcmp(content, names[i]) == 0) {
                    cues.push_back({start, duration, i, {}});
                }
            }
        }
    }
    std::fclose(script);
    return cues;
}

void tutorial_cue(dc::Frame &frame, const std::vector<Cue> &cues) {
    size_t caption = 0;
    for (const auto &cue : cues) {
        const int elapsed = frame.time_ms - cue.start;
        if (elapsed < 0 || elapsed > cue.duration) {
            continue;
        }
        if (cue.picture < 0) {
            if (caption < frame.tutorial_captions.size()) {
                frame.tutorial_captions[caption++] = cue.text.c_str();
            }
        } else {
            const float in = std::min(1.0f, elapsed / 500.0f);
            const float out = std::min(1.0f, (cue.duration - elapsed) / 500.0f);
            frame.tutorial_picture = cue.picture;
            const float blend = 1.0f - in * out;
            frame.picture_fade = blend * blend;
        }
    }
}

static dc::Audio audio;
static dc::Audio menu_audio;
static dc::Audio collection_audio[2];
static dc::ResultAudio result_audio;
static SoundEffects sounds;

void service_audio_while_loading() {
    audio.poll();
    menu_audio.poll();
    for (auto &preview : collection_audio) {
        preview.poll();
    }
    result_audio.poll(false);
    sounds.poll();
}

enum InputButton : unsigned {
    nav_up = 1u << 0, nav_down = 1u << 1, nav_left = 1u << 2, nav_right = 1u << 3,
    select_option = 1u << 4, cancel_option = 1u << 5, pause_song = 1u << 6,
    pick_up = 1u << 7, pick_down = 1u << 8, pick_extra = 1u << 9,
    editor_play = 1u << 10, editor_delete = 1u << 11, editor_previous = 1u << 12, editor_next = 1u << 13
};

struct Input {
    dc::InputDevice device = dc::InputDevice::none;
    unsigned buttons = 0;
    uint8_t frets = 0;
};

constexpr unsigned fret_shift = 14;
constexpr unsigned button_mask = (1u << fret_shift) - 1;

// Resolve shared bindings in the same order as Player.Controls in 1.3.110.
std::array<unsigned, KBD_MAX_KEYS> keyboard_bindings{};

void apply_keyboard_settings(const dc::Settings &settings) {
    using K = dc::KeyAction;
    const auto irq = irq_disable();
    keyboard_bindings.fill(0);
    keyboard_bindings[settings.key(K::left)] = nav_left;
    keyboard_bindings[settings.key(K::right)] = nav_right;
    keyboard_bindings[settings.key(K::up)] = nav_up;
    keyboard_bindings[settings.key(K::down)] = nav_down;
    keyboard_bindings[settings.key(K::pick)] = pick_down;
    keyboard_bindings[settings.key(K::secondary_pick)] = pick_up;
    for (unsigned fret = 0; fret < 5; ++fret) {
        keyboard_bindings[settings.keys[unsigned(K::fret1) + fret]] = 1u << (fret_shift + fret);
    }
    keyboard_bindings[settings.key(K::cancel)] = cancel_option | pause_song;
    keyboard_bindings[KBD_KEY_ENTER] |= select_option;
    keyboard_bindings[KBD_KEY_SPACE] |= editor_play;
    keyboard_bindings[KBD_KEY_DEL] |= editor_delete;
    keyboard_bindings[KBD_KEY_PGUP] |= editor_previous;
    keyboard_bindings[KBD_KEY_PGDOWN] |= editor_next;
    irq_restore(irq);
}

Input read_keyboard(const kbd_state_t &keys) {
    unsigned held = 0;
    for (const auto key : keys.cond.keys) {
        held |= keyboard_bindings[key];
    }
    for (unsigned mods = keys.cond.modifiers.raw; mods; mods &= mods - 1) {
        held |= keyboard_bindings[0xe0 + unsigned(__builtin_ctz(mods))];
    }
    return {dc::InputDevice::keyboard, held & button_mask, uint8_t(held >> fret_shift)};
}

Input read_input() {
    if (const auto *keys = kbd_get_state(maple_enum_type(0, MAPLE_FUNC_KEYBOARD))) {
        return read_keyboard(*keys);
    }

    auto *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    const auto *pad = controller ? static_cast<cont_state_t *>(maple_dev_status(controller)) : nullptr;
    if (!pad) {
        return {};
    }
    Input input{dc::InputDevice::controller};
    constexpr struct {
        unsigned key;
        unsigned buttons;
    } bindings[] = {
        {CONT_DPAD_UP, nav_up | pick_up}, {CONT_DPAD_DOWN, nav_down | pick_down},
        {CONT_DPAD_LEFT, nav_left}, {CONT_DPAD_RIGHT, nav_right},
        {CONT_A, select_option | pick_extra}, {CONT_START, select_option | pause_song},
        {CONT_B, cancel_option}
    };
    for (const auto &binding : bindings) {
        if (pad->buttons & binding.key) {
            input.buttons |= binding.buttons;
        }
    }
    input.frets = (pad->ltrig >= 96 ? 1u << 0 : 0) | (pad->buttons & CONT_X ? 1u << 1 : 0) |
                  (pad->buttons & CONT_Y ? 1u << 2 : 0) | (pad->buttons & CONT_B ? 1u << 3 : 0) |
                  (pad->rtrig >= 96 ? 1u << 4 : 0);
    return input;
}

struct KeyboardReport {
    Input state;
    uint64_t timestamp_us;
    unsigned pressed;
    uint8_t pressed_frets;
    uint8_t released_frets;
};

class KeyboardReports {
public:
    void start() {
        observer_ = this;
    }

    static void observe_poll(maple_frame_t *frame) {
        // The KOS keyboard event callback omits modifiers. Observe the same
        // GETCOND completion that updates all keys, including right Shift.
        if (observer_ && frame->cmd == MAPLE_COMMAND_GETCOND &&
            frame->length == 1 && frame->callback && frame->callback != reply && frame->send_buf &&
            *static_cast<const uint32_t *>(frame->send_buf) == MAPLE_FUNC_KEYBOARD) {
            const auto *keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
            if (keyboard && &keyboard->frame == frame) {
                observer_->original_reply_ = frame->callback;
                frame->callback = reply;
            }
        }
    }

    void capture(maple_device_t *dev, uint64_t timestamp_us) {
        const auto *keys = kbd_get_state(dev);
        if (!keys) {
            return;
        }
        if (capturing_key_ && !captured_key_) {
            const auto held = raw_keys(keys);
            for (const auto key : held) {
                if (dc::valid_key(key) && std::find(raw_baseline_.begin(), raw_baseline_.end(), key) ==
                                          raw_baseline_.end()) {
                    captured_key_ = key;
                    break;
                }
            }
            raw_baseline_ = held;
        }
        const auto state = read_keyboard(*keys);
        if (state.buttons == previous_.buttons && state.frets == previous_.frets) {
            return;
        }
        const auto pressed = state.buttons & ~previous_.buttons;
        const auto pressed_frets = uint8_t(state.frets & ~previous_.frets);
        const auto released_frets = uint8_t(previous_.frets & ~state.frets);
        previous_ = state;
        if (written_ - read_ == reports_.size()) {
            ++read_;
            ++overflow;
        }
        reports_[written_++ % reports_.size()] = {state, timestamp_us, pressed, pressed_frets, released_frets};
    }

    void capture_next_key() {
        const auto irq = irq_disable();
        raw_baseline_ = raw_keys(kbd_get_state(maple_enum_type(0, MAPLE_FUNC_KEYBOARD)));
        capturing_key_ = true;
        captured_key_ = 0;
        irq_restore(irq);
    }

    void stop_key_capture() {
        const auto irq = irq_disable();
        capturing_key_ = false;
        captured_key_ = 0;
        irq_restore(irq);
    }

    uint8_t take_captured_key() {
        const auto irq = irq_disable();
        const auto key = captured_key_;
        if (key) {
            captured_key_ = 0;
            capturing_key_ = false;
        }
        irq_restore(irq);
        return key;
    }

    void reset(const Input &state) {
        const auto irq = irq_disable();
        read_ = written_;
        previous_ = state;
        irq_restore(irq);
    }

    bool pop(KeyboardReport &report, uint64_t through_us = std::numeric_limits<uint64_t>::max()) {
        const auto irq = irq_disable();
        const bool available = read_ != written_ && reports_[read_ % reports_.size()].timestamp_us <= through_us;
        if (available) {
            report = reports_[read_++ % reports_.size()];
        }
        irq_restore(irq);
        return available;
    }

    unsigned overflow = 0;

private:
    static std::array<uint8_t, KBD_MAX_PRESSED_KEYS + 8> raw_keys(const kbd_state_t *state) {
        std::array<uint8_t, KBD_MAX_PRESSED_KEYS + 8> keys{};
        if (state) {
            std::copy(std::begin(state->cond.keys), std::end(state->cond.keys), keys.begin());
            for (unsigned mods = state->cond.modifiers.raw; mods; mods &= mods - 1) {
                const auto bit = unsigned(__builtin_ctz(mods));
                keys[KBD_MAX_PRESSED_KEYS + bit] = 0xe0 + bit;
            }
        }
        return keys;
    }

    static void reply(maple_state_t *state, maple_frame_t *frame) {
        const auto timestamp_us = timer_us_gettime64();
        frame->callback = observer_->original_reply_;
        frame->callback(state, frame);
        observer_->capture(frame->dev, timestamp_us);
    }

    static KeyboardReports *observer_;
    decltype(maple_frame_t::callback) original_reply_ = nullptr;
    Input previous_{dc::InputDevice::keyboard};
    std::array<KeyboardReport, 32> reports_{};
    unsigned read_ = 0;
    unsigned written_ = 0;
    std::array<uint8_t, KBD_MAX_PRESSED_KEYS + 8> raw_baseline_{};
    bool capturing_key_ = false;
    uint8_t captured_key_ = 0;
};

KeyboardReports *KeyboardReports::observer_ = nullptr;

unsigned move_list(unsigned current, unsigned count, int direction) {
    return (current + count + direction % int(count)) % count;
}

int menu_direction(unsigned buttons, bool keyboard) {
    return bool((buttons & nav_down) || (keyboard && (buttons & pick_up))) -
           bool((buttons & nav_up) || (keyboard && (buttons & pick_down) && !(buttons & select_option)));
}

bool repeats_menu_navigation(dc::Screen screen, bool entering_name) {
    switch (screen) {
        case dc::Screen::menu:
        case dc::Screen::setlist:
        case dc::Screen::difficulty:
        case dc::Screen::pause:
        case dc::Screen::editor_menu:
        case dc::Screen::editor_actions:
        case dc::Screen::editor_import:
        case dc::Screen::editor_label:
        case dc::Screen::failed:
            return true;
        case dc::Screen::results:
            return !entering_name;
        default:
            return false;
    }
}

enum class NameCommand { editing, accept, cancel };

enum class Accent { none, acute, grave, circumflex, tilde, diaeresis, cedilla };

Accent us_accent(kbd_key_t key, kbd_mods_t mods) {
    if (!mods.ralt) {
        return Accent::none;
    }
    const bool shift = mods.raw & KBD_MOD_SHIFT;
    switch (key) {
        case KBD_KEY_QUOTE: return shift ? Accent::diaeresis : Accent::acute;
        case KBD_KEY_TILDE: return shift ? Accent::tilde : Accent::grave;
        case KBD_KEY_6: return Accent::circumflex;
        case KBD_KEY_COMMA: return Accent::cedilla;
        default: return Accent::none;
    }
}

unsigned char compose_name_letter(Accent accent, unsigned char letter) {
    constexpr struct {
        char base;
        std::array<unsigned char, 6> accents;
    } alphabet[] = {
        {'a', {0xe1, 0xe0, 0xe2, 0xe3, 0xe4, 0}},
        {'c', {0, 0, 0, 0, 0, 0xe7}},
        {'e', {0xe9, 0xe8, 0xea, 0, 0xeb, 0}},
        {'i', {0xed, 0xec, 0xee, 0, 0xef, 0}},
        {'n', {0, 0, 0, 0xf1, 0, 0}},
        {'o', {0xf3, 0xf2, 0xf4, 0xf5, 0xf6, 0}},
        {'u', {0xfa, 0xf9, 0xfb, 0, 0xfc, 0}},
        {'y', {0xfd, 0, 0, 0, 0, 0}},
    };
    if (accent == Accent::none) {
        return letter;
    }
    const bool upper = letter >= 'A' && letter <= 'Z';
    const char base = upper ? letter + ('a' - 'A') : letter;
    for (const auto &entry : alphabet) {
        if (entry.base == base) {
            const auto composed = entry.accents[unsigned(accent) - 1];
            if (composed) {
                return upper ? composed - 0x20 : composed;
            }
            break;
        }
    }
    return letter;
}

void append_name_letter(std::string &name, unsigned char ch, size_t limit, size_t byte_limit) {
    if (ch < 32 || (ch >= 127 && ch < 160)) {
        return;
    }
    const size_t before = name.size();
    dc::utf8::append_latin1(name, ch);
    if (dc::utf8::prefix(name, limit, byte_limit) != name.size()) {
        name.resize(before);
    }
}

NameCommand read_name_keys(std::string &name, uint64_t &last_key_ms, size_t limit,
                           Accent *composition = nullptr) {
    auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    const auto *keyboard = kbd_get_state(device);
    if (!keyboard) {
        return NameCommand::editing;
    }
    for (int raw; (raw = kbd_queue_pop(device, false)) != KBD_QUEUE_END;) {
        last_key_ms = timer_ms_gettime64();
        const auto key = static_cast<kbd_key_t>(raw & 0xff);
        if (key == KBD_KEY_ENTER || key == KBD_KEY_F1) {
            return NameCommand::accept;
        }
        if (key == KBD_KEY_ESCAPE || key == KBD_KEY_F2) {
            return NameCommand::cancel;
        }
        if (key == KBD_KEY_BACKSPACE) {
            if (composition && *composition != Accent::none) {
                *composition = Accent::none;
            } else {
                dc::utf8::backspace(name);
            }
            continue;
        }
        kbd_mods_t mods{};
        kbd_leds_t leds{};
        mods.raw = static_cast<uint8_t>(raw >> 8);
        leds.raw = static_cast<uint8_t>(raw >> 16);
        const auto region = keyboard->region >= KBD_REGION_JP && keyboard->region <= KBD_REGION_ES
                                ? keyboard->region : KBD_REGION_US;
        if (composition && region == KBD_REGION_US) {
            const auto accent = us_accent(key, mods);
            if (accent != Accent::none) {
                *composition = accent;
                continue;
            }
        }
        auto ch = static_cast<unsigned char>(kbd_key_to_ascii(key, region, mods, leds));
        if (composition && ch >= 32) {
            ch = compose_name_letter(*composition, ch);
            *composition = Accent::none;
        }
        append_name_letter(name, ch, limit, composition ? dc::ScoreBook::max_name_bytes : limit);
    }
    return NameCommand::editing;
}

void read_filter_keys(dc::SongChooser &chooser) {
    auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    const auto *keyboard = kbd_get_state(device);
    if (!keyboard) {
        return;
    }
    for (int raw; (raw = kbd_queue_pop(device, false)) != KBD_QUEUE_END;) {
        const auto key = static_cast<kbd_key_t>(raw & 0xff);
        kbd_mods_t mods{};
        kbd_leds_t leds{};
        mods.raw = static_cast<uint8_t>(raw >> 8);
        leds.raw = static_cast<uint8_t>(raw >> 16);
        if (key == KBD_KEY_BACKSPACE) {
            chooser.type('\b');
        } else {
            chooser.type(static_cast<unsigned char>(kbd_key_to_ascii(key, keyboard->region, mods, leds)));
        }
    }
}

enum class Cheat { none, autoplay, skip };

Cheat read_cheat_keys(std::string &prefix, bool listen, bool *dismissed = nullptr) {
    auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if (!kbd_get_state(device)) {
        return Cheat::none;
    }
    Cheat found = Cheat::none;
    constexpr char autoplay[] = "uptomytempo";
    constexpr char skip[] = "fastforward";
    for (int raw; (raw = kbd_queue_pop(device, false)) != KBD_QUEUE_END;) {
        const auto key = raw & 0xff;
        const unsigned handled = select_option | cancel_option | (3u << fret_shift);
        if (dismissed && (keyboard_bindings[key] & handled)) {
            *dismissed = true;
        }
        if (!listen || key < KBD_KEY_A || key > KBD_KEY_Z || keyboard_bindings[key]) {
            continue;
        }
        prefix += static_cast<char>('a' + key - KBD_KEY_A);
        if (prefix == autoplay || prefix == skip) {
            found = prefix == autoplay ? Cheat::autoplay : Cheat::skip;
            prefix.clear();
        } else if (std::strncmp(autoplay, prefix.c_str(), prefix.size()) != 0 &&
                   std::strncmp(skip, prefix.c_str(), prefix.size()) != 0) {
            prefix.clear();
        }
    }
    return found;
}

struct FrameTiming {
    uint64_t last = 0;
    unsigned max_us = 0;
    unsigned over_25_ms = 0;
    unsigned over_50_ms = 0;
    unsigned frames = 0;

    void sample() {
        const auto now = timer_us_gettime64();
        if (last) {
            const auto delta = static_cast<unsigned>(now - last);
            max_us = std::max(max_us, delta);
            over_25_ms += delta > 25000;
            over_50_ms += delta > 50000;
        }
        last = now;
        ++frames;
    }
};

} // namespace

extern "C" int __wrap_maple_queue_frame(maple_frame_t *frame) {
    KeyboardReports::observe_poll(frame);
    return __real_maple_queue_frame(frame);
}

int main() {
    dc::View view;
    if (!view.init()) {
        std::puts("FoFDC: could not load the classic renderer");
        return 1;
    }

    sounds.load();
    KeyboardReports keyboard_reports;
    keyboard_reports.start();
    maple_wait_scan();
    dc::Settings settings;
    settings.load();
    if (settings.language >= dc::Language::count()) {
        settings.language = 0;
    }
    apply_keyboard_settings(settings);
    dc::ScoreBook scores;
    dc::ScoreUpload score_upload;
    scores.load();
    scores.load_disc("/cd/scores.dcs");
    std::printf("FoFDC: Maple report sampling at GETCOND completion\n");
    const auto tutorial = read_tutorial_cues();
    auto songs = dc::song_library();
    auto imports = dc::imported_song_sources();
    dc::SongEdits edits;
    edits.load(songs);
    edits.load(imports);
    for (const auto &entry : imports) {
        if (edits.has_playable_edit(entry)) {
            songs.push_back(entry);
        }
    }
    dc::SongEditor editor;
    std::vector<dc::EditorLabel> label_choices;
    struct EditorSave {
        dc::SongEdits &edits;
        const dc::SongEditor &editor;
        kthread_t *worker = nullptr;
        std::atomic<bool> done{false};
        dc::SongEdits::SaveResult result = dc::SongEdits::SaveResult::failed;
        std::string message = {};
    } editor_save{edits, editor};
    dc::SongChart song;
    dc::Round round;
    dc::Frame frame;
    frame.scores = &scores;
    frame.editor = &editor;
    frame.import_sources = &imports;
    frame.editor_labels = &label_choices;
    dc::SettingsMenu settings_menu;
    frame.settings_menu = &settings_menu;
    dc::SettingsWriter settings_writer;
    bool settings_dirty = false;
    uint64_t settings_repeat_at = 0;
    unsigned settings_held_controls = 0;
    dc::Screen repeat_screen = dc::Screen::error;
    int repeat_direction = 0;
    uint64_t menu_repeat_at = 0;
    std::vector<int> song_difficulties(songs.size(), -1);
    size_t loaded_chart = songs.size();
    std::string load_error;
    bool training = false;
    bool choose_for_editor = false;
    uint64_t editor_open_ms = 0;
    uint64_t editor_frame_ms = 0;
    bool editor_preview_active = false;
    std::array<uint64_t, 13> bpm_taps{};
    size_t bpm_tap_count = 0;
    double estimated_bpm = 0.0;
    bool settings_in_pause = false;
    unsigned song_index = 1;
    unsigned last_regular_song = 0;
    dc::SongChooser chooser;
    unsigned previous_buttons = 0;
    Input processed_input;
    uint8_t blocked_frets = 0;
    int processed_time = 0;
    int chart_delay_ms = 0;
    uint64_t countdown_start = 0;
    unsigned countdown_ms = 0;
    uint64_t pause_frame_ms = 0;
    bool closing_pause = false;
    uint64_t round_start_ms = 0;
    uint64_t results_start_ms = 0;
    uint64_t name_key_ms = 0;
    Accent name_accent = Accent::none;
    uint64_t name_frame_ms = 0;
    bool closing_name = false;
    unsigned name_letter = 0;
    uint64_t credits_start_ms = 0;
    uint64_t credits_close_ms = 0;
    float credits_visibility_at_close = 0.0f;
    unsigned music_level = 100;
    unsigned preview_channel = 0;
    size_t preview_selection = songs.size();
    uint64_t preview_due_ms = 0;
    bool error_to_main = false;
    std::string cheat_prefix;
    bool cheated = false;
    bool autoplay = false;
    bool skip_round = false;
    uint8_t autoplay_frets = 0;
    FrameTiming timing;
    frame.difficulty = settings.difficulty;

    const auto apply_settings = [&] {
        frame.delay_ms = settings.delay_ms;
        frame.text_scale = settings.text_scale;
        frame.lefty = settings.lefty;
        frame.tapping = settings.tapping;
        frame.language = settings.language;
        frame.mods = settings.mods;
        audio.set_mix(settings.guitar_level, settings.song_level, settings.rhythm_level);
        menu_audio.set_mix(settings.guitar_level, settings.song_level, settings.rhythm_level);
        for (auto &preview : collection_audio) {
            preview.set_mix(settings.guitar_level, settings.song_level, settings.rhythm_level);
        }
        sounds.set_mix(settings.guitar_level, settings.miss_level);
        result_audio.set_volume(settings.guitar_level);
    };
    const auto save_settings = [&] {
        if (settings_dirty) {
            settings_writer.queue(settings);
            settings_dirty = false;
        }
    };
    const auto show_settings = [&](bool in_pause) {
        settings_held_controls = 0;
        settings_menu.draft = settings;
        settings_menu.open(in_pause ? dc::SettingsPage::pause_volume : dc::SettingsPage::categories);
        settings_in_pause = in_pause;
        frame.screen = dc::Screen::settings;
    };
    apply_settings();

    const auto chooser_selection = [&] {
        frame.library_item = !chooser.selected() || chooser.folder();
        frame.selected = frame.library_item ? songs.size() : chooser.song_index();
    };
    const auto to_setlist = [&] {
        menu_audio.fade_out(1000);
        audio.stop();
        frame.countdown = 0;
        training = false;
        choose_for_editor = false;
        if (songs.size() < 2) {
            frame.screen = dc::Screen::error;
            frame.error = "No songs on the disc.";
            error_to_main = true;
            return;
        }
        chooser.open(songs, last_regular_song);
        if (auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD)) {
            while (kbd_queue_pop(device, false) != KBD_QUEUE_END) {}
        }
        frame.screen = dc::Screen::setlist;
        chooser_selection();
    };
    const auto to_main = [&] {
        audio.stop();
        menu_audio.stop();
        frame.countdown = 0;
        choose_for_editor = false;
        frame.screen = dc::Screen::menu;
        frame.selected = training ? 1 : 0;
    };
    const auto read_chart = [&](size_t index) {
        if (loaded_chart == index) {
            return true;
        }
        if (!edits.open(songs[index], song, load_error)) {
            loaded_chart = songs.size();
            song_difficulties[index] = 0;
            return false;
        }
        loaded_chart = index;
        const auto &entry = songs[index];
        std::string edit_key;
        if (const auto *midi = edits.midi_override(entry)) {
            const auto edited_hash = dc::ranking::song_hash({}, *midi);
            if (!edited_hash.empty() && edited_hash != dc::ranking::song_hash(entry.midi, {})) {
                const std::vector<uint8_t> path_bytes(entry.midi.begin(), entry.midi.end());
                edit_key = "@" + dc::ranking::song_hash({}, path_bytes) + edited_hash;
            }
        }
        scores.use_chart(entry.midi, edit_key);
        unsigned available = 0;
        for (unsigned i = 0; i < song.guitar.size(); ++i) {
            if (!song.guitar[i].empty()) {
                available |= 1u << i;
            }
        }
        song_difficulties[index] = available;
        return true;
    };
    const auto open_editor = [&](const dc::SongEntry &entry, dc::Screen failure_screen) {
        dc::SongChart editable;
        if (!edits.open(entry, editable, frame.editor_text, true)) {
            frame.screen = failure_screen;
            std::printf("FoFDC: editor open failed midi=%s error=%s\n", entry.midi.c_str(), frame.editor_text.c_str());
            return;
        }
        audio.stop();
        editor_preview_active = false;
        editor.open(entry, std::move(editable));
        editor_open_ms = editor_frame_ms = timer_ms_gettime64();
        frame.editor_text.clear();
        frame.screen = dc::Screen::editing;
        std::printf("FoFDC: editor opened midi=%s\n", entry.midi.c_str());
    };
    const auto preview_editor = [&](unsigned position) {
        const auto &entry = editor.entry();
        const auto &instrument = entry.guitar.empty() ? entry.backing : entry.guitar;
        // The Linux seekable editor uses the instrument as its music stream. Its
        // separate rhythm channel starts with the estimator at zero and is not seekable.
        const char *rhythm = position == 0 && !entry.rhythm.empty() ? entry.rhythm.c_str() : nullptr;
        editor_preview_active = audio.prepare(instrument.c_str(), nullptr, rhythm,
                                              service_audio_while_loading, position) &&
                                audio.play_prepared();
        if (editor_preview_active) {
            std::printf("FoFDC: editor preview seek=%u delay=%d\n", position, entry.delay_ms);
        }
        return editor_preview_active;
    };
    const auto save_editor = [&] {
        if (!editor.has_changes()) {
            frame.editor_text = "There are no changes to save.";
            frame.screen = dc::Screen::editor_message;
            return;
        }
        editor_save.done.store(false, std::memory_order_relaxed);
        editor_save.worker = thd_create(false, [](void *data) -> void * {
            auto &save = *static_cast<EditorSave *>(data);
            save.result = save.edits.save(save.editor, save.message);
            save.done.store(true, std::memory_order_release);
            return nullptr;
        }, &editor_save);
        if (!editor_save.worker) {
            frame.editor_text = "Could not start the VMU save.";
            frame.screen = dc::Screen::editor_message;
            return;
        }
        frame.screen = dc::Screen::editor_saving;
    };
    const auto finish_editor_save = [&] {
        thd_join(editor_save.worker, nullptr);
        editor_save.worker = nullptr;
        frame.editor_text = std::move(editor_save.message);
        frame.screen = dc::Screen::editor_message;
        if (editor_save.result == dc::SongEdits::SaveResult::failed) {
            return;
        }
        if (editor_save.result == dc::SongEdits::SaveResult::saved) {
            editor.mark_saved();
        }
        const auto identity = editor.entry().midi;
        const auto found = std::find_if(songs.begin(), songs.end(), [&](const dc::SongEntry &entry) {
            return entry.midi == identity;
        });
        if (found != songs.end()) {
            edits.update_metadata(*found);
            song_difficulties[found - songs.begin()] = -1;
        } else if (edits.has_playable_edit(editor.entry())) {
            songs.push_back(editor.entry());
            song_difficulties.push_back(-1);
        }
        for (auto &entry : imports) {
            if (entry.midi == identity) {
                edits.update_metadata(entry);
                break;
            }
        }
        loaded_chart = songs.size();
    };
    const auto load_song = [&] {
        const auto &entry = songs[song_index];
        chart_delay_ms = entry.delay_ms;
        frame.title = entry.title.c_str();
        frame.artist = entry.artist.c_str();
        error_to_main = training;
        if (!read_chart(song_index)) {
            frame.screen = dc::Screen::error;
            frame.error = load_error.c_str();
            std::printf("FoFDC: %s\n", frame.error);
            return;
        }
        frame.available = song_difficulties[song_index];
        if (!frame.available) {
            frame.screen = dc::Screen::error;
            frame.error = "No supported notes in this chart.";
            return;
        }
        frame.selected = (frame.available & (1u << settings.difficulty)) ? settings.difficulty :
                         31 - __builtin_clz(frame.available);
        frame.screen = dc::Screen::difficulty;
        std::printf("FoFDC: loaded %s (A=%u M=%u E=%u S=%u)\n", entry.title.c_str(),
                    unsigned(song.guitar[0].size()), unsigned(song.guitar[1].size()),
                    unsigned(song.guitar[2].size()), unsigned(song.guitar[3].size()));
    };
    const auto prepare_entry = [&](dc::Audio &player, const dc::SongEntry &entry) {
        if (!player.prepare(entry.backing.c_str(), entry.guitar.empty() ? nullptr : entry.guitar.c_str(),
                            entry.rhythm.empty() ? nullptr : entry.rhythm.c_str(), service_audio_while_loading)) {
            return false;
        }
        player.set_mix(settings.guitar_level, settings.song_level, settings.rhythm_level);
        return true;
    };
    const auto begin_countdown = [&] {
        read_cheat_keys(cheat_prefix, false);
        cheat_prefix.clear();
        frame.cheat_message = nullptr;
        cheated = autoplay = skip_round = false;
        autoplay_frets = 0;
        menu_audio.fade_out(1000);
        collection_audio[preview_channel].fade_out(1000);
        frame.countdown = 0;
        const auto &entry = songs[song_index];
        const auto prep_start = timer_us_gettime64();
        if (!prepare_entry(audio, entry)) {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
            return;
        }
        std::printf("FoFDC: audio prepared before countdown in %llu us\n",
                    static_cast<unsigned long long>(timer_us_gettime64() - prep_start));
        round = dc::Round{};
        view.reset_effects(frame.screen == dc::Screen::pause);
        frame.gems = 0;
        int end_ms = 0;
        for (const auto &target : song.guitar[frame.difficulty]) {
            frame.gems += dc::fret_count(target.frets);
            end_ms = std::max(end_ms, *std::max_element(target.ends.begin(), target.ends.end()));
        }
        countdown_ms = 8 * song.initial_us_per_beat / 1000;
        frame.screen = dc::Screen::countdown;
        frame.time_ms = -int(countdown_ms) - frame.delay_ms - chart_delay_ms;
        frame.countdown = 8;
        frame.duration_ms = (end_ms + 1500) / 1000 * 1000;
        countdown_start = timer_ms_gettime64();
        timing = {};
        keyboard_reports.overflow = 0;
        sounds.play(start_song);
    };
    const auto begin_audio = [&] {
        frame.countdown = 0;
        if (!audio.play_prepared()) {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
            return;
        }
        music_level = audio.backing_level();
        round_start_ms = timer_ms_gettime64();
        frame.time_ms = -frame.delay_ms - chart_delay_ms;
        processed_input = read_input();
        blocked_frets = 0;
        processed_time = frame.time_ms;
        keyboard_reports.reset(processed_input);
        frame.screen = dc::Screen::play;
        std::printf("FoFDC: playing difficulty=%u goals=%u gems=%u delay=%d chart_start=%d\n", frame.difficulty,
                    unsigned(song.guitar[frame.difficulty].size()), frame.gems, frame.delay_ms + chart_delay_ms,
                    frame.time_ms);
        std::printf("FoFDC: memory heap_used=%u vram_free=%u aica_free=%u\n",
                    unsigned(mallinfo().uordblks), unsigned(pvr_mem_available()), unsigned(snd_mem_available()));
    };
    const auto resume_audio = [&] {
        // GuitarScene reloads the configured background volume before unpausing.
        audio.restore_backing_gain();
        if (audio.playing() || audio.resume()) {
            music_level = audio.backing_level();
            frame.screen = dc::Screen::play;
            frame.time_ms = audio.position_ms() - frame.delay_ms - chart_delay_ms;
            const auto input = read_input();
            blocked_frets = input.device == dc::InputDevice::keyboard
                                ? uint8_t(input.frets & ~processed_input.frets) : 0;
            processed_input = input;
            processed_input.frets &= ~blocked_frets;
            processed_time = frame.time_ms;
            keyboard_reports.reset(input);
            timing.last = 0;
            std::printf("FoFDC: resumed %d ms\n", audio.position_ms());
        } else {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
        }
    };
    const auto show_pause = [&] {
        result_audio.pause();
        sounds.pause();
        frame.screen = dc::Screen::pause;
        frame.selected = 0;
        frame.pause_visibility = 0.0f;
        pause_frame_ms = timer_ms_gettime64();
        closing_pause = false;
        std::printf("FoFDC: pause opened chart=%d countdown=%.3f pcm=%d\n",
                    frame.time_ms, double(frame.countdown), frame.countdown > 0.0f ? 0 : audio.position_ms());
    };
    const auto toggle = [&](int steps) {
        if (steps) {
            sounds.play(static_cast<Effect>(select1 + std::rand() % 3));
        }
    };

    const auto resume_menu_music = [&] {
        if (!menu_audio.playing() &&
            (!menu_audio.prepare("/rd/menu.ogg", nullptr, nullptr, service_audio_while_loading) ||
             !menu_audio.play_prepared())) {
            std::printf("FoFDC: menu audio error: %s\n", menu_audio.error());
        }
    };

    const auto record_pick = [&](dc::Pick result, int when, uint8_t frets, bool after_stage_update = false) {
        audio.guitar_gain(round.guitar_muted ? 0.0f : 1.0f, result != dc::Pick::neutral);
        music_level = audio.backing_level();
        if (result == dc::Pick::hit) {
            trace_input(InputTrace::hit, when, round.last_hit_ms - round.last_hit_offset_ms, frets, round);
            view.record_pick(round, after_stage_update);
        } else if (result == dc::Pick::bad) {
            sounds.play(static_cast<Effect>(miss1 + std::rand() % 6));
        }
    };

    for (;;) {
        frame.sustain_expired = false;
        const auto previous_screen = frame.screen;
        const auto input = read_input();
        const bool same_device = input.device == frame.input;
        unsigned pressed = same_device && input.device == dc::InputDevice::controller ?
                           input.buttons & ~previous_buttons : 0;
        int step = menu_direction(pressed, false);
        previous_buttons = input.buttons;
        unsigned menu_frets = 0;
        const auto &chart = song.guitar[frame.difficulty];
        const bool pause_menu = frame.screen == dc::Screen::pause ||
                                (frame.screen == dc::Screen::settings && settings_in_pause);
        if (frame.screen != dc::Screen::play || input.device != dc::InputDevice::keyboard) {
            KeyboardReport report;
            while (keyboard_reports.pop(report)) {
                if (input.device == dc::InputDevice::keyboard) {
                    pressed |= report.pressed;
                    menu_frets |= report.pressed_frets;
                    step += menu_direction(report.pressed |
                                           (report.state.buttons & select_option), true);
                    // Menu.keyPressed consumes presses. Its inherited
                    // keyReleased still passes fret releases to GuitarScene.
                    if (pause_menu && frame.countdown <= 0 && report.released_frets &&
                        processed_input.device == dc::InputDevice::keyboard) {
                        const dc::Rules rules{60000000.0f / song.initial_us_per_beat,
                                              frame.delay_ms + chart_delay_ms, settings.tapping};
                        processed_input.frets &= ~report.released_frets;
                        trace_input(InputTrace::report, frame.time_ms, report.state.buttons,
                                    report.state.frets, round);
                        record_pick(round.fret_edge(chart, frame.time_ms, processed_input.frets, rules, true),
                                    frame.time_ms, processed_input.frets);
                        trace_input(InputTrace::fret, frame.time_ms, 1, processed_input.frets, round);
                    }
                }
            }
        }
        frame.held = input.frets;
        frame.strumming = input.buttons & (pick_up | pick_down | pick_extra);
        frame.input = input.device;
        const bool confirm = (pressed & select_option) || (menu_frets & (1u << 0));
        const bool cancel = (pressed & cancel_option) || (menu_frets & (1u << 1));
        const int held_direction = frame.input == dc::InputDevice::keyboard &&
                                   repeats_menu_navigation(frame.screen, frame.results_name)
                                       ? menu_direction(input.buttons, true) : 0;
        const auto navigation_now = timer_ms_gettime64();
        if (repeat_screen != frame.screen || repeat_direction != held_direction) {
            repeat_screen = frame.screen;
            repeat_direction = held_direction;
            menu_repeat_at = navigation_now + 300;
        } else if (held_direction && navigation_now >= menu_repeat_at) {
            step += held_direction;
            menu_repeat_at = navigation_now + 30;
        }
        const int adjust = bool((pressed & nav_right) || (menu_frets & (1u << 3))) -
                           bool((pressed & nav_left) || (menu_frets & (1u << 2)));
        frame.tutorial_captions = {};
        frame.tutorial_picture = -1;

        switch (frame.screen) {
            case dc::Screen::menu:
                resume_menu_music();
                frame.selected = move_list(frame.selected, 6, step);
                toggle(step);
                if (confirm) {
                    sounds.play(accept);
                    if (frame.selected == 5) {
                        save_settings();
                        settings_writer.flush();
                        scores.flush();
                        arch_menu();
                    } else if (frame.selected == 3) {
                        show_settings(false);
                    } else if (frame.selected == 4) {
                        menu_audio.fade_out(1000);
                        auto &credits_audio = collection_audio[preview_channel];
                        bool started = false;
                        if (credits_audio.prepare("/cd/songs/defy/song.ogg", "/cd/songs/defy/guitar.ogg",
                                                  nullptr, service_audio_while_loading)) {
                            // Credits.songLoaded does not set its new Sound volume.
                            // SDL_mixer's music channel retains the last backing volume.
                            credits_audio.set_mix(100, music_level, 100);
                            started = credits_audio.play_prepared();
                        }
                        credits_start_ms = timer_ms_gettime64();
                        credits_close_ms = 0;
                        frame.credits = {};
                        frame.screen = dc::Screen::credits;
                        std::printf("FoFDC: credits music=%s\n", started ? "Defy The Machine" : "unavailable");
                    } else if (frame.selected == 0) {
                        to_setlist();
                    } else if (frame.selected == 1) {
                        training = true;
                        song_index = 0;
                        load_song();
                        if (frame.screen == dc::Screen::difficulty) {
                            frame.difficulty = (frame.available & 2) ? 1 : frame.selected;
                            begin_countdown();
                        }
                    } else {
                        frame.editor_text.clear();
                        frame.selected = 0;
                        frame.screen = dc::Screen::editor_menu;
                    }
                } else if (cancel) {
                    save_settings();
                    settings_writer.flush();
                    scores.flush();
                    arch_menu();
                }
                break;
            case dc::Screen::settings: {
                using P = dc::SettingsPage;
                using S = dc::Setting;
                if (!settings_in_pause) {
                    resume_menu_music();
                }
                if (settings_menu.capture) {
                    const auto key = keyboard_reports.take_captured_key();
                    if (key || (input.device == dc::InputDevice::controller && cancel)) {
                        const auto action = settings_menu.row(settings_menu.selected).action;
                        if (key && key != KBD_KEY_ESCAPE && key != settings.key(dc::KeyAction::cancel) &&
                            key != settings.key(dc::KeyAction::fret2)) {
                            settings.keys[unsigned(action)] = settings_menu.draft.keys[unsigned(action)] = key;
                            apply_keyboard_settings(settings);
                            keyboard_reports.reset(read_input());
                            settings_dirty = true;
                            sounds.play(accept);
                            std::printf("FoFDC: Maple binding action=%u key=%02x\n", unsigned(action), key);
                        } else {
                            sounds.play(back);
                        }
                        keyboard_reports.stop_key_capture();
                        settings_menu.capture = false;
                        settings_held_controls = 0;
                    }
                    break;
                }
                if (settings_menu.page == P::key_test ? (pressed & cancel_option) != 0 : cancel) {
                    sounds.play(back);
                    if (!settings_menu.back()) {
                        frame.screen = settings_in_pause ? dc::Screen::pause : dc::Screen::menu;
                        save_settings();
                    }
                    settings_held_controls = 0;
                    break;
                }
                if (settings_menu.page == P::key_test) {
                    settings_menu.test_held = input.frets | (frame.strumming ? 1u << 5 : 0);
                    break;
                }
                constexpr unsigned repeat_buttons = nav_up | nav_down | nav_left | nav_right | pick_up | pick_down;
                constexpr unsigned repeat_frets = (1u << 2) | (1u << 3);
                const auto held_controls = (input.buttons & repeat_buttons) |
                                           ((input.frets & repeat_frets) << fret_shift);
                unsigned repeat = 0;
                const auto now = timer_ms_gettime64();
                if (held_controls != settings_held_controls) {
                    settings_held_controls = held_controls;
                    settings_repeat_at = now + 300;
                } else if (held_controls && now >= settings_repeat_at) {
                    repeat = held_controls;
                    settings_repeat_at = now + 30;
                }
                const unsigned ui_press = pressed | repeat;
                const unsigned ui_frets = menu_frets | (repeat >> fret_shift);
                const bool keyboard = frame.input == dc::InputDevice::keyboard;
                const int settings_step = bool((ui_press & nav_down) || (keyboard && (ui_press & pick_up))) -
                                          bool((ui_press & nav_up) || (keyboard && !(input.buttons & select_option) &&
                                                                      !confirm && (ui_press & pick_down)));
                const int settings_adjust = bool((ui_press & nav_right) || (ui_frets & (1u << 3))) -
                                            bool((ui_press & nav_left) || (ui_frets & (1u << 2)));
                if (settings_step) {
                    settings_menu.move(settings_step);
                    toggle(settings_step);
                }
                const auto &option = settings_menu.row(settings_menu.selected);
                const auto setting = option.setting;
                auto &draft = settings_menu.draft;
                if (dc::adjust_setting(draft, setting, settings_adjust, dc::Language::count())) {
                    if (setting == S::upload) {
                        settings.upload_scores = draft.upload_scores;
                        settings_dirty = true;
                        save_settings();
                    }
                    if (setting == S::lefty || setting == S::tapping) {
                        settings.lefty = draft.lefty;
                        settings.tapping = draft.tapping;
                        settings_dirty = true;
                        apply_settings();
                    }
                    if (dc::setting_is_volume(setting)) {
                        if (settings_in_pause) {
                            settings.guitar_level = draft.guitar_level;
                            settings.song_level = draft.song_level;
                            settings.rhythm_level = draft.rhythm_level;
                            settings.miss_level = draft.miss_level;
                            settings_dirty = true;
                            apply_settings();
                        }
                        const unsigned level = setting == S::guitar ? draft.guitar_level :
                                               setting == S::song ? draft.song_level :
                                               setting == S::rhythm ? draft.rhythm_level : draft.miss_level;
                        sounds.preview(level);
                    }
                }
                if (!confirm) {
                    break;
                }
                sounds.play(accept);
                switch (setting) {
                    case S::game: settings_menu.open(P::game); break;
                    case S::mods: settings_menu.open(P::mods); break;
                    case S::keys: settings_menu.open(P::keys); break;
                    case S::video: settings_menu.open(P::video); break;
                    case S::audio: settings_menu.open(P::audio); break;
                    case S::volume: settings_menu.open(P::volume); break;
                    case S::test_keys: settings_menu.open(P::key_test); break;
                    case S::binding:
                        if (input.device == dc::InputDevice::keyboard) {
                            settings_menu.capture = true;
                            keyboard_reports.capture_next_key();
                        }
                        break;
                    case S::apply:
                        settings = draft;
                        settings_dirty = true;
                        apply_settings();
                        music_level = 100;
                        save_settings();
                        training = false;
                        to_main();
                        menu_audio.stop();
                        break;
                    default: break;
                }
                break;
            }
            case dc::Screen::credits: {
                const uint64_t now = timer_ms_gettime64();
                frame.credits.elapsed_ms = unsigned(now - credits_start_ms);
                constexpr float transition_ms = dc::credits_transition_ms;
                if (!credits_close_ms) {
                    frame.credits.visibility = std::min(1.0f, frame.credits.elapsed_ms / transition_ms);
                    if (confirm || cancel || frame.credits.elapsed_ms >= dc::credits_duration_ms) {
                        credits_visibility_at_close = frame.credits.visibility;
                        credits_close_ms = now;
                    }
                } else {
                    frame.credits.visibility = credits_visibility_at_close - (now - credits_close_ms) / transition_ms;
                    if (frame.credits.visibility <= 0.0f) {
                        to_main();
                        frame.selected = 0;
                        std::printf("FoFDC: credits closed\n");
                    }
                }
                break;
            }
            case dc::Screen::setlist: {
                const bool quiet = !collection_audio[preview_channel].playing();
                if (frame.input == dc::InputDevice::keyboard) {
                    read_filter_keys(chooser);
                }
                chooser.move(step);
                if (quiet) {
                    toggle(step);
                }
                if (confirm && chooser.matches()) {
                    if (quiet) {
                        sounds.play(accept);
                    }
                    if (!chooser.enter()) {
                        if (choose_for_editor) {
                            open_editor(songs[chooser.song_index()], dc::Screen::setlist);
                        } else {
                            song_index = last_regular_song = chooser.song_index();
                            load_song();
                        }
                    }
                } else if (cancel) {
                    if (quiet) {
                        sounds.play(back);
                    }
                    if (!chooser.parent()) {
                        if (choose_for_editor) {
                            choose_for_editor = false;
                            frame.selected = 0;
                            frame.screen = dc::Screen::editor_menu;
                        } else {
                            to_main();
                        }
                    }
                }
                if (frame.screen == dc::Screen::setlist) {
                    chooser_selection();
                }
                break;
            }
            case dc::Screen::difficulty:
                for (int moves = std::abs(step); moves > 0; --moves) {
                    do {
                        frame.selected = move_list(frame.selected, 4, step > 0 ? -1 : 1);
                    } while (!(frame.available & (1u << frame.selected)));
                }
                toggle(step);
                if (confirm) {
                    frame.difficulty = frame.selected;
                    settings_dirty |= settings.difficulty != frame.difficulty;
                    settings.difficulty = frame.difficulty;
                    save_settings();
                    begin_countdown();
                } else if (cancel) {
                    sounds.play(back);
                    to_setlist();
                }
                break;
            case dc::Screen::countdown:
                if ((pressed & pause_song) && !frame.cheat_message) {
                    show_pause();
                }
                break;
            case dc::Screen::play:
                break;
            case dc::Screen::pause: {
                const auto now = timer_ms_gettime64();
                const float transition = (now - pause_frame_ms) / 512.0f;
                pause_frame_ms = now;
                if (closing_pause) {
                    frame.pause_visibility = std::max(0.0f, frame.pause_visibility - transition);
                    if (frame.pause_visibility == 0.0f) {
                        chart_delay_ms = 0; // GuitarScene.resumeGame reloads the global A/V delay.
                        result_audio.resume();
                        sounds.resume();
                        if (frame.countdown > 0.0f) {
                            frame.screen = dc::Screen::countdown;
                            std::printf("FoFDC: returned to countdown=%.3f\n", double(frame.countdown));
                        } else {
                            resume_audio();
                        }
                    }
                    break;
                }
                frame.pause_visibility = std::min(1.0f, frame.pause_visibility + transition);
                frame.selected = move_list(frame.selected, 5, step);
                toggle(step);
                if (cancel || (frame.input == dc::InputDevice::controller && confirm && frame.selected == 0)) {
                    sounds.play(cancel ? back : accept);
                    closing_pause = true;
                    std::printf("FoFDC: pause closing countdown=%.3f pcm=%d\n",
                                double(frame.countdown), frame.countdown > 0.0f ? 0 : audio.position_ms());
                } else if (confirm && frame.selected == 0) {
                    sounds.play(accept);
                } else if (confirm && frame.selected == 1) {
                    chart_delay_ms = 0;
                    result_audio.resume();
                    sounds.resume();
                    begin_countdown();
                } else if (confirm && frame.selected == 2) {
                    to_setlist();
                } else if (confirm && frame.selected == 3) {
                    show_settings(true);
                } else if (confirm) {
                    to_main();
                }
                break;
            }
            case dc::Screen::results: {
                frame.results_age_ms = static_cast<unsigned>(timer_ms_gettime64() - results_start_ms);
                const auto &result_entry = songs[frame.result_song];
                const auto &record_song = result_entry.midi;
                if (frame.results_name) {
                    const auto now = timer_ms_gettime64();
                    const float transition = (now - name_frame_ms) / 512.0f;
                    name_frame_ms = now;
                    if (closing_name) {
                        frame.name_visibility = std::max(0.0f, frame.name_visibility - transition);
                        if (frame.name_visibility == 0.0f) {
                            frame.highscore_rank = scores.add(record_song, frame.difficulty, round.score,
                                                              round.rating(frame.gems), frame.score_name);
                            if (frame.highscore_rank >= 0 && settings.upload_scores) {
                                score_upload.start(scores, record_song, edits.midi_override(result_entry));
                            }
                            frame.results_name = false;
                            frame.results_actions = true;
                            kbd_set_repeat_timing(600, 20);
                            std::puts("FoFDC: results actions opened");
                        }
                        break;
                    }
                    frame.name_visibility = std::min(1.0f, frame.name_visibility + transition);
                    NameCommand command = NameCommand::editing;
                    if (frame.input == dc::InputDevice::keyboard) {
                        command = read_name_keys(frame.score_name, name_key_ms, dc::ScoreBook::max_name, &name_accent);
                    } else if (confirm || cancel) {
                        command = cancel ? NameCommand::cancel : NameCommand::accept;
                    } else {
                        constexpr char wheel[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ ";
                        name_letter = move_list(name_letter, sizeof(wheel) - 1, step);
                        frame.name_wheel = wheel[name_letter];
                        if (adjust > 0) {
                            append_name_letter(frame.score_name, frame.name_wheel, dc::ScoreBook::max_name,
                                               dc::ScoreBook::max_name_bytes);
                        } else if (adjust < 0) {
                            dc::utf8::backspace(frame.score_name);
                        }
                    }
                    if (command != NameCommand::editing) {
                        closing_name = true;
                        if (command == NameCommand::cancel) {
                            frame.score_name = scores.name();
                        }
                    }
                    frame.name_cursor = (now - std::min(now, name_key_ms)) % 500 < 250;
                    break;
                }
                if (!frame.results_actions) {
                    if (confirm || cancel || (frame.input == dc::InputDevice::keyboard && (pressed & pick_up))) {
                        if (!cheated && scores.qualifies(record_song, frame.difficulty, round.score)) {
                            kbd_set_repeat_timing(300, 30);
                            if (auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD)) {
                                while (kbd_queue_pop(device, false) != KBD_QUEUE_END) {}
                            }
                            frame.score_name = scores.name();
                            name_accent = Accent::none;
                            frame.name_visibility = 0.0f;
                            frame.results_name = true;
                            frame.name_cursor = true;
                            name_key_ms = name_frame_ms = timer_ms_gettime64();
                            closing_name = false;
                            name_letter = 0;
                            frame.name_wheel = 'A';
                            std::puts("FoFDC: high score prompt opened");
                        } else {
                            frame.results_actions = true;
                            std::puts("FoFDC: results actions opened");
                        }
                    }
                    break;
                }
            }
                [[fallthrough]];
            case dc::Screen::failed:
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step);
                if (cancel || (confirm && frame.selected == 2)) {
                    to_main();
                } else if (confirm && frame.selected == 1) {
                    to_setlist();
                } else if (confirm) {
                    chart_delay_ms = songs[song_index].delay_ms;
                    begin_countdown();
                }
                break;
            case dc::Screen::error:
                if (cancel || confirm) {
                    error_to_main ? to_main() : to_setlist();
                }
                break;
            case dc::Screen::editor_menu:
                resume_menu_music();
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step);
                if (cancel) {
                    to_main();
                    frame.selected = 2;
                } else if (confirm) {
                    sounds.play(accept);
                    frame.editor_text.clear();
                    if (frame.selected == 0) {
                        to_setlist();
                        choose_for_editor = frame.screen == dc::Screen::setlist;
                    } else {
                        frame.selected = 0;
                        frame.screen = dc::Screen::editor_import;
                    }
                }
                break;
            case dc::Screen::editor_import:
                resume_menu_music();
                if (cancel) {
                    frame.selected = 1;
                    frame.editor_text.clear();
                    frame.screen = dc::Screen::editor_menu;
                } else if (!imports.empty()) {
                    frame.selected = move_list(frame.selected, imports.size(), step);
                    toggle(step);
                    if (confirm) {
                        menu_audio.fade_out(1000);
                        open_editor(imports[frame.selected], dc::Screen::editor_import);
                    }
                }
                break;
            case dc::Screen::editing: {
                const auto now = timer_ms_gettime64();
                const auto elapsed = std::min<uint64_t>(100, now - editor_frame_ms);
                editor_frame_ms = now;
                const bool was_playing = editor_preview_active && audio.playing();
                audio.poll();
                if (was_playing) {
                    editor.follow_audio(audio.position_ms());
                }
                if (pressed & cancel_option) {
                    editor.end_note();
                    frame.selected = 0;
                    frame.screen = dc::Screen::editor_actions;
                    break;
                }
                const int difficulty_step = bool(pressed & editor_next) - bool(pressed & editor_previous);
                if (difficulty_step) {
                    editor.change_difficulty(difficulty_step);
                }
                const int lane_step = bool(pressed & nav_down) - bool(pressed & nav_up);
                if (lane_step) {
                    editor.move_lane(lane_step);
                }
                const int time_step = bool(pressed & nav_right) - bool(pressed & nav_left);
                if (time_step && !(editor_preview_active && audio.playing())) {
                    editor.move_quarter(time_step);
                }
                if (pressed & editor_delete) {
                    editor.remove_note();
                }
                const unsigned picks = input.device == dc::InputDevice::keyboard ? (pick_up | pick_down) : pick_extra;
                if (pressed & picks) {
                    editor.begin_note(input.frets | menu_frets);
                }
                if (pressed & editor_play) {
                    if (editor_preview_active && audio.playing()) {
                        audio.stop();
                        editor_preview_active = false;
                    } else if (!preview_editor(editor.position_ms())) {
                        frame.editor_text = audio.error();
                        frame.selected = 0;
                        frame.screen = dc::Screen::editor_message;
                    }
                }
                const int held = bool(input.buttons & nav_right) - bool(input.buttons & nav_left);
                editor.update(elapsed, held, input.buttons & picks || input.frets,
                              editor_preview_active && audio.playing());
                break;
            }
            case dc::Screen::editor_actions:
                frame.selected = move_list(frame.selected, 10, step);
                toggle(step);
                if (cancel) {
                    editor_frame_ms = timer_ms_gettime64();
                    frame.screen = dc::Screen::editing;
                } else if (confirm) {
                    sounds.play(accept);
                    if (frame.selected == 9) {
                        to_main();
                        frame.selected = 2;
                    } else if (frame.selected == 0) {
                        save_editor();
                    } else if (frame.selected == 4) {
                        bpm_tap_count = 0;
                        estimated_bpm = 0.0;
                        frame.editor_text.clear();
                        if (preview_editor(0)) {
                            frame.screen = dc::Screen::editor_tempo;
                        } else {
                            frame.editor_text = audio.error();
                            frame.screen = dc::Screen::editor_message;
                        }
                    } else if (frame.selected == 7) {
                        label_choices = dc::editor_labels();
                        const auto current = std::find_if(label_choices.begin(), label_choices.end(),
                            [&](const dc::EditorLabel &label) {
                                return label.key == editor.entry().cassette_label;
                            });
                        frame.selected = current == label_choices.end() ? 0 : current - label_choices.begin();
                        frame.screen = dc::Screen::editor_label;
                    } else if (frame.selected == 8) {
                        frame.editor_text.clear();
                        frame.screen = dc::Screen::editor_message;
                    } else {
                        if (auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD)) {
                            while (kbd_queue_pop(device, false) != KBD_QUEUE_END) {}
                        }
                        frame.editor_text = editor.field(frame.selected);
                        frame.screen = dc::Screen::editor_value;
                    }
                }
                break;
            case dc::Screen::editor_value: {
                const auto command = read_name_keys(frame.editor_text, name_key_ms, 60);
                if (command == NameCommand::cancel || cancel) {
                    frame.screen = dc::Screen::editor_actions;
                } else if (command == NameCommand::accept || confirm) {
                    if (editor.set_field(frame.selected, frame.editor_text)) {
                        frame.screen = dc::Screen::editor_actions;
                    } else {
                        frame.editor_text = "Invalid value.";
                        frame.screen = dc::Screen::editor_message;
                    }
                }
                break;
            }
            case dc::Screen::editor_tempo:
                if (pressed & editor_play) {
                    if (bpm_tap_count == bpm_taps.size()) {
                        std::move(bpm_taps.begin() + 1, bpm_taps.end(), bpm_taps.begin());
                        --bpm_tap_count;
                    }
                    bpm_taps[bpm_tap_count++] = timer_ms_gettime64();
                    if (bpm_tap_count == bpm_taps.size() && bpm_taps.back() > bpm_taps.front()) {
                        estimated_bpm = 60000.0 * (bpm_taps.size() - 1) / (bpm_taps.back() - bpm_taps.front());
                        char bpm[20];
                        std::snprintf(bpm, sizeof(bpm), "%.2f", estimated_bpm);
                        frame.editor_text = bpm;
                    }
                }
                if (cancel || confirm) {
                    if (!cancel && estimated_bpm != 0.0 && editor.set_tempo(estimated_bpm)) {
                        std::printf("FoFDC: editor estimated bpm=%.3f\n", estimated_bpm);
                    }
                    audio.fade_out(1000);
                    editor_preview_active = false;
                    frame.screen = dc::Screen::editor_actions;
                }
                break;
            case dc::Screen::editor_label:
                frame.selected = move_list(frame.selected, label_choices.size(), step);
                toggle(step);
                if (cancel || confirm) {
                    if (!cancel) {
                        editor.set_label(label_choices[frame.selected].key);
                        sounds.play(accept);
                    }
                    frame.selected = 7;
                    frame.screen = dc::Screen::editor_actions;
                }
                break;
            case dc::Screen::editor_saving:
                if (editor_save.done.load(std::memory_order_acquire)) {
                    finish_editor_save();
                }
                break;
            case dc::Screen::editor_message:
                if (confirm || cancel) {
                    frame.screen = dc::Screen::editor_actions;
                }
                break;
        }

        if (frame.screen >= dc::Screen::editor_actions && audio.playing()) {
            audio.poll();
            if (editor_preview_active) {
                editor.follow_audio(audio.position_ms());
                editor.update(0, 0, false, audio.playing());
            }
        }
        frame.editor_playing = editor_preview_active && audio.playing();
        if (frame.screen >= dc::Screen::editing) {
            frame.editor_age_ms = timer_ms_gettime64() - editor_open_ms;
        }

        bool dismiss_cheat = false;
        if (frame.screen == dc::Screen::play || frame.screen == dc::Screen::countdown) {
            const auto cheat = read_cheat_keys(cheat_prefix, !frame.cheat_message,
                                                frame.cheat_message ? &dismiss_cheat : nullptr);
            if (frame.cheat_message && (confirm || cancel || dismiss_cheat)) {
                dismiss_cheat = true;
                frame.cheat_message = nullptr;
            }
            if (cheat != Cheat::none) {
                cheated = true;
                if (cheat == Cheat::skip) {
                    skip_round = true;
                    frame.countdown = 0.0f;
                    frame.screen = dc::Screen::play;
                } else {
                    autoplay = !autoplay;
                    autoplay_frets = 0;
                    frame.cheat_message = autoplay ? "Jurgen will show you how it is done." :
                                                    "Jurgen has left the building.";
                }
                std::printf("FoFDC: cheat %s\n", cheat == Cheat::skip ? "results" : autoplay ? "Jurgen on" : "Jurgen off");
            }
        }
        const bool pause_layer = frame.screen == dc::Screen::pause ||
                                 (frame.screen == dc::Screen::settings && settings_in_pause);
        const bool interactive = frame.screen == dc::Screen::play;
        if (frame.countdown > 0.0f) {
            const int remaining = std::max(0, int(countdown_ms) - int(timer_ms_gettime64() - countdown_start));
            frame.countdown = remaining * 1000.0f / song.initial_us_per_beat;
            frame.time_ms = -remaining - frame.delay_ms - chart_delay_ms;
            if (remaining == 0) {
                const auto overlay = frame.screen;
                begin_audio();
                if (pause_layer && frame.screen == dc::Screen::play) {
                    frame.screen = overlay;
                    std::printf("FoFDC: countdown completed under pause\n");
                }
            }
        } else if (interactive || (pause_layer && audio.playing())) {
            audio.poll();
            frame.time_ms = audio.position_ms() - frame.delay_ms - chart_delay_ms;
            const dc::Rules rules{60000000.0f / song.initial_us_per_beat,
                                  frame.delay_ms + chart_delay_ms, settings.tapping};
            if (audio.error()) {
                frame.screen = dc::Screen::error;
                frame.error = audio.error();
            } else if (skip_round || !audio.playing()) {
                if (skip_round) {
                    audio.stop();
                } else {
                    round.finish(chart, frame.time_ms, rules);
                }
                const unsigned stars = round.rating(frame.gems);
                const char *result_clip = nullptr;
                if (!round.score) {
                    result_clip = "/rd/fx/jurgen1.ogg";
                } else if (round.gem_hits * 100 >= frame.gems * 99) {
                    result_clip = "/rd/fx/myhero.ogg";
                } else if (stars <= 1) {
                    constexpr const char *taunts[] = {
                        "/rd/fx/jurgen2.ogg", "/rd/fx/jurgen3.ogg", "/rd/fx/jurgen4.ogg", "/rd/fx/jurgen5.ogg"
                    };
                    result_clip = taunts[std::rand() % 4];
                } else if (stars == 5) {
                    constexpr const char *responses[] = {
                        "/rd/fx/perfect1.ogg", "/rd/fx/perfect2.ogg", "/rd/fx/perfect3.ogg"
                    };
                    result_clip = responses[std::rand() % 3];
                }
                result_audio.schedule(result_clip);
                frame.selected = 0;
                results_start_ms = timer_ms_gettime64();
                frame.results_age_ms = 0;
                frame.results_actions = false;
                frame.results_name = false;
                frame.result_song = song_index;
                frame.highscore_rank = -1;
                score_upload.reset();
                frame.screen = dc::Screen::results;
                std::printf("FoFDC: end pcm=%d elapsed=%llu frames=%u hit=%u miss=%u extra=%u score=%u\n",
                            audio.position_ms(),
                            static_cast<unsigned long long>(timer_ms_gettime64() - round_start_ms),
                            timing.frames, round.hits, round.misses, round.overstrums, round.score);
                std::printf("FoFDC: gems=%u/%u stars=%u frame_max_us=%u over25=%u over50=%u\n",
                            round.gem_hits, frame.gems, stars, timing.max_us, timing.over_25_ms, timing.over_50_ms);
                std::printf("FoFDC: input_queue_overflow=%u\n", keyboard_reports.overflow);
                pvr_stats_t pvr_stats{};
                if (pvr_get_stats(&pvr_stats) == 0) {
                    std::printf("FoFDC: pvr_vertex_peak=%u vram_free=%u aica_free=%u heap_used=%u\n",
                                unsigned(pvr_stats.vtx_buffer_used_max), unsigned(pvr_mem_available()),
                                unsigned(snd_mem_available()), unsigned(mallinfo().uordblks));
                }
            } else {
                const auto process_input = [&](Input state, int when, uint8_t releases = 0) {
                    trace_input(InputTrace::report, when, state.buttons, state.frets, round);
                    if (state.device == processed_input.device) {
                        const auto edges = state.buttons & ~processed_input.buttons;
                        if (frame.cheat_message || dismiss_cheat) {
                            processed_input.buttons = state.buttons;
                            return true;
                        }
                        if (edges & pause_song) {
                            return false;
                        }
                        blocked_frets &= state.frets;
                        state.frets &= ~blocked_frets;
                        if (state.frets != processed_input.frets || releases) {
                            const bool released = releases || (processed_input.frets & ~state.frets);
                            record_pick(round.fret_edge(chart, when, state.frets, rules, released),
                                        when, state.frets);
                            trace_input(InputTrace::fret, when, released, state.frets, round);
                        }
                        for (const auto pick : {pick_up, pick_down, pick_extra}) {
                            if (edges & pick) {
                                record_pick(round.pick(chart, when, state.frets, rules, false),
                                            when, state.frets);
                                trace_input(InputTrace::strum, when, pick, state.frets, round);
                            }
                        }
                    } else {
                        blocked_frets = 0;
                    }
                    processed_input = state;
                    processed_time = when;
                    return true;
                };
                bool keep_playing = true;
                if (interactive && input.device == dc::InputDevice::keyboard) {
                    // The cursor is sampled before refilling the audio ring. Use
                    // that sample's timestamp even when decoding takes a while.
                    const auto sampled_at_us = audio.position_sampled_at_us();
                    KeyboardReport report;
                    while (keep_playing && keyboard_reports.pop(report, sampled_at_us)) {
                        const auto age_ms = (sampled_at_us - report.timestamp_us) / 1000;
                        keep_playing = process_input(report.state,
                                                     std::max(processed_time, frame.time_ms - int(age_ms)),
                                                     report.released_frets);
                    }
                    if (keep_playing && processed_input.device != input.device) {
                        keep_playing = process_input(input, frame.time_ms);
                    }
                } else if (interactive) {
                    keep_playing = process_input(input, frame.time_ms);
                }
                if (autoplay && keep_playing) {
                    const uint8_t required = round.required_frets(chart, frame.time_ms, rules);
                    if (required && required != autoplay_frets) {
                        record_pick(round.pick(chart, frame.time_ms, required, rules, false),
                                    frame.time_ms, required);
                    }
                    autoplay_frets = required;
                }
                frame.sustain_expired = round.sustain_frets && frame.time_ms > round.sustained_until();
                record_pick(round.advance(chart, frame.time_ms,
                                          autoplay ? autoplay_frets : processed_input.frets, rules),
                            frame.time_ms, processed_input.frets, true);
                trace_input(InputTrace::advance, frame.time_ms, 0, processed_input.frets, round);
                processed_time = frame.time_ms;
                if (keep_playing) {
                    timing.sample();
                } else if (audio.pause()) {
                    show_pause();
                    std::printf("FoFDC: paused at %d ms\n", audio.position_ms());
                } else {
                    frame.screen = dc::Screen::error;
                    frame.error = audio.error();
                }
            }
        }

        dc_gameplay_active = frame.screen == dc::Screen::play;
        if (frame.screen == dc::Screen::play || (frame.screen == dc::Screen::pause && frame.countdown <= 0)) {
            if (songs[song_index].tutorial) {
                tutorial_cue(frame, tutorial);
            }
            frame.held = autoplay ? autoplay_frets : processed_input.frets;
            frame.strumming = processed_input.buttons & (pick_up | pick_down | pick_extra);
        }
        record_round(round, frame.time_ms);
        if (previous_screen == dc::Screen::menu && frame.screen != dc::Screen::menu &&
            frame.screen != dc::Screen::settings) {
            menu_audio.fade_out(1000);
        }
        if ((previous_screen == dc::Screen::setlist && frame.screen != dc::Screen::setlist) ||
            (previous_screen == dc::Screen::credits && frame.screen != dc::Screen::credits)) {
            collection_audio[preview_channel].fade_out(1000);
        }
        if (frame.screen == dc::Screen::setlist) {
            const size_t selection = frame.library_item ? songs.size() : frame.selected;
            const auto now = timer_ms_gettime64();
            if (previous_screen != frame.screen || preview_selection != selection) {
                preview_selection = selection;
                preview_due_ms = now + 1024;
            } else if (preview_due_ms && now >= preview_due_ms && selection < songs.size()) {
                preview_due_ms = 0;
                auto &outgoing = collection_audio[preview_channel];
                auto &incoming = collection_audio[preview_channel ^ 1u];
                outgoing.fade_out(1000);
                if (prepare_entry(incoming, songs[selection]) && incoming.play_prepared()) {
                    outgoing.stop(); // Loading the next track replaces SDL_mixer's music channel.
                    preview_channel ^= 1u;
                    music_level = incoming.backing_level();
                    std::printf("FoFDC: preview %s\n", songs[selection].title.c_str());
                } else {
                    std::printf("FoFDC: preview unavailable: %s\n", incoming.error());
                }
            }
        }
        result_audio.poll(frame.screen == dc::Screen::results);
        sounds.poll();
        menu_audio.poll();
        for (auto &preview : collection_audio) {
            preview.poll();
        }
        scores.poll();
        score_upload.poll();
        frame.upload_status = score_upload.status();
        frame.upload_ordinal = score_upload.ordinal();
        settings_writer.poll();
        if (frame.screen == dc::Screen::setlist) {
            frame.available = 0;
            if (!frame.library_item) {
                if (song_difficulties[frame.selected] < 0) {
                    read_chart(frame.selected);
                }
                frame.available = song_difficulties[frame.selected];
            }
            auto display = frame;
            display.selected = chooser.selected();
            display.chooser = &chooser;
            display.library_generation = chooser.generation();
            display.search = chooser.query();
            display.filter_match = chooser.matches();
            view.draw(display, song, round, chooser.items());
        } else {
            view.draw(frame, song, round, songs);
        }
    }
}
