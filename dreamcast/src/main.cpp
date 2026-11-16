// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"
#include "chart.hpp"
#include "song_library.hpp"
#include "view.hpp"

#include <algorithm>
#include <array>
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
            // SDL mixer channels use 128 for full gain and 32 for screwupvol=0.25.
            const unsigned volume = i >= miss1 ? 32 : 128;
            if (!samples_[i].load(path, volume)) {
                std::printf("FoFDC: could not load sound %s\n", path);
            }
        }
    }

    void play(Effect effect) {
        const auto &clip = samples_[effect];
        if (!clip.loaded()) {
            return;
        }
        // GuitarScene replaces the last mixer channel. Other effects take the first free channel.
        const auto target = effect >= miss1 ? voices_.end() - 1 :
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
        if (!voice || !voice->start_clip(clip)) {
            std::printf("FoFDC: could not play sound %u\n", effect);
        }
    }

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
    for (const auto &cue : cues) {
        const int elapsed = frame.time_ms - cue.start;
        if (elapsed < 0 || elapsed > cue.duration) {
            continue;
        }
        if (cue.picture < 0) {
            frame.caption = cue.text.c_str();
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
static dc::ResultAudio result_audio;
static SoundEffects sounds;

enum InputButton : unsigned {
    nav_up = 1u << 0, nav_down = 1u << 1, nav_left = 1u << 2, nav_right = 1u << 3,
    select_option = 1u << 4, cancel_option = 1u << 5, pause_song = 1u << 6,
    pick_up = 1u << 7, pick_down = 1u << 8, pick_extra = 1u << 9
};

struct Input {
    dc::InputDevice device = dc::InputDevice::none;
    unsigned buttons = 0;
    uint8_t frets = 0;
};

Input read_keyboard(const kbd_state_t &keys) {
    Input input{dc::InputDevice::keyboard};
    for (unsigned fret = 0; fret < 5; ++fret) {
        if (keys.key_states[KBD_KEY_F1 + fret].is_down) {
            input.frets |= 1u << fret;
        }
    }
    constexpr struct {
        kbd_key_t key;
        unsigned buttons;
    } bindings[] = {
        {KBD_KEY_UP, nav_up}, {KBD_KEY_DOWN, nav_down},
        {KBD_KEY_LEFT, nav_left}, {KBD_KEY_RIGHT, nav_right},
        {KBD_KEY_ENTER, select_option | pick_down}, {KBD_KEY_ESCAPE, cancel_option | pause_song}
    };
    for (const auto &binding : bindings) {
        if (keys.key_states[binding.key].is_down) {
            input.buttons |= binding.buttons;
        }
    }
    if (keys.cond.modifiers.raw & KBD_MOD_RSHIFT) {
        input.buttons |= pick_up;
    }
    return input;
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
};

KeyboardReports *KeyboardReports::observer_ = nullptr;

unsigned move_list(unsigned current, unsigned count, int direction) {
    return (current + count + direction) % count;
}

enum class NameCommand { editing, accept, cancel };

NameCommand read_name_keys(std::string &name, uint64_t &last_key_ms) {
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
            if (!name.empty()) {
                name.pop_back();
            }
            continue;
        }
        kbd_mods_t mods{};
        kbd_leds_t leds{};
        mods.raw = static_cast<uint8_t>(raw >> 8);
        leds.raw = static_cast<uint8_t>(raw >> 16);
        const auto ch = static_cast<unsigned char>(kbd_key_to_ascii(key, keyboard->region, mods, leds));
        if (ch >= 32 && ch <= 126 && name.size() < dc::ScoreBook::max_name) {
            name.push_back(ch);
        }
    }
    return NameCommand::editing;
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
    dc::ScoreBook scores;
    scores.load();
    std::printf("FoFDC: Maple report sampling at GETCOND completion\n");
    const auto tutorial = read_tutorial_cues();
    const auto songs = dc::song_library();
    dc::SongChart song;
    dc::Round round;
    dc::Frame frame;
    frame.scores = &scores;
    std::vector<int> song_difficulties(songs.size(), -1);
    size_t loaded_chart = songs.size();
    std::string load_error;
    bool training = false;
    bool settings_in_pause = false;
    unsigned song_index = 1;
    unsigned last_regular_song = 1;
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
    uint64_t name_frame_ms = 0;
    bool closing_name = false;
    unsigned name_letter = 0;
    uint64_t credits_start_ms = 0;
    uint64_t credits_close_ms = 0;
    float credits_visibility_at_close = 0.0f;
    bool error_to_main = false;
    FrameTiming timing;
    frame.delay_ms = 100;
    frame.difficulty = 2; // Player.py defaults to Easy.

    const auto to_setlist = [&] {
        audio.stop();
        frame.countdown = 0;
        training = false;
        if (songs.size() < 2) {
            frame.screen = dc::Screen::error;
            frame.error = "No songs on the disc.";
            error_to_main = true;
            return;
        }
        song_index = last_regular_song;
        frame.screen = dc::Screen::setlist;
        frame.selected = song_index;
    };
    const auto to_main = [&] {
        audio.stop();
        frame.countdown = 0;
        frame.screen = dc::Screen::menu;
        frame.selected = training ? 1 : 0;
    };
    const auto read_chart = [&](size_t index) {
        if (loaded_chart == index) {
            return true;
        }
        if (!dc::load_song_chart(songs[index].midi.c_str(), song, load_error)) {
            loaded_chart = songs.size();
            song_difficulties[index] = 0;
            return false;
        }
        loaded_chart = index;
        unsigned available = 0;
        for (unsigned i = 0; i < song.guitar.size(); ++i) {
            if (!song.guitar[i].empty()) {
                available |= 1u << i;
            }
        }
        song_difficulties[index] = available;
        return true;
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
        frame.selected = (frame.available & (1u << frame.difficulty)) ? frame.difficulty :
                         31 - __builtin_clz(frame.available);
        frame.screen = dc::Screen::difficulty;
        std::printf("FoFDC: loaded %s (A=%u M=%u E=%u S=%u)\n", entry.title.c_str(),
                    unsigned(song.guitar[0].size()), unsigned(song.guitar[1].size()),
                    unsigned(song.guitar[2].size()), unsigned(song.guitar[3].size()));
    };
    const auto prepare_audio = [](const char *backing, const char *guitar = nullptr) {
        return audio.prepare(backing, guitar, +[] {
            result_audio.poll(false);
            sounds.poll();
        });
    };
    const auto begin_countdown = [&] {
        frame.countdown = 0;
        const auto &entry = songs[song_index];
        const auto prep_start = timer_us_gettime64();
        if (!prepare_audio(entry.backing.c_str(), entry.guitar.empty() ? nullptr : entry.guitar.c_str())) {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
            return;
        }
        std::printf("FoFDC: audio prepared before countdown in %llu us\n",
                    static_cast<unsigned long long>(timer_us_gettime64() - prep_start));
        round = dc::Round{};
        view.reset_effects();
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
        if (audio.playing() || audio.resume()) {
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
        audio.poll();
        if (!audio.playing() && (!prepare_audio("/rd/menu.ogg") || !audio.play_prepared())) {
            std::printf("FoFDC: menu audio error: %s\n", audio.error());
        }
    };

    const auto record_pick = [&](dc::Pick result, int when, uint8_t frets, bool after_stage_update = false) {
        audio.guitar_gain(round.guitar_muted ? 0.0f : 1.0f);
        if (result == dc::Pick::hit) {
            trace_input(InputTrace::hit, when, round.last_hit_ms - round.last_hit_offset_ms, frets, round);
            view.record_pick(round, after_stage_update);
        } else if (result == dc::Pick::bad) {
            sounds.play(static_cast<Effect>(miss1 + std::rand() % 6));
        }
    };

    for (;;) {
        const auto input = read_input();
        const bool same_device = input.device == frame.input;
        unsigned pressed = same_device && input.device == dc::InputDevice::controller ?
                           input.buttons & ~previous_buttons : 0;
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
                    // Menu.keyPressed consumes presses. Its inherited
                    // keyReleased still passes fret releases to GuitarScene.
                    if (pause_menu && frame.countdown <= 0 && report.released_frets &&
                        processed_input.device == dc::InputDevice::keyboard) {
                        const dc::Rules rules{60000000.0f / song.initial_us_per_beat,
                                              frame.delay_ms + chart_delay_ms};
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
        const int step = bool(pressed & nav_down) - bool(pressed & nav_up) +
                         (frame.input == dc::InputDevice::keyboard && (pressed & pick_up) ? 1 : 0);
        const int adjust = bool((pressed & nav_right) || (menu_frets & (1u << 3))) -
                           bool((pressed & nav_left) || (menu_frets & (1u << 2)));
        frame.caption = nullptr;
        frame.tutorial_picture = -1;

        switch (frame.screen) {
            case dc::Screen::menu:
                resume_menu_music();
                frame.selected = move_list(frame.selected, 6, step);
                toggle(step);
                if (confirm) {
                    sounds.play(accept);
                    if (frame.selected == 5) {
                        scores.flush();
                        arch_menu();
                    } else if (frame.selected == 3) {
                        settings_in_pause = false;
                        frame.screen = dc::Screen::settings;
                        frame.selected = 0;
                    } else if (frame.selected == 4) {
                        const bool started = audio.start("/cd/songs/defy/song.ogg", "/cd/songs/defy/guitar.ogg");
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
                        audio.stop();
                        error_to_main = true;
                        frame.screen = dc::Screen::error;
                        frame.error = "This menu is in development.";
                    }
                } else if (cancel) {
                    scores.flush();
                    arch_menu();
                }
                break;
            case dc::Screen::settings:
                if (!settings_in_pause) {
                    resume_menu_music();
                }
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step || adjust);
                if (frame.selected == 0) {
                    frame.delay_ms = std::clamp(frame.delay_ms + 10 * adjust, -250, 250);
                } else if (frame.selected == 1) {
                    frame.board_speed = std::clamp(frame.board_speed + 5 * adjust, 50, 200);
                }
                if (cancel || (confirm && frame.selected == 2)) {
                    frame.selected = settings_in_pause ? 0 : 3;
                    frame.screen = settings_in_pause ? dc::Screen::pause : dc::Screen::menu;
                    sounds.play(back);
                }
                break;
            case dc::Screen::credits: {
                audio.poll();
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
            case dc::Screen::setlist:
                frame.selected = 1 + move_list(frame.selected - 1, songs.size() - 1, step);
                toggle(step);
                if (confirm) {
                    song_index = last_regular_song = frame.selected;
                    sounds.play(accept);
                    load_song();
                } else if (cancel) {
                    sounds.play(back);
                    to_main();
                }
                break;
            case dc::Screen::difficulty:
                if (step) {
                    do {
                        frame.selected = move_list(frame.selected, 4, -step);
                    } while (!(frame.available & (1u << frame.selected)));
                }
                toggle(step);
                if (confirm) {
                    frame.difficulty = frame.selected;
                    begin_countdown();
                } else if (cancel) {
                    sounds.play(back);
                    to_setlist();
                }
                break;
            case dc::Screen::countdown:
                if (pressed & pause_song) {
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
                    settings_in_pause = true;
                    frame.screen = dc::Screen::settings;
                    frame.selected = 0;
                } else if (confirm) {
                    to_main();
                }
                break;
            }
            case dc::Screen::results: {
                frame.results_age_ms = static_cast<unsigned>(timer_ms_gettime64() - results_start_ms);
                const auto &record_song = songs[frame.result_song].midi;
                if (frame.results_name) {
                    const auto now = timer_ms_gettime64();
                    const float transition = (now - name_frame_ms) / 512.0f;
                    name_frame_ms = now;
                    if (closing_name) {
                        frame.name_visibility = std::max(0.0f, frame.name_visibility - transition);
                        if (frame.name_visibility == 0.0f) {
                            frame.highscore_rank = scores.add(record_song, frame.difficulty, round.score,
                                                              round.rating(frame.gems), frame.score_name);
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
                        command = read_name_keys(frame.score_name, name_key_ms);
                    } else if (confirm || cancel) {
                        command = cancel ? NameCommand::cancel : NameCommand::accept;
                    } else {
                        constexpr char wheel[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ ";
                        name_letter = move_list(name_letter, sizeof(wheel) - 1, step);
                        frame.name_wheel = wheel[name_letter];
                        if (adjust > 0 && frame.score_name.size() < dc::ScoreBook::max_name) {
                            frame.score_name.push_back(frame.name_wheel);
                        } else if (adjust < 0 && !frame.score_name.empty()) {
                            frame.score_name.pop_back();
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
                        if (scores.qualifies(record_song, frame.difficulty, round.score)) {
                            kbd_set_repeat_timing(300, 30);
                            if (auto *device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD)) {
                                while (kbd_queue_pop(device, false) != KBD_QUEUE_END) {}
                            }
                            frame.score_name = scores.name();
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
                                  frame.delay_ms + chart_delay_ms};
            if (audio.error()) {
                frame.screen = dc::Screen::error;
                frame.error = audio.error();
            } else if (!audio.playing()) {
                round.finish(chart, frame.time_ms, rules);
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
                frame.screen = dc::Screen::results;
                std::printf("FoFDC: end pcm=%d elapsed=%llu frames=%u hit=%u miss=%u extra=%u score=%u\n",
                            audio.position_ms(),
                            static_cast<unsigned long long>(timer_ms_gettime64() - round_start_ms),
                            timing.frames, round.hits, round.misses, round.overstrums, round.score);
                std::printf("FoFDC: gems=%u/%u stars=%u frame_max_us=%u over25=%u over50=%u\n",
                            round.gem_hits, frame.gems, stars, timing.max_us, timing.over_25_ms, timing.over_50_ms);
                std::printf("FoFDC: input_queue_overflow=%u\n", keyboard_reports.overflow);
            } else {
                if (songs[song_index].tutorial) {
                    tutorial_cue(frame, tutorial);
                }
                const auto process_input = [&](Input state, int when, uint8_t releases = 0) {
                    trace_input(InputTrace::report, when, state.buttons, state.frets, round);
                    if (state.device == processed_input.device) {
                        const auto edges = state.buttons & ~processed_input.buttons;
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
                record_pick(round.advance(chart, frame.time_ms, processed_input.frets, rules),
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
            frame.held = processed_input.frets;
            frame.strumming = processed_input.buttons & (pick_up | pick_down | pick_extra);
        }
        record_round(round, frame.time_ms);
        result_audio.poll(frame.screen == dc::Screen::results);
        sounds.poll();
        scores.poll();
        if (frame.screen == dc::Screen::setlist) {
            if (song_difficulties[frame.selected] < 0) {
                read_chart(frame.selected);
            }
            frame.available = song_difficulties[frame.selected];
        }
        view.draw(frame, song, round, songs);
    }
}
