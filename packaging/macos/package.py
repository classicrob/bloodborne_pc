#!/usr/bin/env python3
"""Validate a relocatable macOS app plan; build only after a passing GPU milestone."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

from importlib.util import spec_from_file_location, module_from_spec

ROOT = Path(__file__).resolve().parents[2]
spec = spec_from_file_location('python_pin', Path(__file__).with_name('fetch-python.py'))
pin = module_from_spec(spec)
spec.loader.exec_module(pin)


def output(*arguments):
    return subprocess.check_output(arguments, text=True).strip()


def architectures(path):
    return output('/usr/bin/lipo', '-archs', str(path)).split()


def dependencies(path):
    lines = output('/usr/bin/otool', '-arch', 'x86_64', '-L', str(path)).splitlines()[1:]
    return [line.strip().split(' (compatibility')[0] for line in lines]


def system_library(name):
    return name.startswith(('/System/Library/', '/usr/lib/'))


def library_closure(seeds, prefix):
    libraries = {}
    pending = [(path.name, path) for path in seeds]
    while pending:
        alias, path = pending.pop()
        path = path.resolve(strict=True)
        if alias in libraries:
            if libraries[alias] != path:
                raise ValueError(f'Conflicting dependency name: {alias}')
            continue
        if 'x86_64' not in architectures(path):
            raise ValueError(f'GPU dependency has no x86_64 slice: {path}')
        libraries[alias] = path
        for name in dependencies(path):
            if system_library(name):
                continue
            if name.startswith('@rpath/'):
                candidate = prefix / 'lib' / name.removeprefix('@rpath/')
                if name.removeprefix('@rpath/') == 'libbbgpu.dylib':
                    candidate = ROOT / 'out/gpu/libbbgpu.dylib'
            elif name.startswith('@loader_path/'):
                candidate = path.parent / name.removeprefix('@loader_path/')
            elif name.startswith('/'):
                candidate = Path(name)
            else:
                raise ValueError(f'Unsupported library reference in {path}: {name}')
            if candidate.resolve() != path:
                pending.append((Path(name).name, candidate))
    return libraries


def rpaths(path):
    result = []
    lines = output('/usr/bin/otool', '-arch', 'x86_64', '-l', str(path)).splitlines()
    for index, line in enumerate(lines):
        if line.strip() == 'cmd LC_RPATH':
            result.append(lines[index + 2].strip().split('path ', 1)[1].split(' (offset')[0])
    return result


def relocate(path, search, dylib=False):
    arguments = ['/usr/bin/install_name_tool']
    if dylib:
        arguments += ['-id', '@rpath/' + path.name]
    for name in dependencies(path):
        if not system_library(name):
            arguments += ['-change', name, '@rpath/' + Path(name).name]
    for name in rpaths(path):
        arguments += ['-delete_rpath', name]
    arguments += ['-add_rpath', search, str(path)]
    subprocess.run(arguments, check=True)


def thin(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if architectures(source) == ['x86_64']:
        shutil.copy2(source, destination)
    else:
        subprocess.run(['/usr/bin/lipo', str(source), '-thin', 'x86_64', '-output', str(destination)], check=True)
    destination.chmod(destination.stat().st_mode | 0o200)


def milestone_report(path):
    if path is None:
        raise ValueError('--build requires --milestone-a-report from the real GPU-path acceptance run.')
    report = json.loads(path.read_text())
    if report.get('milestone') != 'A' or report.get('result') != 'pass' or report.get('gpu_path') is not True:
        raise ValueError('Milestone A report must record result=pass and gpu_path=true; CPU-only does not qualify.')
    log = Path(report['log'])
    if not log.is_file() or not log.stat().st_size:
        raise ValueError(f'Milestone A log is missing or empty: {log}')
    return {'report': str(path.resolve()), 'report_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest()}


def build(args, libraries, acceptance):
    app = args.output.resolve()
    if app.exists():
        raise ValueError(f'Refusing to overwrite an existing app: {app}')
    contents = app / 'Contents'
    frameworks = contents / 'Frameworks'
    resources = contents / 'Resources'
    port = resources / 'port'
    for directory in (frameworks, port / 'bin', contents / 'MacOS'):
        directory.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.python, resources / 'python', symlinks=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copytree(ROOT / 'scripts', port / 'scripts',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    (port / 'launcher').mkdir()
    for name in ('bbport_launcher_mac.py', 'bbport_launcher_win.py', 'bbport_lang.py'):
        shutil.copy2(ROOT / 'launcher' / name, port / 'launcher' / name)
    shutil.copy2(ROOT / 'run.sh', port / 'run.sh')
    for name in ('LICENSE', 'COPYING', 'NOTICE'):
        if (ROOT / name).is_file():
            shutil.copy2(ROOT / name, port / name)
    for name in ('bb-probe', 'bb-gpu-capabilities'):
        binary = port / 'bin' / name
        thin(ROOT / 'out' / name, binary)
        relocate(binary, '@loader_path/../../../Frameworks')
    for name, source in libraries.items():
        if name in ('bb-probe', 'bb-gpu-capabilities'):
            continue
        library = frameworks / name
        thin(source, library)
        relocate(library, '@loader_path', dylib=True)
    deps = port / 'deps'
    deps.mkdir()
    (deps / 'lib').symlink_to(os.path.relpath(frameworks, deps), target_is_directory=True)
    icd = deps / 'share/vulkan/icd.d'
    icd.mkdir(parents=True)
    shutil.copy2(args.prefix / 'share/vulkan/icd.d/MoltenVK_icd.json', icd)
    executable = contents / 'MacOS/Bloodborne'
    subprocess.run(['clang', '-arch', 'arm64', '-fobjc-arc', '-framework', 'AppKit',
                    str(Path(__file__).with_name('app_main.m')), '-o', str(executable)], check=True)
    info = {'CFBundleExecutable': 'Bloodborne', 'CFBundleIdentifier': 'org.bbport.bloodborne',
            'CFBundleName': 'Bloodborne', 'CFBundleDisplayName': 'Bloodborne',
            'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1', 'CFBundleShortVersionString': '0.1',
            'LSMinimumSystemVersion': '14.0', 'LSArchitecturePriority': ['arm64'],
            'NSHighResolutionCapable': True, 'NSPrincipalClass': 'NSApplication'}
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    (resources / 'acceptance.json').write_text(json.dumps(acceptance, indent=2) + '\n')
    subprocess.run([sys.executable, str(Path(__file__).with_name('check-exports.py')),
                    str(frameworks / 'libbbgpu.dylib'), str(port / 'bin/bb-probe')],
                    check=True, stdout=sys.stderr)
    for path in sorted(contents.rglob('*'), key=lambda item: len(item.parts), reverse=True):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as source:
                magic = source.read(4)
            if magic in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
                subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(app)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    print(f'Created {app}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, default=Path.home() / 'bbport-deps/x86_64')
    parser.add_argument('--python', type=Path, default=Path.home() / 'bbport-deps/python-arm64')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/Bloodborne.app')
    parser.add_argument('--build', action='store_true', help='Write an app after real GPU-path Milestone A passes.')
    parser.add_argument('--milestone-a-report', type=Path)
    args = parser.parse_args()
    args.prefix, args.python = args.prefix.resolve(), args.python.resolve()
    receipt = json.loads((args.python / 'bbport-runtime.json').read_text())
    if receipt.get('sha256') != pin.SHA256 or receipt.get('archive') != pin.ARCHIVE:
        raise ValueError('Bundled Python must match the pinned native runtime; run fetch-python.py.')
    pin.verify(args.python)
    libraries = library_closure([ROOT / 'out/bb-probe', ROOT / 'out/bb-gpu-capabilities',
                                args.prefix / 'lib/libMoltenVK.dylib'], args.prefix)
    subprocess.run([sys.executable, str(Path(__file__).with_name('check-exports.py')),
                    str(ROOT / 'out/gpu/libbbgpu.dylib'), str(ROOT / 'out/bb-probe')],
                    check=True, stdout=sys.stderr)
    manifest = json.loads((args.prefix / 'share/vulkan/icd.d/MoltenVK_icd.json').read_text())
    if manifest['ICD']['library_path'] != '../../../lib/libMoltenVK.dylib':
        raise ValueError('MoltenVK manifest must use the relocatable prefix-relative library path.')
    plan = {'output': str(args.output), 'launcher_architecture': 'arm64', 'guest_process_architecture': 'x86_64',
            'python_archive': pin.ARCHIVE, 'python_sha256': pin.SHA256,
            'libraries': {name: str(path) for name, path in sorted(libraries.items())},
            'writable_data': '~/Library/Application Support/bbport',
            'bundle_estimated_bytes': sum(path.stat().st_size for path in libraries.values()) +
                sum(path.stat().st_size for path in args.python.rglob('*') if path.is_file()),
            'build_requires': 'Passing real GPU-path Milestone A report; CPU-only is insufficient.'}
    print(json.dumps(plan, indent=2))
    if args.build:
        build(args, libraries, milestone_report(args.milestone_a_report))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'macOS packaging failed: {error}') from error
