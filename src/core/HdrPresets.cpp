// ---------------------------------------------------------------------------
// HdrPresets.cpp - the HDR colour metadata presets.
//
// The built-in table is a verbatim port of HDR_PRESETS from hdr_gui.py, in
// the same display order. Values stay strings on purpose: what the user typed
// ("0.170") is exactly what mkvmerge receives.
// ---------------------------------------------------------------------------
#include "core/HdrPresets.h"

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace hh {

namespace {

constexpr const wchar_t* kLog = L"Presets";
constexpr uint64_t kMaxPresetsBytes = 4ull * 1024 * 1024;
constexpr int kPresetsFileVersion = 1;

// The shared PQ mastering-display values (Rec.2020 primaries, D65 white).
constexpr const wchar_t* kRec2020Chromaticity = L"0.708,0.292,0.170,0.797,0.131,0.046";
constexpr const wchar_t* kD65WhitePoint = L"0.3127,0.329";

/**
 * @brief Convenience builder for the built-in table.
 */
HdrPreset make(const wchar_t* id, const wchar_t* label, HdrPreset::Family family,
               const wchar_t* matrix, const wchar_t* range, const wchar_t* transfer, const wchar_t* primaries,
               const wchar_t* maxCll = L"", const wchar_t* maxFall = L"", const wchar_t* chromaticity = L"",
               const wchar_t* whitePoint = L"", const wchar_t* maxLuminance = L"", const wchar_t* minLuminance = L"") {
    HdrPreset p;
    p.id = id;
    p.label = label;
    p.family = family;
    p.matrix = matrix;
    p.range = range;
    p.transfer = transfer;
    p.primaries = primaries;
    p.maxCll = maxCll;
    p.maxFall = maxFall;
    p.chromaticity = chromaticity;
    p.whitePoint = whitePoint;
    p.maxLuminance = maxLuminance;
    p.minLuminance = minLuminance;
    p.builtIn = true;
    return p;
}

/**
 * @brief Sentinel entries ("custom", "source") carry no values.
 */
HdrPreset makeSentinel(const wchar_t* id, const wchar_t* label, HdrPreset::Family family) {
    HdrPreset p;
    p.id = id;
    p.label = label;
    p.family = family;
    p.builtIn = true;
    return p;
}

/// Family -> JSON token.
const char* familyToJson(HdrPreset::Family f) noexcept {
    switch (f) {
    case HdrPreset::Family::HLG: return "hlg";
    case HdrPreset::Family::PQ:  return "pq";
    case HdrPreset::Family::SDR: return "sdr";
    case HdrPreset::Family::Custom: return "custom";
    case HdrPreset::Family::Source: return "source";
    }
    return "pq";
}

/// JSON token -> family (nullopt when unknown).
std::optional<HdrPreset::Family> familyFromJson(std::wstring_view s) noexcept {
    const std::wstring_view t = platform::trim(s);
    if (platform::iequals(t, L"hlg")) return HdrPreset::Family::HLG;
    if (platform::iequals(t, L"pq"))  return HdrPreset::Family::PQ;
    if (platform::iequals(t, L"sdr")) return HdrPreset::Family::SDR;
    return std::nullopt;
}

/**
 * @brief Transfer code text -> kind, independent of the preset's family:
 *        "16" -> PQ, "18" -> HLG, "1"/"6" -> SDR, anything else Unknown.
 */
TransferKind kindFromTransferText(std::wstring_view transfer) noexcept {
    const std::wstring_view t = platform::trim(transfer);
    if (t == L"16") return TransferKind::PQ;
    if (t == L"18") return TransferKind::HLG;
    if (t == L"1" || t == L"6") return TransferKind::SDR;
    return TransferKind::Unknown;
}

/// TransferKind -> the family that carries it (PQ for Unknown, the safest default).
HdrPreset::Family familyForTransfer(TransferKind t) noexcept {
    switch (t) {
    case TransferKind::HLG: return HdrPreset::Family::HLG;
    case TransferKind::SDR: return HdrPreset::Family::SDR;
    case TransferKind::PQ:
    case TransferKind::Unknown:
        break;
    }
    return HdrPreset::Family::PQ;
}

/**
 * @brief Reads a JSON member as a wide string. Numbers are accepted and
 *        rendered verbatim; anything else yields an empty string.
 */
std::wstring jsonString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || key == nullptr) {
        return {};
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return {};
    }
    if (it->is_string()) {
        return platform::toWide(it->get_ref<const std::string&>());
    }
    if (it->is_number() || it->is_boolean()) {
        return platform::toWide(it->dump());
    }
    return {};
}

/**
 * @brief Adds "--flag <trackId>:<value>" when the value is non-empty.
 */
void pushFlag(std::vector<std::wstring>& out, const wchar_t* flag, int trackId, const std::wstring& value) {
    const std::wstring_view v = platform::trim(value);
    if (v.empty()) {
        return;
    }
    out.emplace_back(flag);
    out.push_back(std::to_wstring(trackId) + L":" + std::wstring(v));
}

/**
 * @brief Lower-cased id made of [a-z0-9_] for presets that arrive without one.
 */
std::wstring slugify(std::wstring_view label) {
    std::wstring out;
    out.reserve(label.size());
    for (wchar_t c : platform::toLowerInvariant(label)) {
        const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        if (alnum) {
            out += c;
        } else if (!out.empty() && out.back() != L'_') {
            out += L'_';
        }
    }
    while (!out.empty() && out.back() == L'_') {
        out.pop_back();
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// HdrPreset
// ---------------------------------------------------------------------------

/**
 * @brief "16" -> PQ, "18" -> HLG, "1"/"6" -> SDR, anything else Unknown.
 */
TransferKind HdrPreset::transferKind() const noexcept {
    if (isSentinel()) {
        return TransferKind::Unknown;
    }
    return kindFromTransferText(transfer);
}

/**
 * @brief Field-by-field comparison (trimmed); id, label and family are ignored.
 */
bool HdrPreset::sameValuesAs(const HdrPreset& other) const noexcept {
    const auto eq = [](const std::wstring& a, const std::wstring& b) noexcept {
        return platform::trim(a) == platform::trim(b);
    };
    return eq(matrix, other.matrix) && eq(range, other.range) && eq(transfer, other.transfer) &&
           eq(primaries, other.primaries) && eq(maxCll, other.maxCll) && eq(maxFall, other.maxFall) &&
           eq(chromaticity, other.chromaticity) && eq(whitePoint, other.whitePoint) &&
           eq(maxLuminance, other.maxLuminance) && eq(minLuminance, other.minLuminance);
}

// ---------------------------------------------------------------------------
// PresetRegistry: built-ins
// ---------------------------------------------------------------------------

/**
 * @brief Fills the built-in table (display order = hdr_gui.py order).
 */
PresetRegistry::PresetRegistry() {
    using F = HdrPreset::Family;
    builtIn_.reserve(13);

    // ---- HLG family (no mastering-display metadata needed) -----------------
    builtIn_.push_back(make(L"dji_pocket3_hlg", L"DJI Osmo Pocket 3 (HLG)",        F::HLG, L"9", L"1", L"18", L"9"));
    builtIn_.push_back(make(L"dji_mavic_hlg",   L"DJI Mavic / Air / Mini (HLG)",   F::HLG, L"9", L"1", L"18", L"9"));
    builtIn_.push_back(make(L"sony_hlg",        L"Sony HLG (a7S III / FX3 / FX6)", F::HLG, L"9", L"1", L"18", L"9"));
    builtIn_.push_back(make(L"panasonic_hlg",   L"Panasonic HLG (GH5/GH6/S5)",     F::HLG, L"9", L"1", L"18", L"9"));
    builtIn_.push_back(make(L"generic_hlg",     L"Generic Rec.2100 HLG",           F::HLG, L"9", L"1", L"18", L"9"));

    // ---- PQ / HDR10 family (mastering metadata required) -------------------
    builtIn_.push_back(make(L"iphone_pq", L"iPhone Dolby Vision / HDR10 (PQ)", F::PQ, L"9", L"1", L"16", L"9",
                            L"1000", L"400", kRec2020Chromaticity, kD65WhitePoint, L"1000", L"0.0001"));
    builtIn_.push_back(make(L"sony_hdr10_pq", L"Sony HDR10 (PQ)", F::PQ, L"9", L"1", L"16", L"9",
                            L"1000", L"400", kRec2020Chromaticity, kD65WhitePoint, L"1000", L"0.0001"));
    builtIn_.push_back(make(L"generic_pq_1000", L"Generic Rec.2100 PQ / HDR10 (1000 nits)", F::PQ, L"9", L"1", L"16", L"9",
                            L"1000", L"400", kRec2020Chromaticity, kD65WhitePoint, L"1000", L"0.0001"));
    builtIn_.push_back(make(L"generic_pq_4000", L"Generic Rec.2100 PQ / HDR10 (4000 nits)", F::PQ, L"9", L"1", L"16", L"9",
                            L"4000", L"1000", kRec2020Chromaticity, kD65WhitePoint, L"4000", L"0.0001"));

    // ---- SDR ---------------------------------------------------------------
    builtIn_.push_back(make(L"sdr_rec709", L"SDR Rec.709 (no HDR)", F::SDR, L"1", L"1", L"1", L"1"));

    // ---- Sentinels (always last) -------------------------------------------
    builtIn_.push_back(makeSentinel(L"custom", L"Custom (manual)", F::Custom));
    builtIn_.push_back(makeSentinel(L"source", L"From source", F::Source));
}

/**
 * @brief Built-in followed by user presets.
 */
std::vector<HdrPreset> PresetRegistry::all() const {
    std::vector<HdrPreset> out;
    out.reserve(builtIn_.size() + user_.size());
    out.insert(out.end(), builtIn_.begin(), builtIn_.end());
    out.insert(out.end(), user_.begin(), user_.end());
    return out;
}

/**
 * @brief Non-sentinel presets of the matching family; every family for Unknown.
 */
std::vector<HdrPreset> PresetRegistry::forTransfer(TransferKind t) const {
    std::vector<HdrPreset> out;
    for (const HdrPreset& p : all()) {
        if (p.isSentinel()) {
            continue;
        }
        switch (t) {
        case TransferKind::PQ:
            if (p.family == HdrPreset::Family::PQ) out.push_back(p);
            break;
        case TransferKind::HLG:
            if (p.family == HdrPreset::Family::HLG) out.push_back(p);
            break;
        case TransferKind::SDR:
            if (p.family == HdrPreset::Family::SDR) out.push_back(p);
            break;
        case TransferKind::Unknown:
            out.push_back(p);
            break;
        }
    }
    return out;
}

/**
 * @brief Lookup by id (case-insensitive). Built-ins win over user presets.
 */
const HdrPreset* PresetRegistry::find(std::wstring_view id) const noexcept {
    const std::wstring_view t = platform::trim(id);
    if (t.empty()) {
        return nullptr;
    }
    for (const HdrPreset& p : builtIn_) {
        if (platform::iequals(p.id, t)) {
            return &p;
        }
    }
    for (const HdrPreset& p : user_) {
        if (platform::iequals(p.id, t)) {
            return &p;
        }
    }
    return nullptr;
}

/**
 * @brief Lookup by display label (case-insensitive), e.g. for legacy files.
 */
const HdrPreset* PresetRegistry::findByLabel(std::wstring_view label) const noexcept {
    const std::wstring_view t = platform::trim(label);
    if (t.empty()) {
        return nullptr;
    }
    for (const HdrPreset& p : builtIn_) {
        if (platform::iequals(p.label, t)) {
            return &p;
        }
    }
    for (const HdrPreset& p : user_) {
        if (platform::iequals(p.label, t)) {
            return &p;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// PresetRegistry: user presets (JSON)
// ---------------------------------------------------------------------------

/**
 * @brief Loads {"version":1,"presets":[{id,label,family,matrix,...}]}.
 *        A missing file is fine (no user presets). Malformed entries are
 *        skipped with a warning; a malformed document is an error.
 */
Result<void> PresetRegistry::loadUser(const std::wstring& path) {
    user_.clear();
    if (platform::trim(path).empty()) {
        return Error::text(L"PresetRegistry::loadUser: empty path");
    }
    if (!platform::exists(path)) {
        return {};
    }

    auto data = platform::readAll(path, kMaxPresetsBytes);
    if (!data) {
        return data.error();
    }
    const std::vector<uint8_t>& bytes = data.value();

    // Skip a UTF-8 BOM if an editor added one.
    size_t start = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        start = 3;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()) + start, bytes.size() - start);
    if (platform::trim(text).empty()) {
        return {};
    }

    // Exceptions are disabled for parsing; the try block is a last line of
    // defence against anything the library might still throw on odd input.
    try {
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            return Error::text(L"presets.json is not a JSON object: " + path);
        }

        // Version check: newer files are still read (unknown keys are ignored).
        const auto ver = doc.find("version");
        if (ver != doc.end() && ver->is_number_integer() && ver->get<int>() > kPresetsFileVersion) {
            HH_LOG_WARN(kLog, L"presets.json version {} is newer than {}; reading what we understand",
                        ver->get<int>(), kPresetsFileVersion);
        }

        const auto presets = doc.find("presets");
        if (presets == doc.end() || !presets->is_array()) {
            return {};
        }

        size_t index = 0;
        for (const nlohmann::json& entry : *presets) {
            ++index;
            if (!entry.is_object()) {
                HH_LOG_WARN(kLog, L"presets.json entry #{} is not an object; skipped", index);
                continue;
            }

            HdrPreset p;
            p.builtIn = false;
            p.id = std::wstring(platform::trim(jsonString(entry, "id")));
            p.label = std::wstring(platform::trim(jsonString(entry, "label")));
            p.matrix = jsonString(entry, "matrix");
            p.range = jsonString(entry, "range");
            p.transfer = jsonString(entry, "transfer");
            p.primaries = jsonString(entry, "primaries");
            p.maxCll = jsonString(entry, "maxCll");
            p.maxFall = jsonString(entry, "maxFall");
            p.chromaticity = jsonString(entry, "chromaticity");
            p.whitePoint = jsonString(entry, "whitePoint");
            p.maxLuminance = jsonString(entry, "maxLuminance");
            p.minLuminance = jsonString(entry, "minLuminance");

            // Family: explicit token, else derived from the transfer code.
            const std::optional<HdrPreset::Family> fam = familyFromJson(jsonString(entry, "family"));
            p.family = fam.value_or(familyForTransfer(kindFromTransferText(p.transfer)));

            if (p.id.empty() && p.label.empty()) {
                HH_LOG_WARN(kLog, L"presets.json entry #{} has neither id nor label; skipped", index);
                continue;
            }
            upsertUser(std::move(p));
        }
    } catch (...) {
        user_.clear();
        return Error::text(L"presets.json could not be parsed: " + path);
    }

    HH_LOG_INFO(kLog, L"loaded {} user preset(s) from {}", user_.size(), path);
    return {};
}

/**
 * @brief Writes the user presets as pretty-printed JSON (atomic replace).
 */
Result<void> PresetRegistry::saveUser(const std::wstring& path) const {
    if (platform::trim(path).empty()) {
        return Error::text(L"PresetRegistry::saveUser: empty path");
    }

    std::string text;
    try {
        nlohmann::json doc = nlohmann::json::object();
        doc["version"] = kPresetsFileVersion;
        nlohmann::json list = nlohmann::json::array();
        for (const HdrPreset& p : user_) {
            nlohmann::json e = nlohmann::json::object();
            e["id"] = platform::toUtf8(p.id);
            e["label"] = platform::toUtf8(p.label);
            e["family"] = familyToJson(p.family);
            e["matrix"] = platform::toUtf8(p.matrix);
            e["range"] = platform::toUtf8(p.range);
            e["transfer"] = platform::toUtf8(p.transfer);
            e["primaries"] = platform::toUtf8(p.primaries);
            e["maxCll"] = platform::toUtf8(p.maxCll);
            e["maxFall"] = platform::toUtf8(p.maxFall);
            e["chromaticity"] = platform::toUtf8(p.chromaticity);
            e["whitePoint"] = platform::toUtf8(p.whitePoint);
            e["maxLuminance"] = platform::toUtf8(p.maxLuminance);
            e["minLuminance"] = platform::toUtf8(p.minLuminance);
            list.push_back(std::move(e));
        }
        doc["presets"] = std::move(list);
        // The replace handler guarantees dump() never throws on odd strings.
        text = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        text += '\n';
    } catch (...) {
        return Error::text(L"PresetRegistry::saveUser: could not serialise presets");
    }

    const std::wstring dir = path::parent(path);
    if (!dir.empty()) {
        if (auto made = platform::createDirectories(dir); !made) {
            return made.error();
        }
    }
    return platform::writeAllAtomic(path, text);
}

/**
 * @brief Adds or replaces a user preset. Sentinel families are not allowed
 *        for user presets; an id that collides with a built-in gets "_user".
 */
void PresetRegistry::upsertUser(HdrPreset preset) {
    preset.builtIn = false;
    preset.id = std::wstring(platform::trim(preset.id));
    preset.label = std::wstring(platform::trim(preset.label));

    // Derive whatever is missing between id and label.
    if (preset.id.empty()) {
        preset.id = slugify(preset.label);
        if (preset.id.empty()) {
            preset.id = L"user_preset";
        }
    }
    if (preset.label.empty()) {
        preset.label = preset.id;
    }

    // User presets always carry values, so they cannot be Custom/Source. The
    // family is read off the transfer code directly (transferKind() would
    // report Unknown for a sentinel family).
    if (preset.isSentinel()) {
        preset.family = familyForTransfer(kindFromTransferText(preset.transfer));
    }

    // Keep clear of the built-in ids.
    const auto collidesWithBuiltIn = [this](const std::wstring& id) {
        return std::any_of(builtIn_.begin(), builtIn_.end(),
                           [&id](const HdrPreset& b) { return platform::iequals(b.id, id); });
    };
    if (collidesWithBuiltIn(preset.id)) {
        preset.id += L"_user";
        for (int n = 2; n < 1000 && collidesWithBuiltIn(preset.id); ++n) {
            preset.id = std::wstring(platform::trim(preset.id)) + L"_" + std::to_wstring(n);
        }
    }

    // Replace in place or append.
    for (HdrPreset& existing : user_) {
        if (platform::iequals(existing.id, preset.id)) {
            existing = std::move(preset);
            return;
        }
    }
    user_.push_back(std::move(preset));
}

/**
 * @brief Removes a user preset by id; false when there was none.
 */
bool PresetRegistry::removeUser(std::wstring_view id) {
    const std::wstring_view t = platform::trim(id);
    if (t.empty()) {
        return false;
    }
    const auto before = user_.size();
    std::erase_if(user_, [t](const HdrPreset& p) { return platform::iequals(p.id, t); });
    return user_.size() != before;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

/**
 * @brief Default id per transfer; empty for Unknown (the job is held instead).
 */
std::wstring PresetRegistry::defaultIdFor(TransferKind t) {
    switch (t) {
    case TransferKind::PQ:  return L"generic_pq_1000";
    case TransferKind::HLG: return L"generic_hlg";
    case TransferKind::SDR: return L"sdr_rec709";
    case TransferKind::Unknown: break;
    }
    return {};
}

/**
 * @brief mkvmerge colour flags in the order the Python tool emitted them.
 *        Empty fields and sentinels emit nothing.
 */
std::vector<std::wstring> PresetRegistry::colourFlags(const HdrPreset& preset, int trackId) {
    std::vector<std::wstring> out;
    if (preset.isSentinel()) {
        return out;
    }
    if (trackId < 0) {
        HH_LOG_WARN(kLog, L"colourFlags: negative track id {} clamped to 0", trackId);
        trackId = 0;
    }
    out.reserve(20);
    pushFlag(out, L"--color-matrix-coefficients",      trackId, preset.matrix);
    pushFlag(out, L"--color-range",                    trackId, preset.range);
    pushFlag(out, L"--color-transfer-characteristics", trackId, preset.transfer);
    pushFlag(out, L"--color-primaries",                trackId, preset.primaries);
    pushFlag(out, L"--max-content-light",              trackId, preset.maxCll);
    pushFlag(out, L"--max-frame-light",                trackId, preset.maxFall);
    pushFlag(out, L"--chromaticity-coordinates",       trackId, preset.chromaticity);
    pushFlag(out, L"--white-color-coordinates",        trackId, preset.whitePoint);
    pushFlag(out, L"--max-luminance",                  trackId, preset.maxLuminance);
    pushFlag(out, L"--min-luminance",                  trackId, preset.minLuminance);
    return out;
}

const std::vector<EnumOption>& PresetRegistry::matrixOptions() {
    static const std::vector<EnumOption> options = {
        {L"1 - BT.709",              L"1"},
        {L"9 - BT.2020 non-constant", L"9"},
        {L"10 - BT.2020 constant",   L"10"},
        {L"0 - Identity / RGB",      L"0"},
    };
    return options;
}

const std::vector<EnumOption>& PresetRegistry::rangeOptions() {
    static const std::vector<EnumOption> options = {
        {L"1 - Limited / Broadcast", L"1"},
        {L"2 - Full / PC",           L"2"},
        {L"0 - Unspecified",         L"0"},
    };
    return options;
}

const std::vector<EnumOption>& PresetRegistry::transferOptions() {
    static const std::vector<EnumOption> options = {
        {L"1 - BT.709 (SDR)",                L"1"},
        {L"16 - SMPTE ST 2084 (PQ / HDR10)", L"16"},
        {L"18 - ARIB STD-B67 (HLG)",         L"18"},
        {L"6 - SMPTE 170M (NTSC)",           L"6"},
        {L"14 - BT.2020 10-bit",             L"14"},
        {L"15 - BT.2020 12-bit",             L"15"},
    };
    return options;
}

const std::vector<EnumOption>& PresetRegistry::primariesOptions() {
    static const std::vector<EnumOption> options = {
        {L"1 - BT.709",            L"1"},
        {L"9 - BT.2020",           L"9"},
        {L"11 - DCI P3",           L"11"},
        {L"12 - Display P3 / D65", L"12"},
    };
    return options;
}

} // namespace hh
