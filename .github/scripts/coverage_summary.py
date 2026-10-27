#!/usr/bin/env python3
"""Turns the raw profiles from a clang coverage build (-fprofile-instr-generate
-fcoverage-mapping, tests run with LLVM_PROFILE_FILE set) into an HTML report
and a Markdown summary for the GitHub Actions job summary page.

Usage:
  coverage_summary.py <build dir> <profile dir> <html output dir>

The Markdown goes to $GITHUB_STEP_SUMMARY if set, otherwise stdout. The
llvm-profdata and llvm-cov commands can be overridden with the LLVM_PROFDATA
and LLVM_COV environment variables (e.g. LLVM_COV="xcrun llvm-cov").

Third-party code (including the vendored ulog.c logging backend), the tests
themselves and the standalone LDPC tools are left out. llvm-cov only knows
about files linked into a test, so backend sources that were compiled but
never linked into one are listed separately.
"""
import glob
import json
import os
import platform
import re
import shlex
import subprocess
import sys

SOURCE_DIR = os.path.realpath(os.path.join(os.path.dirname(__file__), '..', '..', 'src'))
EXCLUDE = r'3rdparty|/test/|Test\.cpp$|ldpc_(en|de)code\.c(pp)?$|util/timespec\.[ch]$|util/logging/libfmemopen|util/logging/ulog\.c$|websocketpp/'


def tool(env_name, default):
    return shlex.split(os.environ.get(env_name, default))


def test_binaries(build_dir):
    """Every executable in the build tree that a ctest test runs."""
    tests = json.loads(subprocess.run(['ctest', '--show-only=json-v1'], cwd=build_dir,
                                      capture_output=True, text=True, check=True).stdout)['tests']
    build_dir = os.path.realpath(build_dir)
    binaries = set()
    for test in tests:
        for arg in test.get('command', []):
            path = os.path.realpath(arg)
            if (path.startswith(build_dir + os.sep) and os.path.isfile(path)
                    and os.access(path, os.X_OK) and not path.endswith('.py')):
                binaries.add(path)
    return sorted(binaries)


def compiled_sources(build_dir):
    """Backend sources the build compiled (needs CMAKE_EXPORT_COMPILE_COMMANDS)."""
    path = os.path.join(build_dir, 'compile_commands.json')
    if not os.path.exists(path):
        return set()
    sources = set()
    for entry in json.load(open(path)):
        file = os.path.realpath(os.path.join(entry['directory'], entry['file']))
        if file.startswith(SOURCE_DIR + os.sep):
            rel = os.path.relpath(file, SOURCE_DIR)
            if not re.search(EXCLUDE, rel):
                sources.add(rel)
    return sources


def percent(summary):
    count = summary['count']
    return 100.0 * summary['covered'] / count if count else None


def cell(summary):
    pct = percent(summary)
    if pct is None:
        return '–'
    return f"{pct:.1f}% ({summary['covered']}/{summary['count']})"


def main():
    build_dir, profile_dir, html_dir = sys.argv[1:4]
    profiles = glob.glob(os.path.join(profile_dir, '*.profraw'))
    if not profiles:
        sys.exit(f'no .profraw files in {profile_dir}')

    profdata = os.path.join(profile_dir, 'merged.profdata')
    subprocess.run(tool('LLVM_PROFDATA', 'llvm-profdata') + ['merge', '-sparse', '-o', profdata] + profiles,
                   check=True)

    binaries = test_binaries(build_dir)
    cov_args = [binaries[0]]
    for binary in binaries[1:]:
        cov_args += ['-object', binary]
    cov_args += ['-instr-profile=' + profdata]

    # Filter on resolved paths: the build may see the tree through a symlink.
    llvm_cov = tool('LLVM_COV', 'llvm-cov')
    export = json.loads(subprocess.run(llvm_cov + ['export', '-summary-only'] + cov_args,
                                       capture_output=True, text=True, check=True).stdout)['data'][0]
    measured = {}
    for f in export['files']:
        path = os.path.realpath(f['filename'])
        if path.startswith(SOURCE_DIR + os.sep):
            rel = os.path.relpath(path, SOURCE_DIR)
            if not re.search(EXCLUDE, rel):
                measured[rel] = f
    files = sorted((name, f['summary']) for name, f in measured.items())
    totals = {}
    for kind in ('lines', 'functions', 'branches'):
        totals[kind] = {'count': sum(s[kind]['count'] for _, s in files),
                        'covered': sum(s[kind]['covered'] for _, s in files)}

    subprocess.run(llvm_cov + ['show', '-format=html', '-show-line-counts-or-regions',
                               '-output-dir=' + html_dir] + cov_args +
                   [f['filename'] for f in measured.values()], check=True)

    not_linked = sorted(compiled_sources(build_dir) - {name for name, _ in files})

    out = []
    out.append('## Backend code coverage')
    out.append('')
    out.append(f'{platform.system()}, clang source-based coverage, {len(binaries)} test programs. '
               'Third-party code and the tests themselves are excluded. The line-by-line '
               'report is in the `coverage-html` artifact of this run.')
    out.append('')
    out.append('| | Covered |')
    out.append('|---|---|')
    out.append(f"| Lines | {cell(totals['lines'])} |")
    out.append(f"| Functions | {cell(totals['functions'])} |")
    out.append(f"| Branches | {cell(totals['branches'])} |")
    out.append(f'| Files | {len(files)} measured, {len(not_linked)} not linked into any test |')
    out.append('')
    if not_linked:
        out.append('**Not linked into any test (0% coverage):** ' + ', '.join(f'`{f}`' for f in not_linked))
        out.append('')
    out.append('<details><summary>Coverage by file</summary>')
    out.append('')
    out.append('| File | Lines | Functions | Branches |')
    out.append('|---|---|---|---|')
    for name, summary in files:
        out.append(f"| `{name}` | {cell(summary['lines'])} | {cell(summary['functions'])} | "
                   f"{cell(summary['branches'])} |")
    out.append('')
    out.append('</details>')
    out.append('')

    markdown = '\n'.join(out)
    summary_path = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary_path:
        with open(summary_path, 'a') as fh:
            fh.write(markdown)
    else:
        print(markdown)


if __name__ == '__main__':
    main()
