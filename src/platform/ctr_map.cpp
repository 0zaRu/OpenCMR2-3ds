// The stage map for the Nintendo 3DS's bottom screen (port/stagemap.h).
//
// The CPU draws it into an image that the renderer (render/gfx_c3d.cpp)
// shows on the bottom screen, under the HUD it moves there: the route once
// per stage into a background, then a few times a second the cars over it,
// and the frame rate of the top screen.

#include "platform/ctr.h"
#include "platform/platform.h"
#include "port/stagemap.h"

#include <3ds.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

const int s_width = 320, s_height = 240;
// Updates of the cars: one every this many frames.
const int s_interval = 4;

// 0xAARRGGBB, as the game's textures.
const uint32_t s_backColor = 0xff1c2026, s_routeColor = 0xffc8c8c8, s_splitColor = 0xffffa500,
               s_startColor = 0xff00e000, s_finishColor = 0xffff2020, s_playerColor = 0xffffff00,
               s_otherColor = 0xff58a0ff, s_textColor = 0xffffffff;

int s_enabled = -1;             // [3ds] map, read on the first update
bool s_active;                  // a route and cars this frame
const void *s_route;            // the route the background was drawn for
int s_routeCount;
std::vector<uint32_t> s_background;     // landscape rows
std::vector<uint32_t> s_image;          // background, cars and the frame rate
bool s_changed;
// Route to screen: screen = (world - origin) * scale + offset.
float s_originX, s_originZ, s_scale, s_offsetX, s_offsetY;
int s_frame;
float s_fps;

void Plot(std::vector<uint32_t> &image, int x, int y, uint32_t color)
{
    if (x >= 0 && y >= 0 && x < s_width && y < s_height)
        image[y * s_width + x] = color;
}

void Disc(int cx, int cy, int r, uint32_t color)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                Plot(s_background, cx + x, cy + y, color);
}

// A line two pixels thick.
void Line(float x0, float y0, float x1, float y1, uint32_t color)
{
    int steps = (int)std::max(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        int x = (int)lroundf(x0 + (x1 - x0) * t), y = (int)lroundf(y0 + (y1 - y0) * t);
        Plot(s_background, x, y, color);
        Plot(s_background, x + 1, y, color);
        Plot(s_background, x, y + 1, color);
        Plot(s_background, x + 1, y + 1, color);
    }
}

void ToScreen(int x, int z, float &sx, float &sy)
{
    // North (+z) up.
    sx = ((x >> 16) - s_originX) * s_scale + s_offsetX;
    sy = s_height - 1 - (((z >> 16) - s_originZ) * s_scale + s_offsetY);
}

void DrawBackground(const int *position, const unsigned char *flags, int count, int stride)
{
    s_background.assign((size_t)s_width * s_height, s_backColor);
    auto node = [&](int i) { return (const int *)((const char *)position + (size_t)i * stride); };
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < count; i++) {
        float x = (float)(node(i)[0] >> 16), z = (float)(node(i)[2] >> 16);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }
    const float margin = 14;
    float spanX = std::max(1.0f, maxX - minX), spanZ = std::max(1.0f, maxZ - minZ);
    s_scale = std::min((s_width - 2 * margin) / spanX, (s_height - 2 * margin) / spanZ);
    s_originX = minX;
    s_originZ = minZ;
    s_offsetX = (s_width - spanX * s_scale) / 2;
    s_offsetY = (s_height - spanZ * s_scale) / 2;

    float px = 0, py = 0;
    for (int i = 0; i < count; i++) {
        float sx, sy;
        ToScreen(node(i)[0], node(i)[2], sx, sy);
        if (i > 0)
            Line(px, py, sx, sy, s_routeColor);
        px = sx;
        py = sy;
    }
    for (int i = 1; i + 1 < count; i++)
        if (*((const unsigned char *)flags + (size_t)i * stride) & 1) {
            float sx, sy;
            ToScreen(node(i)[0], node(i)[2], sx, sy);
            Disc((int)sx, (int)sy, 3, s_splitColor);
        }
    float sx, sy;
    ToScreen(node(count - 1)[0], node(count - 1)[2], sx, sy);
    Disc((int)sx, (int)sy, 4, s_finishColor);
    ToScreen(node(0)[0], node(0)[2], sx, sy);
    Disc((int)sx, (int)sy, 4, s_startColor);
}

// Digits, 'f', 'p', 's' and space in a 3x5 font, rows top down, bit 2 left.
const uint8_t *Glyph(char c)
{
    static const uint8_t digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
        { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    };
    static const uint8_t f[5] = { 3, 4, 6, 4, 4 }, p[5] = { 6, 5, 6, 4, 4 }, s[5] = { 3, 4, 2, 1, 6 },
                         dot[5] = { 0, 0, 0, 0, 2 }, blank[5] = {};
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    return c == 'f' ? f : c == 'p' ? p : c == 's' ? s : c == '.' ? dot : blank;
}

// Text at (x, y), each font pixel a 2x2 square, on a dark box.
void Text(int x, int y, const char *text)
{
    int length = (int)strlen(text);
    for (int py = y - 2; py < y + 12; py++)
        for (int px = x - 2; px < x + length * 8; px++)
            Plot(s_image, px, py, 0xff000000);
    for (int i = 0; text[i]; i++) {
        const uint8_t *g = Glyph(text[i]);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col))
                    for (int d = 0; d < 4; d++)
                        Plot(s_image, x + i * 8 + col * 2 + (d & 1), y + row * 2 + (d >> 1), s_textColor);
    }
}

void DrawImage(const int *cars, int count, int forwardX, int forwardZ)
{
    s_image = s_background;
    // The others first, so that the player's arrow stays on top.
    for (int c = count - 1; c >= 0; c--) {
        float sx, sy;
        ToScreen(cars[c * 2], cars[c * 2 + 1], sx, sy);
        int cx = (int)lroundf(sx), cy = (int)lroundf(sy);
        if (c > 0) {
            for (int y = -2; y <= 2; y++)
                for (int x = -2; x <= 2; x++)
                    if (x * x + y * y <= 5)
                        Plot(s_image, cx + x, cy + y, s_otherColor);
            continue;
        }
        // The player: an arrow along the forward axis (screen y is -z).
        float fx = (float)forwardX, fy = -(float)forwardZ;
        float length = sqrtf(fx * fx + fy * fy);
        if (length <= 0) {
            fx = 0;
            fy = -1;
        } else {
            fx /= length;
            fy /= length;
        }
        const float r = 7;
        for (int y = -8; y <= 8; y++)
            for (int x = -8; x <= 8; x++) {
                // Along and across the heading.
                float a = x * fx + y * fy, b = -x * fy + y * fx;
                if (a <= r && a >= -r * 0.6f && fabsf(b) <= (r - a) * 0.45f)
                    Plot(s_image, cx + x, cy + y, s_playerColor);
            }
    }
    // The top screen's frame rate, bottom right.
    char text[16];
    snprintf(text, sizeof(text), "%d fps", (int)lroundf(s_fps));
    Text(s_width - 6 - (int)strlen(text) * 8, s_height - 14, text);
    s_changed = true;
}

} // namespace

bool Ctr_MapWanted(void)
{
    if (s_enabled < 0)
        s_enabled = Platform_GetSettingInt("3ds.map", 1) != 0;
    return s_enabled && Ctr_IsDriving();
}

const uint32_t *Ctr_MapImage(bool *changed)
{
    *changed = s_changed;
    s_changed = false;
    return s_active && !s_image.empty() ? s_image.data() : NULL;
}

void Ctr_MapSetFps(float fps)
{
    s_fps = fps;
}

extern "C" void StageMap_Update(const int *position, const unsigned char *flags, int routeCount, int stride,
                                const int *cars, int carCount, int forwardX, int forwardZ)
{
    s_active = Ctr_MapWanted() && position != NULL && routeCount >= 2 && carCount >= 1;
    if (!s_active)
        return;
    if (position != s_route || routeCount != s_routeCount) {
        DrawBackground(position, flags, routeCount, stride);
        s_route = position;
        s_routeCount = routeCount;
        s_frame = 0;
    }
    if (s_frame++ % s_interval == 0)
        DrawImage(cars, carCount, forwardX, forwardZ);
}
