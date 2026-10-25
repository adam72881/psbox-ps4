/* SPDX-License-Identifier: GPL-3.0-or-later
 * Obiettivo ~50 ms, prefill 40 ms, trim a passi di 10 ms con fade di 5 ms.
 * Il taglio secco da 60 ms a 40 ms faceva un click a ogni raffica.
 */
#ifndef PSBOX_AUDIO_PACE_H
#define PSBOX_AUDIO_PACE_H

struct AudioPace {
    int priming;
};

struct AudioDecision {
    int play;
    int dropFrames;
    int fadeFrames;
};

static void audioPaceInit(AudioPace* p) {
    if (p) p->priming = 1;
}

static AudioDecision audioDecide(AudioPace* p, int queuedFrames, int rate) {
    AudioDecision d;
    int targetPrefill, high, step, fade;
    d.play = 0;
    d.dropFrames = 0;
    d.fadeFrames = 0;
    if (!p || rate <= 0 || queuedFrames < 0) return d;
    targetPrefill = rate * 40 / 1000;
    high = rate * 100 / 1000;
    step = rate * 10 / 1000;
    fade = rate * 5 / 1000;
    if (step < 1) step = 1;
    if (p->priming) {
        if (queuedFrames < targetPrefill) return d;
        p->priming = 0;
    }
    if (queuedFrames == 0) {
        p->priming = 1;
        return d;
    }
    if (queuedFrames > high) {
        d.dropFrames = step;
        if (d.dropFrames >= queuedFrames) d.dropFrames = queuedFrames / 2;
        d.fadeFrames = fade;
        if (d.fadeFrames > d.dropFrames) d.fadeFrames = d.dropFrames;
    }
    d.play = 1;
    return d;
}

/* Mescola gli ultimi fadeFrames del tratto scartato con l'inizio di ciò che resta. */
static void audioApplyFade(float* interleaved, int frames, int dropFrames, int fadeFrames) {
    int fade, keep, i;
    if (!interleaved || dropFrames <= 0 || frames <= dropFrames) return;
    fade = fadeFrames;
    if (fade > dropFrames) fade = dropFrames;
    keep = frames - dropFrames;
    if (fade > keep) fade = keep;
    if (fade < 1) return;
    for (i = 0; i < fade; ++i) {
        float t = (float)(i + 1) / (float)(fade + 1);
        int a = (dropFrames - fade + i) * 2;
        int b = dropFrames * 2 + i * 2;
        interleaved[b] = interleaved[a] * (1.f - t) + interleaved[b] * t;
        interleaved[b + 1] = interleaved[a + 1] * (1.f - t) + interleaved[b + 1] * t;
    }
}

#endif
