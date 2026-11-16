// SPDX-License-Identifier: GPL-2.0-or-later
#include "editor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <zlib.h>

namespace dc {
namespace {

constexpr char signature[] = "FOFED1";
constexpr char app_id[16] = "FOF CHART EDIT";
constexpr unsigned max_file_size = 100 * 1024;
constexpr unsigned max_midi_size = 2 * 1024 * 1024;

std::string filename(const std::string &path, int slot) {
    uint32_t hash = 2166136261u;
    for (const unsigned char ch : path) {
        hash = (hash ^ ch) * 16777619u;
    }
    char name[13];
    std::snprintf(name, sizeof(name), "FE%08lX%d", static_cast<unsigned long>(hash), slot);
    return name;
}

void encode_number(std::vector<uint8_t> &buffer, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) {
        buffer.push_back(uint8_t(n >> (i * 8)));
    }
}

void encode_text(std::vector<uint8_t> &buffer, const std::string &text) {
    encode_number(buffer, static_cast<uint32_t>(text.size()));
    buffer.insert(buffer.end(), text.begin(), text.end());
}

struct Reader {
    const uint8_t *at;
    const uint8_t *end;

    size_t left() const { return end - at; }

    bool number(uint32_t &n) {
        if (left() < 4) {
            return false;
        }
        n = 0;
        for (unsigned i = 0; i < 4; ++i) {
            n |= uint32_t(*at++) << (8 * i);
        }
        return true;
    }

    bool text(std::string &text, size_t max) {
        uint32_t size;
        if (!number(size) || size > max || size > left() ||
            !std::all_of(at, at + size, [](uint8_t ch) { return ch >= 32 && ch != 127; })) {
            return false;
        }
        text.assign(at, at + size);
        at += size;
        return true;
    }
};

} // namespace

bool SongEdits::read_slot(maple_device *device, const std::string &path, int slot, Edit &edit) {
    void *raw = nullptr;
    int size = 0;
    const auto name = filename(path, slot);
    const int result = vmufs_read(device, name.c_str(), &raw, &size);
    std::unique_ptr<void, decltype(&std::free)> file(raw, &std::free);
    if (result == -2) {
        return true;
    }
    if (result || size < int(sizeof(vmu_hdr_t) + sizeof(signature) + 16) ||
        size > int(max_file_size)) {
        return false;
    }
    const auto &header = *static_cast<vmu_hdr_t *>(raw);
    if (std::memcmp(header.app_id, app_id, sizeof(app_id)) || header.icon_cnt ||
        header.eyecatch_type || header.data_len < sizeof(signature) + 16 ||
        header.data_len > unsigned(size) - sizeof(header)) {
        return false;
    }
    vmu_pkg_t package{};
    if (vmu_pkg_parse(static_cast<uint8_t *>(raw), size, &package) ||
        std::memcmp(package.data, signature, sizeof(signature))) {
        return false;
    }
    Reader reader{package.data + sizeof(signature), package.data + package.data_len};
    Edit decoded;
    std::string identity;
    uint32_t midi_size, delay;
    if (!reader.number(decoded.generation) || !decoded.generation ||
        !reader.number(midi_size) || midi_size < 14 || midi_size > max_midi_size ||
        !reader.number(delay) || delay > 20000 || !reader.number(decoded.color) ||
        !reader.text(identity, 511) || identity != path ||
        !reader.text(decoded.title, 511) || decoded.title.empty() ||
        !reader.text(decoded.artist, 511) || !reader.left()) {
        return false;
    }
    decoded.midi.resize(midi_size);
    uLongf unpacked = midi_size;
    if (uncompress(decoded.midi.data(), &unpacked, reader.at, reader.left()) != Z_OK || unpacked != midi_size) {
        return false;
    }
    SongChart chart;
    std::string error;
    if (!parse_song_midi(decoded.midi, chart, error, true)) {
        return false;
    }
    decoded.playable = std::any_of(chart.guitar.begin(), chart.guitar.end(),
                                  [](const Chart &d) { return !d.empty(); });
    decoded.delay_ms = int(delay) - 10000;
    decoded.crc = header.crc;
    decoded.slot = slot;
    decoded.device_port = device->port;
    decoded.device_unit = device->unit;
    edit = std::move(decoded);
    return true;
}

void SongEdits::load(std::vector<SongEntry> &songs) {
    for (int index = 0; auto *card = maple_enum_type(index, MAPLE_FUNC_MEMCARD); ++index) {
        vmu_dir_t *entries = nullptr;
        int count = 0;
        const auto directory_status = vmufs_readdir(card, &entries, &count);
        std::unique_ptr<vmu_dir_t, decltype(&std::free)> directory(entries, &std::free);
        if (directory_status || !count || !entries) {
            continue;
        }
        for (const auto &song : songs) {
            if (song.tutorial) {
                continue;
            }
            for (int slot = 0; slot < 2; ++slot) {
                const auto name = filename(song.midi, slot);
                const auto found = std::find_if(entries, entries + count, [&](const vmu_dir_t &entry) {
                    return std::strncmp(entry.filename, name.c_str(), sizeof(entry.filename)) == 0;
                });
                if (found == entries + count) {
                    continue;
                }
                Edit candidate;
                if (read_slot(card, song.midi, slot, candidate) && candidate.generation) {
                    const auto current = entries_.find(song.midi);
                    if (current == entries_.end() || candidate.generation > current->second.generation) {
                        entries_[song.midi] = std::move(candidate);
                    }
                }
            }
        }
    }
    for (auto &song : songs) {
        update_metadata(song);
    }
}

bool SongEdits::persist(const std::string &path, Edit &edit, std::string &message) {
    auto *device = edit.device_port < 0 ? maple_enum_type(0, MAPLE_FUNC_MEMCARD) :
                   maple_enum_dev(edit.device_port, edit.device_unit);
    if (!device || !(device->info.functions & MAPLE_FUNC_MEMCARD)) {
        message = "Chart changed in RAM. Connect a VMU to save.";
        return false;
    }
    if (edit.generation == std::numeric_limits<uint32_t>::max()) {
        message = "Chart changed in RAM. The save counter is full.";
        return false;
    }
    const int next_slot = edit.slot == 0 ? 1 : 0;
    if (edit.slot >= 0) {
        Edit current;
        if (!read_slot(device, path, edit.slot, current) || current.generation != edit.generation ||
            current.crc != edit.crc) {
            message = "Chart changed in RAM. Original VMU could not be verified.";
            return false;
        }
    }
    Edit previous;
    if (!read_slot(device, path, next_slot, previous) ||
        previous.generation >= uint64_t(edit.generation) + 1) {
        message = "Chart changed in RAM. Existing VMU file was preserved.";
        return false;
    }

    std::vector<uint8_t> payload(std::begin(signature), std::end(signature));
    encode_number(payload, edit.generation + 1);
    encode_number(payload, static_cast<uint32_t>(edit.midi.size()));
    encode_number(payload, uint32_t(edit.delay_ms + 10000));
    encode_number(payload, edit.color);
    if (path.size() > 511 || edit.title.size() > 511 || edit.artist.size() > 511) {
        message = "Chart changed in RAM. The song metadata is too long.";
        return false;
    }
    encode_text(payload, path);
    encode_text(payload, edit.title);
    encode_text(payload, edit.artist);
    const auto first = payload.size();
    uLongf bytes = compressBound(edit.midi.size());
    payload.resize(first + bytes);
    if (compress2(payload.data() + first, &bytes, edit.midi.data(), edit.midi.size(), Z_BEST_COMPRESSION) != Z_OK) {
        message = "Chart changed in RAM. Could not compress the chart.";
        return false;
    }
    payload.resize(first + bytes);
    if (payload.size() + sizeof(vmu_hdr_t) > max_file_size) {
        message = "Chart changed in RAM. This chart is too large for a VMU.";
        return false;
    }

    vmu_pkg_t package{};
    uint8_t dummy = 0;
    std::strcpy(package.desc_short, "FoF Chart");
    std::strcpy(package.desc_long, "Frets on Fire edited chart");
    std::strcpy(package.app_id, app_id);
    package.icon_data = &dummy;
    package.eyecatch_data = &dummy;
    package.data = payload.data();
    package.data_len = static_cast<int>(payload.size());
    uint8_t *raw = nullptr;
    int size = 0;
    if (vmu_pkg_build(&package, &raw, &size)) {
        message = "Chart changed in RAM. Could not build the VMU file.";
        return false;
    }
    std::unique_ptr<uint8_t, decltype(&std::free)> file(raw, &std::free);
    std::vector<uint8_t> blocks((size + 511) & ~511);
    std::copy(raw, raw + size, blocks.begin());
    const auto name = filename(path, next_slot);
    const auto result = vmufs_write(device, name.c_str(), blocks.data(), blocks.size(), VMUFS_OVERWRITE);
    if (result) {
        message = result == -7 ? "Chart changed in RAM. The VMU is full."
                               : "Chart changed in RAM. The VMU write failed.";
        return false;
    }
    Edit checked;
    if (!read_slot(device, path, next_slot, checked) || checked.generation != edit.generation + 1 ||
        checked.midi != edit.midi || checked.title != edit.title || checked.artist != edit.artist ||
        checked.delay_ms != edit.delay_ms || checked.color != edit.color) {
        message = "Chart changed in RAM. The VMU readback failed.";
        return false;
    }
    edit.generation = checked.generation;
    edit.crc = checked.crc;
    edit.device_port = checked.device_port;
    edit.device_unit = checked.device_unit;
    edit.slot = checked.slot;
    message = "Chart saved to VMU.";
    std::printf("FoFDC: editor saved midi=%s bytes=%u generation=%lu slot=%c%d\n",
                path.c_str(), unsigned(edit.midi.size()), static_cast<unsigned long>(edit.generation),
                'A' + edit.device_port, edit.device_unit);
    return true;
}

} // namespace dc
