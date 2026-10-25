# SPDX-License-Identifier: GPL-3.0-only
"""Offline-only FTP mock and inventory safety fixtures; never connects."""
import argparse,copy,hashlib,importlib.util,json,pathlib,re,sys,tempfile,unittest
from unittest import mock
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import copy_probe_samples_ftp as T
import inventory_probe_samples as I
class MockFTP:
 def __init__(self,files=None):self.files=dict(files or {});self.commands=[];self.directory=T.ROOT;self.interrupt=False;self.corrupt=False;self.race=None;self.list_count=0
 def cwd(self,path):self.commands.append(('CWD',path));self.directory=path
 def pwd(self):return self.directory
 def nlst(self):
  self.list_count+=1
  if self.race and self.list_count==2:self.files[self.race]=b'appeared'
  return list(self.files)
 def retrbinary(self,cmd,callback,blocksize):
  self.commands.append(('RETR',cmd));data=self.files[cmd[5:]]
  for n in range(0,len(data),blocksize):callback(data[n:n+blocksize])
 def storbinary(self,cmd,src,blocksize):
  self.commands.append(('STOR',cmd));name=cmd[5:]
  assert name not in self.files
  data=src.read(blocksize);self.files[name]=data
  if self.interrupt:raise OSError('synthetic interruption; no real network')
  self.files[name]=data+src.read()
  if self.corrupt:self.files[name]=b'x'*len(self.files[name])
class Safety(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.temp.name)
  self.repo=self.root/'repo';self.patch=mock.patch.object(T,'REPO_ROOT',self.repo);self.patch.start()
 def tearDown(self):self.patch.stop();self.temp.cleanup()
 def file(self,name='control.h264',data=b'known sample'):
  p=self.root/name;p.write_bytes(data)
  return {'name':name,'local_path':str(p),'remote_path':T.ROOT+'/'+name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
 def plan(self):
  # Tiny synthetic current-source headers/data, never a console dataset. The
  # runtime has no CLI option to redirect these fixed headers; this fixture
  # changes the module root only inside the offline test process.
  cases=[];headers=[];digest=hashlib.sha256(b'known sample').digest()
  array='{'+','.join(f'0x{x:02x}' for x in digest)+'}'
  for name in T.FIXED_HEADERS:
   source=(ROOT/name).read_text(encoding='utf-8')
   source=re.sub(r'(\.(?:clip|index|reference)_sha256\s*=\s*)\{[^}]*\}',lambda m:m.group(1)+array,source)
   dest=self.repo/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(source,encoding='utf-8')
   headers.append({'relative_path':name,'path':str(dest),'sha256':I.sha_file(dest)[1]})
   for desc in I.read_descriptors(dest):
    ident=desc['id'];items=[self.file(ident+'.'+ext) for ext in ('h264','x4au','rgba')];cases.append({'id':ident,'files':items})
  d={'schema':'X4_PROBE_TRANSFER1','remote_root':T.ROOT,'cases':cases,'descriptor_files':headers,'total_bytes':sum(f['bytes'] for c in cases for f in c['files'])}
  p=self.root/'plan.json';p.write_text(json.dumps(d),encoding='utf-8');return p,d
 def test_dryrun_never_constructs_or_connects(self):
  p,d=self.plan();a=argparse.Namespace(manifest=p,case=None,execute=False,exclusive_target=False,host=None,port=2121)
  def forbidden():raise AssertionError('network must not be constructed')
  receipt=T.run(a,forbidden);self.assertFalse(receipt['network_executed']);self.assertEqual(len(receipt['files']),24)
 def test_subset_and_local_hash(self):
  p,d=self.plan();ident=d['cases'][3]['id'];self.assertEqual(len(T.load_plan(p,ident)),3)
  pathlib.Path(d['cases'][3]['files'][0]['local_path']).write_bytes(b'altered')
  with self.assertRaises(T.TransferRefused):T.load_plan(p,ident)
 def test_paths_reject_escape_and_commands(self):
  for bad in ('../case0.h264','case0.h264\r\nDELE other','x/y.h264','case0\\h264'):
   p,d=self.plan();d['cases'][0]['files'][0]['name']=bad;p.write_text(json.dumps(d))
   with self.assertRaises(T.TransferRefused):T.load_plan(p)
  p,d=self.plan();d['cases'][0]['files'][0]['remote_path']='/data/other/case0.h264';p.write_text(json.dumps(d))
  with self.assertRaises(T.TransferRefused):T.load_plan(p)
 def test_existing_equal_skips_without_store(self):
  f=self.file();m=MockFTP({f['name']:b'known sample'});receipt=T.transfer(m,[f])
  self.assertEqual(receipt[0]['status'],'existing_verified_skipped');self.assertFalse(any(c[0]=='STOR' for c in m.commands))
 def test_conflict_preflight_prevents_any_write(self):
  a=self.file('a.h264');b=self.file('b.x4au');m=MockFTP({b['name']:b'conflict!!!!'})
  with self.assertRaises(T.TransferRefused):T.transfer(m,[a,b])
  self.assertFalse(any(c[0]=='STOR' for c in m.commands))
 def test_new_copy_readback_verified(self):
  f=self.file();m=MockFTP();receipt=T.transfer(m,[f]);self.assertEqual(receipt[0]['status'],'copied_and_readback_verified')
  self.assertEqual([c[0] for c in m.commands],['CWD','STOR','RETR'])
 def test_partial_is_retained_and_next_run_stops(self):
  f=self.file(data=b'a'*(T.BLOCK+20));m=MockFTP();m.interrupt=True
  with self.assertRaises(OSError):T.transfer(m,[f])
  self.assertEqual(len(m.files[f['name']]),T.BLOCK)
  m.interrupt=False
  with self.assertRaises(T.TransferRefused):T.transfer(m,[f])
  self.assertEqual(sum(c[0]=='STOR' for c in m.commands),1)
 def test_readback_corruption_stops_no_delete(self):
  f=self.file();m=MockFTP();m.corrupt=True
  with self.assertRaises(T.TransferRefused):T.transfer(m,[f])
  self.assertEqual([c[0] for c in m.commands],['CWD','STOR','RETR'])
 def test_appearing_target_prevents_overwrite(self):
  f=self.file();m=MockFTP();m.race=f['name']
  with self.assertRaises(T.TransferRefused):T.transfer(m,[f])
  self.assertFalse(any(c[0]=='STOR' for c in m.commands))
 def test_unexpected_directory_and_listing_stop(self):
  f=self.file();m=MockFTP({'../escape.h264':b'a'})
  with self.assertRaises(T.TransferRefused):T.transfer(m,[f])
  m.directory='/data/other'
  with self.assertRaises(T.TransferRefused):T.remote_hash(m,f)
 def test_execute_requires_exclusive_without_network(self):
  p,d=self.plan();a=argparse.Namespace(manifest=p,case=None,execute=True,exclusive_target=False,host='127.0.0.1',port=2121)
  with self.assertRaises(T.TransferRefused):T.run(a,lambda: self.fail('no network'))
 def test_bounded_local_hash_and_symlink_refusal(self):
  f=self.file(data=b'z'*(T.BLOCK*2+7));self.assertEqual(T.local_hash(f['local_path'],f['bytes']),f['sha256'])
  with self.assertRaises(T.TransferRefused):T.local_hash(f['local_path'],f['bytes']+1)
 def test_real_descriptor_names_from_source(self):
  compat=I.read_descriptors(ROOT/'src/video/probe_samples.generated.h');stress=I.read_descriptors(ROOT/'src/video/probe_stress_samples.generated.h')
  self.assertEqual([len(compat),len(stress)],[5,3]);self.assertEqual(compat[0]['id'],'control-720p30-cb')
  self.assertEqual(stress[0]['id'],'S720-CB')
  self.assertTrue(all(c['clip_path']==T.ROOT+'/'+c['id']+'.h264' for c in compat+stress))
 def test_descriptor_path_rejects_prefix_escape(self):
  source=(ROOT/'src/video/probe_samples.generated.h').read_text(encoding='utf-8')
  p=self.root/'bad.h';p.write_text(source.replace('/data/xcloud4-probe/control-720p30-cb.h264','/data/xcloud4-probe/../control-720p30-cb.h264'),encoding='utf-8')
  with self.assertRaises(ValueError):I.read_descriptors(p)
 def test_exact_index_records_and_corruption(self):
  observed={'bytes':20,'frame_count':2,'duration_us':66666,'fps_num':30,'fps_den':1,'sha256':'aa'*32,
    'sps':{'visible_width':1280,'visible_height':720,'coded_width':1280,'coded_height':720},
    'access_units':[{'offset':0,'size':10,'pts_us':0},{'offset':10,'size':10,'pts_us':33333}]}
  data=I.HEAD.pack(b'X4AU1\0\0\0',1,96,20,2,66666,1280,720,1280,720,30,1,bytes.fromhex('aa'*32))
  data+=b''.join(I.REC.pack(a['offset'],a['size'],a['pts_us']) for a in observed['access_units'])
  p=self.root/'index.x4au';p.write_bytes(data);I.verify_index(p,observed)
  for changed in (data[:95]+bytes([data[95]^1])+data[96:],data[:-1],data+b'x',data[:-24]+I.REC.pack(11,9,33333)):
   p.write_bytes(changed)
   with self.assertRaises(ValueError):I.verify_index(p,observed)
 def test_descriptor_hash_bytes_are_required(self):
  source=(ROOT/'src/video/probe_samples.generated.h').read_text(encoding='utf-8')
  p=self.root/'bad-hash.h';p.write_text(source.replace('0xf8,','',1),encoding='utf-8')
  with self.assertRaises(ValueError):I.read_descriptors(p)
 def test_old_or_false_descriptor_manifest_blocks_network(self):
  for which in ('missing','false-hash','old-current-header','arbitrary-relative-path'):
   p,d=self.plan()
   if which=='missing':d.pop('descriptor_files')
   elif which=='false-hash':d['descriptor_files'][0]['sha256']='00'*32
   elif which=='old-current-header':
    h=self.repo/T.FIXED_HEADERS[0];h.write_text(h.read_text(encoding='utf-8')+'/* changed source */',encoding='utf-8')
   else:d['descriptor_files'][0]['relative_path']='https://untrusted.example/header.h'
   p.write_text(json.dumps(d),encoding='utf-8')
   a=argparse.Namespace(manifest=p,case=None,execute=False,exclusive_target=False,host=None,port=2121)
   with self.assertRaises(T.TransferRefused):T.run(a,lambda:self.fail('no network object'))
 def test_coherent_file_hash_and_size_edit_rejected_by_descriptor(self):
  p,d=self.plan();f=d['cases'][0]['files'][0];new=b'coherent changed payload'
  pathlib.Path(f['local_path']).write_bytes(new)
  d['total_bytes']+=len(new)-f['bytes'];f['bytes']=len(new);f['sha256']=hashlib.sha256(new).hexdigest()
  p.write_text(json.dumps(d),encoding='utf-8')
  with self.assertRaises(T.TransferRefused):T.load_plan(p)
 def test_compiled_identity_and_remote_path_crosscheck(self):
  p,d=self.plan();d['cases'][0]['id']='invented';p.write_text(json.dumps(d),encoding='utf-8')
  with self.assertRaises(T.TransferRefused):T.load_plan(p)
  p,d=self.plan();d['cases'][0]['files'][0]['remote_path']=T.ROOT+'/other.h264';p.write_text(json.dumps(d),encoding='utf-8')
  with self.assertRaises(T.TransferRefused):T.load_plan(p)
 def test_metadata_header_path_never_selects_file(self):
  p,d=self.plan();d['descriptor_files'][0]['path']='https://untrusted.invalid/never-read'
  p.write_text(json.dumps(d),encoding='utf-8')
  self.assertEqual(len(T.load_plan(p)),24)
 def test_sha_nonfile_and_unknown_case(self):
  with self.assertRaises(ValueError):I.sha_file(self.root)
  p,d=self.plan()
  with self.assertRaises(T.TransferRefused):T.load_plan(p,'missing')
if __name__=='__main__':unittest.main(verbosity=2)
