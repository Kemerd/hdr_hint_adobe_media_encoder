// ---------------------------------------------------------------------------
// TextStyle.cpp - cache key for typography tokens.
// ---------------------------------------------------------------------------
#include "ui/gfx/TextStyle.h"

#include <bit>
#include <cstdint>

namespace hh::ui {

namespace {

// FNV-1a 64-bit: tiny, well distributed for the handful of styles we have,
// and stable across runs (the key is only used in-process).
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

/**
 * @brief Folds one 64-bit value into the running FNV-1a hash byte by byte.
 */
uint64_t fold(uint64_t hash, uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xFFu;
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

/**
 * @brief Hashes every field that affects the DirectWrite format or layout.
 *
 * Floats go in by their bit pattern so 14.0 and 14.0001 produce different
 * formats (they would also produce different layouts).
 */
uint64_t TextStyle::key() const noexcept {
    uint64_t h = kFnvOffset;
    h = fold(h, static_cast<uint64_t>(family));
    h = fold(h, std::bit_cast<uint32_t>(size));
    h = fold(h, static_cast<uint64_t>(weight));
    h = fold(h, italic ? 1u : 0u);
    h = fold(h, std::bit_cast<uint32_t>(lineHeight));
    h = fold(h, std::bit_cast<uint32_t>(letterSpacing));
    h = fold(h, uppercase ? 1u : 0u);
    h = fold(h, wrap ? 1u : 0u);
    return h;
}

} // namespace hh::ui
