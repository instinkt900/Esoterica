"""Translates the MSBuild property sheets into clang flags.

Everything here comes from Code/PropertySheets/. Where a setting has no Linux equivalent, or
names a dropped dependency, this file says so rather than dropping it silently.

Docs/Linux/03-Dependencies.md holds the property sheet to link flag table this implements.
Docs/Linux/Phases/Phase0-BuildSystem.md, P0.5 to P0.7, holds the flag mapping.
"""

import shutil
import subprocess

from pathlib import Path

#-------------------------------------------------------------------------
# Toolchain
#-------------------------------------------------------------------------

COMPILER_C   = 'clang'
COMPILER_CPP = 'clang++'
LINKER       = 'clang++'

def find_archiver():
    """Picks an archiver that can handle LLVM bitcode.

    Shipping compiles with -flto, so the object files are bitcode rather than ELF and a plain
    `ar` only works where binutils has the LLVM plugin. Distributions often ship llvm-ar only
    under a versioned name, so an unqualified 'llvm-ar' is not safe to assume.
    """

    candidates = [ 'llvm-ar' ]

    # Match the archiver to the compiler's own major version where possible.
    version = subprocess.run( [ COMPILER_CPP, '-dumpversion' ],
                              capture_output = True, text = True )
    if version.returncode == 0 and version.stdout.strip():
        candidates.append( f'llvm-ar-{version.stdout.strip().split( "." )[0]}' )

    candidates.append( 'ar' )

    for candidate in candidates:
        if shutil.which( candidate ) is not None:
            return candidate

    return 'ar'

ARCHIVER     = find_archiver()

#-------------------------------------------------------------------------
# Flags shared by every configuration
#-------------------------------------------------------------------------

# Esoterica.props sets AdditionalIncludeDirectories to $(SolutionDir)Code. EA.props and
# Imgui.props add the rest, and every project imports them through Esoterica.props.
# Note the case: EASTL uses 'Include' and EABase uses 'include'. EA.props writes 'Include' for
# both, which MSBuild does not care about and Linux does. NinjaGen checks that every include
# directory exists, so a case slip here fails loudly instead of producing a build with a
# missing header path.
ESOTERICA_INCLUDE_DIRECTORIES = (
    'Code',
    'Code/Base/ThirdParty/EA/EASTL/Include',
    'Code/Base/ThirdParty/EA/EABase/include/Common',
    'Code/Base/ThirdParty/imgui',
    # Esoterica.props imports Optick.props transitively, so every project gets this path even
    # though no .vcxproj names the sheet. Base/Profiling.h includes <optick.h> unconditionally.
    'External/Optick/include',

    # The Linux stand-in for the Windows SDK.
    #
    # The Reflector includes <dxcapi.h> but imports neither DXC.props nor anything else that
    # would supply it: on Windows it ships with the Windows SDK and is on the compiler's default
    # search path. There is no such default here, so the path is global rather than attached to
    # a property sheet, which is what makes it reachable the way the SDK is.
    #
    # The DXC *library* stays on DXC.props, where the .vcxproj files actually declare it.
    'External/DirectXShaderCompiler/inc/dxc',
)

# EA.props: EASTL_USER_CONFIG_HEADER=$(EE_EASTL_USER_CONFIG_HEADER), whose value already
# includes the quotes. The quotes are part of the macro, so they have to survive the shell.
#
# EASTL does `#include EASTL_USER_CONFIG_HEADER`, so the path resolves against the include
# search path, not the working directory. It is therefore relative to -ICode. MSBuild passes an
# absolute path here, which hides the distinction.
ESOTERICA_DEFINES = (
    ( 'EASTL_USER_CONFIG_HEADER', '"Base/ThirdParty/EA/eastl_Esoterica.h"' ),
)

# Esoterica.props, <ClCompile><PreprocessorDefinitions>. NDEBUG is set for every configuration,
# not only the optimised ones: upstream conditions it on $(Platform) == 'x64', not on
# $(Configuration). Match that rather than improving it.
COMMON_DEFINES = (
    'NDEBUG',
    '_HAS_EXCEPTIONS=0',
    # glibc hides the POSIX surface under a strict -std=c17 or -std=c++20, and the vendored
    # rpmalloc calls posix_madvise. Code/**/ThirdParty cannot be edited (Conventions rule 5),
    # so this is the -D the rule points to. The alternative, -std=gnu17, changes language
    # semantics everywhere to fix one call.
    '_DEFAULT_SOURCE',
    # Profiling.h only sets USE_OPTICK to 0 when EE_DEVELOPMENT_TOOLS is off, so Debug and
    # Release would otherwise emit real Optick calls and need OptickCore at link time. This
    # port takes the headers but never enables profiling, so turn it off for every
    # configuration. Conventions rule 4: the feature stays off through the build configuration,
    # not by touching a call site.
    'USE_OPTICK=0',
)

# NOMINMAX, WIN32_LEAN_AND_MEAN and _CRT_SECURE_NO_WARNINGS sit beside NDEBUG in the same
# PreprocessorDefinitions line. All three are Windows-only, so they are dropped here.

COMMON_COMPILER_FLAGS = (
    '-fno-exceptions',          # ExceptionHandling: false
    '-g',                       # DebugInformationFormat: ProgramDatabase
    '-fPIC',                    # every target may end up inside a .so
    '-msse4.2',                 # the hand-rolled SIMD math assumes these
    '-mavx',
    # Memory.cpp calls _mm256_stream_load_si256 unconditionally, so AVX2 is already the real
    # hardware floor on Windows too. MSVC lets any intrinsic through whatever /arch says;
    # clang gates each one, so the baseline has to be stated.
    '-mavx2',
    # HandleAllocator uses _tzcnt_u64 and _lzcnt_u64. MSVC exposes them unconditionally; clang
    # gates each behind its own instruction set flag.
    '-mbmi',
    '-mlzcnt',
    '-Wall',
    '-Wextra',
    # The vendored imgui and EASTL define their export macros as __declspec(dllexport) under
    # `#if EE_DLL`, and Code/**/ThirdParty is out of bounds (Conventions rule 5). -fdeclspec
    # makes clang parse the attribute and ignore it, which is the generator-side fix that rule
    # points to. It fires -Wignored-attributes hundreds of times from those headers, so the
    # warning is off; nothing first-party uses __declspec.
    '-fdeclspec',
    '-Wno-ignored-attributes',
    # Esoterica.props sets TreatWarningAsError with EnableAllWarnings and a long list of
    # disabled MSVC warning numbers that have no clang equivalent. Phase 0 deliberately does not
    # translate that list, and does not pass -Werror. Conventions rule 3 forbids fixing upstream
    # warnings anyway.
)

C_FLAGS   = ( '-std=c17', )     # LanguageStandard_C: stdc17
CPP_FLAGS = ( '-std=c++20', )   # LanguageStandard: stdcpp20

# FloatingPointModel: Precise is clang's default. Never pass -ffast-math.

#-------------------------------------------------------------------------
# Configurations
#-------------------------------------------------------------------------

class Configuration:
    def __init__( self, name, base_name, defines, compiler_flags, linker_flags ):
        self.name = name
        self.base_name = base_name          # the configuration in Esoterica.slnx terms
        self.defines = tuple( defines )
        self.compiler_flags = tuple( compiler_flags )
        self.linker_flags = tuple( linker_flags )

# Esoterica.props sets EE_DLL for Debug and Release but not Shipping, because Shipping links
# statically. Optimization is Disabled in Debug and MaxSpeed elsewhere, and
# WholeProgramOptimization is on in Shipping only.
BASE_CONFIGURATIONS = {
    'Debug':    ( ( 'EE_DEBUG=1', 'EE_DLL' ),  ( '-O0', ),           () ),
    'Release':  ( ( 'EE_RELEASE=1', 'EE_DLL' ), ( '-O2', ),          () ),
    'Shipping': ( ( 'EE_SHIPPING=1', ),         ( '-O2', '-flto' ),  ( '-flto', ) ),
}

# MSan is deliberately absent. It needs an instrumented libc++ to give usable output, and
# without one it buries the reader in false positives from the standard library.
SANITIZERS = {
    'ASan':  ( ( '-fsanitize=address', '-fno-omit-frame-pointer' ), ( '-fsanitize=address', ) ),
    'TSan':  ( ( '-fsanitize=thread', ),                            ( '-fsanitize=thread', ) ),
    'UBSan': ( ( '-fsanitize=undefined', '-fno-omit-frame-pointer' ),
               ( '-fsanitize=undefined', ) ),
}

# Sanitizers are built on Debug and Release only. Shipping is the link-time-optimised
# configuration; instrumenting it measures the wrong binary and takes far longer to build.
SANITIZED_BASE_CONFIGURATIONS = ( 'Debug', 'Release' )

def build_configurations( configuration_names ):
    """Returns every configuration the generator emits, for the names in Esoterica.slnx."""

    configurations = []

    for name in configuration_names:
        if name not in BASE_CONFIGURATIONS:
            raise RuntimeError(
                f'no flag mapping for configuration "{name}". Add one to BASE_CONFIGURATIONS.' )

        defines, compiler_flags, linker_flags = BASE_CONFIGURATIONS[name]
        configurations.append(
            Configuration( name, name, defines, compiler_flags, linker_flags ) )

        if name not in SANITIZED_BASE_CONFIGURATIONS:
            continue

        for sanitizer_name in sorted( SANITIZERS ):
            sanitizer_compiler_flags, sanitizer_linker_flags = SANITIZERS[sanitizer_name]
            configurations.append( Configuration(
                f'{name}_{sanitizer_name}', name, defines,
                compiler_flags + sanitizer_compiler_flags,
                linker_flags + sanitizer_linker_flags ) )

    return configurations

#-------------------------------------------------------------------------
# Property sheets
#-------------------------------------------------------------------------

class Sheet:
    def __init__( self, include_directories = (), defines = (), libraries = (),
                  library_directories = (), pkg_config = None, requires_path = None,
                  deferred_to_phase = None, note = '' ):
        self.include_directories = tuple( include_directories )
        self.defines = tuple( defines )
        self.libraries = tuple( libraries )             # plain -l names
        self.library_directories = tuple( library_directories )  # -L, for External/ builds
        self.pkg_config = pkg_config                    # pkg-config package name, if it has one
        # A sheet whose dependency is not built yet contributes nothing rather than producing a
        # link error naming a library the reader has no way to supply at this phase.
        self.requires_path = requires_path               # skip the sheet when this is absent
        self.deferred_to_phase = deferred_to_phase       # named in the message when skipped
        self.note = note

# The table from Docs/Linux/03-Dependencies.md. A sheet that is present with no flags
# contributes nothing but is *recognised*; a sheet missing from this table is an error, so that
# a new upstream dependency cannot be ignored by accident.
SHEETS = {
    # Defines and include directories only. Its own imports of EA.props and Imgui.props are
    # folded into ESOTERICA_INCLUDE_DIRECTORIES, because every project imports this sheet.
    'Esoterica':             Sheet(),

    'EA':                    Sheet( note = 'header only, plus vendored source' ),
    'Imgui':                 Sheet( note = 'vendored source' ),
    'Box3D':                 Sheet( include_directories = ( 'Code/Engine/ThirdParty/box3d/include', ) ),
    'RenderDoc':             Sheet( include_directories = ( 'External/RenderDoc', ),
                                    note = 'header only, loaded with dlopen at runtime' ),

    'FreeType':              Sheet( pkg_config = 'freetype2' ),
    'SQLite':                Sheet( libraries = ( 'sqlite3', ) ),
    # MeshOptimizer.props points at src/ for headers and lib/ for the library, rather than a
    # conventional include/ prefix. DownloadDependencies.sh lays it out to match.
    'MeshOptimizer':         Sheet( include_directories = ( 'External/MeshOptimizer/src', ),
                                    library_directories = ( 'External/MeshOptimizer/lib', ),
                                    libraries = ( 'meshoptimizer', ),
                                    requires_path = 'External/MeshOptimizer',
                                    deferred_to_phase = 'Phase 3' ),
    # The install puts headers under include/GameNetworkingSockets/steam, and the engine writes
    # #include <steam/...>, so the search path is one level deeper than the install prefix.
    'GameNetworkingSockets': Sheet( include_directories = ( 'External/GameNetworkingSockets/include/GameNetworkingSockets', ),
                                    library_directories = ( 'External/GameNetworkingSockets/lib', ),
                                    libraries = ( 'GameNetworkingSockets', 'protobuf', 'ssl', 'crypto' ) ),
    'ixWebSocket':           Sheet( include_directories = ( 'External/ixwebsocket/include', ),
                                    library_directories = ( 'External/ixwebsocket/lib', ),
                                    libraries = ( 'ixwebsocket', 'z', 'ssl', 'crypto' ) ),
    # ctt is an open-source Rust crate with C bindings, not the closed Windows blob the layout
    # suggests. The prebuilt External.zip ships only ctt_capi.dll, so DownloadDependencies.sh
    # builds ctt-c-api 0.5.0 from crates.io instead - same library, same version, same ctt.h.
    # Lower-case "ctt", to match CTT.props and the directory the Windows zip extracts. The path
    # was spelled "External/CTT" before, which happens to work on Windows and never on Linux.
    'CTT':                   Sheet( include_directories = ( 'External/ctt/include', ),
                                    library_directories = ( 'External/ctt/lib', ),
                                    libraries = ( 'ctt_capi', ),
                                    requires_path = 'External/ctt' ),
    # DXC.props points at External/DirectXShaderCompiler with inc/ and lib/x64/.
    #
    # Two include paths, not one. The Linux tarball nests its headers under inc/dxc, and
    # ShaderReflection_ShaderCompiler.h also includes d3d12shader.h, which is a Windows SDK
    # header that the tarball does not ship. DirectX-Headers supplies it; that is the project
    # Microsoft publishes for exactly this gap.
    # Include paths are global; see ESOTERICA_INCLUDE_DIRECTORIES for why.
    'DXC':                   Sheet( library_directories = ( 'External/DirectXShaderCompiler/lib/x64', ),
                                    libraries = ( 'dxcompiler', ),
                                    requires_path = 'External/DirectXShaderCompiler',
                                    deferred_to_phase = 'Phase 4' ),
    'LLVM':                  Sheet( include_directories = ( 'External/LLVM/include', ),
                                    library_directories = ( 'External/LLVM/lib', ),
                                    requires_path = 'External/LLVM',
                                    deferred_to_phase = 'Phase 2',
                                    note = 'libraries come from llvm-config, see llvm_link_flags()' ),

    # Dropped. Conventions rule 4: leave the EE_ENABLE_* define unset in the Linux build rather
    # than touching a call site. Naming them here, with no flags, is what "recognised and
    # dropped" looks like.
    'AmdAgs':                Sheet( note = 'dropped, Windows only' ),
    'WinPixEventRuntime':    Sheet( note = 'dropped, Windows only' ),
    # Not dropped after all, but its include path is global rather than per-sheet: see
    # ESOTERICA_INCLUDE_DIRECTORIES. Base/Profiling.h includes <optick.h> unconditionally, and
    # Conventions rule 4 forbids stripping that include, so the header has to exist. No
    # OptickCore library is built: USE_OPTICK stays 0 and the OPTICK_* macros compile away.
    'Optick':                Sheet( note = 'headers only, path is global, profiling never enabled' ),
    'SuperLuminal':          Sheet( note = 'dropped. Do not define EE_ENABLE_SUPERLUMINAL' ),
    'LivePP':                Sheet( note = 'dropped. Do not define EE_ENABLE_LPP' ),
    'NavPower':              Sheet( note = 'dropped. Do not define EE_ENABLE_NAVPOWER' ),
    'FBXSDK':                Sheet( note = 'dropped, no .vcxproj imports it' ),
}

# Sheets whose libraries do not exist yet, by the phase that brings them in. Linking a target
# that needs one of these will fail until then, which is expected and is not a generator bug.
DEFERRED_SHEETS = {
    'DXC':                   'Phase 4',
    'LLVM':                  'Phase 2',
    'CTT':                   'Phase 3',
    'GameNetworkingSockets': 'Phase 1',
    'MeshOptimizer':         'Phase 3',
    'ixWebSocket':           'Phase 3',
    'Box3D':                 'Phase 3',
}

#-------------------------------------------------------------------------

def pkg_config_flags( package, mode ):
    """Returns pkg-config output, or None when pkg-config or the package is missing.

    A missing package is not fatal here. Phase 0 is about producing a correct build file, and
    several dependencies do not arrive until later phases.
    """

    if shutil.which( 'pkg-config' ) is None:
        return None

    result = subprocess.run( [ 'pkg-config', mode, package ],
                             capture_output = True, text = True )

    if result.returncode != 0:
        return None

    return result.stdout.split()

# LLVM.props links clangAST, clangBasic, clangLex and libclang by name, plus about 18 LLVM*
# libraries. The clang ones have no llvm-config equivalent, so they are named; the LLVM ones are
# asked for by component so that the list keeps working if upstream adds one.
# 'clang' is libclang, the stable C API that ClangParser.cpp calls (clang_parseTranslationUnit
# and friends). It ships as a shared library, unlike the rest, which are static.
CLANG_LIBRARIES = ( 'clang', 'clangAST', 'clangBasic', 'clangLex', 'clangFrontend',
                    'clangSerialization', 'clangDriver', 'clangParse', 'clangSema', 'clangEdit',
                    'clangAnalysis', 'clangASTMatchers', 'clangSupport', 'clangAPINotes' )

LLVM_COMPONENTS = ( 'core', 'support', 'analysis', 'object', 'bitreader', 'profiledata',
                    'frontendhlsl', 'frontendopenmp', 'mc', 'transformutils', 'scalaropts',
                    'targetparser', 'demangle', 'remarks', 'binaryformat' )

def find_linker_flags( repo_root ):
    """Returns the flags needed to link against the pinned LLVM, or an empty list.

    The official LLVM release archives hold **LLVM IR bitcode**, not ELF objects: the release is
    built with LTO. GNU ld cannot read them and fails with "file format not recognized". LLD
    understands bitcode natively, and the LLVM tarball ships its own ld.lld, so the Reflector is
    linked with that.

    Only targets that actually link LLVM need this, but passing it everywhere would change the
    linker for the whole build, so the caller applies it per project.
    """

    linker = repo_root / 'External/LLVM/bin/ld.lld'
    if linker.is_file():
        return [ f'-fuse-ld={linker}' ]

    if shutil.which( 'ld.lld' ) is not None:
        return [ '-fuse-ld=lld' ]

    return []

def llvm_config_path( repo_root ):
    """Prefers the pinned External/LLVM over anything on PATH.

    A distro llvm-config would silently report a different major version, and clang's C++ AST
    API is not stable across those, so the pinned build has to win.
    """

    pinned = repo_root / 'External/LLVM/bin/llvm-config'
    if pinned.is_file():
        return str( pinned )

    return shutil.which( 'llvm-config' )

def llvm_link_flags( repo_root ):
    """LLVM.props maps to whatever llvm-config reports. Returns None when it is not installed."""

    config = llvm_config_path( repo_root )
    if config is None:
        return None

    result = subprocess.run( [ config, '--libs' ] + list( LLVM_COMPONENTS ),
                             capture_output = True, text = True )
    if result.returncode != 0:
        return None

    flags = [ f'-l{library}' for library in CLANG_LIBRARIES ]
    flags += result.stdout.split()

    # llvm-config --system-libs names the platform libraries the static LLVM archives need,
    # such as -lz, -lzstd, -ltinfo. Leaving them out gives a wall of undefined references.
    system = subprocess.run( [ config, '--system-libs' ], capture_output = True, text = True )
    if system.returncode == 0:
        flags += system.stdout.split()

    return flags

#-------------------------------------------------------------------------

def resolve_sheets( sheet_names, repo_root = None ):
    """Turns a project's property sheet imports into include directories and link flags.

    Returns ( include_directories, defines, link_flags, problems ).
    """

    include_directories = []
    defines = []
    link_flags = []
    problems = []
    root = repo_root if repo_root is not None else Path( '.' )

    for name in sheet_names:
        sheet = SHEETS.get( name )

        if sheet is None:
            problems.append(
                f'no Linux mapping for property sheet "{name}.props". Add it to '
                f'Toolchain.SHEETS and to Docs/Linux/03-Dependencies.md.' )
            continue

        # Skip a dependency that has not been built yet. Emitting its -l would fail the link
        # with "cannot find -lfoo", which tells the reader nothing about which phase supplies it.
        if sheet.requires_path is not None:
            if not ( root / sheet.requires_path ).exists():
                problems.append(
                    f'{name}.props is skipped: {sheet.requires_path} does not exist. '
                    f'{sheet.deferred_to_phase} builds it. Anything needing its symbols will '
                    f'fail to link until then.' )
                continue

        include_directories.extend( sheet.include_directories )
        defines.extend( sheet.defines )

        if sheet.pkg_config is not None:
            # --cflags matters as much as --libs: Freetype's headers live under
            # /usr/include/freetype2, so <ft2build.h> is not on the default search path.
            cflags = pkg_config_flags( sheet.pkg_config, '--cflags' )
            if cflags is not None:
                include_directories.extend(
                    f[2:] for f in cflags if f.startswith( '-I' ) )

            flags = pkg_config_flags( sheet.pkg_config, '--libs' )
            if flags is None:
                problems.append( f'pkg-config has no package "{sheet.pkg_config}" '
                                 f'(for {name}.props). Falling back to -l{sheet.pkg_config}.' )
                link_flags.append( f'-l{sheet.pkg_config}' )
            else:
                link_flags.extend( flags )

        if name == 'LLVM':
            # Must come before the -l flags, so the linker choice applies to them.
            link_flags.extend( find_linker_flags( root ) )
            flags = llvm_link_flags( root )
            if flags is None:
                problems.append( 'llvm-config is not installed, so LLVM.props cannot be '
                                 'resolved. The Reflector will not link until Phase 2.' )
            else:
                link_flags.extend( flags )

        # -L before -l, and an rpath so an executable finds the .so at run time without
        # LD_LIBRARY_PATH. These live under External/, not in a system directory.
        for directory in sheet.library_directories:
            link_flags.append( f'-L{directory}' )
            link_flags.append( f'-Wl,-rpath,{directory}' )

        link_flags.extend( f'-l{library}' for library in sheet.libraries )

    return include_directories, defines, link_flags, problems

def sheet_include_flags( sheet_names ):
    include_directories, _, _, _ = resolve_sheets( sheet_names )
    return include_directories

def missing_include_directories( repo_root, include_directories ):
    """Returns the include directories that do not exist.

    A path that is merely mis-cased still produces a build that compiles nothing useful, and the
    error it eventually gives names a header rather than the include path. Check up front.
    """

    return [ d for d in include_directories
             if not Path( d ).is_absolute() and not ( repo_root / d ).is_dir() ]

def project_define( project_name ):
    """Esoterica.props sets $(ProjectName.ToUpper().Replace('.','_')).

    This drives the dllexport and dllimport switch in every _Module/API.h, so it has to match
    MSBuild exactly. Esoterica.Base becomes ESOTERICA_BASE.
    """

    return project_name.upper().replace( '.', '_' )
