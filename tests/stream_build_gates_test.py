#!/usr/bin/env python3
"""Scoped normal-video gate fixtures; no native target is executed."""
from pathlib import Path
import sys,tempfile,unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'));sys.path.insert(0,str(ROOT/'tests'))
import audit_probe_posix_elf as posix
import prepare_stream_stack_policy as policy
from probe_stack_build_gate_test import elf_fixture
from probe_posix_elf_audit_test import row,blocks
class Tests(unittest.TestCase):
    def audit(self,refs,extra=None,imports=()):
        symbols={'stream':row('stream',16),'old_auth':row('old_auth',32)}
        for name in imports:symbols[name]=row(name,undefined=True)
        if extra:symbols.update(extra)
        graph={'stream':{'address':16,'references':set(refs),'indirect_instructions':0},'old_auth':{'address':32,'references':set(),'indirect_instructions':0}}
        return posix.audit(symbols,graph,roots=('stream',),required_imports=(),indirect_roots=(),all_imports_in_scope=False)
    def test_unrelated_legacy_stub_reported_not_false_failure(self):
        d=self.audit([],imports=('mkdirat',));self.assertTrue(d['passed']);self.assertEqual(d['outside_scope_known_stub_imports'],['mkdirat'])
    def test_reachable_stub_import_rejected(self):
        d=self.audit(['mkdirat'],imports=('mkdirat',));self.assertFalse(d['passed']);self.assertEqual(d['forbidden_imports'],['mkdirat'])
    def test_no_relaxation_of_default_probe_gate(self):
        s={n:row(n,a) for n,a in [('main',16),('x4_probe_controller_run',32)]};s['mkdirat']=row('mkdirat',undefined=True)
        self.assertFalse(posix.audit(s,blocks())['passed'])
    def test_scope_cannot_hide_alias_of_stub(self):
        symbols={'stream':row('stream',16),'lstat':row('lstat',64),'other':row('other',64)}
        graph={'stream':{'address':16,'references':{'other'},'indirect_instructions':0},'other':{'address':64,'references':set(),'indirect_instructions':0}}
        d=posix.audit(symbols,graph,roots=('stream',),required_imports=(),indirect_roots=(),all_imports_in_scope=False)
        self.assertFalse(d['passed'])
    def fixture(self,tmp):
        root=Path(tmp);elf=root/'app.elf';su=root/'build';su.mkdir();out=root/'policy.json'
        names=sorted({e for _,e in policy.ENTRIES}|set(policy.ROOTS)|{'x4_probe_thread_trampoline','x4_stream_pipeline_present'})
        elf.write_bytes(elf_fixture(names));(su/'stream.su').write_text('src/video/stream_pipeline.c:1:1:stream_pipeline_decode_worker\t32\tstatic\n');(su/'disabled.su').write_text('')
        return elf,su,out
    def test_stack_policy_same_build_and_partial_main_scope(self):
        with tempfile.TemporaryDirectory() as tmp:
            e,s,o=self.fixture(tmp);p=policy.prepare(e,s,o)
            self.assertEqual(len(p['empty_disabled_translation_units']),1)
            self.assertIn('stream_pipeline_decode_worker',p['own_functions'])
            for t in p['threads']:
                if t['name']=='main':self.assertIsNone(t['stack_bytes']);self.assertEqual(t['entry'],'x4_stream_pipeline_present')
                else:self.assertEqual(t['stack_bytes'],262144)
            self.assertEqual(p['max_fraction'],.5);self.assertEqual(p['individual_frame_limit_bytes'],8192)
            policy.gate.validate_policy(p,policy.gate.sha256(e.read_bytes()),o.parent)
    def test_missing_native_entry_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            e,s,o=self.fixture(tmp);e.write_bytes(elf_fixture(('video_worker',)))
            with self.assertRaisesRegex(ValueError,'Missing linked'):policy.prepare(e,s,o)
    def test_no_same_build_usage_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            e,s,o=self.fixture(tmp);(s/'stream.su').write_text('')
            with self.assertRaisesRegex(ValueError,'No same-build'):policy.prepare(e,s,o)
if __name__=='__main__':unittest.main(verbosity=2)
