// ---------------------------------------------------------------------------
// ShellServicesMac.mm - ShellServices.h on macOS.
//
//   clipboard  NSPasteboard (plain string)
//   version    CFBundleShortVersionString of the running bundle
//   pickers    NSOpenPanel, app-modal so the view models keep their simple
//              synchronous "show, then use the answer" flow
// ---------------------------------------------------------------------------
#include "ui/app/ShellServices.h"

#include "core/Logger.h"
#include "platform/FileIo.h"
#include "platform/Utf.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

namespace hh::ui {

namespace {

constexpr const wchar_t* kLog = L"ViewModel";

/// Version shown when the bundle carries no version (tests, the bare binary).
constexpr const wchar_t* kFallbackVersion = L"1.0.0";

/// Wide text -> NSString (nil-safe).
NSString* ns(std::wstring_view text) {
    const std::string utf8 = platform::toUtf8(text);
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding] ?: @"";
}

/// NSString -> wide text.
std::wstring wide(NSString* s) {
    if (s == nil) {
        return {};
    }
    const char* utf8 = [s UTF8String];
    return utf8 ? platform::normalizeNfc(platform::toWide(utf8)) : std::wstring();
}

} // namespace

bool copyTextToClipboard(NativeWindowHandle owner, std::wstring_view text) {
    static_cast<void>(owner);
    @autoreleasepool {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        if (text.empty()) {
            return true;
        }
        const BOOL ok = [board setString:ns(text) forType:NSPasteboardTypeString];
        if (!ok) {
            HH_LOG_WARN(kLog, L"NSPasteboard refused the text");
        }
        return ok == YES;
    }
}

std::wstring appVersionString() {
    static const std::wstring cached = []() -> std::wstring {
        @autoreleasepool {
            NSString* version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            if (![version isKindOfClass:[NSString class]] || version.length == 0) {
                return kFallbackVersion;
            }
            return wide(version);
        }
    }();
    return cached;
}

std::wstring showPathPicker(NativeWindowHandle owner, bool pickFolder, const wchar_t* title,
                            const std::wstring& startFolder, const wchar_t* filterLabel,
                            const wchar_t* filterPattern) {
    static_cast<void>(owner);
    static_cast<void>(filterLabel);
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = !pickFolder;
        panel.canChooseDirectories = pickFolder;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = pickFolder;
        panel.resolvesAliases = YES;
        if (title != nullptr && *title != L'\0') {
            panel.message = ns(title);
            panel.prompt = pickFolder ? @"Choose" : @"Open";
        }

        // "*.cube" narrows the list to that extension; an exact name (the
        // Windows "mkvmerge.exe" style) cannot be expressed as a type, so
        // those pickers show everything and the caller validates the pick.
        if (!pickFolder && filterPattern != nullptr) {
            const std::wstring pattern(filterPattern);
            if (pattern.size() > 2 && pattern[0] == L'*' && pattern[1] == L'.') {
                UTType* type = [UTType typeWithFilenameExtension:ns(pattern.substr(2))];
                if (type != nil) {
                    panel.allowedContentTypes = @[ type ];
                }
            }
        }

        // Start where the current value points.
        if (!startFolder.empty() && platform::isDirectory(startFolder)) {
            panel.directoryURL = [NSURL fileURLWithPath:ns(startFolder) isDirectory:YES];
        }

        // App-modal: the answer is needed before the caller continues.
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK) {
            return {};
        }
        NSURL* url = panel.URLs.firstObject;
        if (url == nil || !url.isFileURL) {
            return {};
        }
        return wide(url.path);
    }
}

} // namespace hh::ui
