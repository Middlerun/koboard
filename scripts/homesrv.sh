#!/bin/sh
# Launcher for the HomeSrv Kobo client. Spawned by NickelMenu (cmd_spawn),
# which runs commands via /bin/sh -c with cwd=/.
# Keeps a log of the most recent run for debugging.

BASE="/mnt/onboard/.adds/homesrv"
LOG="$BASE/homesrv.log"

{
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] launching $BASE/bin/homesrv"
	"$BASE/bin/homesrv"
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] exited with status $?"
} > "$LOG" 2>&1
