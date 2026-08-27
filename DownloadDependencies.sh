#!/usr/bin/env bash
#
# Fetches and builds the Linux dependencies into External/.
#
#   ./DownloadDependencies.sh              everything this phase needs
#   ./DownloadDependencies.sh optick       one dependency at a time
#   ./DownloadDependencies.sh --list       what is available, and what is already built
#
# This is not a port of DownloadDependencies.bat. That script downloads one prebuilt
# External.zip of Windows binaries from an upstream release, and there is no Linux equivalent
# of it, so each dependency is fetched from source and built here.
#
# External/ is in .gitignore, which is what keeps this out of the repository.

set -euo pipefail

REPO_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
EXTERNAL_DIR="${REPO_ROOT}/External"
BUILD_JOBS="${BUILD_JOBS:-$( nproc )}"

#-------------------------------------------------------------------------

info()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
fail()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

#-------------------------------------------------------------------------
# System package check
#-------------------------------------------------------------------------
# Only what the requested dependencies actually need gets checked, and everything missing is
# reported in one message. Failing deep inside a nested build tells the reader far less than a
# single list of what to install.

MISSING_PACKAGES=()

require_command()
{
    command -v "$1" >/dev/null || MISSING_PACKAGES+=( "$2" )
}

require_pkg_config()
{
    pkg-config --exists "$1" || MISSING_PACKAGES+=( "$2" )
}

require_header()
{
    [[ -f "$1" ]] || MISSING_PACKAGES+=( "$2" )
}

# What each dependency needs, beyond git.
requirements_optick()      { :; }

requirements_ixwebsocket()
{
    require_command cmake cmake
    require_command ninja ninja-build
    require_pkg_config openssl libssl-dev
    require_pkg_config zlib zlib1g-dev
}

requirements_gamenetworkingsockets()
{
    require_command cmake cmake
    require_command ninja ninja-build
    require_command protoc protobuf-compiler
    require_pkg_config openssl libssl-dev
    require_pkg_config protobuf libprotobuf-dev
}

check_requirements()
{
    MISSING_PACKAGES=()
    require_command git git

    for name in "$@"
    do
        "requirements_${name}"
    done

    if [[ ${#MISSING_PACKAGES[@]} -gt 0 ]]
    then
        # Deduplicate, since several dependencies want the same packages.
        local unique
        unique=$( printf '%s\n' "${MISSING_PACKAGES[@]}" | sort -u | tr '\n' ' ' )
        fail "missing system packages for: $*

    sudo apt install ${unique}
"
    fi
}

#-------------------------------------------------------------------------
# Dependencies
#-------------------------------------------------------------------------

# Optick. Needed to *compile*, not merely to profile: Base/Profiling.h includes <optick.h>
# unconditionally, so the header has to exist even though this port does not enable profiling.
# Conventions rule 4 forbids stripping that include, so the dependency is real.
# Pinned to a commit, not a tag. Optick's only tags (up to v1.1.2) are the old Brofiler layout
# and contain no optick.h at all; the modern tree exists only on the default branch.
OPTICK_REPO="https://github.com/bombomby/optick.git"
OPTICK_COMMIT="8abd28dee1a4034c973a3d32cd1777118e72df7e"

fetch_optick()
{
    local target="${EXTERNAL_DIR}/Optick"

    if [[ -f "${target}/include/optick.h" ]]
    then
        info "Optick already present"
        return
    fi

    info "fetching Optick ${OPTICK_COMMIT:0:12}"
    rm -rf "${target}.src"
    mkdir -p "${target}.src"
    git -C "${target}.src" init -q
    git -C "${target}.src" remote add origin "${OPTICK_REPO}"
    git -C "${target}.src" fetch -q --depth 1 origin "${OPTICK_COMMIT}"
    git -C "${target}.src" checkout -q FETCH_HEAD

    # The property sheet expects External/Optick/include, and Optick keeps its headers in src/.
    info "arranging Optick headers"
    mkdir -p "${target}/include"
    cp "${target}.src"/src/*.h "${target}/include/"

    # Profiling is off in this port, so the headers alone are enough. USE_OPTICK is 0 in
    # Shipping and the OPTICK_* macros compile away. Building OptickCore is left until
    # somebody actually wants profiling on Linux.
    info "Optick headers installed (no library built: this port does not enable profiling)"
    rm -rf "${target}.src"
}

# ixWebSocket. Base/Network uses it for the resource server connection.
IXWEBSOCKET_REPO="https://github.com/machinezone/IXWebSocket.git"
# v12.0.1, not something older. Base/Network/Servers/NetworkServer_WebSockets.cpp constructs
# ix::WebSocketServer with 8 arguments, and the 8th, sendTimeoutSeconds, only exists from
# v12.0.0 onwards. Anything earlier fails to compile against the engine.
IXWEBSOCKET_TAG="v12.0.1"

fetch_ixwebsocket()
{
    local target="${EXTERNAL_DIR}/ixwebsocket"

    if [[ -f "${target}/include/ixwebsocket/IXWebSocket.h" ]]
    then
        info "ixWebSocket already present"
        return
    fi

    info "fetching ixWebSocket ${IXWEBSOCKET_TAG}"
    rm -rf "${target}.src"
    git clone --depth 1 --branch "${IXWEBSOCKET_TAG}" "${IXWEBSOCKET_REPO}" "${target}.src"

    info "building ixWebSocket"
    cmake -S "${target}.src" -B "${target}.src/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${target}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DUSE_TLS=ON
    cmake --build "${target}.src/build" --parallel "${BUILD_JOBS}"
    cmake --install "${target}.src/build"
    rm -rf "${target}.src"
}

# GameNetworkingSockets. Base/Network uses it for the engine's own client and server.
# This is the expensive one: it needs protobuf and either OpenSSL or libsodium.
GNS_REPO="https://github.com/ValveSoftware/GameNetworkingSockets.git"
GNS_TAG="v1.4.1"

fetch_gamenetworkingsockets()
{
    local target="${EXTERNAL_DIR}/GameNetworkingSockets"

    if [[ -f "${target}/include/GameNetworkingSockets/steam/steamnetworkingsockets.h" ]]
    then
        info "GameNetworkingSockets already present"
        return
    fi

    command -v protoc >/dev/null || fail "GameNetworkingSockets needs protobuf. Install it:

    sudo apt install libprotobuf-dev protobuf-compiler
"

    info "fetching GameNetworkingSockets ${GNS_TAG}"
    rm -rf "${target}.src"
    git clone --depth 1 --branch "${GNS_TAG}" "${GNS_REPO}" "${target}.src"

    info "building GameNetworkingSockets (this takes a while)"
    cmake -S "${target}.src" -B "${target}.src/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${target}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIB=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DUSE_CRYPTO=OpenSSL
    cmake --build "${target}.src/build" --parallel "${BUILD_JOBS}"
    cmake --install "${target}.src/build"
    rm -rf "${target}.src"
}

#-------------------------------------------------------------------------

ALL_DEPENDENCIES=( optick ixwebsocket gamenetworkingsockets )

list_dependencies()
{
    printf '%-24s %s\n' "DEPENDENCY" "STATUS"
    for name in "${ALL_DEPENDENCIES[@]}"
    do
        local status="not fetched"
        case "${name}" in
            optick)                 [[ -f "${EXTERNAL_DIR}/Optick/include/optick.h" ]] && status="ready" ;;
            ixwebsocket)            [[ -f "${EXTERNAL_DIR}/ixwebsocket/include/ixwebsocket/IXWebSocket.h" ]] && status="ready" ;;
            gamenetworkingsockets)  [[ -f "${EXTERNAL_DIR}/GameNetworkingSockets/include/GameNetworkingSockets/steam/steamnetworkingsockets.h" ]] && status="ready" ;;
        esac
        printf '%-24s %s\n' "${name}" "${status}"
    done
}

main()
{
    if [[ "${1:-}" == "--list" ]]
    then
        list_dependencies
        return 0
    fi

    local requested=( "$@" )
    if [[ ${#requested[@]} -eq 0 ]]
    then
        requested=( "${ALL_DEPENDENCIES[@]}" )
    fi

    check_requirements "${requested[@]}"
    mkdir -p "${EXTERNAL_DIR}"

    for name in "${requested[@]}"
    do
        case "${name}" in
            optick)                 fetch_optick ;;
            ixwebsocket)            fetch_ixwebsocket ;;
            gamenetworkingsockets)  fetch_gamenetworkingsockets ;;
            *)                      fail "unknown dependency \"${name}\". Try --list." ;;
        esac
    done

    info "done"
    list_dependencies
}

main "$@"
