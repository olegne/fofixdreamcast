// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "song_library.hpp"

namespace dc {

class SongChooser {
public:
    void open(const std::vector<SongEntry> &songs, size_t previous);
    void move(int step);
    void type(unsigned char symbol);
    bool enter();
    bool parent();
    bool matches() const;
    bool matches(const SongEntry &item) const;

    const std::vector<SongEntry> &items() const { return items_; }
    unsigned selected() const { return selected_; }
    size_t song_index() const { return indices_[selected_]; }
    bool folder() const { return items_[selected_].library_count >= 0; }
    unsigned generation() const { return generation_; }
    const std::string &query() const { return query_; }

private:
    void refresh(const std::string &previous);
    const std::vector<SongEntry> *songs_ = nullptr;
    std::string path_, query_, folded_;
    std::vector<SongEntry> items_;
    std::vector<size_t> indices_;
    unsigned selected_ = 0;
    unsigned generation_ = 0;
};

} // namespace dc
