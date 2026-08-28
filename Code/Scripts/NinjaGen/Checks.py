"""The few build-generator checks the build itself cannot make.

    python3 Code/Scripts/NinjaGen/Checks.py

**The Linux build is the real test.** If a flag is wrong, an include path is missing, or a
source is absent, the compiler says so immediately and in more detail than any assertion here
would. Nothing in this file re-checks something a build would catch.

What is here is the opposite case: failures that leave a **green build** behind and only surface
much later, usually on an upstream merge.

  - the sync check no longer detects a changed .vcxproj
  - the generated ninja file is not deterministic, so it cannot be diffed
  - an exclusion glob has gone stale and silently readmits a file
  - the glob syntax means something other than what the lists assume
  - a new upstream property sheet has no Linux mapping
  - a .vcxproj or Esoterica.slnx got modified, against the prime directive
  - a flag that silently changes behaviour, such as -ffast-math, crept in

Add a check here only when a failure would be silent. Otherwise let the build find it.
"""

import subprocess
import sys

from pathlib import Path

sys.path.insert( 0, str( Path( __file__ ).resolve().parent ) )

import NinjaGen
import SourceLists
import SyncUpstream
import Toolchain

#-------------------------------------------------------------------------

_failures = []

def check( condition, description ):
    if condition:
        print( f'  pass  {description}' )
    else:
        print( f'  FAIL  {description}' )
        _failures.append( description )

#-------------------------------------------------------------------------

def test_glob_semantics():
    """A wrong glob silently includes or excludes sources, and the build may still succeed."""

    print( 'glob semantics' )

    cases = [
        ( '**/*_Win32.cpp', 'Code/Base/Threading/Platform/Threading_Win32.cpp', True ),
        ( '**/*_Win32.cpp', 'Code/Base/Threading/Threading.cpp', False ),
        ( '**/Win32/**', 'Code/Applications/Editor/Win32/Resource.h', True ),
        ( '**/Win32/**', 'Code/Applications/Editor/EditorApplication.cpp', False ),
        # '*' must not cross a directory separator, or '**' would mean nothing.
        ( 'Code/*/Foo.cpp', 'Code/Base/Foo.cpp', True ),
        ( 'Code/*/Foo.cpp', 'Code/Base/Render/Foo.cpp', False ),
    ]

    for pattern, path, expected in cases:
        actual = SourceLists.compile_glob( pattern ).match( path ) is not None
        check( actual == expected, f'{pattern!r} vs {path!r} is {expected}' )

def test_lists_are_healthy():
    """A stale exclusion glob stops excluding, and nothing about the build says so."""

    print( 'source lists' )

    solution, problems = SourceLists.load()
    check( problems == [], f'the three lists load with no problems ({problems})' )

    # Overlapping globs must both be credited, or the second reads as stale and gets deleted.
    globs = SourceLists.GlobList( [ '**/*_Win32.cpp', '**/Win32/**' ] )
    globs.matches( 'Code/Applications/Editor/Win32/EditorApplication_Win32.cpp' )
    check( globs.unused_patterns() == [], 'overlapping globs are both credited for a match' )

    stale = SourceLists.GlobList( [ '**/*_Durango.cpp' ] )
    stale.matches( 'Code/Base/Platform/Platform_Win32.cpp' )
    check( stale.unused_patterns() == [ '**/*_Durango.cpp' ],
           'a glob that matches nothing is reported' )

def test_sync_detects_drift():
    """The whole safety mechanism of the three-list design. A green build never exercises it."""

    print( 'upstream sync' )

    repo_root = SourceLists.find_repo_root()

    result = subprocess.run( [ sys.executable, 'Code/Scripts/NinjaGen/SyncUpstream.py' ],
                             cwd = repo_root, capture_output = True, text = True )
    check( result.returncode == 0,
           'UpstreamProjects.txt matches the Visual Studio projects' )

    solution = SyncUpstream.read_slnx( repo_root )
    committed = ( repo_root / SourceLists.SOURCE_LISTS_DIRECTORY /
                  SourceLists.UPSTREAM_PROJECTS_FILE ).read_text()

    check( SourceLists.format_upstream_projects( solution ) == committed,
           'a re-read of the .vcxproj files reproduces the committed list byte for byte' )

    solution.get_project( 'Esoterica.Base' ).upstream_sources.append( 'Render/RHI_Metal.cpp' )
    check( SourceLists.format_upstream_projects( solution ) != committed,
           'an added upstream source is detected, rather than joining the build unnoticed' )

def test_determinism():
    """Acceptance criterion 6. Building twice never reveals a non-deterministic build file."""

    print( 'determinism' )

    repo_root = SourceLists.find_repo_root()
    configurations = Toolchain.build_configurations( [ 'Debug', 'Release', 'Shipping' ] )

    first, _, _ = NinjaGen.emit( repo_root, SourceLists.load( repo_root )[0], configurations )
    second, _, _ = NinjaGen.emit( repo_root, SourceLists.load( repo_root )[0], configurations )

    check( first == second, 'two runs produce a byte-identical ninja file' )

def test_property_sheets_are_mapped():
    """An unmapped sheet surfaces much later as a link error that names nothing useful."""

    print( 'property sheets' )

    solution, _ = SourceLists.load()

    imported = set()
    for project in solution.projects:
        for configuration in solution.configurations:
            imported.update( project.get_property_sheets( configuration ) )

    unmapped = sorted( imported - set( Toolchain.SHEETS ) )
    check( unmapped == [], f'every imported property sheet has a Linux mapping ({unmapped})' )

    # LINUX_ONLY_SHEETS attaches a sheet by project name, so a typo in either name silently
    # contributes nothing. The build stays green until something needs the symbols, and the link
    # error then names a Vulkan function rather than the misspelling that caused it.
    project_names = { project.name for project in solution.projects }

    unknown_projects = sorted( set( Toolchain.LINUX_ONLY_SHEETS ) - project_names )
    check( unknown_projects == [],
           f'every LINUX_ONLY_SHEETS key names a real project ({unknown_projects})' )

    unknown_sheets = sorted( { name
                               for names in Toolchain.LINUX_ONLY_SHEETS.values()
                               for name in names } - set( Toolchain.SHEETS ) )
    check( unknown_sheets == [],
           f'every LINUX_ONLY_SHEETS value is in SHEETS ({unknown_sheets})' )

    # Conventions rule 4: a dropped middleware stays off by leaving its define unset, never by
    # editing a call site. None of these may contribute anything.
    for name in ( 'LivePP', 'SuperLuminal', 'NavPower', 'Optick' ):
        sheet = Toolchain.SHEETS[name]
        check( not sheet.defines and not sheet.libraries and not sheet.pkg_config,
               f'{name}.props contributes nothing, so its EE_ENABLE_* define stays unset' )

def test_silently_dangerous_flags():
    """These change behaviour without failing the build, so only a check catches them."""

    print( 'flags' )

    repo_root = SourceLists.find_repo_root()
    ninja_path = repo_root / NinjaGen.NINJA_FILE

    if not ninja_path.is_file():
        check( False, 'Build/Linux/Esoterica.ninja exists (run NinjaGen.py first)' )
        return

    text = ninja_path.read_text()
    check( '-ffast-math' not in text,
           '-ffast-math is never passed: Esoterica.props sets FloatingPointModel to Precise' )

def test_no_project_files_touched():
    """The prime directive, in one command."""

    print( 'read only' )

    repo_root = SourceLists.find_repo_root()
    result = subprocess.run(
        [ 'git', 'status', '--porcelain', '--', '*.vcxproj', 'Esoterica.slnx' ],
        cwd = repo_root, capture_output = True, text = True )

    check( result.stdout.strip() == '',
           f'no .vcxproj or .slnx file is modified ({result.stdout.strip()})' )

#-------------------------------------------------------------------------

def main():
    test_glob_semantics()
    test_lists_are_healthy()
    test_sync_detects_drift()
    test_determinism()
    test_property_sheets_are_mapped()
    test_silently_dangerous_flags()
    test_no_project_files_touched()

    print()
    if _failures:
        print( f'{len( _failures )} failure(s).' )
        return 1

    print( 'all checks passed.' )
    return 0

if __name__ == '__main__':
    sys.exit( main() )
