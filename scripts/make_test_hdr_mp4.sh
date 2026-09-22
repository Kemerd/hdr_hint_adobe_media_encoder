#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# make_test_hdr_mp4.sh - small HDR / SDR HEVC test clips for HDR Hint
# (make_test_hdr_mp4.ps1's twin for macOS and other POSIX shells).
#
# Writes into OUT_DIR (default ${TMPDIR:-/tmp}/hdrhint_tests):
#
#   test_pq_noSEI.mp4      PQ, BT.2020, 10-bit, no mastering-display / CLL SEI
#                          (AME with "Include HDR10 Metadata" off)
#   test_pq_SEI.mp4        the same plus mastering display 1000 / 0.0001 cd/m2
#                          and MaxCLL 1000 / MaxFALL 400 (the box on)
#   test_hlg.mp4           HLG, BT.2020, 10-bit
#   test_sdr.mp4           BT.709, 8-bit
#   test_ünïcode 日本.mp4   byte copy of test_pq_noSEI.mp4 with a non-ASCII name
#
# Every clip is 1920x1080, 30 fps, testsrc2 + a 440 Hz tone (AAC 128 kbps),
# libx265 ultrafast, hvc1 tag, faststart - shaped like an AME export - and is
# checked with ffprobe afterwards (colour tags, SEI presence).
#
#   scripts/make_test_hdr_mp4.sh [--out DIR] [--seconds N] [--ffmpeg PATH]
#
# ffmpeg with libx265: brew install ffmpeg
# Exit codes: 0 all good, 1 a clip failed, 2 ffmpeg / ffprobe missing.
# ---------------------------------------------------------------------------
set -uo pipefail
export LC_ALL="${LC_ALL:-en_US.UTF-8}"

out_dir="${TMPDIR:-/tmp}"
out_dir="${out_dir%/}/hdrhint_tests"
seconds=3
ffmpeg="${FFMPEG:-}"

usage() {
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) out_dir="${2:?--out needs a folder}"; shift ;;
        --seconds) seconds="${2:?--seconds needs a number}"; shift ;;
        --ffmpeg) ffmpeg="${2:?--ffmpeg needs a path}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
if ! [[ "$seconds" =~ ^[0-9]+$ ]] || (( seconds < 1 || seconds > 60 )); then
    echo "--seconds must be 1..60" >&2
    exit 2
fi

# ---- tools (explicit, then Homebrew, then PATH) ---------------------------------------
find_tool() {
    local name="$1" explicit="$2"
    if [[ -n "$explicit" && -x "$explicit" ]]; then echo "$explicit"; return 0; fi
    for candidate in "/opt/homebrew/bin/$name" "/usr/local/bin/$name"; do
        [[ -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    command -v "$name" 2>/dev/null
}
ffmpeg="$(find_tool ffmpeg "$ffmpeg")" || true
[[ -n "$ffmpeg" ]] || { echo "ffmpeg not found. brew install ffmpeg, or pass --ffmpeg." >&2; exit 2; }
ffprobe="$(find_tool ffprobe "$(dirname "$ffmpeg")/ffprobe")" || true
[[ -n "$ffprobe" ]] || { echo "ffprobe not found next to $ffmpeg or on PATH." >&2; exit 2; }

echo "ffmpeg : $ffmpeg"
echo "ffprobe: $ffprobe"
echo "output : $out_dir"
mkdir -p "$out_dir"

# ---- recipes ----------------------------------------------------------------------------------
# x265 gets the VUI through -x265-params (what lands in the bitstream); the
# -color_* flags tag the stream side so both agree, like an AME export.
pq_vui='colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited'
hlg_vui='colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:range=limited'
sdr_vui='colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited'
hdr10_sei=':master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400'

pq_color=(-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv)
hlg_color=(-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc -color_range tv)
sdr_color=(-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv)

# encode <file> <x265 params> <pix fmt> <colour args...>
encode() {
    local file="$1" x265="$2" pix="$3"
    shift 3
    "$ffmpeg" -y -hide_banner -nostdin -loglevel error \
        -f lavfi -i "testsrc2=size=1920x1080:rate=30" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000" \
        -t "$seconds" -map 0:v:0 -map 1:a:0 \
        -c:v libx265 -preset ultrafast -pix_fmt "$pix" -tag:v hvc1 -x265-params "$x265" \
        "$@" \
        -c:a aac -b:a 128k -ar 48000 -ac 2 -movflags +faststart \
        "$out_dir/$file"
}

# verify <file> <pix fmt> <transfer> <expect SEI yes|no>
verify() {
    local file="$1" pix="$2" transfer="$3" sei="$4"
    local probe
    probe="$("$ffprobe" -v error -select_streams v:0 \
        -show_entries stream=pix_fmt,color_transfer -of default=noprint_wrappers=1 "$out_dir/$file")" || return 1
    grep -q "pix_fmt=$pix" <<<"$probe" || { echo "  pix_fmt mismatch: $probe"; return 1; }
    grep -q "color_transfer=$transfer" <<<"$probe" || { echo "  transfer mismatch: $probe"; return 1; }
    local side
    side="$("$ffprobe" -v error -select_streams v:0 -read_intervals '%+#1' \
        -show_entries frame_side_data=side_data_type -of default=noprint_wrappers=1 "$out_dir/$file")"
    if [[ "$sei" == "yes" ]]; then
        grep -qi "mastering display" <<<"$side" || { echo "  mastering-display SEI missing"; return 1; }
    else
        grep -qi "mastering display" <<<"$side" && { echo "  unexpected mastering-display SEI"; return 1; }
    fi
    return 0
}

status=0
run() {
    local file="$1" x265="$2" pix="$3" transfer="$4" sei="$5"
    shift 5
    printf 'Encoding %-20s ' "$file"
    if encode "$file" "$x265" "$pix" "$@" && verify "$file" "$pix" "$transfer" "$sei"; then
        echo "OK"
    else
        echo "FAILED"
        status=1
    fi
}

run test_pq_noSEI.mp4 "$pq_vui" yuv420p10le smpte2084 no "${pq_color[@]}"
run test_pq_SEI.mp4 "$pq_vui$hdr10_sei" yuv420p10le smpte2084 yes "${pq_color[@]}"
run test_hlg.mp4 "$hlg_vui" yuv420p10le arib-std-b67 no "${hlg_color[@]}"
run test_sdr.mp4 "$sdr_vui" yuv420p bt709 no "${sdr_color[@]}"

# The non-ASCII copy (APFS stores names NFD-insensitive; the app normalises to NFC).
unicode_name="test_ünïcode 日本.mp4"
if [[ -f "$out_dir/test_pq_noSEI.mp4" ]]; then
    cp "$out_dir/test_pq_noSEI.mp4" "$out_dir/$unicode_name"
    if cmp -s "$out_dir/test_pq_noSEI.mp4" "$out_dir/$unicode_name"; then
        printf 'Copied   %-20s OK\n' "$unicode_name"
    else
        printf 'Copied   %-20s FAILED\n' "$unicode_name"
        status=1
    fi
fi

echo
[[ $status -eq 0 ]] && echo "All clips generated and verified in $out_dir" || echo "Some clips failed (see above)."
exit $status
