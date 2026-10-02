#!/usr/bin/env python3
"""Source/provenance and stale-header checks, with deliberate wrong-value controls."""
import contextlib
import copy
import importlib.util
import io
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
(ROOT / "re/auto").mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('descgen', ROOT / 'tools/gen-desc-xlat.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)


def array(text, name):
    body = re.search(r'\b' + name + r'\[\d+\]\s*=\s*\{([^}]+)\}', text).group(1)
    return [int(x.rstrip('u'), 0) for x in re.findall(r'0x[0-9a-f]+u?|\b[0-9]+u?', body)]


class DescriptorSources(unittest.TestCase):
    def test_captured_fixtures(self):
        text = (ROOT / 'src/xlat12/tests/fixture_desc_captures.h').read_text()
        hw = ROOT / 'notes/logs/runs/g2-1/g2out-simple/00_fragment_SimpleTextureFragment_hwshader.bin'
        captured_udes = list(struct.unpack_from('<3I', hw.read_bytes(), 0x20))
        self.assertEqual(array(text, 'fx_g2_simple_udes'), captured_udes)
        self.assertNotEqual(array(text.replace('00102130u', '00102131u'), 'fx_g2_simple_udes'), captured_udes)
        log = (ROOT / 'notes/logs/runs/r69/driverlog.txt').read_text()
        for kind, n in [('image', 8), ('sampler', 4)]:
            hits = re.findall(kind + r' record \(VRAM\) gfx10 ((?:[0-9a-f]{8} ?){' + str(n) + '})', log)
            self.assertEqual(len(hits), 1)
            want = [int(x, 16) for x in hits[0].split()]
            self.assertEqual(array(text, 'fx_r69_' + kind), want)
            bad = text.replace(f'{want[0]:08x}u', f'{want[0]^1:08x}u', 1)
            self.assertNotEqual(array(bad, 'fx_r69_' + kind), want)
            print(f'r69 {kind}: 1 captured record; injected wrong fixture FAIL; original PASS')
        print('g2-1: 1 captured hwshader / 3 UDEs; injected wrong UDE FAIL; original PASS')

    def test_format_by_name_and_missing_counterpart(self):
        d10, d12 = g.load('gfx10-rsrc.json'), g.load('gfx12-rsrc.json')
        pairs, missing = g.enum_map(d10, d12, 'GFX10_FORMAT', 'GFX11_FORMAT')
        self.assertIn((71, 57, '16_16_16_16_FLOAT'), pairs)
        bad = copy.deepcopy(d12)
        bad['enums']['GFX11_FORMAT']['entries'] = [e for e in bad['enums']['GFX11_FORMAT']['entries']
                                                  if e['name'] != 'GFX11_FORMAT_16_16_16_16_FLOAT']
        badpairs, badmissing = g.enum_map(d10, bad, 'GFX10_FORMAT', 'GFX11_FORMAT')
        self.assertNotIn(71, [p[0] for p in badpairs])
        self.assertIn('16_16_16_16_FLOAT', badmissing)
        print(f'formats: {len(pairs)} named counterparts / {len(missing)} refused; deleted counterpart -> 71 refused')

    def test_unknown_field_refusal(self):
        a = g.types_of(g.load('gfx10-rsrc.json'), 'SQ_IMG_RSRC_WORD', 'gfx10')
        b = g.types_of(g.load('gfx12-rsrc.json'), 'SQ_IMG_RSRC_WORD', 'gfx12')
        good = g.plan(a, b, 'image')
        bad = copy.deepcopy(a)
        bad.setdefault(7, []).append(('INJECTED_UNKNOWN', 0, 0, ''))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as err:
            g.plan(bad, b, 'image')
        self.assertEqual(err.exception.code, 2)
        self.assertEqual(g.plan(a, b, 'image'), good)
        print('unknown source field: injected -> REFUSED (2); removed -> PASS')

    def test_generated_header_checker(self):
        original = (ROOT / 'src/xlat12/xlat12_desc.h').read_text()
        with tempfile.TemporaryDirectory(dir=ROOT / 're/auto') as td:
            hdr = Path(td) / 'descriptor.h'
            def check(text):
                hdr.write_text(text)
                p = subprocess.run([sys.executable, str(ROOT / 'tools/gen-desc-xlat.py'), '--check', str(hdr)],
                                   capture_output=True, text=True)
                return p.returncode, p.stdout.splitlines()[-1]
            wrong = original.replace('{ 71u, 57u }', '{ 71u, 58u }')
            self.assertNotEqual(original, wrong)
            self.assertEqual(check(wrong), (1, 'header STALE'))
            self.assertEqual(check(original), (0, 'header UP TO DATE'))
            print('header checker: injected 71->58 -> STALE (exit 1); restored 71->57 -> UP TO DATE (exit 0)')


if __name__ == '__main__':
    unittest.main()
