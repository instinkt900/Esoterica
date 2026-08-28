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

requirements_llvm()
{
    require_command curl curl
    require_command tar tar
    require_command xz xz-utils
}

requirements_dxc()
{
    require_command cmake cmake
    require_command ninja ninja-build
    require_command python3 python3
    require_command c++ build-essential
}

requirements_directx_headers() { :; }

requirements_ctt()
{
    require_command curl curl
    require_command tar tar
    require_command cargo rustup
    require_command c++ build-essential
}

requirements_meshoptimizer()
{
    require_command cmake cmake
    require_command ninja ninja-build
}

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

# LLVM and libclang. The Reflector parses the codebase's headers with them.
#
# Version 21.1.8, and the pin matters. LLVM.props records no version at all - it just points at
# External/LLVM, which the prebuilt Windows External.zip fills - so the version is inferred from
# the library list it links: LLVMFrontendDirective and LLVMDebugInfoDWARFLowLevel both first
# appear in LLVM 21, and 21.1.8 is the last 21.x release. Clang's C++ AST API is not stable
# across major versions, and ClangUtils.h uses it directly, so a mismatch shows up as obscure
# compile errors rather than a clear diagnostic.
#
# The official prebuilt release is used rather than a source build: it is the same artifact the
# LLVM project ships, it is pinned, and it takes minutes rather than hours. A distro
# libclang-dev is deliberately not used - Ubuntu 24.04 offers 18, three major versions adrift.
LLVM_VERSION="21.1.8"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-Linux-X64.tar.xz"

fetch_llvm()
{
    local target="${EXTERNAL_DIR}/LLVM"

    if [[ -f "${target}/bin/llvm-config" ]]
    then
        info "LLVM already present ($( "${target}/bin/llvm-config" --version ))"
        return
    fi

    info "fetching LLVM ${LLVM_VERSION} (this is a large download)"
    rm -rf "${target}" "${target}.tar.xz"
    curl -fL --no-progress-meter -o "${target}.tar.xz" "${LLVM_URL}"

    info "extracting LLVM"
    mkdir -p "${target}"
    tar -xJf "${target}.tar.xz" -C "${target}" --strip-components=1
    rm -f "${target}.tar.xz"

    info "LLVM $( "${target}/bin/llvm-config" --version ) installed"
}

# DXC, the DirectX Shader Compiler. The Reflector's shader reflection pass and the shader
# pipeline both need it, and eight Engine Render files include generated .esh reflection headers
# that only exist once it has run.
#
# Built from source, not downloaded. Microsoft ships prebuilt Linux binaries, and Phases 2 and 3
# used them, but the SPIR-V back end crashes on the engine's mesh shaders. The fix is in
# Code/Scripts/DXCPatches, so the source has to be compiled here. See the 2026-08-28 decision in
# Docs/Linux/Progress.md, and Docs/Linux/Phases/Phase4-ShaderPipeline.md.
#
# This takes tens of minutes and is by far the slowest dependency. It is also the only one whose
# output differs from what upstream ships, which is why the patches carry a full explanation
# each.
#
# The tag is the same one the prebuilt binaries were built from, so the only difference between
# this compiler and the official one is the patches.
#
# DXC.props expects External/DirectXShaderCompiler with inc/ and lib/x64/, so the build output is
# rearranged to match rather than inventing a second layout.
DXC_REPO="https://github.com/microsoft/DirectXShaderCompiler.git"
DXC_VERSION="v1.10.2605.37"

# DirectX-Headers. Microsoft's cross-platform D3D12 headers.
#
# Not in the plan, and needed: shader reflection uses ID3D12ShaderReflection, declared in
# d3d12shader.h, which is a Windows SDK header. The Linux DXC tarball does not ship it. This is
# the project Microsoft publishes for exactly that gap, and it is what DXC's own Linux
# consumers use.
DIRECTX_HEADERS_REPO="https://github.com/microsoft/DirectX-Headers.git"
DIRECTX_HEADERS_TAG="v1.619.5"

fetch_directx_headers()
{
    local target="${EXTERNAL_DIR}/DirectX-Headers"

    if [[ -f "${target}/include/directx/d3d12shader.h" ]]
    then
        info "DirectX-Headers already present"
        return
    fi

    info "fetching DirectX-Headers ${DIRECTX_HEADERS_TAG}"
    rm -rf "${target}"
    git clone -q --depth 1 --branch "${DIRECTX_HEADERS_TAG}" "${DIRECTX_HEADERS_REPO}" "${target}"

    # d3dcommon.h includes "rpc.h", which does not exist off Windows. DirectX-Headers ships a
    # stub for it, but the whole wsl/stubs directory cannot go on the include path: its COM
    # shims collide with the ones in DXC's own WinAdapter.h, which turns one missing header into
    # twenty redefinition errors. Only rpc.h is needed, and it is an empty stub, so it gets a
    # directory of its own.
    mkdir -p "${target}/include/linux-shims"
    cp "${target}/include/wsl/stubs/rpc.h" "${target}/include/linux-shims/"

    info "DirectX-Headers installed"
}

fetch_dxc()
{
    local target="${EXTERNAL_DIR}/DirectXShaderCompiler"
    local source_dir="${EXTERNAL_DIR}/DirectXShaderCompiler_src"
    local patch_dir="${REPO_ROOT}/Code/Scripts/DXCPatches"

    if [[ -f "${target}/lib/x64/libdxcompiler.so" ]]
    then
        info "DXC already present"
        return
    fi

    # The source tree is kept between runs, because it is a 250MB clone and the build tree next
    # to it is several GB. A re-run resets it instead of fetching it again, which also discards
    # any hand editing done while debugging a patch.
    if [[ -d "${source_dir}/.git" ]]
    then
        info "resetting the DXC source tree"
        # build/ is excluded from the clean on purpose. It is untracked, so a plain clean would
        # delete it and turn every re-run into a full rebuild. Keeping it means a re-run only
        # recompiles the files the patches touch.
        git -C "${source_dir}" reset -q --hard
        git -C "${source_dir}" clean -qfd -e build
    else
        info "cloning DXC ${DXC_VERSION}"
        rm -rf "${source_dir}"
        git clone -q --depth 1 --branch "${DXC_VERSION}" \
            --recurse-submodules --shallow-submodules "${DXC_REPO}" "${source_dir}"
    fi

    # Every patch is applied, in name order, and a failure to apply is fatal. A patch that no
    # longer applies means the pin moved and nobody re-checked the fix, which is exactly the
    # silent-breakage case worth stopping on.
    local patch
    for patch in "${patch_dir}"/*.patch
    do
        [[ -e "${patch}" ]] || break
        info "applying $( basename "${patch}" )"
        git -C "${source_dir}" apply "${patch}" \
            || fail "${patch} does not apply to DXC ${DXC_VERSION}"
    done

    # LLVM_TARGETS_TO_BUILD is set to None by DXC's own cache file: this compiler emits DXIL and
    # SPIR-V, and needs no LLVM hardware back end. Tests are off because they roughly double the
    # build and nothing here runs them.
    #
    # LLVM_APPEND_VC_REV is off, against DXC's cache file, which turns it on. With it on, the
    # generated version header derives from git state, and anything that moves the commit count
    # rewrites that header and rebuilds all 1018 targets rather than the one file a patch
    # touched. That was measured, by committing a patch while debugging it. The reset below keeps
    # the tree on the pinned tag, so the count should be stable anyway; this pins the LLVM half
    # of the version too, and costs nothing.
    info "configuring DXC"
    cmake -S "${source_dir}" -B "${source_dir}/build" -G Ninja \
        -C "${source_dir}/cmake/caches/PredefinedParams.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHLSL_INCLUDE_TESTS=OFF \
        -DSPIRV_BUILD_TESTS=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DCLANG_INCLUDE_TESTS=OFF \
        -DLLVM_APPEND_VC_REV=OFF \
        -DLLVM_PARALLEL_LINK_JOBS=1 \
        >/dev/null

    info "building DXC, this takes tens of minutes"
    ninja -C "${source_dir}/build" -j "${BUILD_JOBS}" dxcompiler dxc

    # libdxil.so is not installed, unlike the prebuilt tarball which ships it. It signs DXIL
    # containers, which is a Windows concern: SPIR-V is not signed, and the DXC property sheet
    # links dxcompiler only.
    # inc/hlsl holds the HLSL library headers, which the prebuilt tarball ships next to inc/dxc.
    # Only inc/dxc is on this project's include path, so they are copied for parity with the
    # tarball rather than because anything here needs them. In the source tree they do not sit
    # under include/, unlike the dxc ones.
    mkdir -p "${target}/inc" "${target}/lib/x64" "${target}/bin/x64"
    cp -r "${source_dir}/include/dxc" "${target}/inc/"
    cp -r "${source_dir}/tools/clang/lib/Headers/hlsl" "${target}/inc/"
    cp -P "${source_dir}/build/lib/libdxcompiler.so"* "${target}/lib/x64/"
    cp "${source_dir}/build/bin/dxc" "${target}/bin/x64/"

    info "DXC installed"
}

# MeshOptimizer. Engine and EngineTools use it for mesh simplification and vertex cache
# optimisation. MeshOptimizer.props points at External/MeshOptimizer/src for headers and
# External/MeshOptimizer/lib for the library, so the layout here matches that rather than
# CMake's default install prefix.
MESHOPT_REPO="https://github.com/zeux/meshoptimizer.git"
# v1.2, not something older. Engine calls meshopt_buildMeshletsSpatial and
# meshopt_computePositionExponent, neither of which exists before v0.24.
MESHOPT_TAG="v1.2"

fetch_meshoptimizer()
{
    local target="${EXTERNAL_DIR}/MeshOptimizer"

    if [[ -f "${target}/src/meshoptimizer.h" && -f "${target}/lib/libmeshoptimizer.a" ]]
    then
        info "MeshOptimizer already present"
        return
    fi

    info "fetching MeshOptimizer ${MESHOPT_TAG}"
    rm -rf "${target}.src"
    git clone -q --depth 1 --branch "${MESHOPT_TAG}" "${MESHOPT_REPO}" "${target}.src"

    info "building MeshOptimizer"
    cmake -S "${target}.src" -B "${target}.src/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DMESHOPT_BUILD_DEMO=OFF
    cmake --build "${target}.src/build" --parallel "${BUILD_JOBS}"

    mkdir -p "${target}/src" "${target}/lib"
    cp "${target}.src"/src/*.h "${target}/src/"
    find "${target}.src/build" -name 'libmeshoptimizer.a' -exec cp {} "${target}/lib/" \;

    rm -rf "${target}.src"
    info "MeshOptimizer installed"
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

# ctt, GPU texture compression. EngineTools/Render/ResourceCompilers/ResourceCompiler_RenderTexture.cpp
# includes <ctt.h> and calls 64 of its symbols, so this is not optional for the resource compiler.
#
# The Windows path fetches a prebuilt ctt_capi.dll out of upstream's External.zip, and that zip
# holds no Linux binary. ctt itself is open source though - a Rust crate with C bindings, MIT /
# Apache-2.0 / Zlib - so this builds the same library rather than substituting a different
# compressor. That distinction matters: swapping in ispc_texcomp or bc7enc would change the
# compressed bytes and break byte-identical output between Linux and Windows. This does not.
#
# 0.5.0, to match what upstream ships. The header in External.zip is dated three days after
# ctt-c-api 0.5.0 was published, and every ctt_* and CTT_* identifier the engine uses is present
# in the 0.5.0 header. Bumping this without checking that again risks a silent API drift.
#
# The crates.io tarball is used rather than a git clone: it is the exact published artifact, it
# carries a Cargo.lock, and it needs no submodules. Default features give the full encoder set
# (bc7enc, Intel ISPC, etcpak, AMD Compressonator, astcenc) with prebuilt ISPC static libraries,
# so no ispc compiler is needed on PATH.
CTT_VERSION="0.5.0"
CTT_URL="https://static.crates.io/crates/ctt-c-api/ctt-c-api-${CTT_VERSION}.crate"

fetch_ctt()
{
    local target="${EXTERNAL_DIR}/ctt"

    if [[ -f "${target}/lib/libctt_capi.so" && -f "${target}/include/ctt.h" ]]
    then
        info "ctt already present"
        return
    fi

    # MSRV is 1.90 and the crate is edition 2024. An older toolchain fails deep inside cargo's
    # resolver with a message that does not mention the version, so check it here instead.
    local rustc_version
    rustc_version=$( cargo --version | awk '{print $2}' )
    if [[ "$( printf '%s\n1.90.0\n' "${rustc_version}" | sort -V | head -1 )" != "1.90.0" ]]
    then
        fail "ctt needs Rust 1.90 or newer, found ${rustc_version}. Update it:

    rustup update stable
"
    fi

    info "fetching ctt ${CTT_VERSION}"
    rm -rf "${target}.src" "${target}.crate"
    curl -fL --no-progress-meter -o "${target}.crate" "${CTT_URL}"
    mkdir -p "${target}.src"
    tar -xzf "${target}.crate" -C "${target}.src" --strip-components=1
    rm -f "${target}.crate"

    info "building ctt (this compiles five C/C++ encoder backends, so it takes a while)"
    cargo build --release --manifest-path "${target}.src/Cargo.toml"

    # CTT.props expects External/ctt/include and External/ctt/lib, which is also where the
    # Windows zip puts them, so the same property sheet mapping works on both platforms.
    mkdir -p "${target}/include" "${target}/lib"
    cp "${target}.src/include/ctt.h" "${target}/include/"
    cp "${target}.src/target/release/libctt_capi.so" "${target}/lib/"

    rm -rf "${target}.src"
    info "ctt installed"
}

#-------------------------------------------------------------------------

ALL_DEPENDENCIES=( optick meshoptimizer ctt ixwebsocket gamenetworkingsockets directx_headers dxc llvm )

list_dependencies()
{
    printf '%-24s %s\n' "DEPENDENCY" "STATUS"
    for name in "${ALL_DEPENDENCIES[@]}"
    do
        local status="not fetched"
        case "${name}" in
            optick)                 [[ -f "${EXTERNAL_DIR}/Optick/include/optick.h" ]] && status="ready" ;;
            meshoptimizer)          [[ -f "${EXTERNAL_DIR}/MeshOptimizer/lib/libmeshoptimizer.a" ]] && status="ready" ;;
            ctt)                    [[ -f "${EXTERNAL_DIR}/ctt/lib/libctt_capi.so" ]] && status="ready (${CTT_VERSION})" ;;
            dxc)                    [[ -f "${EXTERNAL_DIR}/DirectXShaderCompiler/lib/x64/libdxcompiler.so" ]] && status="ready" ;;
            directx_headers)        [[ -f "${EXTERNAL_DIR}/DirectX-Headers/include/directx/d3d12shader.h" ]] && status="ready" ;;
            ixwebsocket)            [[ -f "${EXTERNAL_DIR}/ixwebsocket/include/ixwebsocket/IXWebSocket.h" ]] && status="ready" ;;
            gamenetworkingsockets)  [[ -f "${EXTERNAL_DIR}/GameNetworkingSockets/include/GameNetworkingSockets/steam/steamnetworkingsockets.h" ]] && status="ready" ;;
            llvm)                   [[ -f "${EXTERNAL_DIR}/LLVM/bin/llvm-config" ]] && status="ready ($( "${EXTERNAL_DIR}/LLVM/bin/llvm-config" --version ))" ;;
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
            meshoptimizer)          fetch_meshoptimizer ;;
            ctt)                    fetch_ctt ;;
            dxc)                    fetch_dxc ;;
            directx_headers)        fetch_directx_headers ;;
            ixwebsocket)            fetch_ixwebsocket ;;
            gamenetworkingsockets)  fetch_gamenetworkingsockets ;;
            llvm)                   fetch_llvm ;;
            *)                      fail "unknown dependency \"${name}\". Try --list." ;;
        esac
    done

    info "done"
    list_dependencies
}

main "$@"
