#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Final-source native A/B/A flag rebuild proof; never package or execute ELF."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

FLAGS = {
    'X4_RECEIVER_NACK': ('0', '1', 'build/streaming/rtc_transport.o'),
    'X4_NACK_GRACE_MS': ('0', '45', 'build/media/live_media.o'),
    'X4_RUMBLE': ('0', '1', 'build/core/main.o'),
    'X4_INGRESS_MODE': ('mpsc', 'baseline', 'build/media/video_ingress.o'),
    'X4_VIDEO_CAPACITY_1080': ('0', '1', 'build/video/live_h264.o'),
    'X4_VIDEO_PROBE': ('0', '1', 'build/core/main.o'),
}
DEFAULTS = {key: values[0] for key, values in FLAGS.items()}
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--flags', nargs='+', choices=list(FLAGS), default=list(FLAGS))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
if out == root or root in out.parents:
    raise SystemExit('Evidence output must be outside the frozen source tree')
out.mkdir(parents=True, exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def source_identity():
    h = hashlib.sha256()
    ignored = {'build', '.git', '__pycache__', 'dist', 'outputs', 'node_modules'}
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        if path.is_file() and not any(part in ignored for part in rel.parts):
            h.update(rel.as_posix().encode('utf-8') + b'\0')
            h.update(bytes.fromhex(sha(path)))
    return h.hexdigest()

def object_state():
    return {p.relative_to(root).as_posix(): {'sha256': sha(p), 'mtime_ns': p.stat().st_mtime_ns}
            for p in (root / 'build').rglob('*.o')}

def machine_text(path):
    """Extract actual ELF64 LE executable .text sections, excluding string/debug markers."""
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        raise AssertionError('Expected native ELF64 little-endian object: ' + str(path))
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shentsize, shnum, strndx = struct.unpack_from('<HHH', data, 58)
    if shentsize != 64 or not shnum or strndx >= shnum or shoff + shnum * 64 > len(data):
        raise AssertionError('Unsupported or malformed ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * 64) for i in range(shnum)]
    strings = sections[strndx]
    names = data[strings[4]:strings[4] + strings[5]]
    result = bytearray()
    for section in sections:
        end = names.find(b'\0', section[0])
        if end < 0:
            raise AssertionError('Malformed section name')
        name = names[section[0]:end]
        if name == b'.text' or name.startswith(b'.text.'):
            if section[4] + section[5] > len(data):
                raise AssertionError('Malformed code section')
            result.extend(name + b'\0' + data[section[4]:section[4] + section[5]])
    if not result:
        raise AssertionError('No machine-code text section')
    return bytes(result)

nm = shutil.which('llvm-nm') or shutil.which('nm')
objdump = shutil.which('llvm-objdump') or shutil.which('objdump')
if not nm or not objdump:
    raise SystemExit('Native inspection needs nm and objdump; no result is declared without them')
frozen = source_identity()
receipt = {'schema': 'X4PROBE_BUILD_SWITCH1', 'frozen_source_sha256': frozen,
           'campaigns': [], 'native_executed': False, 'package_generated': False,
           'inspection_tools': {'nm': nm, 'objdump': objdump}}

def build(config, directory):
    directory.mkdir(parents=True, exist_ok=True)
    if source_identity() != frozen:
        raise AssertionError('Source changed after the final-source campaign started')
    before = object_state()
    cmd = ['make', '-j' + str(args.jobs), 'build/xcloud4.elf'] + [key + '=' + config[key] for key in DEFAULTS]
    run = subprocess.run(cmd, cwd=root, text=True, capture_output=True)
    (directory / 'build.stdout.txt').write_text(run.stdout, encoding='utf-8')
    (directory / 'build.stderr.txt').write_text(run.stderr, encoding='utf-8')
    if run.returncode:
        raise AssertionError('Native compile/link failed: ' + str(directory))
    if source_identity() != frozen:
        raise AssertionError('Source changed during native compile/link')
    after = object_state()
    compiled = [name for name, value in after.items() if before.get(name) != value]
    stamp = (root / 'build/build_config.h').read_text(encoding='utf-8')
    expect = dict(config)
    expect.pop('X4_INGRESS_MODE')
    expect['X4_INGRESS_MODE_BASELINE'] = '1' if config['X4_INGRESS_MODE'] == 'baseline' else '0'
    for key, value in expect.items():
        if '#define %s %s\n' % (key, value) not in stamp:
            raise AssertionError('Incorrect generated flag stamp: ' + key)
    (directory / 'build_config.h').write_text(stamp, encoding='utf-8')
    elf = root / 'build/xcloud4.elf'
    elf_data = elf.read_bytes()
    if elf_data[:6] != b'\x7fELF\x02\x01':
        raise AssertionError('Linked output is not ELF64 LE')
    elf_symbols = subprocess.run([nm, str(elf)], check=True, text=True, capture_output=True).stdout
    (directory / 'ELF.symbols.txt').write_text(elf_symbols, encoding='utf-8')
    marker_start = elf_data.find(b'XCloud4')
    if marker_start < 0:
        raise AssertionError('Expected product marker absent')
    return {'command': cmd, 'returncode': run.returncode, 'configuration': config,
            'compiled_objects': compiled, 'object_states': after, 'ELF_sha256': sha(elf),
            'ELF_bytes': elf.stat().st_size, 'ELF_product_marker_present': True,
            'stamp_sha256': sha(root / 'build/build_config.h')}

try:
    for flag in args.flags:
        a, b, target = FLAGS[flag]
        baseline = dict(DEFAULTS)
        if flag == 'X4_NACK_GRACE_MS':
            baseline['X4_RECEIVER_NACK'] = '1'
        rounds = []
        for label, value in [('A1', a), ('B', b), ('A2', a), ('unchanged_A2', a)]:
            config = dict(baseline); config[flag] = value
            directory = out / flag / label
            result = build(config, directory)
            text = machine_text(root / target)
            (directory / 'functional-machine-text.bin').write_bytes(text)
            dis = subprocess.run([objdump, '-dr', str(root / target)], check=True, text=True, capture_output=True).stdout
            (directory / 'functional-object.disassembly.txt').write_text(dis, encoding='utf-8')
            symbols = subprocess.run([nm, '--undefined-only', str(root / 'build/core/main.o')],
                                     check=True, text=True, capture_output=True).stdout
            (directory / 'main.undefined-symbols.txt').write_text(symbols, encoding='utf-8')
            result.update(functional_object=target, machine_text_sha256=hashlib.sha256(text).hexdigest(),
                          machine_text_bytes=len(text))
            if flag == 'X4_VIDEO_PROBE':
                enabled = value == '1'
                if ('x4_probe_controller_run' in symbols) != enabled:
                    raise AssertionError('Probe entry call does not correspond to configured mode')
                if ('x4_auth_create' in symbols) == enabled:
                    raise AssertionError('Main authentication allocation exclusion does not match probe mode')
                result['functional_main_probe_entry'] = enabled
                result['functional_main_auth_allocation'] = not enabled
            if flag == 'X4_RUMBLE' and ('scePadSetVibration' in symbols) != (value == '1'):
                raise AssertionError('Rumble production call does not correspond to configured mode')
            (directory / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
            rounds.append(result)
        assert rounds[0]['machine_text_sha256'] == rounds[2]['machine_text_sha256']
        assert rounds[0]['machine_text_sha256'] != rounds[1]['machine_text_sha256']
        assert rounds[2]['ELF_sha256'] == rounds[3]['ELF_sha256']
        assert not rounds[3]['compiled_objects'], 'Unchanged build recompiled objects'
        assert rounds[2]['stamp_sha256'] == rounds[3]['stamp_sha256']
        receipt['campaigns'].append({'flag': flag, 'result': 'PASS', 'rounds': rounds,
            'functional_proof': 'compiled flag tuple + changed actual machine code + retained disassembly; main call exclusion for probe/rumble'})
    receipt['result'] = 'PASS'
except Exception as error:
    receipt['result'] = 'FAIL'; receipt['error'] = str(error)
finally:
    receipt['final_source_sha256'] = source_identity()
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'result': receipt['result'], 'campaigns_completed': len(receipt['campaigns']),
                  'receipt': str(out / 'receipt.json'), 'error': receipt.get('error')}))
raise SystemExit(0 if receipt['result'] == 'PASS' else 1)
