# SPDX-License-Identifier: GPL-3.0-only
"""Verify fixed probe descriptors against preserved local media. No network."""
import argparse, hashlib, json, pathlib, re, struct
import inspect_probe_bitstream as bitstream
BLOCK=128*1024
REMOTE_ROOT="/data/xcloud4-probe"
ID=re.compile(r"[A-Za-z0-9_-]{1,80}\Z")
HASH=re.compile(r"[0-9a-f]{64}\Z")
HEAD=struct.Struct("<8sIIQQQIIIIII32s")
REC=struct.Struct("<QQQ")
FIELDS=("visible_width","visible_height","coded_width","coded_height","crop_left","crop_top","crop_right","crop_bottom","profile_idc","level_idc","max_num_ref_frames","max_dec_frame_buffering","num_reorder_frames","chroma_format_idc","bit_depth_luma","bit_depth_chroma","matrix_coefficients","full_range","frame_mbs_only_flag")
def sha_file(path):
    path=pathlib.Path(path)
    if path.is_symlink() or not path.is_file(): raise ValueError("sample is absent, symlink or non-file")
    before=path.stat();h=hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b=f.read(BLOCK)
            if not b:break
            h.update(b)
    after=path.stat()
    if before.st_size!=after.st_size or before.st_mtime_ns!=after.st_mtime_ns:raise ValueError("sample changed during hash")
    return before.st_size,h.hexdigest()
def bounded_json(path):
    path=pathlib.Path(path)
    if path.stat().st_size>2*1024*1024:raise ValueError("manifest excessive")
    return json.loads(path.read_text(encoding="utf-8"))
def read_descriptors(path):
    path=pathlib.Path(path)
    if path.stat().st_size>128*1024:raise ValueError("descriptor excessive")
    text=path.read_text(encoding="utf-8");result=[]
    for body in re.findall(r"\{\s*\.id\s*=.*?\n\s*\},",text,re.S):
        row={}
        for key in ("id","label","clip_path","index_path","reference_path"):
            m=re.search(r"\."+key+r'\s*=\s*"([^"\\]*)"',body)
            if not m:raise ValueError("missing descriptor string")
            row[key]=m.group(1)
        if not ID.fullmatch(row["id"]):raise ValueError("unsafe descriptor ID")
        for key in ("clip_sha256","index_sha256","reference_sha256"):
            m=re.search(r"\."+key+r"\s*=\s*\{([^}]*)\}",body)
            parts=re.findall(r"0x([0-9a-fA-F]{2})(?=\s*[,}])",m.group(1)+"}" if m else "")
            if len(parts)!=32:raise ValueError("invalid descriptor hash")
            row[key]="".join(parts).lower()
        for key in FIELDS+("fps_num","fps_den","reference_count","au_count","duration_us","chosen_dpb_frames"):
            m=re.search(r"\."+key+r"\s*=\s*([0-9]+)(?:ull)?(?=\s*[,\n])",body)
            if not m:raise ValueError("missing descriptor integer")
            row[key]=int(m.group(1))
        for key,ext in (("clip_path","h264"),("index_path","x4au"),("reference_path","rgba")):
            if row[key]!=f"{REMOTE_ROOT}/{row['id']}.{ext}":raise ValueError("descriptor outside exact permitted destination")
        result.append(row)
    if not result or len(result)>8 or len({c['id'] for c in result})!=len(result):raise ValueError("descriptor count/identity invalid")
    return result

def verify_index(path,observed):
    expected=HEAD.pack(b"X4AU1\0\0\0",1,96,observed["bytes"],observed["frame_count"],observed["duration_us"],
        observed["sps"]["visible_width"],observed["sps"]["visible_height"],observed["sps"]["coded_width"],observed["sps"]["coded_height"],
        observed["fps_num"],observed["fps_den"],bytes.fromhex(observed["sha256"]))
    with pathlib.Path(path).open("rb") as f:
        if f.read(96)!=expected:raise ValueError("index header disagrees with actual clip")
        for au in observed["access_units"]:
            if f.read(24)!=REC.pack(au["offset"],au["size"],au["pts_us"]):raise ValueError("index AU/PTS differs from actual clip")
        if f.read(1):raise ValueError("index trailing bytes")

def build_inventory(repo,roots):
    repo=pathlib.Path(repo);roots=[pathlib.Path(p).resolve() for p in roots]
    headers=[repo/"src/video/probe_samples.generated.h",repo/"src/video/probe_stress_samples.generated.h"]
    groups=[read_descriptors(p) for p in headers]
    if [len(c) for c in groups]!=[5,3]:raise ValueError("expected five compatibility and three stress descriptors")
    descriptors=groups[0]+groups[1]
    if len({c['id'] for c in descriptors})!=8:raise ValueError("duplicate case ID")
    source={}
    for root in roots:
        for case in bounded_json(root/"manifest.json")["cases"]:
            name=case.get("name")
            if not isinstance(name,str) or not ID.fullmatch(name) or name in source:raise ValueError("invalid/duplicate local case")
            source[name]=(root,case)
    cases=[]
    for n,c in enumerate(descriptors):
        if c['id'] not in source:raise ValueError("descriptor has no preserved source manifest")
        root,old=source[c['id']];directory=root/c['id']
        if directory.resolve().parent!=root or directory.is_symlink():raise ValueError("unsafe local case directory")
        files=[]
        for role,name,key,remote in (("clip","clip.h264","clip_sha256","clip_path"),("index","clip.x4au","index_sha256","index_path"),("reference","references.rgba","reference_sha256","reference_path")):
            path=directory/name;size,digest=sha_file(path)
            if digest!=c[key]:raise ValueError("local file differs from compiled descriptor")
            files.append({"role":role,"local_path":str(path.resolve()),"remote_path":c[remote],"name":pathlib.PurePosixPath(c[remote]).name,"bytes":size,"sha256":digest})
        observed=bitstream.inspect_file(directory/"clip.h264",c['fps_num'],c['fps_den'])
        if observed['sha256']!=c['clip_sha256'] or observed['frame_count']!=c['au_count'] or observed['duration_us']!=c['duration_us']:raise ValueError("actual clip count/rate differs from descriptor")
        if observed['max_au_bytes']>2*1024*1024:raise ValueError("AU exceeds source bound")
        sps=observed['sps'];crop=sps['crop_offsets_pixels'];vui=sps['vui']
        actual=dict(sps,crop_left=crop[0],crop_right=crop[1],crop_top=crop[2],crop_bottom=crop[3],
            max_dec_frame_buffering=vui['max_dec_frame_buffering'],num_reorder_frames=vui['max_num_reorder_frames'],
            matrix_coefficients=vui['matrix_coefficients'],full_range=vui['full_range'])
        for key in FIELDS:
            if actual.get(key)!=c[key]:raise ValueError("actual SPS differs from descriptor: "+key)
        if sps!=old['sps'] or observed['pps']!=old['pps']:raise ValueError("preserved SPS/PPS differs from actual clip")
        if not vui['time_scale'] or not vui['num_units_in_tick'] or vui['time_scale']*c['fps_den']!=2*vui['num_units_in_tick']*c['fps_num']:raise ValueError("actual SPS timing differs from index")
        if c['chosen_dpb_frames']!=max(sps['max_num_ref_frames'],vui['max_dec_frame_buffering']) or c['reference_count']!=8:raise ValueError("DPB/reference count differs from verified source policy")
        if files[2]['bytes']!=c['reference_count']*c['visible_width']*c['visible_height']*4:raise ValueError("reference byte count invalid")
        if old['sha256']!=files[0]['sha256'] or old['index']['sha256']!=files[1]['sha256'] or old['reference']['concatenated_rgba']['sha256']!=files[2]['sha256']:raise ValueError("preserved manifest hash differs")
        verify_index(directory/"clip.x4au",observed)
        cases.append({"id":c['id'],"label":c['label'],"group":"compatibility" if n<5 else "stress","descriptor":c,"files":files,
            "observed":{k:observed[k] for k in ('frame_count','duration_us','real_bitrate_kbps','max_au_bytes','sps','pps')},"verification":"actual block hashes, SPS/AU parser and exact index; references hash/size, not native decoder correctness"})
    total=sum(f['bytes'] for c in cases for f in c['files'])
    return {"schema":"X4_PROBE_TRANSFER1","remote_root":REMOTE_ROOT,"cases":cases,"file_count":24,"total_bytes":total,"total_MiB":total/1048576,
        "original_roots":[str(p) for p in roots],"descriptor_files":[{"path":str(p),"relative_path":p.relative_to(repo).as_posix(),"sha256":sha_file(p)[1]} for p in headers],
        "stress_destination_remap":"isolated headers unify prior /data/xcloud4-probe-stress into /data/xcloud4-probe; preserved media/manifests unchanged",
        "network_executed":False,"console_verified":False}

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--baseline',type=pathlib.Path,required=True);ap.add_argument('--repo',type=pathlib.Path,required=True);ap.add_argument('--output',type=pathlib.Path,required=True)
    a=ap.parse_args();base=bounded_json(a.baseline);result=build_inventory(a.repo,base['samples_paths'])
    if a.output.exists():raise ValueError("output exists; preserve earlier inventory")
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f"Verified {result['file_count']} files; {result['total_bytes']} bytes; no network.")
if __name__=='__main__':main()
