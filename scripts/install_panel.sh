#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# install_panel.sh - installs (or removes) the HDR Hint CEP panel on macOS.
#
# The same steps as the app's own "Install Media Encoder panel" button and
# scripts/install_panel.ps1 on Windows:
#
#   1. copies cep/com.everett.hdrhint to
#      ~/Library/Application Support/Adobe/CEP/extensions/com.everett.hdrhint
#   2. writes config.json into it (app path, installed version, socket path)
#   3. sets PlayerDebugMode = 1 for CSXS.9 .. CSXS.14 (unsigned extensions
#      only load with it) - `defaults write com.adobe.CSXS.N PlayerDebugMode 1`
#
# Idempotent: run it again after a rebuild to update in place. Nothing
# outside your home folder is touched and nothing is downloaded.
#
#   scripts/install_panel.sh                         panel for build/HdrHint.app
#   scripts/install_panel.sh --app /Applications/HdrHint.app
#   scripts/install_panel.sh --uninstall
# ---------------------------------------------------------------------------
set -euo pipefail

EXT_ID="com.everett.hdrhint"
CSXS_MAJORS=(9 10 11 12 13 14)

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$root/cep/$EXT_ID"
app="$root/build/HdrHint.app"
dest="$HOME/Library/Application Support/Adobe/CEP/extensions/$EXT_ID"
socket="$HOME/Library/Application Support/HdrHint/HdrHint.sock"
uninstall=0

usage() {
    sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) app="${2:?--app needs a path}"; shift ;;
        --source) source_dir="${2:?--source needs a path}"; shift ;;
        --dest) dest="${2:?--dest needs a path}"; shift ;;
        --uninstall) uninstall=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

step() { printf '[install] %s\n' "$*"; }

# ---- uninstall ------------------------------------------------------------------------
if [[ $uninstall -eq 1 ]]; then
    if [[ -d "$dest" ]]; then
        rm -rf "$dest"
        step "Removed $dest"
    else
        step "Not installed: $dest"
    fi
    step "PlayerDebugMode was left on (other unsigned panels may rely on it)."
    exit 0
fi

# ---- checks ---------------------------------------------------------------------------
[[ -f "$source_dir/CSXS/manifest.xml" ]] || { echo "panel source not found: $source_dir" >&2; exit 1; }
if [[ ! -d "$app" ]]; then
    echo "warning: $app does not exist yet; config.json points at it anyway." >&2
fi
app="$(cd "$(dirname "$app")" 2>/dev/null && pwd)/$(basename "$app")"

# JSON string escaping for paths (backslash and quote are all a path can bring).
json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '%s' "$s"
}

version="$(sed -n 's/.*ExtensionBundleVersion="\([^"]*\)".*/\1/p' "$source_dir/CSXS/manifest.xml" | head -n 1)"

# ---- 1. copy ------------------------------------------------------------------------------
mkdir -p "$(dirname "$dest")"
rm -rf "$dest"
cp -R "$source_dir" "$dest"
step "Copied the panel to $dest"

# ---- 2. config.json ---------------------------------------------------------------------------
printf '{"exePath": "%s", "installedVersion": "%s", "pipeName": "%s"}\n' \
    "$(json_escape "$app")" "$(json_escape "$version")" "$(json_escape "$socket")" > "$dest/config.json"
step "config.json: $app (panel $version)"

# ---- 3. PlayerDebugMode ------------------------------------------------------------------------
for major in "${CSXS_MAJORS[@]}"; do
    defaults write "com.adobe.CSXS.$major" PlayerDebugMode 1
done
step "PlayerDebugMode = 1 for CSXS.${CSXS_MAJORS[0]}..${CSXS_MAJORS[${#CSXS_MAJORS[@]}-1]}"

echo
echo "Restart Adobe Media Encoder, then open Window > Extensions > HDR Hint."
