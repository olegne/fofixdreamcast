// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct kthread;

namespace dc {

struct Score {
    uint32_t points;
    uint8_t stars;
    std::string name;
};

class ScoreBook {
public:
    static constexpr size_t max_name = 20;
    static constexpr size_t max_name_bytes = 4 * max_name;
    using Table = std::vector<Score>;

    enum class SaveStatus { none, pending, saved, no_vmu, full, error };

    ~ScoreBook();
    void load();
    void load_disc(const char *path);
    void use_chart(const std::string &song, const std::string &edit_key);
    const Table &table(const std::string &song, unsigned difficulty) const;
    bool qualifies(const std::string &song, unsigned difficulty, uint32_t points) const;
    int add(const std::string &song, unsigned difficulty, uint32_t points, unsigned stars,
            const std::string &name);
    const std::string &name() const { return name_; }
    void poll();
    void flush();
    SaveStatus save_status() const { return status_; }

private:
    using Songs = std::map<std::string, std::array<Table, 4>>;

    struct Write {
        std::vector<uint8_t> data;
        uint32_t generation = 0;
        uint16_t crc = 0;
        int port = -1;
        int unit = 0;
        int slot = -1;
        SaveStatus status = SaveStatus::error;
        std::atomic<bool> done{false};
    };

    const std::string &chart_key(const std::string &song) const;
    void merge_disc_scores(const std::string &song, unsigned difficulty);
    void start_write();
    void finish_write();
    static void *write_to_vmu(void *context);
    static bool decode(const uint8_t *data, size_t size, Songs &songs, std::string &name);

    Songs songs_;
    Songs disc_songs_;
    Songs combined_scores_;
    std::map<std::string, std::string> chart_keys_;
    std::string name_;
    kthread *writer_ = nullptr;
    Write write_;
    SaveStatus status_ = SaveStatus::none;
    bool dirty_ = false;
};

} // namespace dc
