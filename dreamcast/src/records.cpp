// SPDX-License-Identifier: GPL-2.0-or-later
#include "records.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <kos/thread.h>

namespace dc {
namespace {

constexpr char app_id[] = "FRETS ON FIRE";
constexpr const char *filenames[] = {"FOF3_SCORES0", "FOF3_SCORES1"};
constexpr uint8_t signature[] = {'F', 'O', 'F', 1};
constexpr size_t max_payload = 48 * 1024;
constexpr size_t max_scores = 5;

using SaveStatus = ScoreBook::SaveStatus;

struct Snapshot {
    std::vector<uint8_t> data;
    uint32_t generation = 0;
    uint16_t crc = 0;
    int slot = -1;
    bool readable = false;
};

void put_number(std::vector<uint8_t> &data, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        data.push_back(uint8_t(value >> (8 * i)));
    }
}

void put_text(std::vector<uint8_t> &data, const std::string &text) {
    data.push_back(static_cast<uint8_t>(text.size()));
    data.insert(data.end(), text.begin(), text.end());
}

class Reader {
public:
    Reader(const uint8_t *data, size_t size) : at_(data), end_(data + size) {}

    bool number(uint32_t &value, unsigned bytes) {
        if (remaining() < bytes) {
            return false;
        }
        value = 0;
        for (unsigned i = 0; i < bytes; ++i) {
            value |= uint32_t(*at_++) << (8 * i);
        }
        return true;
    }

    bool text(std::string &value, size_t max_size, bool ascii_only = true) {
        uint32_t size;
        if (!number(size, 1) || size > max_size || remaining() < size) {
            return false;
        }
        const auto *end = at_ + size;
        if (!std::all_of(at_, end, [ascii_only](uint8_t ch) {
            return ch >= 32 && ch != 127 && (!ascii_only || ch <= 126);
        })) {
            return false;
        }
        value.assign(at_, end);
        at_ = end;
        return true;
    }

    size_t remaining() const { return end_ - at_; }

private:
    const uint8_t *at_;
    const uint8_t *end_;
};

Snapshot read_slot(maple_device_t *device, int slot) {
    void *raw = nullptr;
    int size = 0;
    const int result = vmufs_read(device, filenames[slot], &raw, &size);
    std::unique_ptr<void, decltype(&std::free)> file(raw, &std::free);
    if (result == -2) {
        return {{}, 0, 0, -1, true};  // New card or unused slot.
    }
    if (result || size < int(sizeof(vmu_hdr_t) + sizeof(signature) + 4)) {
        return {};
    }

    const auto &header = *static_cast<const vmu_hdr_t *>(raw);
    const char expected_id[sizeof(header.app_id)] = "FRETS ON FIRE";
    if (std::memcmp(header.app_id, expected_id, sizeof(expected_id)) != 0 ||
        header.icon_cnt || header.eyecatch_type || header.data_len > max_payload ||
        header.data_len < sizeof(signature) + 4 ||
        header.data_len > size - sizeof(header)) {
        return {};
    }
    vmu_pkg_t package{};
    if (vmu_pkg_parse(static_cast<uint8_t *>(raw), size, &package) ||
        std::memcmp(package.data, signature, sizeof(signature)) != 0) {
        return {};
    }
    Reader reader(package.data + sizeof(signature), package.data_len - sizeof(signature));
    uint32_t generation;
    if (!reader.number(generation, 4) || !generation) {
        return {};
    }
    return {{package.data, package.data + package.data_len}, generation, header.crc, slot, true};
}

Snapshot read_newest(maple_device_t *device) {
    auto first = read_slot(device, 0);
    auto second = read_slot(device, 1);
    if (!first.generation && !second.generation) {
        first.readable = first.readable && second.readable;
    }
    return second.generation > first.generation ? std::move(second) : std::move(first);
}

void log_status(SaveStatus status) {
    switch (status) {
        case SaveStatus::saved: std::puts("FoFDC: high scores saved to VMU"); break;
        case SaveStatus::no_vmu: std::puts("FoFDC: high scores in RAM (no VMU)"); break;
        case SaveStatus::full: std::puts("FoFDC: high scores in RAM (VMU full)"); break;
        case SaveStatus::error: std::puts("FoFDC: high scores in RAM (VMU read/write error)"); break;
        default: break;
    }
}

} // namespace

ScoreBook::~ScoreBook() {
    flush();
}

void ScoreBook::load() {
    for (int index = 0; auto *device = maple_enum_type(index, MAPLE_FUNC_MEMCARD); ++index) {
        const auto save = read_newest(device);
        if (save.generation && decode(save.data.data(), save.data.size(), songs_, name_)) {
            write_.generation = save.generation;
            write_.crc = save.crc;
            write_.slot = save.slot;
            write_.port = device->port;
            write_.unit = device->unit;
            std::printf("FoFDC: loaded high scores from VMU %c%d generation=%lu\n",
                        'A' + device->port, device->unit, static_cast<unsigned long>(save.generation));
            return;
        }
        if (save.readable && !save.generation && write_.port < 0) {
            write_.port = device->port;
            write_.unit = device->unit;
        }
    }
}

const ScoreBook::Table &ScoreBook::table(const std::string &song, unsigned difficulty) const {
    const auto it = songs_.find(song);
    static const Table empty;
    return it != songs_.end() && difficulty < it->second.size() ? it->second[difficulty] : empty;
}

bool ScoreBook::qualifies(const std::string &song, unsigned difficulty, uint32_t points) const {
    const auto &scores = table(song, difficulty);
    return difficulty < 4 && (scores.size() < max_scores || points > scores.back().points);
}

int ScoreBook::add(const std::string &song, unsigned difficulty, uint32_t points, unsigned stars,
                   const std::string &name) {
    if (!qualifies(song, difficulty, points)) {
        return -1;
    }
    if (!name.empty()) {
        name_ = name.substr(0, max_name);
    }

    auto &scores = songs_[song][difficulty];
    const auto rating = static_cast<uint8_t>(std::min(5u, stars));
    const auto insertion = std::find_if(scores.begin(), scores.end(),
                                        [points](const Score &score) { return score.points < points; });
    scores.insert(insertion, {points, rating, name_});
    if (scores.size() > max_scores) {
        scores.pop_back();
    }
    const auto rank = std::find_if(scores.begin(), scores.end(), [&](const Score &score) {
        return score.points == points && score.stars == rating && score.name == name_;
    });

    dirty_ = true;
    if (!writer_) {
        start_write();
    }
    std::printf("FoFDC: high score rank=%u difficulty=%u score=%lu stars=%u name=\"%s\"\n",
                unsigned(rank - scores.begin() + 1), difficulty, static_cast<unsigned long>(points),
                unsigned(rating), name_.c_str());
    return static_cast<int>(rank - scores.begin());
}

bool ScoreBook::decode(const uint8_t *data, size_t size, Songs &songs, std::string &name) {
    constexpr size_t header_size = sizeof(signature) + 4;
    if (size < header_size) {
        return false;
    }
    Reader reader(data + header_size, size - header_size);
    std::string player_name;
    uint32_t song_count;
    if (!reader.text(player_name, max_name) || !reader.number(song_count, 2) ||
        song_count > reader.remaining() / 6) {
        return false;
    }

    Songs decoded;
    for (uint32_t i = 0; i < song_count; ++i) {
        std::string song;
        if (!reader.text(song, 255, false) || song.empty()) {
            return false;
        }
        auto [it, inserted] = decoded.try_emplace(song);
        if (!inserted) {
            return false;
        }
        for (auto &scores : it->second) {
            uint32_t count;
            if (!reader.number(count, 1) || count > max_scores) {
                return false;
            }
            for (uint32_t j = 0; j < count; ++j) {
                Score score{};
                uint32_t stars;
                if (!reader.number(score.points, 4) || !reader.number(stars, 1) || stars > 5 ||
                    !reader.text(score.name, max_name) ||
                    (j && scores.back().points < score.points)) {
                    return false;
                }
                score.stars = static_cast<uint8_t>(stars);
                scores.push_back(std::move(score));
            }
        }
    }
    if (reader.remaining()) {
        return false;
    }
    songs = std::move(decoded);
    name = std::move(player_name);
    return true;
}

void ScoreBook::start_write() {
    dirty_ = false;
    if (write_.generation == std::numeric_limits<uint32_t>::max()) {
        status_ = SaveStatus::error;
        log_status(status_);
        return;
    }
    auto &data = write_.data;
    data.assign(std::begin(signature), std::end(signature));
    put_number(data, write_.generation + 1, 4);
    put_text(data, name_);
    put_number(data, static_cast<uint32_t>(songs_.size()), 2);
    for (const auto &[song, tables] : songs_) {
        if (song.size() > 255 || data.size() > max_payload) {
            status_ = SaveStatus::full;
            log_status(status_);
            return;
        }
        put_text(data, song);
        for (const auto &scores : tables) {
            data.push_back(static_cast<uint8_t>(scores.size()));
            for (const auto &score : scores) {
                put_number(data, score.points, 4);
                data.push_back(score.stars);
                put_text(data, score.name);
            }
        }
    }
    if (data.size() > max_payload) {
        status_ = SaveStatus::full;
        log_status(status_);
        return;
    }
    write_.done.store(false, std::memory_order_relaxed);
    writer_ = thd_create(false, write_to_vmu, &write_);
    status_ = writer_ ? SaveStatus::pending : SaveStatus::error;
    log_status(status_);
}

void ScoreBook::poll() {
    if (writer_ && write_.done.load(std::memory_order_acquire)) {
        finish_write();
    }
}

void ScoreBook::flush() {
    while (writer_) {
        finish_write();
    }
}

void ScoreBook::finish_write() {
    thd_join(writer_, nullptr);
    writer_ = nullptr;
    status_ = write_.status;
    log_status(status_);
    if (dirty_) {
        start_write();
    }
}

void *ScoreBook::write_to_vmu(void *context) {
    auto &write = *static_cast<Write *>(context);
    write.status = [&] {
        auto *device = write.port < 0 ? maple_enum_type(0, MAPLE_FUNC_MEMCARD)
                                      : maple_enum_dev(write.port, write.unit);
        if (!device || !(device->info.functions & MAPLE_FUNC_MEMCARD)) {
            return SaveStatus::no_vmu;
        }

        // Check the latest copy before touching the inactive one. A card with
        // different data may have been swapped in after the records were loaded.
        const auto current = read_newest(device);
        if (!current.readable || current.generation != write.generation || current.crc != write.crc ||
            current.slot != write.slot) {
            return SaveStatus::error;
        }
        vmu_pkg_t package{};
        uint8_t dummy = 0;
        std::strcpy(package.desc_short, "FoF Records");
        std::strcpy(package.desc_long, "Frets on Fire 1.3.110 records");
        std::strcpy(package.app_id, app_id);
        package.icon_data = &dummy;
        package.eyecatch_data = &dummy;
        package.data_len = static_cast<int>(write.data.size());
        package.data = write.data.data();
        uint8_t *raw = nullptr;
        int size = 0;
        if (vmu_pkg_build(&package, &raw, &size)) {
            return SaveStatus::error;
        }
        std::unique_ptr<uint8_t, decltype(&std::free)> file(raw, &std::free);
        std::vector<uint8_t> blocks((size + 511) & ~511);
        std::copy(raw, raw + size, blocks.begin());

        const int slot = write.slot == 0 ? 1 : 0;
        const int result = vmufs_write(device, filenames[slot], blocks.data(), blocks.size(), VMUFS_OVERWRITE);
        if (result) {
            return result == -7 ? SaveStatus::full : SaveStatus::error;
        }
        write.generation++;
        write.crc = reinterpret_cast<const vmu_hdr_t *>(raw)->crc;
        write.port = device->port;
        write.unit = device->unit;
        write.slot = slot;
        return SaveStatus::saved;
    }();
    write.done.store(true, std::memory_order_release);
    return nullptr;
}

} // namespace dc
