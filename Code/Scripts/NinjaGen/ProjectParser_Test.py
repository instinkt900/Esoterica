"""Checks for ProjectParser.py. Run with: python3 Code/Scripts/NinjaGen/ProjectParser_Test.py

No test framework, on purpose. The build generator must run with nothing but a Python 3
install, so its checks do too.

The first group tests the platform filtering rules in isolation. The second group parses the
real Esoterica.slnx and asserts facts that must hold for the Linux build to be correct.
"""

import sys

from pathlib import Path

sys.path.insert( 0, str( Path( __file__ ).resolve().parent ) )

import ProjectParser

#-------------------------------------------------------------------------

_failures = []

def check( condition, description ):
    if condition:
        print( f'  pass  {description}' )
    else:
        print( f'  FAIL  {description}' )
        _failures.append( description )

def check_equal( actual, expected, description ):
    check( actual == expected, f'{description} (got {actual!r}, want {expected!r})'
           if actual != expected else description )

#-------------------------------------------------------------------------

def test_platform_classification():
    print( 'platform classification' )

    check_equal( ProjectParser.classify_platform( 'Code/Base/Threading/Platform/Threading_Win32.cpp' )[0],
                 'windows', 'a _Win32 suffix is windows' )
    check_equal( ProjectParser.classify_platform( 'Code/Base/Threading/Platform/Threading_Linux.cpp' )[0],
                 'linux', 'a _Linux suffix is linux' )
    check_equal( ProjectParser.classify_platform( 'Code/Applications/Editor/Win32/Resource.h' )[0],
                 'windows', 'a Win32/ directory is windows' )
    check_equal( ProjectParser.classify_platform( 'Code/Base/Threading/Threading.cpp' )[0],
                 None, 'a plain source is platform neutral' )
    check_equal( ProjectParser.classify_platform( 'Code/Base/Network/Clients/NetworkClient_GNS.cpp' )[0],
                 None, 'a non-platform underscore suffix is neutral' )
    check_equal( ProjectParser.classify_platform( 'Code/Base/Foo/Foo_Mac.cpp' )[0],
                 'other', 'an unsupported platform suffix is flagged, not dropped silently' )

def test_platform_filtering():
    print( 'platform filtering' )

    ProjectParser.clear_warnings()

    sources = [
        'Code/Base/Threading/Threading.cpp',
        'Code/Base/Threading/Platform/Threading_Win32.cpp',
        'Code/Base/Threading/Platform/Threading_Linux.cpp',
        'Code/Base/Foo/Foo_Durango.cpp',
        'Code/Base/ThirdParty/pugixml/scripts/pugixml_dll.rc',
    ]

    kept, dropped = ProjectParser.filter_platform_sources( sources )

    check_equal( kept, [ 'Code/Base/Threading/Threading.cpp',
                         'Code/Base/Threading/Platform/Threading_Linux.cpp' ],
                 'neutral and linux sources are kept, in order' )
    check( 'Code/Base/Threading/Platform/Threading_Win32.cpp' in dropped,
           'windows sources are dropped' )
    check( 'Code/Base/ThirdParty/pugixml/scripts/pugixml_dll.rc' in dropped,
           'resource scripts are dropped' )
    check_equal( len( ProjectParser.get_warnings() ), 1,
                 'an unsupported platform source warns exactly once' )

def test_solution_filter():
    print( 'solution build filters' )

    check( ProjectParser.matches_solution_filter( 'Shipping|*', 'Shipping', 'x64' ),
           'Shipping|* matches Shipping' )
    check( not ProjectParser.matches_solution_filter( 'Shipping|*', 'Debug', 'x64' ),
           'Shipping|* does not match Debug' )
    check( ProjectParser.matches_solution_filter( '*|x64', 'Debug', 'x64' ),
           '*|x64 matches any configuration on x64' )
    check( not ProjectParser.matches_solution_filter( '*|Win32', 'Debug', 'x64' ),
           '*|Win32 does not match x64' )
    check( ProjectParser.matches_solution_filter( None, 'Debug', 'x64' ),
           'no filter matches everything' )

#-------------------------------------------------------------------------

def test_real_solution():
    print( 'Esoterica.slnx' )

    ProjectParser.clear_warnings()
    repo_root = ProjectParser.find_repo_root()
    solution = ProjectParser.parse_slnx( repo_root )

    check_equal( solution.configurations, [ 'Debug', 'Release', 'Shipping' ],
                 'configurations come from <Configurations>' )
    check_equal( solution.platform, 'x64', 'platform comes from <Platform>' )

    base = solution.get_project_by_name( 'Esoterica.Base' )
    check( base is not None, 'Esoterica.Base is parsed' )

    # Acceptance criterion 4 in Docs/Linux/Phases/Phase0-BuildSystem.md. The stale generator read
    # ConfigurationType once and got this wrong.
    check_equal( base.get_project_type( 'Debug' ), ProjectParser.PROJECT_TYPE_SHARED_LIBRARY,
                 'Esoterica.Base is a shared library in Debug' )
    check_equal( base.get_project_type( 'Release' ), ProjectParser.PROJECT_TYPE_SHARED_LIBRARY,
                 'Esoterica.Base is a shared library in Release' )
    check_equal( base.get_project_type( 'Shipping' ), ProjectParser.PROJECT_TYPE_STATIC_LIBRARY,
                 'Esoterica.Base is a static library in Shipping' )

    # Acceptance criterion 2.
    check_equal( [ s for s in base.cpp_sources if '_Win32' in s ], [],
                 'no _Win32 source survives filtering' )
    check( len( base.cpp_sources ) > 100, 'Esoterica.Base keeps its neutral sources' )

    editor = solution.get_project_by_name( 'Esoterica.Applications.Editor' )
    editor_entry = solution.solution_projects[editor.path]
    check( editor_entry.builds_in( 'Debug' ), 'the Editor builds in Debug' )
    check( not editor_entry.builds_in( 'Shipping' ),
           '<Build Solution="Shipping|*" Project="false"/> excludes the Editor from Shipping' )
    check_equal( len( editor_entry.build_dependencies ), 2,
                 'the Editor keeps its two <BuildDependency> edges' )

    reflect = solution.get_project_by_name( 'Esoterica.Scripts.Reflect' )
    check( reflect.is_nmake_wrapper,
           'the Reflect project is detected as an NMAKE wrapper by having no ClCompile entries' )
    check( solution.solution_projects[reflect.path].never_build,
           '<Build Project="false"/> marks the Reflect project as never built' )

    engine = solution.get_project_by_name( 'Esoterica.Engine.Runtime' )
    check( 'Code/Base/Esoterica.Base.vcxproj' in engine.project_references,
           'project references resolve to repo-relative paths' )

    # Property sheets decide which external libraries a target links, and they vary by
    # configuration: Esoterica.Base drops ixWebSocket in Shipping.
    check( 'ixWebSocket' in base.get_property_sheets( 'Debug' ),
           'Esoterica.Base imports ixWebSocket.props in Debug' )
    check( 'ixWebSocket' not in base.get_property_sheets( 'Shipping' ),
           'Esoterica.Base does not import ixWebSocket.props in Shipping' )

    # Determinism. Acceptance criterion 6 needs a stable ninja file, which needs stable input.
    second = ProjectParser.parse_slnx( repo_root )
    check_equal( [ p.all_sources for p in solution.projects ],
                 [ p.all_sources for p in second.projects ],
                 'two parses of the same tree produce the same source lists' )

def test_no_project_files_touched():
    print( 'read only' )

    import subprocess

    repo_root = ProjectParser.find_repo_root()
    result = subprocess.run( [ 'git', 'status', '--porcelain', '--',
                               '*.vcxproj', 'Esoterica.slnx' ],
                             cwd = repo_root, capture_output = True, text = True )

    check_equal( result.stdout.strip(), '',
                 'no .vcxproj or .slnx file is modified' )

#-------------------------------------------------------------------------

def main():
    test_platform_classification()
    test_platform_filtering()
    test_solution_filter()
    test_real_solution()
    test_no_project_files_touched()

    print()
    if _failures:
        print( f'{len( _failures )} failure(s).' )
        return 1

    print( 'all checks passed.' )
    return 0

if __name__ == '__main__':
    sys.exit( main() )
