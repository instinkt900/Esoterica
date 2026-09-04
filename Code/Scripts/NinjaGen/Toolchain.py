"""Translates the MSBuild property sheets into clang flags.

Everything here comes from Code/PropertySheets/. Where a setting has no Linux equivalent, or
names a dropped dependency, this file says so rather than dropping it silently.

Docs/Linux/03-Dependencies.md holds the property sheet to link flag table this implements.
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

    # The Linux stand-in for the Windows SDK. The Reflector includes <dxcapi.h> but imports no
    # property sheet that supplies it, because on Windows it arrives with the SDK on the default
    # search path. Global here for the same reach. The DXC library stays on DXC.props, where the
    # .vcxproj files declare it.
    'External/DirectXShaderCompiler/inc/dxc',
)

def include_flag( directory ):
    """-I for code this fork or upstream owns, -isystem for everything else.

    clang does not report warnings from a header reached through -isystem, which is the correct
    tool here rather than a -Wno- list: the vendored EASTL, EABase, imgui and box3d headers and
    everything under External/ are not this fork's to fix, and Conventions rule 5 says so for the
    vendored ones. Before this, EASTL's type_pod.h alone accounted for 25,560 of the build's 85,097
    warning diagnostics - 8 unique sites, re-reported once per translation unit that included it.

    Note the order consequence: -isystem paths are searched after every -I path. Nothing in the tree
    relies on a third-party header shadowing a first-party one of the same name, and a full build of
    every configuration is what confirms that.
    """

    is_first_party = ( directory == 'Code'
                       or ( directory.startswith( 'Code/' ) and '/ThirdParty/' not in directory ) )
    return f'-I{directory}' if is_first_party else f'-isystem{directory}'

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
    # The vendored imgui and EASTL define their export macros as __declspec(dllexport) under
    # `#if EE_DLL`, and Code/**/ThirdParty is out of bounds (Conventions rule 5). -fdeclspec
    # makes clang parse the attribute and ignore it, which is the generator-side fix that rule
    # points to. It fires -Wignored-attributes at every first-party declaration that uses one of
    # those macros - the use site is ours even though the macro is not - so the warning is off.
    '-fdeclspec',
    '-Wno-ignored-attributes',
    # Warning flags are NOT here. They depend on who owns the source being compiled; see
    # WARNING_FLAGS below.
)

C_FLAGS   = ( '-std=c17', )     # LanguageStandard_C: stdc17
CPP_FLAGS = ( '-std=c++20', )   # LanguageStandard: stdcpp20

# FloatingPointModel: Precise is clang's default. Never pass -ffast-math.

#-------------------------------------------------------------------------
# Warnings
#-------------------------------------------------------------------------

# The build is warning-free, and it takes three different policies to be.
#
# Esoterica.props sets TreatWarningAsError with EnableAllWarnings and a long list of disabled MSVC
# warning numbers. Upstream is clean on MSVC *because that list exists*. This generator used to
# pass -Wall -Wextra to everything and translate none of it, which produced 85,097 diagnostics at
# 17,264 unique sites - and 99.3% of those sites were upstream's or the Reflector's, not this
# fork's. Every flag below was triaged once, at that measurement; the counts are from it.
#
# Docs/Linux/Scripts/WarningSweep.py reproduces the measurement. Run it after a merge.
#
# Three tiers, chosen per source file by who owns the file:
#
#   FORK        Sources this fork adds (LinuxSources.txt). Strict, and -Werror.
#   UPSTREAM    Upstream's own sources. -Wall -Wextra minus what upstream trips.
#   GENERATED   Reflector output. Nobody reads it and its shape is upstream's to change.

# What upstream first-party code trips, with the unique-site count from the 2026-09-04 sweep.
#
# Most of the -Wformat family is an LP64 artefact: uint64_t is `unsigned long` here and
# `unsigned long long` on Windows, so `%llu` is correct there and merely mismatched here. But the
# triage also found real defects on BOTH platforms - a missing `return`, `==` written for `=`, class
# types passed through printf varargs, and five incomplete format specifiers. Those are recorded in
# Docs/Linux/Progress.md for an upstream report; they are upstream's to fix, not this fork's, and
# suppressing the flag here does not make them go away.
UPSTREAM_WARNING_SUPPRESSIONS = (
    '-Wno-unused-parameter',                        # 711
    '-Wno-sign-compare',                            # 284
    '-Wno-unused-variable',                         # 124
    '-Wno-format-security',                         # 103
    '-Wno-missing-field-initializers',              # 58
    '-Wno-inconsistent-missing-override',           # 30
    '-Wno-format',                                  # 28. Real defects hide in here; see above
    '-Wno-unused-but-set-variable',                 # 25
    '-Wno-unnecessary-virtual-specifier',           # 22
    '-Wno-unused-const-variable',                   # 14
    '-Wno-trigraphs',                               # 11
    '-Wno-unused-private-field',                    # 8
    '-Wno-braced-scalar-init',                      # 7
    '-Wno-unused-function',                         # 4
    '-Wno-return-type',                             # 4, and two of them are undefined behaviour
    '-Wno-reorder-ctor',                            # 3
    '-Wno-unused-value',                            # 3
    '-Wno-deprecated-copy-with-user-provided-copy', # 3
    '-Wno-format-extra-args',                       # 3
    '-Wno-logical-op-parentheses',                  # 2
    '-Wno-unknown-pragmas',                         # 1
    '-Wno-switch',                                  # 1
    '-Wno-unused-lambda-capture',                   # 1
    '-Wno-defaulted-function-deleted',              # 1
    '-Wno-unused-comparison',                       # 1
    '-Wno-self-assign-field',                       # 1
    '-Wno-deprecated-enum-compare-conditional',     # 1
    '-Wno-tautological-pointer-compare',            # 1
    '-Wno-unneeded-internal-declaration',           # 1
    '-Wno-string-concatenation',                    # 1
    '-Wno-range-loop-construct',                    # 1
    '-Wno-missing-designated-field-initializers',   # 1
)

# This fork's own sources. Strict, and an error, so the clean state cannot rot.
#
# Three suppressions, and each is load-bearing:
#
# - The two missing-field-initializer flags are the Vulkan idiom. `VkFooCreateInfo x = { VK_STRUCTURE
#   _TYPE_FOO };` leaves the rest zero-initialised, which is exactly what Vulkan asks for, and
#   RHI_Vulkan.cpp does it 106 times. Designated initialisers do not help: clang 21 moves the same
#   diagnostic to -Wmissing-designated-field-initializers, which -Wextra also enables. The only
#   warning-free spelling is `= {}` plus a separate sType assignment, per struct, per site.
#
# - -Wno-unused-parameter is a real loss and is here under protest. Upstream headers leak 199 of
#   them into this fork's translation units - IDataFile.h, EditorTool.h, ReflectedType.h, String.h -
#   and clang has no per-header suppression that would not also cover this fork's own headers under
#   Code/. An unused parameter in new Linux code will therefore not be caught. Run WarningSweep.py
#   with `--audit-fork` to check for them deliberately.
#
# - -Wno-unused-function is the same leak with one cause: ImguiX.h:501 declares `static void
#   HelpMarker`, and a static function in a header is unused in every translation unit that does not
#   call it. Four of this fork's own files include it transitively, through DialogManager.h. It wants
#   to be `inline`, which is upstream's one-word change to make.
FORK_WARNING_FLAGS = (
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-missing-field-initializers',
    '-Wno-missing-designated-field-initializers',
    '-Wno-unused-parameter',
    '-Wno-unused-function',
)

UPSTREAM_WARNING_FLAGS = ( '-Wall', '-Wextra' ) + UPSTREAM_WARNING_SUPPRESSIONS

# 15,366 sites, and none of them are in code a human wrote. The Reflector's output is regenerated on
# every reflection run and its shape is decided by upstream's code generator, so there is nothing
# here for this fork to fix and nothing for a reviewer to read. -w rather than a flag list, because
# the list would grow every time upstream changes a template.
GENERATED_WARNING_FLAGS = ( '-w', )

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

def build_configurations( configuration_names, repo_root = None ):
    """Returns every configuration the generator emits, for the names in Esoterica.slnx.

    `repo_root` is optional so that Checks.py can ask for the flag shape without a tree. Pass it
    for a real build: Shipping needs it to find a linker that can read bitcode.
    """

    configurations = []

    for name in configuration_names:
        if name not in BASE_CONFIGURATIONS:
            raise RuntimeError(
                f'no flag mapping for configuration "{name}". Add one to BASE_CONFIGURATIONS.' )

        defines, compiler_flags, linker_flags = BASE_CONFIGURATIONS[name]

        # Shipping compiles with -flto, so the linker has to read LLVM bitcode. GNU ld can only do
        # that through LLVMgold.so, which the official LLVM archives do not ship, so the link fails
        # after every compile step passes. ld.lld reads bitcode natively and ships beside the
        # compiler. Same reasoning as find_linker_flags, applied to a whole configuration.
        if '-flto' in linker_flags and repo_root is not None:
            linker_flags = tuple( linker_flags ) + tuple( find_linker_flags( repo_root ) )
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

    #---------------------------------------------------------------------
    # Linux-only. These have no Code/PropertySheets/*.props sibling and never will, because the
    # Windows build has no use for them: Vulkan replaces Direct3D 12, VMA replaces
    # D3D12MemoryAllocator, SPIRV-Reflect replaces ID3D12ShaderReflection, and SDL3 replaces the
    # Win32 window, message loop, raw input and XInput. They reach a project through
    # LINUX_ONLY_SHEETS rather than through a .vcxproj import.
    #---------------------------------------------------------------------

    # The loader and headers come from the distribution, not External/. pkg-config supplies both the
    # include path and -lvulkan. The plain loader rather than volk; swapping to volk is a one-line
    # change here plus an include, if dispatch overhead ever shows up in a profile.
    'Vulkan':                Sheet( pkg_config = 'vulkan' ),
    # Header only. One translation unit in the Vulkan backend defines VMA_IMPLEMENTATION.
    'VMA':                   Sheet( include_directories = ( 'External/VMA/include', ),
                                    requires_path = 'External/VMA',
                                    deferred_to_phase = 'Phase 5' ),
    # The include directory is the dependency root, not an include/ subdirectory: spirv_reflect.h
    # includes "./include/spirv/unified1/spirv.h" relative to itself, so the layout has to mirror
    # the source tree. DownloadDependencies.sh lays it out to match.
    'SPIRVReflect':          Sheet( include_directories = ( 'External/SPIRV-Reflect', ),
                                    library_directories = ( 'External/SPIRV-Reflect/lib', ),
                                    libraries = ( 'spirv-reflect', ),
                                    requires_path = 'External/SPIRV-Reflect',
                                    deferred_to_phase = 'Phase 5' ),
    # Windowing, input and the Vulkan surface. Replaces Application_Win32.cpp's raw Win32 calls,
    # InputDevice_KeyboardMouse_Win32.cpp's raw input, and XInput.
    #
    # From External/ rather than pkg-config, because Ubuntu 24.04 LTS packages no SDL3 at all, so
    # DownloadDependencies.sh builds it and this points at the install prefix.
    'SDL3':                  Sheet( include_directories = ( 'External/SDL3/include', ),
                                    library_directories = ( 'External/SDL3/lib', ),
                                    libraries = ( 'SDL3', ),
                                    requires_path = 'External/SDL3',
                                    deferred_to_phase = 'Phase 6' ),
}

# Sheets that attach to a project by name, because no .vcxproj imports them. Everything else in
# SHEETS arrives through UpstreamProjects.txt, which SyncUpstream.py regenerates from the
# .vcxproj files, so a hand-written entry there would not survive the next sync.
#
# Esoterica.Base needs most of these: it holds RHI_Vulkan.cpp and the _Linux platform layers for
# windowing, input and imgui, and every other project reaches those through it. A project that
# writes SDL code of its own needs the SDL3 sheet as well.
LINUX_ONLY_SHEETS = {
    'Esoterica.Base': ( 'Vulkan', 'VMA', 'SPIRVReflect', 'SDL3' ),
    # EngineApplication_Linux.cpp reads SDL_Event fields, so it needs the headers. Everything
    # else reaches SDL through Esoterica.Base, whose public headers forward declare the three
    # SDL types they mention rather than including anything.
    'Esoterica.Applications.Engine': ( 'SDL3', ),
}

def linux_only_sheets( project_name ):
    return LINUX_ONLY_SHEETS.get( project_name, () )

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
        #
        # The rpath is $ORIGIN relative, not repository relative. -L is resolved by the linker,
        # which ninja runs from the repository root, so a relative path works there. An rpath is
        # resolved by the dynamic loader against the caller's working directory, so a
        # repository-relative one breaks the moment a binary is started from its own output
        # directory.
        #
        # Every output directory is Build/Linux_<configuration>/, two levels below the root, so
        # $ORIGIN/../.. is the root. The $ is doubled because ninja reads this file's output.
        for directory in sheet.library_directories:
            link_flags.append( f'-L{directory}' )
            link_flags.append( f"-Wl,-rpath,'$$ORIGIN/../../{directory}'" )

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
