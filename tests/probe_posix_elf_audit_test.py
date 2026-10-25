#!/usr/bin/env python3
"""Offline fixture tests for native POSIX ELF gate; no console execution."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("audit", ROOT / "scripts/audit_probe_posix_elf.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)

def row(name, address=0, undefined=False, binding=1):
    return {"name":name,"address":address,"undefined":undefined,"size":8,"symbol_kind":2,"symbol_binding":binding}

def blocks(**references):
    references.setdefault('main',(16,[]))
    references.setdefault('x4_probe_controller_run',(32,[]))
    return {name:{"address":address,"references":set(refs),"indirect_instructions":0}
        for name,(address,refs) in references.items()}

def diagnostic_fixture():
    """The exact selfcheck -> one-call wrapper -> libc -> native route."""
    entries = [("main",16),("x4_probe_controller_run",32),
        ("x4_probe_files_selfcheck",48),("x4_probe_files_openat_info",64),("openat",80)]
    symbols = {name:row(name,at) for name,at in entries}
    symbols["_openat"] = row("_openat",undefined=True)
    graph = blocks(main=(16,["x4_probe_controller_run"]),
        x4_probe_controller_run=(32,["x4_probe_files_selfcheck"]),
        x4_probe_files_selfcheck=(48,["x4_probe_files_openat_info"]),
        x4_probe_files_openat_info=(64,["openat"]), openat=(80,["_openat"]))
    graph["x4_probe_files_openat_info"]["call_sites"] = ["openat"]
    return symbols, graph

class Tests(unittest.TestCase):
    def test_reachable_linked_old_lstat_fails(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("lstat",64),("fstatat",80)]}
        result=audit.audit(symbols,blocks(main=(16,["x4_probe_controller_run"]),x4_probe_controller_run=(32,["lstat"]),lstat=(64,["fstatat"])))
        self.assertFalse(result["passed"])
        self.assertEqual({r["name"] for r in result["reachable_linked_stubs"]},{"lstat","fstatat"})
    def test_forbidden_import_fails_even_outside_direct_references(self):
        symbols={"main":row("main",16),"x4_probe_controller_run":row("x4_probe_controller_run",32),"mkdirat":row("mkdirat",undefined=True)}
        self.assertEqual(audit.audit(symbols,blocks(main=(16,[])))['forbidden_imports'],["mkdirat"])
    def test_unreachable_normal_lstat_can_remain(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("normal_auth",40),("lstat",64)]}
        result=audit.audit(symbols,blocks(main=(16,["x4_probe_controller_run"]),x4_probe_controller_run=(32,["sceKernelOpen"]),normal_auth=(40,["lstat"])))
        self.assertTrue(result["passed"]);self.assertEqual(result['unreachable_linked_stub_names'],["lstat"])
    def test_callback_worker_is_root(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("copy_worker",48),("fdatasync",64)]}
        self.assertFalse(audit.audit(symbols,blocks(main=(16,[]),copy_worker=(48,["fdatasync"])))['passed'])
    def test_FILE_seek_is_root(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("__stdio_seek",48),("fstatat",64)]}
        self.assertFalse(audit.audit(symbols,blocks(main=(16,[]),__stdio_seek=(48,["fstatat"])))['passed'])
    def test_alias_address_cannot_hide_stub(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("lstat",64),("lstat64",64)]}
        result=audit.audit(symbols,blocks(main=(16,["x4_probe_controller_run"]),x4_probe_controller_run=(32,["lstat64"]),lstat=(64,[])))
        self.assertFalse(result['passed'])
    def test_unlisted_alias_sharing_ENOSYS_body_cannot_hide_stub(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("fstatat",64),("__fstatat",64)]}
        result=audit.audit(symbols,blocks(main=(16,["x4_probe_controller_run"]),x4_probe_controller_run=(32,["__fstatat"]),__fstatat=(64,[])))
        self.assertFalse(result['passed'])
        stub=next(r for r in result['reachable_linked_stubs'] if r['name']=='fstatat')
        self.assertEqual(stub['route'],['main','x4_probe_controller_run','__fstatat','fstatat'])
    def test_required_native_imports_exist_in_actual_symbol_table(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32)]}
        for name in audit.REQUIRED_NATIVE_IMPORTS:symbols[name]=row(name,undefined=True)
        result=audit.audit(symbols,blocks(),required_imports=audit.REQUIRED_NATIVE_IMPORTS)
        self.assertTrue(result['passed']);self.assertEqual(result['missing_required_native_imports'],[])
    def test_missing_required_native_import_fails(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32)]}
        result=audit.audit(symbols,blocks(),required_imports=['geteuid'])
        self.assertFalse(result['passed']);self.assertEqual(result['missing_required_native_imports'],['geteuid'])
    def test_same_named_linked_body_is_not_native_import_proof(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("geteuid",64)]}
        result=audit.audit(symbols,blocks(),required_imports=['geteuid'])
        self.assertFalse(result['passed']);self.assertEqual(result['missing_required_native_imports'],['geteuid'])
    def test_cycle_terminates(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32)]}
        self.assertTrue(audit.audit(symbols,blocks(main=(16,["x4_probe_controller_run"]),x4_probe_controller_run=(32,["main"])))['passed'])
    def test_missing_root_fails_closed(self):
        with self.assertRaises(ValueError):audit.audit({"main":row("main",16)},blocks(main=(16,[])))
    def test_partial_main_only_disassembly_fails_closed(self):
        symbols={"main":row("main",16),"x4_probe_controller_run":row("x4_probe_controller_run",32)}
        with self.assertRaisesRegex(ValueError,'incomplete disassembly'):
            audit.audit(symbols,{'main':{'address':16,'references':set(),'indirect_instructions':0}})
    def test_malformed_ELF_fails(self):
        with self.assertRaises(ValueError):audit.elf_symbols(b"not an ELF")
    def test_objdump_calls_tail_jumps_callback_offsets_and_indirect(self):
        parsed=audit.parse_disassembly('''0000000000000010 <main>:
 10: e8 00 00 00 00 callq 0x20 <x4_probe_controller_run>
 15: e9 00 00 00 00 jmpq 0x30 <lstat@plt>
 20: 48 8d 05 00 00 00 00 leaq (%rip), %rax # 0x40 <copy_worker+0x10>
 24: ff d0 callq *%rax
0000000000000030 <lstat>:
 30: e9 00 00 00 00 jmp 0x40 <fstatat>
''')
        self.assertEqual(parsed['main']['references'],{'x4_probe_controller_run','lstat','copy_worker'})
        self.assertEqual(parsed['main']['indirect_instructions'],1)
    def test_project_syscall_adapter_not_generic_ban(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32),("syscall",64)]}
        self.assertTrue(audit.audit(symbols,blocks(main=(16,["syscall"])))['passed'])
    def test_native_filesystem_imports_retained(self):
        symbols={name:row(name,at) for name,at in [("main",16),("x4_probe_controller_run",32)]}
        for name in ['open','fstat','sceKernelFsync','read','write']:symbols[name]=row(name,undefined=True)
        self.assertTrue(audit.audit(symbols,blocks(main=(16,list(symbols))))['passed'])
    def test_native_requirements_are_minimal_direct_path(self):
        self.assertEqual(audit.REQUIRED_NATIVE_IMPORTS,('fstat','sceKernelFsync'))
    def test_exact_diagnostic_openat_tail_chain_is_allowed(self):
        symbols, graph = diagnostic_fixture()
        result = audit.audit(symbols,graph)
        self.assertTrue(result['passed'])
        self.assertTrue(result['openat_diagnostic_exception']['body_verified'])
        self.assertEqual(result['forbidden_indirect_selfcheck_routes'],[])
        self.assertEqual(result['openat_diagnostic_exception']['observed_callers'],['x4_probe_files_selfcheck'])
        self.assertIn(['main','x4_probe_controller_run','x4_probe_files_selfcheck',
            'x4_probe_files_openat_info','openat','_openat'],result['openat_diagnostic_exception']['allowed_routes'])
    def test_functional_openat_is_not_hidden_by_prior_diagnostic_visit(self):
        symbols, graph = diagnostic_fixture()
        symbols['z_functional_clip'] = row('z_functional_clip',96)
        graph['x4_probe_controller_run']['references'].add('z_functional_clip')
        graph['z_functional_clip'] = {'address':96,'references':{'openat'},'indirect_instructions':0}
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn(['main','x4_probe_controller_run','z_functional_clip','openat'],
            [item['route'] for item in result['forbidden_effective_paths']])
    def test_functional_visit_cannot_block_later_diagnostic_evidence(self):
        symbols, graph = diagnostic_fixture()
        graph['main']['references'].add('openat')
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertTrue(result['openat_diagnostic_exception']['allowed_routes'])
    def test_openat_native_import_is_forbidden_on_functional_path(self):
        for name in ['openat','_openat','__openat','openat64','_openat64','__openat64']:
            with self.subTest(name=name):
                symbols={'main':row('main',16),'x4_probe_controller_run':row('x4_probe_controller_run',32),name:row(name,undefined=True)}
                result=audit.audit(symbols,blocks(main=(16,[name])))
                self.assertFalse(result['passed']);self.assertEqual(result['forbidden_effective_paths'][0]['name'],name)
    def test_openat64_import_is_not_diagnostic_exception(self):
        for name in ['openat64','_openat64','__openat64']:
            with self.subTest(name=name):
                symbols, graph = diagnostic_fixture()
                symbols[name] = row(name,undefined=True)
                graph['x4_probe_files_openat_info']['references'] = {name}
                graph['x4_probe_files_openat_info']['call_sites'] = [name]
                result = audit.audit(symbols,graph)
                self.assertFalse(result['passed'])
                self.assertFalse(result['openat_diagnostic_exception']['body_verified'])
                self.assertIn(name,[item['name'] for item in result['forbidden_effective_paths']])
    def test_openat64_linked_alias_is_not_diagnostic_exception(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat64'] = row('openat64',80,binding=2)
        graph['x4_probe_files_openat_info']['references'] = {'openat64'}
        graph['x4_probe_files_openat_info']['call_sites'] = ['openat64']
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertFalse(result['openat_diagnostic_exception']['body_verified'])
        self.assertIn('openat64',[item['name'] for item in result['forbidden_effective_paths']])
    def test_passive_SDK_openat64_weak_alias_does_not_block_info(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat64'] = row('openat64',80,binding=2)
        result = audit.audit(symbols,graph)
        self.assertTrue(result['passed'])
        self.assertEqual(result['openat_diagnostic_exception']['passive_same_address_weak_aliases'],[
            {'name':'openat64','explicit_reference':'openat','address':'0x50','symbol_binding':2}])
        self.assertNotIn('openat64',[route[-1] for route in result['openat_diagnostic_exception']['allowed_routes']])
    def test_disassembly_weak_alias_header_preserves_strong_call_target(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat64'] = row('openat64',80,binding=2)
        del graph['openat']
        graph['openat64'] = {'address':80,'references':{'_openat'},'indirect_instructions':0}
        result = audit.audit(symbols,graph)
        self.assertTrue(result['passed'])
        self.assertIn(['main','x4_probe_controller_run','x4_probe_files_selfcheck',
            'x4_probe_files_openat_info','openat','_openat'],result['openat_diagnostic_exception']['allowed_routes'])
    def test_weak_alias_explicit_functional_call_still_fails(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat64'] = row('openat64',80,binding=2)
        graph['main']['references'].add('openat64')
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn(['main','openat64'],[item['route'] for item in result['forbidden_effective_paths']])
    def test_passive_alias_requires_exact_weak_binding_and_same_address(self):
        for address,binding in [(80,1),(96,2)]:
            with self.subTest(address=address,binding=binding):
                symbols, graph = diagnostic_fixture()
                symbols['openat64'] = row('openat64',address,binding=binding)
                if address != 80:
                    graph['openat']['references'].add('openat64')
                self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_alias_exception_never_hides_ENOSYS(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat64'] = row('openat64',80,binding=2)
        symbols['fstatat'] = row('fstatat',80,binding=2)
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn('fstatat',[item['name'] for item in result['reachable_linked_stubs']])
    def test_unlisted_same_address_alias_cannot_be_diagnostic_target(self):
        symbols, graph = diagnostic_fixture()
        symbols['sdk_other_open'] = row('sdk_other_open',80,binding=2)
        graph['x4_probe_files_openat_info']['references'] = {'sdk_other_open'}
        graph['x4_probe_files_openat_info']['call_sites'] = ['sdk_other_open']
        self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_linked_alias_of_openat_cannot_hide_functional_call(self):
        symbols, graph = diagnostic_fixture()
        symbols['openat_alias'] = row('openat_alias',80)
        graph['main']['references'].add('openat_alias')
        result=audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn('openat_alias',[item['name'] for item in result['forbidden_effective_paths']])
    def test_diagnostic_wrapper_unapproved_caller_is_rejected(self):
        symbols, graph=diagnostic_fixture()
        graph['main']['references'].add('x4_probe_files_openat_info')
        result=audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn('diagnostic wrapper referenced by unapproved caller: main',result['openat_diagnostic_exception']['errors'])
    def test_callback_reference_to_diagnostic_wrapper_is_rejected(self):
        symbols, graph=diagnostic_fixture()
        symbols['video_worker']=row('video_worker',96)
        graph['video_worker']={'address':96,'references':{'x4_probe_files_openat_info'},'indirect_instructions':0}
        self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_worker_cannot_reach_selfcheck_even_when_main_has_visited_it(self):
        symbols, graph = diagnostic_fixture()
        symbols['video_worker'] = row('video_worker',96)
        graph['video_worker'] = {'address':96,'references':{'x4_probe_files_selfcheck'},'indirect_instructions':0}
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertEqual(result['forbidden_indirect_selfcheck_routes'],[
            {'origin':'video_worker','route':['video_worker','x4_probe_files_selfcheck']}])
    def test_all_worker_and_FILE_roots_are_independently_checked(self):
        for origin in audit.INDIRECT_ROOTS:
            with self.subTest(origin=origin):
                symbols, graph = diagnostic_fixture()
                symbols[origin] = row(origin,96)
                graph[origin] = {'address':96,'references':{'x4_probe_controller_run'},'indirect_instructions':0}
                result = audit.audit(symbols,graph)
                self.assertFalse(result['passed'])
                self.assertEqual(result['forbidden_indirect_selfcheck_routes'],[
                    {'origin':origin,'route':[origin,'x4_probe_controller_run','x4_probe_files_selfcheck']}])
    def test_worker_selfcheck_alias_cannot_hide_route(self):
        symbols, graph = diagnostic_fixture()
        symbols['copy_worker'] = row('copy_worker',96)
        symbols['selfcheck_alias'] = row('selfcheck_alias',48,binding=2)
        graph['copy_worker'] = {'address':96,'references':{'selfcheck_alias'},'indirect_instructions':0}
        result = audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertEqual(result['forbidden_indirect_selfcheck_routes'][0]['route'],
            ['copy_worker','selfcheck_alias','x4_probe_files_selfcheck'])
    def test_worker_with_cycle_but_no_selfcheck_remains_allowed(self):
        symbols, graph = diagnostic_fixture()
        symbols['video_worker'] = row('video_worker',96)
        symbols['worker_helper'] = row('worker_helper',112)
        graph['video_worker'] = {'address':96,'references':{'worker_helper'},'indirect_instructions':0}
        graph['worker_helper'] = {'address':112,'references':{'video_worker'},'indirect_instructions':0}
        result = audit.audit(symbols,graph)
        self.assertTrue(result['passed'])
        self.assertEqual(result['forbidden_indirect_selfcheck_routes'],[])
    def test_wrapper_call_site_evidence_required(self):
        symbols, graph=diagnostic_fixture()
        del graph['x4_probe_files_openat_info']['call_sites']
        result=audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn('diagnostic wrapper call-site evidence is missing',result['openat_diagnostic_exception']['errors'])
    def test_two_openat_calls_in_wrapper_rejected(self):
        symbols, graph=diagnostic_fixture()
        graph['x4_probe_files_openat_info']['call_sites']=['openat','openat']
        self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_indirect_openat_wrapper_rejected(self):
        symbols, graph=diagnostic_fixture()
        graph['x4_probe_files_openat_info']['indirect_instructions']=1
        self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_wrapper_cannot_whitelist_additional_helper(self):
        symbols, graph=diagnostic_fixture()
        graph['x4_probe_files_openat_info']['references'].add('helper')
        self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_openat_libc_helper_does_not_inherit_exception(self):
        symbols, graph=diagnostic_fixture()
        symbols['__errno_location']=row('__errno_location',96)
        graph['openat']['references'].add('__errno_location')
        graph['__errno_location']={'address':96,'references':{'_openat'},'indirect_instructions':0}
        result=audit.audit(symbols,graph)
        self.assertFalse(result['passed'])
        self.assertIn('__errno_location',result['forbidden_effective_paths'][0]['route'])
    def test_fchmod_native_and_libc_aliases_always_forbidden(self):
        for name in ['fchmod','_fchmod','__fchmod','sceKernelFchmod']:
            with self.subTest(name=name):
                symbols, graph=diagnostic_fixture()
                symbols[name]=row(name,undefined=True)
                graph['openat']['references'].add(name)
                self.assertFalse(audit.audit(symbols,graph)['passed'])
    def test_ENOSYS_alias_imports_not_exempted_by_diagnostic(self):
        for name in ['fstatat','_fstatat','__fstatat','fstatat64','__lstat','_mkdirat']:
            with self.subTest(name=name):
                symbols, graph=diagnostic_fixture()
                symbols[name]=row(name,undefined=True)
                graph['openat']['references'].add(name)
                result=audit.audit(symbols,graph)
                self.assertFalse(result['passed']);self.assertIn(name,result['forbidden_imports'])
    def test_ENOSYS_alias_body_not_exempted_by_diagnostic(self):
        symbols, graph=diagnostic_fixture()
        symbols['__fstatat']=row('__fstatat',96)
        graph['openat']['references'].add('__fstatat')
        graph['__fstatat']={'address':96,'references':set(),'indirect_instructions':0}
        result=audit.audit(symbols,graph)
        self.assertFalse(result['passed']);self.assertEqual(result['reachable_linked_stubs'][0]['name'],'__fstatat')
    def test_no_exception_when_wrapper_is_missing(self):
        symbols={'main':row('main',16),'x4_probe_controller_run':row('x4_probe_controller_run',32),'openat':row('openat',undefined=True)}
        result=audit.audit(symbols,blocks(main=(16,['openat'])))
        self.assertFalse(result['passed'])
        self.assertFalse(result['openat_diagnostic_exception']['body_verified'])
    def test_unreachable_normal_fchmod_is_not_probe_path(self):
        symbols={'main':row('main',16),'x4_probe_controller_run':row('x4_probe_controller_run',32),
            'normal_auth':row('normal_auth',48),'sceKernelFchmod':row('sceKernelFchmod',undefined=True)}
        result=audit.audit(symbols,blocks(normal_auth=(48,['sceKernelFchmod'])))
        self.assertTrue(result['passed'])
    def test_objdump_verified_single_call_and_tail_jump(self):
        for instruction in ['callq','jmpq']:
            with self.subTest(instruction=instruction):
                symbols, graph=diagnostic_fixture()
                parsed=audit.parse_disassembly('''0000000000000040 <x4_probe_files_openat_info>:
 40: e8 00 00 00 00 '''+instruction+''' 0x50 <openat>
''')
                graph['x4_probe_files_openat_info']=parsed['x4_probe_files_openat_info']
                self.assertTrue(audit.audit(symbols,graph)['passed'])
    def test_wrapper_shared_address_with_forbidden_function_rejected(self):
        symbols, graph=diagnostic_fixture()
        symbols['fchmod']=row('fchmod',64)
        self.assertFalse(audit.audit(symbols,graph)['passed'])

if __name__=='__main__':unittest.main()
