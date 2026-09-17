// ---------------------------------------------------------------------------
// JobModel.cpp - helpers on the job data model: identity checks, display
// strings and the enum <-> text tables used by persistence and IPC.
// ---------------------------------------------------------------------------
#include "core/JobModel.h"

#include "core/PathUtil.h"
#include "platform/Utf.h"

#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace hh {

namespace {

/**
 * @brief "59.94", "30", "23.976": three decimals with trailing zeros removed.
 */
std::wstring formatFps(double fps) {
    if (!std::isfinite(fps) || fps <= 0.0) {
        return {};
    }
    std::wstring s = std::format(L"{:.3f}", fps);
    // Trim "30.000" -> "30" and "59.940" -> "59.94".
    while (!s.empty() && s.back() == L'0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == L'.') {
        s.pop_back();
    }
    return s;
}

/**
 * @brief Human name of a transfer for the summary line (empty for Unknown).
 */
const wchar_t* transferLabel(TransferKind t) noexcept {
    switch (t) {
    case TransferKind::PQ:  return L"Rec.2100 PQ";
    case TransferKind::HLG: return L"Rec.2100 HLG";
    case TransferKind::SDR: return L"Rec.709 SDR";
    case TransferKind::Unknown: break;
    }
    return L"";
}

} // namespace

// ---------------------------------------------------------------------------
// VideoSummary
// ---------------------------------------------------------------------------

/**
 * @brief Joins the known parts with a middle dot: "3840x2160 · 59.94 fps ·
 *        Rec.2100 PQ · Hardware (Nvidia)". Empty parts are skipped; when
 *        nothing is known the raw log line is returned instead.
 */
std::wstring VideoSummary::describe() const {
    std::vector<std::wstring> parts;

    // Resolution only when both dimensions are known.
    if (width > 0 && height > 0) {
        parts.push_back(std::format(L"{}x{}", width, height));
    }

    // Frame rate.
    const std::wstring fpsText = formatFps(fps);
    if (!fpsText.empty()) {
        parts.push_back(fpsText + L" fps");
    }

    // Transfer characteristic.
    const std::wstring_view transferText = transferLabel(transfer);
    if (!transferText.empty()) {
        parts.emplace_back(transferText);
    }

    // Codec / preset hint when the log gave us one.
    const std::wstring_view codec = platform::trim(codecHint);
    if (!codec.empty()) {
        parts.emplace_back(codec);
    }

    // Encoder: "Hardware (Nvidia)", "Hardware", or just the vendor name.
    const std::wstring_view vendorText = platform::trim(vendor);
    if (hardwareEncoding) {
        parts.push_back(vendorText.empty() ? std::wstring(L"Hardware")
                                           : std::format(L"Hardware ({})", vendorText));
    } else if (!vendorText.empty()) {
        parts.emplace_back(vendorText);
    }

    // Nothing parsed: fall back to the raw line so the UI still shows something.
    if (parts.empty()) {
        return std::wstring(platform::trim(raw));
    }
    return platform::join(parts, L" · ");
}

// ---------------------------------------------------------------------------
// SourceStamp
// ---------------------------------------------------------------------------

/**
 * @brief Same file? Volume serial + file id when both sides have one,
 *        otherwise creation time + size. Two invalid stamps never match.
 */
bool SourceStamp::sameFileAs(const SourceStamp& o) const noexcept {
    if (!valid || !o.valid) {
        return false;
    }
    if (hasFileId && o.hasFileId) {
        return volumeSerial == o.volumeSerial && fileIdLow == o.fileIdLow && fileIdHigh == o.fileIdHigh;
    }
    return creationUtc == o.creationUtc && size == o.size;
}

// ---------------------------------------------------------------------------
// Job
// ---------------------------------------------------------------------------

/**
 * @brief Done / Failed / SkippedSdr / Cancelled are terminal.
 */
bool Job::isTerminal() const noexcept {
    switch (state) {
    case JobState::Done:
    case JobState::Failed:
    case JobState::SkippedSdr:
    case JobState::Cancelled:
        return true;
    case JobState::Discovered:
    case JobState::Encoding:
    case JobState::Ready:
    case JobState::Held:
    case JobState::Muxing:
    case JobState::Verifying:
        break;
    }
    return false;
}

/**
 * @brief File name of the AME output (falls back to the key when empty).
 */
std::wstring Job::displayName() const {
    std::wstring name = path::fileName(outputPath);
    if (name.empty()) {
        name = path::fileName(key);
    }
    return name;
}

// ---------------------------------------------------------------------------
// enum -> text
// ---------------------------------------------------------------------------

const wchar_t* toString(JobState s) noexcept {
    switch (s) {
    case JobState::Discovered: return L"Discovered";
    case JobState::Encoding:   return L"Encoding";
    case JobState::Ready:      return L"Ready";
    case JobState::Held:       return L"Held";
    case JobState::Muxing:     return L"Muxing";
    case JobState::Verifying:  return L"Verifying";
    case JobState::Done:       return L"Done";
    case JobState::Failed:     return L"Failed";
    case JobState::SkippedSdr: return L"SkippedSdr";
    case JobState::Cancelled:  return L"Cancelled";
    }
    return L"Discovered";
}

const wchar_t* toString(TransferKind t) noexcept {
    switch (t) {
    case TransferKind::Unknown: return L"Unknown";
    case TransferKind::PQ:      return L"PQ";
    case TransferKind::HLG:     return L"HLG";
    case TransferKind::SDR:     return L"SDR";
    }
    return L"Unknown";
}

const wchar_t* toString(JobSource s) noexcept {
    switch (s) {
    case JobSource::Log:     return L"Log";
    case JobSource::Folder:  return L"Folder";
    case JobSource::Cep:     return L"Cep";
    case JobSource::Manual:  return L"Manual";
    case JobSource::CatchUp: return L"CatchUp";
    }
    return L"Folder";
}

const wchar_t* toString(RecycleStatus s) noexcept {
    switch (s) {
    case RecycleStatus::NotRequested:      return L"NotRequested";
    case RecycleStatus::Pending:           return L"Pending";
    case RecycleStatus::Recycled:          return L"Recycled";
    case RecycleStatus::KeptNoBin:         return L"KeptNoBin";
    case RecycleStatus::KeptSourceChanged: return L"KeptSourceChanged";
    case RecycleStatus::KeptDisabled:      return L"KeptDisabled";
    case RecycleStatus::Failed:            return L"Failed";
    }
    return L"NotRequested";
}

// ---------------------------------------------------------------------------
// text -> enum (case-insensitive so hand-edited files still load)
// ---------------------------------------------------------------------------

std::optional<JobState> jobStateFromString(std::wstring_view s) noexcept {
    const std::wstring_view t = platform::trim(s);
    if (t.empty()) {
        return std::nullopt;
    }
    // Walk the enum range and compare against the canonical names.
    static constexpr JobState kAll[] = {
        JobState::Discovered, JobState::Encoding, JobState::Ready, JobState::Held, JobState::Muxing,
        JobState::Verifying, JobState::Done, JobState::Failed, JobState::SkippedSdr, JobState::Cancelled,
    };
    for (JobState v : kAll) {
        if (platform::iequals(t, toString(v))) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<TransferKind> transferKindFromString(std::wstring_view s) noexcept {
    const std::wstring_view t = platform::trim(s);
    if (t.empty()) {
        return std::nullopt;
    }
    static constexpr TransferKind kAll[] = {
        TransferKind::Unknown, TransferKind::PQ, TransferKind::HLG, TransferKind::SDR,
    };
    for (TransferKind v : kAll) {
        if (platform::iequals(t, toString(v))) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<JobSource> jobSourceFromString(std::wstring_view s) noexcept {
    const std::wstring_view t = platform::trim(s);
    if (t.empty()) {
        return std::nullopt;
    }
    static constexpr JobSource kAll[] = {
        JobSource::Log, JobSource::Folder, JobSource::Cep, JobSource::Manual, JobSource::CatchUp,
    };
    for (JobSource v : kAll) {
        if (platform::iequals(t, toString(v))) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<RecycleStatus> recycleStatusFromString(std::wstring_view s) noexcept {
    const std::wstring_view t = platform::trim(s);
    if (t.empty()) {
        return std::nullopt;
    }
    static constexpr RecycleStatus kAll[] = {
        RecycleStatus::NotRequested, RecycleStatus::Pending, RecycleStatus::Recycled, RecycleStatus::KeptNoBin,
        RecycleStatus::KeptSourceChanged, RecycleStatus::KeptDisabled, RecycleStatus::Failed,
    };
    for (RecycleStatus v : kAll) {
        if (platform::iequals(t, toString(v))) {
            return v;
        }
    }
    return std::nullopt;
}

} // namespace hh
