/* SPDX-License-Identifier: GPL-3.0-only */
/* RHYTM-SEC-1: generative drums and bass after PERF-1 (perf/db_engine.c: deamonSLINKY's PERF-1, from RedtailWorks' HYP-1 and
 * Charles Vestal's AMB-1) as a fm1-dock module, for working up ideas: patterns drawn from each of 19 genres' feel and
 * left to evolve, a bass line on your key, sections and fills only when you ask.
 *   PLAY            start / stop
 *   SELECT          style (genre)    PRESETS  root note (both at once; a new style: its own density, complexity back to 45)
 *   ALGORITHM       the DJ filter: left a low-pass, right a high-pass, the middle off
 *   SEL             the knobs' page: DRUMS (tempo, swing, density, complexity), GEN (evolve, every, fills, ghosts),
 *                   BASS (line, cutoff, reso, sound), SOUND (one drum's level, tune, decay, kit)
 *   SEL + SELECT    the tempo        SEL + PRESETS     the scale
 *   SEL + ALGORITHM the output: stereo, or split (drums left, bass right)
 *                   (a host built with USB stems, --uac-stems 4, also records bass, kick, snare + clap, hats + perc,
 *                   one mono input each, after the master's two)
 *   GLO             a new pattern (next bar)        REC   lock the pattern (no evolving)
 *   SEQ             the chords: move with the style / hold on the root
 *   SAVE            breakdown on the next bar; again: the build; again: the drop
 *   FX              a fill next bar
 *   OCT- / OCT+     the bass an octave down / up
 *   keys F3 .. B3   mute kick, snare, clap, hat, open hat, perc, bass (lit: playing)
 *   keys C4 .. F4   a new draw for kick, snare, clap, hat, open hat, perc (next bar); on the SOUND page: the drum
 *                   its knobs set
 * Its settings and the pattern itself are kept: saved once it has been stopped and left alone for 10 s (fm1-dock writes a
 * save to flash 2 s later with the interrupts off, which would stop the sound if it were playing by then).
 * HOME held 1.5 s alone opens fm1-dock's module list. */
#include "hb_module.h"
#include "db_engine.c"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);

static const hb_host_t *H;

#define C_BG 0x0000u
#define C_DIM HB_RGB(70, 70, 70)
#define C_TXT HB_RGB(200, 200, 200)
#define C_ACC HB_RGB(255, 140, 180)              /* PERF-1's drums pink */
#define C_BASS HB_RGB(220, 60, 60)               /* its bass crimson */
#define C_GEN HB_RGB(150, 230, 120)
#define C_ACC_DIM HB_RGB(105, 55, 75)            /* a step in the pattern that did not play (a break, a fill's gap) */
#define C_BASS_DIM HB_RGB(95, 30, 30)
#define C_EMPTY HB_RGB(30, 30, 30)

/* the player's settings over the style's; bit k of set: field k below is the player's (else the style's) */
enum { U_TEMPO, U_SWING, U_DENS, U_GHOST, U_LINE, U_CUT, U_RES, U_SOUND, U_BOCT,
       U_LANE, U_KIT = U_LANE + 18, U_N = U_KIT + 4 };   /* U_LANE + 3 * lane + 0 level, 1 tune, 2 decay; U_KIT + kick, snare, hats, perc */
#define LQ(l) Q_LANE + 3 * (l), Q_LANE + 3 * (l) + 1, Q_LANE + 3 * (l) + 2
static const uint8_t U_Q[U_N] = {Q_TEMPO, Q_SWING, Q_DENS, Q_GHOST, Q_BL_MODE, Q_DR_CUT, Q_DR_RES, Q_DR_INST, Q_BASSOCT,
                                 LQ(0), LQ(1), LQ(2), LQ(3), LQ(4), LQ(5), Q_KIT_KICK, Q_KIT_SNARE, Q_KIT_HAT, Q_KIT_PERC};
_Static_assert(U_N <= 32, "set's bits");
static const uint8_t LANE_KIT[EN_NLANE] = {0, 1, 1, 2, 2, 3};   /* (the clap plays the snare's kit, the open hat the hats') */
static const char *const KIT_NAME[EN_NKIT] = {"Hypno", "House", "Techno", "Acid", "Break", "Minimal", "Hard", "808", "Dusty", "Heavy",
                                              "909", "Electro", "DnB", "Trap"};
static const char *const LANE_NAME[EN_NLANE] = {"KICK", "SNARE", "CLAP", "HAT", "OPEN HAT", "PERC"};
#define REC_MAGIC 0x34424450u                    /* "PDB4" (drum tune now in semitones) */
static struct {
    uint32_t magic;
    uint8_t style, root, mode, page;
    uint8_t cplx, evolve, every, fills, locked, chords, mute, sel;
    uint8_t split, pad[3];
    uint32_t set;
    int16_t u[U_N];
    uint8_t thr[EN_NLANE][16];
} rec;
_Static_assert(sizeof rec <= 240, "a module's settings record");
static uint8_t dirty;
static uint32_t idle_since;                      /* the time of the last touch, or of playing (the save waits 10 s after it) */

static volatile int8_t req_style = -1, req_key;   /* asked for: the engine takes them at the bar */

static void mark(void) { dirty = 1; }
static void user_apply(void)                     /* the player's values over the style's */
{
    int k, l;
    for (k = 0; k < U_N; k++)
        if (rec.set >> k & 1u)
            par[U_Q[k]] = rec.u[k];
    if (rec.set >> U_DENS & 1u)
        for (l = 1; l < EN_NLANE; l++)
            par[Q_DENS + l] = rec.u[U_DENS];
    apply_all();
}
static void user_set(int k, int v)
{
    rec.u[k] = (int16_t)v;
    rec.set |= 1u << k;
    user_apply();
    gen_dirty = 1;
    mark();
}
static void gen_push(void)                       /* the knobs the generator reads */
{
    cplx = rec.cplx;
    evolve = rec.evolve;
    every_i = rec.every;
    fills_i = rec.fills;
    locked = rec.locked;
    chords_on = rec.chords;
    gen_dirty = 1;
}

static void render(int32_t *out, uint32_t n)
{
    if (req_style >= 0) {                        /* (a new style or key at once, mid-bar) */
        rec.style = (uint8_t)req_style;
        req_style = -1;
        en_style(rec.style);
        user_apply();
        en_key(rec.root, rec.mode);
    }
    if (req_key) {
        req_key = 0;
        en_key(rec.root, rec.mode);
    }
    {                                            /* a host with USB stems, recording: bass, kick, snare + clap, hats + perc */
        int k;
        stems_on = 0;
        for (k = 0; k < EN_NSTEM; k++)
            stems_on |= (st_out[k] = HB_HOST_HAS(H, stem) ? H->stem((uint32_t)k) : 0) != 0;
    }
    clk_sync(n);                                 /* (a DAW's MIDI clock, when one comes) */
    en_render(out, n);
}

/* ---- the screen: each part is drawn when what it shows has changed (kept here as drawn) */
static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const SCALE[8] = {"Major", "Dorian", "Phrygian", "Lydian", "Mixolyd", "Minor", "Pent maj", "Pent min"};
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *d, int v)
{
    char b[8];
    int i = 0;
    if (v < 0) {
        *d++ = '-';
        v = -v;
    }
    do b[i++] = (char)('0' + v % 10); while ((v /= 10) && i < 7);
    while (i) *d++ = b[--i];
    *d = 0;
    return d;
}
static struct {
    int genre;
    char info[48];
    uint16_t cell[7][16], lbl[7];
    uint8_t chord;
    char title[12], flags[40], v[4][12];
    int page;
    uint8_t led[20];
} shown;

static void draw_head(int full)
{
    if (!full && shown.genre == genre)
        return;
    shown.genre = genre;
    H->fill(0, HB_TOP, 240, 36, C_BG);
    H->text(0, HB_TOP + 2, 240, 1, GENRE_NAME[genre], C_ACC, 1);
}
static void draw_info(int full)
{
    static const char *const SEC[3] = {"GROOVE", "BREAK", "BUILD"};
    char s[48], *p = s;
    memset(s, 0, sizeof s);                      /* (compared whole with what is shown) */
    p = cat(p, NOTE[rec.root % 12u]);
    p = cat(p, " ");
    p = cat(p, SCALE[rec.mode & 7u]);
    p = cat(p, "  ");
    if (en_clk_on)                               /* following a MIDI clock: its tempo */
        p = num(cat(p, "EXT "), (int)(EN_SR * 15.0f / clk_step + 0.5f));
    else
        p = num(p, par[Q_TEMPO]);
    p = cat(p, "  ");
    p = cat(p, en_playing ? SEC[en_sec % 3u] : "STOP");
    if (en_playing && en_sec == SEC_BUILD) {
        p = cat(p, " ");
        num(p, en_sec_bars);
    }
    if (!full && !memcmp(s, shown.info, sizeof s))
        return;
    memcpy(shown.info, s, sizeof s);
    H->text(0, 62, 240, 0, s, C_TXT, 1);
}
/* the grid: while playing, what each step played over the last 16 (bright), the pattern's steps that did not (dim: a
 * break, a build, a fill's gap), the step playing (white); stopped, the pattern. The bass row is labelled with the
 * chord's root. */
static void draw_grid(int full)
{
    static const char *const LBL[6] = {"K", "S", "C", "H", "O", "P"};
    uint16_t cell[7][16], lbl[7];
    const int cur = en_playing ? (int)((sixteenth - 1u) & 15u) : -1, sound = rec.page % 4u == 3u;
    const uint8_t chord = en_chord_root % 12u;
    int r, s;
    for (r = 0; r < 7; r++) {
        const uint16_t pat = r < 6 ? gmask[r] : en_bass_mask(), played = en_playing ? en_seen[r] : pat;
        const int muted = en_lane_mute >> r & 1u;
        const uint16_t on = muted ? C_DIM : r < 6 ? C_ACC : C_BASS, dim = muted ? C_EMPTY : r < 6 ? C_ACC_DIM : C_BASS_DIM;
        lbl[r] = sound && r == rec.sel ? C_GEN : muted ? C_DIM : r < 6 ? C_TXT : C_BASS;
        for (s = 0; s < 16; s++) {
            const int hit = played >> s & 1u, inpat = pat >> s & 1u;
            cell[r][s] = s == cur ? (hit ? 0xFFFFu : inpat ? HB_RGB(90, 90, 90) : HB_RGB(55, 55, 55))
                                  : hit ? on : inpat ? dim : C_EMPTY;
        }
    }
    if (!full && !memcmp(cell, shown.cell, sizeof cell) && !memcmp(lbl, shown.lbl, sizeof lbl) && chord == shown.chord)
        return;
    memcpy(shown.cell, cell, sizeof cell);
    memcpy(shown.lbl, lbl, sizeof lbl);
    shown.chord = chord;
    H->cv_begin(240, 88, C_BG);
    for (r = 0; r < 7; r++) {
        const int y = r * 12 + 2;
        H->cv_text(r < 6 ? 4 : 1, y - 3, 0, r < 6 ? LBL[r] : NOTE[chord], lbl[r]);
        for (s = 0; s < 16; s++)
            H->cv_rect(22 + s * 13, y, 11, 9, cell[r][s]);
    }
    H->cv_blit(0, 86);
}
static void draw_knobs(int full)
{
    static const char *const NAME[4][4] = {{"TEMPO", "SWING", "DENS", "CMPLX"}, {"EVOLVE", "EVERY", "FILLS", "GHOST"},
                                           {"LINE", "CUTOFF", "RESO", "SOUND"}, {"LEVEL", "TUNE", "DECAY", "KIT"}};
    static const char *const PAGE[3] = {"DRUMS", "GEN", "BASS"};
    static const uint16_t PC[4] = {C_ACC, C_GEN, C_BASS, C_ACC};
    char v[4][12], title[12], flags[40], cand[5][8];
    int k, nc = 0;
    int32_t room;
    const int pg = rec.page % 4u, sl = rec.sel % EN_NLANE;
    memset(v, 0, sizeof v);
    memset(flags, 0, sizeof flags);
    memset(title, 0, sizeof title);
    if (pg == 0) {
        num(v[0], par[Q_TEMPO]);
        num(v[1], par[Q_SWING]);
        num(v[2], par[Q_DENS]);
        num(v[3], rec.cplx);
    } else if (pg == 1) {
        num(v[0], rec.evolve);
        cat(num(v[1], EVERY[rec.every % 5u]), " bar");
        if (FILLS_N[rec.fills % 5u])
            cat(num(v[2], FILLS_N[rec.fills % 5u]), " bar");
        else
            cat(v[2], "Off");
        num(v[3], par[Q_GHOST]);
    } else if (pg == 2) {
        cat(v[0], BL_NAME[(unsigned)par[Q_BL_MODE] < 16u ? par[Q_BL_MODE] : 0]);
        num(v[1], par[Q_DR_CUT]);
        num(v[2], par[Q_DR_RES]);
        cat(v[3], DRONE_NAME[(unsigned)par[Q_DR_INST] < EN_NSOUND ? par[Q_DR_INST] : 0]);
    } else {
        char *p = v[1];
        num(v[0], par[Q_LANE + 3 * sl]);
        if (par[Q_LANE + 3 * sl + 1] > 0)
            *p++ = '+';
        cat(num(p, par[Q_LANE + 3 * sl + 1]), " st");   /* (semitones) */
        num(v[2], par[Q_LANE + 3 * sl + 2]);
        cat(v[3], KIT_NAME[(unsigned)par[Q_KIT_KICK + LANE_KIT[sl]] < EN_NKIT ? par[Q_KIT_KICK + LANE_KIT[sl]] : 0]);
    }
    cat(title, pg == 3 ? LANE_NAME[sl] : PAGE[pg]);
    /* the flags, most wanted first, as many as fit beside the title (LOCK and HOLD also show on REC and SEQ) */
    if (dj_filter)
        num(cat(cand[nc++], dj_filter < 0 ? "LP" : "HP"), dj_filter < 0 ? -dj_filter : dj_filter);
    if (rec.split)
        cat(cand[nc++], "SPLIT");
    if (par[Q_BASSOCT])
        num(cat(cand[nc], par[Q_BASSOCT] > 0 ? "O+" : "O"), par[Q_BASSOCT]), nc++;
    if (!rec.chords)
        cat(cand[nc++], "HOLD");
    if (rec.locked)
        cat(cand[nc++], "LOCK");
    room = 240 - 8 - H->text_w(0, title) - 16;
    for (k = 0; k < nc; k++) {
        char *e = flags + strlen(flags);
        if (H->text_w(0, flags) + (k ? H->text_w(0, " ") : 0) + H->text_w(0, cand[k]) > room)
            break;
        if (k)
            e = cat(e, " ");
        cat(e, cand[k]);
    }
    if (!full && pg == shown.page && !memcmp(v, shown.v, sizeof v) && !memcmp(title, shown.title, sizeof title) &&
        !memcmp(flags, shown.flags, sizeof flags))
        return;
    shown.page = pg;
    memcpy(shown.v, v, sizeof v);
    memcpy(shown.title, title, sizeof title);
    memcpy(shown.flags, flags, sizeof flags);
    H->fill(0, 178, 240, 62, C_BG);
    H->text(8, 178, 232, 0, title, PC[pg], 0);
    H->text(240 - 8 - H->text_w(0, flags), 178, H->text_w(0, flags) + 1, 0, flags, C_GEN, 0);
    for (k = 0; k < 4; k++) {
        H->text(k * 60, 200, 60, 0, NAME[pg][k], C_DIM, 1);
        H->text(k * 60, 218, 60, 0, v[k], C_TXT, 1);
    }
}
/* the lights: sent when they change (0 off, 1 lit, 2 the glow) */
static void lights(int full)
{
    static const uint8_t BTN[7] = {HB_BTN_PLAY, HB_BTN_SEL, HB_BTN_REC, HB_BTN_SEQ, HB_BTN_SAVE, HB_BTN_FX, HB_BTN_GLO};
    uint8_t led[20];
    int k;
    led[0] = en_playing ? 1u : 2u;
    led[1] = 2u;
    led[2] = rec.locked ? 1u : 2u;
    led[3] = rec.chords ? 1u : 2u;
    led[4] = en_sec != SEC_GROOVE || en_sec_req ? 1u : 2u;
    led[5] = fill_q ? 1u : 2u;
    led[6] = gen_new ? 1u : 2u;
    for (k = 0; k < 7; k++)
        led[7 + k] = en_lane_mute >> k & 1u ? 0u : 1u;
    for (k = 0; k < 6; k++)
        led[14 + k] = (rec.page % 4u == 3u ? rec.sel == k : gen_lane_new >> k & 1u) ? 1u : 0u;
    for (k = 0; k < 20; k++)
        if (full || led[k] != shown.led[k]) {
            shown.led[k] = led[k];
            H->led(k < 7 ? BTN[k] : HB_LED_KEY(k - 7), led[k]);
        }
}

/* ---- the panel */
static uint8_t sel_held, sel_used;
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t step1(int32_t d) { return d > 0 ? 1 : -1; }
static void knob(int k, int32_t d)
{
    const int sl = rec.sel % EN_NLANE;
    switch (rec.page % 4u * 4u + (uint32_t)k) {
    case 0: user_set(U_TEMPO, clampi(par[Q_TEMPO] + d, 60, 200)); break;
    case 1: user_set(U_SWING, clampi(par[Q_SWING] + d, 0, 100)); break;
    case 2: user_set(U_DENS, clampi(par[Q_DENS] + d * 2, 0, 100)); break;
    case 3: rec.cplx = (uint8_t)clampi(rec.cplx + d * 2, 0, 100); break;
    case 4: rec.evolve = (uint8_t)clampi(rec.evolve + d * 2, 0, 100); break;
    case 5: rec.every = (uint8_t)clampi(rec.every + step1(d), 0, 4); break;
    case 6: rec.fills = (uint8_t)clampi(rec.fills + step1(d), 0, 4); break;
    case 7: user_set(U_GHOST, clampi(par[Q_GHOST] + d * 2, 0, 100)); break;
    case 8: user_set(U_LINE, clampi(par[Q_BL_MODE] + step1(d), 0, 15)); break;
    case 9: user_set(U_CUT, clampi(par[Q_DR_CUT] + d * 2, 0, 100)); break;
    case 10: user_set(U_RES, clampi(par[Q_DR_RES] + d * 2, 0, 100)); break;
    case 11: user_set(U_SOUND, clampi(par[Q_DR_INST] + step1(d), 0, EN_NSOUND - 1)); break;
    case 12: user_set(U_LANE + 3 * sl, clampi(par[Q_LANE + 3 * sl] + d * 2, 0, 100)); break;
    case 13: user_set(U_LANE + 3 * sl + 1, clampi(par[Q_LANE + 3 * sl + 1] + step1(d), -24, 24)); break;   /* semitones */
    case 14: user_set(U_LANE + 3 * sl + 2, clampi(par[Q_LANE + 3 * sl + 2] + d * 2, 0, 100)); break;
    default: user_set(U_KIT + LANE_KIT[sl], clampi(par[Q_KIT_KICK + LANE_KIT[sl]] + step1(d), 0, EN_NKIT - 1)); break;
    }
    gen_push();
    mark();
}
static void ui(uint32_t ev, int32_t arg)
{
    const uint32_t id = HB_EV_ID(ev);
    idle_since = H->ms();
    switch (HB_EV_KIND(ev)) {
    case HB_EV_ENC:
        if (sel_held)
            sel_used = 1;                        /* (SEL held for a knob: no page turn when it is let go) */
        if (id == HB_ENC_SELECT && sel_held) {
            user_set(U_TEMPO, clampi(par[Q_TEMPO] + arg, 60, 200));
        } else if (id == HB_ENC_SELECT) {
            req_style = (int8_t)clampi((req_style >= 0 ? req_style : rec.style) + step1(arg), 0, EN_NGENRE - 1);
            rec.set &= 1u << U_BOCT;             /* (a new style: its own tempo, density, sound and line; the octave is yours) */
            rec.cplx = 45;                       /* (and complexity back to the start) */
            gen_push();
            mark();
        } else if (id == HB_ENC_PRESETS && sel_held) {
            rec.mode = (uint8_t)clampi(rec.mode + step1(arg), 0, 7);
            req_key = 1;
            mark();
        } else if (id == HB_ENC_PRESETS) {
            rec.root = (uint8_t)((rec.root + 12 + step1(arg)) % 12);
            req_key = 1;
            mark();
        } else if (id == HB_ENC_ALGORITHM && sel_held) {
            rec.split = arg > 0;
            out_split = rec.split;
            mark();
        } else if (id == HB_ENC_ALGORITHM) {
            dj_filter = (int8_t)clampi(dj_filter + arg * 3, -100, 100);
            if (dj_filter > -3 && dj_filter < 3)
                dj_filter = 0;                   /* (a detent in the middle: off) */
        } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) {
            knob((int)(id - HB_ENC_KNOB1), arg);
        }
        break;
    case HB_EV_BTN_DOWN:
        switch (id) {
        case HB_BTN_PLAY: en_play_button(); break;
        case HB_BTN_SEL: sel_held = 1; sel_used = 0; break;   /* (the page turns when it is let go, unless it was held for a knob) */
        case HB_BTN_SAVE:                         /* groove: break; break: build; build: the drop (all on the bar) */
            sec_req = 1 + (sec == SEC_GROOVE ? CMD_BREAK : sec == SEC_BREAK ? CMD_BUILD : CMD_DROP);
            break;
        case HB_BTN_FX: fill_q = 1; break;
        case HB_BTN_GLO: gen_new = 1; mark(); break;
        case HB_BTN_REC: rec.locked ^= 1u; gen_push(); mark(); break;
        case HB_BTN_SEQ: rec.chords ^= 1u; gen_push(); mark(); break;
        case HB_BTN_OCT_DOWN: user_set(U_BOCT, clampi(par[Q_BASSOCT] - 1, -2, 2)); break;
        case HB_BTN_OCT_UP: user_set(U_BOCT, clampi(par[Q_BASSOCT] + 1, -2, 2)); break;
        default: break;
        }
        break;
    case HB_EV_BTN_UP:
        if (id == HB_BTN_SEL) {
            if (!sel_used) {
                rec.page = (uint8_t)((rec.page + 1u) % 4u);
                mark();
            }
            sel_held = 0;
        }
        break;
    case HB_EV_KEY_DOWN:
        if (id < 7u) {
            en_lane_mute ^= 1u << id;
            rec.mute = (uint8_t)en_lane_mute;
            mark();
        } else if (id < 13u && rec.page % 4u == 3u) {
            rec.sel = (uint8_t)(id - 7u);        /* the SOUND page: this drum's knobs */
            mark();
        } else if (id < 13u) {
            gen_lane_new |= (uint8_t)(1u << (id - 7u));
            mark();
        }
        break;
    default:
        break;
    }
}

/* the save: the settings and the pattern, when either changed */
static int changed(void) { return dirty || memcmp(rec.thr, thr, sizeof thr) != 0; }
static void save_now(void)
{
    dirty = 0;
    memcpy(rec.thr, thr, sizeof rec.thr);        /* (the pattern too: the idea comes back as it was) */
    H->save(&rec, sizeof rec);
}

static void draw(uint32_t full)
{
    const uint32_t now = H->ms();
    draw_head((int)full);
    draw_info((int)full);
    draw_grid((int)full);
    draw_knobs((int)full);
    lights((int)full);
    if (en_playing)
        idle_since = now;
    if (now - idle_since >= 10000u && changed())   /* (stopped and left alone for 10 s: fm1-dock writes it 2 s on) */
        save_now();
}

static void note(uint32_t ch, uint32_t key, uint32_t vel) { (void)ch; (void)key; (void)vel; }
static void cc(uint32_t ch, uint32_t num_, uint32_t val) { (void)ch; (void)num_; (void)val; }
static void midi(uint32_t st, uint32_t d1, uint32_t d2)   /* (every message; the clock, transport and song position are used) */
{
    if (st >= 0xF8u || st == 0xF2u)
        en_clock_in(st, d1 | d2 << 7, H->ticks());
}

static int init(const hb_host_t *h)
{
    int l, ok;
    H = h;
    ok = !H->load(&rec, sizeof rec) && rec.magic == REC_MAGIC && rec.style < EN_NGENRE;
    rs ^= H->rand() | 1u;
    if (!ok) {
        memset(&rec, 0, sizeof rec);
        rec.magic = REC_MAGIC;
        rec.style = 2;                           /* house, a steady place to start */
        rec.root = 9;                            /* A minor */
        rec.mode = 5;
        rec.cplx = 45;
        rec.evolve = 40;
        rec.every = 1;                           /* every 2 bars */
        rec.fills = 2;                           /* a fill every 8 bars */
        rec.chords = 1;
        for (l = 0; l < EN_NLANE; l++)
            thr_draw(l);
    } else {
        memcpy(thr, rec.thr, sizeof thr);
    }
    en_lane_mute = rec.mute & 0x7Fu;
    out_split = rec.split;
    dj_filter = 0;
    req_style = -1;
    req_key = 0;
    idle_since = H->ms();
    en_play(0);
    en_style(rec.style);
    user_apply();
    en_key(rec.root, rec.mode);
    gen_push();
    gen_all();
    H->log("rhytm-sec-1: generative drums and bass after PERF-1 (deamonSLINKY/PERF-1 848b3c6)");
    return 0;
}

static void release(void)                        /* (leaving: the last chance to keep it; fm1-dock writes it 2 s on) */
{
    en_play(0);
    if (changed())
        save_now();
}

HB_DESC = {HB_ABI, "RHYTM-SEC-1", init, release, note, cc, render, ui, draw, 0, 0, HB_F_PANEL, midi};
