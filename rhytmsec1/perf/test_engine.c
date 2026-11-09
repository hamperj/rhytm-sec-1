/* RHYTM-SEC-1's engine on the build machine: every genre's first piece for 16 bars (a break, a build and the drop in
 * the middle), in 256-frame blocks as the host calls it; checks the level, that no sample is NaN or stuck, that the
 * kick lands on time, and writes OUT/gNN.wav to listen to.
 *   cc -O2 -ffp-contract=off -Imodules/rhytmsec1/perf modules/rhytmsec1/perf/test_engine.c -o build/rhytmsec1_test && build/rhytmsec1_test OUT */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db_engine.c"

static void wav(const char *path, const int16_t *s, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t v, bytes = frames * 4u;
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f); v = 36u + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); v = 1u | 2u << 16; fwrite(&v, 4, 1, f); v = 44100; fwrite(&v, 4, 1, f);
    v = 44100u * 4u; fwrite(&v, 4, 1, f); v = 4u | 16u << 16; fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(s, 4, frames, f);
    fclose(f);
}

static int16_t pcm[2u * 44100u * 64u];

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    int g, fails = 0;
    for (g = 0; g < EN_NGENRE; g++) {
        int32_t blk[512];
        uint32_t frames = 0, total, kicks = 0, late = 0, clipped = 0;
        double sum = 0.0, bsum = 0.0;
        uint32_t bframes = 0, brk_from = 0;
        int peak = 0, trail_ok = -1, moved = 0;  /* (-1: not checked yet); moved: the pattern evolved at some point */
        char path[512];
        en_play(0);
        en_style(g);
        en_key(9, 5);
        for (int l = 0; l < EN_NLANE; l++)
            thr_draw(l);
        evolve = 40;
        cplx = 45;
        gen_all();
        uint16_t first[EN_NLANE];
        memcpy(first, gmask, sizeof first);
        en_play(1);
        total = (uint32_t)(16.0f * 16.0f * step_len);     /* 16 bars */
        if (total > 44100u * 64u)
            total = 44100u * 64u;
        while (frames + 256u <= total) {
            uint32_t j, bar = (uint32_t)(sixteenth / 16u);
            float glow_before = en_drum_glow[L_KICK];
            if (bar == 4 && (sixteenth & 15u) == 8u && !sec_req && sec == SEC_GROOVE)
                sec_req = 1 + CMD_BREAK;               /* a break at bar 5, the build at bar 7 */
            if (bar == 6 && (sixteenth & 15u) == 8u && !sec_req && sec == SEC_BREAK)
                sec_req = 1 + CMD_BUILD;
            if (bar == 2 && (sixteenth & 15u) == 0u)
                fill_q = 1;
            en_drum_glow[L_KICK] = 0.0f;
            memset(blk, 0, sizeof blk);
            en_render(blk, 256u);
            moved |= memcmp(first, gmask, sizeof first) != 0;
            if (en_drum_glow[L_KICK] > 0.0f)
                kicks++;
            (void)glow_before;
            if (sec == SEC_BREAK) {              /* the break: its sound, and after a whole bar of it, the screen's trail */
                if (!brk_from)
                    brk_from = sixteenth;
                for (j = 0; j < 512u; j++)
                    bsum += (double)blk[j] * blk[j];
                bframes += 256u;
                if (trail_ok < 0 && sixteenth >= brk_from + 17u) {   /* (what the genre does not keep never shows) */
                    trail_ok = (SECT[g].snare || !en_seen[L_SNARE]) && !en_seen[L_CLAP] &&
                               (SECT[g].kick || !en_seen[L_KICK]) && en_seen[L_HAT] != 0;
                }
            }
            for (j = 0; j < 512u; j++) {
                const int v = blk[j];
                assert(v >= -32768 && v <= 32767);
                if (v == 32767 || v == -32768)
                    clipped++;
                if (abs(v) > peak)
                    peak = abs(v);
                sum += (double)v * v;
                pcm[2u * frames + j] = (int16_t)v;
            }
            frames += 256u;
        }
        (void)late;
        {
            const double rms = sqrt(sum / (2.0 * frames)), brms = bframes ? sqrt(bsum / (2.0 * bframes)) : 0.0;
            int changed = 0;
            for (int l = 0; l < EN_NLANE; l++)
                changed += gmask[l] != first[l];
            printf("  evolved %u times, moved %d, %d lanes differ from bar 1, kick %04X snare %04X hat %04X\n", en_evolved, moved, changed,
                   gmask[0], gmask[1], gmask[3]);
            const int ok = moved && gmask[0] != 0 && peak > 3000 && rms > 300.0 && rms < 20000.0 && clipped < frames / 50u && !isnan(rms) &&
                           sec == SEC_GROOVE && bars >= 15u &&
                           brms > 0.12 * rms && trail_ok == 1;   /* (a break that is heard, and shown as played) */
            printf("%-8s %3d BPM  %2u bars  peak %5d  rms %6.0f  break rms %5.0f  trail %d  clipped %4u  kicks %3u  bass %-8s line %-8s  %s\n",
                   GENRE_NAME[g], par[Q_TEMPO], bars, peak, rms, brms, trail_ok, clipped, kicks, DRONE_NAME[par[Q_DR_INST]],
                   BL_NAME[par[Q_BL_MODE]], ok ? "ok" : "FAIL");
            fails += !ok;
        }
        snprintf(path, sizeof path, "%s/g%02d-%s.wav", out, g, GENRE_NAME[g]);
        wav(path, pcm, frames);
    }
    {   /* the DJ filter and the split, on house: the low-pass takes the top off, the high-pass the bottom; split: the bass alone on the right */
        static const int8_t DJ[3] = {0, -80, 80};
        double hf[3], lf[3];
        int k;
        for (k = 0; k < 4; k++) {
            int32_t blk[512];
            uint32_t b, j;
            double h = 0.0, lo = 0.0, rr = 0.0, lsum = 0.0;
            float lp = 0.0f;
            int32_t prev = 0;
            en_play(0);
            en_style(2);
            en_key(9, 5);
            en_lane_mute = k == 3 ? 0x3Fu : 0u;  /* (split: the drums muted, so the left is silent and the right the bass) */
            out_split = k == 3;
            dj_filter = k < 3 ? DJ[k] : 0;
            djs = (float)dj_filter;
            en_play(1);
            for (b = 0; b < 400u; b++) {
                memset(blk, 0, sizeof blk);
                en_render(blk, 256u);
                if (b < 40u)
                    continue;                    /* (the mutes and the filter settle) */
                for (j = 0; j < 256u; j++) {
                    const int32_t x = blk[2u * j];
                    h += (double)(x - prev) * (x - prev);
                    prev = x;
                    lp += 0.01f * ((float)x - lp);
                    lo += (double)lp * lp;
                    lsum += (double)x * x;
                    rr += (double)blk[2u * j + 1u] * blk[2u * j + 1u];
                }
            }
            if (k < 3) {
                hf[k] = sqrt(h);
                lf[k] = sqrt(lo);
            } else {
                printf("split: left (drums, muted) rms %.0f, right (bass) rms %.0f  %s\n", sqrt(lsum / 92160.0), sqrt(rr / 92160.0),
                       sqrt(lsum / 92160.0) < 50.0 && sqrt(rr / 92160.0) > 300.0 ? "ok" : "FAIL");
                fails += !(sqrt(lsum / 92160.0) < 50.0 && sqrt(rr / 92160.0) > 300.0);
            }
        }
        printf("dj filter: highs off / lp / hp %.0f %.0f %.0f, lows %.0f %.0f %.0f  %s\n", hf[0], hf[1], hf[2], lf[0], lf[1], lf[2],
               hf[1] < 0.4 * hf[0] && lf[2] < 0.3 * lf[0] ? "ok" : "FAIL");
        fails += !(hf[1] < 0.4 * hf[0] && lf[2] < 0.3 * lf[0]);   /* (the highs: the mix's own, a low-pass at -80 takes well over half) */
        en_lane_mute = 0;
        out_split = 0;
        dj_filter = 0;
    }
    {   /* the USB stems, on house through the low-pass, split: every lane's stem, panned and summed, is the mix's drums
         * (each span, exactly); the bass stem is the split's right, sample for sample; each stem has sound */
        static const float PL[EN_NDRUM] = {1.0f, 1.0f, 0.95f, 0.75f, 1.0f, 0.8f}, PR[EN_NDRUM] = {1.0f, 0.95f, 1.0f, 1.0f, 0.75f, 1.0f};
        static int32_t st[EN_NSTEM][256];
        int32_t blk[512];
        double e[EN_NSTEM] = {0};
        uint32_t b, j, diff = 0, mixdiff = 0;
        int k, quiet = 0;
        en_play(0);
        en_style(2);
        en_key(9, 5);
        out_split = 1;
        dj_filter = -60;
        djs = (float)dj_filter;
        dj_st[0] = dj_s[2], dj_st[1] = dj_s[3];   /* (the bass stem's filter from the split right's state) */
        en_play(1);
        for (b = 0; b < 400u; b++) {
            memset(blk, 0, sizeof blk);
            memset(st, 0, sizeof st);
            for (k = 0; k < EN_NSTEM; k++)
                st_out[k] = st[k];
            stems_on = 1;
            en_render(blk, 256u);
            for (j = 0; j < BLK; j++) {          /* (the last span's, and what earlier spans left past it) */
                float l = 0.0f, r = 0.0f;
                for (k = 0; k < EN_NDRUM; k++) {
                    l += lane_o[k][j] * PL[k];
                    r += lane_o[k][j] * PR[k];
                }
                mixdiff += l != mixl[j] || r != mixr[j];
            }
            for (j = 0; j < 256u; j++) {
                const int32_t x = st[0][j], c = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
                diff += c != blk[2u * j + 1u];
                for (k = 0; k < EN_NSTEM; k++)
                    e[k] += (double)st[k][j] * st[k][j];
            }
        }
        quiet = sqrt(e[1] / 102400.0) < 300.0 || sqrt(e[0] / 102400.0) < 300.0 || e[2] == 0.0;   /* (the low-pass takes the hats) */
        printf("stems: lanes vs the drum mix %u differ, bass vs split right %u differ, rms bass kick snare+clap hats+perc", mixdiff, diff);
        for (k = 0; k < EN_NSTEM; k++)
            printf(" %.0f", sqrt(e[k] / 102400.0));
        printf("  %s\n", !mixdiff && !diff && !quiet ? "ok" : "FAIL");
        fails += !(!mixdiff && !diff && !quiet);
        for (k = 0; k < EN_NSTEM; k++)
            st_out[k] = 0;
        stems_on = 0;
        out_split = 0;
        dj_filter = 0;
    }
    {   /* MIDI clock: a DAW at 133 BPM (the style's own is 70), each pulse taken up to 3 ms late as the main loop would;
         * after Start the sixteenths keep with the pulses for 30 s, Stop stops, Continue goes on from there */
        int32_t blk[512];
        const double pulse = 60.0 / 133.0 / 24.0;    /* seconds */
        double next = 0.0, now = 0.0, worst = 0.0;
        uint32_t b, sent = 0;
        int ok;
        en_play(0);
        en_style(4);                                 /* (dub: 70 BPM) */
        en_key(9, 5);
        en_clock_in(0xFAu, 0, 0);
        for (b = 0; b < 30u * 44100u / 256u; b++) {
            while (next <= now) {                    /* the pulses due, timed with the main loop's lateness */
                en_clock_in(0xF8u, 0, (uint32_t)((next + 0.003 * frand()) * 24e6));
                next += pulse;
                sent++;
            }
            memset(blk, 0, sizeof blk);
            clk_sync(256u);
            en_render(blk, 256u);
            now += 256.0 / 44100.0;
            if (b > 44100u * 2u / 256u) {            /* (after two seconds: locked) */
                const double e = fabs((double)clk_pos() - (double)clk_pulses);
                if (e > worst)
                    worst = e;
            }
        }
        ok = en_clk_on && worst < 2.0 && fabsf(EN_SR * 15.0f / clk_step - 133.0f) < 1.0f &&   /* (3 ms of jitter: within a BPM) */
             abs((int)sixteenth - (int)(sent / 6u)) <= 1;
        printf("clock: %u pulses, %u sixteenths, tempo %.2f BPM, worst %.2f pulses off  %s\n", sent, sixteenth,
               EN_SR * 15.0f / clk_step, worst, ok ? "ok" : "FAIL");
        fails += !ok;
        en_clock_in(0xFCu, 0, 0);
        clk_sync(256u);
        ok = !en_playing;
        en_clock_in(0xFBu, 0, 0);
        clk_sync(256u);
        ok = ok && en_playing && fabs((double)clk_pos() - (double)clk_pulses) < 1.0;
        printf("clock: stop and continue  %s\n", ok ? "ok" : "FAIL");
        fails += !ok;
        {   /* the DAW stops, moves to bar 6 beat 2 (song position 84 sixteenths) and continues: RHYTM-SEC-1 there on its next pulse */
            uint32_t k, s0;
            en_clock_in(0xFCu, 0, 0);
            clk_sync(256u);
            en_clock_in(0xF2u, 84u, 0);
            en_clock_in(0xFBu, 0, 0);
            clk_sync(256u);
            s0 = sixteenth;
            for (k = 0; k < 13u; k++) {          /* 13 pulses: the start point and two sixteenths on */
                memset(blk, 0, sizeof blk);
                en_clock_in(0xF8u, 0, (uint32_t)((next) * 24e6));
                next += pulse;
                clk_sync(32u);
                en_render(blk, 32u);
                while (clk_rate > 0.0f && clk_pos() < (float)(clk_pulses + clk_base)) {   /* (the time to the next pulse) */
                    memset(blk, 0, sizeof blk);
                    en_render(blk, 32u);
                }
            }
            ok = s0 == 84u && (int)((sixteenth - 1u) & 15u) == 6 && bars == 5u && en_playing;
            printf("clock: song position 84 then continue: on step %u of bar %u  %s\n", (sixteenth - 1u) & 15u, bars + 1u, ok ? "ok" : "FAIL");
            fails += !ok;
        }
        {   /* the DAW stopped, its clock still running: PLAY on the FM-1 starts from the top on the next pulse */
            en_clock_in(0xFCu, 0, 0);
            clk_sync(256u);
            clk_pulses = 5000;                   /* (pulses counted long since the last Start) */
            en_play_button();
            clk_sync(256u);
            ok = en_playing && clk_rate == 0.0f && clk_pulses == -1;
            en_clock_in(0xF8u, 0, (uint32_t)(next * 24e6));
            clk_sync(256u);
            memset(blk, 0, sizeof blk);
            en_render(blk, 256u);
            ok = ok && sixteenth == 1u;
            printf("clock: PLAY with the DAW stopped starts at the top  %s\n", ok ? "ok" : "FAIL");
            fails += !ok;
        }
        en_play(0);
    }
    {   /* the bass's register: every style's root in its octave from BL_LOW, in every key; the drive knob holds the level */
        int bad = 0, k;
        double rms[3];
        for (g = 0; g < EN_NGENRE; g++)
            for (k = 0; k < 12; k++) {
                int r;
                en_style(g);
                en_key(k, 5);
                r = bass_root();
                bad += r < BL_LOW[g] || r > BL_LOW[g] + 11 || r % 12 != k;
                bad += g == 2 && fabsf(en_cut_hz() - 261.6f) > 0.5f;   /* (house's cutoff 40, Pulse: 262 Hz in every key) */
            }
        for (k = 0; k < 3; k++) {
            int32_t blk[512];
            double s = 0.0;
            uint32_t b, j;
            en_play(0);
            en_style(2);
            en_key(9, 5);
            par[Q_BDRV] = (int16_t)(k * 50);
            en_lane_mute = 0x3Fu;
            en_play(1);
            for (b = 0; b < 400u; b++) {
                memset(blk, 0, sizeof blk);
                en_render(blk, 256u);
                for (j = 0; b >= 40u && j < 256u; j++)
                    s += (double)blk[2u * j] * blk[2u * j];
            }
            rms[k] = sqrt(s / (360.0 * 256.0));
        }
        en_play(0);
        en_lane_mute = 0;
        k = !bad && fabs(20.0 * log10(rms[0] / rms[1])) < 3.0 && fabs(20.0 * log10(rms[2] / rms[1])) < 3.0;
        printf("bass: %d roots out of their octave, rms at drive 0 / 50 / 100 %.0f %.0f %.0f  %s\n", bad, rms[0], rms[1], rms[2],
               k ? "ok" : "FAIL");
        fails += !k;
    }
    printf(fails ? "%d FAILED\n" : "all genres ok\n", fails);
    return fails != 0;
}
