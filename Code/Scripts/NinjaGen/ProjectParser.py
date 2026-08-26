"""Reads Esoterica.slnx and the .vcxproj files it lists, and turns them into a build model.

This is the input half of the Linux build generator. It never writes a build file. NinjaGen.py
does that.

The point of parsing the Visual Studio project files, rather than keeping a separate list of
sources, is that upstream is Windows-only and active. When upstream adds a source file, it edits
a .vcxproj, and the Linux build picks the file up with no change on this side. See
Docs/Linux/02-Architecture.md.

Nothing here writes to the .vcxproj files or to Esoterica.slnx. Reading only.
"""

import os
import re
import sys
import xml.etree.ElementTree as ElementTree

from pathlib import Path

#-------------------------------------------------------------------------
# Constants
#-------------------------------------------------------------------------

# Every element in a .vcxproj sits in this namespace. Esoterica.slnx has no namespace.
MSBUILD_NAMESPACE = 'http://schemas.microsoft.com/developer/msbuild/2003'

# MSBuild ConfigurationType values, mapped to what the generator has to produce.
PROJECT_TYPE_SHARED_LIBRARY = 'SharedLibrary'
PROJECT_TYPE_STATIC_LIBRARY = 'StaticLibrary'
PROJECT_TYPE_EXECUTABLE     = 'Executable'
PROJECT_TYPE_NMAKE          = 'NMake'

CONFIGURATION_TYPE_MAP = {
    'DynamicLibrary': PROJECT_TYPE_SHARED_LIBRARY,
    'StaticLibrary':  PROJECT_TYPE_STATIC_LIBRARY,
    'Application':    PROJECT_TYPE_EXECUTABLE,
    'Makefile':       PROJECT_TYPE_NMAKE,
}

C_SOURCE_EXTENSIONS   = ( '.c', )
CPP_SOURCE_EXTENSIONS = ( '.cpp', '.cc', '.cxx' )
HEADER_EXTENSIONS     = ( '.h', '.hpp', '.hxx', '.inl' )

# Sources the Linux build never wants, whatever else the rules say.
IGNORED_SOURCE_EXTENSIONS = ( '.rc', '.aps', '.def', '.manifest' )

#-------------------------------------------------------------------------
# Warnings
#-------------------------------------------------------------------------

# Collected rather than printed, so that a caller can fail the run on a drift warning if it
# wants to, and so that the output stays deterministic.
_warnings = []

def warn( message ):
    _warnings.append( message )
    print( f'warning: {message}', file = sys.stderr )

def get_warnings():
    return list( _warnings )

def clear_warnings():
    _warnings.clear()

#-------------------------------------------------------------------------
# Platform source filtering
#-------------------------------------------------------------------------

# The most important table in the generator. A source file belongs to a platform in one of two
# ways: a `_<Platform>` suffix on its stem, or a `<Platform>/` directory in its path. Both forms
# appear upstream.
#
#   Code/Base/Threading/Platform/Threading_Win32.cpp    <- suffix form
#   Code/Applications/Editor/Win32/EditorApplication_Win32.cpp   <- both forms
#
# 'linux' entries are kept. 'windows' entries are dropped. Everything else is a platform this
# port does not build, and it produces a warning instead of being silently dropped. A new
# upstream platform must not slip through unnoticed.
PLATFORM_TOKENS = {
    'win32':    'windows',
    'win64':    'windows',
    'windows':  'windows',
    'linux':    'linux',
    # Xbox is Windows-derived, but this port does not build it. Classing it as 'other' means a
    # new console source warns instead of disappearing quietly.
    'uwp':      'other',
    'xbox':     'other',
    'xboxone':  'other',
    'durango':  'other',
    'scarlett': 'other',
    'mac':      'other',
    'macos':    'other',
    'osx':      'other',
    'apple':    'other',
    'ios':      'other',
    'android':  'other',
    'switch':   'other',
    'nx':       'other',
    'ps4':      'other',
    'ps5':      'other',
    'orbis':    'other',
    'prospero': 'other',
    'posix':    'other',
    'unix':     'other',
    'emscripten': 'other',
    'wasm':     'other',
}

def classify_platform( source_path ):
    """Returns ( platform, reason ) for a repo-relative source path.

    `platform` is 'windows', 'linux', 'other', or None when the file is platform-neutral.
    `reason` names the path element that decided it, for warning messages.
    """

    path = Path( source_path )

    # Directory form. Checked first, because Win32/EditorApplication_Win32.cpp agrees either way
    # but a future Win32/Something.cpp only matches here.
    for part in path.parent.parts:
        platform = PLATFORM_TOKENS.get( part.lower() )
        if platform is not None:
            return platform, f'{part}/ directory'

    # Suffix form.
    stem_tokens = path.stem.split( '_' )
    if len( stem_tokens ) > 1:
        platform = PLATFORM_TOKENS.get( stem_tokens[-1].lower() )
        if platform is not None:
            return platform, f'_{stem_tokens[-1]} suffix'

    return None, ''

def is_ignored_source( source_path ):
    return Path( source_path ).suffix.lower() in IGNORED_SOURCE_EXTENSIONS

def filter_platform_sources( source_paths ):
    """Drops Windows sources, keeps neutral and Linux ones, warns about anything else.

    Returns ( kept, dropped ). `dropped` feeds the Linux source discovery below, which looks
    beside each dropped Windows file for its Linux sibling.
    """

    kept = []
    dropped = []

    for source_path in source_paths:
        if is_ignored_source( source_path ):
            dropped.append( source_path )
            continue

        platform, reason = classify_platform( source_path )

        if platform == 'windows':
            dropped.append( source_path )
        elif platform == 'other':
            warn( f'{source_path}: platform source for an unsupported platform ({reason}). '
                  f'It is not built on Linux. Add its token to PLATFORM_TOKENS if this is new '
                  f'upstream code.' )
            dropped.append( source_path )
        else:
            kept.append( source_path )

    return kept, dropped

def discover_linux_sources( repo_root, project_directory, dropped_paths, extensions ):
    """Finds the *_Linux files that no .vcxproj lists.

    The .vcxproj files never mention Linux sources, by design: that is what keeps them
    unmodified, and keeps upstream merges cheap. So the generator finds them itself, in three
    places:

      1. Beside every dropped Windows source. Threading_Win32.cpp implies Threading_Linux.cpp.
      2. Under any Platform/ directory in the project, whether or not a Windows file was dropped
         from it.
      3. Under a Linux/ directory matching a dropped Win32/ directory.
    """

    search_directories = set()

    for dropped_path in dropped_paths:
        path = Path( dropped_path )

        # Rule 1: the directory the Windows file sat in.
        search_directories.add( path.parent )

        # Rule 3: swap a Win32/ path element for Linux/.
        parts = list( path.parent.parts )
        replaced = False
        for index, part in enumerate( parts ):
            if PLATFORM_TOKENS.get( part.lower() ) == 'windows':
                parts[index] = 'Linux'
                replaced = True
        if replaced:
            search_directories.add( Path( *parts ) )

    # Rule 2: every Platform/ directory under the project.
    project_absolute = repo_root / project_directory
    if project_absolute.is_dir():
        for directory, subdirectories, _ in os.walk( project_absolute ):
            subdirectories[:] = [ s for s in subdirectories if s != 'ThirdParty' ]
            if Path( directory ).name.lower() == 'platform':
                search_directories.add( Path( directory ).relative_to( repo_root ) )

    found = set()

    for search_directory in search_directories:
        absolute_directory = repo_root / search_directory
        if not absolute_directory.is_dir():
            continue

        for entry in absolute_directory.iterdir():
            if not entry.is_file():
                continue
            if entry.suffix.lower() not in extensions:
                continue

            platform, _ = classify_platform( entry.relative_to( repo_root ) )
            if platform == 'linux':
                found.add( entry.relative_to( repo_root ).as_posix() )

    return sorted( found )

#-------------------------------------------------------------------------
# Autogenerated source globbing
#-------------------------------------------------------------------------

# Esoterica.props adds these as MSBuild wildcards, so no .vcxproj lists the files:
#
#   <_WildCardClCompile Include="_Module\_Autogenerated\TypeInfo\*.cpp" />
#   <_WildCardClCompile Include="_Module\_Autogenerated\Shaders\*.cpp" />
#
# Upstream spells the directory both '_Autogenerated' (Esoterica.props) and '_AutoGenerated'
# (Reflect.nmake, .gitignore). Windows does not care. Linux does, so match case-insensitively
# and use whatever is actually on disk.
AUTOGENERATED_GLOBS = (
    ( '_Module', '_Autogenerated', 'TypeInfo' ),
    ( '_Module', '_Autogenerated', 'Shaders' ),
)

def resolve_directory_ignoring_case( root, parts ):
    """Walks `parts` under `root`, matching each element case-insensitively.

    Returns the real path, or None when any element is missing. The directories do not exist
    before the Reflector has run, so absence is normal and not an error.
    """

    current = root
    for part in parts:
        if not current.is_dir():
            return None

        match = None
        for entry in sorted( current.iterdir() ):
            if entry.is_dir() and entry.name.lower() == part.lower():
                match = entry
                break

        if match is None:
            return None
        current = match

    return current

def resolve_file_ignoring_case( repo_root, relative_path ):
    """Finds a listed source on a case-sensitive filesystem.

    MSBuild does not care about case, so a .vcxproj entry can disagree with the file on disk.
    Upstream has at least one: Esoterica.Engine.Runtime lists 'Navmesh/NavPower.cpp' and the
    file is 'Navmesh/Navpower.cpp'. Windows builds it, Linux would not find it. Resolving the
    case here fixes it in the generator, which Conventions rule 3 prefers over editing upstream.

    Returns the real repo-relative path, or None when nothing matches.
    """

    if ( repo_root / relative_path ).is_file():
        return relative_path

    path = Path( relative_path )
    directory = resolve_directory_ignoring_case( repo_root, path.parent.parts )
    if directory is None:
        return None

    for entry in sorted( directory.iterdir() ):
        if entry.is_file() and entry.name.lower() == path.name.lower():
            return entry.relative_to( repo_root ).as_posix()

    return None

def is_autogenerated_path( relative_path ):
    parts = [ part.lower() for part in Path( relative_path ).parts ]
    return '_autogenerated' in parts

def glob_autogenerated_sources( repo_root, project_directory ):
    """Returns the reflection and shader sources the Reflector emits for this project."""

    found = []
    project_absolute = repo_root / project_directory

    for parts in AUTOGENERATED_GLOBS:
        directory = resolve_directory_ignoring_case( project_absolute, parts )
        if directory is None:
            continue

        for entry in sorted( directory.iterdir() ):
            if entry.is_file() and entry.suffix.lower() == '.cpp':
                found.append( entry.relative_to( repo_root ).as_posix() )

    return sorted( found )

#-------------------------------------------------------------------------
# .vcxproj parsing
#-------------------------------------------------------------------------

class Project:
    """One .vcxproj, reduced to what the Linux build needs."""

    def __init__( self, name, project_path ):
        self.name = name                    # <ProjectName>, or the file stem
        self.path = project_path            # repo-relative, posix
        self.directory = str( Path( project_path ).parent.as_posix() )

        self.c_sources = []                 # repo-relative, posix, Linux set
        self.cpp_sources = []
        self.headers = []                   # REFLECT rule dependency set

        self.excluded_sources = []          # dropped by platform filtering, kept for reporting
        self.autogenerated_sources = []     # globbed, not listed in the .vcxproj

        self.project_references = []        # repo-relative .vcxproj paths
        self.configuration_types = {}       # configuration name -> PROJECT_TYPE_*
        self.property_sheets = {}           # configuration name -> [ sheet name ]

        self.is_nmake_wrapper = False       # no ClCompile entries at all

    @property
    def all_sources( self ):
        return sorted( self.c_sources + self.cpp_sources + self.autogenerated_sources )

    def get_project_type( self, configuration ):
        return self.configuration_types.get( configuration )

    def get_property_sheets( self, configuration ):
        return self.property_sheets.get( configuration, [] )

def strip_namespace( tag ):
    return tag.split( '}', 1 )[-1] if tag.startswith( '{' ) else tag

def msbuild_path_to_posix( value ):
    return value.replace( '\\', '/' )

CONFIGURATION_CONDITION = re.compile(
    r"'\$\(Configuration\)\|\$\(Platform\)'\s*==\s*'([^|']+)\|([^']+)'" )

def parse_configuration_condition( condition ):
    """Returns ( configuration, platform ) from an MSBuild condition, or ( None, None )."""

    if not condition:
        return None, None

    match = CONFIGURATION_CONDITION.search( condition )
    if match is None:
        return None, None

    return match.group( 1 ), match.group( 2 )

def parse_vcxproj( repo_root, project_path, configurations, platform ):
    """Parses one .vcxproj into a Project.

    `project_path` is repo-relative. Source paths inside a .vcxproj are relative to the .vcxproj,
    so they are rebased onto the repo root here. Every path this function stores is
    repo-relative and posix.
    """

    absolute_path = repo_root / project_path
    project_directory = Path( project_path ).parent

    tree = ElementTree.parse( absolute_path )
    root = tree.getroot()

    project = Project( Path( project_path ).stem, Path( project_path ).as_posix() )

    listed_c_sources = []
    listed_cpp_sources = []
    listed_headers = []
    unknown_sources = []

    for element in root.iter():
        tag = strip_namespace( element.tag )

        if tag == 'ProjectName' and element.text:
            project.name = element.text.strip()

        elif tag == 'ClCompile' and 'Include' in element.attrib:
            source_path = ( project_directory /
                            msbuild_path_to_posix( element.attrib['Include'] ) ).as_posix()
            extension = Path( source_path ).suffix.lower()

            if extension in CPP_SOURCE_EXTENSIONS:
                listed_cpp_sources.append( source_path )
            elif extension in C_SOURCE_EXTENSIONS:
                listed_c_sources.append( source_path )
            else:
                unknown_sources.append( source_path )

        elif tag == 'ClInclude' and 'Include' in element.attrib:
            header_path = ( project_directory /
                            msbuild_path_to_posix( element.attrib['Include'] ) ).as_posix()
            if Path( header_path ).suffix.lower() in HEADER_EXTENSIONS:
                listed_headers.append( header_path )
            else:
                unknown_sources.append( header_path )

        elif tag == 'ProjectReference' and 'Include' in element.attrib:
            reference_path = ( project_directory /
                               msbuild_path_to_posix( element.attrib['Include'] ) )
            project.project_references.append( os.path.normpath( reference_path ).replace( '\\', '/' ) )

    for source_path in unknown_sources:
        if not is_ignored_source( source_path ):
            warn( f'{project_path}: unrecognised file extension on "{source_path}". '
                  f'It is not built on Linux.' )

    # ConfigurationType varies by configuration. Esoterica.Base is a DynamicLibrary in Debug and
    # Release, and a StaticLibrary in Shipping, declared in three separate PropertyGroup blocks.
    # Read it per configuration rather than once.
    for property_group in root.iter( f'{{{MSBUILD_NAMESPACE}}}PropertyGroup' ):
        configuration, condition_platform = parse_configuration_condition(
            property_group.attrib.get( 'Condition' ) )
        if configuration is None or condition_platform != platform:
            continue

        type_element = property_group.find( f'{{{MSBUILD_NAMESPACE}}}ConfigurationType' )
        if type_element is None or not type_element.text:
            continue

        configuration_type = type_element.text.strip()
        mapped_type = CONFIGURATION_TYPE_MAP.get( configuration_type )
        if mapped_type is None:
            warn( f'{project_path}: unknown ConfigurationType "{configuration_type}" '
                  f'for {configuration}.' )
            continue

        project.configuration_types[configuration] = mapped_type

    # Property sheet imports are per configuration too. Shipping drops ixWebSocket.props in
    # Esoterica.Base, for example. The sheets decide which external libraries a target links.
    for import_group in root.iter( f'{{{MSBUILD_NAMESPACE}}}ImportGroup' ):
        configuration, condition_platform = parse_configuration_condition(
            import_group.attrib.get( 'Condition' ) )
        if configuration is None or condition_platform != platform:
            continue

        sheets = project.property_sheets.setdefault( configuration, [] )
        for import_element in import_group.iter( f'{{{MSBUILD_NAMESPACE}}}Import' ):
            import_path = import_element.attrib.get( 'Project', '' )
            if 'PropertySheets' not in import_path.replace( '\\', '/' ):
                continue

            sheet_name = Path( msbuild_path_to_posix( import_path ) ).stem
            if sheet_name not in sheets:
                sheets.append( sheet_name )

    # An NMAKE wrapper project has no C++ sources. Code/Scripts/Reflect is the only one: it
    # drives Reflect.nmake. Detect it by the absence of ClCompile entries, not by its name, so
    # that a renamed or added wrapper still works.
    project.is_nmake_wrapper = len( listed_c_sources ) + len( listed_cpp_sources ) == 0

    kept_c, dropped_c = filter_platform_sources( listed_c_sources )
    kept_cpp, dropped_cpp = filter_platform_sources( listed_cpp_sources )
    kept_headers, dropped_headers = filter_platform_sources( listed_headers )

    dropped_sources = dropped_c + dropped_cpp

    project.c_sources = sorted( set( kept_c + discover_linux_sources(
        repo_root, project.directory, dropped_sources, C_SOURCE_EXTENSIONS ) ) )
    project.cpp_sources = sorted( set( kept_cpp + discover_linux_sources(
        repo_root, project.directory, dropped_sources, CPP_SOURCE_EXTENSIONS ) ) )
    project.headers = sorted( set( kept_headers + discover_linux_sources(
        repo_root, project.directory, dropped_headers + dropped_sources, HEADER_EXTENSIONS ) ) )

    project.excluded_sources = sorted( dropped_c + dropped_cpp + dropped_headers )
    project.autogenerated_sources = glob_autogenerated_sources( repo_root, project.directory )

    project.c_sources = resolve_listed_paths( repo_root, project_path, project.c_sources )
    project.cpp_sources = resolve_listed_paths( repo_root, project_path, project.cpp_sources )
    project.headers = resolve_listed_paths( repo_root, project_path, project.headers )

    return project

def resolve_listed_paths( repo_root, project_path, paths ):
    """Fixes the case of listed paths, and drops the ones that are not on disk at all.

    A path that no file matches is a stale .vcxproj entry. Dropping it keeps ninja from failing
    with 'missing and no known rule to make it', which says nothing useful.
    """

    resolved = []

    for relative_path in paths:
        # A path under _Module/_AutoGenerated is a Reflector output. It is absent until the
        # Reflector has run, which is normal, so keep it and stay quiet.
        if is_autogenerated_path( relative_path ):
            resolved.append( relative_path )
            continue

        real_path = resolve_file_ignoring_case( repo_root, relative_path )

        if real_path is None:
            warn( f'{project_path}: "{relative_path}" is listed but does not exist on disk. '
                  f'It is not built.' )
            continue

        if real_path != relative_path:
            warn( f'{project_path}: "{relative_path}" is listed, but the file on disk is '
                  f'"{real_path}". Using the name on disk.' )

        resolved.append( real_path )

    return sorted( set( resolved ) )

#-------------------------------------------------------------------------
# .slnx parsing
#-------------------------------------------------------------------------

class SolutionProject:
    """One <Project> entry in Esoterica.slnx, before the .vcxproj is read."""

    def __init__( self, project_path ):
        self.path = project_path            # repo-relative, posix
        self.never_build = False            # <Build Project="false" />
        self.excluded_configurations = set()# <Build Solution="Shipping|*" Project="false" />
        self.build_dependencies = []        # <BuildDependency Project="..." />

    def builds_in( self, configuration ):
        return not self.never_build and configuration not in self.excluded_configurations

class Solution:
    def __init__( self, solution_path ):
        self.path = solution_path
        self.configurations = []            # <BuildType Name="..." />, in file order
        self.platforms = []                 # <Platform Name="..." />
        self.projects = []                  # [ Project ], .vcxproj already parsed
        self.solution_projects = {}         # project path -> SolutionProject

    def get_project( self, project_path ):
        for project in self.projects:
            if project.path == project_path:
                return project
        return None

    def get_project_by_name( self, name ):
        for project in self.projects:
            if project.name == name:
                return project
        return None

def matches_solution_filter( solution_filter, configuration, platform ):
    """Matches a <Build Solution="Shipping|*"> filter against one configuration.

    '*' is a wildcard on either side. An absent filter matches everything.
    """

    if not solution_filter:
        return True

    parts = solution_filter.split( '|' )
    filter_configuration = parts[0].strip() if len( parts ) > 0 else '*'
    filter_platform = parts[1].strip() if len( parts ) > 1 else '*'

    if filter_configuration not in ( '*', configuration ):
        return False
    if filter_platform not in ( '*', platform ):
        return False

    return True

def parse_slnx( repo_root, solution_path = 'Esoterica.slnx', platform = None ):
    """Parses Esoterica.slnx and every .vcxproj it lists.

    Configuration names come from <Configurations>, never from a hardcoded list, so that an
    upstream configuration change needs no edit here.
    """

    absolute_path = repo_root / solution_path
    tree = ElementTree.parse( absolute_path )
    root = tree.getroot()

    solution = Solution( solution_path )

    configurations_element = root.find( 'Configurations' )
    if configurations_element is not None:
        for build_type in configurations_element.findall( 'BuildType' ):
            name = build_type.attrib.get( 'Name' )
            if name:
                solution.configurations.append( name )

        for platform_element in configurations_element.findall( 'Platform' ):
            name = platform_element.attrib.get( 'Name' )
            if name:
                solution.platforms.append( name )

    if not solution.configurations:
        raise RuntimeError( f'{solution_path}: no <BuildType> entries found.' )
    if not solution.platforms:
        raise RuntimeError( f'{solution_path}: no <Platform> entries found.' )

    if platform is None:
        platform = solution.platforms[0]

    for project_element in root.iter( 'Project' ):
        raw_path = project_element.attrib.get( 'Path' )
        if not raw_path:
            warn( f'{solution_path}: <Project> entry with no Path attribute. Skipped.' )
            continue

        project_path = msbuild_path_to_posix( raw_path )
        solution_project = SolutionProject( project_path )

        for build_element in project_element.findall( 'Build' ):
            builds = build_element.attrib.get( 'Project', 'true' ).strip().lower() != 'false'
            if builds:
                continue

            solution_filter = build_element.attrib.get( 'Solution' )
            if not solution_filter:
                solution_project.never_build = True
                continue

            for configuration in solution.configurations:
                if matches_solution_filter( solution_filter, configuration, platform ):
                    solution_project.excluded_configurations.add( configuration )

        for dependency_element in project_element.findall( 'BuildDependency' ):
            dependency_path = dependency_element.attrib.get( 'Project' )
            if dependency_path:
                solution_project.build_dependencies.append(
                    msbuild_path_to_posix( dependency_path ) )

        solution.solution_projects[project_path] = solution_project

        if not ( repo_root / project_path ).is_file():
            warn( f'{solution_path}: "{project_path}" is listed but does not exist. Skipped.' )
            continue

        project = parse_vcxproj( repo_root, project_path, solution.configurations, platform )
        project.solution_entry = solution_project
        solution.projects.append( project )

    # A project the generator cannot classify is the failure mode that hurts most on a future
    # upstream merge: it vanishes from the build with no error. Say so, every time.
    for project in solution.projects:
        if project.is_nmake_wrapper:
            warn( f'{project.path}: no ClCompile entries. Treated as an NMAKE wrapper project '
                  f'and not built directly.' )
            continue

        for configuration in solution.configurations:
            if not solution.solution_projects[project.path].builds_in( configuration ):
                continue
            project_type = project.get_project_type( configuration )
            if project_type == PROJECT_TYPE_NMAKE:
                continue
            if project_type is None:
                warn( f'{project.path}: no ConfigurationType for {configuration}|{platform}. '
                      f'The generator cannot tell what to produce, so it is not built.' )

    # Resolve project references onto parsed projects, so later stages work in names.
    known_paths = { project.path for project in solution.projects }
    for project in solution.projects:
        for reference_path in project.project_references:
            if reference_path not in known_paths:
                warn( f'{project.path}: references "{reference_path}", which Esoterica.slnx '
                      f'does not list.' )

    solution.platform = platform
    return solution

#-------------------------------------------------------------------------
# Entry point for inspection
#-------------------------------------------------------------------------

def find_repo_root():
    return Path( __file__ ).resolve().parents[3]

def dump( solution ):
    """Prints the parsed model. Used to check this file's output by hand."""

    print( f'solution:       {solution.path}' )
    print( f'platform:       {solution.platform}' )
    print( f'configurations: {", ".join( solution.configurations )}' )
    print()

    for project in sorted( solution.projects, key = lambda p: p.name ):
        entry = solution.solution_projects[project.path]

        print( f'{project.name}' )
        print( f'  path              {project.path}' )

        if project.is_nmake_wrapper:
            print( f'  nmake wrapper     yes (no ClCompile entries, not built)' )

        types = [ f'{c}={project.get_project_type( c ) or "-"}'
                  for c in solution.configurations ]
        print( f'  type              {" ".join( types )}' )

        if entry.never_build:
            print( f'  builds            never (<Build Project="false"/>)' )
        elif entry.excluded_configurations:
            built = [ c for c in solution.configurations if entry.builds_in( c ) ]
            print( f'  builds            {", ".join( built )}' )

        print( f'  c sources         {len( project.c_sources )}' )
        print( f'  cpp sources       {len( project.cpp_sources )}' )
        print( f'  autogenerated     {len( project.autogenerated_sources )}' )
        print( f'  headers           {len( project.headers )}' )
        print( f'  excluded          {len( project.excluded_sources )}' )

        for source_path in project.excluded_sources:
            print( f'                    - {source_path}' )

        linux_sources = [ s for s in project.c_sources + project.cpp_sources
                          if classify_platform( s )[0] == 'linux' ]
        if linux_sources:
            print( f'  linux sources     {len( linux_sources )}' )
            for source_path in linux_sources:
                print( f'                    + {source_path}' )

        if project.project_references:
            print( f'  references        {", ".join( project.project_references )}' )

        for configuration in solution.configurations:
            sheets = project.get_property_sheets( configuration )
            if sheets:
                print( f'  sheets {configuration:<10} {", ".join( sheets )}' )

        print()

def main():
    repo_root = find_repo_root()
    solution = parse_slnx( repo_root )
    dump( solution )

    warnings = get_warnings()
    print( f'{len( warnings )} warning(s).' )
    return 0

if __name__ == '__main__':
    sys.exit( main() )
