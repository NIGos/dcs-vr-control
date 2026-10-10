"""Package checked-out source snapshots and the manager; exclude build/cache data."""
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

from product_version import product_version

root = Path(__file__).resolve().parent.parent
destination = root / f'artifacts/release/DcsControl-{product_version()}-sources.zip'
destination.parent.mkdir(parents=True, exist_ok=True)
revisions = {}
added = set()

with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    def add(path, name):
        # General-purpose SDK/build executables have their own vendor distribution.
        # Obtain them from the pinned official checkouts; keep all modified source here.
        if name.startswith('upstream/') and path.suffix.lower() in {'.exe','.dll','.lib','.pdb'}:
            return
        if path.is_file() and name not in added:
            archive.write(path, name)
            added.add(name)

    def repository(path, prefix):
        revision = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        revisions[prefix] = revision
        files = subprocess.check_output(['git', '-C', str(path), 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).decode().split('\0')
        for relative in filter(None, files):
            full = path / relative
            if full.is_dir() and (full / '.git').exists():
                repository(full, prefix + '/' + relative)
            else:
                add(full, prefix + '/' + relative)

    files = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).decode().split('\0')
    for relative in filter(None, files):
        add(root / relative, 'manager/' + relative)
    for name in ['ofxr', 'ofxr-djules75', 'cheeky', 'quadviews', 'sboys']:
        repository(root / 'external' / name, 'upstream/' + name)
    # OFXR's build dependency is a separate checkout, not an upstream gitlink.
    repository(root / 'external/ofxr/external/FidelityFX-SDK-v1.1.4', 'upstream/ofxr/external/FidelityFX-SDK-v1.1.4')
    archive.writestr('UPSTREAM_REVISIONS.json', json.dumps(revisions, indent=2))
    archive.writestr('SOURCE_README.txt', 'Local preview corresponding source. Build instructions: manager/README.md and manager/docs/THIRD_PARTY.md.\nThe Cheeky focus adapter, Quad Views settings patch and FidelityFX build fixes are included. The shipped OFXR layer is upstream/ofxr-djules75 (tag 0.2.9.1) plus manager/patches/ofxr-djules75/*.patch, applied by scripts/build-ofxr-djules75.ps1.\nUPSTREAM_REVISIONS.json lists base commits; snapshots include local modifications.\nGeneral-purpose SDK executables, binary libraries and debug symbols are excluded. Obtain MSVC, Windows SDK and the FidelityFX SDK build tools from the pinned official distributions before overlaying these source snapshots.\nSboys private functionality is not included. Vendor packages retain their own terms.\n')

digest = hashlib.sha256(destination.read_bytes()).hexdigest()
destination.with_suffix('.zip.sha256').write_text(digest + '\n', encoding='ascii')
print(f'{destination}: {len(added)} source files, {destination.stat().st_size:,} bytes')
