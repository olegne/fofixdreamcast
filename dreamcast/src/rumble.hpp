// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace dc {

// Update before gameplay events and immediately on leaving play. With a
// keyboard, feedback belongs exclusively to physical port B (port index 1).
void rumble_context(bool playing, bool keyboard);

// Observe every change to the authoritative round state, including misses.
// Only an actual hit which raises the multiplier creates a short pulse.
void rumble_observe(unsigned multiplier, bool hit);

// Cancel pending feedback and establish the new song's multiplier baseline.
void rumble_reset(unsigned multiplier = 1);

// Called by the Maple queue-flush wrapper AFTER input polling and BEFORE LCD
// traffic. Its hardware-clock deadline also runs during loading/long frames.
void rumble_after_polling();

} // namespace dc
