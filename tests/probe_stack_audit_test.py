#!/usr/bin/env python3
"""Deterministic realistic-stack-gate fixtures. No native ELF is executed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('stack_gate', ROOT / 'scripts/audit_probe_stack.py')
gate = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(gate)

def symbol(name, address=0x100, undefined=False):
    return {'name': name, 'address': address, 'size': 64, 'undefined': undefined}

def fixture(dump, usage='', stack=262144):
    functions = gate.parse_disassembly(dump)
    symbols = {name: symbol(name, row['address']) for name,row in functions.items()}
    policy = {'schema': gate.SCHEMA, 'max_fraction': .5,
        'threads': [{'name':'x4-video','entry':'video_worker','stack_bytes':stack}],
        'own_functions':['video_worker','hash_file'], 'sites':{}}
    return [symbols,functions,gate.parse_stack_usage(usage) if usage else {},policy]

MINIMAL = '''100 <video_worker>:
 100: push %rbp
 101: mov %rsp,%rbp
 104: sub $0x20,%rsp
 108: callq 0x200 <hash_file>
 10d: add $0x20,%rsp
 111: pop %rbp
 112: ret
200 <hash_file>:
 200: sub $0x10,%rsp
 204: add $0x10,%rsp
 208: ret
'''

class Tests(unittest.TestCase):
    def test_direct_route_known_separate_from_assumed_startup(self):
        r=gate.audit(*fixture(MINIMAL)); t=r['threads'][0]
        self.assertTrue(r['passed']);self.assertEqual(t['known_path_bytes'],72)
        self.assertEqual(t['assumption_bytes'],16384);self.assertEqual(t['modeled_total_bytes'],16456)
        self.assertIsNone(t['call_chain_bound_bytes']);self.assertEqual(t['allowed_bytes'],131072)
    def test_64k_negative_frame_fails_even_generous_capacity(self):
        r=gate.audit(*fixture(MINIMAL,'src/video/probe_hash.c:2:hash_file\t65720\tstatic\n',stack=1048576))
        self.assertFalse(r['passed']);i=next(i for i in r['issues'] if i['kind']=='individual_frame_limit_exceeded')
        self.assertEqual(i['measured_bytes'],65728)
    def test_hash_additional_root_blocks_64k_behind_unresolved_callback(self):
        dump=MINIMAL.replace('hash_file','x4_probe_hash_file').replace('callq 0x200 <x4_probe_hash_file>','callq *%rax')
        data=fixture(dump,'src/video/probe_hash.c:2:x4_probe_hash_file\t65720\tstatic\n')
        data[3]['additional_roots']=['x4_probe_hash_file']
        result=gate.audit(*data)
        self.assertFalse(result['passed'])
        issue=next(i for i in result['issues'] if i['kind']=='individual_frame_limit_exceeded')
        self.assertEqual((issue['function'],issue['measured_bytes']),('x4_probe_hash_file',65728))
        self.assertTrue(any(a['kind']=='unresolved_indirect' and a['address']==0x108 for a in result['assumptions']))
        self.assertNotIn('x4_probe_hash_file',[edge['callee'] for edge in result['call_graph']['video_worker']])
        self.assertEqual(result['additional_root_routes'][0]['known_path_bytes'],65728)
    def test_hash_additional_root_exposes_linked_callees_and_keeps_dispatch_opaque(self):
        dump=MINIMAL.replace('hash_file','x4_probe_hash_file').replace('callq 0x200 <x4_probe_hash_file>','callq *%rax')
        dump=dump.replace(' 208: ret',' 208: callq 0x300 <mbedtls_sha256_update>\n 20d: ret')
        dump+='300 <mbedtls_sha256_update>:\n 300: sub $0x40,%rsp\n 304: add $0x40,%rsp\n 308: ret\n'
        data=fixture(dump);data[3]['additional_roots']=['x4_probe_hash_file']
        result=gate.audit(*data)
        self.assertTrue(result['passed'])
        self.assertIn('mbedtls_sha256_update',result['functions'])
        self.assertEqual(result['additional_root_routes'][0]['known_path_bytes'],96)
        self.assertEqual(result['threads'][0]['assumption_bytes'],32768)
    def test_frame_limit_includes_return_slot(self):
        for count,passed in [(8184,True),(8185,False)]:
            self.assertEqual(gate.audit(*fixture(MINIMAL,f'probe.c:2:hash_file\t{count}\tstatic\n'))['passed'],passed)
    def test_half_explicit_budget_blocks(self):
        r=gate.audit(*fixture(MINIMAL,stack=32768));self.assertFalse(r['passed'])
        self.assertIn('thread_stack_budget_exceeded',[i['kind'] for i in r['issues']])
    def test_unknown_main_has_no_false_margin(self):
        data=fixture(MINIMAL);data[3]['threads'][0].update(name='main',stack_bytes=None)
        r=gate.audit(*data);self.assertTrue(r['passed'])
        for key in ['allowed_bytes','remaining_stack_bytes','remaining_to_policy_limit_bytes']:
            self.assertIsNone(r['threads'][0][key])
        self.assertEqual(r['threads'][0]['capacity_check'],'not_checked_unknown_main_stack')
    def test_unknown_default_info_has_no_false_capacity_margin(self):
        data=fixture(MINIMAL.replace('video_worker','probe_default_stack_entry'))
        data[3]['threads'][0].update(name='x4-stack-default',entry='probe_default_stack_entry',stack_bytes=None)
        r=gate.audit(*data);self.assertTrue(r['passed'])
        for key in ['allowed_bytes','remaining_stack_bytes','remaining_to_policy_limit_bytes']:
            self.assertIsNone(r['threads'][0][key])
        self.assertEqual(r['threads'][0]['capacity_check'],'not_checked_unknown_default_info_stack')
    def test_unknown_default_info_still_checks_large_frame_vla_and_recursion(self):
        for suffix,usage,kind in [('', 'src/video/probe_stack.c:1:probe_default_stack_entry\t9000\tstatic\n','individual_frame_limit_exceeded'),
            ('', 'src/video/probe_stack.c:1:probe_default_stack_entry\t128\tdynamic\n','dynamic_stack_forbidden'),
            (' 100: callq 0x100 <probe_default_stack_entry>\n','src/video/probe_stack.c:1:probe_default_stack_entry\t32\tstatic\n','own_source_recursion_forbidden')]:
            data=fixture('100 <probe_default_stack_entry>:\n'+suffix+' 105: ret\n',usage)
            data[3]['threads'][0].update(name='x4-stack-default',entry='probe_default_stack_entry',stack_bytes=None)
            result=gate.audit(*data)
            self.assertFalse(result['passed']);self.assertIn(kind,[i['kind'] for i in result['issues']])
    def test_memory_callback_additional_root_is_checked_without_new_thread(self):
        data=fixture(MINIMAL+'300 <native_map>:\n 300: ret\n','src/video/probe_memory.c:1:native_map\t9000\tstatic\n')
        data[3]['additional_roots']=['native_map']
        result=gate.audit(*data);self.assertFalse(result['passed'])
        self.assertEqual(len(result['threads']),1)
        self.assertTrue(any(i['kind']=='individual_frame_limit_exceeded' and i['function']=='native_map' for i in result['issues']))
        self.assertEqual(result['additional_root_routes'][0]['entry'],'native_map')
        self.assertEqual(result['additional_root_routes'][0]['known_path_bytes'],9008)
    def test_opaque_native_import_is_allowance_not_measured_stub(self):
        d=fixture(MINIMAL.split('200 <hash_file>:')[0].replace('hash_file','sceNative'))
        d[0]['sceNative']=symbol('sceNative',undefined=True)
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertIsNone(r['functions']['sceNative']['bytes'])
        self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
    def test_native_plt_requires_real_undefined_symbol(self):
        f=gate.parse_disassembly('100 <scePthreadAttrSetstacksize@plt>:\n 100: jmpq *0x123(%rip)\n')
        s={'scePthreadAttrSetstacksize':symbol('scePthreadAttrSetstacksize',0,True)}
        rows,issues=gate.audit_native_imports(s,f,['scePthreadAttrSetstacksize'])
        self.assertTrue(rows[0]['passed']);self.assertFalse(issues)
        s['scePthreadAttrSetstacksize']['undefined']=False
        self.assertTrue(gate.audit_native_imports(s,f,['scePthreadAttrSetstacksize'])[1])
    def test_sdk_placeholder_self_loop_cannot_pass_native_import(self):
        f=gate.parse_disassembly('100 <scePthreadAttrSetstacksize>:\n 100: endbr64\n 104: push %rbp\n 105: mov %rsp,%rbp\n 108: jmp 0x108 <scePthreadAttrSetstacksize+0x8>\n')
        rows,issues=gate.audit_native_imports({'scePthreadAttrSetstacksize':symbol('scePthreadAttrSetstacksize')},f,['scePthreadAttrSetstacksize'])
        self.assertFalse(rows[0]['passed']);self.assertTrue(issues)
    def test_native_plt_is_not_charged_eight_bytes(self):
        d=fixture(MINIMAL.split('200 <hash_file>:')[0].replace('hash_file','sceNative@plt')+'200 <sceNative@plt>:\n 200: jmpq *0x123(%rip)\n')
        d[0].pop('sceNative@plt');d[0]['sceNative']=symbol('sceNative',0,True)
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertIsNone(r['functions']['sceNative@plt']['bytes'])
        self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
    def test_unresolved_indirect_nonblocking_visible(self):
        r=gate.audit(*fixture(MINIMAL.replace('callq 0x200 <hash_file>','callq *%rax')))
        self.assertTrue(r['passed']);self.assertEqual(r['threads'][0]['known_path_bytes'],48)
        self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
        self.assertTrue(any(a['kind']=='unresolved_indirect' and a['address']==0x108 for a in r['assumptions']))
    def test_resolved_indirect_targets_expand_coverage(self):
        d=fixture(MINIMAL.replace('callq 0x200 <hash_file>','callq *%rax'))
        d[3]['sites']['video_worker@0x108']={'kind':'indirect_targets','targets':['hash_file']}
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertEqual(r['threads'][0]['known_path_bytes'],72)
    def test_missing_root_blocks(self):
        d=fixture(MINIMAL);d[3]['threads'][0]['entry']='absent'
        self.assertFalse(gate.audit(*d)['passed'])
    def test_missing_external_body_is_explicit_assumption(self):
        d=fixture(MINIMAL);del d[1]['hash_file'];del d[0]['hash_file'];d[3]['own_functions']=['video_worker']
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertIn('unknown_external',[a['kind'] for a in r['assumptions']])
    def test_missing_own_body_not_hidden_by_su_frame(self):
        d=fixture(MINIMAL,'src/video/probe_hash.c:2:hash_file\t16\tstatic\n')
        del d[1]['hash_file']
        r=gate.audit(*d);self.assertFalse(r['passed'])
        self.assertIn('incomplete_probe_frame_evidence',[i['kind'] for i in r['issues']])
    def test_own_recursion_fails_even_with_shared_exception(self):
        d=fixture(MINIMAL.replace(' 208: ret',' 208: callq 0x200 <hash_file>\n 20d: ret'))
        d[3]['shared_recursion']=[{'members':['hash_file'],'max_visits':2}]
        r=gate.audit(*d);self.assertFalse(r['passed']);self.assertIn('own_source_recursion_forbidden',[i['kind'] for i in r['issues']])
    def test_probe_tu_automatically_owned(self):
        d=fixture(MINIMAL.replace('hash_file','helper').replace(' 208: ret',' 208: callq 0x200 <helper>\n 20d: ret'),
            'src/video/probe_hash.c:2:helper\t16\tstatic\n')
        self.assertFalse(gate.audit(*d)['passed'])
    def test_probe_helpers_in_shared_project_units_cannot_use_recursion_allowance(self):
        for name in ['probe_video_batch', 'probe_source_matches', 'same_probe_mapping',
                     'x4_live_video_probe_start', 'x4_live_media_probe_create']:
            with self.subTest(name=name):
                d=fixture(MINIMAL.replace('hash_file',name).replace(' 208: ret',f' 208: callq 0x200 <{name}>\n 20d: ret'),
                    f'src/media/live_media.c:100:{name}\t16\tstatic\n')
                r=gate.audit(*d)
                self.assertFalse(r['passed'])
                self.assertTrue(r['functions'][name]['own_source'])
                self.assertIn('own_source_recursion_forbidden',[i['kind'] for i in r['issues']])
                self.assertFalse(any(a['kind']=='shared_project_recursion' for a in r['assumptions']))
    def test_library_recursion_assumption_keeps_measured_member(self):
        d=fixture(MINIMAL.replace('hash_file','frexpl').replace(' 208: ret',' 208: callq 0x200 <frexpl>\n 20d: ret'))
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertEqual(r['threads'][0]['known_path_bytes'],72)
        self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
        self.assertIn('library_recursion',[a['kind'] for a in r['assumptions']])
    def test_shared_project_recursion_named_not_third_party(self):
        d=fixture(MINIMAL.replace('hash_file','x4_module_open').replace(' 208: ret',' 208: callq 0x200 <x4_module_open>\n 20d: ret'),
            'src/core/module.c:94:x4_module_open\t16\tstatic\n')
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertIn('shared_project_recursion',[a['kind'] for a in r['assumptions']])
        d[3]['shared_recursion']=[{'members':['x4_module_open'],'max_visits':2}]
        r=gate.audit(*d);self.assertTrue(r['passed']);self.assertEqual(r['threads'][0]['known_path_bytes'],96)
        self.assertEqual(r['threads'][0]['assumption_bytes'],16384)
    def test_vla_dynamic_even_bounded_forbidden(self):
        for q in ['dynamic','dynamic,bounded']:
            r=gate.audit(*fixture(MINIMAL,f'src/video/probe_hash.c:2:hash_file\t16\t{q}\n'))
            self.assertFalse(r['passed']);self.assertIn('dynamic_stack_forbidden',[i['kind'] for i in r['issues']])
    def test_dynamic_rsp_fallback_forbidden(self):
        r=gate.audit(*fixture(MINIMAL.replace('sub $0x10,%rsp','sub %rax,%rsp')))
        self.assertFalse(r['passed'])
    def test_alloca_call_forbidden(self):
        r=gate.audit(*fixture(MINIMAL.replace('hash_file','alloca')))
        self.assertFalse(r['passed'])
    def test_duplicate_su_uses_largest(self):
        d=fixture(MINIMAL,'a.c:2:hash_file\t16\tstatic\nb.c:3:hash_file\t300\tstatic\n')
        self.assertEqual(gate.audit(*d)['functions']['hash_file']['bytes'],308)
    def test_stale_site_proof_fails(self):
        d=fixture(MINIMAL);d[3]['sites']['video_worker@0x999']={'kind':'indirect_targets','targets':['hash_file']}
        self.assertFalse(gate.audit(*d)['passed'])
    def test_mismatched_symbol_disassembly_fails(self):
        d=fixture(MINIMAL);d[0]['hash_file']['address']=0x700
        self.assertFalse(gate.audit(*d)['passed'])
    def test_wrapper_envelope_is_added_not_maxed(self):
        d=fixture(MINIMAL+'300 <wrapper>:\n 300: sub $0x10,%rsp\n 304: callq *%rax\n 308: add $0x10,%rsp\n 30c: ret\n')
        d[3]['threads'][0]['wrapper_entry']='wrapper';r=gate.audit(*d)
        self.assertEqual(r['threads'][0]['known_path_bytes'],96);self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
    def test_sequential_unknown_calls_are_max_not_total_runtime_count(self):
        d=fixture(MINIMAL.replace('callq 0x200 <hash_file>','callq *%rax\n 10c: callq *%rbx'))
        r=gate.audit(*d);self.assertEqual(r['threads'][0]['assumption_bytes'],32768)
        self.assertEqual(len([a for a in r['assumptions'] if a['kind']=='unresolved_indirect']),2)
    def test_largest_known_path_distinct_from_largest_modeled_path(self):
        text=MINIMAL.replace(' 10d: add',' 10c: callq *%rax\n 10d: add')
        text=text.replace(' 208: ret',' 208: callq 0x300 <other>\n 20d: ret')
        text+='300 <other>:\n 300: sub $0x1000,%rsp\n 304: add $0x1000,%rsp\n 308: ret\n'
        d=fixture(text,'probe.c:2:hash_file\t4096\tstatic\n')
        r=gate.audit(*d);t=r['threads'][0]
        self.assertTrue(r['passed']);self.assertGreater(t['largest_known_only_path_bytes'],t['known_path_bytes'])
        self.assertEqual(t['assumption_bytes'],32768)
    def test_static_su_parser_clang_cpp_synthetic(self):
        r=gate.parse_stack_usage('src/a.cpp:__clang_call_terminate\t8\tstatic\n')
        self.assertEqual(r['__clang_call_terminate'][0]['bytes'],8)
        self.assertFalse(r['__clang_call_terminate'][0]['source_line_available'])
        with self.assertRaisesRegex(ValueError,'malformed'):
            gate.parse_stack_usage('src/a.cpp:f\tunknown\tstatic\n')
    def test_cfg_branch_max_not_sum(self):
        f=gate.parse_disassembly('100 <f>:\n 100: test %rax,%rax\n 103: je 0x110 <f+0x10>\n 105: sub $0x20,%rsp\n 109: add $0x20,%rsp\n 10d: jmp 0x118 <f+0x18>\n 110: sub $0x40,%rsp\n 114: add $0x40,%rsp\n 118: ret\n')['f']
        self.assertEqual(gate.disassembly_frame(f)[0],72)
    def test_leaf_red_zone(self):
        f=gate.parse_disassembly('100 <f>:\n 100: mov %rax,-0x10(%rsp)\n 105: ret\n')['f']
        self.assertEqual(gate.disassembly_frame(f)[0],136)
    def test_intel_fixed_frame(self):
        f=gate.parse_disassembly('100 <f>:\n 100: push rbp\n 101: mov rbp, rsp\n 104: sub rsp, 0x20\n 108: add rsp, 0x20\n 10c: pop rbp\n 10d: ret\n')['f']
        self.assertEqual(gate.disassembly_frame(f)[0],48)
    def test_homonyms_resolve_by_address(self):
        text=MINIMAL.replace('hash_file','stage').replace(' 10d: add',' 10c: callq 0x300 <stage>\n 10d: add')
        text+='300 <stage>:\n 300: sub $0x100,%rsp\n 304: add $0x100,%rsp\n 308: ret\n'
        d=fixture(text)
        for key in ['stage@0x200','stage@0x300']:d[0][key]['name']='stage'
        r=gate.audit(*d);self.assertEqual(r['threads'][0]['known_path_bytes'],312)
    def test_policy_fixed_allowance_cannot_be_silently_reduced(self):
        p={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'opaque_allowance_bytes':0}
        with self.assertRaisesRegex(ValueError,'16384'):gate.validate_policy(p,'0'*64,Path('.'))
    def test_policy_frame_threshold_cannot_be_relaxed(self):
        p={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'individual_frame_limit_bytes':10000}
        with self.assertRaisesRegex(ValueError,'8192'):gate.validate_policy(p,'0'*64,Path('.'))
    def test_policy_elf_hash_and_fraction(self):
        with self.assertRaisesRegex(ValueError,'ELF SHA'):gate.validate_policy({'schema':gate.SCHEMA,'elf_sha256':'1'*64},'0'*64,Path('.'))
        with self.assertRaisesRegex(ValueError,'0.5'):gate.validate_policy({'schema':gate.SCHEMA,'elf_sha256':'0'*64,'max_fraction':.51},'0'*64,Path('.'))
    def test_policy_source_hash_binding(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);src=root/'explicit.h';src.write_bytes(b'fixture configured attribute 256 KiB')
            e={'kind':'configured_attribute','artifact':src.name,'sha256':gate.sha256(src.read_bytes()),'detail':'fixture attribute configuration not a native measurement'}
            p={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'threads':[{'name':'x4-video','entry':'video_worker','stack_bytes':262144,'evidence':e}]}
            gate.validate_policy(p,'0'*64,root);src.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'hash-mismatched'):gate.validate_policy(p,'0'*64,root)
    def test_unknown_worker_capacity_rejected(self):
        p={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'threads':[{'name':'x4-video','entry':'video_worker','stack_bytes':None}]}
        with self.assertRaisesRegex(ValueError,'only main'):gate.validate_policy(p,'0'*64,Path('.'))
    def test_null_attributes_info_exception_is_source_bound_and_narrow(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);source=root/'probe_stack.c';source.write_bytes(b'fixture NULL attributes only informational thread; source bound')
            evidence={'kind':'informational_null_attributes','artifact':source.name,'sha256':gate.sha256(source.read_bytes()),
                'detail':'fixture NULL attributes diagnostic does not claim a configured capacity'}
            row={'name':'x4-stack-default','entry':'probe_default_stack_entry','stack_bytes':None,'informational_only':True,'evidence':evidence}
            policy={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'threads':[row]}
            gate.validate_policy(policy,'0'*64,root)
            for patch in [{'entry':'video_worker'},{'stack_bytes':262144},{'informational_only':False},{'wrapper_entry':'x4_probe_thread_trampoline'},
                          {'evidence':{**evidence,'kind':'configured_attribute'}}]:
                with self.subTest(patch=patch),self.assertRaisesRegex(ValueError,'NULL-attributes INFO'):
                    gate.validate_policy({**policy,'threads':[{**row,**patch}]},'0'*64,root)
            source.write_bytes(b'changed source')
            with self.assertRaisesRegex(ValueError,'hash-mismatched'):
                gate.validate_policy(policy,'0'*64,root)
    def test_empty_indirect_exclusion_rejected(self):
        p={'schema':gate.SCHEMA,'elf_sha256':'0'*64,'threads':[{'name':'main','entry':'main','stack_bytes':None}],
            'sites':{'main@0x1':{'kind':'indirect_targets','targets':[]}}}
        with self.assertRaisesRegex(ValueError,'no targets'):gate.validate_policy(p,'0'*64,Path('.'))

if __name__=='__main__': unittest.main()
