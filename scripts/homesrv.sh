#!/bin/sh
# HomeSrv Kobo client launcher. Spawned by NickelMenu (cmd_spawn).
# Kills the watchdog, pauses Nickel, runs homesrv, resumes Nickel.

BASE="/mnt/onboard/.adds/homesrv"
LOG="$BASE/homesrv.log"

if [ ! -f "$BASE/config" ]; then
	mkdir -p "$BASE"
	cat > "$BASE/config" << 'EOF'
# HomeSrv Kobo client config
SERVER_URL=http://192.168.1.1:8080
API_TOKEN=your_token_here
EOF
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] first run: config template created at $BASE/config" > "$LOG"
fi

# Kill the watchdog so it doesn't force-reboot while nickel is paused
killall -TERM sickel 2>/dev/null

# Pause Nickel (don't kill — preserves Wi-Fi, framebuffer state)
killall -STOP nickel 2>/dev/null

sleep 1

{
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] launching homesrv"
	"$BASE/bin/homesrv" "$BASE"
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] exited with status $?"
} >> "$LOG" 2>&1

# Resume Nickel — it will repaint the screen
killall -CONT nickel 2>/dev/null
