#!/usr/bin/env bash
#
# Runs the Reflector's shader pass, which compiles every .esh to SPIR-V and generates the C++
# that embeds it.
#
#   ./CompileShaders.sh              compile what is missing or out of date
#   ./CompileShaders.sh --hotreload  the fast path, see below
#   ./CompileShaders.sh --rebuild    recompile everything
#   ./CompileShaders.sh --clean      delete the generated shader output
#
# Mirrors CompileShaders.bat. Like RunReflection.sh, this does not build the Reflector for you.
# Build it first:
#
#   python3 Code/Scripts/NinjaGen/NinjaGen.py
#   ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Reflector
#
# --hotreload is not the default, and the .bat passes it. This is the one place the two scripts
# differ, on purpose.
#
# -hotreload does not notify a running process, which is what the phase plan suspected. It is
# purely an in-process shortcut: it skips the type reflection pass that a plain -shaders run
# performs when the generated *.parameters.h files change. When they do change, the Reflector
# refuses the shortcut and exits 1 with "Shader Hot-Reload will result in changes to the engine
# runtime types requiring type reflection. Aborting Hot-Reload!".
#
# A first run always changes those files, so mirroring the .bat exactly would give a script that
# fails on a fresh checkout. The default is therefore the run that always works, and the fast
# path is one flag away. Use it when iterating on a shader body; drop it when you change a
# shader's parameters.
#
# Note also that the generated .cpp files are picked up by a glob, so NinjaGen.py has to be
# re-run after the first successful compile or the build will not see them.

set -euo pipefail

REPO_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SOLUTION_PATH="${REPO_ROOT}/Esoterica.slnx"

REFLECTOR_EXE="${REPO_ROOT}/Build/Linux_Release/Esoterica.Applications.Reflector"

#-------------------------------------------------------------------------

main()
{
    local mode="${1:-}"

    if [[ ! -x "${REFLECTOR_EXE}" ]]
    then
        echo "error: ${REFLECTOR_EXE#${REPO_ROOT}/} does not exist." >&2
        echo "       Build it first:" >&2
        echo "         ninja -f Build/Linux/Esoterica.ninja Build/Linux_Release/Esoterica.Applications.Reflector" >&2
        exit 1
    fi

    # -clean, -rebuild and -hotreload are mutually exclusive, and the Reflector rejects any two
    # of them together. -shaders scopes all of them to the shader output, so the Reflector cleans
    # the right directories itself and there is nothing for this script to delete.
    case "${mode}" in
        --clean|-clean|clean)
            "${REFLECTOR_EXE}" -s "${SOLUTION_PATH}" -shaders -clean
            ;;
        --rebuild|-rebuild|rebuild)
            "${REFLECTOR_EXE}" -s "${SOLUTION_PATH}" -shaders -rebuild
            ;;
        --hotreload|-hotreload|hotreload)
            "${REFLECTOR_EXE}" -s "${SOLUTION_PATH}" -shaders -hotreload
            ;;
        '')
            "${REFLECTOR_EXE}" -s "${SOLUTION_PATH}" -shaders
            ;;
        *)
            echo "error: unknown option \"${mode}\". Use --hotreload, --rebuild, --clean, or no argument." >&2
            exit 1
            ;;
    esac
}

main "$@"
