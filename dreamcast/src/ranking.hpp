// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ranking_wire.hpp"

#include <atomic>
#include <kos/thread.h>

namespace dc {

enum class UploadStatus { idle, pending, succeeded, failed };

class ScoreUpload {
public:
    ~ScoreUpload();
    void start(const ScoreBook &scores, const std::string &midi_path, const std::vector<uint8_t> *edited);
    void poll();
    void reset();
    UploadStatus status() const { return status_; }
    int ordinal() const { return ordinal_; }

private:
    static void *send_scores(void *context);

    struct Job {
        ranking::Tables tables;
        std::string midi_path;
        std::vector<uint8_t> edited_midi;
        ranking::Reply reply;
    };

    Job job_;
    kthread_t *thread_ = nullptr;
    std::atomic<bool> done_{false};
    UploadStatus status_ = UploadStatus::idle;
    int ordinal_ = 0;
};

} // namespace dc
