// The citro3d executor (Nintendo 3DS): runs a recorded frame (gfx_core.cpp)
// on the PICA200 with the vertex shaders in shaders/*.v.pica and the texture
// combiners for Direct3D 7's texture stages.
//
// The game keeps its 640x480 back buffer; its picture is drawn at half size
// on the 400x240 top screen (4:3 in the middle, or stretched with [3ds]
// stretch = 1), the screen being rotated as the 3DS has it. Textures are
// shrunk to [3ds] texture_max, converted to 16-bit formats and their CPU
// copies released (gfx_core.cpp, Upload) to fit the console's memory.
//
// Left out: specular light, spot cones (lit as point lights), bump mapping,
// lines and points, and drawing into textures (render targets keep their
// last upload, and the stages sampling the car's reflection cube map are
// skipped).

#include "platform/platform.h"
#include "port/sys.h"
#include "render/gfx_internal.h"
#include "platform/ctr.h"

#include "shaders_pica.h"

#include <3ds.h>
#include <citro3d.h>

#include <malloc.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>

extern "C" u32 __ctru_heap_size;

namespace {

// ---- texture conversion -----------------------------------------------------

// Offset of a pixel in the PICA's tiled layout: 8x8 tiles in rows, the pixels
// of a tile in Morton order. The first row in memory is the bottom one
// (t = 0), so the game's rows (v = 0 at the top) are stored upside down.
inline uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    y = height - 1 - y;
    uint32_t tile = (y >> 3) * (width >> 3) + (x >> 3);
    uint32_t morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return tile * 64 + morton;
}

enum AlphaKind { ALPHA_NONE, ALPHA_BINARY, ALPHA_FULL };

GPU_TEXCOLOR TexColor(AlphaKind alpha)
{
    return alpha == ALPHA_NONE ? GPU_RGB565 : alpha == ALPHA_BINARY ? GPU_RGBA5551 : GPU_RGBA4;
}

// One BGRA pixel (0xAARRGGBB) in a PICA 16-bit format.
inline uint16_t Pack(uint32_t p, GPU_TEXCOLOR format)
{
    uint32_t a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
    switch (format) {
    case GPU_RGB565:
        return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
    case GPU_RGBA5551:
        return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128 ? 1 : 0));
    default:
        return (uint16_t)((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4));
    }
}

// The top level of a texture as BGRA pixels (compressed ones decoded).
void TopLevel(const GfxTexture *t, std::vector<uint32_t> &out)
{
    const uint8_t *src = t->levels[0][0].data();
    size_t n = (size_t)t->width * t->height;
    out.resize(n);
    switch (t->format) {
    case GFX_FORMAT_BGRX8:
        for (size_t i = 0; i < n; i++)
            out[i] = ((const uint32_t *)src)[i] | 0xff000000;
        break;
    case GFX_FORMAT_BGRA8:
        memcpy(out.data(), src, n * 4);
        break;
    case GFX_FORMAT_BC1:
    case GFX_FORMAT_BC3: {
        int blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (t->width + 3) / 4, bh = (t->height + 3) / 4;
        uint32_t block[16];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                Gfx_DecodeBlock(t->format, src + (by * bw + bx) * blockBytes, block);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int px = bx * 4 + x, py = by * 4 + y;
                        if (px < t->width && py < t->height)
                            out[(size_t)py * t->width + px] = block[y * 4 + x];
                    }
            }
        break;
    }
    default:
        std::fill(out.begin(), out.end(), 0xffffffffu);
        break;
    }
}

AlphaKind Alpha(const GfxTexture *t, const std::vector<uint32_t> &pixels)
{
    if (t->format == GFX_FORMAT_BGRX8)
        return ALPHA_NONE;
    AlphaKind kind = ALPHA_NONE;
    for (uint32_t p : pixels) {
        uint32_t a = p >> 24;
        if (a != 255) {
            if (a != 0)
                return ALPHA_FULL;
            kind = ALPHA_BINARY;
        }
    }
    return kind;
}

int PowerOfTwoAtLeast(int v)
{
    int p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

// Resamples src (sw x sh) to dst (dw x dh): box filter when shrinking, the
// nearest pixel when growing.
void Resample(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
    int fx = std::max(1, sw / dw), fy = std::max(1, sh / dh);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh);
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw);
            if (fx == 1 && fy == 1) {
                dst[y * dw + x] = src[(size_t)y0 * sw + x0];
                continue;
            }
            uint32_t sum[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < fy; j++)
                for (int i = 0; i < fx; i++) {
                    uint32_t p = src[(size_t)(y0 + j) * sw + x0 + i];
                    sum[0] += p & 0xff;
                    sum[1] += (p >> 8) & 0xff;
                    sum[2] += (p >> 16) & 0xff;
                    sum[3] += p >> 24;
                }
            uint32_t n = (uint32_t)(fx * fy);
            dst[y * dw + x] = (sum[0] / n) | (sum[1] / n) << 8 | (sum[2] / n) << 16 | (sum[3] / n) << 24;
        }
    }
}

struct CtrTexture {
    C3D_Tex tex;
    bool valid = false;
    int maxLevel = 0;           // mip levels below the top one
    bool bump = false;          // dU/dV maps: no equivalent, sampled as white
    // The top level as uploaded (tiled, 16-bit), for the game's locks and
    // copies of a texture whose CPU copy was released (ReadPixels).
    std::vector<uint16_t> shadow;
};

// One PICA 16-bit pixel as BGRA (0xAARRGGBB).
inline uint32_t Unpack(uint16_t p, GPU_TEXCOLOR format)
{
    uint32_t r, g, b, a;
    switch (format) {
    case GPU_RGB565:
        r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        a = 255;
        break;
    case GPU_RGBA5551:
        r = (p >> 11) & 31; g = (p >> 6) & 31; b = (p >> 1) & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        a = (p & 1) ? 255 : 0;
        break;
    default:
        r = ((p >> 12) & 15) * 17; g = ((p >> 8) & 15) * 17; b = ((p >> 4) & 15) * 17; a = (p & 15) * 17;
        break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

// ---- state translation ----------------------------------------------------------

GPU_BLENDFACTOR BlendFactor(int d3d)
{
    switch (d3d) {
    case GFX_BLEND_ZERO: return GPU_ZERO;
    case GFX_BLEND_ONE: return GPU_ONE;
    case GFX_BLEND_SRCCOLOR: return GPU_SRC_COLOR;
    case GFX_BLEND_INVSRCCOLOR: return GPU_ONE_MINUS_SRC_COLOR;
    case GFX_BLEND_SRCALPHA: return GPU_SRC_ALPHA;
    case GFX_BLEND_INVSRCALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case GFX_BLEND_DESTALPHA: return GPU_DST_ALPHA;
    case GFX_BLEND_INVDESTALPHA: return GPU_ONE_MINUS_DST_ALPHA;
    case GFX_BLEND_DESTCOLOR: return GPU_DST_COLOR;
    case GFX_BLEND_INVDESTCOLOR: return GPU_ONE_MINUS_DST_COLOR;
    case GFX_BLEND_SRCALPHASAT: return GPU_SRC_ALPHA_SATURATE;
    default: return GPU_ONE;
    }
}

GPU_TESTFUNC TestFunc(int d3d)
{
    switch (d3d) {
    case GFX_CMP_NEVER: return GPU_NEVER;
    case GFX_CMP_LESS: return GPU_LESS;
    case GFX_CMP_EQUAL: return GPU_EQUAL;
    case GFX_CMP_LESSEQUAL: return GPU_LEQUAL;
    case GFX_CMP_GREATER: return GPU_GREATER;
    case GFX_CMP_NOTEQUAL: return GPU_NOTEQUAL;
    case GFX_CMP_GREATEREQUAL: return GPU_GEQUAL;
    default: return GPU_ALWAYS;
    }
}

GPU_TEXTURE_WRAP_PARAM Wrap(int address)
{
    switch (address) {
    case GFX_ADDRESS_CLAMP: return GPU_CLAMP_TO_EDGE;
    case GFX_ADDRESS_MIRROR: return GPU_MIRRORED_REPEAT;
    default: return GPU_REPEAT;
    }
}

uint32_t PicaColor(DWORD argb)     // 0xAARRGGBB -> the PICA's 0xAABBGGRR
{
    return (argb & 0xff00ff00) | ((argb >> 16) & 0xff) | ((argb & 0xff) << 16);
}

void Transform(const GfxVector &v, float w, const GfxMatrix &m, float out[3])
{
    const float *p = &m._11;
    for (int c = 0; c < 3; c++)
        out[c] = v.x * p[c] + v.y * p[4 + c] + v.z * p[8 + c] + w * p[12 + c];
}

void Multiply(const GfxMatrix &a, const GfxMatrix &b, GfxMatrix &out)
{
    const float *pa = &a._11, *pb = &b._11;
    float *po = &out._11;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += pa[r * 4 + k] * pb[k * 4 + c];
            po[r * 4 + c] = s;
        }
}

inline float At(const GfxMatrix &m, int row, int col)
{
    return (&m._11)[row * 4 + col];
}

// Uniform locations of the two programs.
struct Uniforms3D {
    int projection, modelView, tex0Matrix, tex1Matrix;
    int materialDiffuse, materialAmbient, materialEmissive, ambient, lights;
    int lightLoop;
    int lighting, diffuseFromVertex, ambientFromVertex, emissiveFromVertex;
    int reflect0, reflect1, tex0FromUV1, tex1FromUV1, normalizeNormals;
};

struct Executor : GfxExecutor {
    bool ready = false;
    C3D_RenderTarget *top = nullptr;
    DVLB_s *dvlb = nullptr;
    std::vector<uint32_t> shaderCode;       // the DVLB, word-aligned
    shaderProgram_s program3D, programTL;
    Uniforms3D u3d = {};
    int uScreen = -1;
    C3D_AttrInfo attr3D, attrTL;
    shaderProgram_s *bound = nullptr;

    int gameWidth = 640, gameHeight = 480;
    bool stretch = false;
    int textureMax = 128;

    // Vertices and indices of the frame, in linear memory.
    uint8_t *arena = nullptr;
    size_t arenaSize = 0, arenaUsed = 0;
    std::vector<void *> retiredLinear;      // freed once the GPU is done

    bool inFrame = false;
    u64 lastFrameEnd = 0;
    unsigned frames = 0;
    unsigned converted = 0;     // textures since the last frame
    u64 convertMs = 0;
    std::vector<C3D_Tex> retiredTextures;

    CtrTexture white;
    std::vector<uint32_t> pixels, scaled, level;
    uint16_t *scratch = nullptr;            // tiled level being uploaded (linear memory)
    size_t scratchSize = 0;

    C3D_FogLut fogLut;
    float fogKey[6] = { 0, 0, 0, 0, -1, -1 };

    // What was last sent to citro3d, which records every call as commands:
    // a stage sends thousands of draws, and the command buffer must hold a
    // whole frame (libctru stops the program when it overflows).
    struct Sent {
        bool valid = false;
        GPU_TESTFUNC depthFunc;
        int depthMask;
        float depthScale, depthOffset;
        GPU_CULLMODE cull;
        GPU_BLENDFACTOR blend[2];
        bool alphaTest;
        GPU_TESTFUNC alphaFunc;
        int alphaRef;
        u32 viewport[4];
        bool fog;
        u32 fogColor;
        bool fogLut;
        C3D_AttrInfo *attr;
        C3D_TexEnv env[6];
        C3D_Tex tex[2];
        bool texBound[2];
    } sent;

    // Statistics for the log.
    unsigned drawsThisFrame = 0, maxDraws = 0, dropped = 0;
    float maxCmdUsage = 0.0f;
    u64 statsStart = 0;

    ~Executor() override
    {
        if (!ready)
            return;
        if (inFrame)
            C3D_FrameEnd(0);
        C3D_Fini();
    }

    // ---- setup ----

    bool Init()
    {
        if (!C3D_Init(0x400000)) {
            Sys_Log("gfx: C3D_Init failed");
            return false;
        }
        top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (top == NULL) {
            Sys_Log("gfx: no render target");
            return false;
        }
        C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT,
                                  GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                      GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

        shaderCode.resize((sizeof(g_fixed_shbin) + 3) / 4);
        memcpy(shaderCode.data(), g_fixed_shbin, sizeof(g_fixed_shbin));
        dvlb = DVLB_ParseFile((u32 *)shaderCode.data(), (u32)sizeof(g_fixed_shbin));
        if (dvlb == NULL || dvlb->numDVLE < 2) {
            Sys_Log("gfx: bad shader binary");
            return false;
        }
        shaderProgramInit(&program3D);
        shaderProgramSetVsh(&program3D, &dvlb->DVLE[0]);
        shaderProgramInit(&programTL);
        shaderProgramSetVsh(&programTL, &dvlb->DVLE[1]);

        shaderInstance_s *vs = program3D.vertexShader;
        u3d.projection = shaderInstanceGetUniformLocation(vs, "projection");
        u3d.modelView = shaderInstanceGetUniformLocation(vs, "modelView");
        u3d.tex0Matrix = shaderInstanceGetUniformLocation(vs, "tex0Matrix");
        u3d.tex1Matrix = shaderInstanceGetUniformLocation(vs, "tex1Matrix");
        u3d.materialDiffuse = shaderInstanceGetUniformLocation(vs, "materialDiffuse");
        u3d.materialAmbient = shaderInstanceGetUniformLocation(vs, "materialAmbient");
        u3d.materialEmissive = shaderInstanceGetUniformLocation(vs, "materialEmissive");
        u3d.ambient = shaderInstanceGetUniformLocation(vs, "ambient");
        u3d.lights = shaderInstanceGetUniformLocation(vs, "lights");
        u3d.lightLoop = shaderInstanceGetUniformLocation(vs, "lightLoop");
        u3d.lighting = shaderInstanceGetUniformLocation(vs, "lighting");
        u3d.diffuseFromVertex = shaderInstanceGetUniformLocation(vs, "diffuseFromVertex");
        u3d.ambientFromVertex = shaderInstanceGetUniformLocation(vs, "ambientFromVertex");
        u3d.emissiveFromVertex = shaderInstanceGetUniformLocation(vs, "emissiveFromVertex");
        u3d.reflect0 = shaderInstanceGetUniformLocation(vs, "reflect0");
        u3d.reflect1 = shaderInstanceGetUniformLocation(vs, "reflect1");
        u3d.tex0FromUV1 = shaderInstanceGetUniformLocation(vs, "tex0FromUV1");
        u3d.tex1FromUV1 = shaderInstanceGetUniformLocation(vs, "tex1FromUV1");
        u3d.normalizeNormals = shaderInstanceGetUniformLocation(vs, "normalizeNormals");
        uScreen = shaderInstanceGetUniformLocation(programTL.vertexShader, "screen");

        // GfxVertex: position, normal, diffuse, specular, uv0, uv1 (48 bytes).
        AttrInfo_Init(&attr3D);
        AttrInfo_AddLoader(&attr3D, 0, GPU_FLOAT, 3);
        AttrInfo_AddLoader(&attr3D, 1, GPU_FLOAT, 3);
        AttrInfo_AddLoader(&attr3D, 2, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(&attr3D, 3, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(&attr3D, 4, GPU_FLOAT, 2);
        AttrInfo_AddLoader(&attr3D, 5, GPU_FLOAT, 2);
        // GfxTLVertex: x, y, z, rhw, colour, specular, uv (32 bytes).
        AttrInfo_Init(&attrTL);
        AttrInfo_AddLoader(&attrTL, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(&attrTL, 1, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(&attrTL, 2, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(&attrTL, 3, GPU_FLOAT, 2);

        // Stages without a texture sample this.
        if (C3D_TexInit(&white.tex, 8, 8, GPU_RGB565)) {
            uint16_t *data = (uint16_t *)white.tex.data;
            for (int i = 0; i < 64; i++)
                data[i] = 0xffff;
            C3D_TexFlush(&white.tex);
            white.valid = true;
        }

        textureMax = std::max(8, std::min(1024, Platform_GetSettingInt("3ds.texture_max", 128)));
        stretch = Platform_GetSettingInt("3ds.stretch", 0) != 0;
        Sys_Log("gfx: citro3d, textures up to %d", textureMax);
        ready = true;
        return true;
    }

    bool SetVideoMode(int width, int height, bool) override
    {
        if (!ready && !Init())
            return false;
        gameWidth = width;
        gameHeight = height;
        return true;
    }

    void SetFullscreen(bool) override {}

    // ---- memory the GPU may still read ----

    // Called when the GPU is idle (after C3D_FrameBegin's wait).
    void FreeRetired()
    {
        for (C3D_Tex &t : retiredTextures)
            C3D_TexDelete(&t);
        retiredTextures.clear();
        for (void *p : retiredLinear)
            linearFree(p);
        retiredLinear.clear();
    }

    // Outside a frame and long after the last one, the GPU is done with it.
    bool GpuIdle() const
    {
        return !inFrame && osGetTime() - lastFrameEnd > 100;
    }

    void Retire(C3D_Tex &t)
    {
        if (GpuIdle())
            C3D_TexDelete(&t);
        else
            retiredTextures.push_back(t);
    }

    uint8_t *Allocate(size_t bytes)
    {
        bytes = (bytes + 15) & ~(size_t)15;
        if (arenaUsed + bytes > arenaSize) {
            size_t size = std::max(arenaSize * 2, std::max(bytes, (size_t)0x100000));
            uint8_t *bigger = (uint8_t *)linearAlloc(size);
            if (bigger == NULL) {
                Sys_Log("gfx: out of linear memory (%u bytes)", (unsigned)size);
                return NULL;
            }
            if (arena != NULL)
                retiredLinear.push_back(arena);
            arena = bigger;
            arenaSize = size;
            arenaUsed = 0;
        }
        uint8_t *p = arena + arenaUsed;
        arenaUsed += bytes;
        return p;
    }

    // ---- textures ----

    CtrTexture *Ensure(GfxTexture *t)
    {
        if (t->gpu == NULL)
            t->gpu = new CtrTexture;
        return (CtrTexture *)t->gpu;
    }

    bool UploadLevel(C3D_Tex &tex, int levelIndex, const uint32_t *src, int w, int h, GPU_TEXCOLOR format)
    {
        size_t bytes = (size_t)w * h * 2;
        if (bytes > scratchSize) {
            if (scratch != NULL)
                linearFree(scratch);
            scratch = (uint16_t *)linearAlloc(bytes);
            scratchSize = scratch != NULL ? bytes : 0;
            if (scratch == NULL)
                return false;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                scratch[TiledOffset(x, y, w, h)] = Pack(src[y * w + x], format);
        GSPGPU_FlushDataCache(scratch, bytes);
        C3D_TexLoadImage(&tex, scratch, GPU_TEXFACE_2D, levelIndex);
        return true;
    }

    // Converts the CPU copy of t (top level) for the GPU.
    bool Convert(GfxTexture *t)
    {
        u64 start = osGetTime();
        Ctr_Breadcrumb("converting a %dx%d texture (format %d), %u since the last frame", t->width, t->height,
                       t->format, converted);
        bool done = ConvertLevels(t);
        Ctr_Breadcrumb("converted a %dx%d texture, %u since the last frame", t->width, t->height, converted + 1);
        converted++;
        convertMs += osGetTime() - start;
        return done;
    }

    bool ConvertLevels(GfxTexture *t)
    {
        CtrTexture *g = Ensure(t);
        if (t->format == GFX_FORMAT_BUMP_DUDVL) {
            g->bump = true;
            return true;
        }
        if (t->levels[0][0].empty())
            return false;
        TopLevel(t, pixels);
        AlphaKind alpha = Alpha(t, pixels);
        GPU_TEXCOLOR format = TexColor(alpha);

        // Halve until it fits texture_max; at least 8 (the PICA's minimum).
        int w = t->width, h = t->height;
        while (w > textureMax || h > textureMax) {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        int tw = PowerOfTwoAtLeast(w), th = PowerOfTwoAtLeast(h);
        scaled.resize((size_t)tw * th);
        Resample(pixels.data(), t->width, t->height, scaled.data(), tw, th);
        std::vector<uint32_t>().swap(pixels);

        int maxLevel = 0;
        if (t->mipLevels > 1)
            while ((tw >> (maxLevel + 1)) >= 8 && (th >> (maxLevel + 1)) >= 8)
                maxLevel++;

        if (g->valid && (g->tex.width != tw || g->tex.height != th || g->tex.fmt != format ||
                         g->maxLevel != maxLevel)) {
            Retire(g->tex);
            g->valid = false;
        }
        if (!g->valid) {
            C3D_TexInitParams params = {};
            params.width = (u16)tw;
            params.height = (u16)th;
            params.maxLevel = (u8)maxLevel;
            params.format = format;
            params.type = GPU_TEX_2D;
            params.onVram = true;
            if (!C3D_TexInitWithParams(&g->tex, NULL, params)) {
                params.onVram = false;
                if (!C3D_TexInitWithParams(&g->tex, NULL, params)) {
                    Sys_Log("gfx: no memory for a %dx%d texture", tw, th);
                    return false;
                }
            }
            g->valid = true;
            g->maxLevel = maxLevel;
        }
        int lw = tw, lh = th;
        const uint32_t *src = scaled.data();
        for (int m = 0; m <= maxLevel; m++) {
            if (m > 0) {
                int nw = lw / 2, nh = lh / 2;
                level.resize((size_t)nw * nh);
                Resample(src, lw, lh, level.data(), nw, nh);
                scaled.swap(level);
                src = scaled.data();
                lw = nw;
                lh = nh;
            }
            if (!UploadLevel(g->tex, m, src, lw, lh, format))
                return false;
            if (m == 0)
                g->shadow.assign(scratch, scratch + (size_t)lw * lh);
        }
        uintptr_t address = (uintptr_t)g->tex.data;
        if (address < OS_VRAM_VADDR || address >= OS_VRAM_VADDR + OS_VRAM_SIZE)
            C3D_TexFlush(&g->tex);
        return true;
    }

    bool Upload(GfxTexture *t) override
    {
        if (!ready)
            return false;
        if (t->flags & (GFX_TEXTURE_CUBE | GFX_TEXTURE_RENDER_TARGET | GFX_TEXTURE_STAGING))
            return false;
        return Convert(t);
    }

    void DestroyTexture(GfxTexture *t) override
    {
        CtrTexture *g = (CtrTexture *)t->gpu;
        if (g == NULL)
            return;
        if (g->valid)
            Retire(g->tex);
        delete g;
        t->gpu = NULL;
    }

    void ReadTexture(GfxTexture *) override {}

    bool ReadPixels(GfxTexture *t, std::vector<uint32_t> &bgra) override
    {
        CtrTexture *g = (CtrTexture *)t->gpu;
        if (g == NULL || !g->valid || g->shadow.empty())
            return false;
        int tw = g->tex.width, th = g->tex.height;
        GPU_TEXCOLOR format = g->tex.fmt;
        bgra.resize((size_t)t->width * t->height);
        for (int y = 0; y < t->height; y++) {
            int sy = (int)((long long)y * th / t->height);
            for (int x = 0; x < t->width; x++) {
                int sx = (int)((long long)x * tw / t->width);
                bgra[(size_t)y * t->width + x] = Unpack(g->shadow[TiledOffset(sx, sy, tw, th)], format);
            }
        }
        return true;
    }

    void UploadDirty(std::vector<GfxTexture *> &textures)
    {
        for (GfxTexture *t : textures) {
            if (!t->dirty)
                continue;
            t->dirty = false;
            if (t->flags & (GFX_TEXTURE_CUBE | GFX_TEXTURE_STAGING))
                continue;
            Convert(t);
        }
    }

    // The texture a stage samples, or NULL (white) when it has none the
    // 3DS can use.
    CtrTexture *StageTexture(const GfxStageState &stage)
    {
        GfxTexture *t = stage.texture;
        if (t == NULL)
            return NULL;
        if (t->cube != NULL)
            t = t->cube;
        if (t->flags & GFX_TEXTURE_CUBE)
            return NULL;
        CtrTexture *g = (CtrTexture *)t->gpu;
        if (g == NULL || !g->valid || g->bump)
            return NULL;
        return g;
    }

    static bool IsCube(const GfxStageState &stage)
    {
        GfxTexture *t = stage.texture;
        if (t != NULL && t->cube != NULL)
            t = t->cube;
        return t != NULL && (t->flags & GFX_TEXTURE_CUBE);
    }

    // ---- uniforms (written only when they change) ----

    void SetF(int id, float x, float y, float z, float w)
    {
        C3D_FVec &v = C3D_FVUnif[GPU_VERTEX_SHADER][id];
        if (v.x == x && v.y == y && v.z == z && v.w == w)
            return;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, id, x, y, z, w);
    }

    void SetB(int id, bool value)
    {
        if (id < 0)
            return;
        bool current = (C3D_BoolUnifs[GPU_VERTEX_SHADER] & BIT(id - 0x68)) != 0;
        if (current != value || !sent.valid)
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, id, value);
    }

    void SetAttrInfo(C3D_AttrInfo *attr)
    {
        if (sent.valid && sent.attr == attr)
            return;
        C3D_SetAttrInfo(attr);
        sent.attr = attr;
    }

    // Room left in the command buffer for one more draw; when it is short
    // the rest of the frame is dropped instead of overflowing.
    bool CommandRoom()
    {
        u32 *addr, size, offset;
        GPUCMD_GetBuffer(&addr, &size, &offset);
        if (size - offset >= 0x4000)
            return true;
        if (dropped++ == 0)
            Sys_Log("gfx: command buffer full, dropping the rest of the frame");
        return false;
    }

    void Bind(shaderProgram_s *program)
    {
        if (bound != program) {
            C3D_BindProgram(program);
            bound = program;
        }
    }

    // ---- screen mapping ----

    // The game's picture on the top screen (landscape, y down).
    void PictureRect(bool stretched, float &x, float &y, float &w, float &h) const
    {
        if (stretched || stretch) {
            x = 0;
            y = 0;
            w = 400;
            h = 240;
        } else {
            w = 240.0f * gameWidth / gameHeight;
            if (w > 400)
                w = 400;
            h = 240;
            x = (400 - w) / 2;
            y = 0;
        }
    }

    // A landscape rectangle as the rotated framebuffer's viewport.
    void SetViewport(float lx, float ly, float lw, float lh)
    {
        int x = (int)lroundf(240 - (ly + lh)), y = (int)lroundf(400 - (lx + lw));
        int w = (int)lroundf(lh), h = (int)lroundf(lw);
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0)
            w = h = 1;
        u32 v[4] = { (u32)x, (u32)y, (u32)w, (u32)h };
        if (sent.valid && memcmp(v, sent.viewport, sizeof(v)) == 0)
            return;
        memcpy(sent.viewport, v, sizeof(v));
        C3D_SetViewport(v[0], v[1], v[2], v[3]);
    }

    void SetGameViewport(const GfxViewport &vp, bool stretched)
    {
        float px, py, pw, ph;
        PictureRect(stretched, px, py, pw, ph);
        float sx = pw / gameWidth, sy = ph / gameHeight;
        SetViewport(px + vp.x * sx, py + vp.y * sy, vp.width * sx, vp.height * sy);
    }

    void SetPictureViewport(bool stretched)
    {
        float px, py, pw, ph;
        PictureRect(stretched, px, py, pw, ph);
        SetViewport(px, py, pw, ph);
    }

    // ---- fixed-function state ----

    void SetFog(const GfxDrawState &s, bool transformed)
    {
        bool linear = s.fog && !transformed &&
                      (s.fogTableMode == GFX_FOG_LINEAR || (s.fogTableMode == GFX_FOG_NONE && s.fogVertexMode == GFX_FOG_LINEAR));
        if (!linear) {
            if (sent.fog || !sent.valid)
                C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            sent.fog = false;
            return;
        }
        // The LUT maps depth (0 near .. 1 far, from the projection) to the
        // fog factor of the eye-space depth: (end - z) / (end - start).
        float key[6] = { At(s.projection, 2, 2), At(s.projection, 2, 3), At(s.projection, 3, 2),
                         At(s.projection, 3, 3), s.fogStart, s.fogEnd };
        if (memcmp(key, fogKey, sizeof(key)) != 0) {
            memcpy(fogKey, key, sizeof(key));
            float data[256];
            float range = s.fogEnd - s.fogStart;
            for (int i = 0; i <= 128; i++) {
                float d = i / 128.0f;
                float den = d * key[1] - key[0];
                float z = den != 0.0f ? (key[2] - d * key[3]) / den : s.fogEnd;
                float f = range != 0.0f ? (s.fogEnd - z) / range : 1.0f;
                f = std::max(0.0f, std::min(1.0f, f));
                if (i < 128)
                    data[i] = f;
                if (i > 0)
                    data[i + 127] = f - data[i - 1];
            }
            FogLut_FromArray(&fogLut, data);
            sent.fogLut = false;
        }
        u32 color = PicaColor(s.fogColor) & 0xffffff;
        if (!sent.fog || sent.fogColor != color) {
            C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
            C3D_FogColor(color);
            sent.fog = true;
            sent.fogColor = color;
        }
        // The table is 128 words: sent only when it changes.
        if (!sent.fogLut) {
            C3D_FogLutBind(&fogLut);
            sent.fogLut = true;
        }
    }

    // Direct3D texture stage arguments as TEV sources and operands.
    static void Source(int arg, int stage, bool hasTexture, GPU_TEVSRC &src, int &rgbOp, int &alphaOp)
    {
        switch (arg & 0xf) {
        case GFX_TA_DIFFUSE:
        case GFX_TA_SPECULAR:
            src = GPU_PRIMARY_COLOR;
            break;
        case GFX_TA_CURRENT:
            src = stage == 0 ? GPU_PRIMARY_COLOR : GPU_PREVIOUS;
            break;
        case GFX_TA_TEXTURE:
            src = !hasTexture ? GPU_CONSTANT : stage == 0 ? GPU_TEXTURE0 : GPU_TEXTURE1;
            break;
        case GFX_TA_TFACTOR:
            src = GPU_CONSTANT;
            break;
        default:
            src = GPU_PREVIOUS;
            break;
        }
        bool complement = (arg & GFX_TA_COMPLEMENT) != 0, replicate = (arg & GFX_TA_ALPHAREPLICATE) != 0;
        rgbOp = replicate ? (complement ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA)
                          : (complement ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR);
        alphaOp = complement ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA;
    }

    static void Function(int op, GPU_COMBINEFUNC &func, GPU_TEVSCALE &scale, bool &secondArg)
    {
        scale = GPU_TEVSCALE_1;
        secondArg = true;
        switch (op) {
        case GFX_TOP_SELECTARG1: func = GPU_REPLACE; secondArg = false; break;
        case GFX_TOP_SELECTARG2: func = GPU_REPLACE; break;     // the caller passes arg 2
        case GFX_TOP_MODULATE: func = GPU_MODULATE; break;
        case GFX_TOP_MODULATE2X: func = GPU_MODULATE; scale = GPU_TEVSCALE_2; break;
        case GFX_TOP_MODULATE4X: func = GPU_MODULATE; scale = GPU_TEVSCALE_4; break;
        case GFX_TOP_ADD: func = GPU_ADD; break;
        case GFX_TOP_ADDSIGNED: func = GPU_ADD_SIGNED; break;
        case GFX_TOP_ADDSIGNED2X: func = GPU_ADD_SIGNED; scale = GPU_TEVSCALE_2; break;
        case GFX_TOP_SUBTRACT: func = GPU_SUBTRACT; break;
        default: func = GPU_REPLACE; secondArg = false; break;
        }
    }

    // One Direct3D stage as one TEV stage; false when it is disabled.
    static bool SetStage(C3D_TexEnv *env, int index, const GfxStageState &st, DWORD textureFactor, bool hasTexture)
    {
        C3D_TexEnvInit(env);
        int colorOp = st.colorOp;
        if (colorOp == GFX_TOP_DISABLE || colorOp == GFX_TOP_BUMPENVMAP || colorOp == GFX_TOP_BUMPENVMAPLUMINANCE) {
            if (index != 0)
                return false;
            // Stage 0 disabled: the diffuse colour.
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            return true;
        }
        // A missing texture samples white: the constant, unless the stage
        // also uses the texture factor (then the factor stands in for it).
        bool usesFactor = ((st.colorArg1 & 0xf) == GFX_TA_TFACTOR) || ((st.colorArg2 & 0xf) == GFX_TA_TFACTOR) ||
                          ((st.alphaArg1 & 0xf) == GFX_TA_TFACTOR) || ((st.alphaArg2 & 0xf) == GFX_TA_TFACTOR);
        C3D_TexEnvColor(env, (usesFactor || hasTexture) ? PicaColor(textureFactor) : 0xffffffff);

        GPU_TEVSRC s1, s2;
        int r1, r2, a1, a2, unused;
        GPU_COMBINEFUNC func;
        GPU_TEVSCALE scale;
        bool second;
        Function(colorOp, func, scale, second);
        Source(colorOp == GFX_TOP_SELECTARG2 ? st.colorArg2 : st.colorArg1, index, hasTexture, s1, r1, unused);
        Source(st.colorArg2, index, hasTexture, s2, r2, unused);
        C3D_TexEnvSrc(env, C3D_RGB, s1, s2, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)r1, (GPU_TEVOP_RGB)r2);
        C3D_TexEnvFunc(env, C3D_RGB, func);
        C3D_TexEnvScale(env, C3D_RGB, scale);

        int alphaOp = st.alphaOp;
        if (alphaOp == GFX_TOP_DISABLE) {
            C3D_TexEnvSrc(env, C3D_Alpha, index == 0 ? GPU_PRIMARY_COLOR : GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        } else {
            Function(alphaOp, func, scale, second);
            Source(alphaOp == GFX_TOP_SELECTARG2 ? st.alphaArg2 : st.alphaArg1, index, hasTexture, s1, unused, a1);
            Source(st.alphaArg2, index, hasTexture, s2, unused, a2);
            C3D_TexEnvSrc(env, C3D_Alpha, s1, s2, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)a1, (GPU_TEVOP_A)a2);
            C3D_TexEnvFunc(env, C3D_Alpha, func);
            C3D_TexEnvScale(env, C3D_Alpha, scale);
        }
        return true;
    }

    // Binds a texture unless that one, with the same parameters, already is.
    void BindTexture(int unit, C3D_Tex *tex)
    {
        if (sent.valid && sent.texBound[unit] && memcmp(&sent.tex[unit], tex, sizeof(C3D_Tex)) == 0)
            return;
        C3D_TexBind(unit, tex);
        sent.tex[unit] = *tex;
        sent.texBound[unit] = true;
    }

    void SetTexture(int unit, const GfxStageState &st, CtrTexture *g)
    {
        if (g == NULL) {
            if (white.valid)
                BindTexture(unit, &white.tex);
            return;
        }
        C3D_Tex *tex = &g->tex;
        GPU_TEXTURE_FILTER_PARAM mag = st.magFilter == GFX_FILTER_LINEAR ? GPU_LINEAR : GPU_NEAREST;
        GPU_TEXTURE_FILTER_PARAM min = st.minFilter == GFX_FILTER_LINEAR ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(tex, mag, min);
        C3D_TexSetFilterMipmap(tex, st.mipFilter == GFX_MIPFILTER_LINEAR ? GPU_LINEAR : GPU_NEAREST);
        C3D_TexSetWrap(tex, Wrap(st.address), Wrap(st.address));
        // Without a mip filter only the top level is used.
        tex->maxLevel = st.mipFilter == GFX_MIPFILTER_NONE ? 0 : g->maxLevel;
        BindTexture(unit, tex);
    }

    void SetCommonState(const GfxDrawState &s, bool transformed, int colorMask)
    {
        bool depthTest = s.depthTest;
        int mask = colorMask | ((depthTest && s.depthWrite) ? GPU_WRITE_DEPTH : 0);
        GPU_TESTFUNC depthFunc = depthTest ? TestFunc(s.depthFunc) : GPU_ALWAYS;
        if (!sent.valid || sent.depthFunc != depthFunc || sent.depthMask != mask) {
            C3D_DepthTest(true, depthFunc, (GPU_WRITEMASK)mask);
            sent.depthFunc = depthFunc;
            sent.depthMask = mask;
        }
        float scale = transformed ? -1.0f : -(s.viewport.maxZ - s.viewport.minZ);
        float offset = transformed ? 0.0f : s.viewport.minZ;
        if (!sent.valid || sent.depthScale != scale || sent.depthOffset != offset) {
            C3D_DepthMap(true, scale, offset);
            sent.depthScale = scale;
            sent.depthOffset = offset;
        }
        // Direct3D's front faces are clockwise on screen; the rotation of the
        // 3DS screen keeps the winding.
        GPU_CULLMODE cull = s.cull == GFX_CULL_CCW ? GPU_CULL_FRONT_CCW
                                                   : s.cull == GFX_CULL_CW ? GPU_CULL_BACK_CCW : GPU_CULL_NONE;
        if (!sent.valid || sent.cull != cull) {
            C3D_CullFace(cull);
            sent.cull = cull;
        }
        GPU_BLENDFACTOR src = s.alphaBlend ? BlendFactor(s.srcBlend) : GPU_ONE;
        GPU_BLENDFACTOR dst = s.alphaBlend ? BlendFactor(s.destBlend) : GPU_ZERO;
        if (!sent.valid || sent.blend[0] != src || sent.blend[1] != dst) {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, src, dst);
            sent.blend[0] = src;
            sent.blend[1] = dst;
        }
        GPU_TESTFUNC alphaFunc = TestFunc(s.alphaFunc);
        int alphaRef = (int)(s.alphaRef & 0xff);
        if (!sent.valid || sent.alphaTest != s.alphaTest ||
            (s.alphaTest && (sent.alphaFunc != alphaFunc || sent.alphaRef != alphaRef))) {
            C3D_AlphaTest(s.alphaTest, alphaFunc, alphaRef);
            sent.alphaTest = s.alphaTest;
            sent.alphaFunc = alphaFunc;
            sent.alphaRef = alphaRef;
        }

        // Texture stages.
        GfxStageState st0 = s.stages[0], st1 = s.stages[1];
        // No reflections: stages sampling the cube map are skipped.
        if (IsCube(st1))
            st1.colorOp = st1.alphaOp = GFX_TOP_DISABLE;
        CtrTexture *tex0 = StageTexture(st0), *tex1 = StageTexture(st1);
        SetTexture(0, st0, tex0);
        C3D_TexEnv env[6];
        for (int i = 0; i < 6; i++)
            C3D_TexEnvInit(&env[i]);
        SetStage(&env[0], 0, st0, s.textureFactor, tex0 != NULL);
        if (st0.colorOp != GFX_TOP_DISABLE && SetStage(&env[1], 1, st1, s.textureFactor, tex1 != NULL))
            SetTexture(1, st1, tex1);
        else
            C3D_TexEnvInit(&env[1]);
        for (int i = 0; i < 6; i++)
            if (!sent.valid || memcmp(&env[i], &sent.env[i], sizeof(C3D_TexEnv)) != 0) {
                C3D_SetTexEnv(i, &env[i]);
                sent.env[i] = env[i];
            }
        SetFog(s, transformed);
        sent.valid = true;
    }

    // The vertex shader's uniforms for a 3D draw.
    void Set3DUniforms(const GfxDrawState &s)
    {
        // Projection, column-vector rows, rotated for the screen (x' = y,
        // y' = -x) and with depth to [-1, 0] (z' = -z).
        const GfxMatrix &p = s.projection;
        SetF(u3d.projection + 0, At(p, 0, 1), At(p, 1, 1), At(p, 2, 1), At(p, 3, 1));
        SetF(u3d.projection + 1, -At(p, 0, 0), -At(p, 1, 0), -At(p, 2, 0), -At(p, 3, 0));
        SetF(u3d.projection + 2, -At(p, 0, 2), -At(p, 1, 2), -At(p, 2, 2), -At(p, 3, 2));
        SetF(u3d.projection + 3, At(p, 0, 3), At(p, 1, 3), At(p, 2, 3), At(p, 3, 3));
        GfxMatrix mv;
        Multiply(s.world, s.view, mv);
        for (int r = 0; r < 4; r++)
            SetF(u3d.modelView + r, At(mv, 0, r), At(mv, 1, r), At(mv, 2, r), At(mv, 3, r));
        for (int stage = 0; stage < 2; stage++) {
            int id = stage == 0 ? u3d.tex0Matrix : u3d.tex1Matrix;
            const GfxStageState &st = s.stages[stage];
            if (st.textureTransform != GFX_TTFF_DISABLE) {
                const GfxMatrix &t = s.texture[stage];
                SetF(id, At(t, 0, 0), At(t, 1, 0), At(t, 2, 0), At(t, 3, 0));
                SetF(id + 1, At(t, 0, 1), At(t, 1, 1), At(t, 2, 1), At(t, 3, 1));
            } else {
                SetF(id, 1, 0, 0, 0);
                SetF(id + 1, 0, 1, 0, 0);
            }
            bool reflect = (st.texCoordIndex & 0xffff0000) == GFX_TCI_CAMERASPACEREFLECTIONVECTOR;
            SetB(stage == 0 ? u3d.reflect0 : u3d.reflect1, reflect);
            SetB(stage == 0 ? u3d.tex0FromUV1 : u3d.tex1FromUV1, (st.texCoordIndex & 0xffff) == 1);
        }
        SetB(u3d.normalizeNormals, s.normalizeNormals);
        SetB(u3d.lighting, s.lighting);
        if (!s.lighting)
            return;

        const GfxMaterial &m = s.material;
        SetF(u3d.materialDiffuse, m.diffuse.r, m.diffuse.g, m.diffuse.b, m.diffuse.a);
        SetF(u3d.materialAmbient, m.ambient.r, m.ambient.g, m.ambient.b, m.ambient.a);
        SetF(u3d.materialEmissive, m.emissive.r, m.emissive.g, m.emissive.b, m.emissive.a);
        SetB(u3d.diffuseFromVertex, s.colorVertex && s.diffuseSource != GFX_MCS_MATERIAL);
        SetB(u3d.ambientFromVertex, s.colorVertex && s.ambientSource != GFX_MCS_MATERIAL);
        SetB(u3d.emissiveFromVertex, s.colorVertex && s.emissiveSource != GFX_MCS_MATERIAL);

        float ar = ((s.ambient >> 16) & 0xff) / 255.0f, ag = ((s.ambient >> 8) & 0xff) / 255.0f,
              ab = (s.ambient & 0xff) / 255.0f;
        int count = 0;
        for (int i = 0; i < 8; i++) {
            if (!s.lightEnabled[i])
                continue;
            const GfxLight &l = s.lights[i];
            ar += l.ambient.r;
            ag += l.ambient.g;
            ab += l.ambient.b;
            int id = u3d.lights + count * 3;
            float v[3];
            if (l.type == GFX_LIGHT_DIRECTIONAL) {
                Transform(l.direction, 0.0f, s.view, v);
                SetF(id, -v[0], -v[1], -v[2], 0.0f);
                SetF(id + 2, 1.0f, 0.0f, 0.0f, 1e30f);
            } else {
                Transform(l.position, 1.0f, s.view, v);
                SetF(id, v[0], v[1], v[2], 1.0f);
                float a0 = l.attenuation0, a1 = l.attenuation1, a2 = l.attenuation2;
                if (a0 == 0.0f && a1 == 0.0f && a2 == 0.0f)
                    a0 = 1.0f;
                float range = l.range > 0.0f ? l.range * l.range : 1e30f;
                SetF(id + 2, a0, a1, a2, range);
            }
            SetF(id + 1, l.diffuse.r, l.diffuse.g, l.diffuse.b, 0.0f);
            count++;
        }
        if (count == 0) {
            // The loop runs at least once: a light that adds nothing.
            SetF(u3d.lights, 0, 0, 1, 0);
            SetF(u3d.lights + 1, 0, 0, 0, 0);
            SetF(u3d.lights + 2, 1, 0, 0, 1e30f);
            count = 1;
        }
        SetF(u3d.ambient, ar, ag, ab, 0.0f);
        C3D_IVec loop = IVec_Pack(count - 1, 0, 3, 0);
        if (C3D_IVUnif[GPU_VERTEX_SHADER][u3d.lightLoop - 0x60] != loop || !sent.valid)
            C3D_IVUnifSet(GPU_VERTEX_SHADER, u3d.lightLoop, count - 1, 0, 3, 0);
    }

    void SetTLUniforms()
    {
        float h = (float)gameHeight, w = (float)gameWidth;
        SetF(uScreen, -2.0f / h, -2.0f / w, 1.0f - 1.0f / h, 1.0f - 1.0f / w);
    }

    static GPU_Primitive_t Primitive(int d3d)
    {
        return d3d == GFX_TRIANGLESTRIP ? GPU_TRIANGLE_STRIP : GPU_TRIANGLES;
    }

    // A clear as a quad over its viewport.
    void Clear(const GfxCommand &c, bool stretched)
    {
        bool color = (c.clearFlags & GFX_CLEAR_TARGET) != 0, depth = (c.clearFlags & GFX_CLEAR_ZBUFFER) != 0;
        if (!color && !depth)
            return;
        GfxTLVertex *v = (GfxTLVertex *)Allocate(4 * sizeof(GfxTLVertex));
        if (v == NULL)
            return;
        float x0 = (float)c.clearViewport.x - 0.5f, y0 = (float)c.clearViewport.y - 0.5f;
        float x1 = x0 + c.clearViewport.width, y1 = y0 + c.clearViewport.height;
        float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
        for (int i = 0; i < 4; i++) {
            v[i].sx = xs[i];
            v[i].sy = ys[i];
            v[i].sz = c.clearZ;
            v[i].rhw = 1.0f;
            v[i].color = c.clearColor;
            v[i].specular = 0;
            v[i].tu = v[i].tv = 0;
        }
        GSPGPU_FlushDataCache(v, 4 * sizeof(GfxTLVertex));
        Bind(&programTL);
        SetAttrInfo(&attrTL);
        SetTLUniforms();
        C3D_BufInfo buf;
        BufInfo_Init(&buf);
        BufInfo_Add(&buf, v, sizeof(GfxTLVertex), 4, 0x3210);
        C3D_SetBufInfo(&buf);
        SetPictureViewport(stretched);
        GfxDrawState s;
        s.depthTest = true;
        s.depthFunc = GFX_CMP_ALWAYS;
        s.depthWrite = depth;
        s.cull = GFX_CULL_NONE;
        s.stages[0].colorOp = GFX_TOP_DISABLE;
        s.stages[0].alphaOp = GFX_TOP_DISABLE;
        s.stages[0].texture = NULL;
        s.stages[1].colorOp = GFX_TOP_DISABLE;
        s.stages[1].texture = NULL;
        SetCommonState(s, true, color ? GPU_WRITE_COLOR : 0);
        C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
    }

    // ---- frames ----

    void BeginFrame()
    {
        Ctr_Breadcrumb("starting frame %u: waiting for the GPU", frames + 1);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        FreeRetired();
        arenaUsed = 0;
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000ff, 0xffffffff);
        C3D_FrameDrawOn(top);
        bound = NULL;
        // Everything is sent again once a frame (C3D_FrameDrawOn also sets
        // the viewport).
        sent.valid = false;
        sent.fogLut = false;
        sent.texBound[0] = sent.texBound[1] = false;
        drawsThisFrame = 0;
        inFrame = true;
    }

    void Execute(GfxFrame &frame, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present, bool,
                 bool stretched) override
    {
        if (!ready)
            return;
        UploadDirty(textures);
        if (!inFrame)
            BeginFrame();
        Ctr_Breadcrumb("frame %u: %u commands, %u vertex bytes%s", frames + 1, (unsigned)frame.commands.size(),
                       (unsigned)frame.vertices.size(), present ? "" : " (flush)");

        uint8_t *vertices = NULL;
        uint16_t *indices = NULL;
        if (!frame.vertices.empty()) {
            vertices = Allocate(frame.vertices.size());
            if (vertices != NULL) {
                memcpy(vertices, frame.vertices.data(), frame.vertices.size());
                GSPGPU_FlushDataCache(vertices, frame.vertices.size());
            }
        }
        if (!frame.indices.empty()) {
            indices = (uint16_t *)Allocate(frame.indices.size() * 2);
            if (indices != NULL) {
                memcpy(indices, frame.indices.data(), frame.indices.size() * 2);
                GSPGPU_FlushDataCache(indices, frame.indices.size() * 2);
            }
        }

        bool skipping = false;      // drawing into a texture, which is not done
        for (const GfxCommand &c : frame.commands) {
            if (c.type == GFX_CMD_TARGET) {
                skipping = c.target != NULL;
                continue;
            }
            if (skipping)
                continue;
            if (!CommandRoom())
                break;
            if (c.type == GFX_CMD_CLEAR) {
                Clear(c, stretched);
                continue;
            }
            if (vertices == NULL || c.primitive == GFX_POINTLIST || c.primitive == GFX_LINELIST ||
                c.primitive == GFX_LINESTRIP)
                continue;
            if (c.firstIndex != ~0u && indices == NULL)
                continue;
            const GfxDrawState &s = frame.states[c.state];
            C3D_BufInfo buf;
            BufInfo_Init(&buf);
            if (c.transformed) {
                Bind(&programTL);
                SetAttrInfo(&attrTL);
                SetTLUniforms();
                BufInfo_Add(&buf, vertices + c.firstVertexByte, sizeof(GfxTLVertex), 4, 0x3210);
                SetPictureViewport(stretched);
            } else {
                Bind(&program3D);
                SetAttrInfo(&attr3D);
                Set3DUniforms(s);
                BufInfo_Add(&buf, vertices + c.firstVertexByte, sizeof(GfxVertex), 6, 0x543210);
                SetGameViewport(s.viewport, stretched);
            }
            C3D_SetBufInfo(&buf);
            SetCommonState(s, c.transformed, GPU_WRITE_COLOR);
            if (c.firstIndex != ~0u)
                C3D_DrawElements(Primitive(c.primitive), (int)c.indexCount, C3D_UNSIGNED_SHORT, indices + c.firstIndex);
            else
                C3D_DrawArrays(Primitive(c.primitive), 0, (int)c.vertexCount);
            drawsThisFrame++;
        }

        if (!present)
            return;
        Ctr_Breadcrumb("frame %u: ending, waiting for vblank", frames + 1);
        C3D_FrameEnd(0);
        inFrame = false;
        u64 previous = lastFrameEnd;
        lastFrameEnd = osGetTime();
        Ctr_FrameShown();
        // After a load: what it took the renderer.
        if (previous != 0 && lastFrameEnd - previous > 2000 && converted > 0)
            Sys_Log("gfx: %.1f s without frames; %u textures converted in %.1f s", (lastFrameEnd - previous) / 1000.0f,
                    converted, convertMs / 1000.0f);
        converted = 0;
        convertMs = 0;
        maxDraws = std::max(maxDraws, drawsThisFrame);
        maxCmdUsage = std::max(maxCmdUsage, C3D_GetCmdBufUsage());
        if (++frames % 600 == 1) {
            float seconds = (lastFrameEnd - statsStart) / 1000.0f;
            Sys_Log("mem: heap %u of %u KB used, linear %u KB, VRAM %u KB free", (unsigned)(mallinfo().uordblks / 1024),
                    (unsigned)(__ctru_heap_size / 1024), (unsigned)(linearSpaceFree() / 1024),
                    (unsigned)(vramSpaceFree() / 1024));
            Sys_Log("gfx: %.1f fps, up to %u draws and %.0f%% of the command buffer a frame, %u frames cut short",
                    frames > 1 && seconds > 0 ? 599 / seconds : 0.0f, maxDraws, maxCmdUsage * 100.0f, dropped);
            statsStart = lastFrameEnd;
            maxDraws = 0;
            maxCmdUsage = 0.0f;
            dropped = 0;
        }
        // No reads of the screen (the sun's visibility test): it reads as
        // covered, so there is no lens flare.
        if (readback.requested) {
            readback.pixels.assign((size_t)readback.width * readback.height, 0xff000000);
            readback.readyWidth = readback.width;
            readback.readyHeight = readback.height;
            readback.ready = true;
            readback.requested = false;
        }
    }
};

} // namespace

GfxExecutor *Gfx_CreateGpuExecutor(void)
{
    return new Executor;
}
