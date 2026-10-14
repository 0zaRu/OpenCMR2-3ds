// The stage map on the Nintendo 3DS's bottom screen (port/stagemap.h).
//
// The CPU draws it into the screen's framebuffer, without the GPU: the route
// once per stage into a background image, then a few times a second only
// the cars, each over the background it covered. The log on the bottom
// screen pauses while the map is shown ([3ds] map = 0 keeps the log).

#include "platform/ctr.h"
#include "platform/platform.h"
#include "port/stagemap.h"

#include <3ds.h>

#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

const int s_width = 320, s_height = 240;
// Updates of the cars: one every this many frames.
const int s_interval = 4;

const uint16_t s_backColor = 0x18e4, s_routeColor = 0xc618, s_splitColor = 0xfd20, s_startColor = 0x07e0,
               s_finishColor = 0xf800, s_playerColor = 0xffe0, s_otherColor = 0x5d1f;

int s_enabled = -1;             // [3ds] map, read on the first update
bool s_shown;
const void *s_route;            // the route the background was drawn for
int s_routeCount;
std::vector<uint16_t> s_background;     // RGB565, landscape rows
// Route to screen: screen = (world - origin) * scale + offset.
float s_originX, s_originZ, s_scale, s_offsetX, s_offsetY;
int s_frame;
// The rectangles the cars were drawn over, to restore.
struct Box {
    int x0, y0, x1, y1;
};
std::vector<Box> s_drawn;

void Plot(int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < s_width && y < s_height)
        s_background[y * s_width + x] = color;
}

void Disc(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                Plot(cx + x, cy + y, color);
}

// A line two pixels thick.
void Line(float x0, float y0, float x1, float y1, uint16_t color)
{
    int steps = (int)std::max(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        int x = (int)lroundf(x0 + (x1 - x0) * t), y = (int)lroundf(y0 + (y1 - y0) * t);
        Plot(x, y, color);
        Plot(x + 1, y, color);
        Plot(x, y + 1, color);
        Plot(x + 1, y + 1, color);
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
    const float margin = 12;
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

// The bottom screen's framebuffer: 240x320, columns of the landscape picture
// from the bottom up.
void Put(uint8_t *fb, bool rgb565, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= s_width || y >= s_height)
        return;
    size_t i = (size_t)x * s_height + (s_height - 1 - y);
    if (rgb565) {
        ((uint16_t *)fb)[i] = color;
    } else {
        uint8_t *p = fb + i * 3;
        p[0] = (uint8_t)((color & 0x1f) << 3);
        p[1] = (uint8_t)(((color >> 5) & 0x3f) << 2);
        p[2] = (uint8_t)((color >> 11) << 3);
    }
}

void CopyBox(uint8_t *fb, bool rgb565, const Box &b)
{
    for (int y = std::max(0, b.y0); y <= std::min(s_height - 1, b.y1); y++)
        for (int x = std::max(0, b.x0); x <= std::min(s_width - 1, b.x1); x++)
            Put(fb, rgb565, x, y, s_background[y * s_width + x]);
}

uint8_t *Framebuffer(bool &rgb565)
{
    rgb565 = gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES;
    return gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
}

void Flush(uint8_t *fb, bool rgb565)
{
    GSPGPU_FlushDataCache(fb, (u32)s_width * s_height * (rgb565 ? 2 : 3));
}

void ShowAll()
{
    bool rgb565;
    uint8_t *fb = Framebuffer(rgb565);
    CopyBox(fb, rgb565, { 0, 0, s_width - 1, s_height - 1 });
    Flush(fb, rgb565);
    s_drawn.clear();
}

void DrawCars(const int *cars, int count, int forwardX, int forwardZ)
{
    bool rgb565;
    uint8_t *fb = Framebuffer(rgb565);
    for (const Box &b : s_drawn)
        CopyBox(fb, rgb565, b);
    s_drawn.clear();
    // The others first, so that the player's arrow stays on top.
    for (int c = count - 1; c >= 0; c--) {
        float sx, sy;
        ToScreen(cars[c * 2], cars[c * 2 + 1], sx, sy);
        int cx = (int)lroundf(sx), cy = (int)lroundf(sy);
        if (c > 0) {
            for (int y = -2; y <= 2; y++)
                for (int x = -2; x <= 2; x++)
                    if (x * x + y * y <= 5)
                        Put(fb, rgb565, cx + x, cy + y, s_otherColor);
            s_drawn.push_back({ cx - 2, cy - 2, cx + 2, cy + 2 });
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
                    Put(fb, rgb565, cx + x, cy + y, s_playerColor);
            }
        s_drawn.push_back({ cx - 8, cy - 8, cx + 8, cy + 8 });
    }
    Flush(fb, rgb565);
}

} // namespace

void Ctr_MapHide(void)
{
    if (!s_shown)
        return;
    s_shown = false;
    s_drawn.clear();
    Ctr_PauseConsole(false);
}

extern "C" void StageMap_Update(const int *position, const unsigned char *flags, int routeCount, int stride,
                                const int *cars, int carCount, int forwardX, int forwardZ)
{
    if (s_enabled < 0)
        s_enabled = Platform_GetSettingInt("3ds.map", 1) != 0;
    if (!s_enabled || position == NULL || routeCount < 2 || carCount < 1 || !Ctr_IsDriving()) {
        Ctr_MapHide();
        return;
    }
    if (position != s_route || routeCount != s_routeCount) {
        DrawBackground(position, flags, routeCount, stride);
        s_route = position;
        s_routeCount = routeCount;
        s_shown = false;
    }
    if (!s_shown) {
        Ctr_PauseConsole(true);
        ShowAll();
        s_shown = true;
        s_frame = 0;
    }
    if (s_frame++ % s_interval == 0)
        DrawCars(cars, carCount, forwardX, forwardZ);
}
