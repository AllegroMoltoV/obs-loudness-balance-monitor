#!/usr/bin/env python3
"""Build and run the OBS 33 macOS baseline on a fresh hosted runner."""
import argparse
import configparser
import getpass
import xml.etree.ElementTree as ET
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


def diagnostic(args):
    result = subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True, timeout=60)
    return {'command': [str(x) for x in args], 'exit_code': result.returncode,
            'stdout': result.stdout.decode(errors='replace'), 'stderr': result.stderr.decode(errors='replace')}


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
    artifact = ROOT / 'release' / f"{spec['name']}-{spec['version']}-macos-universal.pkg"
    if not artifact.is_file():
        raise RuntimeError(f'Normal package script did not generate: {artifact}')
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
        'bundle': str(bundle.relative_to(ROOT)), 'files': bundle_manifest(bundle),
        'bundle_id': spec['platformConfig']['macos']['bundleId']}
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
    build_env = os.environ.copy()
    build_env['CODESIGN_IDENT'] = '-'
    build_env['CODESIGN_TEAM'] = ''
    run(['zsh', '.github/scripts/build-macos', '--config', 'RelWithDebInfo'], env=build_env)
    run(['zsh', '.github/scripts/package-macos', '--config', 'RelWithDebInfo', '--package'], env=build_env)
    # Enable the diagnostic module only after the normal product artifact is finalized.
    run(['cmake', '--preset', 'macos-ci', '-DENABLE_BASELINE_PROBE=ON', *extra], env=build_env)
    run(['cmake', '--build', 'build_macos', '--config', 'RelWithDebInfo', '--target', 'lbm-baseline-probe', '--parallel'], env=build_env)
    bundle = expand_distribution(spec)
    binary = bundle / 'Contents/MacOS/loudness-balance-monitor'
    architectures = run(['lipo', '-archs', binary]).decode().split()
    if set(architectures) != {'arm64', 'x86_64'}:
        raise RuntimeError(f'Expected Universal plugin, found {architectures}')
    run(['otool', '-arch', 'all', '-L', binary])
    run(['otool', '-arch', 'all', '-l', binary])
    run(['codesign', '--verify', '--deep', '--strict', binary.parents[2]])


def install():
    config = Path.home() / 'Library/Application Support/obs-studio'
    if config.exists() or os.geteuid() == 0:
        raise RuntimeError('Install requires a fresh non-root runner account without OBS config')
    evidence = json.loads((OUT / 'package-evidence.json').read_text())
    package = ROOT / evidence['package']
    if hashlib.sha256(package.read_bytes()).hexdigest() != evidence['package_sha256']:
        raise RuntimeError('Distribution pkg changed after extraction')
    distribution = ET.parse(OUT / 'expanded-package/Distribution').getroot()
    domains = distribution.find('domains')
    if domains is None or domains.attrib != {'enable_currentUserHome': 'true',
            'enable_anywhere': 'false', 'enable_localSystem': 'false'}:
        raise RuntimeError('Distribution does not exclusively target the current user home')
    installer = '/usr/sbin/installer'
    run([installer, '-help'])
    domain_info = run([installer, '-dominfo', '-pkg', package]).decode()
    if 'CurrentUserHomeDirectory' not in domain_info:
        raise RuntimeError(f'Installer does not expose the current-user domain: {domain_info}')
    run([installer, '-volinfo', '-pkg', package])
    plan = run([installer, '-pkg', package, '-target', 'CurrentUserHomeDirectory', '-config'])
    (OUT / 'installer-target.plist').write_bytes(plan)
    target_plan = plistlib.loads(plan)
    if target_plan.get('target') != 'CurrentUserHomeDirectory':
        raise RuntimeError(f'Unexpected installer target configuration: {target_plan}')
    diagnostics = {'user': getpass.getuser(), 'uid': os.geteuid(), 'home': str(Path.home()),
        'target': 'CurrentUserHomeDirectory', 'signature': diagnostic(['/usr/sbin/pkgutil', '--check-signature', package]),
        'gatekeeper': diagnostic(['/usr/sbin/spctl', '--assess', '--type', 'install', '--verbose=4', package]),
        'notarization': 'Not submitted by baseline; no signing credentials configured',
        'intel_execution': 'not tested', 'minimum_os_execution': 'not tested'}
    (OUT / 'installation-evidence.json').write_text(json.dumps(diagnostics, indent=2), encoding='utf-8')
    # Do not add sudo/allowUntrusted or change the target after an installation failure.
    run([installer, '-pkg', package, '-target', 'CurrentUserHomeDirectory', '-verboseR', '-dumplog'])
    installed = config / 'plugins/loudness-balance-monitor.plugin'
    if not installed.is_dir() or bundle_manifest(installed) != evidence['files']:
        raise RuntimeError('Installed product does not match the distribution payload')
    if (config / 'global.ini').exists() or (config / 'user.ini').exists():
        raise RuntimeError('Unexpected OBS settings appeared before first launch')
    diagnostics['installed_bundle'] = str(installed)
    diagnostics['bundle_signature'] = diagnostic(['codesign', '-d', '--verbose=4', installed])
    diagnostics['receipt'] = diagnostic(['/usr/sbin/pkgutil', '--volume', str(Path.home()),
                                         '--pkg-info-plist', evidence['bundle_id']])
    diagnostics['installed_payload_matches'] = True
    (OUT / 'installation-evidence.json').write_text(json.dumps(diagnostics, indent=2), encoding='utf-8')


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


def verify_loaded_images(report, installed):
    images = report.get('loaded_images')
    if not isinstance(images, list) or not images:
        raise RuntimeError('Probe did not report loaded image paths')
    paths = [Path(name).resolve() for name in images]
    product = (installed / 'Contents/MacOS/loudness-balance-monitor').resolve()
    if product not in paths:
        raise RuntimeError('Product was not loaded from the normal installed path')
    frameworks = (OUT / 'OBS.app/Contents/Frameworks').resolve()
    for name in ('QtCore', 'QtWidgets', 'libobs', 'obs-frontend-api'):
        matches = [path for path in paths if path.name == name or path.name.startswith(name + '.')]
        if not matches or any(not path.is_relative_to(frameworks) for path in matches):
            raise RuntimeError(f'Expected official OBS runtime dependency {name}: {matches}')
    if any(path.is_relative_to(ROOT / '.deps') for path in paths):
        raise RuntimeError('OBS loaded a development dependency from .deps')


def launch(probe, result_dir):
    config = Path.home() / 'Library/Application Support/obs-studio'
    installed = config / 'plugins/loudness-balance-monitor.plugin'
    evidence = json.loads((OUT / 'package-evidence.json').read_text())
    installation = json.loads((OUT / 'installation-evidence.json').read_text())
    if installation.get('installed_payload_matches') is not True or not installed.is_dir():
        raise RuntimeError('Normal installation has not passed')
    if bundle_manifest(installed) != evidence['files']:
        raise RuntimeError('Installed product changed before load')
    if (config / 'global.ini').exists() or (config / 'user.ini').exists() or result_dir.exists():
        raise RuntimeError('First-run settings or result directory already exists')
    if not probe.is_dir():
        raise RuntimeError('Probe bundle is missing')
    probe_dir = OUT / 'probe-plugins'
    probe_dir.mkdir()
    shutil.copytree(probe, probe_dir / probe.name)
    if list(probe_dir.glob('loudness-balance-monitor.plugin')):
        raise RuntimeError('Product must not be present in the probe search path')
    (config / 'global.ini').write_text('[General]\nMacOSPermissionsDialogLastShown=1\n', encoding='utf-8')
    (config / 'user.ini').write_text('[General]\nFirstRun=true\n[Basic]\nSceneCollection=Baseline\nSceneCollectionFile=Baseline\n', encoding='utf-8')
    scenes = config / 'basic/scenes'
    scenes.mkdir(parents=True)
    scene = {'name': 'Baseline', 'current_scene': 'Empty', 'current_program_scene': 'Empty',
        'sources': [{'name': 'Empty', 'id': 'scene', 'versioned_id': 'scene',
                     'settings': {'items': []}}], 'scene_order': [{'name': 'Empty'}],
        'groups': [], 'transitions': [], 'current_transition': 'Fade', 'transition_duration': 300}
    (scenes / 'Baseline.json').write_text(json.dumps(scene), encoding='utf-8')
    executable = OUT / 'OBS.app/Contents/MacOS/OBS'
    for phase in ('setup', 'restore', 'restore-hidden'):
        phase_dir = result_dir / phase
        phase_dir.mkdir(parents=True)
        env = os.environ.copy()
        for key in ('OBS_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_DATA_PATH'):
            env.pop(key, None)
        env['OBS_PLUGINS_PATH'] = str(probe_dir)
        env['LBM_BASELINE_RESULT_DIR'] = str(phase_dir)
        env['LBM_BASELINE_PHASE'] = phase
        console = OUT / f'obs-{phase}.log'
        with console.open('wb') as log:
            process = subprocess.Popen([str(executable), '--disable-updater', '--disable-missing-files-check'],
                cwd=executable.parent, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                process.wait(timeout=120)
                if process.returncode != 0:
                    raise RuntimeError(f'OBS {phase} exited with {process.returncode}')
                report = json.loads((phase_dir / 'result.json').read_text())
                if report.get('passed') is not True or report.get('obs') != '33.0.1' or report.get('phase') != phase:
                    raise RuntimeError(f'Probe failed: {report}')
                if not (phase_dir / 'dock.png').is_file() or b'LBM_BASELINE PASS' not in console.read_bytes():
                    raise RuntimeError(f'Probe screenshot/PASS log is missing: {phase}')
                verify_loaded_images(report, installed)
                settings = config / 'plugin_config/loudness-balance-monitor/settings.json'
                values = json.loads(settings.read_text())
                if any(values.get(k) != v for k, v in {'vad_threshold': -35, 'balance_target': 8, 'mix_preset': 1}.items()):
                    raise RuntimeError(f'Saved settings mismatch: {values}')
                user = configparser.ConfigParser(interpolation=None)
                user.read(config / 'user.ini')
                if not user.get('BasicWindow', 'DockState', fallback=''):
                    raise RuntimeError('DockState was not saved')
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
                shutil.copytree(config, OUT / 'obs-config' / phase, dirs_exist_ok=True)
    print('OBS 33.0.1 normal installation and restoration probes passed', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['build', 'prepare', 'install', 'run'])
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--result-dir', type=Path)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted':
        raise RuntimeError('Only a fresh GitHub-hosted macOS arm64 job is supported')
    OUT.mkdir(parents=True, exist_ok=True)
    if args.stage == 'build':
        build(args.cmake_arg)
    elif args.stage == 'prepare':
        prepare()
    elif args.stage == 'install':
        install()
    else:
        if args.probe is None or args.result_dir is None:
            parser.error('run requires --probe and --result-dir')
        launch(args.probe.resolve(), args.result_dir.resolve())


if __name__ == '__main__':
    main()
