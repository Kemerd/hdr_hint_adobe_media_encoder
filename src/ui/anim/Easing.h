// ---------------------------------------------------------------------------
// Easing.h - timed easings for the few things a spring is overkill for.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>

namespace hh::ui::easing {

inline float linear(float t) { return std::clamp(t, 0.0f, 1.0f); }
inline float easeOutCubic(float t) { t = std::clamp(t, 0.0f, 1.0f); const float u = 1.0f - t; return 1.0f - u * u * u; }
inline float easeInOutSine(float t) { t = std::clamp(t, 0.0f, 1.0f); return 0.5f * (1.0f - std::cos(3.14159265f * t)); }
inline float easeInOutCubic(float t) { t = std::clamp(t, 0.0f, 1.0f); return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }

/// A fixed-duration animation from 0 to 1.
struct Timed {
    double start = 0;
    double duration = 0.15;
    float (*fn)(float) = &easeOutCubic;
    [[nodiscard]] float at(double now) const {
        if (duration <= 0) return 1.0f;
        return fn(static_cast<float>((now - start) / duration));
    }
    [[nodiscard]] bool done(double now) const { return now >= start + duration; }
};

} // namespace hh::ui::easing
