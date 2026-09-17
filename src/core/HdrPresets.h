// ---------------------------------------------------------------------------
// HdrPresets.h - the HDR colour metadata presets.
//
// Values are kept as strings exactly like the Python tool so that what the
// user typed ("0.170") is what mkvmerge receives. An empty field emits no flag.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "core/JobModel.h"
#include "platform/Win.h"

#include <string>
#include <vector>

namespace hh {

struct HdrPreset {
    enum class Family { HLG, PQ, SDR, Custom, Source };

    std::wstring id;             ///< stable id, e.g. "generic_pq_1000"
    std::wstring label;          ///< display label, e.g. "Generic Rec.2100 PQ / HDR10 (1000 nits)"
    Family family = Family::PQ;

    std::wstring matrix;         ///< --color-matrix-coefficients   (e.g. "9")
    std::wstring range;          ///< --color-range                 (e.g. "1")
    std::wstring transfer;       ///< --color-transfer-characteristics (16 PQ, 18 HLG, 1 SDR)
    std::wstring primaries;      ///< --color-primaries             (e.g. "9")
    std::wstring maxCll;         ///< --max-content-light
    std::wstring maxFall;        ///< --max-frame-light
    std::wstring chromaticity;   ///< --chromaticity-coordinates    "rx,ry,gx,gy,bx,by"
    std::wstring whitePoint;     ///< --white-color-coordinates     "x,y"
    std::wstring maxLuminance;   ///< --max-luminance
    std::wstring minLuminance;   ///< --min-luminance
    bool builtIn = true;

    /// Custom/Source are sentinels: they carry no values of their own.
    [[nodiscard]] bool isSentinel() const noexcept { return family == Family::Custom || family == Family::Source; }
    /// PQ / HLG / SDR derived from the transfer field.
    [[nodiscard]] TransferKind transferKind() const noexcept;
    /// True when every colour field equals @p other's (ignores id/label).
    [[nodiscard]] bool sameValuesAs(const HdrPreset& other) const noexcept;
};

/// A label/value pair for the enum dropdowns (matrix, range, transfer, primaries).
struct EnumOption {
    std::wstring label;
    std::wstring value;
};

/**
 * @brief Built-in presets (ported from hdr_gui.py) plus user presets from JSON.
 */
class PresetRegistry {
public:
    PresetRegistry();

    /// The 11 built-in presets in display order (+ the "custom"/"source" sentinels last).
    [[nodiscard]] const std::vector<HdrPreset>& builtIn() const noexcept { return builtIn_; }
    /// User presets loaded from presets.json.
    [[nodiscard]] const std::vector<HdrPreset>& user() const noexcept { return user_; }
    /// Built-in followed by user presets.
    [[nodiscard]] std::vector<HdrPreset> all() const;
    /// Presets usable for a given transfer (sentinels excluded).
    [[nodiscard]] std::vector<HdrPreset> forTransfer(TransferKind t) const;

    [[nodiscard]] const HdrPreset* find(std::wstring_view id) const noexcept;
    [[nodiscard]] const HdrPreset* findByLabel(std::wstring_view label) const noexcept;

    /// Loads user presets ({"version":1,"presets":[...]}); missing file = ok, empty.
    Result<void> loadUser(const std::wstring& path);
    Result<void> saveUser(const std::wstring& path) const;
    /// Adds or replaces a user preset (id gets "_user" suffix when it collides with a built-in).
    void upsertUser(HdrPreset preset);
    bool removeUser(std::wstring_view id);

    /// Default preset id for a transfer kind ("generic_pq_1000", "generic_hlg", "sdr_rec709").
    static std::wstring defaultIdFor(TransferKind t);

    /**
     * @brief mkvmerge colour flags for a preset, e.g.
     *        {"--color-matrix-coefficients","0:9","--color-range","0:1",...}
     *        Empty fields emit nothing. Sentinels emit nothing.
     */
    static std::vector<std::wstring> colourFlags(const HdrPreset& preset, int trackId);

    static const std::vector<EnumOption>& matrixOptions();
    static const std::vector<EnumOption>& rangeOptions();
    static const std::vector<EnumOption>& transferOptions();
    static const std::vector<EnumOption>& primariesOptions();

private:
    std::vector<HdrPreset> builtIn_;
    std::vector<HdrPreset> user_;
};

} // namespace hh
