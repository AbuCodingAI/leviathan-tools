#!/bin/bash

# Churro Desktop Environment Session Starter
# This replaces GNOME/X.Org with Churro widgets

export DESKTOP_SESSION=churro
export XDG_CURRENT_DESKTOP=Churro

CHURRO_BIN="/media/abu/CoderDrive/LeviathanOS/bin"

# Start D-Bus session (optional)
if command -v dbus-launch &> /dev/null; then
    eval $(dbus-launch --sh-syntax) 2>/dev/null || true
fi

# Start window manager (try multiple options)
WINDOW_MANAGER_PID=""
for wm in mutter openbox fluxbox i3 xfwm4 metacity; do
    if command -v $wm &> /dev/null; then
        echo "[Churro] Starting $wm window manager..."
        $wm &
        WINDOW_MANAGER_PID=$!
        break
    fi
done

if [ -z "$WINDOW_MANAGER_PID" ]; then
    echo "[Churro] WARNING: No window manager found, continuing without one..."
fi

sleep 1

# Start the compositor (kills screen tearing, adds vsync). Light config for old
# hardware; fullscreen video/games bypass it. Non-fatal if picom is missing.
if command -v picom &>/dev/null; then
    echo "[Churro] Starting picom compositor…"
    picom --config /etc/xdg/picom.conf &>/dev/null &
fi

# Start the notification daemon so apps can actually show notifications
# (Guardian scan results, "printing ready", screenshots, etc.).
if command -v dunst &>/dev/null; then
    echo "[Churro] Starting dunst notification daemon…"
    dunst &>/dev/null &
fi

# Night light / blue-light filter (nice on old Mac screens at night).
if command -v redshift-gtk &>/dev/null; then
    redshift-gtk &>/dev/null &
elif command -v redshift &>/dev/null; then
    redshift &>/dev/null &
fi

# Start Churro widgets
echo "[Churro] Starting Churro desktop environment..."

echo "[Churro] Starting time widget..."
"$CHURRO_BIN/churro-time-widget" &
TIME_WIDGET_PID=$!

sleep 0.5

echo "[Churro] Starting dock..."
"$CHURRO_BIN/churro-dock" &
DOCK_PID=$!

sleep 0.5

echo "[Churro] Starting system tray..."
"$CHURRO_BIN/churro-tray" &
TRAY_PID=$!

sleep 1

echo "[Churro] Starting keyboard shortcuts daemon…"
"$CHURRO_BIN/churro-keybind" &
KEYBIND_PID=$!

sleep 0.5

echo "[Churro] Desktop environment started!"
echo "[Churro] PIDs: WM=$WINDOW_MANAGER_PID TIME=$TIME_WIDGET_PID DOCK=$DOCK_PID TRAY=$TRAY_PID KEYBIND=$KEYBIND_PID"

# Wait for any process to die
wait
