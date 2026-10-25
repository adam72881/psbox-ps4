# SPDX-License-Identifier: GPL-3.0-only
"""Prepare-only by default. Future explicitly authorized FTP copy of probe files.
No deletes/overwrites/resume. Only /data/xcloud4-probe; FTP cannot guarantee
atomic create-if-absent, so execution additionally requires exclusive target use.
"""
import argparse, ftplib, getpass, hashlib, ipaddress, json, pathlib, re
import inventory_probe_samples as inventory
REPO_ROOT=pathlib.Path(__file__).resolve().parents[1]
FIXED_HEADERS=("src/video/probe_samples.generated.h","src/video/probe_stress_samples.generated.h")
ROOT="/data/xcloud4-probe"
BLOCK=128*1024
NAME=re.compile(r"[A-Za-z0-9_-]{1,80}\.(h264|x4au|rgba)\Z")
HEX=re.compile(r"[0-9a-f]{64}\Z")
class TransferRefused(ValueError):pass

def local_hash(path,limit):
    p=pathlib.Path(path)
    if p.is_symlink() or not p.is_file() or p.stat().st_size!=limit:raise TransferRefused("local sample size/type changed")
    st=p.stat();digest=hashlib.sha256()
    with p.open('rb') as f:
        while True:
            b=f.read(BLOCK)
            if not b:break
            digest.update(b)
    after=p.stat()
    if st.st_size!=after.st_size or st.st_mtime_ns!=after.st_mtime_ns:raise TransferRefused("local sample changed during verification")
    return digest.hexdigest()

def load_plan(path,case_id=None):
    p=pathlib.Path(path)
    if p.stat().st_size>2*1024*1024:raise TransferRefused("transfer manifest excessive")
    d=json.loads(p.read_text(encoding='utf-8'))
    if d.get('schema')!='X4_PROBE_TRANSFER1' or d.get('remote_root')!=ROOT:raise TransferRefused("unexpected manifest/root")
    # Only these current source headers are opened; manifest path metadata
    # never selects a header or URL. Relative keys survive a source relocation.
    header_rows=d.get('descriptor_files')
    if not isinstance(header_rows,list) or len(header_rows)!=2:raise TransferRefused("current descriptor identities missing")
    declared={}
    for row in header_rows:
        if not isinstance(row,dict) or row.get('relative_path') not in FIXED_HEADERS or row.get('relative_path') in declared:raise TransferRefused("invalid fixed descriptor identity")
        sha=row.get('sha256')
        if not isinstance(sha,str) or not HEX.fullmatch(sha):raise TransferRefused("invalid descriptor hash")
        declared[row['relative_path']]=sha
    expected={};counts=[]
    for name in FIXED_HEADERS:
        header=REPO_ROOT/name
        if inventory.sha_file(header)[1]!=declared.get(name):raise TransferRefused("current descriptor differs from old or modified plan")
        descriptors=inventory.read_descriptors(header);counts.append(len(descriptors))
        for c in descriptors:
            if c['id'] in expected:raise TransferRefused("duplicate compiled case identity")
            expected[c['id']]=c
    if counts!=[5,3]:raise TransferRefused("unexpected current descriptor catalog")
    cases=d.get('cases');files=[];ids=set();names=set()
    if not isinstance(cases,list) or len(cases)!=8:raise TransferRefused("expected eight cases")
    for c in cases:
        ident=c.get('id')
        if not isinstance(ident,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,80}',ident) or ident in ids:raise TransferRefused("invalid case identity")
        if ident not in expected:raise TransferRefused("case missing from current compiled descriptors")
        ids.add(ident);items=c.get('files')
        if not isinstance(items,list) or len(items)!=3:raise TransferRefused("expected three files percase")
        extensions=set()
        for f in items:
            name=f.get('name');size=f.get('bytes');sha=f.get('sha256');dest=f.get('remote_path');local=f.get('local_path')
            if not isinstance(name,str) or not NAME.fullmatch(name) or name in names:raise TransferRefused("unsafe/duplicate filename")
            ext=name.rsplit('.',1)[1];extensions.add(ext);names.add(name)
            if name!=ident+'.'+ext or dest!=ROOT+'/'+name:raise TransferRefused("path outside exact case destination")
            role={"h264":("clip_path","clip_sha256"),"x4au":("index_path","index_sha256"),"rgba":("reference_path","reference_sha256")}[ext]
            if dest!=expected[ident][role[0]] or sha!=expected[ident][role[1]]:raise TransferRefused("planned file path/hash differs from current compiled descriptor")
            if isinstance(size,bool) or not isinstance(size,int) or not 0<size<=2*1024*1024*1024:raise TransferRefused("invalid byte bound")
            if not isinstance(sha,str) or not HEX.fullmatch(sha) or not isinstance(local,str) or not pathlib.Path(local).is_absolute():raise TransferRefused("invalid local identity")
            if case_id is None or ident==case_id:files.append(dict(f,case_id=ident))
        if extensions!={'h264','x4au','rgba'}:raise TransferRefused("missing extension")
    if ids!=set(expected):raise TransferRefused("plan/current descriptor cases differ")
    if case_id is not None and case_id not in ids:raise TransferRefused("unknown exact caseID")
    if len(names)!=24 or sum(f['bytes'] for c in cases for f in c['files'])!=d.get('total_bytes'):raise TransferRefused("manifest total disagrees")
    for f in files:
        if local_hash(f['local_path'],f['bytes'])!=f['sha256']:raise TransferRefused("local sample hash changed")
    return files

def assert_directory(ftp):
    if ftp.pwd()!=ROOT:raise TransferRefused("FTP directory identity changed")
def listing(ftp):
    assert_directory(ftp);names=set()
    for raw in ftp.nlst():
        if not isinstance(raw,str) or '\r' in raw or '\n' in raw or '\\' in raw:raise TransferRefused("unsafe remote listing")
        name=raw[len(ROOT)+1:] if raw.startswith(ROOT+'/') else raw
        if not NAME.fullmatch(name):raise TransferRefused("remote entry not an allowed sample name")
        names.add(name)
    return names

def remote_hash(ftp,f):
    assert_directory(ftp);digest=hashlib.sha256();seen=0
    def consume(block):
        nonlocal seen
        seen+=len(block)
        if seen>f['bytes']:raise TransferRefused("remote file exceeds verified size")
        digest.update(block)
    ftp.retrbinary('RETR '+f['name'],consume,blocksize=BLOCK)
    if seen!=f['bytes']:raise TransferRefused("remote file size differs; replacement requires separate authorization")
    return digest.hexdigest()

def transfer(ftp,files):
    """Injected FTP object makes offline test coverage explicit. Caller owns close.
    Full conflict preflight precedes any STOR; no resume/delete/rename commands.
    """
    ftp.cwd(ROOT);existing=listing(ftp);skip=set()
    for f in files:
        if f['name'] in existing:
            if remote_hash(ftp,f)!=f['sha256']:raise TransferRefused("existing remote hash differs; replacement requires separate authorization")
            skip.add(f['name'])
    receipt=[]
    for f in files:
        assert_directory(ftp)
        if f['name'] in skip:
            receipt.append({'name':f['name'],'status':'existing_verified_skipped','sha256':f['sha256'],'bytes':f['bytes']});continue
        # Recheck immediately before STOR. FTP still has no atomic no-replace:
        # future execution requires no concurrent writer to this directory.
        if f['name'] in listing(ftp):raise TransferRefused("remote target appeared after preflight; no overwrite")
        if local_hash(f['local_path'],f['bytes'])!=f['sha256']:raise TransferRefused("local sample changed before upload")
        with pathlib.Path(f['local_path']).open('rb') as src:ftp.storbinary('STOR '+f['name'],src,blocksize=BLOCK)
        if local_hash(f['local_path'],f['bytes'])!=f['sha256']:raise TransferRefused("local sample changed during upload")
        if remote_hash(ftp,f)!=f['sha256']:raise TransferRefused("readback differs; stop without overwrite/delete")
        receipt.append({'name':f['name'],'status':'copied_and_readback_verified','sha256':f['sha256'],'bytes':f['bytes']})
    return receipt

def run(args,ftp_factory=ftplib.FTP):
    files=load_plan(args.manifest,args.case)
    if not args.execute:
        return {'dry_run':True,'network_executed':False,'remote_root':ROOT,'files':[{k:f[k] for k in ('name','bytes','sha256','remote_path')} for f in files],
            'required_bytes_if_absent':sum(f['bytes'] for f in files),'future_condition':'explicit authorization plus exclusive target; existing directory and collision verification required'}
    if not args.exclusive_target:raise TransferRefused("execute requires explicit exclusive-target acknowledgment")
    if not args.host:raise TransferRefused("execute requires explicit console IP; no discovery")
    ipaddress.ip_address(args.host)
    if not 1<=args.port<=65535:raise TransferRefused("invalid port")
    user=getpass.getpass('Usuario FTP (no se registra): ');password=getpass.getpass('Contraseña FTP (no se registra): ')
    if any(c in user+password for c in ('\r','\n')):raise TransferRefused("invalid credential characters")
    ftp=ftp_factory()
    try:
        ftp.connect(args.host,args.port,timeout=15);ftp.login(user,password)
        files_result=transfer(ftp,files)
        return {'dry_run':False,'network_executed':True,'remote_root':ROOT,'files':files_result}
    finally:
        user=password=''
        ftp.close() # No raw server/credential exception text in the CLI receipt.
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--manifest',type=pathlib.Path,required=True);ap.add_argument('--case',help='exact compiled caseID; omit for all24files')
    ap.add_argument('--execute',action='store_true',help='future explicit authorization only; absent means no FTP');ap.add_argument('--exclusive-target',action='store_true',help='acknowledge no concurrent writer; FTP has no atomic no-replace')
    ap.add_argument('--host',help='explicit console IP for future execute only');ap.add_argument('--port',type=int,default=2121)
    a=ap.parse_args()
    try:print(json.dumps(run(a),ensure_ascii=False,indent=2))
    except Exception as exc:
        # Server exceptions expose only their class. Our own refusal messages
        # are fixed strings and can explain the need for separate authorization.
        result={'success':False,'error_class':type(exc).__name__,
            'action':'Stopped. Partial files remain. No overwrite, deletion or automatic retry; any conflicting target needs separate authorization.'}
        if isinstance(exc,TransferRefused):result['reason']=str(exc)
        print(json.dumps(result));raise SystemExit(1)
if __name__=='__main__':main()
