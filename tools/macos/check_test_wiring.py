#!/usr/bin/env python3
"""Every test in tests/ is run by Darwin `build.sh --test` or excluded here with a reason.

Fails, naming the file, when a test source or script under tests/ is neither reachable from
build.sh (directly, through a script build.sh runs, or as a source of a CMake target build.sh runs)
nor listed in EXCLUDED. Data, docs and shader inputs are not tests and are ignored.
  python3 tools/macos/check_test_wiring.py [repository root]
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST_SUFFIXES = ('.c', '.cpp', '.mm', '.py', '.sh')

# path -> why build.sh --test does not run it. Keep reasons specific; a vague reason is a gap.
EXCLUDED = {
    'tests/paths.py': 'helper module imported by the Python tests, not a test',
    'tests/compare_atrac9_asset.py': 'manual tool: compares against a local game asset that the '
                                     'repository does not ship',
    'tests/test_ajm_asset.c': 'diagnostic decoder for compare_atrac9_asset.py: needs ATRAC9 frames '
                              'from a local game asset',
    'tests/test_buffer_bounds_benchmark.cpp': 'benchmark (buffer-bounds-benchmark target): timings, '
                                              'no pass/fail; the bounds behaviour is buffer-bounds-test',
    'tests/macos_gs/bench/': 'design measurement of the render-pass split cost (docs/macos/'
                             'geometry-shaders.md): timings, no pass/fail',
    'tests/macos_spike/run.sh': 'the original Rosetta feasibility spike (historical); the TLS and HLE '
                                'tests in this directory are run',
    'tests/macos_spike/spike.c': 'part of the Rosetta feasibility spike (macos_spike/run.sh)',
    'tests/macos_spike/scan_x86.c': 'eboot instruction scanner from the Rosetta spike (a research '
                                    'tool, needs the game executable)',
    'tests/macos_float_modes/run.sh': 'research probe of MoltenVK float modes; exits 2 by design '
                                      'when it observes the known differences; the production check '
                                      'is macos_float_modes/production.sh',
    'tests/macos_float_modes/variants.py': 'variant generator of the float-mode research probe '
                                           '(macos_float_modes/run.sh)',
}


def read(path):
    with open(os.path.join(ROOT, path), errors='replace') as file:
        return file.read()


def cmake_targets():
    """CMake executable name -> tests/ sources it compiles."""
    targets = {}
    text = read('gpu/CMakeLists.txt')
    for match in re.finditer(r'add_executable\(\s*([\w-]+)\s+(?:EXCLUDE_FROM_ALL\s+)?([^)]*)\)', text):
        targets[match.group(1)] = ['tests/' + s for s in re.findall(r'\.\./tests/([\w./-]+)', match.group(2))]
    return targets


def reachable():
    """Files under tests/ that build.sh --test compiles or runs."""
    build = read('build.sh')
    seen, queue, reached = set(), ['build.sh'], set()
    while queue:
        path = queue.pop()
        if path in seen or not os.path.isfile(os.path.join(ROOT, path)):
            continue
        seen.add(path)
        text = read(path)
        names = set(re.findall(r'tests/[\w./-]+', text))
        if path.startswith('tests/'):
            # Scripts name their own directory's files relative to it ("$here/build.sh").
            here = os.path.dirname(path)
            names |= {os.path.join(here, n) for n in re.findall(r'[\w-]+\.(?:c|cpp|mm|py|sh)\b', text)}
        for name in names:
            name = name.rstrip('.')
            full = os.path.join(ROOT, name)
            if os.path.isdir(full):
                for script in ('run.sh', 'build.sh'):
                    if os.path.isfile(os.path.join(full, script)):
                        queue.append(os.path.join(name, script))
                continue
            if os.path.isfile(full):
                reached.add(name)
                if name.endswith(('.sh', '.py')):
                    queue.append(name)
    # unittest discovery runs every tests/test_*.py (and what those scripts compile or run).
    texts = [read(p) for p in seen]
    if any(re.search(r'unittest discover -s tests(?! -p)', t) for t in texts):
        for f in os.listdir(os.path.join(ROOT, 'tests')):
            if f.startswith('test_') and f.endswith('.py'):
                path = 'tests/' + f
                reached.add(path)
                for name in re.findall(r'tests/[\w./-]+', read(path)):
                    if os.path.isfile(os.path.join(ROOT, name.rstrip('.'))):
                        reached.add(name.rstrip('.'))
    for target, sources in cmake_targets().items():
        if re.search(r'out/gpu/' + re.escape(target) + r'\b', build):
            reached |= set(sources)
    return reached


def main():
    reached = reachable()
    problems = []
    for directory, subdirectories, files in os.walk(os.path.join(ROOT, 'tests')):
        subdirectories[:] = [d for d in subdirectories if d != '__pycache__']
        for name in files:
            path = os.path.relpath(os.path.join(directory, name), ROOT)
            if not name.endswith(TEST_SUFFIXES):
                continue
            excluded = EXCLUDED.get(path) or next(
                (reason for prefix, reason in EXCLUDED.items() if prefix.endswith('/') and path.startswith(prefix)), None)
            if path in reached and excluded:
                problems.append(f'{path}: run by --test but also listed in EXCLUDED')
            elif path not in reached and not excluded:
                problems.append(f'{path}: neither run by build.sh --test nor excluded in tools/macos/check_test_wiring.py')
    for path in EXCLUDED:
        if not os.path.exists(os.path.join(ROOT, path)):
            problems.append(f'{path}: listed in EXCLUDED but does not exist')
    if problems:
        print('Test wiring check FAILED:', *problems, sep='\n  ')
        sys.exit(1)
    print(f'Test wiring: {len(reached)} test files run, {len(EXCLUDED)} excluded with reasons')


if __name__ == '__main__':
    main()
