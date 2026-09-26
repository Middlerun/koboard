#!/bin/sh
# Golden layout tests.
#
# Renders every fixture at every device size and compares the computed rect of
# every node against a checked-in dump. A layout regression shows up here, on
# the desktop, instead of on the Kobo.
#
#   scripts/run-tests.sh build/homesrv-preview check    # compare (exit 1 on diff)
#   scripts/run-tests.sh build/homesrv-preview update   # rewrite the goldens
set -eu

BIN="$1"
MODE="${2:-check}"
DIR="$(dirname "$0")/.."
cd "$DIR"

# A fixed clock and timezone: the dashboard prints the time, so without these
# the dumps would differ on every run.
export TZ=UTC
NOW=1758880860

DEVICES="glo glohd"
FIXTURES="typical empty overflow finished"
GOLDEN=tests/golden
mkdir -p "$GOLDEN"

fail=0
for fixture in $FIXTURES; do
	for dev in $DEVICES; do
		name="$fixture-$dev"
		out="build/layout-$name.json"
		"$BIN" --json "fixtures/$fixture.json" --device "$dev" --now "$NOW" \
			--font fonts/DejaVuSans.ttf \
			--font-bold fonts/DejaVuSans-Bold.ttf \
			--out "build/test-$name.png" \
			--dump-rects "$out" 2>/dev/null

		if [ "$MODE" = update ]; then
			cp "$out" "$GOLDEN/$name.json"
			echo "updated $GOLDEN/$name.json"
		elif [ ! -f "$GOLDEN/$name.json" ]; then
			echo "MISSING golden for $name (run: make golden)"
			fail=1
		elif diff -u "$GOLDEN/$name.json" "$out" > "build/diff-$name.txt" 2>&1; then
			echo "ok      $name"
		else
			echo "FAIL    $name  (see build/diff-$name.txt)"
			head -30 "build/diff-$name.txt"
			fail=1
		fi
	done
done

[ "$MODE" = update ] && exit 0
if [ "$fail" -ne 0 ]; then
	echo
	echo "Layout changed. Review the diffs and the PNGs in build/, then run"
	echo "'make golden' to accept the new layout."
	exit 1
fi
echo "all layout tests passed"
