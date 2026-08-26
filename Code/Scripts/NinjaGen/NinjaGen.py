
from ninja_syntax import Writer
from pathlib import Path

import copy, os, re, subprocess, sys
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
        result.compiler_flags   = '-D__LINUX__ -g -m64 -msse4.2 -mavx -MMD -mwindows -Wno-multichar -Wno-c++20-compat'
        result.c_flags          = '-std=c11'
        result.cpp_flags        = '-std=c++17'
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
        result.compiler_flags   = '-D__LINUX__ -g -m64 -msse4.2 -mavx -MMD'
        result.c_flags          = '-std=c11'
        result.cpp_flags        = '-std=c++17'
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
        result.compiler_flags   = '-D__LINUX__ -g -m64 -msse4.2 -mavx -MMD'
        result.c_flags          = '-std=c11'
        result.cpp_flags        = '-std=c++17'
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
        result.compiler_flags   = ''
        result.c_flags          = '/std:c11'
        result.cpp_flags        = '/std:c++17'
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
    result = {}
    for directory, directories, files in os.walk(repo_root):
        directories[:] = [ directory for directory in directories if directory not in ( '.git', 'Build', 'External' ) ]
        for filename in files:
            current = Path(directory) / filename
            try:
                relative = current.relative_to(repo_root).as_posix()
            except ValueError:
                continue
            result.setdefault(relative.lower(), relative)
    return result


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


def parse_vcxproj(filename, case_map, configuration_names):
    file_path = Path(filename)
    base_path = str(file_path.parent).replace(os.sep, '/')

    result = Project(file_path.stem)

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


def project_output_path(toolchain, configuration, project):
    base = f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{project.name}'
    if project.project_type == 'Lib':
        return f'{base}/{project.name}.a'
    return f'{base}/{project.name}'


def build_toolchain(build, toolchain, configurations, projects):
    build.variable(f'CompilerFlags_{toolchain.name}', toolchain.compiler_flags)
    build.variable(f'CFlags_{toolchain.name}', toolchain.c_flags)
    build.variable(f'CppFlags_{toolchain.name}', toolchain.cpp_flags)
    build.variable(f'LibrarianFlags_{toolchain.name}', toolchain.librarian_flags)
    build.variable(f'LinkerFlags_{toolchain.name}', toolchain.linker_flags)

    for configuration in configurations:
        build.variable(f'CompilerFlags_{configuration.name}', configuration.compiler_flags)
        build.variable(f'LinkerFlags_{configuration.name}', configuration.linker_flags)

        cc_rule     = f'CC_{toolchain.name}_{configuration.name}'
        cpp_rule    = f'CPP_{toolchain.name}_{configuration.name}'
        lib_rule    = f'LIB_{toolchain.name}_{configuration.name}'
        link_rule   = f'LINK_{toolchain.name}_{configuration.name}'

        build.rule(cc_rule,
            f'{toolchain.compiler_c} -MF $out.d -c $in -o $out $CompilerFlags_{toolchain.name} $CompilerFlags_{configuration.name} $CFlags_{toolchain.name}',
            None, '$out.d')
        build.rule(cpp_rule,
            f'{toolchain.compiler_c} -MF $out.d -c $in -o $out $CompilerFlags_{toolchain.name} $CompilerFlags_{configuration.name} $CppFlags_{toolchain.name}',
            None, '$out.d')
        build.rule(lib_rule, f'{toolchain.librarian} $LibrarianFlags_{toolchain.name} rcs $in $out')
        build.rule(link_rule, f'{toolchain.linker} $LinkerFlags_{toolchain.name} $LinkerFlags_{configuration.name} $in -o $out')

        projects_by_name = { project.name: project for project in projects }

        for project in projects:
            object_files = []

            project_out_path = f'Build/x64_Linux_{toolchain.name}_{configuration.name}/{project.name}'
            project_obj_path = f'{project_out_path}/Obj'

            for c_source_file in project.c_source_files:
                out_file = f'{project_obj_path}/{c_source_file}.o'
                build.build(out_file, cc_rule, c_source_file)
                object_files.append(out_file)

            for cpp_source_file in project.cpp_source_files:
                out_file = f'{project_obj_path}/{cpp_source_file}.o'
                build.build(out_file, cpp_rule, cpp_source_file)
                object_files.append(out_file)

            for reference in project.project_references:
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
                build.build(f'{project_out_path}/{project.name}.a', lib_rule, object_files, order_only=order_only or None)
            elif project.project_type == 'Exe':
                build.build(f'{project_out_path}/{project.name}', link_rule, object_files, order_only=order_only or None)
            else:
                warning(f'Unknown project type: {project.name} {project.project_type}')

all_toolchains = [
    Toolchain.WineGCC(),
    Toolchain.GCC(),
    Toolchain.Clang(),
]

esoterica_flags = '-ICode/Base/ThirdParty/EA/EABase/include/Common -ICode/Base/ThirdParty/EA/EASTL/Include -ICode/Base/ThirdParty/imgui -IExternal/Optick/include'
esoterica_flags_msvc = esoterica_flags.replace('-I', '/I')

case_map = build_case_map('.')
configuration_names, all_projects = parse_projects('Esoterica.slnx', case_map)

configuration_flags_by_name = {
    'Debug':    (f'{esoterica_flags} -Wall -O0 -ICode -DEE_DEBUG=1',             ''),
    'Release':  (f'{esoterica_flags} -Wall -O2 -ICode -DEE_RELEASE=1',           ''),
    'Shipping': (f'{esoterica_flags} -Wall -O2 -ICode -DEE_SHIPPING=1 -flto',    '-flto'),
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
compile_commands_warnings = (
    '-Wall',
    ' -Wno-c++98-compat -Wno-c++98-compat-pedantic'
)
compile_commands_configurations = [
    Configuration('Debug', f'{esoterica_flags_msvc} {compile_commands_warnings} /ICode /DEE_DEBUG=1', ''),
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
        build_toolchain(build, toolchain, all_configurations, all_projects)

    # Special case - reflector to generate reflection metadata
    reflector_project = None
    autogenerated_files = []
    reflector_dependencies = set()
    module_pattern = str(Path('_Module/_AutoGenerated/_module.cpp'))
    for project in all_projects:
        if project.name == 'Esoterica.Applications.Reflector':
            reflector_project = project
        for source_file in project.cpp_source_files:
            reflector_dependencies.update(project.header_files)
            if source_file.endswith(module_pattern):
                autogenerated_files.append(source_file)

    if reflector_project is not None:
        reflector_dependencies.update(reflector_project.c_source_files)
        reflector_dependencies.update(reflector_project.cpp_source_files)

        build.rule('REFLECT',
            f'Build/x64_Linux_{reflector_toolchain.name}_{reflector_configuration.name}/{reflector_project.name}/{reflector_project.name} -s Esoterica.slnx')
        if autogenerated_files:
            build.build(autogenerated_files, 'REFLECT', list(reflector_dependencies))
        else:
            note('no autogenerated sources present; the REFLECT rule is emitted without an output (Phase 0, P0.4 reworks this)')
    else:
        warning('Esoterica.Applications.Reflector not found in the solution; not emitting the REFLECT rule')

    build.close()

with open('Build/x64_Linux/Esoterica.x64.CompileCommands.ninja', 'w') as build_output:
    build = Writer(build_output, 110)
    build_toolchain(build, compile_commands_toolchain, compile_commands_configurations, all_projects)
    build.close()

subprocess.run(
    'ninja -f Build/x64_Linux/Esoterica.x64.CompileCommands.ninja -t compdb > compile_commands.json',
    shell = True, check = True);
