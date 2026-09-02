#!/usr/bin/env bash
#
# Run a graphical Esoterica application on a throwaway X server.
#
# The editor and the engine both need an X display with a window manager, and both are awkward
# to run on the desktop session: a screen lock replaces every screenshot with the lock screen,
# DPMS throttles the frame loop, and the window fights for focus with whatever else is running.
# This script gives them their own Xvfb display with a minimal i3 on it instead.
#
# NVIDIA's Vulkan driver presents to Xvfb, so the RTX 3090 is still the device that renders.
# Verified with vkcube, which reports "Selected GPU 0: NVIDIA GeForce RTX 3090".
#
# Usage:
#   VirtualDisplay.sh start [-d :99] [-s 1920x1080]   start Xvfb + i3, print the DISPLAY
#   VirtualDisplay.sh stop [-d :99]                   kill both and remove the state directory
#   VirtualDisplay.sh shot [-d :99] <file.png>        capture the whole screen
#   VirtualDisplay.sh status [-d :99]                 report whether the display is up
#   VirtualDisplay.sh run [-d :99] [-s WxH] -- <cmd>  start, run the command, stop
#
# Typical session:
#   eval "$( Docs/Linux/Scripts/VirtualDisplay.sh start )"    # exports DISPLAY
#   Build/Linux_Release/Esoterica.Applications.Editor &
#   sleep 20
#   Docs/Linux/Scripts/VirtualDisplay.sh shot /tmp/editor.png
#   Docs/Linux/Scripts/VirtualDisplay.sh stop
#
# Needs: Xvfb, i3, ffmpeg. All three are packages, not build dependencies.

set -euo pipefail

DISPLAY_NUM=":99"
SCREEN_SIZE="1920x1080"

STATE_ROOT="${TMPDIR:-/tmp}/esoterica-vdisplay"

#-------------------------------------------------------------------------

die() { echo "error: $*" >&2; exit 1; }

state_dir() { echo "${STATE_ROOT}/${DISPLAY_NUM#:}"; }

parse_opts()
{
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d) DISPLAY_NUM="$2"; shift 2 ;;
            -s) SCREEN_SIZE="$2"; shift 2 ;;
            --) shift; break ;;
            *)  break ;;
        esac
    done
    REST=( "$@" )
}

require_tools()
{
    for tool in "$@"; do
        command -v "$tool" >/dev/null || die "$tool is not installed"
    done
}

#-------------------------------------------------------------------------

do_start()
{
    require_tools Xvfb i3

    local dir; dir="$( state_dir )"
    if [[ -e "/tmp/.X11-unix/X${DISPLAY_NUM#:}" ]]; then
        die "display ${DISPLAY_NUM} is already in use. Pick another with -d, or run stop first."
    fi

    mkdir -p "${dir}"
    echo "${SCREEN_SIZE}" > "${dir}/size"

    # Our own i3 config, not the user's. The desktop config here starts xss-lock, and a lock
    # screen inside the virtual display would replace every screenshot with i3lock's image.
    # This one has no bar, no lock, and no autostart.
    cat > "${dir}/i3.conf" <<'EOF'
# Minimal i3 for a throwaway display. Tiling defaults are deliberate: the editor has only ever
# been shaken down under i3, so this matches the environment it is known to work in.
font pango:monospace 8
focus_follows_mouse no
EOF

    # Deliberately no `assign` rule to move the Resource Server off this workspace, and no
    # stacking or tabbed layout. Both leave its window unmapped, and an unmapped Resource Server
    # stops servicing requests: it still binds and listens on 5556, but every request times out
    # with "Request acknowledgement timed out ... max retries exceeded" and the application dies
    # with "Failed to load required engine module resources". The server has to stay visible.
    #
    # So the editor gets tiled beside it and only half the width. Ask for a wider screen rather
    # than trying to hide the server - -s 5120x1440 leaves the editor a usable 2560.

    Xvfb "${DISPLAY_NUM}" -screen 0 "${SCREEN_SIZE}x24" -nolisten tcp > "${dir}/xvfb.log" 2>&1 &
    echo $! > "${dir}/xvfb.pid"

    # Wait for the server to accept connections rather than guessing at a sleep.
    local waited=0
    until DISPLAY="${DISPLAY_NUM}" xdpyinfo >/dev/null 2>&1; do
        sleep 0.2
        waited=$(( waited + 1 ))
        [[ ${waited} -lt 50 ]] || die "Xvfb did not come up. See ${dir}/xvfb.log"
    done

    DISPLAY="${DISPLAY_NUM}" i3 -c "${dir}/i3.conf" > "${dir}/i3.log" 2>&1 &
    echo $! > "${dir}/i3.pid"
    sleep 1

    echo "export DISPLAY=${DISPLAY_NUM}"
}

do_stop()
{
    local dir; dir="$( state_dir )"
    [[ -d "${dir}" ]] || { echo "display ${DISPLAY_NUM} is not running" >&2; return 0; }

    for name in i3 xvfb; do
        if [[ -f "${dir}/${name}.pid" ]]; then
            kill "$( cat "${dir}/${name}.pid" )" 2>/dev/null || true
        fi
    done
    sleep 0.5
    rm -rf "${dir}"
}

do_shot()
{
    require_tools ffmpeg

    local out="${1:-}"
    [[ -n "${out}" ]] || die "shot needs an output path"

    local dir; dir="$( state_dir )"
    [[ -f "${dir}/size" ]] || die "display ${DISPLAY_NUM} is not running"

    ffmpeg -loglevel error -y -f x11grab -draw_mouse 1 \
        -video_size "$( cat "${dir}/size" )" -i "${DISPLAY_NUM}" -frames:v 1 "${out}"
    echo "${out}"
}

do_status()
{
    local dir; dir="$( state_dir )"
    if [[ -d "${dir}" ]] && DISPLAY="${DISPLAY_NUM}" xdpyinfo >/dev/null 2>&1; then
        echo "${DISPLAY_NUM} up, $( cat "${dir}/size" )"
    else
        echo "${DISPLAY_NUM} down"
        return 1
    fi
}

do_run()
{
    [[ $# -gt 0 ]] || die "run needs a command after --"
    do_start >/dev/null
    trap do_stop EXIT
    DISPLAY="${DISPLAY_NUM}" "$@"
}

#-------------------------------------------------------------------------

[[ $# -gt 0 ]] || die "usage: $( basename "$0" ) {start|stop|shot|status|run} [options]"

COMMAND="$1"; shift
REST=()
parse_opts "$@"

case "${COMMAND}" in
    start)  do_start ;;
    stop)   do_stop ;;
    shot)   do_shot "${REST[@]}" ;;
    status) do_status ;;
    run)    do_run "${REST[@]}" ;;
    *)      die "unknown command: ${COMMAND}" ;;
esac
