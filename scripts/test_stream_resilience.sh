#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Real raw adapter + scheduler/ring/converter/exporter; host native substitutes.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
out=${X4_RESILIENCE_TEST_OUTPUT:-$(mktemp -d /tmp/x4-resilience.XXXXXX)}
if [[ -z ${X4_RESILIENCE_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$out"' EXIT;fi
mkdir -p "$out"
for profile in 1 2;do
 for variant in plain sanitized;do
  destination="$out/profile-$profile-$variant";mkdir -p "$destination"
  flags=(-std=gnu2x -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections
   -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 -DX4_STREAM_PROFILE="$profile" -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1
   -Itests/video_capacity_stubs -Itests/host_stubs)
  if [[ $variant == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
  "${CC:-gcc}" "${flags[@]}" tests/stream_resilience_integration_test.c \
   src/video/video_capacity.c src/video/video_pipeline.c src/video/decoder_depth.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_sps.c src/video/stream_diagnostics.c src/video/stream_color_policy.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c \
   src/media/live_trace.c src/media/live_monitor.c src/media/video_ingress.c \
   -Wl,--gc-sections -o "$destination/integration"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 180s "$destination/integration" "$destination"
  python3 - "$root" "$destination" "$profile" <<'PY'
import importlib.util,json,pathlib,sys
root,out=map(pathlib.Path,sys.argv[1:3]);profile=int(sys.argv[3])
p=root/'scripts/analyze_stream_pipeline.py';spec=importlib.util.spec_from_file_location('real_stream_reader',p)
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
reports=sorted(out.glob('xcloud4-stream-*-p*-*.json'))
assert len(reports)==23,(len(reports),reports)
for p in reports:
 d=json.loads(p.read_text());result=reader.analyze(d)
 assert d['schema']=='X4STREAM_PIPELINE5'and d['profile']==(1 if profile==2 else 0)
 assert d['status']['presentations']==result['presentation']['count']
 assert all(t is None or t['configured_bytes']==262144 for t in d['stacks'])
 case=int(p.stem.rsplit('-',1)[-1])
 assert d['status']['decoder_close']==(-10103 if case in (109,206) else 0)
 assert d['status']['retained']==(case in (109,112,206))
 assert d['status']['display_detached']==(case!=112)
 assert not any(w in p.read_text().lower()for w in ('gstoken','bearer','authorization','password'))
 if case in (109,206):assert d['status']['error']==-10103
 if case==112:assert d['status']['error']==-10105
 if case==104:assert d['decoder_start']['attempt_count']==3
 if case in (102,115):assert d['status']['error']==-7432
 if case==110:assert d['status']['error']==-7430
 if case==114:
  assert d['status']['error']==0 and d['recovery']['idr_wait']['recoveries']==1
  assert any(r['duration_us']==2000000 and r['classification']=='waiting_idr' and r['evidence_mask']==2
             for r in d['metrics']['pause_evidence']['records'])
 if case==107:assert d['status']['error']==(-7433 if profile==2 else -7413)
 depth=d['decoder_depth'];stages=d['metrics']['stages']
 assert not depth['flush_on_close'] and not depth['input_contract_SDK_verified']
 assert depth['calls_with_output']==stages['decode_with_output']['count']
 assert depth['calls_without_output']==stages['decode_without_output']['count']
 assert depth['calls_with_output']+depth['calls_without_output']==stages['decode']['count']
 if case<200:
  assert depth['selection']==1
 elif case==204:
  assert depth['selection']==0 and depth['active']==1 and depth['output_buffers']==4
  assert [epoch['decoder_start']['active_depth'] for epoch in d['recovery']['epochs']]==[1,2,1]
 elif case in (207,208):
  assert depth['selection']==2 and depth['requested']==2 and depth['active']==1 and depth['fallback']
  assert depth['input_buffers']==2 and depth['output_buffers']==4
  assert [attempt['depth'] for attempt in d['decoder_start']['attempts']]==[2,1]
 else:
  assert depth['selection']==2 and depth['requested']==2 and depth['active']==2 and not depth['fallback']
  assert depth['input_buffers']==4 and depth['output_buffers']==5 and depth['input_retirement_posterior_calls']==3
  assert depth['calls_without_output']>=1 and depth['native_budget_bytes']==192*1024*1024
 if case>=200 and case!=206:
  assert d['status']['error']==0 and depth['held_output_mask_after_close']==depth['live_inputs_after_close']==0
 if case==201:
  assert depth['calls_without_output']==1 and depth['calls_with_output']==79
  assert stages['native_output_interval']['count']==78 and d['status']['native_outputs']==79
 if case==202:
  assert d['recovery']['waiting_idr_drops']==1 and d['recovery']['frame_errors']==1
 if case==203:
  assert depth['calls_without_output']==3 and d['recovery']['restarts']==1
 if case==206:
  assert depth['held_output_mask_after_close'].bit_count()==2 and depth['live_inputs_after_close']==2
 print('PASS real reader accepted real integration export',case)
print('PASS 23 real exports per profile: fifteen depth1 regressions plus depth2 continuous/IDR/BAD_FRAME/restart/Auto SPS/cancel/Delete failure/Query/Create fallback')
PY
 done
done
printf 'PASS real resilience integration B2-B6 profiles1/2 plain+ASan/UBSan; native/kernel/thread/clock/VideoOut are host doubles.\n'
