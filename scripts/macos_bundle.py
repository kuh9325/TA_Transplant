#!/usr/bin/env python3
"""Audit, prepare (never sign), and verify TA Transplant macOS bundles.

Python 3.9+, Apple's otool/file/lipo/plutil/install_name_tool/codesign.
Packaging always creates a new destination and never edits its input bundle.
Verification exit 0 means structural closure, NOT signing/release acceptance;
--strict additionally requires an icon and a valid bundle signature.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
from macos_icon import verify_icon

SYSTEM = ('/System/Library/', '/usr/lib/')
SHADERS = [name + ext for name in (
    'basicColor', 'basicTexture', 'mapTerrain', 'unitTexture', 'unitShadow',
    'unitBuild', 'unitTextureInstanced', 'unitShadowInstanced',
    'unitBuildInstanced', 'flashEffect', 'worldPost') for ext in ('.vert', '.frag')]
LOADS = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB', 'LC_LOAD_UPWARD_DYLIB'}
EXTERNAL_CODECS = re.compile(r'lib(?:FLAC|fluidsynth|mpg123|opusfile|vorbisfile|vorbisidec|gme|xmp|wavpack)[^\s/]*\.dylib')


def run(*args, check=True):
    p = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode:
        raise RuntimeError(f'{args[0]} failed ({p.returncode}): {p.stderr or p.stdout}')
    return p


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def macho(p):
    output = run('/usr/bin/otool', '-l', p).stdout
    info = {'dependencies': [], 'rpaths': [], 'id': None, 'minos': None,
            'architecture': run('/usr/bin/lipo', '-archs', p).stdout.strip(),
            'file': run('/usr/bin/file', '-b', p).stdout.strip()}
    for block in output.split('Load command ')[1:]:
        m = re.search(r'\bcmd (LC_\w+)', block)
        if not m:
            continue
        command = m.group(1)
        if command in LOADS or command == 'LC_ID_DYLIB':
            value = re.search(r'\bname (.*?) \(offset', block).group(1)
            if command == 'LC_ID_DYLIB':
                info['id'] = value
            else:
                info['dependencies'].append(value)
        elif command == 'LC_RPATH':
            info['rpaths'].append(re.search(r'\bpath (.*?) \(offset', block).group(1))
        elif command == 'LC_BUILD_VERSION':
            info['minos'] = re.search(r'\bminos (\S+)', block).group(1)
        elif command == 'LC_VERSION_MIN_MACOSX':
            info['minos'] = re.search(r'\bversion (\S+)', block).group(1)
    return info


def expand(value, loader, executable):
    return value.replace('@loader_path', str(loader.parent)).replace('@executable_path', str(executable.parent))


def executable(app):
    plist = plistlib.loads((app/'Contents/Info.plist').read_bytes())
    name = plist['CFBundleExecutable']
    if Path(name).name != name:
        raise ValueError('CFBundleExecutable must be a filename')
    return app/'Contents/MacOS'/name, plist


def graph(app, extras=()):
    exe, _ = executable(app)
    nodes, edges = {}, []
    queue = [(exe, [])] + [(Path(p), []) for p in extras]
    while queue:
        loader, inherited = queue.pop(0)
        loader = loader.resolve(strict=True)
        if str(loader) in nodes:
            continue
        info = macho(loader)
        nodes[str(loader)] = info
        rpaths = [expand(p, loader, exe) for p in info['rpaths']] + inherited
        for dependency in info['dependencies']:
            edge = {'loader': str(loader), 'name': dependency}
            if dependency.startswith(SYSTEM):
                edge.update(category='A', resolved=dependency)
            else:
                if dependency.startswith('@rpath/'):
                    candidates = [Path(r)/dependency[len('@rpath/'):] for r in rpaths]
                else:
                    candidates = [Path(expand(dependency, loader, exe))]
                resolved = next((p.resolve() for p in candidates if p.is_absolute() and p.is_file()), None)
                if resolved is None:
                    raise RuntimeError(f'Unresolved dependency: {loader}: {dependency}')
                inside = resolved.is_relative_to(app.resolve())
                edge.update(category='B' if inside else ('D' if str(resolved).startswith(('/opt/homebrew/', '/Users/')) else 'C'), resolved=str(resolved))
                queue.append((resolved, rpaths))
            edges.append(edge)
    return {'nodes': nodes, 'edges': edges}


def licenses(source, nodes, destination):
    destination.mkdir()
    paths = {
        'TA-Transplant-GPL.txt': source/'LICENSE',
        'SDL3.txt': source/'libs/sdl3/LICENSE.txt',
        'SDL3-mixer.txt': source/'libs/sdl3_mixer/LICENSE.txt',
        'dr-libs.txt': source/'libs/sdl3_mixer/src/dr_libs/LICENSE',
        'TiMidity.txt': source/'libs/sdl3_mixer/src/timidity/COPYING',
        'ImGui.txt': source/'libs/imgui/LICENSE.txt',
        'json.txt': source/'libs/json/LICENSE.MIT',
        'Asio.txt': source/'libs/asio/LICENSE_1_0.txt',
        'utfcpp.txt': source/'libs/utfcpp/LICENSE',
        'SDL-hidapi.txt': source/'libs/sdl3/src/hidapi/LICENSE.txt',
        'SDL-hidapi-BSD.txt': source/'libs/sdl3/src/hidapi/LICENSE-bsd.txt',
        'SDL-hidapi-original.txt': source/'libs/sdl3/src/hidapi/LICENSE-orig.txt',
        'SDL-yuv2rgb.txt': source/'libs/sdl3/src/video/yuv2rgb/LICENSE',
    }
    for p in map(Path, nodes):
        if p.name.startswith('libGLEW'):
            paths['GLEW.txt'] = p.parent.parent/'LICENSE.txt'
        elif p.name.startswith(('libprotobuf', 'libutf8_')):
            paths['Protobuf.txt'] = p.parent.parent/'LICENSE'
        elif p.name.startswith('libabsl_'):
            paths['Abseil.txt'] = p.parent.parent/'LICENSE'
        else:
            raise RuntimeError(f'No redistribution notice mapping for {p}')
    for name, path in paths.items():
        shutil.copyfile(path, destination/name)
    stb = (source/'libs/sdl3_mixer/src/stb_vorbis/stb_vorbis.h').read_text()
    (destination/'stb-vorbis.txt').write_text(stb[stb.index('This software is available under 2 licenses'):].removesuffix('*/\n'))
    # utf8_range is a separate MIT component carried by Homebrew protobuf.
    shutil.copyfile(source/'libs/protobuf/third_party/utf8_range/LICENSE', destination/'utf8-range.txt')
    (destination/'README.txt').write_text(
        'Local unsigned packaging candidate. Original Total Annihilation assets are not included.\n'
        'Before redistribution, supply the exact corresponding modified GPL source and build scripts,\n'
        'retain third-party copyright/license notices, and review the complete binary/source inventory.\n'
        'Dependency versions and source binary hashes are recorded in packaging-manifest.json.\n'
        'No offer of source or public distribution is made by this local packaging operation.\n')


def package(app, output, source):
    # Refuse reuse, including a partially prepared previous output. No clean/delete.
    output.mkdir(parents=True, exist_ok=False)
    if not (source/'CMakeLists.txt').is_file():
        raise RuntimeError('Missing source root')
    g = graph(app)
    exe, _ = executable(app)
    if EXTERNAL_CODECS.search(run('/usr/bin/strings', '-a', exe).stdout):
        raise RuntimeError('External dlopen codec found; rebuild with RWE_STANDALONE_MACOS=ON')
    dependencies = [Path(p) for p in g['nodes'] if Path(p) != exe.resolve()]
    names = [p.name for p in dependencies]
    if len(names) != len(set(names)):
        raise RuntimeError('Dependency basename collision; manual framework layout review required')
    if any('.framework' in str(p) for p in dependencies):
        raise RuntimeError('Non-system frameworks need a reviewed full-framework packaging rule')
    dest = output/app.name
    shutil.copytree(app, dest, symlinks=True)
    frameworks = dest/'Contents/Frameworks'
    frameworks.mkdir(exist_ok=False)
    destinations = {str(exe.resolve()): executable(dest)[0]}
    for p in dependencies:
        target = frameworks/p.name
        shutil.copy2(p, target)
        target.chmod(target.stat().st_mode | 0o200)
        destinations[str(p)] = target
    for original, target in destinations.items():
        info = g['nodes'][original]
        args = ['/usr/bin/install_name_tool']
        if info['id']:
            args += ['-id', '@rpath/'+target.name]
        for edge in g['edges']:
            if edge['loader'] == original and edge['category'] != 'A':
                relative = '@executable_path/../Frameworks/' if target == executable(dest)[0] else '@loader_path/'
                args += ['-change', edge['name'], relative+Path(edge['resolved']).name]
        for rpath in info['rpaths']:
            args += ['-delete_rpath', rpath]
        if len(args) > 1:
            result = run(*args, target)
            if result.stderr:
                print(result.stderr, file=sys.stderr, end='')
    resources = dest/'Contents/Resources'
    licenses(source, [str(p) for p in dependencies], resources/'Licenses')
    files = {str(p.relative_to(dest)): digest(p) for p in sorted(dest.rglob('*')) if p.is_file()}
    metadata = {
        'schema': 1, 'signing': 'NOT SIGNED BY PACKAGER; install-name edits require owner-authorized signing',
        'minimum_os_policy': 'Explicit deployment target; not a cross-machine compatibility claim',
        'dependencies': [{'file': p.name, 'source': str(p), 'source_sha256': digest(p), **g['nodes'][str(p)]} for p in dependencies],
        'files': files,
    }
    (resources/'packaging-manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    return dest


def version(s):
    return (tuple(int(n) for n in s.split('.')) + (0, 0, 0))[:3]


def verify(app, source=None):
    errors, warnings = [], []
    exe, plist = executable(app)
    run('/usr/bin/plutil', '-lint', app/'Contents/Info.plist')
    for key in ('CFBundleExecutable', 'CFBundleIdentifier', 'CFBundleName', 'CFBundleDisplayName', 'CFBundleVersion', 'CFBundleShortVersionString'):
        if not isinstance(plist.get(key), str) or not plist[key]:
            errors.append('Missing/invalid Info.plist key: '+key)
    if plist.get('CFBundleIdentifier') != 'io.github.kuh9325.TATransplant' or plist.get('CFBundlePackageType') != 'APPL':
        errors.append('Unexpected bundle identity/package type')
    for key in ('CFBundleVersion', 'CFBundleShortVersionString'):
        if not re.fullmatch(r'\d+(\.\d+){0,2}', plist.get(key, '')):
            errors.append('Invalid numeric version: '+key)
    if not os.access(exe, os.X_OK):
        errors.append('Main executable lacks executable permission')
    if EXTERNAL_CODECS.search(run('/usr/bin/strings', '-a', exe).stdout):
        errors.append('Unbundled optional dlopen codec name in executable')
    for p in app.rglob('*'):
        if p.is_symlink() and not p.resolve().is_relative_to(app.resolve()):
            errors.append('Escaping bundle symlink: '+str(p))
        if p.suffix.lower() in ('.hpi', '.ufo', '.ccx', '.gpf', '.gp3', '.sav'):
            errors.append('Prohibited retail/save file: '+str(p))
    g = graph(app)
    for edge in g['edges']:
        if edge['category'] not in ('A', 'B'):
            errors.append('External runtime dependency: '+edge['name'])
    # Inspect every Mach-O, including files not reachable from the executable.
    for p in app.rglob('*'):
        if p.is_file() and not p.is_symlink():
            with p.open('rb') as f:
                magic = f.read(4)
            if magic in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                if str(p.resolve()) not in g['nodes']:
                    errors.append('Unreferenced Mach-O requires inventory review: '+str(p))
    for name, info in g['nodes'].items():
        if info['architecture'] != 'arm64':
            errors.append('Expected arm64-only: '+name)
        for rp in info['rpaths']:
            if not rp.startswith(('@loader_path/', '@executable_path/')) or not Path(expand(rp, Path(name), exe)).resolve().is_relative_to(app.resolve()):
                errors.append('Invalid runtime search path: '+rp)
        if not info['minos'] or version(info['minos']) > version(plist.get('LSMinimumSystemVersion', '0')):
            errors.append('Minimum OS exceeds plist or is unknown: '+name)
    resources = app/'Contents/Resources'
    for shader in SHADERS:
        path = resources/'shaders'/shader
        if not path.is_file() or path.stat().st_size == 0:
            errors.append('Missing shader: '+shader)
        elif source and digest(path) != digest(source/'shaders'/shader):
            errors.append('Stale shader: '+shader)
    icon = plist.get('CFBundleIconFile')
    icon_details = {}
    if not icon:
        warnings.append('ICON INCOMPLETE: CFBundleIconFile absent; no approved icon supplied')
    else:
        icon_errors, icon_details = verify_icon(resources, plist)
        errors.extend(icon_errors)
    manifest = resources/'packaging-manifest.json'
    if not manifest.is_file():
        errors.append('Packaging manifest missing')
    else:
        meta = json.loads(manifest.read_text())
        # Signing changes Mach-O bytes. Resource integrity remains checkable after signing.
        for name, expected in meta['files'].items():
            p = app/name
            if not p.is_file():
                errors.append('Manifest file missing: '+name)
            elif name.startswith('Contents/Resources/') and digest(p) != expected:
                errors.append('Resource hash mismatch: '+name)
    if not (resources/'Licenses/TA-Transplant-GPL.txt').is_file():
        errors.append('License notices missing')
    signing = run('/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=2', app, check=False)
    if signing.returncode:
        warnings.append('SIGNING PENDING: '+(signing.stderr or signing.stdout).strip())
    return {'structural_result': 'FAIL' if errors else 'PASS', 'release_ready': False,
            'errors': errors, 'warnings': warnings, 'signing_exit': signing.returncode,
            'icon': icon_details, 'dependency_graph': g}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=['audit', 'package', 'verify'])
    ap.add_argument('app', type=Path)
    ap.add_argument('--output', type=Path, help='New output directory (package), or JSON report (audit/verify)')
    ap.add_argument('--source', type=Path, help='Repository root for licenses and shader comparison')
    ap.add_argument('--extra', action='append', default=[], help='Additional dlopen root for original audit')
    ap.add_argument('--strict', action='store_true', help='Treat missing icon/signature as failures')
    args = ap.parse_args()
    app = args.app.resolve(strict=True)
    if args.action == 'package':
        if not args.output or not args.source:
            ap.error('package requires --output and --source')
        dest = package(app, args.output.resolve(), args.source.resolve())
        result = verify(dest, args.source.resolve())
        (args.output/'verification.json').write_text(json.dumps(result, indent=2)+'\n')
        print(dest)
    elif args.action == 'audit':
        result = graph(app, args.extra)
    else:
        result = verify(app, args.source.resolve() if args.source else None)
    if args.action != 'package' and args.output:
        args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('nodes', 'edges', 'dependency_graph')}, indent=2))
    if args.action == 'audit':
        print(f"Audited {len(result['nodes'])} Mach-O files; {len(result['edges'])} dependency edges")
    return int(bool(result.get('errors')) or bool(args.strict and result.get('warnings')))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as e:
        print('FAIL: '+str(e), file=sys.stderr)
        sys.exit(1)
