#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# install_standalone.sh - installs HDR Hint on macOS as a watch-folder tool,
# no Adobe software required (install_standalone.ps1's twin).
#
# Copies the built HdrHint.app into ~/Applications (or /Applications with
# --system), clears the download quarantine so Gatekeeper does not block the
# ad-hoc signed build, and optionally:
#
#   --watch DIR     watch DIR for new videos (repeat for several folders);
#                   written into settings.ini so the app is ready at once
#   --login         open at login, quietly in the menu bar (the app registers
#                   itself as a login item the next time it starts)
#   --launch        start it when the install is done
#   --uninstall     quit it and delete the installed app (settings and logs in
#                   ~/Library/Application Support/HdrHint and ~/Library/Logs/HdrHint stay)
#
#   scripts/install_standalone.sh --watch ~/Movies/Renders --login --launch
#   scripts/install_standalone.sh --uninstall
# ---------------------------------------------------------------------------
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_app="$root/build/HdrHint.app"
dest_dir="$HOME/Applications"
watch=()
login=0
launch=0
uninstall=0

settings_dir="$HOME/Library/Application Support/HdrHint"
settings_ini="$settings_dir/settings.ini"

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source) source_app="${2:?--source needs a path}"; shift ;;
        --system) dest_dir="/Applications" ;;
        --dest) dest_dir="${2:?--dest needs a folder}"; shift ;;
        --watch) watch+=("${2:?--watch needs a folder}"); shift ;;
        --login) login=1 ;;
        --launch) launch=1 ;;
        --uninstall) uninstall=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

dest_app="$dest_dir/HdrHint.app"

# Quits a running copy politely (menu bar apps have no window to close).
quit_running() {
    if pgrep -x HdrHint >/dev/null 2>&1; then
        echo "Quitting the running copy..."
        osascript -e 'tell application id "com.everett.hdrhint" to quit' >/dev/null 2>&1 || pkill -x HdrHint || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            pgrep -x HdrHint >/dev/null 2>&1 || break
            sleep 0.3
        done
    fi
}

# Sets key = value in [section] of the ini file (creating either as needed).
ini_set() {
    local section="$1" key="$2" value="$3" file="$4"
    mkdir -p "$(dirname "$file")"
    [[ -f "$file" ]] || : > "$file"
    local tmp
    tmp="$(mktemp)"
    awk -v section="[$section]" -v key="$key" -v value="$value" '
        function emit() { print key " = " value; done = 1 }
        {
            line = $0
            trimmed = line; gsub(/^[ \t]+|[ \t]+$/, "", trimmed)
            if (substr(trimmed, 1, 1) == "[") {
                if (in_section && !done) emit()
                in_section = (trimmed == section)
                print line
                next
            }
            if (in_section) {
                split(trimmed, kv, "=")
                k = kv[1]; gsub(/[ \t]+$/, "", k)
                if (k == key) { if (!done) emit(); next }
            }
            print line
        }
        END {
            if (!done) {
                if (!in_section) print section
                emit()
            }
        }' "$file" > "$tmp"
    mv "$tmp" "$file"
}

# ---- uninstall -----------------------------------------------------------------------------
if [[ $uninstall -eq 1 ]]; then
    quit_running
    if [[ -d "$dest_app" ]]; then
        rm -rf "$dest_app"
        echo "Removed $dest_app"
    else
        echo "Not installed: $dest_app"
    fi
    echo
    echo "Settings and logs were kept. Delete these by hand to remove every trace:"
    echo "  $settings_dir"
    echo "  $HOME/Library/Logs/HdrHint"
    exit 0
fi

# ---- copy ------------------------------------------------------------------------------------
[[ -d "$source_app" ]] || { echo "HdrHint.app not found at '$source_app'. Build it first (make) or pass --source." >&2; exit 1; }
quit_running
mkdir -p "$dest_dir"
rm -rf "$dest_app"
# ditto keeps the bundle's signature, extended attributes and symlinks intact.
ditto "$source_app" "$dest_app"
xattr -dr com.apple.quarantine "$dest_app" 2>/dev/null || true
echo "Installed $dest_app"

# ---- settings (only while the app is not running: it rewrites the file on exit) ------
if [[ ${#watch[@]} -gt 0 ]]; then
    folders=()
    for folder in "${watch[@]}"; do
        if [[ -d "$folder" ]]; then
            folders+=("$(cd "$folder" && pwd)")
        else
            echo "warning: watch folder does not exist, skipped: $folder" >&2
        fi
    done
    if [[ ${#folders[@]} -gt 0 ]]; then
        joined="$(IFS='|'; echo "${folders[*]}")"
        # The app stores lists pipe-separated under [ame].
        ini_set ame extra_watch_folders "$joined" "$settings_ini"
        echo "Watching: $joined"
    fi
fi
if [[ $login -eq 1 ]]; then
    ini_set app start_with_windows true "$settings_ini"
    echo "HDR Hint will open at login, in the menu bar."
fi

echo
echo "Done. HDR Hint runs on its own: it watches the folders in Settings and"
echo "processes new videos with mkvmerge. Media Encoder is not required."
if [[ $launch -eq 1 ]]; then
    open "$dest_app"
    echo "Started."
fi
