// The release of texture CPU copies for executors short of memory (the
// 3DS's, render/gfx_c3d.cpp) and what the game does with such textures
// afterwards: lock them again to change them (RemapTextureAlpha) and copy
// them (its texture cache). Also the BC1/BC3 encoder those use.

#include "port/sys.h"
#include "render/gfx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <vector>

extern "C" void Sys_GetDesktopSize(int *w, int *h) { *w = 640; *h = 480; }
extern "C" void Sys_Log(const char *, ...) {}
int Platform_GetSettingInt(const char *, int value) { return value; }

static void Check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "texture release: %s failed\n", what);
        exit(1);
    }
}

// Keeps each uploaded texture's top level as BGRA, as the 3DS keeps its
// reduced copy, and hands it back.
struct ReleasingExecutor : GfxExecutor {
    std::map<GfxTexture *, std::vector<uint32_t>> copies;
    bool SetVideoMode(int, int, bool) override { return true; }
    void SetFullscreen(bool) override {}
    void Execute(GfxFrame &, std::vector<GfxTexture *> &textures, GfxReadback &, bool, bool, bool) override
    {
        for (GfxTexture *t : textures)
            t->dirty = false;
    }
    void DestroyTexture(GfxTexture *t) override { copies.erase(t); }
    void ReadTexture(GfxTexture *) override {}
    bool Upload(GfxTexture *t) override
    {
        std::vector<uint32_t> &copy = copies[t];
        copy.resize((size_t)t->width * t->height);
        if (t->format == GFX_FORMAT_BGRA8 || t->format == GFX_FORMAT_BGRX8) {
            memcpy(copy.data(), t->levels[0][0].data(), copy.size() * 4);
        } else {
            int bw = (t->width + 3) / 4, blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
            uint32_t block[16];
            for (int by = 0; by < (t->height + 3) / 4; by++)
                for (int bx = 0; bx < bw; bx++) {
                    Gfx_DecodeBlock(t->format, t->levels[0][0].data() + (by * bw + bx) * blockBytes, block);
                    for (int i = 0; i < 16; i++)
                        copy[(size_t)(by * 4 + i / 4) * t->width + bx * 4 + i % 4] = block[i];
                }
        }
        return true;
    }
    bool ReadPixels(GfxTexture *t, std::vector<uint32_t> &bgra) override
    {
        auto it = copies.find(t);
        if (it == copies.end())
            return false;
        bgra = it->second;
        return true;
    }
};
GfxExecutor *Gfx_CreateGpuExecutor() { return new ReleasingExecutor; }

static int Error(uint32_t a, uint32_t b)
{
    int worst = 0;
    for (int s = 0; s < 32; s += 8)
        worst = std::max(worst, abs((int)((a >> s) & 0xff) - (int)((b >> s) & 0xff)));
    return worst;
}

static DWORD Pattern(int x, int y) { return 0xff000000u | (DWORD)(x * 16) << 16 | (DWORD)(y * 16) << 8 | 0x40; }

static GfxTexture *Filled(int format)
{
    GfxTexture *t = Gfx_CreateTexture(16, 16, format, 1, 0);
    GfxLockedRect rect;
    Check(Gfx_LockTexture(t, 0, 0, &rect), "lock");
    std::vector<uint32_t> pixels(256);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            pixels[y * 16 + x] = Pattern(x, y);
    if (format == GFX_FORMAT_BGRA8) {
        memcpy(rect.pixels, pixels.data(), 1024);
    } else {
        for (int by = 0; by < 4; by++)
            for (int bx = 0; bx < 4; bx++) {
                uint32_t block[16];
                for (int i = 0; i < 16; i++)
                    block[i] = pixels[(by * 4 + i / 4) * 16 + bx * 4 + i % 4];
                Gfx_EncodeBlock(format, block, (uint8_t *)rect.pixels + (by * 4 + bx) * (format == GFX_FORMAT_BC1 ? 8 : 16));
            }
    }
    Gfx_UnlockTexture(t, 0, 0);
    return t;
}

int main()
{
    // The encoder: a smooth block close to its colours, and BC1's
    // transparent pixels kept.
    for (int format : { GFX_FORMAT_BC1, GFX_FORMAT_BC3 }) {
        uint32_t in[16], out[16];
        uint8_t block[16];
        for (int i = 0; i < 16; i++)
            in[i] = (format == GFX_FORMAT_BC3 ? (uint32_t)(i * 17) : 0xffu) << 24 | (uint32_t)(i * 8) << 16 |
                    (uint32_t)(255 - i * 8) << 8 | 0x80;
        if (format == GFX_FORMAT_BC1)
            in[5] = 0;
        Gfx_EncodeBlock(format, in, block);
        Gfx_DecodeBlock(format, block, out);
        for (int i = 0; i < 16; i++) {
            if (format == GFX_FORMAT_BC1 && i == 5)
                Check(out[i] >> 24 == 0, "BC1 transparency");
            else
                // Three colours only, with a transparent pixel.
                Check(Error(in[i], out[i]) <= (format == GFX_FORMAT_BC1 ? 32 : 24), "BC encoding");
        }
    }

    Check(Gfx_SetVideoMode(640, 480, FALSE), "renderer init");
    for (int format : { GFX_FORMAT_BGRA8, GFX_FORMAT_BC1, GFX_FORMAT_BC3 }) {
        GfxTexture *t = Filled(format);
        // The next texture releases the finished one's copy.
        GfxTexture *other = Gfx_CreateTexture(8, 8, GFX_FORMAT_BGRA8, 1, 0);
        Check(t->released && t->levels[0][0].empty(), "copy released");

        // A copy into the game's cache keeps the image.
        GfxTexture *cache = Gfx_CreateTexture(16, 16, GFX_FORMAT_BGRA8, 1, GFX_TEXTURE_DYNAMIC);
        Gfx_CopyTexture(cache, t);
        const uint32_t *cached = (const uint32_t *)cache->levels[0][0].data();
        // Compressed: a gradient in two directions per block, approximated.
        int tolerance = format == GFX_FORMAT_BGRA8 ? 0 : 40;
        for (int i = 0; i < 256; i++)
            Check(Error(cached[i], Pattern(i % 16, i / 16)) <= tolerance, "copy of a released texture");

        // Locked again, it has its image back, changes stick and it is
        // released again afterwards.
        GfxLockedRect rect;
        Check(Gfx_LockTexture(t, 0, 0, &rect) && !t->released, "lock after release");
        if (format == GFX_FORMAT_BGRA8) {
            uint32_t *p = (uint32_t *)rect.pixels;
            for (int i = 0; i < 256; i++)
                Check(p[i] == Pattern(i % 16, i / 16), "restored pixels");
            p[0] = 0xff00ff00;
        } else {
            uint32_t block[16];
            Gfx_DecodeBlock(format, (uint8_t *)rect.pixels, block);
            Check(Error(block[5], Pattern(1, 1)) <= 40, "restored compressed pixels");
        }
        Gfx_UnlockTexture(t, 0, 0);
        Gfx_DestroyTexture(other);
        Gfx_Present(FALSE);
        Check(t->released, "released again");
        if (format == GFX_FORMAT_BGRA8) {
            Gfx_CopyTexture(cache, t);
            Check(((const uint32_t *)cache->levels[0][0].data())[0] == 0xff00ff00, "change kept");
        }
        Gfx_DestroyTexture(cache);
        Gfx_DestroyTexture(t);
    }

    // Vertex buffers get their memory on the first lock.
    GfxVertexBuffer *buffer = Gfx_CreateVertexBuffer(1000);
    Check(buffer->vertices.empty(), "lazy vertex buffer");
    Check(Gfx_LockVertexBuffer(buffer) != NULL && buffer->vertices.size() == 1000, "vertex buffer lock");
    Gfx_DestroyVertexBuffer(buffer);
    puts("texture release: copies released, restored for locks and copies; BC encoding");
    return 0;
}
