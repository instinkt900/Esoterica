#!/usr/bin/env bash
#
# Kill every running Esoterica application. Mirrors KillEsotericaProcesses.bat.
#
# Worth running after a crash. The Resource Server's `ResourceCompiler -worker` children inherit
# its listening socket and can outlive it, so a stale worker holds 5556 with no server anywhere in
# `ps`, and the next launch looks like a networking failure.
#
# Two deliberate choices:
#
#   - It matches on the executable name, the way the .bat's `taskkill /FI "IMAGENAME eq Esoterica*"`
#     does, rather than on the command line. That difference matters: `pkill -f Esoterica.Applications`
#     also matches any shell whose own command line contains that string, so a script that calls it
#     kills itself. Reading /proc/<pid>/exe cannot make that mistake.
#   - It sends SIGKILL, matching the .bat's `/F`. SIGTERM is not enough: an application showing a
#     modal error dialog ignores it and stays up.
#
# It also reports anything still holding port 5556 afterwards, because that is not always an
# Esoterica process. An error dialog inherits the listening socket from the application that
# spawned it, so a `zenity` left over from a failed startup can hold the port on its own.

set -uo pipefail

killed=0
for procDir in /proc/[0-9]*; do
    pid="${procDir#/proc/}"
    exe="$( readlink "/proc/${pid}/exe" 2>/dev/null )" || continue
    case "$( basename "${exe}" )" in
        Esoterica.Applications.*)
            if kill -KILL "${pid}" 2>/dev/null; then
                echo "killed ${pid} $( basename "${exe}" )"
                killed=$(( killed + 1 ))
            fi
            ;;
    esac
done

if [[ ${killed} -eq 0 ]]; then
    echo "no Esoterica processes running"
fi

# Wait for the port to come back, then say who still has it if anyone does.
for _ in $( seq 20 ); do
    ss -ltn 2>/dev/null | grep -q ':5556' || exit 0
    sleep 0.25
done

echo "warning: port 5556 is still held. The next launch will fail to bind it." >&2
ss -ltnp 2>/dev/null | grep ':5556' >&2
