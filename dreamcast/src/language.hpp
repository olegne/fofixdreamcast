// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace dc {

class Language {
public:
    static unsigned count();
    static const char *name(unsigned index);

    void select(unsigned index);
    const char *translate(const char *source) const;
    std::string format(const char *source, const std::string &value) const;
    bool international() const { return !messages_.empty(); }
    bool reversed() const { return reversed_; }

private:
    unsigned selected_ = 0;
    bool reversed_ = false;
    std::vector<char> catalog_;
    std::vector<std::pair<const char *, const char *>> messages_;
};

} // namespace dc
