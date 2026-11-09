/* SPDX-License-Identifier: GPL-3.0-only */
/* RHYTM-SEC-1's engine: PERF-1's drums and bass alone (deamonSLINKY/PERF-1 848b3c6, firmware/src/dsp/amb.c; a fork of
 * RedtailWorks' HYP-1, itself of Charles Vestal's AMB-1). Taken from amb.c, cut down to two voices: its six drum
 * voices and their kits, its sequencer (six patterns, density, chance, ghosts, the steps' level / push / ratchet,
 * swing, humanize, fills, drum drift, the breakdown / build / drop), its bass (the drone and the bass line's modes, on
 * the genre's chord moves) and the kick's sidechain on the bass. The tables are amb.c's, as they are there; the
 * styles (styles.h) are PERF-1's genres, dumped by gen/dump.c from its own pieces. Where PERF-1 plays a piece's six
 * written patterns, this draws its patterns from each genre's map of where its 60 patterns hit (STYLE_PROB), and lets
 * them evolve a step at a time: density and complexity shape the draw, the player locks or redraws it.
 * Not here: the pad, the acid, the pluck, the DX voices, the effects, the reverb and echo, the master bus, the
 * chains / slices / time / conditions of the phrase page, the breakcore crush, MIDI out. */
#include <stdint.h>
#include "fastmath.h"

#define EN_SR 44100.0f
#define EN_NDRUM 6
#define EN_NLANE 6
#define EN_NGENRE 19
#define EN_NKIT 14
#define EN_NSOUND 24
#define EN_NBLFIG 9
#include "styles.h"

/* styles.h's columns (gen/dump.c's KEEP) */
enum { Q_TEMPO, Q_ROOT, Q_MODE, Q_SWING, Q_PULSE, Q_BASS, Q_DRIVE, Q_KIT_KICK, Q_KIT_SNARE, Q_KIT_HAT, Q_KIT_PERC,
       Q_LANE, Q_DENS = Q_LANE + 18, Q_PROB = Q_DENS + 6, Q_GHOST = Q_PROB + 6, Q_ACCENT, Q_HUMAN, Q_BL_MODE, Q_BL_LEN,
       Q_BL_MOVE, Q_BL_GLIDE, Q_BASSOCT, Q_OCT, Q_DR_INST, Q_DR_CUT, Q_DR_RES, Q_DE_TRIG, Q_DE_ATK, Q_DE_DEC, Q_DE_AMT,
       Q_TUNE, Q_N };
_Static_assert(Q_N == PC_N, "styles.h's columns");

static const char *const GENRE_NAME[EN_NGENRE] = {"HYPNO", "LOFI", "HOUSE", "DUBSTEP", "DUB", "DNB", "BREAKS", "GARAGE", "ELECTRO",
    "TRAP", "DISCO", "FUNK", "REGGAE", "SYNTHWV", "AMAPI", "TRIPHOP", "INDUS", "PSY", "FOREST"};
static const char *const DRONE_NAME[EN_NSOUND] = {"Saws", "Warm", "Grit", "Dirty", "Reese", "Pulse", "FM", "Organ",
    "Rumble", "Sub", "Square", "Hoover", "Fifths", "Metal", "Sync", "Bright", "Wub", "Bite", "Tine", "Clang", "Gnarl", "Wide",
    "Power", "Air"};
static const char *const BL_NAME[16] = {"Drone", "Root", "Kick", "Eighths", "Offbeat", "Walk", "Riff", "Octaves", "Two-step",
    "One-drop", "Log drum", "Skippy", "Funk", "Big beat", "Dub", "Gallop"};
static const uint16_t MODE_POOL[8] = {0xAB5, 0x6AD, 0x5AB, 0xAD5, 0x6B5, 0x5AD, 0x295, 0x4A9};   /* amb.c's EN_MODE_POOL */

static int16_t par[Q_N];                         /* the piece playing (the player's knobs over it) */
static uint8_t mods[EN_NLANE][16];               /* its steps' tricks: ratchet (bits 0-2), push (3-5), level (6-7) */
static int genre;
static volatile uint32_t en_lane_mute;           /* bit l: drum lane l muted; bit 6: the bass */
static volatile uint8_t en_playing;

static uint32_t rs = 0x2545F491u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frand(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static int irand(int n) { return n > 0 ? (int)((rnd() >> 8) % (uint32_t)n) : 0; }

#define SN 1024
#include "sint_tab.h"
static inline float sine(float ph)
{
    float x = (ph - (float)(int)ph) * (float)SN;
    int i = (int)x;
    float f = x - (float)i;
    return SINT[i] + (SINT[i + 1] - SINT[i]) * f;
}
static inline float sine_any(float ph)
{
    float fl = (float)(int)ph;
    if (ph < fl)
        fl -= 1.0f;
    return sine(ph - fl);
}
static float tunefac = 1.0f;
static float mtof(float n) { return 440.0f * tunefac * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }
static inline float blep_saw(float t, float dt)
{
    float y = 2.0f * t - 1.0f;
    if (t < dt) {
        float x = t / dt;
        y -= x + x - x * x - 1.0f;
    } else if (t > 1.0f - dt) {
        float x = (t - 1.0f) / dt;
        y -= x * x + x + x + 1.0f;
    }
    return y;
}
static void svf_coefs(float *c, float f, float k)
{
    float g = fm_tanf(FM_PI * fm_clampf(f, 40.0f, 16000.0f) * (1.0f / EN_SR));
    c[0] = 1.0f / (1.0f + g * (g + k));
    c[1] = g * c[0];
    c[2] = g * c[1];
    c[3] = k;
}
static inline float svf_lp(float x, const float *c, float *s)
{
    float v3 = x - s[1], v1 = c[0] * s[0] + c[1] * v3, v2 = s[1] + c[1] * s[0] + c[2] * v3;
    s[0] = 2.0f * v1 - s[0];
    s[1] = 2.0f * v2 - s[1];
    return v2;
}

/* ---- the kits (amb.c's KITS) */
typedef struct {
    float k_start, k_drop, k_dec0, k_dec1, k_click, k_drive, k_rumble;
    float sn_f1, sn_f2, sn_noise, sn_body, sn_dec;
    float cl_gap, cl_dec;
    float h_metal, h_noise, h_dec, oh_dec;
    float p_freq, p_ratio, p_idx;
    float duck, chop, crush;
} kit_t;
static const kit_t KITS[EN_NKIT] = {
    {5.0f, 0.0016f, 0.15f, 0.75f, 0.25f, 1.0f, 0.0f, 185.0f, 330.0f, 1.1f, 0.7f, 1.0f, 353.0f, 1.0f,
     1.0f, 0.6f, 1.0f, 1.0f, 380.0f, 1.48f, 1.4f, 1.0f, 0.0f, 0.0f},
    {3.2f, 0.0011f, 0.2f, 0.8f, 0.12f, 0.6f, 0.0f, 210.0f, 360.0f, 1.3f, 0.5f, 0.9f, 440.0f, 1.4f,
     1.25f, 0.9f, 1.1f, 1.4f, 300.0f, 1.0f, 0.6f, 1.25f, 0.0f, 0.0f},
    {7.0f, 0.0024f, 0.12f, 0.5f, 0.4f, 1.6f, 0.6f, 170.0f, 290.0f, 1.4f, 0.4f, 0.7f, 260.0f, 0.7f,
     1.55f, 0.35f, 0.7f, 1.3f, 560.0f, 3.5f, 2.2f, 1.1f, 0.0f, 0.0f},
    {4.0f, 0.0014f, 0.13f, 0.6f, 0.35f, 1.25f, 0.0f, 180.0f, 330.0f, 1.5f, 0.6f, 1.2f, 440.0f, 1.1f,
     1.0f, 0.6f, 0.9f, 1.6f, 560.0f, 1.48f, 2.0f, 1.0f, 0.0f, 0.0f},
    {6.0f, 0.0022f, 0.1f, 0.45f, 0.45f, 2.5f, 0.2f, 240.0f, 420.0f, 1.6f, 0.8f, 0.6f, 300.0f, 0.8f,
     1.3f, 1.0f, 0.6f, 0.8f, 200.0f, 1.0f, 0.9f, 0.8f, 1.0f, 0.6f},
    {4.0f, 0.0018f, 0.08f, 0.35f, 0.5f, 1.0f, 0.0f, 400.0f, 820.0f, 0.5f, 1.0f, 0.5f, 300.0f, 0.6f,
     1.4f, 0.3f, 0.5f, 0.7f, 720.0f, 2.0f, 0.8f, 0.9f, 0.0f, 0.0f},
    {8.0f, 0.0028f, 0.14f, 0.6f, 0.6f, 3.2f, 0.9f, 200.0f, 340.0f, 1.8f, 0.5f, 0.8f, 220.0f, 0.8f,
     1.7f, 0.8f, 0.8f, 1.2f, 140.0f, 2.76f, 2.8f, 1.3f, 0.0f, 0.25f},
    {2.2f, 0.0007f, 0.3f, 1.6f, 0.08f, 0.5f, 0.0f, 238.0f, 476.0f, 1.0f, 0.9f, 1.1f, 380.0f, 1.6f,
     1.15f, 0.5f, 0.8f, 1.8f, 540.0f, 1.48f, 1.2f, 1.0f, 0.0f, 0.0f},
    {2.8f, 0.0010f, 0.2f, 0.95f, 0.08f, 0.45f, 0.0f, 172.0f, 290.0f, 0.9f, 0.85f, 0.95f, 380.0f, 0.8f,
     0.85f, 0.55f, 0.85f, 1.1f, 520.0f, 1.5f, 0.9f, 0.35f, 0.0f, 0.0f},
    {3.0f, 0.0013f, 0.28f, 1.35f, 0.25f, 1.1f, 0.35f, 190.0f, 330.0f, 1.5f, 0.75f, 1.05f, 330.0f, 1.1f,
     1.2f, 0.5f, 0.7f, 1.0f, 300.0f, 2.2f, 1.6f, 1.0f, 0.0f, 0.0f},
    {5.0f, 0.0015f, 0.16f, 0.7f, 0.3f, 1.1f, 0.0f, 188.0f, 332.0f, 1.25f, 0.75f, 0.85f, 410.0f, 0.95f,
     1.3f, 0.7f, 0.7f, 1.2f, 150.0f, 1.0f, 0.5f, 1.1f, 0.0f, 0.0f},
    {6.0f, 0.0020f, 0.09f, 0.4f, 0.5f, 1.3f, 0.0f, 220.0f, 400.0f, 1.5f, 0.5f, 0.6f, 300.0f, 0.4f,
     1.6f, 0.5f, 0.5f, 1.0f, 160.0f, 1.0f, 1.2f, 1.0f, 0.0f, 0.0f},
    {5.5f, 0.0018f, 0.1f, 0.4f, 0.35f, 1.0f, 0.0f, 240.0f, 460.0f, 1.3f, 0.7f, 0.55f, 380.0f, 0.5f,
     1.5f, 0.5f, 0.45f, 0.8f, 320.0f, 1.5f, 0.8f, 1.0f, 0.0f, 0.0f},
    {2.0f, 0.0006f, 0.35f, 2.0f, 0.05f, 0.6f, 0.0f, 200.0f, 390.0f, 1.2f, 0.6f, 0.9f, 460.0f, 1.0f,
     1.7f, 0.4f, 0.4f, 0.9f, 460.0f, 1.48f, 1.0f, 1.1f, 0.0f, 0.0f},
};
static const kit_t *KK = &KITS[0], *KS = &KITS[0], *KH = &KITS[0], *KP = &KITS[0];

/* ---- the bass's sounds (amb.c's DRONE_SOUND) */
enum { DO_SAWS, DO_REESE, DO_PULSE, DO_FM, DO_ORGAN, DO_SINE, DO_SQUARE, DO_HOOVER, DO_FIFTHS, DO_METAL, DO_SYNC, DO_FMR };
typedef struct { uint8_t osc; float saw, sub, drive, cut, g; } drone_snd_t;
static const float DRONE_FR[EN_NSOUND] = {[17] = 2.0f, [18] = 3.5f, [19] = 5.19f};
static const drone_snd_t DRONE_SOUND[EN_NSOUND] = {
    {DO_SAWS, 0.4f, 0.55f, 1.0f, 0.0f, 1.0f},   {DO_SAWS, 0.3f, 0.75f, 0.8f, -0.4f, 1.0f},
    {DO_SAWS, 0.45f, 0.5f, 2.2f, -0.3f, 1.0f},  {DO_SAWS, 0.3f, 0.8f, 3.0f, -0.6f, 0.9f},
    {DO_REESE, 0.5f, 0.4f, 1.4f, 0.0f, 1.0f},   {DO_PULSE, 0.45f, 0.45f, 1.1f, 0.0f, 1.0f},
    {DO_FM, 0.55f, 0.35f, 1.2f, 0.4f, 1.0f},    {DO_ORGAN, 0.45f, 0.5f, 1.0f, 0.6f, 0.9f},
    {DO_SAWS, 0.12f, 1.0f, 2.6f, -1.0f, 0.95f}, {DO_SINE, 0.6f, 0.5f, 1.3f, 0.0f, 1.05f},
    {DO_SQUARE, 0.45f, 0.45f, 1.0f, 0.2f, 0.95f}, {DO_HOOVER, 0.4f, 0.5f, 1.6f, 0.5f, 0.85f},
    {DO_FIFTHS, 0.4f, 0.5f, 1.2f, 0.2f, 0.9f},  {DO_METAL, 0.5f, 0.45f, 1.3f, 0.5f, 0.95f},
    {DO_SYNC, 0.45f, 0.5f, 1.2f, 0.3f, 0.85f},  {DO_SAWS, 0.45f, 0.4f, 1.2f, 1.2f, 0.85f},
    {DO_SAWS, 0.35f, 0.9f, 3.5f, -0.3f, 0.9f},  {DO_FMR, 0.5f, 0.45f, 1.3f, 0.4f, 0.95f},
    {DO_FMR, 0.45f, 0.4f, 1.1f, 0.6f, 0.9f},    {DO_FMR, 0.5f, 0.4f, 1.4f, 0.5f, 0.9f},
    {DO_REESE, 0.55f, 0.35f, 2.6f, 0.2f, 0.9f}, {DO_HOOVER, 0.3f, 0.6f, 1.0f, 0.3f, 0.9f},
    {DO_FIFTHS, 0.45f, 0.6f, 1.8f, -0.2f, 0.9f}, {DO_SINE, 0.7f, 0.2f, 1.0f, 1.0f, 1.0f},
};

/* ---- the drum voices (amb.c's drum_t and render_*) */
enum { L_KICK, L_SNARE, L_CLAP, L_HAT, L_OHAT, L_PERC };
typedef struct {
    float ph, inc, inc_end, amp, adec, idx, x1, x2, att, mph[4], pend, t, hp, ph2, inc2;
    int32_t wait, wait2;
    float pend2;
    int32_t rgap;
    float rmul;
    float xl, fo;
    uint8_t rn;
    uint8_t on;
} drum_t;
static drum_t drum[EN_NDRUM];
#define DR_FADE (1.0f - 1.0f / (0.0008f * EN_SR))
static float pulse_vol, swing, drive_k = 1.0f, lane_g[EN_NDRUM], lane_tune[EN_NDRUM], lane_dec[EN_NDRUM];
static float step_len = 5512.5f;
static uint32_t noise = 22222u;
static const float HAT_INC[4] = {304.4f / EN_SR, 369.6f / EN_SR, 522.7f / EN_SR, 800.0f / EN_SR};
static float hat_inc[4] = {304.4f / EN_SR, 369.6f / EN_SR, 522.7f / EN_SR, 800.0f / EN_SR};
static float duck_env, duck_dec = 0.9997f;
volatile float en_drum_glow[EN_NDRUM];

#define BLK 64
static float mixl[BLK], mixr[BLK], bass_o[BLK];
static float lane_on_g[EN_NDRUM + 1] = {1, 1, 1, 1, 1, 1, 1};   /* the mutes, eased (no click) */
/* the USB stems: the host's mono blocks for this render (0: none): 0 the bass, 1 the kick, 2 snare and clap, 3 the hats
 * and perc (STEM_LANES); while any is there each voice also leaves its own samples, before the pan, in lane_o (the mix
 * itself unchanged) */
#define EN_NSTEM 4
static const uint8_t STEM_LANES[EN_NSTEM] = {0, 1u << 0, 1u << 1 | 1u << 2, 1u << 3 | 1u << 4 | 1u << 5};
static int32_t *st_out[EN_NSTEM];
static uint8_t stems_on;
static float lane_o[EN_NDRUM][BLK];

static void render_hat(drum_t *v, uint32_t n, float g, float pl, float pr, float *so)
{
    uint32_t j;
    int k;
    float x1, x2, amp, att, fo, x = 0.0f;
    if (!v->on)
        return;
    x1 = v->x1, x2 = v->x2, amp = v->amp, att = v->att, fo = v->fo;
    for (j = 0; j < n; j++) {
        float in = 0.0f, hp;
        for (k = 0; k < 4; k++) {
            v->mph[k] += hat_inc[k];
            if (v->mph[k] >= 1.0f)
                v->mph[k] -= 1.0f;
            in += v->mph[k] < 0.5f ? 0.25f : -0.25f;
        }
        noise = noise * 1664525u + 1013904223u;
        in = 0.5f * in + KH->h_noise * (float)(int32_t)noise * (1.0f / 2147483648.0f);
        hp = in - x2 - 0.8f * x1;
        x1 += v->hp * hp;
        x2 += v->hp * x1;
        x = hp * amp * att * g + fo;
        fo *= DR_FADE;
        mixl[j] += x * pl;
        mixr[j] += x * pr;
        if (so)
            so[j] = x;
        amp *= v->adec;
        att += (1.0f - att) * 0.3f;
    }
    v->x1 = fm_flush(x1);
    v->x2 = fm_flush(x2);
    v->amp = amp;
    v->att = att;
    v->fo = fm_flush(fo);
    v->xl = x;
    v->on = amp > 0.0005f;
}

static void render_clap(drum_t *v, uint32_t n, float g, float *so)
{
    uint32_t j;
    float s1 = v->x1, s2 = v->x2, amp = v->amp, t = v->t, ph = v->ph, fo = v->fo, x = 0.0f;
    const float gap = KS->cl_gap;
    if (!v->on)
        return;
    for (j = 0; j < n; j++) {
        float in, bp;
        if (t == gap || t == 2.0f * gap)
            amp = v->pend;
        noise = noise * 1664525u + 1013904223u;
        in = (float)(int32_t)noise * (1.0f / 2147483648.0f);
        bp = in - s2 - 0.7f * s1;
        s1 += v->hp * bp;
        s2 += v->hp * s1;
        x = (s1 * 1.6f + 0.35f * sine(ph) * fm_maxf(1.0f - t * (1.0f / 2500.0f), 0.0f)) * amp * g + fo;
        fo *= DR_FADE;
        ph += v->inc;
        if (ph >= 1.0f)
            ph -= 1.0f;
        mixl[j] += x * 0.95f;
        mixr[j] += x;
        if (so)
            so[j] = x;
        amp *= t < 2.0f * gap ? v->inc_end : v->adec;
        t += 1.0f;
    }
    v->x1 = fm_flush(s1);
    v->x2 = fm_flush(s2);
    v->amp = amp;
    v->t = t;
    v->ph = ph;
    v->fo = fm_flush(fo);
    v->xl = x;
    v->on = t < 2.0f * gap || amp > 0.0005f;
}

static void render_snare(drum_t *v, uint32_t n, float g, float *so)
{
    uint32_t j;
    float s1 = v->x1, s2 = v->x2, amp = v->amp, ph = v->ph, ph2 = v->ph2, att = v->att, body = v->idx, fo = v->fo, x = 0.0f;
    if (!v->on)
        return;
    if (v->t == 0.0f)
        body = 1.0f;
    for (j = 0; j < n; j++) {
        float in, bp;
        noise = noise * 1664525u + 1013904223u;
        in = (float)(int32_t)noise * (1.0f / 2147483648.0f);
        bp = in - s2 - 0.5f * s1;
        s1 += v->hp * bp;
        s2 += v->hp * s1;
        x = ((sine(ph) + 0.6f * sine(ph2)) * body * KS->sn_body + s1 * KS->sn_noise) * amp * att * g + fo;
        fo *= DR_FADE;
        ph += v->inc;
        ph2 += v->inc2;
        if (ph >= 1.0f)
            ph -= 1.0f;
        if (ph2 >= 1.0f)
            ph2 -= 1.0f;
        mixl[j] += x;
        mixr[j] += x * 0.95f;
        if (so)
            so[j] = x;
        amp *= v->adec;
        body *= 0.9992f;
        att += (1.0f - att) * 0.4f;
    }
    v->x1 = fm_flush(s1);
    v->x2 = fm_flush(s2);
    v->amp = amp;
    v->ph = ph;
    v->ph2 = ph2;
    v->att = att;
    v->idx = body;
    v->t = 1.0f;
    v->fo = fm_flush(fo);
    v->xl = x;
    v->on = amp > 0.0005f;
}

static void render_perc(drum_t *v, uint32_t n, float g, float *so)
{
    uint32_t j;
    float ph = v->ph, ph2 = v->ph2, inc = v->inc, amp = v->amp, idx = v->idx, att = v->att, fo = v->fo, x = 0.0f;
    if (!v->on)
        return;
    for (j = 0; j < n; j++) {
        float m = sine(ph2) * idx;
        x = sine(ph + m + 4.0f) * amp * att * g + fo;
        fo *= DR_FADE;
        ph += inc;
        ph2 += inc * v->inc2;
        if (ph >= 1.0f)
            ph -= 1.0f;
        if (ph2 >= 1.0f)
            ph2 -= 1.0f;
        inc += (v->inc_end - inc) * 0.004f;
        mixl[j] += x * 0.8f;
        mixr[j] += x;
        if (so)
            so[j] = x;
        amp *= v->adec;
        idx *= 0.9995f;
        att += (1.0f - att) * 0.3f;
    }
    v->ph = ph;
    v->ph2 = ph2;
    v->inc = inc;
    v->amp = amp;
    v->idx = idx;
    v->att = att;
    v->fo = fm_flush(fo);
    v->xl = x;
    v->on = amp > 0.0005f;
}

static void render_drums(uint32_t n)
{
    uint32_t j;
    const float g = pulse_vol * 1.1f;
    drum_t *v = &drum[L_KICK];
    float *so = stems_on ? lane_o[L_KICK] : 0;
    if (stems_on) {                              /* (a voice that is off leaves its lane silent) */
        int l;
        for (l = 0; l < EN_NDRUM; l++)
            for (j = 0; j < n; j++)
                lane_o[l][j] = 0.0f;
    }
    if (v->on) {
        float ph = v->ph, inc = v->inc, amp = v->amp, att = v->att, ck = v->idx, gk = g * lane_g[L_KICK] * 1.15f * lane_on_g[L_KICK], fo = v->fo, x = 0.0f;
        float r1 = v->x1, r2 = v->x2, rl1 = v->mph[0], rl2 = v->mph[1], rp = v->ph2;
        const float dk = drive_k * KK->k_drive, drop = KK->k_drop, cka = KK->k_click, rum = KK->k_rumble;
        const float norm = 0.85f / fm_minf(1.0f + 0.25f * (dk - 1.0f), 1.6f);
        for (j = 0; j < n; j++) {
            float nz;
            noise = noise * 1664525u + 1013904223u;
            nz = (float)(int32_t)noise * (1.0f / 2147483648.0f);
            x = sine(ph) * amp * att + ck * cka * nz;
            if (rum > 0.0f) {
                rl1 += 0.012f * (nz - rl1);
                rl2 += 0.012f * (rl1 - rl2);
                x += (r1 - r2) * (3.0f * rl2 + 0.5f * sine(rp));
                rp += v->inc_end;
                if (rp >= 1.0f)
                    rp -= 1.0f;
                r1 *= 0.99985f;
                r2 *= 0.9985f;
            }
            x = fm_tanhf(x * 1.3f * dk) * norm * gk + fo;
            fo *= DR_FADE;
            mixl[j] += x;
            mixr[j] += x;
            if (so)
                so[j] = x;
            ph += inc;
            if (ph >= 1.0f)
                ph -= 1.0f;
            inc += (v->inc_end - inc) * drop;
            amp *= v->adec;
            att += (1.0f - att) * 0.25f;
            ck *= 0.985f;
        }
        v->ph = ph;
        v->inc = inc;
        v->amp = amp;
        v->att = att;
        v->idx = ck;
        v->x1 = r1;
        v->x2 = r2;
        v->mph[0] = fm_flush(rl1);
        v->mph[1] = fm_flush(rl2);
        v->ph2 = rp;
        v->fo = fm_flush(fo);
        v->xl = x;
        v->on = amp > 0.0005f || r1 - r2 > 0.002f;
    }
    render_snare(&drum[L_SNARE], n, g * 0.38f * lane_g[L_SNARE] * lane_on_g[L_SNARE], so ? lane_o[L_SNARE] : 0);
    render_clap(&drum[L_CLAP], n, g * 0.42f * lane_g[L_CLAP] * lane_on_g[L_CLAP], so ? lane_o[L_CLAP] : 0);
    render_hat(&drum[L_HAT], n, g * 0.22f * lane_g[L_HAT] * lane_on_g[L_HAT], 0.75f, 1.0f, so ? lane_o[L_HAT] : 0);
    render_hat(&drum[L_OHAT], n, g * 0.18f * lane_g[L_OHAT] * lane_on_g[L_OHAT], 1.0f, 0.75f, so ? lane_o[L_OHAT] : 0);
    render_perc(&drum[L_PERC], n, g * 0.3f * lane_g[L_PERC] * lane_on_g[L_PERC], so ? lane_o[L_PERC] : 0);
}

/* ---- harmony: the scale, the chord's degree, its moves (amb.c's chord_change without the progression page) */
#define EB2 0x80u
typedef struct { uint8_t from, to, w; } ch_edge_t;
static const struct { uint8_t tonic, cad_every, cad_to, back, hr, dom; ch_edge_t e[8]; } CHT[EN_NGENRE] = {
    {0, 0, 0, 0, 0, 0, {{0}}},
    {2, 8, 4, 0, 2, 1, {{1, 4, 12}, {4, 0, 12}, {0, 1, 4}, {0, 3, 4}, {3, 1, 5}, {5, 1, 6}, {2, 5, 6}, {6, 2, 5}}},
    {2, 4, 6, 1, 2, 0, {{0, 6, 8}, {6, 5, 6}, {5, 6, 6}, {6, 0, 5}, {5, 0, 3}, {0, 5, 5}, {3, 6, 3}, {0, 0, 0}}},
    {4, 8, 6, 0, 8, 0, {{0, 5, 6}, {5, 0, 8}, {0, 6, 4}, {6, 0, 6}, {0, 4, 3}, {0, 1, EB2 | 5}, {1, 0, EB2 | 8}, {0, 0, 0}}},
    {3, 4, 3, 1, 4, 0, {{0, 3, 8}, {3, 0, 8}, {0, 6, 5}, {6, 0, 5}, {0, 5, 3}, {5, 6, 3}, {0, 0, 0}, {0, 0, 0}}},
    {2, 4, 4, 0, 4, 1, {{1, 4, 10}, {4, 0, 8}, {0, 5, 5}, {5, 1, 6}, {0, 3, 4}, {3, 1, 4}, {0, 0, 0}, {0, 0, 0}}},
    {3, 4, 4, 0, 4, 1, {{0, 3, 4}, {3, 4, 4}, {4, 0, 6}, {0, 5, 4}, {5, 3, 4}, {6, 0, 4}, {0, 0, 0}, {0, 0, 0}}},
    {2, 4, 4, 0, 2, 1, {{1, 4, 8}, {4, 0, 8}, {0, 5, 5}, {5, 3, 5}, {3, 1, 5}, {2, 5, 4}, {0, 0, 0}, {0, 0, 0}}},
    {4, 4, 4, 0, 4, 0, {{0, 4, 5}, {4, 0, 7}, {0, 3, 4}, {3, 0, 6}, {0, 6, 3}, {6, 0, 4}, {0, 0, 0}, {0, 0, 0}}},
    {5, 4, 6, 0, 4, 0, {{0, 5, 6}, {5, 6, 5}, {6, 0, 8}, {0, 3, 3}, {3, 0, 7}, {5, 0, 5}, {0, 1, EB2 | 5}, {1, 0, EB2 | 8}}},
    {2, 4, 4, 0, 2, 1, {{1, 4, 10}, {4, 0, 9}, {0, 3, 5}, {3, 6, 6}, {6, 2, 6}, {2, 5, 6}, {5, 1, 6}, {0, 0, 0}}},
    {5, 4, 3, 1, 4, 0, {{0, 3, 8}, {3, 0, 8}, {0, 6, 3}, {6, 0, 5}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
    {3, 4, 4, 1, 2, 1, {{0, 3, 7}, {3, 0, 7}, {0, 4, 5}, {4, 0, 7}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
    {3, 4, 6, 0, 2, 0, {{0, 5, 8}, {5, 2, 8}, {2, 6, 8}, {6, 0, 7}, {6, 5, 3}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
    {2, 8, 4, 0, 4, 1, {{1, 4, 10}, {4, 0, 9}, {0, 1, 5}, {0, 3, 4}, {3, 1, 5}, {5, 1, 5}, {0, 0, 0}, {0, 0, 0}}},
    {5, 8, 6, 0, 4, 0, {{0, 5, 5}, {5, 0, 7}, {0, 3, 4}, {3, 0, 6}, {6, 0, 5}, {0, 1, EB2 | 4}, {1, 0, EB2 | 7}, {0, 0, 0}}},
    {5, 4, 1, 1, 2, 0, {{0, 1, 9}, {1, 0, 9}, {0, 6, 4}, {6, 0, 4}, {0, 4, 3}, {4, 1, 3}, {0, 0, 0}, {0, 0, 0}}},
    {5, 8, 1, 1, 8, 0, {{0, 1, 8}, {1, 0, 10}, {0, 6, 4}, {6, 0, 5}, {0, 5, 3}, {5, 0, 4}, {0, 0, 0}, {0, 0, 0}}},
    {6, 8, 1, 1, 8, 0, {{0, 1, 10}, {1, 0, 10}, {0, 3, 3}, {3, 0, 4}, {0, 6, 2}, {6, 0, 3}, {0, 0, 0}, {0, 0, 0}}},
};
static const ch_edge_t SYN_MAJ[8] = {{0, 5, 8}, {5, 3, 8}, {3, 4, 8}, {4, 0, 8}, {0, 3, 4}, {4, 5, 3}, {0, 0, 0}, {0, 0, 0}};
static uint32_t pool;
static int steps[12], nsteps, deg, deg_prev, cad_n, dom_on, hr_left;
static int chord_pcs[3], chord_nt;
static uint8_t change_pending, drop_tonic;
volatile uint8_t en_chord_root;                  /* the chord's root, a pitch class (the screen) */

static int deg_nat(int k) { return steps[k % 7] - (k % 7 == 6 ? dom_on : 0); }
static int deg_dim(int i) { return nsteps == 7 && (deg_nat(i + 4) - deg_nat(i) + 12) % 12 == 6; }
static void dom_apply(void)
{
    if (nsteps != 7)
        return;
    steps[6] -= dom_on;
    dom_on = (int)(CHT[genre].dom && deg == 4 && steps[6] - steps[4] == 3 && steps[6] < 11);
    steps[6] += dom_on;
}
static void scale_set(void)
{
    int i;
    nsteps = 0;
    for (i = 0; i < 12; i++)
        if ((pool | 1u) >> i & 1u)
            steps[nsteps++] = i;
    dom_on = 0;
}
static void chord_set(void)                      /* the degree's triad in the scale */
{
    int k;
    chord_nt = nsteps >= 5 ? 3 : 1;
    for (k = 0; k < chord_nt; k++)
        chord_pcs[k] = (par[Q_ROOT] + steps[(deg + 2 * k) % nsteps] + 12 * ((deg + 2 * k) / nsteps)) % 12;
    en_chord_root = (uint8_t)chord_pcs[0];
}
static int hr_bars(void) { return CHT[genre].hr ? CHT[genre].hr : 4; }
static void bass_env_go(void);
static void chord_to(int i)
{
    deg_prev = deg;
    cad_n = i == 0 && nsteps == 7 && genre && cad_n >= CHT[genre].cad_every ? 0 : cad_n + 1;
    deg = i;
    dom_apply();
    chord_set();
    if (par[Q_DE_TRIG] == 0)
        bass_env_go();
    hr_left = hr_bars() - 1;
}
static void chord_change(void)
{
    static const float W7[7] = {4.0f, 2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 0.4f};
    float w[12], sum = 0.0f, r;
    int i, cad_to = 0;
    const int cg = genre, rich = nsteps == 7 && cg;
    if (rich) {
        cad_to = CHT[cg].cad_to;
        if (deg_dim(cad_to))
            cad_to = deg_dim(4) ? 3 : 4;
    }
    for (i = 0; i < nsteps; i++) {
        if (i == deg) {
            w[i] = 0.0f;
        } else if (rich) {
            const ch_edge_t *const e = cg == 13 && deg_nat(2) == 4 ? SYN_MAJ : CHT[cg].e;
            int k;
            w[i] = 1.0f + (i == 0 ? (float)CHT[cg].tonic : 0.0f);
            for (k = 0; k < 8; k++)
                if (e[k].w && e[k].from == deg && e[k].to == i && (!(e[k].w & EB2) || deg_nat(1) == 1))
                    w[i] += (float)(e[k].w & ~EB2);
            if (!CHT[cg].back && i == deg_prev && deg_prev != deg && cad_n)
                w[i] = 0.0f;
            if (deg_dim(i))
                w[i] *= 0.15f;
            if (cad_n + 1 >= CHT[cg].cad_every && i == cad_to && deg != 0)
                w[i] += 10.0f;
            if (cad_n >= CHT[cg].cad_every && i == 0)
                w[i] += 30.0f;
        } else {
            w[i] = nsteps == 7 ? W7[i] : i == 0 ? 3.0f : 1.5f;
        }
        sum += w[i];
    }
    r = frand() * sum;
    for (i = 0; i < nsteps; i++) {
        r -= w[i];
        if (r <= 0.0f && w[i] > 0.0f)
            break;
    }
    if (i >= nsteps)
        i = deg == 0 ? (nsteps > 1 ? 1 : 0) : 0;
    if (drop_tonic) {
        drop_tonic = 0;
        i = 0;
    }
    chord_to(i);
}

/* ---- the bass (amb.c's drone and bass line) */
static struct {
    float ph[3], sph, mph, note, target, amp;
    float s[4], c1[4], c2[4];
    float env;
    uint8_t env_up;
} bass;
static float bl_env, bl_gate, bass_vol, reso_k = 1.0f, brk, bass_duck;
static uint8_t bl_up, bl_kick, bl_beat, bl_long, bl_struck;   /* (bl_long: a breakdown's held note; bl_struck: a note this step) */
static int32_t bl_wait;
static int bl_pn;
static uint32_t dr_ctl;
static const float GLIDE_K = 1.0f / (0.6f * EN_SR);
static const uint16_t BL_ROOT[EN_NGENRE] = {0x1111, 0x0421, 0x1111, 0x0401, 0x0501, 0x0441, 0x1121, 0x0449, 0x5555, 0x2401, 0x5555, 0x4529, 0x0149, 0x5555, 0x2449, 0x0401, 0x0405, 0xEEEE, 0xEEEE};
typedef struct { uint16_t mask; int8_t iv[4]; } bl_fig_t;
static const bl_fig_t BL_RIFF[EN_NGENRE] = {
    {0x5555, {0, 0, 12, 0}},  {0x0421, {0, 7, 5, 0}},   {0x4444, {0, 0, 12, 0}},  {0x0401, {0, 0, -2, 0}},  {0x0501, {0, 0, 7, 5}},
    {0x0441, {0, 0, 7, 5}},   {0x1121, {0, 0, 12, 7}},  {0x0449, {0, 7, 0, 12}},  {0x5555, {0, 0, 12, 0}},  {0x2401, {0, 0, 12, 7}},
    {0x5555, {0, 12, 0, 12}}, {0x0509, {0, 12, 7, 0}},  {0x0149, {0, 0, 7, 5}},   {0x5555, {0, 0, 12, 0}},  {0x2449, {0, 0, 7, 0}},   {0x0401, {0, 0, -2, 0}},
    {0x1249, {0, 1, 0, -2}}, {0xEEEE, {0, 0, 0, 12}}, {0xEEEE, {0, 0, 1, 0}}};
static const bl_fig_t BL_FIG[EN_NBLFIG] = {
    {0x5555, {0, 12, 0, 12}}, {0x0441, {0, 0, 12, 0}}, {0x0149, {0, 0, 7, 5}}, {0x2449, {0, 0, 7, 0}}, {0x0449, {0, 7, 0, 12}},
    {0x0509, {0, 12, 7, 0}},  {0x1121, {0, 0, 12, 7}}, {0x0501, {0, 0, 7, 5}}, {0x7777, {0, 0, 0, 12}}};
static float de_atk_s(int v) { return 0.01f * fm_exp2f((float)v * 0.0864f); }
static float de_dec_s(int v) { return 0.05f * fm_exp2f((float)v * 0.0832f); }
static void bass_env_go(void) { bass.env_up = 1; }
static int bl_fit(int n)
{
    while (n < 24)
        n += 12;
    while (n > 83)
        n -= 12;
    return n;
}
static int bass_root(void) { return bl_fit(36 + chord_pcs[0] % 12 + 12 * (par[Q_BASSOCT] + par[Q_OCT])); }
static int bl_below(int root)
{
    int d;
    for (d = 1; d <= 2; d++)
        if (pool >> (((root - d - par[Q_ROOT]) % 12 + 12) % 12) & 1u)
            return root - d;
    return root - 1;
}
static void bl_note(int n)
{
    n = bl_fit(n);
    bass.target = (float)n;
    if (!par[Q_BL_GLIDE] || bass.note < 1.0f)
        bass.note = (float)n;
    bass.env_up = 1;
    bl_up = 1;
    bl_gate = bl_long ? 7.5f * step_len : (0.25f + 0.0475f * (float)par[Q_BL_LEN]) * step_len;
}
static void bl_go(int n, int32_t w)
{
    bl_struck = 1;
    if (bl_wait > 0) {
        bl_wait = 0;
        bl_note(bl_pn);
    }
    if (w > 0) {
        bl_pn = n;
        bl_wait = w;
    } else {
        bl_note(n);
    }
}
static int bl_fifth(void)
{
    int k;
    for (k = 1; k < chord_nt; k++) {
        const int d = (chord_pcs[k] - chord_pcs[0] + 12) % 12;
        if (d == 7)
            return 7;
        if (d == 6 || d == 8)
            return d;
    }
    return pool >> (((chord_pcs[0] + 7 - par[Q_ROOT]) % 12 + 12) % 12) & 1u ? 7 : 6;
}

/* ---- the sequencer (amb.c's tick, its drums and bass line) */
enum { SEC_GROOVE, SEC_BREAK, SEC_BUILD };
enum { CMD_BREAK, CMD_BUILD, CMD_DROP };
enum { SF_ROLL = 1, SF_HROLL = 4, SF_GAP = 8, SF_PBUILD = 16, SF_PFILL = 32, SF_KDROP = 64 };
static const struct { uint8_t kick, hat, build, kret, snare, bass, fl; } SECT[EN_NGENRE] = {
    {1, 0, 4, 1, 0, 0, SF_KDROP}, {0, 0, 2, 0, 0, 0, SF_PBUILD}, {1, 0, 8, 1, 0, 0, SF_GAP}, {0, 0, 8, 0, 0, 0, SF_ROLL | SF_GAP},
    {1, 1, 4, 0, 1, 1, SF_PBUILD}, {0, 1, 8, 0, 0, 0, SF_ROLL | SF_GAP}, {1, 1, 4, 0, 1, 0, 0}, {0, 0, 4, 0, 0, 0, SF_PFILL},
    {0, 0, 4, 1, 0, 0, 0}, {0, 0, 4, 0, 0, 0, SF_ROLL | SF_HROLL | SF_GAP}, {0, 0, 4, 1, 0, 0, SF_GAP}, {1, 1, 4, 0, 1, 0, 0},
    {0, 0, 4, 0, 0, 0, SF_PBUILD}, {0, 0, 4, 1, 0, 0, SF_GAP}, {0, 0, 4, 0, 0, 0, SF_PBUILD}, {0, 0, 2, 0, 0, 0, SF_PBUILD},
    {0, 1, 4, 1, 0, 0, SF_ROLL | SF_HROLL | SF_GAP}, {0, 0, 8, 0, 0, 0, SF_ROLL | SF_GAP}, {0, 1, 4, 1, 0, 0, SF_ROLL | SF_HROLL | SF_GAP}};
static const uint8_t FILL_OK[EN_NGENRE] = {0x3F, 0x16, 0x07, 0x49, 0x14, 0x7F, 0x7E, 0x5A, 0x3F, 0x64, 0x87, 0x9E, 0x14, 0x87, 0x12, 0x16, 0x70, 0x47, 0x7E};
static const uint8_t FILLS[5][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 5, 0, 6, 7, 8, 9}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 4, 6, 9},
    {0, 0, 0, 0, 0, 0, 0, 0, 3, 4, 5, 6, 7, 8, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 6, 0, 7, 0, 9},
    {0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 5, 6, 8, 9}};
static const uint16_t GHOST_SN[EN_NGENRE] = {0, 0x8480, 0x0200, 0, 0, 0x8200, 0xC288, 0x0408, 0, 0, 0, 0x8A48, 0, 0, 0, 0x8280, 0x8A20, 0, 0x0420};
static const uint16_t GHOST_KK[EN_NGENRE] = {0, 0x4400, 0, 0, 0, 0, 0x2200, 0, 0, 0, 0, 0x2200, 0, 0, 0, 0x4400, 0x0880, 0, 0};
static const struct { uint8_t n, div; float rf, rm; } RATCHET[8] = {
    {0, 0, 1.0f, 1.0f},   {1, 2, 1.0f, 0.8f},  {2, 3, 1.0f, 0.8f},  {3, 4, 1.0f, 0.8f},
    {5, 6, 1.0f, 0.88f},  {1, 0, 0.4f, 2.5f},  {2, 3, 0.5f, 1.4f},  {3, 4, 0.4f, 1.35f}};

static int sec, sec_bars, sec_req, sec_n = 4;
static uint32_t bars, sixteenth;
static float step_t;                              /* samples into the sixteenth playing */
static uint8_t fill_q, fill_bar, drop_imp, gap_on, grv_bars, crash_next, fill_kind;
static float snr_rise, perc_fall, gap_g = 1.0f;
volatile uint8_t en_sec, en_sec_bars, en_sec_req;
volatile uint16_t en_seen[EN_NDRUM + 1];          /* what played on each of the last 16 steps (bit = step), drums then the bass: the screen's */

/* ---- the generator: each lane's pattern drawn from the style's map. Each step keeps its own draw (thr), so turning
 * density or complexity adds and takes hits smoothly, the same ones each way; evolving redraws a step or two, a new
 * pattern redraws them all. Complexity bends the map: low keeps the genre's surest hits, high spreads them out. */
static uint8_t thr[EN_NLANE][16];
static uint16_t gmask[EN_NLANE];                  /* the pattern playing */
static uint8_t qv[EN_NLANE][16];                  /* each step's chance, as drawn (0..255): what evolving can change */
static volatile uint8_t gen_dirty = 1, gen_new, gen_lane_new;   /* recompute; redraw all at the bar; lanes to redraw */
static volatile uint8_t cplx = 35, evolve = 30, every_i = 1, fills_i = 2, locked, chords_on = 1;
static const uint8_t EVERY[5] = {1, 2, 4, 8, 16};
static const uint8_t FILLS_N[5] = {0, 4, 8, 16, 32};
volatile uint32_t en_evolved;
static void thr_draw(int l)
{
    int s;
    for (s = 0; s < 16; s++)
        thr[l][s] = (uint8_t)(rnd() >> 24);
}
static uint16_t gen_lane(int l)
{
    const uint8_t *P = STYLE_PROB[genre][l];
    const float c = (float)cplx * 0.01f, gam = 2.5f - 2.0f * c, fl = 0.12f * c * c,
                ds = fm_exp2f((float)(par[Q_DENS + l] - 50) * 0.04f);
    float mean = 0.0f;
    uint32_t m = 0;
    int s, best = 0;
    for (s = 0; s < 16; s++)
        mean += (float)P[s];
    mean *= 1.0f / (16.0f * 255.0f);
    for (s = 0; s < 16; s++) {
        const float p = (float)P[s] * (1.0f / 255.0f);
        float q = 0.0f;
        if (p >= 1.0f) {
            q = 1.0f;
        } else if (p > 0.0f) {                   /* the contrast: sure hits surer and rare ones rarer (gam > 1), or all
                                                  * of them nearer an even chance (gam < 1) */
            const float a = fm_powf(p, gam), b = fm_powf(1.0f - p, gam);
            q = a / (a + b);
        }
        q = (q * (1.0f - fl) + fl * fm_minf(mean * 4.0f, 1.0f)) * ds;   /* (a lane the genre never plays stays empty) */
        qv[l][s] = (uint8_t)(fm_minf(q, 1.0f) * 255.0f);
        if (q * 255.0f > (float)thr[l][s])
            m |= 1u << s;
        if (P[s] > P[best])
            best = s;
    }
    if (l == L_KICK && P[0] >= 128 && par[Q_DENS] >= 20)
        m |= 1u;                                 /* the floor's one */
    if (!m && mean * 16.0f >= 1.5f && par[Q_DENS + l] >= 30)
        m |= 1u << best;                         /* a lane the genre leans on is never empty */
    return (uint16_t)m;
}
static void gen_all(void)
{
    int l;
    for (l = 0; l < EN_NLANE; l++)
        gmask[l] = gen_lane(l);
}
static void evolve_bar(void)                     /* at a bar line: a few steps drawn again (Evolve 100: four) */
{
    const float want = (float)evolve * 0.04f;
    int k, n = (int)want + (frand() < want - (float)(int)want);
    if (locked || !evolve || bars % EVERY[every_i % 5u] != 0u)
        return;
    for (k = 0; k < n; k++) {                    /* a step of any lane, the ones nearest an even chance most often: a sure one
                                                  * stays, and a long shot rarely changes anything when drawn again */
        uint32_t w[EN_NLANE * 16], tot = 0, r;
        int i;
        for (i = 0; i < EN_NLANE * 16; i++) {    /* (the weight squared: 96 x (16256 >> 4)^2 stays inside 32 bits) */
            const uint32_t q = qv[i >> 4][i & 15], u = q < 10u || q > 245u ? 0u : q * (255u - q) >> 4;
            w[i] = u * u;
            tot += w[i];
        }
        if (!tot)
            return;
        for (r = rnd() % tot, i = 0; r >= w[i]; i++)
            r -= w[i];
        thr[i >> 4][i & 15] = (uint8_t)(rnd() >> 24);
        en_evolved++;
    }
}

static void section_cmd(int c)
{
    if (c == CMD_BREAK) {
        sec = SEC_BREAK;
    } else if (c == CMD_BUILD) {
        if (sec == SEC_BREAK) {
            sec = SEC_BUILD;
            sec_n = sec_bars = SECT[genre].build;
        }
    } else if (sec != SEC_GROOVE) {
        sec = SEC_GROOVE;
        brk = 0.0f;
        drop_imp = 1;
        grv_bars = 0;
        gap_on = 0;
        if (deg != 0 && nsteps > 1 && frand() < 0.7f) {
            drop_tonic = 1;
            change_pending = 1;
        } else if (frand() < 0.6f) {
            change_pending = 1;
        }
    }
}
static int kick_note(void) { return 28 + (par[Q_ROOT] + 8) % 12; }
static void drum_fire(int d, float vel)
{
    drum_t *v = &drum[d];
    const float dec = d == L_OHAT && crash_next ? 1.0f : lane_dec[d],
                tf = fm_exp2f((lane_tune[d] + (d == L_SNARE ? snr_rise : d == L_PERC ? perc_fall : 0.0f)) * (1.0f / 12.0f));
    if (d == L_OHAT)
        crash_next = 0;
    v->fo = v->on ? v->xl : 0.0f;
    v->on = 1;
    v->amp = vel;
    v->att = 0.0f;
    v->t = 0.0f;
    switch (d) {
    case L_KICK: {
        float f = mtof((float)kick_note() + lane_tune[0]);
        v->inc = f * KK->k_start * (1.0f / EN_SR);
        v->inc_end = f * (1.0f / EN_SR);
        v->adec = fm_expf(-6.9f / ((KK->k_dec0 + KK->k_dec1 * dec) * EN_SR));
        v->ph = 0.0f;
        v->idx = 1.0f;
        v->x1 = KK->k_rumble;
        v->x2 = KK->k_rumble;
        if (lane_g[L_KICK] > 0.0f && !(en_lane_mute & 1u))
            duck_env = 1.0f;
        break;
    }
    case L_SNARE:
        v->inc = KS->sn_f1 * tf * (1.0f / EN_SR);
        v->inc2 = KS->sn_f2 * tf * (1.0f / EN_SR);
        v->ph = v->ph2 = 0.0f;
        v->adec = fm_expf(-6.9f / ((0.08f + 0.3f * dec) * KS->sn_dec * EN_SR));
        v->hp = fm_clampf(0.42f * tf, 0.15f, 0.9f);
        break;
    case L_CLAP:
        v->inc = 185.0f * tf * (1.0f / EN_SR);
        v->ph = 0.0f;
        v->adec = fm_expf(-6.9f / ((0.1f + 0.4f * dec) * KS->cl_dec * EN_SR));
        v->inc_end = fm_expf(-6.9f / (0.012f * EN_SR));
        v->pend = vel;
        v->hp = fm_clampf(0.18f * tf, 0.08f, 0.5f);
        break;
    case L_HAT:
        v->adec = fm_expf(-6.9f / ((0.015f + 0.1f * dec) * KH->h_dec * EN_SR));
        v->hp = fm_clampf(0.72f * tf, 0.4f, 1.1f);
        drum[L_OHAT].adec = fm_expf(-6.9f / (0.02f * EN_SR));
        break;
    case L_OHAT:
        v->adec = fm_expf(-6.9f / ((0.08f + 0.6f * dec) * KH->oh_dec * EN_SR));
        v->hp = fm_clampf(0.72f * tf, 0.4f, 1.1f);
        break;
    default: {
        float f = KP->p_freq * tf;
        v->inc = f * 2.0f * (1.0f / EN_SR);
        v->inc_end = f * (1.0f / EN_SR);
        v->inc2 = KP->p_ratio;
        v->idx = KP->p_idx;
        v->ph = v->ph2 = 0.0f;
        v->adec = fm_expf(-6.9f / ((0.04f + 0.35f * dec) * EN_SR));
        break;
    }
    }
    if (!(en_lane_mute >> d & 1u))
        en_drum_glow[d] = 1.0f;
}
static void drum_hit(int d, float vel, int32_t wait)
{
    if (wait > 0) {
        drum[d].wait = wait;
        drum[d].pend = vel;
    } else {
        drum_fire(d, vel);
    }
}
static void drum_chain(int d, float vel)
{
    drum_t *v = &drum[d];
    if (v->rn) {
        v->rn--;
        v->wait = v->rgap;
        v->pend = vel * v->rmul;
    }
}
static void drum_ratchet(int d, float vel, int32_t wait, int r)
{
    drum_t *v = &drum[d];
    const float v1 = vel * RATCHET[r].rf;
    v->rn = RATCHET[r].n;
    v->rgap = RATCHET[r].div ? (int32_t)(step_len / (float)RATCHET[r].div) : (int32_t)(0.02f * EN_SR);
    v->rmul = RATCHET[r].rm;
    if (wait > 0) {
        v->wait = wait;
        v->pend = v1;
    } else {
        drum_fire(d, v1);
        drum_chain(d, v1);
    }
}
static int32_t loose(int32_t sw, float ms)
{
    if (par[Q_HUMAN]) {
        const float r = frand();
        sw += (int32_t)(r * r * (float)par[Q_HUMAN] * 0.01f * ms * 0.001f * EN_SR);
    }
    return sw;
}
static void bl_step(int st, int32_t w)
{
    const int mode = par[Q_BL_MODE], g = genre;
    int hit = 0, iv = 0, root;
    if (!mode || !en_playing || !nsteps)
        return;
    if (st == 0)
        bl_beat = 0;
    bl_long = 0;
    if (sec == SEC_BREAK && !SECT[g].bass) {     /* the breakdown: the root held, a half bar a note, the filter closing (brk) */
        if (st == 0 || st == 8) {
            bl_long = 1;
            bl_go(bass_root(), w);
        }
        return;
    }
    switch (mode) {
    case 1: hit = BL_ROOT[g] >> st & 1; break;
    case 2: hit = bl_kick && sec != SEC_BUILD; break;
    case 3: hit = !(st & 1); break;
    case 4: hit = (st & 3) == 2; break;
    case 5: hit = !(st & 3); break;
    case 6: hit = BL_RIFF[g].mask >> st & 1; break;
    default: hit = BL_FIG[(mode - 7) % EN_NBLFIG].mask >> st & 1; break;
    }
    if (drop_imp && st == 0)
        hit = 1;
    if (!hit)
        return;
    root = bass_root();
    if (mode == 5) {
        const int beat = st >> 2, three = chord_nt > 1 ? root + (chord_pcs[1] - chord_pcs[0] + 12) % 12 : root + 3,
                  five = root + bl_fifth();
        int n = root;
        if (beat == 1)
            n = frand() < 0.5f ? three : five;
        else if (beat == 2)
            n = five;
        else if (beat == 3 && frand() * 100.0f < (float)par[Q_BL_MOVE] + 20.0f)
            n = frand() < 0.5f ? root - 1 : bl_below(root);
        bl_go(n, w);
        bl_beat++;
        return;
    }
    if (mode == 6)
        iv = BL_RIFF[g].iv[bl_beat & 3];
    else if (mode > 6)
        iv = BL_FIG[(mode - 7) % EN_NBLFIG].iv[bl_beat & 3];
    if (frand() * 100.0f < (float)par[Q_BL_MOVE]) {
        const float r = frand();
        iv = r < 0.5f ? 12 : r < 0.8f ? bl_fifth() : bl_below(root) - root;
    }
    bl_go(root + iv, w);
    bl_beat++;
}
static void tick(void)
{
    static const uint8_t TRIG_BARS[6] = {0, 1, 2, 4, 8, 0};
    int l, st = (int)(sixteenth & 15u);
    const int32_t sw = (st & 1) ? (int32_t)(fm_minf(swing, 0.5f) * step_len) : 0;
    const float chop = KK->chop * 0.7f;
    if (st == 0) {
        const int tb = TRIG_BARS[(unsigned)par[Q_DE_TRIG] < 6u ? par[Q_DE_TRIG] : 0];
        fill_bar = fill_q;
        fill_q = 0;
        {
            const uint32_t ok = FILL_OK[genre];
            uint32_t k = (bars + (uint32_t)genre * 2u) % 8u, n;
            for (n = 0; n < 8u && !(ok >> k & 1u); n++)
                k = (k + 1u) % 8u;
            fill_kind = (uint8_t)k;
        }
        if (sec == SEC_BUILD && --sec_bars <= 0)
            section_cmd(CMD_DROP);
        if (sec_req) {
            section_cmd(sec_req - 1);
            sec_req = 0;
        }
        if (sec == SEC_GROOVE && grv_bars < 255u)
            grv_bars++;
        gap_on = 0;
        if (gen_new) {                           /* a new pattern, asked for */
            gen_new = 0;
            for (l = 0; l < EN_NLANE; l++)
                thr_draw(l);
        }
        for (l = 0; l < EN_NLANE; l++)
            if (gen_lane_new >> l & 1u)
                thr_draw(l);
        gen_lane_new = 0;
        evolve_bar();
        gen_dirty = 1;
        if (FILLS_N[fills_i % 5u] && sec == SEC_GROOVE && bars % FILLS_N[fills_i % 5u] == FILLS_N[fills_i % 5u] - 1u)
            fill_bar = 1;                        /* the fill every 4 .. 32 bars */
        if (tb && bars % (uint32_t)tb == 0u)
            bass_env_go();
        bars++;
    }
    en_sec = (uint8_t)sec;
    en_sec_bars = (uint8_t)(sec == SEC_BUILD ? sec_bars : 0);
    en_sec_req = (uint8_t)sec_req;
    if (gen_dirty) {
        gen_dirty = 0;
        gen_all();
    }
    snr_rise = sec == SEC_BUILD ? 7.0f * (float)((sec_n - sec_bars) * 16 + st) / (float)(sec_n * 16) : 0.0f;
    if (sec == SEC_BUILD && sec_bars == 1 && st >= 12 && (SECT[genre].fl & SF_GAP))
        gap_on = 1;
    perc_fall = fill_bar && fill_kind == 7 && st >= 8 ? -0.9f * (float)(st - 8) : 0.0f;
    for (l = 0; l < EN_NDRUM; l++) {
        const int ls = st, fill = fill_bar;
        int hit = gmask[l] >> ls & 1, dbl = 0, rat = 0;
        float vel = 0.9f;
        int32_t push = 0;
        if (l == L_KICK && sec == SEC_GROOVE && st >= 12 && (SECT[genre].fl & SF_KDROP) && grv_bars % 8u == 0u)
            hit = 0;
        if (sec == SEC_BREAK) {
            const int keep = (l == L_KICK && SECT[genre].kick) || (l == L_HAT && SECT[genre].hat) || (l == L_SNARE && SECT[genre].snare);
            if (!keep)                           /* (what the genre does not keep: the hats and perc on their surest steps, the rest rests) */
                hit = hit && (l == L_HAT || l == L_OHAT || l == L_PERC) && STYLE_PROB[genre][l][ls] >= 160u;
        } else if (sec == SEC_BUILD) {
            if (l == L_PERC && (SECT[genre].fl & SF_PBUILD)) {
                hit = sec_bars > 1 ? (st & 1) == 0 : st < 14;
                vel = 0.3f + 0.6f * ((float)((sec_n - sec_bars) * 16 + st)) / (float)(sec_n * 16);
            } else if (l == L_KICK || l == L_CLAP || l == L_PERC) {
                hit = 0;
                if (l == L_KICK && sec_bars == 1 && SECT[genre].kret && (st == 0 || st == 8))
                    hit = 1;
            } else if (l == L_SNARE && (SECT[genre].fl & SF_PBUILD)) {
                hit = 0;
            } else if (l == L_SNARE) {
                hit = sec_bars > 2 ? (st & 3) == 0 : sec_bars == 2 ? (st & 1) == 0 : st < 14;
                vel = 0.3f + 0.7f * ((float)((sec_n - sec_bars) * 16 + st)) / (float)(sec_n * 16);
                if (sec_bars == 1 && (SECT[genre].fl & SF_ROLL) && st >= 8 && st < 14)
                    dbl = st >= 12 ? 2 : 1;
            } else if (l == L_OHAT && sec_bars == 1) {
                hit = (st & 1) == 0;
                vel = 0.55f;
            }
        } else if (sec_req == CMD_BREAK + 1 && (l == L_PERC || l == L_OHAT || (l == L_HAT && (st & 1)))) {
            hit = 0;
        }
        if (fill && l == L_PERC && st >= 8 && ((fill_kind == 6 && (SECT[genre].fl & SF_PFILL)) || fill_kind == 7)) {
            hit = fill_kind == 7 ? st != 9 && st != 11 : st >= 12 || !(st & 1);
            vel = 0.5f + 0.05f * (float)(st - 8);
            dbl = fill_kind == 6 && st >= 12 ? 2 : 0;
        }
        if (fill && (l == L_SNARE || l == L_HAT)) {
            if (!fill_kind) {
                hit = 1;
                vel = l == L_SNARE ? 0.35f + 0.04f * (float)st : 0.5f;
            } else if (l == L_SNARE && st >= 8 && (fill_kind == 7 || (fill_kind == 6 && (SECT[genre].fl & SF_PFILL)))) {
                hit = 0;
            } else if (l == L_SNARE && st >= 8 && fill_kind == 6) {
                hit = st >= 12 || !(st & 1);
                vel = 0.45f + 0.06f * (float)(st - 8);
                dbl = st >= 12 ? 2 : 0;
            } else if (l == L_SNARE && st >= 8) {
                const uint8_t fv = FILLS[fill_kind - 1][st];
                hit = fv != 0;
                vel = 0.1f * (float)fv + 0.1f;
            } else if (l == L_HAT && st >= 8 && (SECT[genre].fl & SF_HROLL)) {
                hit = 1;
                vel = (st & 1) ? 0.45f : 0.6f;
                dbl = st >= 12 ? 2 : 1;
            }
        }
        if (chop > 0.0f && sec == SEC_GROOVE) {
            if (!hit && l == L_SNARE && frand() < 0.16f * chop) {
                hit = 1;
                vel = 0.25f + 0.25f * frand();
            } else if (!hit && l == L_KICK && (st & 1) && frand() < 0.1f * chop) {
                hit = 1;
                vel = 0.7f;
            } else if (l == L_HAT && frand() < 0.25f * chop) {
                hit = !hit;
                vel = 0.5f;
            }
        }
        if (!hit && par[Q_GHOST] && sec == SEC_GROOVE && !fill && (l == L_SNARE || l == L_KICK) &&
            ((l == L_SNARE ? GHOST_SN[genre] : GHOST_KK[genre]) >> ls & 1u) && frand() * 100.0f < 0.6f * (float)par[Q_GHOST]) {
            hit = 1;
            vel = 0.22f + 0.2f * frand();
        }
        if (hit && par[Q_PROB + l] < 100 && frand() * 100.0f >= (float)par[Q_PROB + l])
            hit = 0;
        if (drop_imp && st == 0 && (l == L_KICK || l == L_CLAP || l == L_OHAT)) {
            hit = 1;
            vel = l == L_KICK ? 1.0f : 0.95f;
            if (l == L_OHAT)
                crash_next = 1;
        }
        if (l == L_KICK)
            bl_kick = (uint8_t)(hit != 0);
        if (hit && sec == SEC_GROOVE && !fill && !drop_imp) {
            static const float LV[4] = {1.0f, 0.4f, 0.7f, 1.12f};
            const uint32_t md = mods[l][ls & 15];
            vel = fm_clampf(vel * (1.0f + (LV[md >> 6 & 3u] - 1.0f) * (float)par[Q_ACCENT] * 0.02f), 0.05f, 1.0f);
            push = (int32_t)((float)(md >> 3 & 7u) * step_len * (1.0f / 12.0f));
            rat = (int)(md & 7u);
        }
        if (hit && par[Q_HUMAN]) {
            const float hm = (float)par[Q_HUMAN] * 0.01f;
            vel *= 1.0f - 0.2f * hm * frand();
            push += (int32_t)(frand() * hm * 0.12f * step_len);
        }
        en_seen[l] = (uint16_t)((en_seen[l] & ~(1u << ls)) | (uint32_t)(hit != 0) << ls);
        if (hit) {
            drum[l].rn = 0;
            if (drum[l].wait > 0) {
                drum[l].wait = 0;
                drum_fire(l, drum[l].pend);
            }
            if (drum[l].wait2 > 0) {
                drum[l].wait2 = 0;
                drum_fire(l, drum[l].pend2);
            }
            if (dbl) {
                drum_fire(l, vel);
                drum[l].wait = (int32_t)(step_len * (dbl == 2 ? 0.3333f : 0.5f));
                drum[l].pend = vel * 0.7f;
                if (dbl == 2) {
                    drum[l].wait2 = (int32_t)(step_len * 0.6667f);
                    drum[l].pend2 = vel * 0.6f;
                }
            } else if (rat) {
                drum_ratchet(l, vel, sw + push, rat);
            } else if (chop > 0.0f && !sw && !push && (l == L_SNARE || l == L_KICK || l == L_HAT) && frand() < 0.2f * chop) {
                drum_fire(l, vel);
                drum[l].wait = (int32_t)(0.5f * step_len);
                drum[l].pend = vel * 0.7f;
            } else {
                drum_hit(l, vel, sw + push);
            }
        }
    }
    bl_struck = 0;
    bl_step(st, loose(sw, 4.0f));
    en_seen[EN_NDRUM] = (uint16_t)((en_seen[EN_NDRUM] & ~(1u << st)) | (uint32_t)(bl_struck || !par[Q_BL_MODE]) << st);
    drop_imp = 0;
    if (st == 0 && change_pending) {
        change_pending = 0;
        if (chords_on)
            chord_change();
    }
    if (st == 0 && !chords_on && deg)
        chord_to(0);                             /* (the chords held: home, on the bar) */
    if (st == 0 && en_playing && chords_on) {                 /* the genre's harmonic rhythm: the chord's last bar asks for the change */
        if (hr_left > 0)
            hr_left--;
        else
            change_pending = 1;
    }
    sixteenth++;
}

static void waits_step(int32_t k)               /* k samples went by: the hits (and the bass note) that waited */
{
    int l;
    for (l = 0; l < EN_NDRUM; l++) {
        drum_t *v = &drum[l];
        if (v->wait > 0 && (v->wait -= k) <= 0) {
            v->wait = 0;
            drum_fire(l, v->pend);
            drum_chain(l, v->pend);
        }
        if (v->wait2 > 0 && (v->wait2 -= k) <= 0) {
            v->wait2 = 0;
            drum_fire(l, v->pend2);
        }
    }
    if (bl_wait > 0 && (bl_wait -= k) <= 0) {
        bl_wait = 0;
        bl_note(bl_pn);
    }
}
static int32_t waits_next(void)                  /* samples to the next thing that waits (0: none) */
{
    int32_t m = 0;
    int l;
    for (l = 0; l < EN_NDRUM; l++) {
        if (drum[l].wait > 0 && (!m || drum[l].wait < m))
            m = drum[l].wait;
        if (drum[l].wait2 > 0 && (!m || drum[l].wait2 < m))
            m = drum[l].wait2;
    }
    if (bl_wait > 0 && (!m || bl_wait < m))
        m = bl_wait;
    return m;
}

static void render_bass(uint32_t n)
{
    uint32_t j;
    const drone_snd_t *DS = &DRONE_SOUND[(unsigned)par[Q_DR_INST] < EN_NSOUND ? par[Q_DR_INST] : 0];
    const int osc = DS->osc;
    const float drv = 1.4f * DS->drive, k1 = reso_k, k2 = 1.4f,
                g = bass_vol * 0.36f * DS->g / fm_minf(1.0f + 0.15f * (DS->drive - 1.0f), 1.3f) * lane_on_g[6],
                dsaw = DS->saw, dsub = DS->sub, tgt = en_playing ? 1.0f : 0.0f,
                rate = tgt > 0.0f ? 1.0f / (2.0f * EN_SR) : 1.0f / (0.5f * EN_SR),
                fmi = 0.5f + 2.2f * bass.env, pw = 0.5f;
    const int bl_on = par[Q_BL_MODE] != 0;
    float p0 = bass.ph[0], p1 = bass.ph[1], p2 = bass.ph[2], sp = bass.sph, mp = bass.mph, amp = bass.amp, f, i0, i1, i2, mi;
    if (amp < 1e-5f && !en_playing) {
        bass.amp = 0.0f;
        for (j = 0; j < n; j++)
            bass_o[j] = 0.0f;
        return;
    }
    if (!bl_on)                                  /* the drone: the chord's root */
        bass.target = (float)bass_root();
    {
        float gk = GLIDE_K;
        if (bl_on)
            gk = par[Q_BL_GLIDE] ? 1.0f / ((0.01f + 0.004f * (float)par[Q_BL_GLIDE]) * EN_SR) : 1.0f;
        bass.note += (bass.target - bass.note) * fm_minf((float)n * gk, 1.0f);
    }
    f = mtof(bass.note);
    i0 = f * (osc == DO_FIFTHS ? 1.49831f : osc == DO_REESE || osc == DO_HOOVER ? 0.9862f : 0.99481f) * (1.0f / EN_SR);
    i1 = f * (1.0f / EN_SR);
    i2 = f * (osc == DO_REESE || osc == DO_HOOVER ? 1.0140f : 1.00521f) * (1.0f / EN_SR);
    mi = osc == DO_METAL ? i1 * 1.4142f : osc == DO_FMR ? i1 * DRONE_FR[(unsigned)par[Q_DR_INST] < EN_NSOUND ? par[Q_DR_INST] : 0] : i1 * (1.5f + 2.5f * bass.env);
    for (j = 0; j < n; j++) {
        float x;
        if (!(dr_ctl++ & 31u)) {
            const float oct = (float)par[Q_DR_CUT] * 0.075f - 1.5f + DS->cut + 4.0f * (float)par[Q_DE_AMT] * 0.01f * bass.env -
                              2.0f * fm_maxf(brk, 0.0f) - 1.6f * fm_minf(brk, 0.0f) - 0.35f * bass_duck * pulse_vol;
            const float fc = fm_clampf(f * 2.0f * fm_exp2f(oct), 45.0f, 9000.0f);
            svf_coefs(bass.c1, fc, k1);
            svf_coefs(bass.c2, fc * 1.3f, k2);
        }
        switch (osc) {
        case DO_REESE: x = (blep_saw(p0, i0) + blep_saw(p2, i2)) * dsaw * 1.35f; break;
        case DO_PULSE: {
            float q = p1 + pw;
            if (q >= 1.0f)
                q -= 1.0f;
            x = (blep_saw(p1, i1) - blep_saw(q, i1)) * dsaw * 1.4f;
            break;
        }
        case DO_FM: x = sine_any(p1 + fmi * sine(p0)) * dsaw * 2.2f; break;
        case DO_ORGAN: x = (sine(p1) + 0.5f * sine(2.0f * p1) + 0.33f * sine(3.0f * p1) + 0.2f * sine(4.0f * p1)) * dsaw * 1.5f; break;
        case DO_SINE: x = (sine(p1) + 0.15f * sine(2.0f * p1)) * dsaw * 1.8f; break;
        case DO_SQUARE: {
            float q = p1 + 0.5f;
            if (q >= 1.0f)
                q -= 1.0f;
            x = (blep_saw(p1, i1) - blep_saw(q, i1)) * dsaw * 1.2f;
            break;
        }
        case DO_HOOVER: {
            float q0 = p0 + 0.3f, q2 = p2 + 0.3f;
            if (q0 >= 1.0f)
                q0 -= 1.0f;
            if (q2 >= 1.0f)
                q2 -= 1.0f;
            x = (blep_saw(p0, i0) - blep_saw(q0, i0) + blep_saw(p2, i2) - blep_saw(q2, i2)) * dsaw * 0.9f + 0.5f * dsaw * (2.0f * sp - 1.0f);
            break;
        }
        case DO_FIFTHS: x = (blep_saw(p1, i1) + 0.75f * blep_saw(p0, i0)) * dsaw * 1.1f; break;
        case DO_FMR: x = sine_any(p1 + (0.25f + 1.5f * bass.env) * sine(mp)) * dsaw * 2.0f; break;
        case DO_METAL: x = sine_any(p1 + (0.3f + 1.6f * bass.env) * sine(mp)) * dsaw * 2.0f; break;
        case DO_SYNC: x = blep_saw(mp, mi) * dsaw * 1.3f; break;
        default: x = (blep_saw(p0, i0) + blep_saw(p1, i1) + blep_saw(p2, i2)) * dsaw; break;
        }
        x += dsub * sine(sp);
        p0 += i0;
        p1 += i1;
        p2 += i2;
        sp += i1 * 0.5f;
        mp += mi;
        if (mp >= 1.0f)
            mp -= (float)(int)mp;
        if (osc == DO_SYNC && p1 >= 1.0f)
            mp = (p1 - 1.0f) * (mi / i1);
        if (p0 >= 1.0f)
            p0 -= 1.0f;
        if (p1 >= 1.0f)
            p1 -= 1.0f;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        if (sp >= 1.0f)
            sp -= 1.0f;
        x = svf_lp(x, bass.c1, &bass.s[0]);
        x = svf_lp(x, bass.c2, &bass.s[2]);
        amp += (tgt - amp) * rate;
        if (bl_on) {
            if (bl_up) {
                bl_env += 1.0f / (0.004f * EN_SR);
                if (bl_env >= 1.0f) {
                    bl_env = 1.0f;
                    bl_up = 0;
                }
            } else if (bl_gate > 0.0f) {
                bl_gate -= 1.0f;
                bl_env *= 0.99994785f;
            } else {
                bl_env *= 0.9948f;
            }
            x *= bl_env;
        }
        bass_o[j] = fm_tanhf(x * drv) * amp * g;
    }
    for (j = 0; j < 4; j++)
        bass.s[j] = fm_flush(bass.s[j]);
    bass.ph[0] = p0;
    bass.ph[1] = p1;
    bass.ph[2] = p2;
    bass.sph = sp;
    bass.mph = mp;
    bass.amp = amp;
}

/* the output: the DJ filter (-100 low-pass down to ~80 Hz .. 0 off .. 100 high-pass up to ~8 kHz, a little resonant) and the split
 * (drums left, bass right) */
static volatile int8_t dj_filter;
static volatile uint8_t out_split;
static float djs, dj_wet, dj_c[4], dj_s[4], dj_st[2 * EN_NSTEM];   /* (the stems' filters: one each) */
static inline float dj_tap(float x, float *s, int hp)
{
    const float v3 = x - s[1], v1 = dj_c[0] * s[0] + dj_c[1] * v3, v2 = s[1] + dj_c[1] * s[0] + dj_c[2] * v3;
    s[0] = fm_flush(2.0f * v1 - s[0]);
    s[1] = fm_flush(2.0f * v2 - s[1]);
    return hp ? x - dj_c[3] * v1 - v2 : v2;
}

/* one stretch of samples with nothing due inside it: the voices, the duck, into out (Q15 stereo, added) */
static void render_span(int32_t *out, uint32_t n)
{
    uint32_t j;
    const float depth = fm_minf(0.55f * KK->duck, 0.85f) * fm_minf(pulse_vol * 1.5f, 1.0f);
    float de = duck_env;
    int l;
    for (l = 0; l <= EN_NDRUM; l++) {            /* the mutes: ~5 ms */
        const float t = en_lane_mute >> l & 1u ? 0.0f : 1.0f;
        lane_on_g[l] += (t - lane_on_g[l]) * fm_minf((float)n / (0.005f * EN_SR), 1.0f);
    }
    if (bass.env_up) {
        bass.env += (float)n / (de_atk_s(par[Q_DE_ATK]) * EN_SR);
        if (bass.env >= 1.0f) {
            bass.env = 1.0f;
            bass.env_up = 0;
        }
    } else {
        bass.env *= fm_expf(-6.9f * (float)n / (de_dec_s(par[Q_DE_DEC]) * EN_SR));
    }
    {
        const float bt = sec == SEC_BREAK ? 1.0f : sec == SEC_BUILD ? -0.6f : 0.0f;
        brk += (bt - brk) * fm_minf((float)n / ((sec == SEC_BUILD ? 4.0f : 2.0f) * EN_SR), 1.0f);
    }
    gap_g += ((gap_on ? 0.0f : 1.0f) - gap_g) * fm_minf((float)n / (0.003f * EN_SR), 1.0f);
    bass_duck = duck_env;
    for (j = 0; j < n; j++)
        mixl[j] = mixr[j] = 0.0f;
    render_bass(n);
    render_drums(n);                             /* (a kick in it sets duck_env for the next span) */
    djs += ((float)dj_filter - djs) * fm_minf((float)n / (0.02f * EN_SR), 1.0f);   /* (the knob eased: no steps) */
    {
        const float a = fm_fabsf(djs) * 0.01f;
        dj_wet = fm_minf(a * 20.0f, 1.0f);       /* (the middle: the filter faded out, the sound untouched) */
        svf_coefs(dj_c, djs < 0.0f ? 20000.0f * fm_exp2f(-8.0f * a) : 20.0f * fm_exp2f(8.6f * a), 0.7f);
    }
    for (j = 0; j < n; j++) {
        const float b = bass_o[j] * (1.0f - depth * de);
        float l, r;
        int32_t vl, vr;
        if (out_split) {                         /* drums left, bass right: two tracks from one stereo recording */
            l = (mixl[j] + mixr[j]) * 0.5f * gap_g;
            r = b * gap_g;
        } else {
            l = (mixl[j] + b) * gap_g;
            r = (mixr[j] + b) * gap_g;
        }
        de *= duck_dec;
        if (dj_wet > 0.0f) {                     /* the DJ filter: low-pass to the left of the middle, high-pass to the right */
            l += (dj_tap(l, &dj_s[0], djs > 0.0f) - l) * dj_wet;
            r += (dj_tap(r, &dj_s[2], djs > 0.0f) - r) * dj_wet;
        }
        if (stems_on) {                          /* the stems: each lane and the bass through its own filter and ceiling */
            int k;
            for (k = 0; k < EN_NSTEM; k++) {
                float x = k ? 0.0f : b;
                int l2;
                if (!st_out[k])
                    continue;
                for (l2 = 0; l2 < EN_NDRUM; l2++)
                    if (STEM_LANES[k] >> l2 & 1u)
                        x += lane_o[l2][j];
                x *= gap_g;
                if (dj_wet > 0.0f)
                    x += (dj_tap(x, &dj_st[2 * k], djs > 0.0f) - x) * dj_wet;
                st_out[k][j] += (int32_t)(fm_tanhf(x * 0.9f) * 30000.0f);
            }
        }
        l = fm_tanhf(l * 0.9f);                  /* (a soft ceiling where PERF-1 has its master bus) */
        r = fm_tanhf(r * 0.9f);
        vl = out[2u * j] + (int32_t)(l * 30000.0f);
        vr = out[2u * j + 1u] + (int32_t)(r * 30000.0f);
        out[2u * j] = vl > 32767 ? 32767 : vl < -32768 ? -32768 : vl;
        out[2u * j + 1u] = vr > 32767 ? 32767 : vr < -32768 ? -32768 : vr;
    }
    duck_env = fm_flush(de);
}

/* MIDI clock (24 pulses a quarter; en_clock_in, clk_sync below): the sixteenths go at clk_rate, a little faster or
 * slower to stay with the pulses, 0 while waiting for the first pulse after a Start */
static float clk_rate = 1.0f;
static volatile int32_t clk_pulses = -1;          /* pulses since the start point, the first is 0 (-1: waiting for it) */
static volatile int32_t clk_base;                 /* the start point, in pulses from the top (a song position) */
static volatile int32_t clk_spp = -1;             /* a song position (sixteenths) waiting for Continue */
static volatile uint32_t clk_age = 0xFFFFFFFFu;   /* samples since the last pulse */
static volatile float clk_step;                  /* a sixteenth by the clock, in samples (0: none yet) */
static volatile uint8_t clk_cmd;                 /* 1 Start, 2 Continue, 3 Stop: for clk_sync */
static volatile uint8_t clk_run;                 /* the DAW is playing (from Start or Continue to Stop) */
volatile uint8_t en_clk_on;                      /* following a clock (the screen) */

/* the engine's block: the sixteenths, the waits and the voices, each on its own sample */
static void en_render(int32_t *out, uint32_t n)
{
    while (n) {
        uint32_t k = n > BLK ? BLK : n;
        if (en_playing && clk_rate > 0.0f) {
            int32_t w = waits_next();
            float to_tick = step_len - step_t;
            if (to_tick <= 0.0f) {
                tick();
                step_t -= step_len;
                continue;
            }
            to_tick /= clk_rate;
            if ((float)k > to_tick)
                k = (uint32_t)to_tick + 1u;
            if (w > 0 && (uint32_t)w < k)
                k = (uint32_t)w;
            step_t += (float)k * clk_rate;
            waits_step((int32_t)k);
        }
        render_span(out, k);
        out += 2u * k;
        if (stems_on) {
            int s_;
            for (s_ = 0; s_ < EN_NSTEM; s_++)
                if (st_out[s_])
                    st_out[s_] += k;
        }
        n -= k;
    }
}

/* ---- the controls (the main loop's; what the sequencer reads, it takes at a sixteenth) */
static void apply_all(void)
{
    int l;
    for (l = 0; l < EN_NDRUM; l++) {
        const int lv = par[Q_LANE + 3 * l], tu = par[Q_LANE + 3 * l + 1], dc = par[Q_LANE + 3 * l + 2];
        lane_g[l] = (float)(lv * lv) * 0.0001f;
        lane_tune[l] = (float)tu;                /* (semitones) */
        lane_dec[l] = (float)dc * 0.01f;
    }
    bass_vol = (float)(par[Q_BASS] * par[Q_BASS]) * 0.0001f;
    pulse_vol = (float)(par[Q_PULSE] * par[Q_PULSE]) * 0.0001f;
    reso_k = 1.5f - 0.0138f * (float)par[Q_DR_RES];
    drive_k = 1.0f + 0.04f * (float)par[Q_DRIVE];
    swing = (float)par[Q_SWING] * 0.005f;
    KK = &KITS[(unsigned)par[Q_KIT_KICK] < EN_NKIT ? par[Q_KIT_KICK] : 0];
    KS = &KITS[(unsigned)par[Q_KIT_SNARE] < EN_NKIT ? par[Q_KIT_SNARE] : 0];
    KP = &KITS[(unsigned)par[Q_KIT_PERC] < EN_NKIT ? par[Q_KIT_PERC] : 0];
    KH = &KITS[(unsigned)par[Q_KIT_HAT] < EN_NKIT ? par[Q_KIT_HAT] : 0];
    for (l = 0; l < 4; l++)
        hat_inc[l] = HAT_INC[l] * KH->h_metal;
    tunefac = fm_exp2f((float)par[Q_TUNE] * (1.0f / 1200.0f));
    if (!en_clk_on) {                            /* (following a clock, the clock sets the tempo: clk_sync) */
        step_len = EN_SR * 60.0f / ((float)par[Q_TEMPO] * 4.0f);
        duck_dec = fm_expf(-4.0f / (step_len * 3.0f));
    }
}
/* a style: the genre's sound and feel (the player's own over it after) and its steps' tricks */
static void en_style(int g)
{
    int l, s, k;
    genre = (unsigned)g < EN_NGENRE ? g : 0;
    for (k = 0; k < Q_N; k++)
        par[k] = STYLE_PAR[genre][k];
    for (l = 0; l < EN_NLANE; l++)
        for (s = 0; s < 16; s++)
            mods[l][s] = STYLE_MOD[genre][l][s];
    for (l = 0; l < EN_NDRUM; l++) {             /* PERF-1's tune (0.24 semitone a step) as semitones */
        const int v = par[Q_LANE + 3 * l + 1] * 24;
        par[Q_LANE + 3 * l + 1] = (int16_t)((v + (v < 0 ? -50 : 50)) / 100);
    }
    apply_all();
    gen_dirty = 1;
}
/* the key: its root (0 C .. 11 B) and scale (MODE_POOL), the chord home */
static void en_key(int root, int mode)
{
    par[Q_ROOT] = (int16_t)(((root % 12) + 12) % 12);
    par[Q_MODE] = (int16_t)((unsigned)mode < 8u ? mode : 0);
    pool = MODE_POOL[par[Q_MODE]];
    scale_set();
    deg = deg_prev = cad_n = 0;
    dom_apply();
    chord_set();
    hr_left = hr_bars() - 1;
    change_pending = drop_tonic = 0;
}
static uint16_t en_bass_mask(void)                /* the steps the bass line's mode plays (the screen's row) */
{
    const int m = par[Q_BL_MODE];
    switch (m) {
    case 0: return 0xFFFFu;
    case 1: return BL_ROOT[genre];
    case 2: return gmask[L_KICK];
    case 3: return 0x5555u;
    case 4: return 0x4444u;
    case 5: return 0x1111u;
    case 6: return BL_RIFF[genre].mask;
    default: return BL_FIG[(m - 7) % EN_NBLFIG].mask;
    }
}
static void en_play(int on)
{
    if (on && !en_playing) {
        int l;
        for (l = 0; l < EN_NDRUM; l++)
            en_seen[l] = gmask[l];               /* (the screen's trail starts from the pattern) */
        en_seen[EN_NDRUM] = en_bass_mask();
        sixteenth = 0;
        bars = 0;
        step_t = step_len;                      /* (the first sixteenth at once) */
        sec = SEC_GROOVE;
        sec_req = 0;
        grv_bars = 0;
        brk = 0.0f;
        gap_on = 0;
        bass_env_go();
    }
    if (!on) {
        int l;
        for (l = 0; l < EN_NDRUM; l++)
            drum[l].wait = drum[l].wait2 = drum[l].rn = 0;
        bl_wait = 0;
        bl_gate = 0.0f;
    }
    en_playing = (uint8_t)(on != 0);
}

/* ---- MIDI clock: a DAW's tempo and transport. en_clock_in (the main loop, as each real-time byte or song position
 * arrives, with the 24 MHz time it was taken) counts the pulses and times them; clk_sync (each render) follows: the tempo
 * from the last beat's pulses (once there are 6), the place by the count since the start point (a sixteenth is 6
 * pulses), held by speeding up or slowing down a little (a jump when it is more than two sixteenths out). Start: from
 * the top on the next pulse; a song position then Continue: from there; Continue alone: on from here; Stop. PLAY on the
 * FM-1 while the clock runs but the DAW is stopped: from the top on the next pulse. No pulses for half a second: the
 * module's own tempo again. */
static uint32_t clk_t[25];
static int clk_n;
static void en_clock_in(uint32_t st, uint32_t data, uint32_t ticks)
{
    if (st == 0xF8u) {
        int m;
        if (clk_age > (uint32_t)EN_SR)
            clk_n = 0;                           /* (after a pause: the gap is no tempo) */
        clk_t[clk_n % 25] = ticks;
        m = clk_n < 24 ? clk_n : 24;
        if (m >= 6)                              /* (fewer, and one late pulse throws the tempo far out) */
            clk_step = (float)(ticks - clk_t[(clk_n - m) % 25]) / (float)m * 6.0f * (EN_SR / 24000000.0f);
        clk_n++;
        clk_pulses++;
        clk_age = 0;
    } else if (st == 0xF2u) {
        clk_spp = (int32_t)(data & 0x3FFFu);     /* (MIDI beats: sixteenths) */
    } else if (st == 0xFAu) {
        clk_pulses = -1;
        clk_spp = -1;
        clk_run = 1;
        clk_cmd = 1;
    } else if (st == 0xFBu) {
        clk_run = 1;
        clk_cmd = 2;
    } else if (st == 0xFCu) {
        clk_run = 0;
        clk_cmd = 3;
    }
}
static void en_play_button(void)                 /* PLAY on the FM-1 */
{
    if (!en_playing && en_clk_on && !clk_run) {  /* a clock running, the DAW stopped: from the top, on its next pulse */
        clk_base = 0;
        clk_pulses = -1;
    }
    en_play(!en_playing);
}
static float clk_pos(void)                       /* where the sixteenths are, in pulses since the top */
{
    return ((float)sixteenth - 1.0f) * 6.0f + 6.0f * step_t / step_len;
}
static void clk_sync(uint32_t n)
{
    const uint8_t c = clk_cmd;
    if (c) {
        clk_cmd = 0;
        if (c == 3u) {
            en_play(0);
        } else if (c == 1u) {
            en_play(0);
            en_play(1);
            clk_base = 0;
        } else if (clk_spp >= 0) {               /* Continue from a song position: there, on the next pulse */
            const int32_t s = clk_spp;
            clk_spp = -1;
            en_play(0);
            en_play(1);
            sixteenth = (uint32_t)s;
            bars = (uint32_t)s / 16u;
            clk_base = s * 6;
            clk_pulses = -1;
        } else if (!en_playing) {                /* Continue alone: from where it stopped */
            en_playing = 1;
            clk_base = 0;
            clk_pulses = (int32_t)clk_pos();
        }
    }
    if (clk_age < (uint32_t)EN_SR)
        clk_age += n;
    en_clk_on = (uint8_t)(clk_age < (uint32_t)(EN_SR * 0.5f) && clk_step > 100.0f);
    clk_rate = 1.0f;
    if (!en_clk_on || !en_playing)
        return;
    if (clk_pulses < 0) {                        /* waiting for the start point's pulse */
        clk_rate = 0.0f;
        return;
    }
    if (step_len != clk_step) {
        step_len = clk_step;
        duck_dec = fm_expf(-4.0f / (step_len * 3.0f));
    }
    {
        const int32_t at = clk_pulses + clk_base;
        const float err = (float)at + 0.5f - clk_pos();
        if (err > 12.0f || err < -12.0f) {       /* far out (a pulse lost, a jump in the DAW): there at once */
            sixteenth = (uint32_t)(at / 6) + 1u;
            step_t = (float)(at % 6) * step_len * (1.0f / 6.0f);
        } else {
            clk_rate = fm_clampf(1.0f + 0.02f * err, 0.92f, 1.08f);
        }
    }
}
