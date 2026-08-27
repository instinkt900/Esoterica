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
ARCHIVER     = 'llvm-ar'

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
)

# NOMINMAX, WIN32_LEAN_AND_MEAN and _CRT_SECURE_NO_WARNINGS sit beside NDEBUG in the same
# PreprocessorDefinitions line. All three are Windows-only, so they are dropped here.

COMMON_COMPILER_FLAGS = (
    '-fno-exceptions',          # ExceptionHandling: false
    '-g',                       # DebugInformationFormat: ProgramDatabase
    '-fPIC',                    # every target may end up inside a .so
    '-msse4.2',                 # the hand-rolled SIMD math assumes both of these
    '-mavx',
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
                  pkg_config = None, note = '' ):
        self.include_directories = tuple( include_directories )
        self.defines = tuple( defines )
        self.libraries = tuple( libraries )     # plain -l names
        self.pkg_config = pkg_config            # pkg-config package name, if it has one
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
    'MeshOptimizer':         Sheet( libraries = ( 'meshoptimizer', ) ),
    'GameNetworkingSockets': Sheet( libraries = ( 'GameNetworkingSockets', 'protobuf', 'ssl', 'crypto' ) ),
    'ixWebSocket':           Sheet( libraries = ( 'ixwebsocket', 'z', 'ssl', 'crypto' ) ),
    'CTT':                   Sheet( libraries = ( 'ctt_capi', ) ),
    'DXC':                   Sheet( libraries = ( 'dxcompiler', ) ),
    'LLVM':                  Sheet( note = 'resolved with llvm-config, see llvm_link_flags()' ),

    # Dropped. Conventions rule 4: leave the EE_ENABLE_* define unset in the Linux build rather
    # than touching a call site. Naming them here, with no flags, is what "recognised and
    # dropped" looks like.
    'AmdAgs':                Sheet( note = 'dropped, Windows only' ),
    'WinPixEventRuntime':    Sheet( note = 'dropped, Windows only' ),
    'Optick':                Sheet( note = 'dropped, Windows only' ),
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

def llvm_link_flags():
    """LLVM.props maps to whatever llvm-config reports. Returns None when it is not installed."""

    if shutil.which( 'llvm-config' ) is None:
        return None

    result = subprocess.run( [ 'llvm-config', '--libs', 'core', 'support' ],
                             capture_output = True, text = True )

    if result.returncode != 0:
        return None

    return result.stdout.split()

#-------------------------------------------------------------------------

def resolve_sheets( sheet_names ):
    """Turns a project's property sheet imports into include directories and link flags.

    Returns ( include_directories, defines, link_flags, problems ).
    """

    include_directories = []
    defines = []
    link_flags = []
    problems = []

    for name in sheet_names:
        sheet = SHEETS.get( name )

        if sheet is None:
            problems.append(
                f'no Linux mapping for property sheet "{name}.props". Add it to '
                f'Toolchain.SHEETS and to Docs/Linux/03-Dependencies.md.' )
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
            flags = llvm_link_flags()
            if flags is None:
                problems.append( 'llvm-config is not installed, so LLVM.props cannot be '
                                 'resolved. The Reflector will not link until Phase 2.' )
            else:
                link_flags.extend( flags )

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
