/*
 * Copyright (c) the MIMO project contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * PLACEHOLDER MIMO renderer: a plain marshmallow that proves the pipeline,
 * not the approved character. It implements the whole avatar interface
 * (components/muse/muse_pixel.h) so the approved design can replace this
 * file's drawing code without touching anything else:
 *
 *   - every state: boot, idle, listening, thinking (and "working"),
 *     speaking (mouth follows the live audio level), error, off, and the
 *     happy reaction when petted;
 *   - drawn on a 64x64 grid with a small palette, blown up with nearest-
 *     neighbour blocks by muse_pixel_scale(), strip by strip, so the full
 *     screen image never exists in RAM (as the default avatar does).
 *
 * Preview it in the simulator with tools/mimo/preview.py; nothing here is
 * built into firmware until it's copied to components/muse/avatar/muse_pixel.c.
 */
#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H

enum {
    C_BG, C_OUT, C_BODY, C_SHADE, C_HI, C_EYE, C_SHINE, C_CHEEK, C_MOUTH, C_TONGUE,
    C_ACC, C_ACC2, C_SHADOW, C_HEART, C_COUNT,
};

static uint32_t s_background = 0x0b0d1a;
static uint8_t s_fb[W * H];
static uint16_t s_pal[C_COUNT], s_pal_dim[C_COUNT];

static const uint32_t ACCENT[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = 0x9aa7ff,      [MUSE_MODE_IDLE] = 0xffb3c7,  [MUSE_MODE_LISTENING] = 0x7fe0ff,
    [MUSE_MODE_THINKING] = 0xc9a7ff,  [MUSE_MODE_SPEAKING] = 0xffd27f, [MUSE_MODE_ERROR] = 0xff6b6b,
    [MUSE_MODE_OFF] = 0x555566,
};

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return (unsigned)mode < MUSE_MODE_COUNT ? ACCENT[mode] : ACCENT[MUSE_MODE_IDLE];
}

uint32_t muse_pixel_working_accent(void)
{
    return 0xffc078;
}

void muse_pixel_set_background(uint32_t rgb)
{
    s_background = rgb & 0xffffff;
}

static uint16_t to565(uint32_t c, float k)
{
    int r = (int)(((c >> 16) & 0xff) * k), g = (int)(((c >> 8) & 0xff) * k), b = (int)((c & 0xff) * k);
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void palette(uint32_t accent, float bright)
{
    const uint32_t rgb[C_COUNT] = {
        [C_BG] = s_background, [C_OUT] = 0x5b4a63, [C_BODY] = 0xfff7f0, [C_SHADE] = 0xf0dcd6,
        [C_HI] = 0xffffff, [C_EYE] = 0x2a2233, [C_SHINE] = 0xffffff, [C_CHEEK] = 0xffa8c0,
        [C_MOUTH] = 0x7a3b4f, [C_TONGUE] = 0xff8fa8, [C_ACC] = accent, [C_ACC2] = accent,
        [C_SHADOW] = 0x05060c, [C_HEART] = 0xff5f8f,
    };
    for (int i = 0; i < C_COUNT; i++) {
        float k = i == C_BG || i == C_ACC2 ? 1.0f : bright;
        s_pal[i] = to565(rgb[i], i == C_ACC2 ? 0.45f : k);
        s_pal_dim[i] = to565(rgb[i], (i == C_ACC2 ? 0.45f : k) * 0.82f);
    }
}

static void px(int x, int y, uint8_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H) s_fb[y * W + x] = c;
}

static void rect(int x0, int y0, int w, int h, uint8_t c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) px(x, y, c);
}

/* The marshmallow: a soft superellipse, squashed or stretched. */
static bool in_body(float x, float y, float cx, float cy, float rx, float ry)
{
    float dx = fabsf(x - cx) / rx, dy = fabsf(y - cy) / ry;
    return dx * dx * dx * dx + dy * dy * dy * dy <= 1.0f;
}

static void draw_body(float cx, float cy, float rx, float ry)
{
    /* Shadow on the floor. */
    for (int x = (int)(cx - rx + 3); x <= (int)(cx + rx - 3); x++) px(x, (int)(cy + ry + 3), C_SHADOW);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float fx = x + 0.5f, fy = y + 0.5f;
            if (!in_body(fx, fy, cx, cy, rx, ry)) continue;
            bool edge = !in_body(fx - 1, fy, cx, cy, rx, ry) || !in_body(fx + 1, fy, cx, cy, rx, ry) ||
                        !in_body(fx, fy - 1, cx, cy, rx, ry) || !in_body(fx, fy + 1, cx, cy, rx, ry);
            uint8_t c = C_BODY;
            if (edge) c = C_OUT;
            else if (fy > cy + ry * 0.55f || fx > cx + rx * 0.72f) c = C_SHADE;
            else if (fy < cy - ry * 0.6f && fx < cx - rx * 0.2f && fx > cx - rx * 0.7f) c = C_HI;
            px(x, y, c);
        }
    }
}

typedef enum { EYES_OPEN, EYES_CLOSED, EYES_HAPPY, EYES_X, EYES_WIDE } eyes_t;

static void draw_eye(int x, int y, eyes_t e, int look_x, int look_y)
{
    switch (e) {
    case EYES_CLOSED:
        rect(x - 1, y + 1, 3, 1, C_EYE);
        break;
    case EYES_HAPPY:   /* ^ */
        px(x - 1, y + 1, C_EYE); px(x, y, C_EYE); px(x + 1, y + 1, C_EYE);
        break;
    case EYES_X:
        px(x - 1, y - 1, C_EYE); px(x + 1, y + 1, C_EYE); px(x, y, C_EYE);
        px(x + 1, y - 1, C_EYE); px(x - 1, y + 1, C_EYE);
        break;
    case EYES_WIDE:
        rect(x - 1 + look_x, y - 2 + look_y, 3, 4, C_EYE);
        px(x + look_x, y - 1 + look_y, C_SHINE);
        break;
    default:
        rect(x - 1 + look_x, y - 1 + look_y, 2, 3, C_EYE);
        px(x - 1 + look_x, y - 1 + look_y, C_SHINE);
        break;
    }
}

static void draw_mouth(int x, int y, muse_mode_t mode, float level, float happy)
{
    if (mode == MUSE_MODE_SPEAKING) {
        int open = 1 + (int)(level * 4.0f + 0.5f);
        rect(x - 2, y, 4, open, C_MOUTH);
        if (open >= 3) rect(x - 1, y + open - 1, 2, 1, C_TONGUE);
    } else if (mode == MUSE_MODE_ERROR) {
        rect(x - 2, y + 1, 4, 1, C_MOUTH);   /* a worried line */
    } else if (happy > 0.3f) {
        rect(x - 2, y, 4, 2, C_MOUTH);       /* a big grin */
        rect(x - 1, y + 1, 2, 1, C_TONGUE);
        px(x - 3, y - 1, C_MOUTH); px(x + 2, y - 1, C_MOUTH);
    } else {
        px(x - 2, y, C_MOUTH); px(x + 1, y, C_MOUTH);   /* a small smile */
        rect(x - 1, y + 1, 2, 1, C_MOUTH);
    }
}

static void draw_heart(int x, int y)
{
    static const char *const H5[] = { ".X.X.", "XXXXX", "XXXXX", ".XXX.", "..X.." };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (H5[r][c] == 'X') px(x + c, y + r, C_HEART);
}

void muse_pixel_render(const muse_pose_t *p)
{
    muse_mode_t mode = p->mode;
    float t = p->t, mt = p->mode_t, level = p->level, happy = p->happy;
    float bright = mode == MUSE_MODE_OFF ? 0.35f : mode == MUSE_MODE_BOOT ? fminf(1.0f, 0.3f + mt) : 1.0f;
    palette(p->working > 0.5f ? muse_pixel_working_accent() : muse_pixel_accent(mode), bright);
    memset(s_fb, C_BG, sizeof(s_fb));

    /* Breathing, a happy bounce, and a squash when speaking loudly. */
    float breathe = sinf(t * 2.2f) * 0.6f;
    float bounce = happy > 0 ? -fabsf(sinf(t * 9.0f)) * 3.0f * happy : 0;
    float squash = mode == MUSE_MODE_SPEAKING ? level * 1.2f : 0;
    float cx = 32, cy = 37 + bounce, rx = 19 + squash * 0.6f, ry = 17 + breathe - squash;

    /* Listening: a soft ring that follows your voice. */
    if (mode == MUSE_MODE_LISTENING) {
        float r = 26 + level * 4 + sinf(t * 6) * 0.5f;
        for (int a = 0; a < 96; a++) {
            float th = a * 6.2831853f / 96;
            px((int)(cx + cosf(th) * r), (int)(cy + sinf(th) * r * 0.92f), a % 3 ? C_ACC : C_ACC2);
        }
    }
    draw_body(cx, cy, rx, ry);

    /* Face. */
    int ey = (int)(cy - 3), mx = (int)cx, my = (int)(cy + 4);
    bool blink = fmodf(t + 0.37f, 4.3f) < 0.14f;
    eyes_t eyes = EYES_OPEN;
    int look_x = 0, look_y = 0;
    switch (mode) {
    case MUSE_MODE_OFF: case MUSE_MODE_BOOT: eyes = mode == MUSE_MODE_BOOT && mt > 0.8f ? EYES_OPEN : EYES_CLOSED; break;
    case MUSE_MODE_ERROR: eyes = EYES_X; break;
    case MUSE_MODE_LISTENING: eyes = blink ? EYES_CLOSED : EYES_WIDE; break;
    case MUSE_MODE_THINKING: look_x = 1; look_y = -1; eyes = blink ? EYES_CLOSED : EYES_OPEN; break;
    default: eyes = happy > 0.3f ? EYES_HAPPY : blink ? EYES_CLOSED : EYES_OPEN; break;
    }
    draw_eye(mx - 7, ey, eyes, look_x, look_y);
    draw_eye(mx + 6, ey, eyes, look_x, look_y);
    rect(mx - 12, ey + 3, 3, 2, C_CHEEK);
    rect(mx + 10, ey + 3, 3, 2, C_CHEEK);
    draw_mouth(mx, my, mode, level, happy);

    /* Thinking: three dots drift above the head; working: a little spinner. */
    if (mode == MUSE_MODE_THINKING) {
        int top = (int)(cy - ry - 5);
        if (p->working > 0.5f) {
            int k = (int)(t * 8) % 8;
            static const int8_t SX[8] = { 0, 1, 2, 1, 0, -1, -2, -1 }, SY[8] = { -2, -1, 0, 1, 2, 1, 0, -1 };
            for (int i = 0; i < 8; i++) px(mx + 14 + SX[i], top + 2 + SY[i], i == k ? C_ACC : C_ACC2);
        } else {
            for (int i = 0; i < 3; i++) {
                int up = (int)(t * 3) % 3 == i ? 1 : 0;
                rect(mx - 5 + i * 5, top - up, 2, 2, C_ACC);
            }
        }
    }
    /* Error: an alert mark. Happy: a heart floats up. */
    if (mode == MUSE_MODE_ERROR) {
        rect(mx + 16, (int)(cy - ry - 4), 2, 5, C_ACC);
        rect(mx + 16, (int)(cy - ry + 2), 2, 2, C_ACC);
    }
    if (happy > 0.05f) {
        draw_heart(mx + 13, (int)(cy - ry - 6 - fmodf(t * 6, 6)));
    }
}

/* ---- Scaling: grid cell per screen pixel, the cell's last pixel dimmer ---- */

#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px_size)
{
    s_size = px_size < MAP_MAX ? px_size : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        for (int x = x0; x <= x1; x++) {
            uint8_t xm = s_map[x];
            uint8_t c = row[xm & 0x7f];
            dst[x - x0] = (m | xm) & 0x80 ? s_pal_dim[c] : s_pal[c];
        }
    }
}
