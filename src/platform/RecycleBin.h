// ---------------------------------------------------------------------------
// RecycleBin.h - moving files to the Recycle Bin and revealing them.
//
// The one rule: never delete permanently. Every path that could end in a
// permanent delete (remote volume, NukeOnDelete, bin too small, no bin)
// returns a "Kept*" outcome instead.
// ---------------------------------------------------------------------------
#pragma once

#include "platform/Win.h"

#include <string>

namespace hh::platform {

enum class RecycleOutcome {
    Recycled,        ///< file is in the Recycle Bin
    KeptRemote,      ///< network volume: no bin, file kept
    KeptNoBin,       ///< volume has no Recycle Bin (or NukeOnDelete): file kept
    KeptTooLarge,    ///< file exceeds the bin's capacity: file kept
    KeptNotFound,    ///< file was already gone
    Failed,          ///< shell operation failed: file kept
};

struct RecycleResult {
    RecycleOutcome outcome = RecycleOutcome::Failed;
    std::wstring message;    ///< human explanation for logs / UI
};

/// Moves a file to the Recycle Bin (must be called on an STA thread).
RecycleResult recycleFile(const std::wstring& path);

/// Explorer window with the file selected (falls back to opening the folder).
bool revealInExplorer(const std::wstring& path);
/// Opens a folder in Explorer.
bool openFolder(const std::wstring& folder);
/// Opens a URL or document with the default handler.
bool openWithShell(const std::wstring& target);

} // namespace hh::platform
