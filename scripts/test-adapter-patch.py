"""Reproduce the adapter from pristine pinned files, without changing the live checkout."""
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

root = Path(__file__).resolve().parent.parent
upstream = root / 'external/cheeky'
expected = 'd6c18b23d31f7339735114c621bc3f9066e1a32d'
revision = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
if revision != expected:
    raise RuntimeError('The reproduction check requires the pinned Cheeky revision.')

fixture = root / 'artifacts/patch-reproduction' / uuid.uuid4().hex
fixture.mkdir(parents=True)
inputs = [
    'CMakeLists.txt', 'openxr_layer/projection_selection.hpp', 'openxr_layer/openxr_layer.cpp',
    'src/gaze_foveation.hpp', 'src/gaze_foveation.cpp', 'src/hooks.cpp',
    'tests/openxr_calibration_tests.cpp', 'tests/gaze_tests.cpp', 'standalone/host.cpp', 'uevr/runtime.cpp',
    'src/d3d11_d3d12_transport.cpp', 'tests/runtime_host_tests.cpp', 'src/d3d11_d3d12_transport.hpp', 'src/dlss_nr.cpp',
    'cmake/Standalone.cmake',
]
for relative in inputs:
    path = fixture / 'external/cheeky' / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(subprocess.check_output(['git', '-C', str(upstream), 'show', f'{expected}:{relative}']))

(fixture / 'scripts').mkdir()
shutil.copyfile(root / 'scripts/patch-cheeky.py', fixture / 'scripts/patch-cheeky.py')
shutil.copytree(root / 'patches/cheeky', fixture / 'patches/cheeky')
script = fixture / 'scripts/patch-cheeky.py'
subprocess.run([sys.executable, str(script)], check=True)
outputs = inputs + ['src/dcs_quad_focus_policy.hpp', 'src/dcs_quad_focus_gate.inc', 'tests/dcs_quad_focus_layer_tests.inc',
    'standalone/dcs_nr_hotkey.hpp', 'src/dcs_feature_ledger.inc', 'src/dcs_deferred_feature.inc', 'tests/dcs_deferred_tests.inc']
first = {relative: (fixture / 'external/cheeky' / relative).read_text(encoding='utf-8') for relative in outputs}
for relative, value in first.items():
    if value != (upstream / relative).read_text(encoding='utf-8'):
        raise RuntimeError(f'Pristine-source reproduction differs from the built checkout: {relative}')

subprocess.run([sys.executable, str(script)], check=True)
for relative, value in first.items():
    if value != (fixture / 'external/cheeky' / relative).read_text(encoding='utf-8'):
        raise RuntimeError(f'Second patch changed the source: {relative}')
print(f'PASS pristine pinned-source reproduction and idempotence: {len(outputs)} files; fixture {fixture}')
