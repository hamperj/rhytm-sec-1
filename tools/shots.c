/* SPDX-License-Identifier: GPL-3.0-only */
/* RHYTM-SEC-1's screen on a computer: the module's own draw() through fm1-dock's own text and canvas code
 * (firmware/src/gfx.c, the same fonts), the top line as the host draws it. Plays a scene and writes
 * the screen as PPM at the points SHOT names. Run from an fm1-dock checkout with the module in
 * modules/rhytmsec1 (tools/shots.sh does it all). */
#include <stdint.h>
#include <stdio.h>

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
#define C_BLACK RGB(0, 0, 0)
#define C_WHITE RGB(255, 255, 255)
#define C_ACC RGB(180, 140, 255)
#define C_OFF RGB(90, 90, 90)

static uint16_t fb[240 * 240];                  /* the panel, RGB565 */
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t r, c;
    for (r = 0; r < h && y + r < 240; r++)
        for (c = 0; c < w && x + c < 240; c++) {
            uint16_t p = px[r * w + c];
            fb[(y + r) * 240 + x + c] = (uint16_t)(p >> 8 | p << 8);   /* (the panel's byte order) */
        }
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t r, k;
    for (r = y; r < y + h && r < 240; r++)
        for (k = x; k < x + w && k < 240; k++)
            fb[r * 240 + k] = c;
}
#define section(x) used
#include "gfx.c"
#undef section

#include "hb_module.h"

static uint32_t now_ms;
static uint64_t now_ticks;
static uint32_t rng_s = 0x1234567u;
static uint32_t h_ms(void) { return now_ms; }
static uint32_t h_ticks(void) { return (uint32_t)now_ticks; }
static void h_log(const char *s) { (void)s; }
static void h_log_u32(const char *s, uint32_t v) { (void)s; (void)v; }
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w > 0 && h > 0) lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c);
}
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center)
{
    if (x >= 0 && y >= 0 && w > 0) draw_text_box((uint32_t)x, (uint32_t)y, (uint32_t)w, big ? &FONT_L : &FONT_S, s, fg, center ? 1 : 0);
}
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg) { cv_begin((uint32_t)w, (uint32_t)h, bg); }
static void h_cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { cv_rect(x, y, w, h, c); }
static void h_cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { cv_line(x0, y0, x1, y1, c); }
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { cv_pset(x, y, c); }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { cv_text(x, y, big ? &FONT_L : &FONT_S, s, fg); }
static void h_cv_blit(int32_t x, int32_t y) { cv_blit((uint32_t)x, (uint32_t)y); }
static void h_led(uint32_t id, uint32_t on) { (void)id; (void)on; }
static uint32_t h_rand(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }
static int h_load(void *p, uint32_t n) { (void)p; (void)n; return -1; }
static void h_save(const void *p, uint32_t n) { (void)p; (void)n; }
static int32_t h_text_w(uint32_t big, const char *s) { return text_w(big ? &FONT_L : &FONT_S, s); }
static int32_t *h_stem(uint32_t idx) { (void)idx; return 0; }

static const hb_host_t host = {
    .abi = HB_ABI, .size = sizeof(hb_host_t), .ms = h_ms, .ticks = h_ticks, .log = h_log, .log_u32 = h_log_u32,
    .fill = h_fill, .text = h_text, .cv_begin = h_cv_begin, .cv_rect = h_cv_rect, .cv_line = h_cv_line,
    .cv_pixel = h_cv_pixel, .cv_text = h_cv_text, .cv_blit = h_cv_blit, .led = h_led, .rand = h_rand,
    .load = h_load, .save = h_save, .text_w = h_text_w, .stem = h_stem,
};

#define HB_DESC const hb_module_t rs1
#include "rhytmsec1.c"

static void top(void)                           /* dock.c's draw_top, as a solo app shows it */
{
    cv_begin(240, 20, C_BLACK);
    cv_text(4, 2, &FONT_S, rs1.name, HB_RGB(180, 140, 255));   /* (the host's lilac) */
    cv_text(236 - text_w(&FONT_S, "OCT +0") - 12 - text_w(&FONT_S, "1CPU 31%"), 2, &FONT_S, "1CPU 31%", C_OFF);
    cv_text(236 - text_w(&FONT_S, "OCT +0"), 2, &FONT_S, "OCT +0", C_OFF);
    cv_rect(0, 19, 240, 1, C_OFF);
    cv_blit(0, 0);
}

static void run_ms(uint32_t ms)                 /* play: the audio blocks, the screen ~30 times a second */
{
    static int32_t out[2 * 256];
    uint32_t end = now_ms + ms, next_draw = now_ms;
    static double t;
    while (now_ms < end) {
        memset(out, 0, sizeof out);
        rs1.render(out, 256);
        t += 256.0 * 1000.0 / 44100.0;
        now_ms = (uint32_t)t;
        now_ticks += 256u * 24000000u / 44100u;
        if (now_ms >= next_draw) {
            rs1.draw(0);
            next_draw = now_ms + 33u;
        }
    }
}
static void ev(uint32_t e, int32_t arg) { rs1.ui(e, arg); run_ms(40); }
static void btn(uint32_t id) { ev(HB_EV_BTN_DOWN | id, 0); ev(HB_EV_BTN_UP | id, 0); }
static void shot(const char *dir, const char *name)
{
    char p[512];
    FILE *f;
    uint32_t i;
    snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    if (!(f = fopen(p, "wb"))) { perror(p); return; }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        uint8_t px[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", p);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (rs1.init(&host)) return 1;
    top();
    rs1.draw(1);
    shot(dir, "01-stopped");
    btn(HB_BTN_PLAY);
    run_ms(7000);
    shot(dir, "02-house-playing");             /* (on the HOME page, where it starts) */
    btn(HB_BTN_SEL);                            /* the knob pages */
    run_ms(1500);
    shot(dir, "09-drums-page");
    btn(HB_BTN_SEL);
    run_ms(1500);
    shot(dir, "03-page-2");
    btn(HB_BTN_SEL);
    run_ms(1500);
    shot(dir, "04-page-3");
    btn(HB_BTN_SEL);
    ev(HB_EV_KEY_DOWN | 8u, 0);                 /* the SOUND page, the snare picked */
    ev(HB_EV_KEY_UP | 8u, 0);
    run_ms(1500);
    shot(dir, "05-sound-page");
    btn(HB_BTN_SEL);
    ev(HB_EV_ENC | HB_ENC_SELECT, 6);           /* another style */
    run_ms(6000);
    shot(dir, "06-style-changed");
    btn(HB_BTN_SAVE);                           /* a break on the next bar */
    run_ms(4500);
    shot(dir, "07-break");
    ev(HB_EV_ENC | HB_ENC_ALGORITHM, -12);      /* the DJ filter, low-pass */
    btn(HB_BTN_FX);                             /* and a fill */
    run_ms(1500);
    shot(dir, "08-filter-fill");
    return 0;
}
