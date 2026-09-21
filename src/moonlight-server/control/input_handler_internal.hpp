#pragma once

#include <algorithm>
#include <cmath>

#include <helpers/utils.hpp>

namespace control {

/**
 * Shared helpers used by the per-device input handlers.
 *
 * These were previously file-local to the single `input_handler.cpp`; they live here so the
 * mouse/keyboard/touch/pen/controller handlers can each live in their own translation unit.
 */

/** Convert a Moonlight network float into a clamped 0..1 value. */
inline float netfloat_to_0_1(const utils::netfloat &f) {
  return std::clamp(utils::from_netfloat(f), 0.0f, 1.0f);
}

inline float deg2rad(float degree) {
  return degree * (M_PI / 180.f);
}

} // namespace control
