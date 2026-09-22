// ---------------------------------------------------------------------------
// posix/RecycleBin.mm - the Trash and Finder reveals on macOS.
//
// Same one rule as Windows: never delete permanently. -[NSFileManager
// trashItemAtURL:] either moves the item into the right Trash (the volume's
// .Trashes on external disks) or fails; it never falls back to a delete.
// Network volumes are refused up front, exactly like the Windows build,
// because many SMB/AFP servers have no Trash at all.
// ---------------------------------------------------------------------------
#include "platform/RecycleBin.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"
#include "platform/posix/PosixCommon.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <string>

namespace hh::platform {

namespace {

/// Component tag for every log line written from this file.
constexpr const wchar_t* kLog = L"RecycleBin";

/// Builds a result and logs nothing (callers log with context).
RecycleResult makeResult(RecycleOutcome outcome, std::wstring message) {
    RecycleResult r;
    r.outcome = outcome;
    r.message = std::move(message);
    return r;
}

/// Wide path -> file URL (nil for an empty / unrepresentable path).
NSURL* fileUrl(const std::wstring& path) {
    const std::string native = posix::toNative(path);
    if (native.empty()) {
        return nil;
    }
    NSString* str = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:native.c_str()
                                                                                length:native.size()];
    return str ? [NSURL fileURLWithPath:str] : nil;
}

/// NSError -> readable wide text.
std::wstring describe(NSError* error) {
    if (error == nil) {
        return L"unknown error";
    }
    NSString* text = error.localizedDescription ?: @"unknown error";
    return toWide([text UTF8String] ?: "unknown error");
}

} // namespace

RecycleResult recycleFile(const std::wstring& path) {
    if (path.empty()) {
        return makeResult(RecycleOutcome::Failed, L"No path given.");
    }
    const std::wstring plain = fullPath(path);

    // ---- 1. does it still exist? ---------------------------------------
    if (!exists(plain)) {
        HH_LOG_INFO(kLog, L"{} not found; nothing to move to the Trash", plain);
        return makeResult(RecycleOutcome::KeptNotFound, L"The file no longer exists: " + plain);
    }

    // ---- 2. network volumes may have no Trash ---------------------------
    if (isRemotePath(plain)) {
        HH_LOG_INFO(kLog, L"{} is on a network volume; keeping it", plain);
        return makeResult(RecycleOutcome::KeptRemote, L"Network volumes have no Trash, so the file was kept: " + plain);
    }

    // ---- 3. move it --------------------------------------------------------
    @autoreleasepool {
        NSURL* url = fileUrl(plain);
        if (url == nil) {
            return makeResult(RecycleOutcome::Failed, L"The path could not be represented: " + plain);
        }
        NSError* error = nil;
        NSURL* resulting = nil;
        const BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:&resulting error:&error];
        if (!ok) {
            const std::wstring why = describe(error);
            // A volume without a Trash reports "feature unsupported": the file stays put.
            if (error != nil && [error.domain isEqualToString:NSCocoaErrorDomain] &&
                error.code == NSFeatureUnsupportedError) {
                HH_LOG_WARN(kLog, L"volume of {} has no Trash ({}); keeping it", plain, why);
                return makeResult(RecycleOutcome::KeptNoBin, L"This volume has no Trash, so the file was kept: " + plain);
            }
            HH_LOG_ERROR(kLog, L"trashItemAtURL({}) failed: {}", plain, why);
            return makeResult(RecycleOutcome::Failed, L"Could not move " + plain + L" to the Trash (" + why + L").");
        }
    }
    HH_LOG_INFO(kLog, L"moved {} to the Trash", plain);
    return makeResult(RecycleOutcome::Recycled, L"Moved to the Trash: " + plain);
}

bool revealInExplorer(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    @autoreleasepool {
        // Select the file when it exists, otherwise open the nearest folder that does.
        if (exists(path)) {
            NSURL* url = fileUrl(fullPath(path));
            if (url == nil) {
                return false;
            }
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
            return true;
        }
    }
    const std::wstring full = fullPath(path);
    const size_t slash = full.find_last_of(L'/');
    return slash != std::wstring::npos && openFolder(slash == 0 ? std::wstring(L"/") : full.substr(0, slash));
}

bool openFolder(const std::wstring& folder) {
    if (folder.empty() || !isDirectory(folder)) {
        return false;
    }
    @autoreleasepool {
        NSURL* url = fileUrl(fullPath(folder));
        return url != nil && [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

bool openWithShell(const std::wstring& target) {
    if (target.empty()) {
        return false;
    }
    @autoreleasepool {
        NSURL* url = nil;
        if (target.find(L"://") != std::wstring::npos || istartsWith(target, L"mailto:")) {
            NSString* str = [NSString stringWithUTF8String:toUtf8(target).c_str()];
            url = str ? [NSURL URLWithString:str] : nil;
        } else {
            url = fileUrl(fullPath(target));
        }
        if (url == nil) {
            HH_LOG_WARN(kLog, L"cannot open '{}': not a valid URL or path", target);
            return false;
        }
        return [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

} // namespace hh::platform
