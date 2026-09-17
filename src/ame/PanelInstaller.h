// ---------------------------------------------------------------------------
// PanelInstaller.h - installs the CEP panel into %APPDATA%\Adobe\CEP\extensions.
// ---------------------------------------------------------------------------
#pragma once

#include "core/Expected.h"
#include "platform/Win.h"

#include <string>

namespace hh::ame {

struct PanelStatus {
    bool installed = false;
    std::wstring installedVersion;   ///< ExtensionBundleVersion of the installed manifest
    std::wstring bundledVersion;     ///< version shipped next to the exe
    std::wstring installPath;
    bool debugModeOk = false;        ///< PlayerDebugMode present for CSXS.12
    [[nodiscard]] bool updateAvailable() const { return installed && !bundledVersion.empty() && installedVersion != bundledVersion; }
};

/// Bundle id / folder name of the panel.
const wchar_t* panelBundleId() noexcept;
/// Where the panel is bundled next to the exe: <exe>\cep\com.everett.hdrhint
std::wstring bundledPanelDirectory();
/// %APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint
std::wstring installedPanelDirectory();

PanelStatus queryPanelStatus();

/**
 * @brief Copies the bundled panel over the installed one, writes config.json,
 *        sets PlayerDebugMode="1" for CSXS.9..14 (never lowering an existing
 *        value) and HKCU\Software\HdrHint\ExePath.
 */
Result<void> installPanel();
Result<void> uninstallPanel();

/// Reads ExtensionBundleVersion="..." from a manifest file (empty when unreadable).
std::wstring readManifestVersion(const std::wstring& manifestPath);

} // namespace hh::ame
