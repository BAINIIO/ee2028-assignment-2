#include "game_display.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Original 1-bit artwork, drawn at the actual 128 x 64 panel resolution. */
typedef struct { uint8_t *pixels; } Canvas;

static const uint8_t letters[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x3a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x41,0x41,0x7f,0x41,0x41}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}
};
static const uint8_t digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x62,0x51,0x49,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}
};
/* The tiny HUD font is deliberately 3 x 5; game instructions use 5 x 7. */
static const uint8_t tiny_letters[26][3] = {
    {0x1e,0x05,0x1e}, {0x1f,0x15,0x0a}, {0x0e,0x11,0x11},
    {0x1f,0x11,0x0e}, {0x1f,0x15,0x11}, {0x1f,0x05,0x01},
    {0x0e,0x11,0x1d}, {0x1f,0x04,0x1f}, {0x11,0x1f,0x11},
    {0x08,0x10,0x0f}, {0x1f,0x04,0x1b}, {0x1f,0x10,0x10},
    {0x1f,0x06,0x1f}, {0x1f,0x01,0x1e}, {0x0e,0x11,0x0e},
    {0x1f,0x05,0x02}, {0x0e,0x19,0x1e}, {0x1f,0x0d,0x16},
    {0x12,0x15,0x09}, {0x01,0x1f,0x01}, {0x0f,0x10,0x0f},
    {0x07,0x18,0x07}, {0x1f,0x0c,0x1f}, {0x1b,0x04,0x1b},
    {0x03,0x1c,0x03}, {0x19,0x15,0x13}
};
static const uint8_t tiny_digits[10][3] = {
    {0x1f,0x11,0x1f}, {0x12,0x1f,0x10}, {0x1d,0x15,0x17},
    {0x15,0x15,0x1f}, {0x07,0x04,0x1f}, {0x17,0x15,0x1d},
    {0x1f,0x15,0x1d}, {0x01,0x1d,0x03}, {0x1f,0x15,0x1f},
    {0x17,0x15,0x1f}
};

static void pixel(Canvas *c, int x, int y, bool on)
{
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    size_t offset = (size_t)x + (size_t)(y / 8) * 128u;
    uint8_t mask = (uint8_t)(1u << (unsigned)(y % 8));
    if (on) c->pixels[offset] |= mask;
    else c->pixels[offset] &= (uint8_t)~mask;
}

static void rectangle(Canvas *c, int x, int y, int w, int h, bool filled, bool on)
{
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            if (filled || row == 0 || row == h - 1 || col == 0 || col == w - 1)
                pixel(c, x + col, y + row, on);
}

static void line(Canvas *c, int x0, int y0, int x1, int y1, bool on)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        pixel(c, x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

static void oval(Canvas *c, int x, int y, int rx, int ry, bool on)
{
    for (int dy = -ry; dy <= ry; ++dy)
        for (int dx = -rx; dx <= rx; ++dx)
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                pixel(c, x + dx, y + dy, on);
}

static void glyph(Canvas *c, int x, int y, unsigned char value, bool tiny)
{
    const uint8_t *shape = NULL;
    uint8_t punctuation[5] = {0};
    int w = tiny ? 3 : 5, h = tiny ? 5 : 7;
    if (value >= 'a' && value <= 'z') value = (unsigned char)(value - 'a' + 'A');
    if (value >= 'A' && value <= 'Z')
        shape = tiny ? tiny_letters[value - 'A'] : letters[value - 'A'];
    else if (value >= '0' && value <= '9')
        shape = tiny ? tiny_digits[value - '0'] : digits[value - '0'];
    else {
        uint8_t middle = tiny ? 0x04u : 0x08u;
        if (value == '-') memset(punctuation, middle, (size_t)w);
        else if (value == '+') {
            memset(punctuation, middle, (size_t)w);
            punctuation[w / 2] = tiny ? 0x0eu : 0x3eu;
        } else if (value == '/') {
            if (tiny) { punctuation[0]=0x10; punctuation[1]=0x0e; punctuation[2]=0x01; }
            else { punctuation[0]=0x40; punctuation[1]=0x30; punctuation[2]=0x08;
                punctuation[3]=0x06; punctuation[4]=0x01; }
        } else if (value == ':') punctuation[w / 2] = tiny ? 0x0au : 0x24u;
        else if (value == '!') punctuation[w / 2] = tiny ? 0x17u : 0x5fu;
        else if (value == '>') {
            punctuation[0] = tiny ? 0x11u : 0x41u;
            punctuation[1] = tiny ? 0x0au : 0x22u;
            punctuation[2] = tiny ? 0x04u : 0x14u;
            if (!tiny) punctuation[3] = 0x08u;
        } else if (value == '=') memset(punctuation, tiny ? 0x0au : 0x14u, (size_t)w);
        shape = punctuation;
    }
    for (int col = 0; col < w; ++col)
        for (int row = 0; row < h; ++row)
            if (shape[col] & (1u << (unsigned)row)) pixel(c, x + col, y + row, true);
}

static void text(Canvas *c, int x, int y, const char *value, bool tiny)
{
    if (!value) return;
    int advance = tiny ? 4 : 6;
    /* Every string here is bounded internally; also stop once off screen. */
    while (*value && x < 128) {
        glyph(c, x, y, (unsigned char)*value++, tiny);
        x += advance;
    }
}

static void centered(Canvas *c, int y, const char *value, bool tiny)
{
    size_t length = strlen(value);
    int width = (int)length * (tiny ? 4 : 6) - 1;
    text(c, (128 - width) / 2, y, value, tiny);
}

static void heart(Canvas *c, int x, int y, bool full)
{
    static const uint8_t shape[] = {0x06,0x0f,0x1f,0x3e,0x1f,0x0f,0x06};
    for (int col = 0; col < 7; ++col) {
        unsigned inside = (unsigned)(shape[col] << 1) & (unsigned)(shape[col] >> 1);
        inside &= col ? shape[col - 1] : 0u;
        inside &= col < 6 ? shape[col + 1] : 0u;
        uint8_t visible = full ? shape[col] : (uint8_t)(shape[col] & ~inside);
        for (int row = 0; row < 6; ++row)
            if (visible & (1u << (unsigned)row)) pixel(c, x + col, y + row, true);
    }
}

static void hud(Canvas *c, const ExternalDisplayFrame *frame)
{
    char number[16];
    text(c, 2, 1, "SCORE", true);
    (void)snprintf(number, sizeof number, "%d", (int)frame->score);
    text(c, 26, 1, number, true);
    for (unsigned i = 0; i < 3u; ++i)
        heart(c, 87 + (int)i * 10, 1, frame->lives > i);
    if (frame->sensor_warning) text(c, 121, 1, "!", false);
    line(c, 0, 8, 127, 8, true);
}

static void cloud(Canvas *c, int x, int y)
{
    line(c, x, y + 3, x + 12, y + 3, true);
    line(c, x + 1, y + 1, x + 4, y + 1, true);
    line(c, x + 5, y, x + 8, y, true);
    line(c, x + 9, y + 2, x + 11, y + 2, true);
}

static void lawn(Canvas *c, uint32_t now_ms)
{
    /* Stationary ground anchors the walking animation; sparse grass reads
     * clearly on a tiny panel without filling the sprites' silhouettes. */
    line(c, 0, 49, 127, 49, true);
    for (int x = 3; x < 128; x += 13) {
        pixel(c, x, 48, true);
        pixel(c, x + 2, 47, true);
        pixel(c, x + 3, 48, true);
    }
    cloud(c, 49 - (int)((now_ms / 1200u) % 7u), 21);
    cloud(c, 111 - (int)((now_ms / 1600u) % 5u), 19);
}

static void plant(Canvas *c, int x, int ground_y, uint32_t now_ms,
                  bool shooting, bool hurt)
{
    int bob = (now_ms / 375u) % 2u ? 1 : 0;
    int head_y = ground_y - 18 + bob;
    int recoil = shooting ? -2 : 0;
    line(c, x + 10, ground_y - 1, x + 10, head_y + 5, true);
    line(c, x + 11, ground_y - 1, x + 11, head_y + 4, true);
    oval(c, x + 5, ground_y - 3, 5, 2, true);
    oval(c, x + 16, ground_y - 3, 5, 2, true);
    line(c, x + 2, ground_y - 4, x + 8, ground_y - 2, false);
    line(c, x + 13, ground_y - 2, x + 19, ground_y - 4, false);
    oval(c, x + 10 + recoil, head_y, 8, 7, true);
    rectangle(c, x + 14 + recoil, head_y - 3, 12, 7, true, true);
    rectangle(c, x + 23 + recoil, head_y - 2, 2, 5, true, false);
    if (hurt) {
        line(c, x + 8 + recoil, head_y - 4, x + 11 + recoil, head_y - 1, false);
        line(c, x + 11 + recoil, head_y - 4, x + 8 + recoil, head_y - 1, false);
    } else {
        rectangle(c, x + 10 + recoil, head_y - 4, 2, 3, true, false);
        pixel(c, x + 10 + recoil, head_y - 4, true);
    }
    /* Small crest and separate leaves make the plant recognisable at 1:1. */
    line(c, x + 6 + recoil, head_y - 7, x + 3 + recoil, head_y - 10, true);
    line(c, x + 7 + recoil, head_y - 7, x + 8 + recoil, head_y - 10, true);
}

static void zombie(Canvas *c, int centre_x, uint32_t now_ms, bool hit)
{
    int step = (now_ms / 220u) % 2u ? 1 : 0;
    int x = centre_x - 8, y = 21 + step;
    if (hit && ((now_ms / 90u) % 2u)) return;
    rectangle(c, x + 3, y, 12, 10, true, true);
    rectangle(c, x + 1, y + 3, 2, 5, true, true);
    pixel(c, x + 5, y - 1, true);
    pixel(c, x + 10, y - 1, true);
    rectangle(c, x + 5, y + 3, 3, 2, true, false);
    rectangle(c, x + 11, y + 3, 2, 2, true, false);
    line(c, x + 7, y + 7, x + 13, y + 7, false);
    pixel(c, x + 9, y + 8, false);
    pixel(c, x + 12, y + 8, false);
    rectangle(c, x + 5, y + 11, 9, 9, false, true);
    line(c, x + 6, y + 11, x + 11, y + 18, true);
    line(c, x + 5, y + 12, x, y + 14, true);
    line(c, x, y + 14, x - 4, y + 12, true);
    line(c, x + 13, y + 12, x + 16, y + 17, true);
    line(c, x + 16, y + 17, x + 12, y + 18, true);
    pixel(c, x + 6, y + 20, true);
    pixel(c, x + 11, y + 20, true);
    line(c, x + 7, y + 20, x + 5 - step, 47, true);
    line(c, x + 11, y + 20, x + 13 + step, 47, true);
    line(c, x + 2 - step, 48, x + 6 - step, 48, true);
    line(c, x + 12 + step, 48, x + 16 + step, 48, true);
}

static void human(Canvas *c, int centre_x, uint32_t now_ms, bool hit)
{
    int step = (now_ms / 200u) % 2u ? 1 : 0;
    int x = centre_x - 7, y = 23 + step;
    if (hit && ((now_ms / 90u) % 2u)) return;
    oval(c, x + 7, y + 4, 5, 5, true);
    line(c, x + 2, y + 1, x + 12, y + 1, false);
    line(c, x + 1, y, x + 13, y, true); /* Friendly wide-brim gardener hat. */
    rectangle(c, x + 5, y - 3, 6, 3, true, true);
    pixel(c, x + 5, y + 3, false);
    pixel(c, x + 9, y + 3, false);
    line(c, x + 5, y + 6, x + 9, y + 6, false);
    rectangle(c, x + 4, y + 10, 7, 9, true, true);
    rectangle(c, x + 6, y + 11, 3, 4, true, false);
    line(c, x + 4, y + 11, x, y + 14, true);
    line(c, x + 10, y + 11, x + 14, y + 7 - step * 2, true);
    line(c, x + 5, y + 19, x + 3 - step, 47, true);
    line(c, x + 9, y + 19, x + 12 + step, 47, true);
    line(c, x + 1 - step, 48, x + 4 - step, 48, true);
    line(c, x + 11 + step, 48, x + 14 + step, 48, true);
}

static int target_x(uint16_t age_ms, uint16_t period_ms)
{
    uint32_t period = period_ms ? period_ms : 4000u;
    uint32_t age = age_ms > period ? period : age_ms;
    return 111 - (int)(age * 66u / period);
}

static void crosshair(Canvas *c, int x, int y)
{
    line(c, x - 11, y - 6, x - 11, y - 2, true);
    line(c, x - 11, y - 6, x - 7, y - 6, true);
    line(c, x + 11, y - 6, x + 11, y - 2, true);
    line(c, x + 11, y - 6, x + 7, y - 6, true);
    line(c, x - 11, y + 6, x - 11, y + 2, true);
    line(c, x - 11, y + 6, x - 7, y + 6, true);
    line(c, x + 11, y + 6, x + 11, y + 2, true);
    line(c, x + 11, y + 6, x + 7, y + 6, true);
}

static void burst(Canvas *c, int x, int y, unsigned phase)
{
    int radius = 3 + (int)(phase % 5u);
    line(c, x - radius, y, x - 2, y, true);
    line(c, x + 2, y, x + radius, y, true);
    line(c, x, y - radius, x, y - 2, true);
    line(c, x, y + 2, x, y + radius, true);
    line(c, x - radius + 1, y - radius + 1, x - 2, y - 2, true);
    line(c, x + 2, y + 2, x + radius - 1, y + radius - 1, true);
    line(c, x - radius + 1, y + radius - 1, x - 2, y + 2, true);
    line(c, x + 2, y - 2, x + radius - 1, y - radius + 1, true);
}

static void energy_bar(Canvas *c, const ExternalDisplayFrame *frame)
{
    char number[8];
    unsigned energy = frame->energy > 10u ? 10u : frame->energy;
    text(c, 2, 56, "E", true);
    rectangle(c, 8, 54, 42, 9, false, true);
    for (unsigned i = 0; i < energy; ++i)
        rectangle(c, 10 + (int)i * 4, 56, 3, 5, true, true);
    (void)snprintf(number, sizeof number, "%u", energy);
    text(c, 53, 56, number, true);
    text(c, 72, 53, "PRESS +3", true);
    text(c, 72, 59, "AUTO >=5", true);
    if (frame->effect == EXTERNAL_EFFECT_CHARGE && frame->effect_age_ms < 650u) {
        rectangle(c, 52, 52, 17, 12, true, false);
        text(c, 53, 55, "+3", false);
    }
}

static void battle(Canvas *c, const ExternalDisplayFrame *frame)
{
    char number[20];
    bool impact = frame->effect == EXTERNAL_EFFECT_HIT ||
                  frame->effect == EXTERNAL_EFFECT_HUMAN_HIT;
    bool shot = impact || frame->effect == EXTERNAL_EFFECT_SHOT;
    bool recent = frame->effect_age_ms < 550u;
    int x = target_x(frame->target_age_ms, frame->target_period_ms);
    int old_x = target_x(frame->effect_target_age_ms, frame->target_period_ms);
    hud(c, frame);
    unsigned goal = frame->goal_hits ? frame->goal_hits : 6u;
    if (goal > 99u) goal = 99u;
    unsigned hits = frame->zombie_hits > 99u ? 99u : frame->zombie_hits;
    (void)snprintf(number, sizeof number, "HITS %u/%u", hits, goal);
    text(c, 2, 11, number, true);
    const char *action = frame->target == EXTERNAL_COLOUR_RED ? "FIRE!" :
                         frame->target == EXTERNAL_COLOUR_GREEN ? "HOLD FIRE" : "GET READY";
    const char *label = frame->target == EXTERNAL_COLOUR_RED ? "ZOMBIE" :
                        frame->target == EXTERNAL_COLOUR_GREEN ? "HUMAN" : "";
    text(c, 49, 11, label, true);
    text(c, 128 - (int)strlen(action) * 4, 11, action, true);
    lawn(c, frame->now_ms);
    bool recoil = shot && recent && frame->effect_age_ms < 170u;
    plant(c, 8, 48, frame->now_ms, recoil,
          recent && (frame->effect == EXTERNAL_EFFECT_MISS ||
                     frame->effect == EXTERNAL_EFFECT_HUMAN_HIT));
    /* The next target becomes live immediately in the game. Keep it visible
     * while briefly showing the old target at its captured impact position. */
    if (recent && frame->effect_age_ms < 260u) {
        if (frame->effect_target == EXTERNAL_COLOUR_RED &&
            (impact || frame->effect == EXTERNAL_EFFECT_MISS))
            zombie(c, old_x, frame->now_ms, impact);
        else if (frame->effect_target == EXTERNAL_COLOUR_GREEN &&
                 (impact || frame->effect == EXTERNAL_EFFECT_HUMAN_SAVED)) {
            human(c, old_x, frame->now_ms, impact);
        }
    }
    if (frame->target == EXTERNAL_COLOUR_RED) zombie(c, x, frame->now_ms, false);
    else if (frame->target == EXTERNAL_COLOUR_GREEN) human(c, x, frame->now_ms, false);
    if (frame->aim_locked && frame->target != EXTERNAL_COLOUR_NONE)
        crosshair(c, x, 32);
    if (frame->aim_locked) text(c, 2, 20, "IR", true);
    if (shot && recent) {
        int end = frame->effect_target == EXTERNAL_COLOUR_NONE ? 124 : old_x;
        if (frame->effect_age_ms < 260u) {
            int progress = (int)frame->effect_age_ms;
            int pea_x = 33 + (end - 33) * progress / 260;
            oval(c, pea_x, 32, 2, 2, true);
            line(c, pea_x - 5, 32, pea_x - 3, 32, true);
        } else if (impact) burst(c, old_x, 32, frame->effect_age_ms / 65u);
    }
    if (recent && frame->effect != EXTERNAL_EFFECT_NONE &&
        frame->effect != EXTERNAL_EFFECT_CHARGE && frame->effect_age_ms >= 260u) {
        const char *feedback = NULL;
        switch (frame->effect) {
        case EXTERNAL_EFFECT_HIT: feedback = "GOOD HIT!"; break;
        case EXTERNAL_EFFECT_HUMAN_HIT: feedback = "HUMAN! -5"; break;
        case EXTERNAL_EFFECT_HUMAN_SAVED: feedback = "SAFE +5"; break;
        case EXTERNAL_EFFECT_MISS: feedback = "MISSED -5"; break;
        case EXTERNAL_EFFECT_RESCUED: feedback = "RESCUED!"; break;
        default: break;
        }
        if (feedback) {
            int width = (int)strlen(feedback) * 4 + 3;
            rectangle(c, 64 - width / 2, 18, width, 9, true, false);
            text(c, 66 - width / 2, 20, feedback, true);
        }
    }
    energy_bar(c, frame);
}

static void standby(Canvas *c, const ExternalDisplayFrame *frame)
{
    centered(c, 3, "GARDEN GUARD", false);
    centered(c, 14, "PROTECT YOUR PATCH", true);
    lawn(c, frame->now_ms);
    plant(c, 10, 48, frame->now_ms, false, false);
    zombie(c, 100, frame->now_ms, false);
    oval(c, 38 + (int)((frame->now_ms / 110u) % 49u), 32, 2, 2, true);
    if ((frame->now_ms / 600u) % 3u != 2u) centered(c, 55, "DOUBLE TO START", false);
    else centered(c, 56, "BLUE USER BUTTON", true);
    if (frame->sensor_warning) text(c, 122, 1, "!", false);
    if (frame->aim_locked) text(c, 2, 22, "IR", true);
}

static void attacked(Canvas *c, const ExternalDisplayFrame *frame)
{
    char number[4];
    hud(c, frame);
    bool blink = (frame->now_ms / 250u) % 2u != 0u;
    rectangle(c, 1, 11, 126, 52, false, true);
    if (blink) rectangle(c, 3, 13, 122, 48, false, true);
    centered(c, 15, "UNDER ATTACK!", false);
    unsigned seconds = frame->rescue_seconds > 10u ? 10u : frame->rescue_seconds;
    (void)snprintf(number, sizeof number, "%u", seconds);
    /* Large seven-segment-style digit uses the same pixel glyph at 2x size. */
    size_t count = strlen(number);
    int start = (128 - (int)count * 12 + 2) / 2;
    for (size_t i = 0; i < count; ++i)
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (digits[(unsigned char)number[i] - '0'][col] & (1u << (unsigned)row))
                    rectangle(c, start + (int)i * 12 + col * 2, 27 + row * 2,
                              2, 2, true, true);
    centered(c, 45, "DOUBLE TO RESCUE", false);
    centered(c, 55, "BEFORE TIME RUNS OUT", true);
}

static void result(Canvas *c, const ExternalDisplayFrame *frame, bool won)
{
    char number[24];
    hud(c, frame);
    centered(c, 12, won ? "LAWN SAVED!" : "LAWN OVERRUN", false);
    lawn(c, frame->now_ms);
    plant(c, 48, 48, frame->now_ms, false, !won);
    if (won) {
        for (unsigned i = 0; i < 8u; ++i) {
            int x = 6 + (int)((i * 37u + frame->now_ms / 75u) % 116u);
            int y = 23 + (int)((i * 11u + frame->now_ms / 110u) % 23u);
            if (x > 42 && x < 82) continue;
            pixel(c, x, y, true);
            pixel(c, x + 1, y, true);
            if (i % 2u) pixel(c, x, y + 1, true);
        }
        burst(c, 24, 32, frame->now_ms / 150u);
        burst(c, 105, 34, frame->now_ms / 130u);
    } else {
        zombie(c, 20, frame->now_ms, false);
        zombie(c, 105, frame->now_ms + 200u, false);
    }
    (void)snprintf(number, sizeof number, "SCORE %d", (int)frame->score);
    rectangle(c, 38, 42, 51, 7, true, false);
    centered(c, 43, number, true);
    centered(c, 53, "2X TO MENU", true);
    centered(c, 59, "THEN 2X TO PLAY", true);
    if (frame->effect == EXTERNAL_EFFECT_CHARGE && frame->effect_age_ms < 650u)
        text(c, 4, 54, "+3", false);
}

static void terminated(Canvas *c, const ExternalDisplayFrame *frame)
{
    (void)frame;
    rectangle(c, 0, 0, 128, 64, false, true);
    centered(c, 8, "RESCUE FAILED", false);
    centered(c, 22, "GAME OVER", false);
    centered(c, 38, "SYSTEM TERMINATED", true);
    centered(c, 50, "PRESS BLACK RESET", true);
}

void GameDisplay_Render(uint8_t buffer[1024], const ExternalDisplayFrame *frame)
{
    if (!buffer) return;
    memset(buffer, 0, 1024);
    if (!frame) return;
    Canvas c = {buffer};
    if (frame->scene == EXTERNAL_SCENE_TERMINATED) {
        terminated(&c, frame);
        return;
    }
    if (frame->attacked || frame->scene == EXTERNAL_SCENE_ATTACKED) {
        attacked(&c, frame);
        return;
    }
    switch (frame->scene) {
    case EXTERNAL_SCENE_STANDBY: standby(&c, frame); break;
    case EXTERNAL_SCENE_BATTLE: battle(&c, frame); break;
    case EXTERNAL_SCENE_WON: result(&c, frame, true); break;
    case EXTERNAL_SCENE_LOST: result(&c, frame, false); break;
    case EXTERNAL_SCENE_TERMINATED: terminated(&c, frame); break;
    default: standby(&c, frame); break;
    }
}
