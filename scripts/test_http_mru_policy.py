#!/usr/bin/env python3
"""Compile the production HTTP route classifier with its exact dependencies.

This is a host policy fixture, not a native HTTP exchange. The extracted
classifier is unchanged source text; the wrapper supplies only pure helpers,
constants and assertions. No credential or live service request is involved.
"""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / "src/auth/http_client.c").read_text(encoding="utf8")
def function(name):
    pos = source.index(name + "(")
    start = source.rfind("\n", 0, pos) + 1
    begin = source.index("{", pos)
    depth = 1
    end = begin + 1
    while depth:
        if source[end] == "{": depth += 1
        elif source[end] == "}": depth -= 1
        end += 1
    return source[start:end]
names = ["xbox_user_url", "xsts_url", "login_urls", "https_scheme", "gssv_suffix", "titles_path", "recent_titles_path", "store_prefix", "store_suffix"]
constants=[]
for name in names:
    pos=source.index(name)
    start=source.rfind("\n",0,pos)+1
    end=source.index(";",pos)+1
    constants.append(source[start:end])
macros=[]
for line in source.splitlines():
    if any(line.startswith("#define " + n + " ") for n in ["X4_HTTP_JSON_URL_MAX", "X4_HTTP_STORE_IDS_MAX", "X4_HTTP_STORE_ID_MAX", "X4_HTTP_REGION_LABELS_MAX"]): macros.append(line)
enum_start=source.index("enum Destination {")
enum=source[enum_start:source.index(";",enum_start)+1]
program="\n".join(["#include <stdbool.h>","#include <stddef.h>","#include <string.h>","#include <stdio.h>","#include <assert.h>",*macros,*constants,enum,*[function(n) for n in ["bounded_length","alphanumeric","region_host","store_url","classify"]]])
program += r'''
int main(void) {
 const char *good[] = {
  "https://uks.core.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles"
 };
 const char *bad[] = {
  "http://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25",
  "https://region.gssv-play-prod.xboxlive.com.evil/v2/titles/mru?mr=25",
  "https://region.gssv-play-prod.xboxlive.com:443/v2/titles/mru?mr=25",
  "https://user@region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=26",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=025",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25&token=x",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25#fragment",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/mru/?mr=25",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles/%6dru?mr=25"
 };
 unsigned checks=0;
 for(size_t i=0;i<sizeof(good)/sizeof(*good);++i){assert(classify(good[i])==DEST_REGION);++checks;}
 for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i){assert(classify(bad[i])==DEST_NONE);++checks;}
 printf("production HTTP MRU route policy PASS: %u checks\n",checks); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="x4-mru-policy-") as directory:
    path=Path(directory)
    (path/"policy.c").write_text(program,encoding="utf8")
    subprocess.run([os.environ.get("CC","cc"),"-std=c11","-Wall","-Wextra","-Werror",str(path/"policy.c"),"-o",str(path/"policy")],check=True)
    subprocess.run([str(path/"policy")],check=True)
print("classifier source SHA256:",hashlib.sha256(source.encode("utf8")).hexdigest())
