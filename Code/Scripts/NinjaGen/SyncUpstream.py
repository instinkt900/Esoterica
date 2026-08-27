"""Syncs UpstreamProjects.txt from Esoterica.slnx and the .vcxproj files.

This is the only thing in the Linux build that reads the Visual Studio project files, and it
runs when upstream changes, not on every build.

    python3 Code/Scripts/NinjaGen/SyncUpstream.py            check. Exits 1 if the list is stale.
    python3 Code/Scripts/NinjaGen/SyncUpstream.py --update    rewrite the list.

The generator runs the check on every build. Parsing twelve XML files costs milliseconds, and it
means a new upstream source stops the build until somebody classifies it, instead of joining the
build unnoticed. That is the one thing deriving the list live was good for, and it is kept.

Nothing here writes to a .vcxproj file or to Esoterica.slnx. Reading only.
"""

import argparse
import difflib
import os
import sys
import xml.etree.ElementTree as ElementTree

from pathlib import Path

sys.path.insert( 0, str( Path( __file__ ).resolve().parent ) )

import SourceLists

#-------------------------------------------------------------------------

MSBUILD_NAMESPACE = 'http://schemas.microsoft.com/developer/msbuild/2003'

CONFIGURATION_TYPE_MAP = {
    'DynamicLibrary': SourceLists.PROJECT_TYPE_SHARED_LIBRARY,
    'StaticLibrary':  SourceLists.PROJECT_TYPE_STATIC_LIBRARY,
    'Application':    SourceLists.PROJECT_TYPE_EXECUTABLE,
    'Makefile':       SourceLists.PROJECT_TYPE_NMAKE,
}

SOURCE_EXTENSIONS = SourceLists.C_SOURCE_EXTENSIONS + SourceLists.CPP_SOURCE_EXTENSIONS

CONFIGURATION_CONDITION = __import__( 're' ).compile(
    r"'\$\(Configuration\)\|\$\(Platform\)'\s*==\s*'([^|']+)\|([^']+)'" )

#-------------------------------------------------------------------------

_warnings = []

def warn( message ):
    _warnings.append( message )
    print( f'warning: {message}', file = sys.stderr )

#-------------------------------------------------------------------------

def strip_namespace( tag ):
    return tag.split( '}', 1 )[-1] if tag.startswith( '{' ) else tag

def msbuild_path_to_posix( value ):
    return value.replace( '\\', '/' )

def parse_configuration_condition( condition ):
    if not condition:
        return None, None

    match = CONFIGURATION_CONDITION.search( condition )
    if match is None:
        return None, None

    return match.group( 1 ), match.group( 2 )

def resolve_file_ignoring_case( repo_root, relative_path ):
    """Finds a listed source on a case-sensitive filesystem.

    MSBuild ignores case, so a .vcxproj entry can disagree with the file on disk. Upstream has
    several. Correcting them here, once at sync time, keeps the correction out of the build and
    out of the .vcxproj files, which this port does not edit.
    """

    if ( repo_root / relative_path ).is_file():
        return relative_path

    path = Path( relative_path )
    directory = SourceLists.resolve_directory_ignoring_case( repo_root, path.parent.parts )
    if directory is None:
        return None

    for entry in sorted( directory.iterdir() ):
        if entry.is_file() and entry.name.lower() == path.name.lower():
            return entry.relative_to( repo_root ).as_posix()

    return None

def matches_solution_filter( solution_filter, configuration, platform ):
    """Matches a <Build Solution="Shipping|*"> filter. '*' is a wildcard on either side."""

    if not solution_filter:
        return True

    parts = solution_filter.split( '|' )
    filter_configuration = parts[0].strip() if len( parts ) > 0 else '*'
    filter_platform = parts[1].strip() if len( parts ) > 1 else '*'

    return ( filter_configuration in ( '*', configuration ) and
             filter_platform in ( '*', platform ) )

#-------------------------------------------------------------------------

def read_vcxproj( repo_root, project_path, platform ):
    """Reads one .vcxproj into a SourceLists.Project."""

    project_directory = Path( project_path ).parent
    root = ElementTree.parse( repo_root / project_path ).getroot()

    project = SourceLists.Project( Path( project_path ).stem )
    project.project_file = Path( project_path ).as_posix()
    project.directory = str( project_directory.as_posix() )

    sources = []

    for element in root.iter():
        tag = strip_namespace( element.tag )

        if tag == 'ProjectName' and element.text:
            project.name = element.text.strip()

        elif tag == 'ClCompile' and 'Include' in element.attrib:
            include = msbuild_path_to_posix( element.attrib['Include'] )
            if Path( include ).suffix.lower() in SOURCE_EXTENSIONS:
                sources.append( include )
            else:
                warn( f'{project_path}: unrecognised source extension on "{include}". Skipped.' )

        elif tag == 'ProjectReference' and 'Include' in element.attrib:
            reference = project_directory / msbuild_path_to_posix( element.attrib['Include'] )
            project.references.append( Path( os.path.normpath( reference ) ).stem )

    # ClInclude entries are deliberately ignored. The headers are only needed as the REFLECT
    # rule's dependency set, and a glob gives that without a list anybody has to maintain.

    # ConfigurationType varies by configuration: Esoterica.Base is a DynamicLibrary in Debug and
    # Release and a StaticLibrary in Shipping. Read it per configuration.
    for property_group in root.iter( f'{{{MSBUILD_NAMESPACE}}}PropertyGroup' ):
        configuration, condition_platform = parse_configuration_condition(
            property_group.attrib.get( 'Condition' ) )
        if configuration is None or condition_platform != platform:
            continue

        type_element = property_group.find( f'{{{MSBUILD_NAMESPACE}}}ConfigurationType' )
        if type_element is None or not type_element.text:
            continue

        configuration_type = type_element.text.strip()
        mapped = CONFIGURATION_TYPE_MAP.get( configuration_type )

        if mapped is None:
            warn( f'{project_path}: unknown ConfigurationType "{configuration_type}" '
                  f'for {configuration}.' )
            continue

        project.configuration_types[configuration] = mapped

    # Property sheet imports vary by configuration too. Esoterica.Base drops ixWebSocket.props
    # in Shipping. The sheets decide which external libraries a target links.
    for import_group in root.iter( f'{{{MSBUILD_NAMESPACE}}}ImportGroup' ):
        configuration, condition_platform = parse_configuration_condition(
            import_group.attrib.get( 'Condition' ) )
        if configuration is None or condition_platform != platform:
            continue

        sheets = project.property_sheets.setdefault( configuration, [] )
        for import_element in import_group.iter( f'{{{MSBUILD_NAMESPACE}}}Import' ):
            import_path = msbuild_path_to_posix( import_element.attrib.get( 'Project', '' ) )
            if 'PropertySheets' not in import_path:
                continue

            sheet_name = Path( import_path ).stem
            if sheet_name not in sheets:
                sheets.append( sheet_name )

    # Correct the case of every source, and drop any that no file matches. A path that matches
    # nothing is a stale .vcxproj entry, and keeping it would only make ninja fail with
    # 'missing and no known rule to make it'.
    resolved = []

    for source in sources:
        repo_path = ( project_directory / source ).as_posix()
        real_path = resolve_file_ignoring_case( repo_root, repo_path )

        if real_path is None:
            warn( f'{project_path}: "{source}" is listed but does not exist on disk. Dropped.' )
            continue

        if real_path != repo_path:
            warn( f'{project_path}: "{source}" is listed, but the file on disk is '
                  f'"{Path( real_path ).relative_to( project_directory ).as_posix()}". '
                  f'Using the name on disk.' )

        resolved.append( Path( real_path ).relative_to( project_directory ).as_posix() )

    project.upstream_sources = sorted( set( resolved ) )
    return project

def read_slnx( repo_root, solution_path = 'Esoterica.slnx' ):
    """Reads Esoterica.slnx and every .vcxproj it lists."""

    root = ElementTree.parse( repo_root / solution_path ).getroot()
    solution = SourceLists.Solution()

    configurations_element = root.find( 'Configurations' )
    if configurations_element is None:
        raise RuntimeError( f'{solution_path}: no <Configurations> element.' )

    for build_type in configurations_element.findall( 'BuildType' ):
        if build_type.attrib.get( 'Name' ):
            solution.configurations.append( build_type.attrib['Name'] )

    platforms = [ p.attrib['Name'] for p in configurations_element.findall( 'Platform' )
                  if p.attrib.get( 'Name' ) ]

    if not solution.configurations:
        raise RuntimeError( f'{solution_path}: no <BuildType> entries.' )
    if not platforms:
        raise RuntimeError( f'{solution_path}: no <Platform> entries.' )

    solution.platform = platforms[0]

    for project_element in root.iter( 'Project' ):
        raw_path = project_element.attrib.get( 'Path' )
        if not raw_path:
            warn( f'{solution_path}: <Project> entry with no Path attribute. Skipped.' )
            continue

        project_path = msbuild_path_to_posix( raw_path )

        if not ( repo_root / project_path ).is_file():
            warn( f'{solution_path}: "{project_path}" is listed but does not exist. Skipped.' )
            continue

        project = read_vcxproj( repo_root, project_path, solution.platform )

        for build_element in project_element.findall( 'Build' ):
            if build_element.attrib.get( 'Project', 'true' ).strip().lower() != 'false':
                continue

            solution_filter = build_element.attrib.get( 'Solution' )

            for configuration in solution.configurations:
                if matches_solution_filter( solution_filter, configuration, solution.platform ):
                    project.skipped_configurations.add( configuration )

        solution.projects.append( project )

    return solution

#-------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser( description = __doc__ )
    parser.add_argument( '--update', action = 'store_true',
                         help = f'rewrite {SourceLists.UPSTREAM_PROJECTS_FILE}' )
    arguments = parser.parse_args()

    repo_root = SourceLists.find_repo_root()
    list_path = ( repo_root / SourceLists.SOURCE_LISTS_DIRECTORY /
                  SourceLists.UPSTREAM_PROJECTS_FILE )

    solution = read_slnx( repo_root )
    generated = SourceLists.format_upstream_projects( solution )

    if arguments.update:
        list_path.parent.mkdir( parents = True, exist_ok = True )
        list_path.write_text( generated )
        print( f'wrote {list_path.relative_to( repo_root )}: '
               f'{len( solution.projects )} projects, '
               f'{sum( len( p.upstream_sources ) for p in solution.projects )} sources.' )
        return 0

    if not list_path.is_file():
        print( f'error: {SourceLists.UPSTREAM_PROJECTS_FILE} does not exist. '
               f'Run SyncUpstream.py --update.', file = sys.stderr )
        return 1

    current = list_path.read_text()

    if current == generated:
        print( f'{SourceLists.UPSTREAM_PROJECTS_FILE} is up to date.' )
        return 0

    diff = difflib.unified_diff(
        current.splitlines( keepends = True ), generated.splitlines( keepends = True ),
        fromfile = f'{SourceLists.UPSTREAM_PROJECTS_FILE} (committed)',
        tofile = f'{SourceLists.UPSTREAM_PROJECTS_FILE} (from the Visual Studio projects)' )

    sys.stderr.writelines( diff )
    print( f'\nerror: {SourceLists.UPSTREAM_PROJECTS_FILE} is stale. The Visual Studio projects '
           f'have changed.\n'
           f'       Run: python3 Code/Scripts/NinjaGen/SyncUpstream.py --update\n'
           f'       Then review the diff, and update Exclusions.txt and LinuxSources.txt if new '
           f'sources need it.', file = sys.stderr )
    return 1

if __name__ == '__main__':
    sys.exit( main() )
