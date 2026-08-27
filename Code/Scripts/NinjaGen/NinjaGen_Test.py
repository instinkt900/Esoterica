"""Checks for NinjaGen.py and Toolchain.py.

    python3 Code/Scripts/NinjaGen/NinjaGen_Test.py

Several of these are the Phase 0 acceptance criteria, written as checks so that they can be run
rather than asserted. The criterion each one covers is named in its description.
"""

import json
import subprocess
import sys

from pathlib import Path

sys.path.insert( 0, str( Path( __file__ ).resolve().parent ) )

import NinjaGen
import SourceLists
import Toolchain

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

def test_defines():
    print( 'define formatting' )

    check_equal( NinjaGen.format_define( 'EE_DEBUG=1' ), '-DEE_DEBUG=1', 'a plain define' )
    check_equal( NinjaGen.format_define( ( 'FOO', 'bar' ) ), '-DFOO=bar',
                 'a define with a simple value needs no quoting' )

    # EASTL does `#include EASTL_USER_CONFIG_HEADER`, so the double quotes are part of the macro
    # and have to reach the compiler intact.
    formatted = NinjaGen.format_define( ( 'EASTL_USER_CONFIG_HEADER', '"a/b.h"' ) )
    check_equal( formatted, '-DEASTL_USER_CONFIG_HEADER=\'"a/b.h"\'',
                 'a define whose value contains quotes is shell quoted' )

    check_equal( Toolchain.project_define( 'Esoterica.Base' ), 'ESOTERICA_BASE',
                 'the project define matches $(ProjectName.ToUpper().Replace(".","_"))' )
    check_equal( Toolchain.project_define( 'Esoterica.Applications.Editor' ),
                 'ESOTERICA_APPLICATIONS_EDITOR', 'and for a three-part project name' )

def test_configurations():
    print( 'configurations' )

    configurations = Toolchain.build_configurations( [ 'Debug', 'Release', 'Shipping' ] )
    names = [ c.name for c in configurations ]

    check( 'Debug' in names and 'Release' in names and 'Shipping' in names,
           'the three solution configurations are present' )
    check( 'Debug_ASan' in names and 'Debug_TSan' in names and 'Debug_UBSan' in names,
           'Debug has ASan, TSan and UBSan variants' )
    check( not any( 'MSan' in name for name in names ),
           'MSan is not emitted: it needs an instrumented libc++ to be usable' )
    check( not any( name.startswith( 'Shipping_' ) for name in names ),
           'Shipping has no sanitizer variants' )

    by_name = { c.name: c for c in configurations }
    check( 'EE_DLL' in by_name['Debug'].defines, 'Debug defines EE_DLL' )
    check( 'EE_DLL' in by_name['Release'].defines, 'Release defines EE_DLL' )
    check( 'EE_DLL' not in by_name['Shipping'].defines,
           'Shipping does not define EE_DLL, because it links statically' )
    check( '-flto' in by_name['Shipping'].compiler_flags,
           'Shipping compiles with -flto (WholeProgramOptimization)' )
    check( '-O0' in by_name['Debug'].compiler_flags, 'Debug compiles with -O0' )

def test_sheets_are_all_mapped():
    print( 'property sheet coverage' )

    solution, _ = SourceLists.load()

    imported = set()
    for project in solution.projects:
        for configuration in solution.configurations:
            imported.update( project.get_property_sheets( configuration ) )

    unmapped = sorted( imported - set( Toolchain.SHEETS ) )
    check_equal( unmapped, [],
                 'every property sheet a .vcxproj imports has a Linux mapping' )

    # Conventions rule 4: a dropped middleware stays off by leaving its define unset, never by
    # editing a call site. Nothing here may contribute an EE_ENABLE_* define.
    for name in ( 'LivePP', 'SuperLuminal', 'NavPower', 'Optick' ):
        sheet = Toolchain.SHEETS[name]
        check( not sheet.defines and not sheet.libraries and not sheet.pkg_config,
               f'{name}.props contributes nothing, so its EE_ENABLE_* define stays unset' )

def test_topological_order():
    print( 'link order' )

    solution, _ = SourceLists.load()
    editor = solution.get_project( 'Esoterica.Applications.Editor' )
    ordered = [ p.name for p in NinjaGen.topological_order( solution, editor ) ]

    check( 'Esoterica.Base' in ordered, 'the Editor depends on Esoterica.Base' )
    check( ordered.index( 'Esoterica.Engine.Tools' ) < ordered.index( 'Esoterica.Base' ),
           'a dependency comes after what needs it, which is what static linking requires' )
    check_equal( len( ordered ), len( set( ordered ) ),
                 'the dependency list has no duplicates' )

def test_output_paths():
    print( 'output paths' )

    solution, _ = SourceLists.load()
    configurations = { c.name: c for c in
                       Toolchain.build_configurations( solution.configurations ) }
    base = solution.get_project( 'Esoterica.Base' )

    # Acceptance criterion 4.
    check_equal( NinjaGen.target_path( base, configurations['Debug'] ),
                 'Build/Linux_Debug/libEsoterica.Base.so',
                 'Esoterica.Base is a .so in Debug' )
    check_equal( NinjaGen.target_path( base, configurations['Release'] ),
                 'Build/Linux_Release/libEsoterica.Base.so',
                 'Esoterica.Base is a .so in Release' )
    check_equal( NinjaGen.target_path( base, configurations['Shipping'] ),
                 'Build/Linux_Shipping/libEsoterica.Base.a',
                 'Esoterica.Base is a .a in Shipping' )

    engine = solution.get_project( 'Esoterica.Applications.Engine' )
    check_equal( NinjaGen.target_path( engine, configurations['Debug'] ),
                 'Build/Linux_Debug/Esoterica.Applications.Engine',
                 'an application has no lib prefix or extension' )

    # The stale upstream generator embedded absolute source paths in object paths, which makes
    # the build directory useless on any other machine.
    object_file = NinjaGen.object_path( base, configurations['Debug'],
                                        'Code/Base/Threading/Threading.cpp' )
    check_equal( object_file,
                 'Build/_Temp/Linux_Debug/Esoterica.Base/Code/Base/Threading/Threading.cpp.o',
                 'object paths are repo-relative' )
    check( not Path( object_file ).is_absolute(), 'object paths are never absolute' )

#-------------------------------------------------------------------------

def read_ninja_file():
    repo_root = SourceLists.find_repo_root()
    return ( repo_root / NinjaGen.NINJA_FILE ).read_text()

def test_generated_file():
    print( 'the generated ninja file' )

    repo_root = SourceLists.find_repo_root()
    ninja_path = repo_root / NinjaGen.NINJA_FILE

    if not ninja_path.is_file():
        check( False, 'Build/Linux/Esoterica.ninja exists (run NinjaGen.py first)' )
        return

    text = ninja_path.read_text()

    # Acceptance criterion 3. ninja_syntax wraps long lines, so the flags are checked against
    # the rule's whole block rather than a single line.
    check( 'rule cxx_Esoterica_Base_Debug\n' in text,
           'there is a C++ rule for Esoterica.Base in Debug' )

    start = text.index( 'rule cxx_Esoterica_Base_Debug' )
    rule_text = text[start:start + 2000]

    for flag in ( '-DESOTERICA_BASE', '-DEE_DLL', '-ICode', '-fno-exceptions',
                  '-fvisibility=hidden' ):
        check( flag in rule_text, f'the Esoterica.Base Debug rule passes {flag}' )
    check( '$cpp_std' in rule_text and '-std=c++20' in text,
           'the C++ rules use -std=c++20' )
    check( '-std=c17' in text, 'the C rules use -std=c17' )
    check( '-ffast-math' not in text,
           '-ffast-math is never passed: FloatingPointModel is Precise' )
    check( '-Werror' not in text,
           '-Werror is not passed in Phase 0, per the phase document' )

    # Acceptance criterion 2.
    check( '_Win32.cpp' not in text, 'no _Win32 source appears in the build file' )
    check( 'RHI_Direct3D12' not in text, 'the Direct3D 12 backend does not appear' )

    check( "-Wl,-rpath,'$$ORIGIN'" in text,
           "executables get -Wl,-rpath,'$ORIGIN' so they find sibling .so files" )

def test_ninja_parses():
    print( 'ninja accepts the file' )

    repo_root = SourceLists.find_repo_root()
    result = subprocess.run( [ 'ninja', '-f', NinjaGen.NINJA_FILE, '-t', 'targets', 'all' ],
                             cwd = repo_root, capture_output = True, text = True )

    check_equal( result.returncode, 0,
                 f'ninja parses the generated file ({result.stderr.strip()[:120]})' )

def test_determinism():
    """Acceptance criterion 6. A non-deterministic build file cannot be diffed."""

    print( 'determinism' )

    repo_root = SourceLists.find_repo_root()
    solution, _ = SourceLists.load( repo_root )
    configurations = Toolchain.build_configurations( solution.configurations )

    first, _, _ = NinjaGen.emit( repo_root, solution, configurations )

    solution, _ = SourceLists.load( repo_root )
    second, _, _ = NinjaGen.emit( repo_root, solution, configurations )

    check( first == second, 'two runs produce a byte-identical ninja file' )
    check( first == read_ninja_file(),
           'the committed-to-disk file matches a fresh generation' )

def test_compile_commands():
    """Acceptance criterion 5."""

    print( 'compile_commands.json' )

    repo_root = SourceLists.find_repo_root()
    path = repo_root / 'compile_commands.json'

    if not path.is_file():
        check( False, 'compile_commands.json exists (run NinjaGen.py first)' )
        return

    entries = json.loads( path.read_text() )
    check( len( entries ) > 100, f'it has an entry per source ({len( entries )})' )

    commands = [ e['command'] for e in entries ]
    check( not any( 'clang-cl' in c for c in commands ),
           'it uses the real Linux toolchain, not clang-cl' )
    check( all( '-ICode' in c for c in commands ),
           'every entry can resolve #include "Base/Esoterica.h" through -ICode' )
    check( any( 'Esoterica.h' in e['file'] or 'Base/' in e['file'] for e in entries ),
           'Esoterica.Base sources are present' )

def test_no_project_files_touched():
    print( 'read only' )

    repo_root = SourceLists.find_repo_root()
    result = subprocess.run(
        [ 'git', 'status', '--porcelain', '--', '*.vcxproj', 'Esoterica.slnx' ],
        cwd = repo_root, capture_output = True, text = True )

    check_equal( result.stdout.strip(), '', 'no .vcxproj or .slnx file is modified' )

#-------------------------------------------------------------------------

def main():
    test_defines()
    test_configurations()
    test_sheets_are_all_mapped()
    test_topological_order()
    test_output_paths()
    test_generated_file()
    test_ninja_parses()
    test_determinism()
    test_compile_commands()
    test_no_project_files_touched()

    print()
    if _failures:
        print( f'{len( _failures )} failure(s).' )
        return 1

    print( 'all checks passed.' )
    return 0

if __name__ == '__main__':
    sys.exit( main() )
