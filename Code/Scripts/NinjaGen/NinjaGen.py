
from ninja_syntax import Writer
from pathlib import Path

import copy, os, re, shutil, subprocess, sys
import xml.etree.ElementTree as ET


def note(message):
    print(f'NinjaGen: {message}', file=sys.stderr)


def warning(message):
    print(f'NinjaGen: warning: {message}', file=sys.stderr)

class Toolchain:
    def WineGCC():
        result = Toolchain()
        result.name             = 'WineGCC'
        result.compiler_c       = 'winegcc'
        result.compiler_cpp     = 'wineg++'
        result.librarian        = 'ar'
        result.linker           = 'g++'
        result.compiler_flags   = '-g -m64 -msse4.2 -mavx -MMD -mwindows -Wall -Wextra -Wno-multichar'
        result.c_flags          = '-std=c17'
        result.cpp_flags        = '-std=c++20'
        result.librarian_flags  = ''
        result.linker_flags     = ''
        return result

    def GCC():
        result = Toolchain()
        result.name             = 'GCC'
        result.compiler_c       = 'gcc'
        result.compiler_cpp     = 'g++'
        result.librarian        = 'ar'
        result.linker           = 'g++'
        result.compiler_flags   = '-g -m64 -msse4.2 -mavx -MMD -Wall -Wextra -fno-exceptions -fPIC'
        result.c_flags          = '-std=c17'
        result.cpp_flags        = '-std=c++20'
        result.librarian_flags  = ''
        result.linker_flags     = ''
        return result

    def Clang():
        result = Toolchain()
        result.name             = 'Clang'
        result.compiler_c       = 'clang'
        result.compiler_cpp     = 'clang++'
        result.librarian        = 'llvm-ar'
        result.linker           = 'clang++'
        result.compiler_flags   = '-g -m64 -msse4.2 -mavx -MMD -Wall -Wextra -fno-exceptions -fPIC'
        result.c_flags          = '-std=c17'
        result.cpp_flags        = '-std=c++20'
        result.librarian_flags  = ''
        result.linker_flags     = ''
        return result

    def ClangCL():
        result = Toolchain()
        result.name             = 'ClangCL'
        result.compiler_c       = 'clang-cl'
        result.compiler_cpp     = 'clang-cl'
        result.librarian        = 'llvm-ar'
        result.linker           = 'clang-cl'
        result.compiler_flags   = '-Wall -Wextra -Wno-c++98-compat -Wno-c++98-compat-pedantic'
        result.c_flags          = '/std:c17'
        result.cpp_flags        = '/std:c++20'
        result.librarian_flags  = ''
        result.linker_flags     = ''
        return result

class Configuration:
    def __init__(self, name, compiler_flags, linker_flags):
        self.name = name
        self.compiler_flags = compiler_flags
        self.linker_flags = linker_flags

class Project:
    def __init__(self, project_name):
        self.name = project_name
        self.directory = ''  # repo-root-relative directory of the .vcxproj
        self.project_type = ''
        self.configuration_types = {}
        self.c_source_files = []
        self.cpp_source_files = []
        self.header_files = []
        self.project_references = []
        self.build_dependencies = []
        self.excluded_configs = set()
        self.props_by_configuration = {}

def build_case_map(repo_root):
    # Returns (files, directories): case-insensitive maps (lower-cased path ->
    # on-disk path) used to fix up upstream casing. External is pruned because
    # P0.9 has not fetched it yet; references to it stay as written.
    files = {}
    directories = {}
    for directory, dirnames, filenames in os.walk(repo_root):
        dirnames[:] = [ name for name in dirnames if name not in ( '.git', 'Build', 'External' ) ]
        for filename in filenames:
            current = Path(directory) / filename
            try:
                relative = current.relative_to(repo_root).as_posix()
            except ValueError:
                continue
            files.setdefault(relative.lower(), relative)
        try:
            relative_directory = Path(directory).relative_to(repo_root).as_posix()
        except ValueError:
            continue
        if relative_directory != '.':
            directories.setdefault(relative_directory.lower(), relative_directory)
    return files, directories


# A case fixup reported once is reported once: the same mismatched reference reaches
# the resolver many times (a sheet is imported per configuration, each re-walking the
# chain), and repeating the warning 36 times says nothing the first occurrence lacks.
case_fixup_warned = set()

def resolve_repo_file(case_map, candidate):
    candidate = os.path.normpath(candidate.replace('\\', '/')).replace(os.sep, '/')
    if os.path.isfile(candidate):
        return candidate

    actual = case_map.get(candidate.lower())
    if actual is not None:
        if candidate not in case_fixup_warned:
            case_fixup_warned.add(candidate)
            warning(f'{candidate} does not exist, but {actual} does; using the on-disk case (upstream case inconsistency)')
        return actual

    warning(f'{candidate} is listed in the project files but does not exist on disk; skipping it')
    return None


def resolve_import(base_path, case_map, source):
    """Resolve a file reference (ClCompile / ClInclude / props Import) to the path that
    exists on disk. Paths are repo-root-relative and the lookup is case-insensitive;
    tooling and per-user imports ($(VCTargetsPath) / $(UserRootDir)) are not part of the
    repo and resolve to None without a warning."""
    if '$(UserRootDir)' in source or '$(VCTargetsPath)' in source:
        return None

    if source.startswith('$(SolutionDir)'):
        candidate = source[len('$(SolutionDir)'):]
    elif source.startswith('$(ProjectDir)'):
        candidate = f'{base_path}/{source[len("$(ProjectDir)"):]}'
    else:
        candidate = f'{base_path}/{source}'

    return resolve_repo_file(case_map, candidate)


# Every .vcxproj and .props file in the repo is in this MSBuild XML namespace; the
# .slnx is (intentionally) not, so namespace handling lives in the vcxproj layer only.
MSBUILD_NS = '{http://schemas.microsoft.com/developer/msbuild/2003}'

CONDITION_RE = re.compile(r"^'\$\((\w+)\)\|\$\((\w+)\)'=='(\w+)\|(\w+)'$")

def configuration_name_from_condition(condition):
    """Map an MSBuild condition like '$(Configuration)|$(Platform)'=='Debug|x64'' to the
    configuration name it scopes. Anything else (exists(...) guards, other shapes)
    returns None, meaning 'not configuration-scoped'."""
    if not condition:
        return None
    match = CONDITION_RE.match(condition)
    if not match:
        return None
    configuration_macro, platform_macro, configuration, _platform = match.groups()
    if configuration_macro != 'Configuration' or platform_macro != 'Platform':
        return None
    return configuration


def iter_import_scopes(element):
    """Yield (scope, Import_element) pairs, where scope is the configuration name from an
    enclosing ImportGroup condition (or the Import's own condition), or None if the
    import is not configuration-scoped."""
    if element.tag == MSBUILD_NS + 'ImportGroup':
        scope = configuration_name_from_condition(element.get('Condition'))
        for imp in element.findall(MSBUILD_NS + 'Import'):
            yield scope, imp
    elif element.tag == MSBUILD_NS + 'Import':
        yield configuration_name_from_condition(element.get('Condition')), element
    else:
        for child in element:
            yield from iter_import_scopes(child)


def collect_sheet_imports(file_path, case_map, chain, result, active_scope=None):
    """Follow the <Import> chain of a property sheet, appending (scope, resolved_path)
    pairs to result. An import's effective scope is its own condition, or the scope it
    was imported under (MSBuild semantics: a sheet's imports apply wherever the sheet
    applies). 'chain' is the set of sheets on the current import path, used only to
    break import cycles; the same sheet may legitimately be imported from several
    places (different configuration groups included), so there is no global
    deduplication here - per-configuration deduplication happens at materialisation."""
    try:
        tree = ET.parse(file_path)
    except ET.ParseError as error:
        warning(f'cannot parse {file_path}: {error}')
        return

    base_path = str(Path(file_path).parent).replace(os.sep, '/')
    for scope, imp in iter_import_scopes(tree.getroot()):
        source = imp.get('Project')
        if not source:
            warning(f'empty <Import> Project in {file_path}')
            continue
        resolved = resolve_import(base_path, case_map, source)
        if resolved is None or resolved in chain:
            continue
        effective_scope = scope if scope else active_scope
        result.append((effective_scope, resolved))
        collect_sheet_imports(resolved, case_map, chain | { resolved }, result, effective_scope)


# P0.3 - platform source filtering.
#
# The .vcxproj files list Windows sources unconditionally: upstream has no
# platform split, and this port never edits project files (see
# TouchedFiles.md, "Esoterica.slnx, all *.vcxproj"). The generator filters
# each project's file lists to the Linux set instead. A source compiles for
# Linux unless one of these rules excludes it:
#
#   1. stem suffix - <stem>_Win32.* is Windows-only. Its <stem>_Linux.*
#      twin is never listed in the .vcxproj (that is what keeps the
#      project files unmodified), so apply_platform_filter() picks it up
#      from disk after the exclusion pass. A stem ending in another known
#      platform suffix (platform_stem_suffixes, action 'warn') is excluded
#      and warned about loudly: a new upstream platform (a future _Mac or
#      _Durango) must become a visible decision, not a silent surprise in
#      this build.
#   2. path - everything under a Win32/ directory (the editor and engine
#      application entry points and their .rc / .aps resources) is
#      Windows-only. Linux entry points live in the sibling Linux/
#      directories, which the addition pass scans.
#   3. filename - two Windows-only sources carry no platform suffix at all
#      and are excluded by explicit path (windows_only_sources). A third
#      entry there means the P0.3 survey missed something: escalate rather
#      than grow the list (Docs/Linux/Phases/Phase0-BuildSystem.md, P0.3).

# Stem suffix -> action. 'exclude' = Windows-only. 'include' = this port's
# Linux twin (never in the .vcxproj; added from disk). 'warn' = a platform
# this port does not support: exclude the file so it cannot quietly land in
# the Linux build, and warn so the new platform cannot be missed.
platform_stem_suffixes = {
    '_Win32':   'exclude',
    '_Linux':   'include',
    '_Mac':     'warn',
    '_Durango': 'warn',
}

# Windows-only sources with no platform suffix, excluded by repo-relative
# path compared case-insensitively (the values are lowercase).
windows_only_sources = {
    'code/base/render/rhi_direct3d12.cpp',
    'code/applications/resourceserver/resourceserverapplication.cpp',
}

# Extensions apply_platform_filter() recognises when adding files it finds on disk.
linux_source_extensions = ( '.cpp', '.c', '.h', '.hpp', '.inl' )


def classify_platform_source(path):
    """Decide whether one repo-relative source or header path belongs in the
    Linux build. Returns (compiles, reason), where reason names the rule that
    decided: 'win32 suffix', 'unsupported platform suffix', 'windows-only
    filename', 'win32 directory', 'resource file', or 'none' when no rule
    excluded it."""
    lowered = path.replace(os.sep, '/').lower()
    parts = lowered.split('/')
    stem = parts[-1].rsplit('.', 1)[0]

    for suffix in sorted(platform_stem_suffixes, key=len, reverse=True):
        if stem.endswith(suffix.lower()):
            action = platform_stem_suffixes[suffix]
            if action == 'include':
                return True, 'none'
            if action == 'warn':
                warning(f'{path} ends in {suffix}: that platform is not supported here, so it is excluded from the Linux build')
                return False, 'unsupported platform suffix'
            return False, 'win32 suffix'

    if lowered in windows_only_sources:
        return False, 'windows-only filename'

    if 'win32' in parts[:-1]:
        return False, 'win32 directory'

    if parts[-1].rsplit('.', 1)[-1] in ( 'rc', 'aps' ):
        return False, 'resource file'

    return True, 'none'


def apply_platform_filter(project):
    """P0.3: reduce a project's file lists to the Linux set.

    The exclusion pass classifies every listed source and header and drops
    the Windows-only ones (classify_platform_source). The addition pass then
    adds the Linux files the .vcxproj never lists: every *_Linux.<ext> file
    on disk in the directory of an excluded source, in a directory below a
    Platform/ component of a listed path, and in the Linux/ sibling of a
    Win32/ directory. Only directories the .vcxproj already points at are
    scanned; missing directories are skipped silently (the Linux/ app entry
    point directories do not exist until a later phase creates them)."""
    source_lists = ( 'cpp_source_files', 'c_source_files', 'header_files' )

    scan_directories = set()
    excluded = 0
    for name in source_lists:
        kept = []
        for source in getattr(project, name):
            parts = source.replace(os.sep, '/').split('/')
            directory = parts[:-1]
            lowered_directory = [ part.lower() for part in directory ]

            if 'platform' in lowered_directory:
                scan_directories.add('/'.join(directory))
            if 'win32' in lowered_directory:
                for case in ( 'Linux', 'linux' ):  # the on-disk case is unknown until the directory exists
                    replaced = [ case if part.lower() == 'win32' else part for part in directory ]
                    scan_directories.add('/'.join(replaced))

            compiles, _ = classify_platform_source(source)
            if compiles:
                kept.append(source)
            else:
                excluded += 1
                scan_directories.add('/'.join(directory))
        setattr(project, name, kept)

    existing = { source for name in source_lists for source in getattr(project, name) }
    added = 0
    for directory in sorted(scan_directories):
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            stem, dot, extension = entry.rpartition('.')
            if not dot or not stem.endswith('_Linux') or f'.{extension}' not in linux_source_extensions:
                continue
            path = f'{directory}/{entry}'
            if path in existing:
                continue
            if extension == 'cpp':
                target = 'cpp_source_files'
            elif extension == 'c':
                target = 'c_source_files'
            else:
                target = 'header_files'
            getattr(project, target).append(path)
            existing.add(path)
            added += 1

    if excluded or added:
        note(f'{project.name}: platform filtering excluded {excluded} Windows-only file(s) and added {added} Linux file(s)')


# P0.4 - autogenerated source globbing.
#
# Esoterica.props (:74-75) adds two MSBuild wildcards per project, resolved
# against the project directory:
#
#   _Module\_Autogenerated\TypeInfo\*.cpp
#   _Module\_Autogenerated\Shaders\*.cpp
#
# so no .vcxproj ever lists these files. The Reflector (Phase 2) writes them;
# until it has run, the directories do not exist in a fresh checkout. The
# directory's case is inconsistent in upstream - the props wildcard writes
# _Autogenerated (lowercase g), while .gitignore, Reflect.nmake and
# ReflectorSettings.h write _AutoGenerated (capital G) - and on a
# case-sensitive filesystem those are different names. The Reflector is what
# creates the directory, so its spelling lands on disk; the generator matches
# case-insensitively so it keeps working either way.

# Subdirectories of the _AutoGenerated directory that Esoterica.props
# wildcards; the Reflector also writes ShaderReflection/, which is not
# compiled, so it is deliberately not globbed here.
autogenerated_subdirectories = ( 'typeinfo', 'shaders' )


def is_autogenerated_source(path):
    """True for a .cpp under the per-project _Module/_AutoGenerated/{TypeInfo,Shaders}
    directories, matched case-insensitively (see the P0.4 note above)."""
    parts = path.replace(os.sep, '/').lower().split('/')
    return (
        len(parts) >= 4
        and parts[-1].endswith('.cpp')
        and parts[-3] == '_autogenerated'
        and parts[-2] in autogenerated_subdirectories
    )


def glob_autogenerated_sources(project):
    """P0.4: add the _Module/_AutoGenerated source files the .vcxproj never
    lists (the Esoterica.props wildcards). Each existing .cpp is appended to
    project.cpp_source_files after the platform filter, classified like any
    other source. A missing _Module or _AutoGenerated directory silently
    adds nothing: on a fresh checkout the Reflector has not run yet, and
    re-running the generator after reflection is how new files are picked up."""
    module_directory = f'{project.directory}/_Module'
    if not os.path.isdir(module_directory):
        return

    existing = set(project.cpp_source_files)
    added = 0
    for entry in sorted(os.listdir(module_directory)):
        if entry.lower() != '_autogenerated':
            continue
        autogenerated_directory = f'{module_directory}/{entry}'
        for subdirectory in sorted(os.listdir(autogenerated_directory)):
            if subdirectory.lower() not in autogenerated_subdirectories:
                continue
            subdirectory_path = f'{autogenerated_directory}/{subdirectory}'
            if not os.path.isdir(subdirectory_path):
                continue
            for filename in sorted(os.listdir(subdirectory_path)):
                if not filename.lower().endswith('.cpp'):
                    continue
                path = f'{subdirectory_path}/{filename}'
                if path in existing:
                    continue
                compiles, _ = classify_platform_source(path)
                if not compiles:
                    continue
                project.cpp_source_files.append(path)
                existing.add(path)
                added += 1

    if added:
        note(f'{project.name}: added {added} autogenerated source(s) from _Module/_AutoGenerated (the Esoterica.props wildcards)')


# P0.5 - compiler and linker flags.
#
# MSBuild takes the per-project and per-configuration compile settings from the
# property sheets imported by the .vcxproj files (Esoterica.props and its
# imports, plus the per-library sheets such as LLVM.props). The generator
# re-reads those sheets and converts the ClCompile entries to native compiler
# flags; the Link entries are P0.6.

PROPERTY_SHEET_CACHE = {}

WINDOWS_ONLY_DEFINES = { 'NOMINMAX', 'WIN32_LEAN_AND_MEAN', '_CRT_SECURE_NO_WARNINGS' }

missing_include_dirs_warned = set()


class PropertySheet:
    def __init__(self, path):
        self.path = path
        self.macros = {}   # UserMacros: name -> raw value
        self.entries = []  # ClCompile entries: (condition, tag, raw value)


def get_property_sheet(path):
    sheet = PROPERTY_SHEET_CACHE.get(path)
    if sheet is not None:
        return sheet
    sheet = PropertySheet(path)
    try:
        tree = ET.parse(path)
    except ET.ParseError as error:
        warning(f'cannot parse property sheet {path}: {error}')
        PROPERTY_SHEET_CACHE[path] = sheet
        return sheet
    root = tree.getroot()
    for property_group in root.iter(MSBUILD_NS + 'PropertyGroup'):
        if property_group.get('Label') == 'UserMacros':
            for child in property_group:
                if child.text:
                    # The macro name is the element tag; strip the MSBuild
                    # namespace ({http://...msbuild/2003}NAME -> NAME).
                    name = child.tag.split('}')[-1]
                    sheet.macros[name] = child.text
    for item_group in root.iter(MSBUILD_NS + 'ItemDefinitionGroup'):
        cl_compile = item_group.find(MSBUILD_NS + 'ClCompile')
        if cl_compile is None:
            continue
        for tag in ( 'AdditionalIncludeDirectories', 'PreprocessorDefinitions' ):
            for element in cl_compile.findall(MSBUILD_NS + tag):
                sheet.entries.append(( element.get('Condition'), tag, element.text or '' ))
    PROPERTY_SHEET_CACHE[path] = sheet
    return sheet


MACRO_RE = re.compile(r'\$\(([^()]+)\)')

def expand_msbuild_value(value, macros, configuration_name, project_name):
    # Fixed-point expansion: a macro may be defined in terms of another
    # ($(EE_EASTL_DIR) references $(EE_EA_DIR), which references $(SolutionDir)).
    result = value
    for _ in range(8):
        def substitute(match):
            name = match.group(1)
            if name == 'SolutionDir':
                return ''
            if name == 'Configuration':
                return configuration_name
            if name == 'Platform':
                return 'x64'
            if name == 'ProjectName':
                return project_name
            return macros.get(name, '')
        expanded = MACRO_RE.sub(substitute, result)
        if expanded == result:
            break
        result = expanded
    return result


SHEET_CONDITION_RE = re.compile(r'^\s*(.+?)\s*(!=|==)\s*(.+?)\s*$')

def evaluate_sheet_condition(condition, macros, configuration_name, project_name):
    if not condition:
        return True
    expanded = expand_msbuild_value(condition, macros, configuration_name, project_name)
    if 'Contains(' in expanded:
        warning(f'unsupported sheet condition {condition!r}; treating the entry as disabled')
        return False
    match = SHEET_CONDITION_RE.match(expanded)
    if match is None:
        warning(f'unsupported sheet condition {condition!r}; treating the entry as disabled')
        return False
    left, operator, right = match.groups()
    left = left.strip('\'"')
    right = right.strip('\'"')
    equal = left == right
    return equal if operator == '==' else not equal


def resolve_include_segment(segment, dir_map, macros, configuration_name, project_name):
    expanded = expand_msbuild_value(segment, macros, configuration_name, project_name)
    expanded = os.path.normpath(expanded.replace('\\', '/')).replace(os.sep, '/')
    if not expanded or expanded in ( '.', '..' ):
        return None
    if os.path.isdir(expanded):
        return expanded
    actual = dir_map.get(expanded.lower())
    if actual is not None:
        if expanded not in case_fixup_warned:
            case_fixup_warned.add(expanded)
            warning(f'{expanded} does not exist, but {actual} does; using the on-disk case (upstream case inconsistency)')
        return actual
    # Not on disk: External/ dependencies are fetched by P0.9. Emit the path
    # as written so the flags do not change when the dependency lands.
    if expanded not in missing_include_dirs_warned:
        missing_include_dirs_warned.add(expanded)
        note(f'include directory {expanded} (from a property sheet) does not exist on disk; emitting it until the dependency lands')
    return expanded


def resolve_define_segment(segment, case_map, macros, configuration_name, project_name):
    if 'ProjectName.ToUpper()' in segment:
        return project_name.upper().replace('.', '_')
    expanded = expand_msbuild_value(segment, macros, configuration_name, project_name).strip()
    if not expanded:
        return None
    if expanded.split('=', 1)[0].strip() in WINDOWS_ONLY_DEFINES:
        return None
    if '=' in expanded:
        name, value = ( part.strip() for part in expanded.split('=', 1) )
        # A define whose value is a path (EASTL_USER_CONFIG_HEADER) must resolve
        # from the eastl headers' include directories, which a repo-relative
        # path does not; make it absolute.
        if value.startswith('"') and value.endswith('"') and len(value) > 2:
            actual = resolve_repo_file(case_map, value[1:-1])
            if actual is not None:
                return f'{name}="{os.path.abspath(actual)}"'
    return expanded


def project_compile_flags(project, configuration_name, case_map, dir_map):
    # The closure already includes Esoterica.props and everything it imports
    # (EA.props, Optick.props, ...), so walking it once covers every sheet.
    macros = {}
    include_directories = []
    defines = []
    for sheet_path in project.props_by_configuration.get(configuration_name, []):
        sheet = get_property_sheet(sheet_path)
        for name, value in sheet.macros.items():
            macros[name] = value
        for condition, tag, raw in sheet.entries:
            if not evaluate_sheet_condition(condition, macros, configuration_name, project.name):
                continue
            if tag == 'AdditionalIncludeDirectories':
                for segment in raw.split(';'):
                    segment = segment.strip()
                    if not segment or segment == '%(AdditionalIncludeDirectories)':
                        continue
                    resolved = resolve_include_segment(segment, dir_map, macros, configuration_name, project.name)
                    if resolved and resolved not in include_directories:
                        include_directories.append(resolved)
            else:
                for segment in raw.split(';'):
                    segment = segment.strip()
                    if not segment or segment == '%(PreprocessorDefinitions)':
                        continue
                    resolved = resolve_define_segment(segment, case_map, macros, configuration_name, project.name)
                    if resolved and resolved not in defines:
                        defines.append(resolved)
    flags = [ f'-I{directory}' for directory in include_directories ]
    flags += [ f'-D{define}' for define in defines ]
    return ' '.join(flags)


# ---------------------------------------------------------------------------
# P0.6 - Property sheet to link flag mapping (Docs/Linux/03-Dependencies.md)
# ---------------------------------------------------------------------------
#
# Each property sheet contributes link flags to the projects that import it.
# A sheet absent from the table contributes nothing; that is deliberate for
# the dropped sheets (AmdAgs, WinPix, Optick, SuperLuminal, LivePP, NavPower),
# which are skipped and whose EE_ENABLE_* macros are never defined
# (Conventions rule 4).
#
# pkg-config and llvm-config are queried at generation time and cached per
# run; both fall back to explicit -l flags from the table when unavailable.
# Libraries that P0.9 stages into External/ get -L (and, for DXC, -Wl,-rpath)
# into the External/<Name>/lib layout that 03-Dependencies.md pins for
# staging.

EXTERNAL_LIB_DIRS = {
    'dxc.props':                   'External/DirectXShaderCompiler/lib',
    'gamenetworkingsockets.props': 'External/GameNetworkingSockets/lib',
    'ixwebsocket.props':           'External/IXWebSocket/lib',
    'meshoptimizer.props':         'External/MeshOptimizer/lib',
    'ctt.props':                   'External/ctt/lib',
}

PKG_CONFIG_CACHE = {}

def pkg_config_libs(package, fallback):
    if package in PKG_CONFIG_CACHE:
        return PKG_CONFIG_CACHE[package]
    flags = None
    if shutil.which('pkg-config') is not None:
        exists = subprocess.run([ 'pkg-config', '--exists', package ], capture_output=True)
        if exists.returncode == 0:
            libs = subprocess.run([ 'pkg-config', '--libs', package ], capture_output=True, text=True)
            if libs.returncode == 0 and libs.stdout.strip():
                flags = ' '.join(libs.stdout.split())
    if flags is None:
        note(f'pkg-config has no {package}; using the explicit {fallback} from the dependency table')
        flags = fallback
    PKG_CONFIG_CACHE[package] = flags
    return flags

LLVM_CONFIG_CACHE = {}

def llvm_config_libs():
    if 'libs' in LLVM_CONFIG_CACHE:
        return LLVM_CONFIG_CACHE['libs']
    candidates = [ 'External/LLVM/bin/llvm-config' ]
    candidates += [ 'llvm-config' ]
    candidates += [ f'llvm-config-{version}' for version in ( 19, 18, 17 ) ]
    flags = None
    for candidate in candidates:
        if '/' in candidate:
            if not os.path.isfile(candidate):
                continue
            binary = candidate
        else:
            binary = shutil.which(candidate)
            if binary is None:
                continue
        result = subprocess.run([ binary, '--libs' ], capture_output=True, text=True)
        if result.returncode != 0 or not result.stdout.strip():
            continue
        if binary != 'llvm-config' and not binary.startswith('External/LLVM'):
            note(f'using host {binary} for LLVM link flags until External/LLVM is staged (P0.9)')
        flags = ' '.join(result.stdout.split())
        break
    if flags is None:
        note('no llvm-config found (External/LLVM is staged by P0.9); the Reflector links -lclang only')
        flags = '-lclang'
    LLVM_CONFIG_CACHE['libs'] = flags
    return flags

def sheet_link_flags(sheet_name):
    # sheet_name is the lowercased basename of a .props file.
    if sheet_name == 'dxc.props':
        directory = os.path.abspath(EXTERNAL_LIB_DIRS[sheet_name])
        return [ f'-L{directory}', '-ldxcompiler', f'-Wl,-rpath,{directory}' ]
    if sheet_name == 'freetype.props':
        return pkg_config_libs('freetype2', '-lfreetype').split()
    if sheet_name == 'gamenetworkingsockets.props':
        directory = os.path.abspath(EXTERNAL_LIB_DIRS[sheet_name])
        return [ f'-L{directory}', '-lGameNetworkingSockets', '-lprotobuf', '-lssl', '-lcrypto' ]
    if sheet_name == 'ixwebsocket.props':
        directory = os.path.abspath(EXTERNAL_LIB_DIRS[sheet_name])
        return [ f'-L{directory}', '-lixwebsocket', '-lz', '-lssl', '-lcrypto' ]
    if sheet_name == 'meshoptimizer.props':
        directory = os.path.abspath(EXTERNAL_LIB_DIRS[sheet_name])
        return [ f'-L{directory}', '-lmeshoptimizer' ]
    if sheet_name == 'ctt.props':
        # 03-Dependencies.md lists the CTT Linux build as an open question; the
        # flag is wired anyway so the link line is ready when the library lands.
        directory = os.path.abspath(EXTERNAL_LIB_DIRS[sheet_name])
        return [ f'-L{directory}', '-lctt_capi' ]
    if sheet_name == 'sqlite.props':
        return [ '-lsqlite3' ]
    if sheet_name == 'llvm.props':
        return llvm_config_libs().split()
    return []

def project_link_flags(project, configuration_name):
    # Walks the same per-configuration sheet closure as project_compile_flags,
    # so configuration-scoped imports apply (Esoterica.Base drops
    # ixWebSocket.props in Shipping) and the dropped sheets contribute nothing.
    flags = []
    for sheet_path in project.props_by_configuration.get(configuration_name, []):
        for flag in sheet_link_flags(os.path.basename(sheet_path).lower()):
            if flag not in flags:
                flags.append(flag)
    return ' '.join(flags)


def parse_vcxproj(filename, case_map, configuration_names):
    file_path = Path(filename)
    base_path = str(file_path.parent).replace(os.sep, '/')

    result = Project(file_path.stem)
    result.directory = base_path

    tree = ET.parse(filename)
    root = tree.getroot()

    # File items live in <ItemGroup>; the same element names inside
    # <ItemDefinitionGroup> are per-item compiler settings, not files.
    for item_group in root.findall(MSBUILD_NS + 'ItemGroup'):
        for item in item_group.findall(MSBUILD_NS + 'ClCompile'):
            include = item.get('Include')
            if not include:
                warning(f'empty <ClCompile> Include in {filename}')
                continue
            resolved = resolve_import(base_path, case_map, include)
            if resolved is None:
                continue
            if include.endswith('.cpp'):
                result.cpp_source_files.append(resolved)
            elif include.endswith('.c'):
                result.c_source_files.append(resolved)
            else:
                warning(f'unexpected ClCompile source {include} in {filename}')

        for item in item_group.findall(MSBUILD_NS + 'ClInclude'):
            include = item.get('Include')
            if not include:
                warning(f'empty <ClInclude> Include in {filename}')
                continue
            resolved = resolve_import(base_path, case_map, include)
            if resolved is None:
                continue
            if include.endswith('.h') or include.endswith('.hpp') or include.endswith('.inl'):
                result.header_files.append(resolved)
            else:
                warning(f'unexpected ClInclude header {include} in {filename}')

        for item in item_group.findall(MSBUILD_NS + 'ProjectReference'):
            include = item.get('Include')
            if not include:
                warning(f'empty <ProjectReference> Include in {filename}')
                continue
            result.project_references.append(Path(include.replace('\\', '/')).stem)

    for element in root.iter(MSBUILD_NS + 'PropertyGroup'):
        configuration_name = configuration_name_from_condition(element.get('Condition'))
        if configuration_name is None:
            continue
        configuration_type = element.find(MSBUILD_NS + 'ConfigurationType')
        if configuration_type is not None and configuration_type.text:
            result.configuration_types[configuration_name] = configuration_type.text.strip()

    import_pairs = []
    for scope, imp in iter_import_scopes(root):
        source = imp.get('Project')
        if not source:
            warning(f'empty <Import> Project in {filename}')
            continue
        resolved = resolve_import(base_path, case_map, source)
        if resolved is None:
            continue
        import_pairs.append((scope, resolved))
        collect_sheet_imports(resolved, case_map, { resolved }, import_pairs, scope)

    for configuration_name in configuration_names:
        sheets = { path for scope, path in import_pairs if scope is None or scope == configuration_name }
        result.props_by_configuration[configuration_name] = sorted(sheets)

    types = set(result.configuration_types.values())
    if types <= { 'Application' } and types:
        result.project_type = 'Exe'
    elif types <= { 'DynamicLibrary', 'StaticLibrary' } and types:
        # Mixed per-configuration types (Esoterica.Base is DynamicLibrary in Debug/Release,
        # StaticLibrary in Shipping) still build as a library until P0.5 splits the output.
        result.project_type = 'Lib'
    elif not types:
        result.project_type = ''
    else:
        result.project_type = None

    return result

class SlnxProject:
    def __init__(self, path, project_id, folder_name):
        self.path = path
        self.id = project_id
        self.folder_name = folder_name
        self.never_build = False
        self.excluded_configs = set()
        self.build_dependencies = []


def parse_slnx(filename):
    configuration_names = []
    entries = []

    tree = ET.parse(filename)
    root = tree.getroot()

    for element in root.findall('./Configurations/*'):
        if element.tag == 'BuildType':
            name = element.get('Name')
            if name:
                configuration_names.append(name)
            else:
                warning(f'<BuildType> without a Name in <Configurations> of {filename}')
        elif element.tag != 'Platform':
            warning(f'unexpected <{element.tag}> in <Configurations> of {filename}')

    if not configuration_names:
        raise SystemExit(f'NinjaGen: error: no <BuildType> configurations in {filename}')

    for folder in root.findall('./Folder'):
        folder_name = folder.get('Name', '')
        for element in folder.findall('Project'):
            path = element.get('Path')
            if not path:
                warning(f'<Project> without a Path in folder {folder_name} of {filename}')
                continue
            path = path.replace('\\', '/')

            entry = SlnxProject(path, element.get('Id', ''), folder_name)

            for build in element.findall('Build'):
                if build.get('Project', 'true').lower() == 'true':
                    continue
                solution = build.get('Solution')
                if solution is None:
                    entry.never_build = True
                else:
                    configs = [ name for name in solution.split('|') if name != '*' ]
                    if configs:
                        entry.excluded_configs.update(configs)
                    else:
                        entry.never_build = True

            for dependency in element.findall('BuildDependency'):
                project = dependency.get('Project')
                if project:
                    entry.build_dependencies.append(project.replace('\\', '/'))
                else:
                    warning(f'<BuildDependency> without a Project for {path} in {filename}')

            entries.append(entry)

    return configuration_names, entries


def parse_projects(filename, case_map):
    configuration_names, entries = parse_slnx(filename)

    recognized_types = { 'Application', 'DynamicLibrary', 'StaticLibrary', 'Makefile' }
    result = []
    for entry in entries:
        if not Path(entry.path).is_file():
            warning(f'{entry.path} (folder {entry.folder_name}) is listed in the solution but missing on disk; skipping')
            continue

        if entry.never_build:
            note(f'skipping {entry.path} - <Build Project="false"/> in the solution')
            continue

        project = parse_vcxproj(entry.path, case_map, configuration_names)
        project.build_dependencies = entry.build_dependencies
        project.excluded_configs = entry.excluded_configs

        if not project.cpp_source_files and not project.c_source_files:
            note(f'skipping {entry.path} - no ClCompile entries (NMAKE wrapper or non-C++ project)')
            continue

        # P0.3: the .vcxproj lists Windows sources unconditionally; keep only
        # the Linux set (this also adds the unlisted *_Linux files).
        apply_platform_filter(project)

        # P0.4: the Esoterica.props wildcards are MSBuild-side, so the
        # .vcxproj never lists them; add them from disk (a no-op until the
        # Reflector has created the directories).
        glob_autogenerated_sources(project)

        if not project.cpp_source_files and not project.c_source_files:
            # Every listed translation unit was Windows-only, e.g. the Engine
            # application, whose sole source is Win32\EngineApplication_Win32.cpp.
            # The project stays in the graph with its reference and
            # build-dependency edges intact, so the Linux entry point a later
            # phase adds needs no graph change; its link edge carries the
            # project references only until that entry point lands.
            note(f'{entry.path} lists no Linux sources after platform filtering; emitted with project references only until a Linux entry point lands')

        unrecognised = set(project.configuration_types.values()) - recognized_types
        if unrecognised:
            warning(f'{entry.path} uses ConfigurationType {sorted(unrecognised)}; the generator does not recognise it, skipping')
            continue

        result.append(project)

    return configuration_names, result


# P0.5: per-configuration output. A library emits a shared object when its
# per-configuration ConfigurationType is DynamicLibrary (.so, the Windows DLL)
# and an archive when it is StaticLibrary (.a, the Windows .lib). Esoterica.Base
# is DynamicLibrary in Debug/Release and StaticLibrary in Shipping; the Tools
# libraries are DynamicLibrary in every configuration. Sanitizer variants
# (_ASan/_MSan/_TSan) keep their base configuration's type. WineGCC cross
# compiles PE and has no .so, so it keeps the archive.

def configuration_scoping_name(configuration_name):
    return configuration_name.split('_')[0]


def uses_shared_output(toolchain, base_configuration_name, project):
    if project.project_type != 'Lib' or toolchain.name == 'WineGCC':
        return False
    return project.configuration_types.get(base_configuration_name) == 'DynamicLibrary'


def shared_link_flags(toolchain):
    if toolchain.name == 'WineGCC':
        return '-shared'
    return '-shared -fPIC -fvisibility=hidden'


def project_output_path(toolchain, configuration, project):
    base = f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{project.name}'
    if project.project_type == 'Lib':
        if uses_shared_output(toolchain, configuration_scoping_name(configuration.name), project):
            return f'{base}/{project.name}.so'
        return f'{base}/{project.name}.a'
    return f'{base}/{project.name}'


def build_toolchain(build, toolchain, configurations, projects, case_map, dir_map):
    # ProjectFlags is per-edge (the sheet flags depend on the project);
    # LinkFlags selects the link mode per edge (-shared for .so, rpath for
    # executables) and is empty where the mode carries no extra flags.
    build.variable(f'CompilerFlags_{toolchain.name}', toolchain.compiler_flags)
    build.variable(f'CFlags_{toolchain.name}', toolchain.c_flags)
    build.variable(f'CppFlags_{toolchain.name}', toolchain.cpp_flags)
    build.variable(f'LibrarianFlags_{toolchain.name}', toolchain.librarian_flags)
    build.variable(f'LinkerFlags_{toolchain.name}', toolchain.linker_flags)

    for configuration in configurations:
        base_configuration_name = configuration_scoping_name(configuration.name)
        build.variable(f'CompilerFlags_{configuration.name}', configuration.compiler_flags)
        build.variable(f'LinkerFlags_{configuration.name}', configuration.linker_flags)

        cc_rule     = f'CC_{toolchain.name}_{configuration.name}'
        cpp_rule    = f'CPP_{toolchain.name}_{configuration.name}'
        lib_rule    = f'LIB_{toolchain.name}_{configuration.name}'
        link_rule   = f'LINK_{toolchain.name}_{configuration.name}'

        build.rule(cc_rule,
            f'{toolchain.compiler_c} -MF $out.d -c $in -o $out '
            f'$CompilerFlags_{toolchain.name} $CompilerFlags_{configuration.name} $CFlags_{toolchain.name} $ProjectFlags',
            None, '$out.d')
        build.rule(cpp_rule,
            f'{toolchain.compiler_cpp} -MF $out.d -c $in -o $out '
            f'$CompilerFlags_{toolchain.name} $CompilerFlags_{configuration.name} $CppFlags_{toolchain.name} $ProjectFlags',
            None, '$out.d')
        build.rule(lib_rule, f'{toolchain.librarian} $LibrarianFlags_{toolchain.name} rcs $in $out')
        build.rule(link_rule, f'{toolchain.linker} $LinkerFlags_{toolchain.name} $LinkerFlags_{configuration.name} $LinkFlags $in -o $out')

        projects_by_name = { project.name: project for project in projects }

        for project in projects:
            object_files = []
            project_flags = project_compile_flags(project, base_configuration_name, case_map, dir_map)
            # Archives (.a) are never linked, so link libraries only reach the
            # .so and executable edges below.
            link_libraries = project_link_flags(project, base_configuration_name)

            project_out_path = f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{project.name}'
            project_obj_path = f'{project_out_path}/Obj'

            for c_source_file in project.c_source_files:
                out_file = f'{project_obj_path}/{c_source_file}.o'
                build.build(out_file, cc_rule, c_source_file, variables={ 'ProjectFlags': project_flags })
                object_files.append(out_file)

            for cpp_source_file in project.cpp_source_files:
                out_file = f'{project_obj_path}/{cpp_source_file}.o'
                build.build(out_file, cpp_rule, cpp_source_file, variables={ 'ProjectFlags': project_flags })
                object_files.append(out_file)

            for reference in project.project_references:
                reference_project = projects_by_name.get(reference)
                if reference_project is not None and uses_shared_output(toolchain, base_configuration_name, reference_project):
                    object_files.append(f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{reference}/{reference}.so')
                else:
                    object_files.append(f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{reference}/{reference}.a')

            order_only = []
            for dependency in project.build_dependencies:
                dependency_name = Path(dependency).stem
                dependency_project = projects_by_name.get(dependency_name)
                if dependency_project is None:
                    warning(f'{project.name} has a build dependency on {dependency} which the generator does not build; ignoring the edge')
                    continue
                order_only.append(project_output_path(toolchain, configuration, dependency_project))

            if project.project_type == 'Lib':
                if uses_shared_output(toolchain, base_configuration_name, project):
                    link_flags = shared_link_flags(toolchain)
                    if link_libraries:
                        link_flags += f' {link_libraries}'
                    build.build(
                        f'{project_out_path}/{project.name}.so', link_rule, object_files,
                        order_only=order_only or None,
                        variables={ 'LinkFlags': link_flags })
                else:
                    build.build(f'{project_out_path}/{project.name}.a', lib_rule, object_files, order_only=order_only or None)
            elif project.project_type == 'Exe':
                # rpath $ORIGIN lets an executable find the sibling shared
                # objects; this replaces the MSBuild Copy targets in the
                # property sheets. $$ is ninja's escape for a literal $.
                link_flags = '' if toolchain.name == 'WineGCC' else "-Wl,-rpath='$$ORIGIN'"
                if link_libraries:
                    link_flags += f' {link_libraries}' if link_flags else link_libraries
                build.build(
                    f'{project_out_path}/{project.name}', link_rule, object_files,
                    order_only=order_only or None,
                    variables={ 'LinkFlags': link_flags })
            else:
                warning(f'Unknown project type: {project.name} {project.project_type}')

all_toolchains = [
    Toolchain.WineGCC(),
    Toolchain.GCC(),
    Toolchain.Clang(),
]

case_map, dir_map = build_case_map('.')
configuration_names, all_projects = parse_projects('Esoterica.slnx', case_map)

# The per-project include directories and preprocessor defines come from the
# property sheets (project_compile_flags); only the optimisation and warning
# level are configuration-scoped here. -Wall -Wextra without -Werror:
# upstream's TreatWarningAsError + MSVC DisableSpecificWarnings list has no
# usable clang translation (P0.5 spec).
configuration_flags_by_name = {
    'Debug':    ('-O0',             ''),
    'Release':  ('-O2',             ''),
    'Shipping': ('-O2 -flto', '-flto'),
}

base_configurations = []
for name in configuration_names:
    flags = configuration_flags_by_name.get(name)
    if flags is None:
        warning(f'no compiler/linker flags defined for configuration {name}; skipping it until a flag template is added')
        continue
    base_configurations.append(Configuration(name, flags[0], flags[1]))

if not base_configurations:
    raise SystemExit('NinjaGen: error: no configurations with known flags; nothing to generate')

reflector_toolchain = all_toolchains[0]             # WineGCC
reflector_configuration = next(
    ( configuration for configuration in base_configurations if configuration.name == 'Shipping' ),
    None)
if reflector_configuration is None:
    reflector_configuration = base_configurations[0]
    warning(f'no Shipping configuration in the solution; REFLECT will use {reflector_configuration.name}')

compile_commands_toolchain = Toolchain.ClangCL()
compile_commands_configurations = [
    Configuration('Debug', '-O0', ''),
]

all_configurations = []
all_configurations.extend(base_configurations)

for configuration in base_configurations:
    asan_configuration = copy.deepcopy(configuration)
    asan_configuration.name += '_ASan'
    asan_configuration.compiler_flags += ' -fsanitize-address -fno-omit-frame-pointer'
    asan_configuration.linker_flags += ' -fsanitize-address'

    msan_configuration = copy.deepcopy(configuration)
    msan_configuration.name += '_MSan'
    msan_configuration.compiler_flags += ' -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer'
    msan_configuration.linker_flags += ' -fsanitize=memory'

    tsan_configuration = copy.deepcopy(configuration)
    tsan_configuration.name += '_TSan'
    tsan_configuration.compiler_flags += ' -fsanitize=thread'
    tsan_configuration.linker_flags += ' -fsanitize=thread'

    all_configurations.append(asan_configuration)
    all_configurations.append(msan_configuration)
    all_configurations.append(tsan_configuration)

Path('Build/x64_Linux').mkdir(parents = True, exist_ok = True)

with open('Build/x64_Linux/Esoterica.x64.Linux.ninja', 'w') as build_output:
    build = Writer(build_output, 110)

    for toolchain in all_toolchains:
        build_toolchain(build, toolchain, all_configurations, all_projects, case_map, dir_map)

    # Special case - reflector to generate reflection metadata
    reflector_project = None
    autogenerated_files = []
    reflector_dependencies = set()
    for project in all_projects:
        if project.name == 'Esoterica.Applications.Reflector':
            reflector_project = project
        for source_file in project.cpp_source_files:
            reflector_dependencies.update(project.header_files)
            if is_autogenerated_source(source_file):
                autogenerated_files.append(source_file)

    if reflector_project is not None:
        reflector_dependencies.update(reflector_project.c_source_files)
        reflector_dependencies.update(reflector_project.cpp_source_files)

        build.rule('REFLECT',
            f'Build/x64_Linux_{reflector_toolchain.name}_{reflector_configuration.name}/{reflector_project.name}/{reflector_project.name} -s Esoterica.slnx')
        if autogenerated_files:
            build.build(autogenerated_files, 'REFLECT', list(reflector_dependencies))
        else:
            note('no autogenerated sources present yet (the Reflector has not run); the REFLECT rule is emitted without an output')
    else:
        warning('Esoterica.Applications.Reflector not found in the solution; not emitting the REFLECT rule')

    build.close()

with open('Build/x64_Linux/Esoterica.x64.CompileCommands.ninja', 'w') as build_output:
    build = Writer(build_output, 110)
    build_toolchain(build, compile_commands_toolchain, compile_commands_configurations, all_projects, case_map, dir_map)
    build.close()

subprocess.run(
    'ninja -f Build/x64_Linux/Esoterica.x64.CompileCommands.ninja -t compdb > compile_commands.json',
    shell = True, check = True);
