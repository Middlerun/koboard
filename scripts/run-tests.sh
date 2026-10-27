#!/bin/sh
# Golden layout tests.
#
# Renders every fixture — a UI tree, exactly as a server would send it — at
# every device size, and compares the computed rect of every node against a
# checked-in dump. A layout regression shows up here, on the desktop, instead
# of on the Kobo. No server is involved: what a particular server *sends* is
# that server's to test.
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

# Compare one dump against its golden (or update it).
check_dump() {
	name="$1"
	out="build/layout-$name.json"
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
}

# The menu is drawn by the client, from a config file, so it is checked the
# same way the dashboard is — it is the one screen that has to work when the
# server cannot be reached.
for dev in $DEVICES; do
	name="menu-$dev"
	"$BIN" --screen menu --config fixtures/servers.config --device "$dev" \
		--font fonts/DejaVuSans.ttf \
		--font-bold fonts/DejaVuSans-Bold.ttf \
		--out "build/test-$name.png" \
		--dump-rects "build/layout-$name.json" 2>/dev/null
	check_dump "$name"
done

for fixture in $FIXTURES; do
	for dev in $DEVICES; do
		name="$fixture-$dev"
		out="build/layout-$name.json"
		"$BIN" --ui "fixtures/$fixture.json" --device "$dev" --now "$NOW" \
			--font fonts/DejaVuSans.ttf \
			--font-bold fonts/DejaVuSans-Bold.ttf \
			--out "build/test-$name.png" \
			--dump-rects "$out" 2>/dev/null

		check_dump "$name"
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
