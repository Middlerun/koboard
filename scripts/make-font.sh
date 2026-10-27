#!/bin/sh
# Regenerate the bundled fonts.
#
# Koboard draws text in DejaVu Sans, plus a set of weather symbols it offers
# servers on top of DejaVu's coverage — most of which DejaVu has no glyph for
# at all (fog, drizzle, droplet). FBInk's fbink_add_ot_font() replaces the font
# for a given style rather than adding a fallback, so the symbols are copied
# from Noto Emoji into the DejaVu files themselves. This script reproduces that:
# it downloads both sources, subsets Noto Emoji to the symbols wanted, and
# merges the result into the regular and bold DejaVu faces. Which symbols is
# EMOJI_CODEPOINTS in merge-emoji.py; the README lists them for server authors.
#
# Requires python3 + fonttools (which provides pyftsubset), curl and unzip.
set -eu

FONTS_DIR="$(cd "$(dirname "$0")/.." && pwd)/fonts"
HERE="$(cd "$(dirname "$0")" && pwd)"

DEJAVU_VER="version_2_37"
DEJAVU_ZIP="dejavu-fonts-ttf-2.37.zip"
DEJAVU_ZIP_URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/${DEJAVU_VER}/${DEJAVU_ZIP}"
NOTO_URL="https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"

# The blocks we pull out of Noto Emoji; merge-emoji.py picks the symbols to
# merge in from these.
EMOJI_UNICODES="U+1F4A7,U+1F300-1F32C,U+2600-26FF,U+2700-27BF"

command -v python3 >/dev/null || { echo "error: python3 required" >&2; exit 1; }
python3 -c 'import fontTools' 2>/dev/null || {
	echo "error: fonttools required (pip install fonttools)" >&2; exit 1; }
command -v pyftsubset >/dev/null || {
	echo "error: pyftsubset required (comes with fonttools)" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$FONTS_DIR"

echo "Downloading DejaVu ${DEJAVU_VER}..."
curl -sL "$DEJAVU_ZIP_URL" -o "$WORK/$DEJAVU_ZIP"
unzip -o -j "$WORK/$DEJAVU_ZIP" \
	"dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" \
	"dejavu-fonts-ttf-2.37/ttf/DejaVuSans-Bold.ttf" -d "$WORK/" >/dev/null

echo "Downloading Noto Emoji..."
curl -sL "$NOTO_URL" -o "$WORK/NotoEmoji.ttf"

echo "Subsetting Noto Emoji..."
pyftsubset "$WORK/NotoEmoji.ttf" \
	--unicodes="$EMOJI_UNICODES" \
	--output-file="$WORK/NotoEmoji-weather.ttf"

echo "Merging weather glyphs into DejaVu Sans..."
python3 "$HERE/merge-emoji.py" \
	"$WORK/DejaVuSans.ttf" "$WORK/NotoEmoji-weather.ttf" \
	"$FONTS_DIR/DejaVuSans.ttf"
python3 "$HERE/merge-emoji.py" \
	"$WORK/DejaVuSans-Bold.ttf" "$WORK/NotoEmoji-weather.ttf" \
	"$FONTS_DIR/DejaVuSans-Bold.ttf"

echo "Fonts written to $FONTS_DIR/"
