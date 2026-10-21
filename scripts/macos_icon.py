#!/usr/bin/env python3
"""Validate actool output and the app's icon references without modifying a bundle."""
import collections
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys


def verify_icon(resources, plist):
    errors, details = [], {}
    icon = plist.get('CFBundleIconFile')
    if not isinstance(icon, str) or not icon or Path(icon).name != icon:
        return ['Missing/invalid CFBundleIconFile'], details
    name = icon.removesuffix('.icns')
    icns = resources / (name + '.icns')
    if not icns.is_file():
        errors.append('Referenced ICNS missing: ' + icns.name)
    else:
        raw = icns.read_bytes()
        if raw[:4] != b'icns' or int.from_bytes(raw[4:8], 'big') != len(raw):
            errors.append('Invalid ICNS container: ' + icns.name)
        details['icns_sha256'] = hashlib.sha256(raw).hexdigest()
    modern = plist.get('CFBundleIconName')
    details['format'] = 'Icon Composer + ICNS' if modern else 'ICNS'
    if modern:
        if modern != name:
            errors.append('CFBundleIconName disagrees with CFBundleIconFile')
        car = resources / 'Assets.car'
        if not car.is_file():
            errors.append('CFBundleIconName requires Assets.car')
        else:
            p = subprocess.run(['/usr/bin/assetutil', '--info', str(car)], capture_output=True, text=True)
            if p.returncode:
                errors.append('Cannot inspect Assets.car: ' + p.stderr.strip())
            else:
                assets = json.loads(p.stdout)
                types = collections.Counter(a.get('AssetType', 'Header') for a in assets)
                details['asset_types'] = dict(types)
                if not any(a.get('Platform') == 'macosx' for a in assets):
                    errors.append('Assets.car is not a macOS catalog')
                if not any(a.get('AssetType') == 'IconImageStack' and a.get('Name') == modern for a in assets):
                    errors.append('Named modern icon stack missing: ' + str(modern))
                if not types['Vector'] or not types['IconGroup']:
                    errors.append('Modern layered vector icon resources missing')
            details['assets_car_sha256'] = hashlib.sha256(car.read_bytes()).hexdigest()
    return errors, details


def main():
    name, directory, partial = sys.argv[1:]
    resources = Path(directory)
    plist = plistlib.loads(Path(partial).read_bytes())
    errors, details = verify_icon(resources, plist)
    if plist.get('CFBundleIconFile') != name or plist.get('CFBundleIconName') != name:
        errors.append('actool metadata differs from configured app icon')
    result = {'name': name, 'metadata': plist, 'errors': errors, **details}
    (resources / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    sys.exit(main())
