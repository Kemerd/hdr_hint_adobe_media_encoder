#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# install_startup.sh - lets Adobe Media Encoder start HDR Hint on macOS.
#
# Media Encoder evaluates every .jsx in its Scripts/Startup folder while it
# launches. Dropping cep/startup/HdrHintLauncher.jsx there makes AME open
# HdrHint.app itself; the app then sits in the menu bar and leaves with AME.
# The app's path is stamped next to the script (HdrHintExePath.txt) as a
# fallback for when ~/Library/Application Support/HdrHint/launcher.json is
# missing.
#
# The Startup folder lives inside Media Encoder's install folder, which is
# usually owned by root: run with sudo when the copy is refused.
#
#   scripts/install_startup.sh
#   scripts/install_startup.sh --app /Applications/HdrHint.app
#   scripts/install_startup.sh --ame-root "/Applications/Adobe Media Encoder 2026"
#   scripts/install_startup.sh --uninstall
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_NAME="HdrHintLauncher.jsx"
STAMP_NAME="HdrHintExePath.txt"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app="$root/build/HdrHint.app"
ame_root=""
uninstall=0

usage() {
    sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) app="${2:?--app needs a path}"; shift ;;
        --ame-root) ame_root="${2:?--ame-root needs a path}"; shift ;;
        --uninstall) uninstall=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---- Media Encoder installs with a Startup folder, newest first -----------------------
startup_folders=()
if [[ -n "$ame_root" ]]; then
    [[ -d "$ame_root" ]] || { echo "Media Encoder folder not found: $ame_root" >&2; exit 1; }
    startup_folders+=("$ame_root/Scripts/Startup")
else
    # "2026" sorts after "2025": reverse order puts the newest first.
    while IFS= read -r folder; do
        [[ -d "$folder/Scripts/Startup" ]] && startup_folders+=("$folder/Scripts/Startup")
    done < <(find /Applications -maxdepth 1 -type d -name 'Adobe Media Encoder*' 2>/dev/null | sort -r)
fi
if [[ ${#startup_folders[@]} -eq 0 ]]; then
    echo "No Adobe Media Encoder Scripts/Startup folder found. Pass --ame-root explicitly." >&2
    exit 1
fi

# ---- the app to launch (install only) ------------------------------------------------------
source_script="$root/cep/startup/$SCRIPT_NAME"
if [[ $uninstall -eq 0 ]]; then
    [[ -d "$app" ]] || { echo "HdrHint.app not found at '$app'. Build it first or pass --app." >&2; exit 1; }
    app="$(cd "$app" && pwd)"
    [[ -f "$source_script" ]] || { echo "startup script missing: $source_script" >&2; exit 1; }
fi

failed=0
for startup in "${startup_folders[@]}"; do
    target="$startup/$SCRIPT_NAME"
    stamp="$startup/$STAMP_NAME"
    if [[ $uninstall -eq 1 ]]; then
        for path in "$target" "$stamp"; do
            if [[ -e "$path" ]]; then
                if rm -f "$path"; then echo "Removed $path"; else failed=1; fi
            else
                echo "Not present: $path"
            fi
        done
        continue
    fi
    if cp "$source_script" "$target" && printf '%s' "$app" > "$stamp"; then
        echo "Installed $target"
        echo "  launches $app"
    else
        failed=1
        echo "warning: could not write to $startup - run again with sudo." >&2
    fi
done
[[ $failed -eq 0 ]] || exit 1

echo
if [[ $uninstall -eq 1 ]]; then
    echo "Media Encoder will no longer start HDR Hint."
else
    echo "Restart Media Encoder. HDR Hint starts with it and waits in the menu bar."
fi
