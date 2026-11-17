// SPDX-License-Identifier: GPL-2.0-or-later
#include "credits.hpp"

namespace dc {
namespace {

// The Dreamcast attribution uses the original 1.5-line gap after the version.
// Other entries retain the order, colors and layout from 1.3.110 Credits.py.
constexpr CreditLine gap{" ", CreditAlign::center, 1.5f, false};
constexpr CreditLine logo{nullptr, CreditAlign::center, 1.0f, false};
constexpr std::array<CreditLine, 81> lines{{
    {"Unreal Voodoo", CreditAlign::center, 1.0f, true},
    {"presents", CreditAlign::center, 1.0f, false},
    {" ", CreditAlign::center, 0.5f, true},
    logo,
    {" ", CreditAlign::center, 0.5f, true},
    {"Version 1.3.110", CreditAlign::center, 0.5f, true},
    {"Dreamcast port by", CreditAlign::center, 0.5f, false},
    {"angelo128bits", CreditAlign::center, 1.0f, true},
    {"Game Design,", CreditAlign::left, 1.0f, false},
    {"Programming:", CreditAlign::left, 1.0f, false},
    {"Sami Kyostila", CreditAlign::right, 1.0f, true},
    gap,
    {"Music,", CreditAlign::left, 1.0f, false},
    {"Sound Effects:", CreditAlign::left, 1.0f, false},
    {"Tommi Inkila", CreditAlign::right, 1.0f, true},
    gap,
    {"Graphics:", CreditAlign::left, 1.0f, false},
    {"Joonas Kerttula", CreditAlign::right, 1.0f, true},
    gap,
    {"Introducing:", CreditAlign::left, 1.0f, false},
    {"Mikko Korkiakoski", CreditAlign::right, 1.0f, true},
    {"as Jurgen, Your New God", CreditAlign::right, 1.0f, true},
    gap,
    {"Marjo Hakkinen", CreditAlign::right, 1.0f, true},
    {"as Groupie", CreditAlign::right, 1.0f, true},
    gap,
    {"Song Credits:", CreditAlign::left, 1.0f, false},
    {"Bang Bang, Mystery Man", CreditAlign::right, 1.0f, true},
    {"music by Mary Jo and Tommi Inkila", CreditAlign::right, 0.5f, true},
    {"lyrics by Mary Jo", CreditAlign::right, 0.5f, true},
    gap,
    {"Defy The Machine", CreditAlign::right, 1.0f, true},
    {"music by Tommi Inkila", CreditAlign::right, 0.5f, true},
    gap,
    {"This Week I've Been", CreditAlign::right, 1.0f, true},
    {"Mostly Playing Guitar", CreditAlign::right, 1.0f, true},
    {"composed and performed by Tommi Inkila", CreditAlign::right, 0.5f, true},
    gap,
    {"Testing:", CreditAlign::left, 1.0f, false},
    {"Mikko Korkiakoski", CreditAlign::right, 1.0f, true},
    {"Tomi Kyostila", CreditAlign::right, 1.0f, true},
    {"Jani Vaarala", CreditAlign::right, 1.0f, true},
    {"Juho Jamsa", CreditAlign::right, 1.0f, true},
    {"Olli Jakola", CreditAlign::right, 1.0f, true},
    gap,
    {"Mac OS X port:", CreditAlign::left, 1.0f, false},
    {"Tero Pihlajakoski", CreditAlign::right, 1.0f, true},
    gap,
    {"Special thanks to:", CreditAlign::left, 1.0f, false},
    {"Tutorial inspired by adam02", CreditAlign::right, 1.0f, true},
    gap,
    {"Made with:", CreditAlign::left, 1.0f, false},
    {"Python", CreditAlign::right, 1.0f, true},
    {"http://www.python.org", CreditAlign::right, 0.5f, true},
    gap,
    {"PyGame", CreditAlign::right, 1.0f, true},
    {"http://www.pygame.org", CreditAlign::right, 0.5f, true},
    gap,
    {"PyOpenGL", CreditAlign::right, 1.0f, true},
    {"http://pyopengl.sourceforge.net", CreditAlign::right, 0.5f, true},
    gap,
    {"Amanith Framework", CreditAlign::right, 1.0f, true},
    {"http://www.amanith.org", CreditAlign::right, 0.5f, true},
    gap,
    {"Illusoft Collada module 1.4", CreditAlign::right, 1.0f, true},
    {"http://colladablender.illusoft.com", CreditAlign::right, 0.5f, true},
    gap,
    {"Psyco specializing compiler", CreditAlign::right, 1.0f, true},
    {"http://psyco.sourceforge.net", CreditAlign::right, 0.5f, true},
    gap,
    {"MXM Python Midi Package 0.1.4", CreditAlign::right, 1.0f, true},
    {"http://www.mxm.dk/products/public/pythonmidi", CreditAlign::right, 0.5f, true},
    gap,
    gap,
    {"Source Code available under the GNU General Public License", CreditAlign::center, 0.5f, false},
    {"http://www.unrealvoodoo.org", CreditAlign::center, 0.5f, true},
    gap,
    gap,
    gap,
    gap,
    {"Copyright 2006-2008 by Unreal Voodoo", CreditAlign::center, 0.5f, false},
}};

} // namespace

const std::array<CreditLine, 81> &credits_lines() {
    return lines;
}

} // namespace dc
