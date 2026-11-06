#!/usr/bin/env python3
"""Build and run the OBS 33 macOS baseline on a fresh hosted runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'obs33-results' / 'macos'
DMG_URL = 'https://github.com/obsproject/obs-studio/releases/download/33.0.1/OBS-Studio-33.0.1-macOS-Apple.dmg'
DMG_SHA = '3d6b2eedeb56416602af635adaba32327fdf11ff9d2806a4ce7d5b608de73238'


def run(args, *, env=None, timeout=1800):
    args = [str(x) for x in args]
    print('+', ' '.join(args), flush=True)
    result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=timeout)
    with (OUT / 'commands.log').open('ab') as log:
        log.write(('\n+ ' + ' '.join(args) + '\n').encode())
        log.write(result.stdout)
        log.write(result.stderr)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}); see commands.log: {args}')
    return result.stdout


def bundle_manifest(bundle):
    entries = {}
    for item in sorted(bundle.rglob('*')):
        name = item.relative_to(bundle).as_posix()
        if item.is_symlink():
            entries[name] = {'symlink': os.readlink(item)}
        elif item.is_file():
            entries[name] = {'sha256': hashlib.sha256(item.read_bytes()).hexdigest()}
    return entries


def expand_distribution(spec):
    package = OUT / 'plugins' / (spec['name'] + '.pkg')
    if not package.is_file():
        raise RuntimeError(f'Distribution pkg was not generated: {package}')
    release = ROOT / 'release'
    release.mkdir(exist_ok=True)
    artifact = release / f"{spec['name']}-{spec['version']}-macos-universal.pkg"
    shutil.copy2(package, artifact)
    # Confirm this runner's supported flags before using its system pkgutil.
    help_text = run(['/usr/sbin/pkgutil', '--help']).decode()
    if '--expand-full' not in help_text:
        raise RuntimeError('This pkgutil does not support --expand-full')
    expanded = OUT / 'expanded-package'
    run(['/usr/sbin/pkgutil', '--expand-full', artifact, expanded])
    bundles = [p for p in expanded.rglob(spec['name'] + '.plugin') if p.is_dir()]
    if len(bundles) != 1:
        raise RuntimeError(f'Expected one product bundle in distribution pkg: {bundles}')
    bundle = bundles[0]
    evidence = {'package': str(artifact.relative_to(ROOT)),
        'package_sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
        'bundle': str(bundle.relative_to(ROOT)), 'files': bundle_manifest(bundle)}
    (OUT / 'package-evidence.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    return bundle


def build(extra):
    run(['xcodebuild', '-version'])
    sdk = run(['xcrun', '--sdk', 'macosx', '--show-sdk-version']).decode().strip()
    if tuple(map(int, sdk.split('.')[:2])) < (26, 5):
        raise RuntimeError(f'macOS SDK 26.5 required, found {sdk}')
    spec = json.loads((ROOT / 'buildspec.json').read_text())
    for name in ('prebuilt', 'qt6'):
        if spec['dependencies'][name]['version'] != '2026-10-23':
            raise RuntimeError(f'Unexpected {name} dependency version')
    if spec['dependencies']['obs-studio']['version'] != '33.0.1':
        raise RuntimeError('OBS sources must be 33.0.1')
    run(['cmake', '--preset', 'macos-ci', '-DENABLE_CCACHE=OFF', '-DENABLE_BASELINE_PROBE=ON', *extra])
    run(['cmake', '--build', 'build_macos', '--config', 'RelWithDebInfo', '--parallel'])
    run(['cmake', '--install', 'build_macos', '--config', 'RelWithDebInfo', '--prefix', OUT / 'plugins'])
    bundle = expand_distribution(spec)
    binary = bundle / 'Contents/MacOS/loudness-balance-monitor'
    architectures = run(['lipo', '-archs', binary]).decode().split()
    if set(architectures) != {'arm64', 'x86_64'}:
        raise RuntimeError(f'Expected Universal plugin, found {architectures}')
    run(['otool', '-L', binary])
    run(['codesign', '--verify', '--deep', '--strict', binary.parents[2]])


def prepare():
    dmg = OUT / 'OBS-Studio-33.0.1-macOS-Apple.dmg'
    if not dmg.exists():
        with urllib.request.urlopen(DMG_URL, timeout=60) as response, dmg.open('wb') as target:
            shutil.copyfileobj(response, target)
    with dmg.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if digest != DMG_SHA:
        raise RuntimeError(f'Official DMG hash mismatch: {digest}')
    mounted = plistlib.loads(run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-plist', dmg]))
    mounts = [Path(item['mount-point']) for item in mounted['system-entities'] if 'mount-point' in item]
    try:
        apps = [mount / 'OBS.app' for mount in mounts if (mount / 'OBS.app').is_dir()]
        if len(apps) != 1:
            raise RuntimeError(f'Expected one OBS.app in DMG: {apps}')
        if (OUT / 'OBS.app').exists():
            raise RuntimeError('OBS.app already exists; use a fresh job')
        run(['ditto', apps[0], OUT / 'OBS.app'])
    finally:
        for mount in mounts:
            run(['hdiutil', 'detach', mount])
    run(['codesign', '--verify', '--deep', '--strict', OUT / 'OBS.app'])
    info = plistlib.loads((OUT / 'OBS.app/Contents/Info.plist').read_bytes())
    if info['CFBundleShortVersionString'] != '33.0.1':
        raise RuntimeError(f'Unexpected app version: {info}')
    (OUT / 'official-app.json').write_text(json.dumps({'url': DMG_URL, 'sha256': digest,
        'version': info['CFBundleShortVersionString'], 'architecture': platform.machine()}, indent=2))


def launch(probe, result_dir):
    result = result_dir / 'result.json'
    config = Path.home() / 'Library/Application Support/obs-studio'
    if config.exists():
        raise RuntimeError(f'Fresh hosted runner required; existing OBS config: {config}')
    if result.exists():
        raise RuntimeError(f'Probe result already exists: {result}')
    evidence = json.loads((OUT / 'package-evidence.json').read_text())
    package = ROOT / evidence['package']
    bundle = ROOT / evidence['bundle']
    if hashlib.sha256(package.read_bytes()).hexdigest() != evidence['package_sha256']:
        raise RuntimeError('Distribution pkg changed after extraction')
    if bundle_manifest(bundle) != evidence['files']:
        raise RuntimeError('Extracted product bundle changed before load')
    plugins = bundle.parent
    if not (plugins / 'loudness-balance-monitor.plugin').is_dir() or not probe.is_dir():
        raise RuntimeError('Product/probe plugin bundle is missing')
    shutil.copytree(probe, plugins / probe.name)
    config.mkdir(parents=True)
    (config / 'global.ini').write_text('[General]\nMacOSPermissionsDialogLastShown=1\n', encoding='utf-8')
    (config / 'user.ini').write_text('[General]\nFirstRun=true\n[Basic]\nSceneCollection=Baseline\nSceneCollectionFile=Baseline\n', encoding='utf-8')
    scenes = config / 'basic/scenes'
    scenes.mkdir(parents=True)
    # Existing scene data skips OBS's default microphone/desktop-device creation.
    scene = {'name': 'Baseline', 'current_scene': 'Empty', 'current_program_scene': 'Empty',
        'sources': [{'name': 'Empty', 'id': 'scene', 'versioned_id': 'scene',
                     'settings': {'items': []}}], 'scene_order': [{'name': 'Empty'}],
        'groups': [], 'transitions': [], 'current_transition': 'Fade', 'transition_duration': 300}
    (scenes / 'Baseline.json').write_text(json.dumps(scene), encoding='utf-8')
    env = os.environ.copy()
    env['OBS_PLUGINS_PATH'] = str(plugins)
    env['LBM_BASELINE_RESULT_DIR'] = str(result_dir)
    executable = OUT / 'OBS.app/Contents/MacOS/OBS'
    with (OUT / 'obs-console.log').open('wb') as log:
        process = subprocess.Popen([str(executable), '--disable-updater', '--disable-missing-files-check'],
            cwd=executable.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=120)
            if process.returncode != 0:
                raise RuntimeError(f'OBS exited with {process.returncode}')
            if not result.is_file():
                raise RuntimeError('Probe did not write its result')
            report = json.loads(result.read_text())
            if report.get('passed') is not True or report.get('obs') != '33.0.1':
                raise RuntimeError(f'Probe failed: {report}')
            if not (result_dir / 'dock.png').is_file():
                raise RuntimeError('Probe screenshot is missing')
            if b'LBM_BASELINE PASS' not in (OUT / 'obs-console.log').read_bytes():
                raise RuntimeError('Probe PASS log is missing')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            shutil.copytree(config, OUT / 'obs-config', dirs_exist_ok=True)
    print(f'OBS 33.0.1 dock probe passed: {result}', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['build', 'prepare', 'run'])
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--result-dir', type=Path)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('Only a fresh GitHub-hosted macOS arm64 job is supported')
    OUT.mkdir(parents=True, exist_ok=True)
    if args.stage == 'build':
        build(args.cmake_arg)
    elif args.stage == 'prepare':
        prepare()
    else:
        if args.probe is None or args.result_dir is None:
            parser.error('run requires --probe and --result-dir')
        launch(args.probe.resolve(), args.result_dir.resolve())


if __name__ == '__main__':
    main()
