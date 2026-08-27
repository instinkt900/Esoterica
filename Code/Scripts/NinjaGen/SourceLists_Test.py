"""Checks for SourceLists.py and SyncUpstream.py.

    python3 Code/Scripts/NinjaGen/SourceLists_Test.py

No test framework, on purpose. The build generator runs with nothing but a Python 3 install, so
its checks do too.
"""

import subprocess
import sys

from pathlib import Path

sys.path.insert( 0, str( Path( __file__ ).resolve().parent ) )

import SourceLists
import SyncUpstream

#-------------------------------------------------------------------------

_failures = []

def check( condition, description ):
    if condition:
        print( f'  pass  {description}' )
    else:
        print( f'  FAIL  {description}' )
        _failures.append( description )

def check_equal( actual, expected, description ):
    if actual == expected:
        check( True, description )
    else:
        check( False, f'{description} (got {actual!r}, want {expected!r})' )

#-------------------------------------------------------------------------

def test_glob_matching():
    print( 'glob matching' )

    cases = [
        ( '**/*_Win32.cpp', 'Code/Base/Threading/Platform/Threading_Win32.cpp', True ),
        ( '**/*_Win32.cpp', 'Code/Base/Threading/Threading.cpp', False ),
        ( '**/*_Win32.cpp', 'Threading_Win32.cpp', True ),
        ( '**/Win32/**', 'Code/Applications/Editor/Win32/Resource.h', True ),
        ( '**/Win32/**', 'Code/Applications/Editor/EditorApplication.cpp', False ),
        ( 'Code/Base/Render/RHI_Direct3D12.cpp', 'Code/Base/Render/RHI_Direct3D12.cpp', True ),
        ( 'Code/Base/Render/RHI_Direct3D12.cpp', 'Code/Base/Render/RHI_Vulkan.cpp', False ),
        # '*' must not cross a directory separator, or '**' would mean nothing.
        ( 'Code/*/Foo.cpp', 'Code/Base/Foo.cpp', True ),
        ( 'Code/*/Foo.cpp', 'Code/Base/Render/Foo.cpp', False ),
    ]

    for pattern, path, expected in cases:
        actual = SourceLists.compile_glob( pattern ).match( path ) is not None
        check_equal( actual, expected, f'{pattern!r} vs {path!r}' )

def test_unused_glob_detection():
    print( 'unused glob detection' )

    globs = SourceLists.GlobList( [ '**/*_Win32.cpp', '**/*_Durango.cpp' ] )
    globs.matches( 'Code/Base/Platform/Platform_Win32.cpp' )

    check_equal( globs.unused_patterns(), [ '**/*_Durango.cpp' ],
                 'a glob that matches nothing is reported' )

    # Overlapping globs must both be credited, or the second reads as stale.
    overlapping = SourceLists.GlobList( [ '**/*_Win32.cpp', '**/Win32/**' ] )
    overlapping.matches( 'Code/Applications/Editor/Win32/EditorApplication_Win32.cpp' )
    check_equal( overlapping.unused_patterns(), [],
                 'overlapping globs are both credited for a match' )

def test_list_round_trip():
    print( 'UpstreamProjects.txt round trip' )

    text = """\
configurations Debug Release Shipping
platform x64

project Test.Project
  file Code/Test/Test.Project.vcxproj
  type Debug SharedLibrary
  type Shipping StaticLibrary
  skip Shipping
  ref Other.Project
  sheet Debug Esoterica ixWebSocket
  src Foo.cpp
  src Bar/Baz.cpp
"""

    solution = SourceLists.parse_upstream_projects( text )
    project = solution.get_project( 'Test.Project' )

    check_equal( solution.configurations, [ 'Debug', 'Release', 'Shipping' ],
                 'configurations parse' )
    check_equal( project.directory, 'Code/Test', 'the project directory comes from its file' )
    check_equal( project.get_project_type( 'Debug' ), 'SharedLibrary', 'per-config type parses' )
    check_equal( project.get_project_type( 'Release' ), None, 'a missing type stays missing' )
    check( not project.builds_in( 'Shipping' ), 'a skipped configuration does not build' )
    check_equal( project.references, [ 'Other.Project' ], 'references parse' )
    check_equal( project.get_property_sheets( 'Debug' ), [ 'Esoterica', 'ixWebSocket' ],
                 'property sheets parse' )
    check_equal( project.upstream_sources, [ 'Foo.cpp', 'Bar/Baz.cpp' ], 'sources parse' )

    # Determinism. Acceptance criterion 6 needs a stable ninja file, which needs stable input.
    reformatted = SourceLists.format_upstream_projects( solution )
    reparsed = SourceLists.parse_upstream_projects( reformatted )
    check_equal( SourceLists.format_upstream_projects( reparsed ), reformatted,
                 'format is a fixed point: write, read, write gives the same bytes' )

def test_linux_sources_format():
    print( 'LinuxSources.txt format' )

    parsed = SourceLists.parse_linux_sources( """\
# comment
[Esoterica.Base]
Platform/Platform_Linux.cpp   # trailing comment
Render/RHI_Vulkan.cpp

[Esoterica.Engine.Tools]
""" )

    check_equal( parsed.get( 'Esoterica.Base' ),
                 [ 'Platform/Platform_Linux.cpp', 'Render/RHI_Vulkan.cpp' ],
                 'sources group under their project, comments stripped' )
    check_equal( parsed.get( 'Esoterica.Engine.Tools' ), [],
                 'an empty section is allowed' )

#-------------------------------------------------------------------------

def test_real_lists():
    print( 'the committed lists' )

    repo_root = SourceLists.find_repo_root()
    solution, problems = SourceLists.load( repo_root )

    check_equal( problems, [], 'the three lists load with no problems' )
    check_equal( solution.configurations, [ 'Debug', 'Release', 'Shipping' ],
                 'configurations come from the .slnx, through the sync' )

    base = solution.get_project( 'Esoterica.Base' )
    check( base is not None, 'Esoterica.Base is present' )

    # Acceptance criterion 4. ConfigurationType varies by configuration, and the stale upstream
    # generator read it once and got this wrong.
    check_equal( base.get_project_type( 'Debug' ), SourceLists.PROJECT_TYPE_SHARED_LIBRARY,
                 'Esoterica.Base is a shared library in Debug' )
    check_equal( base.get_project_type( 'Release' ), SourceLists.PROJECT_TYPE_SHARED_LIBRARY,
                 'Esoterica.Base is a shared library in Release' )
    check_equal( base.get_project_type( 'Shipping' ), SourceLists.PROJECT_TYPE_STATIC_LIBRARY,
                 'Esoterica.Base is a static library in Shipping' )

    # Acceptance criterion 2, and the two bugs the heuristic version shipped with.
    check_equal( [ s for s in base.sources if '_Win32' in s ], [],
                 'no _Win32 source reaches the build' )
    check( 'Code/Base/Render/RHI_Direct3D12.cpp' not in base.sources,
           'the Direct3D 12 backend is excluded' )
    check( not any( 'D3D12MemoryAllocator' in s for s in base.sources ),
           'the Direct3D 12 memory allocator is excluded' )

    # Every excluded source must be a real file. An exclusion for a file that no longer exists
    # is stale, and `load` reports it, but check the count adds up too.
    check_equal( len( base.upstream_sources ) + len( base.excluded_sources ), 147,
                 'kept plus excluded accounts for every ClCompile entry in Esoterica.Base' )

    editor = solution.get_project( 'Esoterica.Applications.Editor' )
    check( editor.builds_in( 'Debug' ), 'the Editor builds in Debug' )
    check( not editor.builds_in( 'Shipping' ),
           'the Editor is excluded from Shipping by the .slnx' )

    reflect = solution.get_project( 'Esoterica.Scripts.Reflect' )
    check_equal( reflect.get_project_type( 'Debug' ), SourceLists.PROJECT_TYPE_NMAKE,
                 'the Reflect wrapper is an NMAKE project' )
    check( not reflect.builds_in( 'Debug' ), 'the Reflect wrapper is never built directly' )

    check( 'ixWebSocket' in base.get_property_sheets( 'Debug' ),
           'Esoterica.Base imports ixWebSocket.props in Debug' )
    check( 'ixWebSocket' not in base.get_property_sheets( 'Shipping' ),
           'Esoterica.Base does not import ixWebSocket.props in Shipping' )

def test_sync_is_current():
    """The check that runs on every build. It is the whole reason the lists are safe."""

    print( 'sync check' )

    repo_root = SourceLists.find_repo_root()
    result = subprocess.run(
        [ sys.executable, 'Code/Scripts/NinjaGen/SyncUpstream.py' ],
        cwd = repo_root, capture_output = True, text = True )

    check_equal( result.returncode, 0,
                 'UpstreamProjects.txt matches the Visual Studio projects' )

def test_sync_detects_drift():
    """A new upstream source must stop the build, not join it unnoticed."""

    print( 'sync drift detection' )

    repo_root = SourceLists.find_repo_root()
    solution = SyncUpstream.read_slnx( repo_root )

    base = solution.get_project( 'Esoterica.Base' )
    base.upstream_sources.append( 'Render/RHI_Metal.cpp' )

    drifted = SourceLists.format_upstream_projects( solution )
    committed = ( repo_root / SourceLists.SOURCE_LISTS_DIRECTORY /
                  SourceLists.UPSTREAM_PROJECTS_FILE ).read_text()

    check( drifted != committed, 'an added upstream source changes the generated list' )
    check( 'RHI_Metal' in drifted, 'the new source appears in the generated list' )

def test_no_project_files_touched():
    print( 'read only' )

    repo_root = SourceLists.find_repo_root()
    result = subprocess.run(
        [ 'git', 'status', '--porcelain', '--', '*.vcxproj', 'Esoterica.slnx' ],
        cwd = repo_root, capture_output = True, text = True )

    check_equal( result.stdout.strip(), '', 'no .vcxproj or .slnx file is modified' )

#-------------------------------------------------------------------------

def main():
    test_glob_matching()
    test_unused_glob_detection()
    test_list_round_trip()
    test_linux_sources_format()
    test_real_lists()
    test_sync_is_current()
    test_sync_detects_drift()
    test_no_project_files_touched()

    print()
    if _failures:
        print( f'{len( _failures )} failure(s).' )
        return 1

    print( 'all checks passed.' )
    return 0

if __name__ == '__main__':
    sys.exit( main() )
