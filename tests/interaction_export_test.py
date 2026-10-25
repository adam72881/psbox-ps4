#!/usr/bin/env python3
"""Exercise new export glue and existing reader; never run a video pipeline.

Argument: a previously generated, valid host export fixture. Child metric
writers are doubles here; their actual JSON writers have dedicated tests.
"""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
media = (root / 'src/media/live_media.c').read_text(encoding='utf-8')
pipeline = (root / 'src/video/stream_pipeline.c').read_text(encoding='utf-8')
start = media.index('static X4LiveAudioSnapshot interaction_audio_final;')
end = media.index('\n#endif\nint x4_live_media_close', start)
assert pipeline.count('fputs(",\\\"interaction\\\":",f);x4_live_interaction_metrics_write(f);') == 1
assert '"X4STREAM_PIPELINE5' in pipeline.replace('\\"', '"')
closing = media[media.index('int x4_live_media_close('):]
assert closing.index('x4_live_audio_stop_report(m->audio,&audio_snapshot)') < closing.index('interaction_audio_final=audio_snapshot;') < closing.index('x4_stream_pipeline_export(')
prefix = r'''
#include <stdbool.h>
#include <stdio.h>
typedef struct {unsigned unused;} X4LiveAudioSnapshot;
void x4_rtc_input_metrics_write(FILE *f){fputs("{\"test_child\":\"input\"}",f);}
void x4_main_interaction_metrics_write(FILE *f){fputs("{\"test_child\":\"main_rumble\"}",f);}
void x4_live_audio_metrics_write(FILE *f,const X4LiveAudioSnapshot *s){
 (void)s;fputs("{\"test_child\":\"audio\"}",f);
}
'''
with tempfile.TemporaryDirectory(prefix='x4-interaction-export-') as temporary:
    tmp = Path(temporary)
    unit = tmp / 'export.c'
    unit.write_text(prefix + media[start:end] +
                    '\nint main(void){x4_live_interaction_metrics_write(stdout);return 0;}\n', encoding='utf-8')
    exe = tmp / 'export'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)], check=True)
    produced = subprocess.run([str(exe)], check=True, text=True, capture_output=True)
    interaction = json.loads(produced.stdout)
    assert interaction == {'input': {'test_child': 'input'},
                           'main_and_rumble': {'test_child': 'main_rumble'},
                           'audio': {'test_child': 'audio'}}
    original = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    enriched = dict(original, interaction=interaction)
    reader_path = root / 'scripts/analyze_stream_pipeline.py'
    spec = importlib.util.spec_from_file_location('interaction_reader', reader_path)
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    assert reader.analyze(original) == reader.analyze(enriched)
    fixture = tmp / 'enriched.json'
    fixture.write_text(json.dumps(enriched), encoding='utf-8')
    subprocess.run([sys.executable, '-B', str(root / 'tests/stream_report_reader_test.py'), str(fixture)], check=True)
print('PASS actual interaction export glue + existing report reader; unchanged video fixture, no video runtime executed')
