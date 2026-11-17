#!/bin/sh
# Compile the real transport code against a deterministic Maple bus fixture.
set -eu
feedback_test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
feedback_source_root=$(CDPATH= cd -- "$feedback_test_root/../../src" && pwd)
feedback_build_dir=$(mktemp -d)
trap 'rm -rf "$feedback_build_dir"' EXIT HUP INT TERM

"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -DFRETSONFIRE_INPUT_TRACE \
    -I"$feedback_test_root/rumble_stubs" -I"$feedback_source_root" \
    "$feedback_test_root/rumble_lifecycle_test.cpp" "$feedback_source_root/rumble.cpp" \
    -o "$feedback_build_dir/rumble"
for feedback_scenario in thresholds keyboard_port controller_port pause_restart source_change \
    lost_reply again_bounded detach_inflight busy_and_failure rapid_crossings nonhit_and_absent; do
    "$feedback_build_dir/rumble" "$feedback_scenario"
done

"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -DFRETSONFIRE_INPUT_TRACE \
    -I"$feedback_test_root/lcd_stubs" -I"$feedback_source_root" \
    "$feedback_test_root/lcd_transport_test.cpp" "$feedback_source_root/vmu_lcd.cpp" \
    -o "$feedback_build_dir/lcd"
"$feedback_build_dir/lcd"
