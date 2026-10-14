// The renderer's front end: the gfx.h state machine, CPU copies of textures
// and vertex buffers, and the recording of each frame for the executor.

#include "platform/platform.h"
#include "port/sys.h"
#include "port/diagnostics.h"
#include "render/gfx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace {

GfxExecutor *s_executor;
GfxDrawState s_state;
bool s_stateDirty = true;
GfxFrame s_frame;
GfxReadback s_readback;
std::vector<GfxTexture *> s_textures;     // every live texture (not face views)
int s_width = 640, s_height = 480;
GfxTexture *s_target;
std::vector<std::pair<GfxDrawState, GfxTexture *>> s_savedStates;
std::vector<std::pair<int, int>> s_modes;

void ReleaseTextureCopies();

GfxMatrix Identity()
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

void ResetState()
{
    s_state = GfxDrawState();
    s_state.world = s_state.view = s_state.projection = Identity();
    s_state.texture[0] = s_state.texture[1] = Identity();
    memset(s_state.lights, 0, sizeof(s_state.lights));
    memset(&s_state.material, 0, sizeof(s_state.material));
    s_state.stages[1].colorOp = GFX_TOP_DISABLE;
    s_state.stages[1].alphaOp = GFX_TOP_DISABLE;
    s_state.stages[1].texCoordIndex = 1;
    s_state.viewport.x = s_state.viewport.y = 0;
    s_state.viewport.width = s_width;
    s_state.viewport.height = s_height;
    s_state.viewport.minZ = 0.0f;
    s_state.viewport.maxZ = 1.0f;
    s_stateDirty = true;
}

inline void Touch()
{
    s_stateDirty = true;
}

uint32_t RecordState()
{
    if (s_stateDirty || s_frame.states.empty()) {
        s_frame.states.push_back(s_state);
        s_stateDirty = false;
    }
    return (uint32_t)s_frame.states.size() - 1;
}

uint32_t AppendVertices(const void *data, size_t bytes)
{
    size_t offset = s_frame.vertices.size();
    s_frame.vertices.resize(offset + bytes);
    memcpy(s_frame.vertices.data() + offset, data, bytes);
    return (uint32_t)offset;
}

// OPENCMR2_GFX_TRACE=file: one line per draw (primitive, counts, a hash of the
// vertices, indices and bound texture sizes, and the calling game function),
// "F n" per presented frame. Builds for different targets must write the same
// lines; diffing them finds drawing that differs (tools: addr2line on caller).
static FILE *TraceFile()
{
    static FILE *file = getenv("OPENCMR2_GFX_TRACE") ? fopen(getenv("OPENCMR2_GFX_TRACE"), "w") : NULL;
    return file;
}
static const void *s_traceCaller;
static unsigned s_traceFrame;

static uint32_t TraceHash(uint32_t h, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

// Records a draw; fans become lists (SDL_GPU has no fans).
void RecordDraw(int primitive, bool transformed, const void *vertices, DWORD vertexCount, const WORD *indices,
                DWORD indexCount)
{
    if (FILE *trace = TraceFile()) {
        uint32_t h = TraceHash(2166136261u, vertices, vertexCount * (transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex)));
        if (indices != NULL)
            h = TraceHash(h, indices, indexCount * sizeof(WORD));
        for (int k = 0; k < 2; k++) {
            const GfxTexture *t = s_state.stages[k].texture;
            int size[2] = {t ? t->width : 0, t ? t->height : 0};
            h = TraceHash(h, size, sizeof(size));
        }
        fprintf(trace, "%d %d %u %u %08x %p\n", primitive, transformed ? 1 : 0, (unsigned)vertexCount,
                (unsigned)indexCount, h, s_traceCaller);
        static int dumpFrame = getenv("OPENCMR2_GFX_TRACE_FRAME") ? atoi(getenv("OPENCMR2_GFX_TRACE_FRAME")) : -1;
        if ((int)s_traceFrame == dumpFrame && transformed)
            for (DWORD v = 0; v < vertexCount; v++) {
                const GfxTLVertex *t = (const GfxTLVertex *)vertices + v;
                fprintf(trace, "  v %g %g %g %g c=%08x s=%08x uv=%g,%g tex=%dx%d\n", t->sx, t->sy, t->sz, t->rhw, t->color, t->specular, t->tu, t->tv,
                        s_state.stages[0].texture ? s_state.stages[0].texture->width : 0, s_state.stages[0].texture ? s_state.stages[0].texture->height : 0);
            }
    }
    if (vertexCount == 0 || s_executor == NULL)
        return;
    std::vector<uint16_t> fan;
    if (primitive == GFX_TRIANGLEFAN) {
        DWORD n = indices ? indexCount : vertexCount;
        for (DWORD i = 1; i + 1 < n; i++) {
            fan.push_back(indices ? indices[0] : 0);
            fan.push_back(indices ? indices[i] : (uint16_t)i);
            fan.push_back(indices ? indices[i + 1] : (uint16_t)(i + 1));
        }
        primitive = GFX_TRIANGLELIST;
        indices = fan.data();
        indexCount = (DWORD)fan.size();
        if (indexCount == 0)
            return;
    }

    GfxCommand cmd = {};
    cmd.type = GFX_CMD_DRAW;
    cmd.primitive = primitive;
    cmd.transformed = transformed;
    cmd.vertexCount = vertexCount;
    cmd.firstVertexByte = AppendVertices(vertices, vertexCount * (transformed ? sizeof(GfxTLVertex) : sizeof(GfxVertex)));
    if (indices != NULL) {
        cmd.firstIndex = (uint32_t)s_frame.indices.size();
        cmd.indexCount = indexCount;
        s_frame.indices.insert(s_frame.indices.end(), indices, indices + indexCount);
    } else {
        cmd.firstIndex = ~0u;
        cmd.indexCount = 0;
    }
    cmd.state = RecordState();
    s_frame.commands.push_back(cmd);
}

GfxStageState *Stage(int stage)
{
    return stage >= 0 && stage < 2 ? &s_state.stages[stage] : NULL;
}

GfxTexture *Root(GfxTexture *t)
{
    return t != NULL && t->cube != NULL ? t->cube : t;
}

// The desktop size, landscape: the game always runs that way (on Android the
// display can still report its natural portrait size).
void LandscapeDesktopSize(int *width, int *height)
{
    Sys_GetDesktopSize(width, height);
    if (*height > *width)
        std::swap(*width, *height);
}

// Resolutions at the display's aspect ratio, from 540 lines up to the display
// itself, and the original 4:3 ones up to 1024x768. At most 10 (the game's
// list), smallest first.
void BuildModes()
{
    static const int classic[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    static const int heights[] = { 540, 720, 900, 1080, 1440, 2160 };
    int desktopW, desktopH;

    s_modes.clear();
    LandscapeDesktopSize(&desktopW, &desktopH);
    auto add = [&](int w, int h) {
        if (w <= desktopW && h <= desktopH &&
            std::find(s_modes.begin(), s_modes.end(), std::make_pair(w, h)) == s_modes.end())
            s_modes.push_back({ w, h });
    };
    for (const auto &c : classic)
        add(c[0], c[1]);
    for (int h : heights)
        add((int)((long long)h * desktopW / desktopH) & ~1, h);
    add(desktopW, desktopH);
    std::sort(s_modes.begin(), s_modes.end());
    while (s_modes.size() > 10)
        s_modes.erase(s_modes.begin() + 1);
}

} // namespace

// ---- formats -------------------------------------------------------------------------

int Gfx_BytesPerPixel(int format)
{
    switch (format) {
    case GFX_FORMAT_BGRX8:
    case GFX_FORMAT_BGRA8:
        return 4;
    case GFX_FORMAT_BUMP_DUDVL:
        return 3;
    default:
        return 0;
    }
}

size_t Gfx_LevelSize(int format, int width, int height, LONG *pitch)
{
    if (format == GFX_FORMAT_BC1 || format == GFX_FORMAT_BC3) {
        int blockBytes = format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (width + 3) / 4, bh = (height + 3) / 4;
        if (pitch)
            *pitch = bw * blockBytes;
        return (size_t)bw * bh * blockBytes;
    }
    int bpp = Gfx_BytesPerPixel(format);
    if (pitch)
        *pitch = width * bpp;
    return (size_t)width * height * bpp;
}

extern "C" void Gfx_GetPixelFormat(int format, GfxPixelFormat *out)
{
    memset(out, 0, sizeof(*out));
    switch (format) {
    case GFX_FORMAT_BGRX8:
        out->flags = GFX_PF_RGB;
        out->bitsPerPixel = 32;
        out->redMask = 0x00ff0000;
        out->greenMask = 0x0000ff00;
        out->blueMask = 0x000000ff;
        break;
    case GFX_FORMAT_BGRA8:
        out->flags = GFX_PF_RGB | GFX_PF_ALPHA;
        out->bitsPerPixel = 32;
        out->redMask = 0x00ff0000;
        out->greenMask = 0x0000ff00;
        out->blueMask = 0x000000ff;
        out->alphaMask = 0xff000000;
        break;
    case GFX_FORMAT_BC1:
        out->flags = GFX_PF_FOURCC;
        out->fourCC = MAKEFOURCC('D', 'X', 'T', '1');
        break;
    case GFX_FORMAT_BC3:
        out->flags = GFX_PF_FOURCC;
        out->fourCC = MAKEFOURCC('D', 'X', 'T', '5');
        break;
    case GFX_FORMAT_BUMP_DUDVL:
        out->flags = GFX_PF_BUMP;
        out->bitsPerPixel = 24;
        out->redMask = 0x0000ff;
        out->greenMask = 0x00ff00;
        out->blueMask = 0xff0000;
        break;
    }
}

// ---- device and frame ----------------------------------------------------------------

extern "C" BOOL Gfx_SetVideoMode(int width, int height, BOOL fullscreen)
{
    if (s_executor == NULL) {
        const char *headless = SDL_getenv("OPENCMR2_HEADLESS");
        if ((headless != NULL && *headless != '0') || Platform_GetSettingInt("video.headless", 0))
            s_executor = Gfx_CreateNullExecutor();
        else
            s_executor = Gfx_CreateGpuExecutor();
    }
    s_width = width;
    s_height = height;
    if (!s_executor->SetVideoMode(width, height, fullscreen != 0)) {
        Sys_Log("gfx: could not set %dx%d", width, height);
        return FALSE;
    }
    ResetState();
    return TRUE;
}

extern "C" void Gfx_SetFullscreen(BOOL fullscreen)
{
    if (s_executor != NULL)
        s_executor->SetFullscreen(fullscreen != 0);
}

extern "C" void Gfx_Shutdown(void)
{
    while (!s_textures.empty())
        Gfx_DestroyTexture(s_textures.back());
    delete s_executor;
    s_executor = NULL;
}

extern "C" int Gfx_GetDisplayModeCount(void)
{
    if (s_modes.empty())
        BuildModes();
    return (int)s_modes.size();
}

extern "C" void Gfx_GetDefaultDisplayMode(int *width, int *height)
{
    // The display's aspect ratio, at a height where the game's 2D (menus,
    // text, HUD), drawn in pixels for 640x480, stays readable: 720 lines, as
    // the game was played at 1024x768, and 540 on Android's small screens.
    // Higher modes stay available for a sharper 3D picture.
#ifdef __ANDROID__
    const int lines = 540;
#else
    const int lines = 720;
#endif
    int desktopW, desktopH;
    LandscapeDesktopSize(&desktopW, &desktopH);
    *width = desktopW;
    *height = desktopH;
    if (desktopH > lines) {
        *width = (int)((long long)lines * desktopW / desktopH) & ~1;
        *height = lines;
    }
}

extern "C" void Gfx_GetDisplayMode(int index, int *width, int *height)
{
    if (s_modes.empty())
        BuildModes();
    *width = s_modes[index].first;
    *height = s_modes[index].second;
}

extern "C" void Gfx_BeginScene(void)
{
}

extern "C" void Gfx_EndScene(void)
{
}

// video.dump_frame = n (or OPENCMR2_GFX_DUMP_FRAME): the state of every draw
// of the n-th presented frame in the log, to compare renderers.
static void DescribeTexture(const GfxTexture *t, char *out, size_t size)
{
    if (t == NULL) {
        snprintf(out, size, "-");
        return;
    }
    const GfxTexture *root = t->cube != NULL ? t->cube : t;
    snprintf(out, size, "%dx%d/f%d/fl%lx", root->width, root->height, root->format, (unsigned long)root->flags);
}

static void DumpFrame(const GfxFrame &frame)
{
    for (const GfxCommand &c : frame.commands) {
        if (c.type == GFX_CMD_TARGET) {
            Sys_Log("target %s", c.target != NULL ? "texture" : "screen");
            continue;
        }
        if (c.type == GFX_CMD_CLEAR) {
            Sys_Log("clear %lx %08lx %.2f", (unsigned long)c.clearFlags, (unsigned long)c.clearColor, c.clearZ);
            continue;
        }
        const GfxDrawState &s = frame.states[c.state];
        char t0[48], t1[48];
        DescribeTexture(s.stages[0].texture, t0, sizeof(t0));
        DescribeTexture(s.stages[1].texture, t1, sizeof(t1));
        int lights = 0;
        for (int i = 0; i < 8; i++)
            lights += s.lightEnabled[i];
        Sys_Log("draw %s p%d v%u i%u | s0 c%d(%x,%x) a%d(%x,%x) tci%lx tt%d %s | s1 c%d(%x,%x) a%d(%x,%x) tci%lx tt%d %s | "
                "blend %d %d,%d test %d,%d,%lu z %d%d cull %d | light %d cv%d src%d%d%d%d mat %.2f,%.2f,%.2f/%.2f,%.2f,%.2f "
                "amb %08lx L%d spec %d | tf %08lx fog %d,%d,%d %.0f-%.0f %08lx",
                c.transformed ? "TL" : "3D", c.primitive, c.vertexCount, c.indexCount, s.stages[0].colorOp,
                s.stages[0].colorArg1, s.stages[0].colorArg2, s.stages[0].alphaOp, s.stages[0].alphaArg1,
                s.stages[0].alphaArg2, (unsigned long)s.stages[0].texCoordIndex, s.stages[0].textureTransform, t0,
                s.stages[1].colorOp, s.stages[1].colorArg1, s.stages[1].colorArg2, s.stages[1].alphaOp,
                s.stages[1].alphaArg1, s.stages[1].alphaArg2, (unsigned long)s.stages[1].texCoordIndex,
                s.stages[1].textureTransform, t1, s.alphaBlend, s.srcBlend, s.destBlend, s.alphaTest, s.alphaFunc,
                (unsigned long)s.alphaRef, s.depthTest, s.depthWrite, s.cull, s.lighting, s.colorVertex,
                s.diffuseSource, s.specularSource, s.ambientSource, s.emissiveSource, s.material.diffuse.r,
                s.material.diffuse.g, s.material.diffuse.b, s.material.ambient.r, s.material.ambient.g,
                s.material.ambient.b, (unsigned long)s.ambient, lights, s.specular, (unsigned long)s.textureFactor,
                s.fog, s.fogVertexMode, s.fogTableMode, s.fogStart, s.fogEnd, (unsigned long)s.fogColor);
        if (!s.lighting || c.transformed)
            continue;
        Sys_Log("  power %.1f emissive %.2f,%.2f,%.2f specular %.2f,%.2f,%.2f normalize %d", s.material.power,
                s.material.emissive.r, s.material.emissive.g, s.material.emissive.b, s.material.specular.r,
                s.material.specular.g, s.material.specular.b, s.normalizeNormals);
        for (int i = 0; i < 8; i++)
            if (s.lightEnabled[i]) {
                const GfxLight &l = s.lights[i];
                Sys_Log("  light %d type %d diffuse %.2f,%.2f,%.2f specular %.2f,%.2f,%.2f ambient %.2f,%.2f,%.2f "
                        "dir %.2f,%.2f,%.2f pos %.1f,%.1f,%.1f range %.1f att %.3f,%.3f,%.3f",
                        i, l.type, l.diffuse.r, l.diffuse.g, l.diffuse.b, l.specular.r, l.specular.g, l.specular.b,
                        l.ambient.r, l.ambient.g, l.ambient.b, l.direction.x, l.direction.y, l.direction.z,
                        l.position.x, l.position.y, l.position.z, l.range, l.attenuation0, l.attenuation1,
                        l.attenuation2);
            }
    }
}

static unsigned s_presented;

static bool DumpingFrame()
{
    static int dumpFrame = getenv("OPENCMR2_GFX_DUMP_FRAME") ? atoi(getenv("OPENCMR2_GFX_DUMP_FRAME"))
                                                             : Platform_GetSettingInt("video.dump_frame", -1);
    return (int)s_presented + 1 == dumpFrame;
}

static void Present(BOOL vsync, bool stretch)
{
    if (DumpingFrame()) {
        Sys_Log("frame %u:", s_presented + 1);
        DumpFrame(s_frame);
    }
    s_presented++;
    if (FILE *trace = TraceFile())
        fprintf(trace, "F %u\n", s_traceFrame++);
    if (Diagnostics::observer)
        Diagnostics::observer->BeginPresent();
    ReleaseTextureCopies();
    if (s_executor != NULL)
        s_executor->Execute(s_frame, s_textures, s_readback, true, vsync != 0, stretch);
    s_frame.Reset();
    s_stateDirty = true;
    // A frame always starts on the back buffer.
    s_target = NULL;
    if (Diagnostics::observer)
        Diagnostics::observer->EndPresent();
}

extern "C" void Gfx_Present(BOOL vsync) { Present(vsync, false); }
extern "C" void Gfx_PresentStretched(BOOL vsync) { Present(vsync, true); }

// Runs what is recorded so far without ending the frame.
static void Flush(void)
{
    if (DumpingFrame()) {
        Sys_Log("frame %u, flushed:", s_presented + 1);
        DumpFrame(s_frame);
    }
    if (s_executor != NULL)
        s_executor->Execute(s_frame, s_textures, s_readback, false, false);
    s_frame.Reset();
    s_stateDirty = true;
    if (s_target != NULL)
        Gfx_SetRenderTarget(s_target);
}

extern "C" void Gfx_Clear(DWORD flags, DWORD color, float z)
{
    GfxCommand cmd = {};
    cmd.type = GFX_CMD_CLEAR;
    cmd.clearFlags = flags;
    cmd.clearColor = color;
    cmd.clearZ = z;
    cmd.clearViewport = s_state.viewport;
    s_frame.commands.push_back(cmd);
}

extern "C" void Gfx_SetViewport(const GfxViewport *viewport)
{
    s_state.viewport = *viewport;
    Touch();
}

extern "C" void Gfx_GetViewport(GfxViewport *viewport)
{
    *viewport = s_state.viewport;
}

extern "C" void Gfx_PushState(void)
{
    s_savedStates.push_back({s_state, s_target});
}

extern "C" void Gfx_PopState(void)
{
    if (s_savedStates.empty())
        return;
    const auto saved = s_savedStates.back();
    s_savedStates.pop_back();
    s_state = saved.first;
    Touch();
    if (s_target != saved.second)
        Gfx_SetRenderTarget(saved.second);
}

extern "C" void Gfx_SetRenderTarget(GfxTexture *target)
{
    GfxCommand cmd = {};
    cmd.type = GFX_CMD_TARGET;
    cmd.target = target;
    s_frame.commands.push_back(cmd);
    s_target = target;
    if (target != NULL)
        Root(target)->renderedTo = true;
}

// ---- fixed-function state --------------------------------------------------------------

extern "C" void Gfx_SetTransform(int type, const GfxMatrix *matrix)
{
    switch (type) {
    case GFX_TRANSFORM_WORLD: s_state.world = *matrix; break;
    case GFX_TRANSFORM_VIEW: s_state.view = *matrix; break;
    case GFX_TRANSFORM_PROJECTION: s_state.projection = *matrix; break;
    case GFX_TRANSFORM_TEXTURE0: s_state.texture[0] = *matrix; break;
    case GFX_TRANSFORM_TEXTURE1: s_state.texture[1] = *matrix; break;
    }
    Touch();
}

extern "C" void Gfx_GetTransform(int type, GfxMatrix *matrix)
{
    switch (type) {
    case GFX_TRANSFORM_WORLD: *matrix = s_state.world; break;
    case GFX_TRANSFORM_VIEW: *matrix = s_state.view; break;
    case GFX_TRANSFORM_PROJECTION: *matrix = s_state.projection; break;
    case GFX_TRANSFORM_TEXTURE0: *matrix = s_state.texture[0]; break;
    case GFX_TRANSFORM_TEXTURE1: *matrix = s_state.texture[1]; break;
    default: *matrix = Identity(); break;
    }
}

extern "C" void Gfx_SetLight(int index, const GfxLight *light)
{
    if (index >= 0 && index < 8) {
        s_state.lights[index] = *light;
        Touch();
    }
}

extern "C" void Gfx_GetLight(int index, GfxLight *light)
{
    if (index >= 0 && index < 8)
        *light = s_state.lights[index];
}

extern "C" void Gfx_EnableLight(int index, BOOL enable)
{
    if (index >= 0 && index < 8) {
        s_state.lightEnabled[index] = enable != 0;
        Touch();
    }
}

extern "C" void Gfx_SetMaterial(const GfxMaterial *material)
{
    s_state.material = *material;
    Touch();
}

#define SETTER(name, type, field)       \
    extern "C" void name(type value)    \
    {                                   \
        s_state.field = value;          \
        Touch();                        \
    }

SETTER(Gfx_SetAmbient, DWORD, ambient)
SETTER(Gfx_SetDiffuseMaterialSource, int, diffuseSource)
SETTER(Gfx_SetSpecularMaterialSource, int, specularSource)
SETTER(Gfx_SetAmbientMaterialSource, int, ambientSource)
SETTER(Gfx_SetEmissiveMaterialSource, int, emissiveSource)
SETTER(Gfx_SetFogColor, DWORD, fogColor)
SETTER(Gfx_SetFogVertexMode, int, fogVertexMode)
SETTER(Gfx_SetFogTableMode, int, fogTableMode)
SETTER(Gfx_SetFogStart, float, fogStart)
SETTER(Gfx_SetFogEnd, float, fogEnd)
SETTER(Gfx_SetDepthFunc, int, depthFunc)
SETTER(Gfx_SetCullMode, int, cull)
SETTER(Gfx_SetSrcBlend, int, srcBlend)
SETTER(Gfx_SetDestBlend, int, destBlend)
SETTER(Gfx_SetAlphaFunc, int, alphaFunc)
SETTER(Gfx_SetAlphaRef, DWORD, alphaRef)
SETTER(Gfx_SetTextureFactor, DWORD, textureFactor)

#define BOOL_SETTER(name, field)        \
    extern "C" void name(BOOL value)    \
    {                                   \
        s_state.field = value != 0;     \
        Touch();                        \
    }

BOOL_SETTER(Gfx_SetLighting, lighting)
BOOL_SETTER(Gfx_SetSpecular, specular)
BOOL_SETTER(Gfx_SetLocalViewer, localViewer)
BOOL_SETTER(Gfx_SetNormalizeNormals, normalizeNormals)
BOOL_SETTER(Gfx_SetColorVertex, colorVertex)
BOOL_SETTER(Gfx_SetFog, fog)
BOOL_SETTER(Gfx_SetDepthTest, depthTest)
BOOL_SETTER(Gfx_SetDepthWrite, depthWrite)
BOOL_SETTER(Gfx_SetAlphaBlend, alphaBlend)
BOOL_SETTER(Gfx_SetAlphaTest, alphaTest)

#define STAGE_SETTER(name, type, field)             \
    extern "C" void name(int stage, type value)     \
    {                                               \
        GfxStageState *s = Stage(stage);            \
        if (s != NULL) {                            \
            s->field = value;                       \
            Touch();                                \
        }                                           \
    }

STAGE_SETTER(Gfx_SetTexture, GfxTexture *, texture)
STAGE_SETTER(Gfx_SetStageColorOp, int, colorOp)
STAGE_SETTER(Gfx_SetStageColorArg1, int, colorArg1)
STAGE_SETTER(Gfx_SetStageColorArg2, int, colorArg2)
STAGE_SETTER(Gfx_SetStageAlphaOp, int, alphaOp)
STAGE_SETTER(Gfx_SetStageAlphaArg1, int, alphaArg1)
STAGE_SETTER(Gfx_SetStageAlphaArg2, int, alphaArg2)
STAGE_SETTER(Gfx_SetStageAddress, int, address)
STAGE_SETTER(Gfx_SetStageMinFilter, int, minFilter)
STAGE_SETTER(Gfx_SetStageMagFilter, int, magFilter)
STAGE_SETTER(Gfx_SetStageMipFilter, int, mipFilter)
STAGE_SETTER(Gfx_SetStageMaxMipLevel, int, maxMipLevel)
STAGE_SETTER(Gfx_SetStageTexCoordIndex, DWORD, texCoordIndex)
STAGE_SETTER(Gfx_SetStageTextureTransform, int, textureTransform)
STAGE_SETTER(Gfx_SetStageBumpEnvLScale, float, bumpLumScale)
STAGE_SETTER(Gfx_SetStageBumpEnvLOffset, float, bumpLumOffset)

extern "C" void Gfx_SetStageBumpEnvMat(int stage, int index, float value)
{
    GfxStageState *s = Stage(stage);
    if (s != NULL && index >= 0 && index < 4) {
        s->bumpMat[index] = value;
        Touch();
    }
}

// ---- geometry ----------------------------------------------------------------------

extern "C" GfxVertexBuffer *Gfx_CreateVertexBuffer(DWORD vertexCount)
{
    // The memory comes with the first lock: the game creates many buffers it
    // may never fill (draws from an unfilled one are skipped below).
    GfxVertexBuffer *buffer = new GfxVertexBuffer;
    buffer->count = vertexCount;
    return buffer;
}

extern "C" void Gfx_DestroyVertexBuffer(GfxVertexBuffer *buffer)
{
    delete buffer;
}

extern "C" GfxVertex *Gfx_LockVertexBuffer(GfxVertexBuffer *buffer)
{
    if (buffer == NULL)
        return NULL;
    if (buffer->vertices.size() != buffer->count)
        buffer->vertices.assign(buffer->count, GfxVertex());
    return buffer->vertices.data();
}

extern "C" void Gfx_UnlockVertexBuffer(GfxVertexBuffer *)
{
}

extern "C" void Gfx_DrawPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    if (buffer == NULL || firstVertex + vertexCount > buffer->vertices.size())
        return;
    RecordDraw(primitive, false, buffer->vertices.data() + firstVertex, vertexCount, NULL, 0);
}

extern "C" void Gfx_DrawIndexedPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount,
                                           const WORD *indices, DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    if (buffer == NULL || firstVertex + vertexCount > buffer->vertices.size())
        return;
    RecordDraw(primitive, false, buffer->vertices.data() + firstVertex, vertexCount, indices, indexCount);
}

extern "C" void Gfx_DrawPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, false, vertices, vertexCount, NULL, 0);
}

extern "C" void Gfx_DrawIndexedPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount, const WORD *indices,
                                         DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, false, vertices, vertexCount, indices, indexCount);
}

extern "C" void Gfx_DrawPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, true, vertices, vertexCount, NULL, 0);
}

extern "C" void Gfx_DrawIndexedPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount,
                                           const WORD *indices, DWORD indexCount)
{
    s_traceCaller = __builtin_return_address(0);
    RecordDraw(primitive, true, vertices, vertexCount, indices, indexCount);
}

// ---- textures -------------------------------------------------------------------------

namespace {

// Hands finished textures to an executor that takes them (Upload, the 3DS's)
// and releases their CPU copies. A texture is finished when it is no longer
// locked and the game has moved on: another texture is created or a frame is
// shown. Staging, dynamic and render-target textures keep their copies.
void ReleaseTextureCopies()
{
    if (s_executor == NULL)
        return;
    for (GfxTexture *t : s_textures) {
        if (!t->dirty || t->released || t->keepCopy || t->locks > 0 ||
            (t->flags & (GFX_TEXTURE_STAGING | GFX_TEXTURE_DYNAMIC | GFX_TEXTURE_RENDER_TARGET | GFX_TEXTURE_CUBE)))
            continue;
        if (!s_executor->Upload(t))
            continue;
        for (int m = 0; m < t->mipLevels; m++)
            std::vector<uint8_t>().swap(t->levels[0][m]);
        t->released = true;
        t->dirty = false;
    }
}

// Blank levels for a texture whose CPU copy was released.
void AllocateLevels(GfxTexture *t)
{
    for (int m = 0; m < t->mipLevels; m++) {
        int w = std::max(1, t->width >> m), h = std::max(1, t->height >> m);
        t->levels[0][m].assign(Gfx_LevelSize(t->format, w, h, NULL), 0);
    }
    t->released = false;
    // Changed again and again (a texture the game draws into): keep it.
    if (++t->restores >= 3)
        t->keepCopy = true;
}

// Writes BGRA pixels of the texture's size into its top level, in its format.
void StorePixels(GfxTexture *t, const std::vector<uint32_t> &pixels)
{
    uint8_t *out = t->levels[0][0].data();
    size_t n = (size_t)t->width * t->height;
    switch (t->format) {
    case GFX_FORMAT_BGRX8:
    case GFX_FORMAT_BGRA8:
        memcpy(out, pixels.data(), n * 4);
        break;
    case GFX_FORMAT_BUMP_DUDVL:
        for (size_t i = 0; i < n; i++) {
            out[i * 3] = (uint8_t)pixels[i];
            out[i * 3 + 1] = (uint8_t)(pixels[i] >> 8);
            out[i * 3 + 2] = (uint8_t)(pixels[i] >> 16);
        }
        break;
    case GFX_FORMAT_BC1:
    case GFX_FORMAT_BC3: {
        int blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (t->width + 3) / 4, bh = (t->height + 3) / 4;
        uint32_t block[16];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int px = std::min(t->width - 1, bx * 4 + x), py = std::min(t->height - 1, by * 4 + y);
                        block[y * 4 + x] = pixels[(size_t)py * t->width + px];
                    }
                Gfx_EncodeBlock(t->format, block, out + (by * bw + bx) * blockBytes);
            }
        break;
    }
    }
}

// Gives a released texture its CPU copy back, from the executor's smaller
// copy of the top level; the lower levels are rebuilt from it.
bool RestoreTexture(GfxTexture *t)
{
    std::vector<uint32_t> pixels;
    bool read = s_executor != NULL && s_executor->ReadPixels(t, pixels);
    AllocateLevels(t);
    if (!read) {
        Sys_Log("gfx: the %dx%d texture's contents are lost", t->width, t->height);
        return false;
    }
    StorePixels(t, pixels);
    return true;
}

} // namespace

extern "C" GfxTexture *Gfx_CreateTexture(int width, int height, int format, int mipLevels, DWORD flags)
{
    if (width <= 0 || height <= 0)
        return NULL;
    ReleaseTextureCopies();
    // Compressed and bump textures keep one level: the original's mip
    // generation (a DirectDraw stretch blit) only worked on plain RGB.
    if (format != GFX_FORMAT_BGRX8 && format != GFX_FORMAT_BGRA8)
        mipLevels = 1;
    if (flags & (GFX_TEXTURE_RENDER_TARGET | GFX_TEXTURE_CUBE))
        mipLevels = 1;
    if (mipLevels < 1)
        mipLevels = 1;
    if (mipLevels > 16)
        mipLevels = 16;

    GfxTexture *t = new GfxTexture();
    t->width = width;
    t->height = height;
    t->format = format;
    t->mipLevels = mipLevels;
    t->flags = flags;
    t->cube = NULL;
    t->face = 0;
    t->dirty = true;
    t->renderedTo = false;
    t->gpu = NULL;
    int faces = (flags & GFX_TEXTURE_CUBE) ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        for (int m = 0; m < mipLevels; m++) {
            int w = std::max(1, width >> m), h = std::max(1, height >> m);
            t->levels[f][m].assign(Gfx_LevelSize(format, w, h, NULL), 0);
        }
    }
    for (int f = 0; f < 6; f++)
        t->faceViews[f] = NULL;
    if (flags & GFX_TEXTURE_CUBE) {
        t->faceViews[0] = t;
        for (int f = 1; f < 6; f++) {
            GfxTexture *v = new GfxTexture();
            v->width = width;
            v->height = height;
            v->format = format;
            v->mipLevels = 1;
            v->flags = flags;
            v->cube = t;
            v->face = f;
            v->gpu = NULL;
            t->faceViews[f] = v;
        }
    }
    s_textures.push_back(t);
    return t;
}

extern "C" void Gfx_DestroyTexture(GfxTexture *texture)
{
    if (texture == NULL || texture->cube != NULL)
        return;     // face views go with their cube
    for (int i = 0; i < 2; i++)
        if (Root(s_state.stages[i].texture) == texture)
            s_state.stages[i].texture = NULL;
    // Draws recorded this frame may still use it: flush them first.
    for (const GfxDrawState &state : s_frame.states)
        for (const GfxStageState &stage : state.stages)
            if (Root(stage.texture) == texture) {
                Flush();
                goto flushed;
            }
flushed:
    if (s_executor != NULL)
        s_executor->DestroyTexture(texture);
    s_textures.erase(std::remove(s_textures.begin(), s_textures.end(), texture), s_textures.end());
    for (int f = 1; f < 6; f++)
        delete texture->faceViews[f];
    delete texture;
}

extern "C" int Gfx_GetTextureMipCount(GfxTexture *texture)
{
    return texture != NULL ? Root(texture)->mipLevels : 0;
}

extern "C" GfxTexture *Gfx_GetCubeFace(GfxTexture *cube, int face)
{
    if (cube == NULL || face < 0 || face > 5)
        return NULL;
    cube = Root(cube);
    return cube->faceViews[face] != NULL ? cube->faceViews[face] : cube;
}

extern "C" BOOL Gfx_LockTexture(GfxTexture *texture, int face, int mip, GfxLockedRect *out)
{
    if (texture == NULL)
        return FALSE;
    if (texture->cube != NULL) {
        face = texture->face;
        texture = texture->cube;
    }
    if (texture->released) {
        // The game changes a texture it loaded earlier (RemapTextureAlpha):
        // its copy went to the executor, get it back.
        if (RestoreTexture(texture))
            Gfx_GenerateMipmaps(texture);
    }
    if (mip < 0 || mip >= texture->mipLevels || face < 0 || face > 5 || texture->levels[face][mip].empty())
        return FALSE;
    if (texture->renderedTo && s_executor != NULL) {
        s_executor->ReadTexture(texture);
        texture->renderedTo = false;
    }
    int w = std::max(1, texture->width >> mip), h = std::max(1, texture->height >> mip);
    memset(out, 0, sizeof(*out));
    texture->locks++;
    out->pixels = texture->levels[face][mip].data();
    Gfx_LevelSize(texture->format, w, h, &out->pitch);
    out->width = w;
    out->height = h;
    Gfx_GetPixelFormat(texture->format, &out->format);
    return TRUE;
}

extern "C" void Gfx_UnlockTexture(GfxTexture *texture, int, int)
{
    if (texture != NULL) {
        texture = Root(texture);
        texture->dirty = true;
        if (texture->locks > 0)
            texture->locks--;
    }
}

namespace {

// The top level of a texture as BGRA pixels.
std::vector<uint32_t> ToBGRA(GfxTexture *t)
{
    std::vector<uint32_t> out((size_t)t->width * t->height);
    const uint8_t *src = t->levels[0][0].data();
    switch (t->format) {
    case GFX_FORMAT_BGRX8:
        for (size_t i = 0; i < out.size(); i++)
            out[i] = ((const uint32_t *)src)[i] | 0xff000000;
        break;
    case GFX_FORMAT_BGRA8:
        memcpy(out.data(), src, out.size() * 4);
        break;
    case GFX_FORMAT_BUMP_DUDVL:
        for (size_t i = 0; i < out.size(); i++)
            out[i] = src[i * 3] | src[i * 3 + 1] << 8 | src[i * 3 + 2] << 16 | 0xff000000;
        break;
    case GFX_FORMAT_BC1:
    case GFX_FORMAT_BC3: {
        int blockBytes = t->format == GFX_FORMAT_BC1 ? 8 : 16;
        int bw = (t->width + 3) / 4, bh = (t->height + 3) / 4;
        uint32_t block[16];
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                Gfx_DecodeBlock(t->format, src + (by * bw + bx) * blockBytes, block);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int px = bx * 4 + x, py = by * 4 + y;
                        if (px < t->width && py < t->height)
                            out[(size_t)py * t->width + px] = block[y * 4 + x];
                    }
            }
        }
        break;
    }
    }
    return out;
}

} // namespace

extern "C" void Gfx_CopyTexture(GfxTexture *dst, GfxTexture *src)
{
    dst = Root(dst);
    src = Root(src);
    if (dst == NULL || src == NULL)
        return;
    if (dst->released)
        AllocateLevels(dst);    // fully overwritten below
    if (src->renderedTo && s_executor != NULL) {
        s_executor->ReadTexture(src);
        src->renderedTo = false;
    }
    if (!src->released && dst->format == src->format && dst->width == src->width && dst->height == src->height) {
        dst->levels[0][0] = src->levels[0][0];
        dst->dirty = true;
        return;
    }
    // A released source: the executor's copy of it.
    std::vector<uint32_t> pixels;
    if (src->released) {
        if (s_executor == NULL || !s_executor->ReadPixels(src, pixels)) {
            Sys_Log("gfx: cannot copy the %dx%d texture, its contents are lost", src->width, src->height);
            return;
        }
    } else {
        pixels = ToBGRA(src);
    }
    if (dst->width != src->width || dst->height != src->height) {
        std::vector<uint32_t> scaled((size_t)dst->width * dst->height);
        for (int y = 0; y < dst->height; y++) {
            int sy = y * src->height / dst->height;
            for (int x = 0; x < dst->width; x++)
                scaled[(size_t)y * dst->width + x] = pixels[(size_t)sy * src->width + x * src->width / dst->width];
        }
        pixels.swap(scaled);
    }
    StorePixels(dst, pixels);
    dst->dirty = true;
}

extern "C" void Gfx_GenerateMipmaps(GfxTexture *texture)
{
    texture = Root(texture);
    if (texture == NULL || texture->released ||
        (texture->format != GFX_FORMAT_BGRX8 && texture->format != GFX_FORMAT_BGRA8))
        return;
    for (int m = 1; m < texture->mipLevels; m++) {
        int sw = std::max(1, texture->width >> (m - 1)), sh = std::max(1, texture->height >> (m - 1));
        int dw = std::max(1, texture->width >> m), dh = std::max(1, texture->height >> m);
        const uint8_t *src = texture->levels[0][m - 1].data();
        uint8_t *dst = texture->levels[0][m].data();
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                int sum[4] = { 0, 0, 0, 0 };
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = std::min(sw - 1, x * 2 + dx), sy = std::min(sh - 1, y * 2 + dy);
                        const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                        for (int c = 0; c < 4; c++)
                            sum[c] += p[c];
                    }
                uint8_t *q = dst + ((size_t)y * dw + x) * 4;
                for (int c = 0; c < 4; c++)
                    q[c] = (uint8_t)((sum[c] + 2) / 4);
            }
        }
    }
    texture->dirty = true;
}

extern "C" BOOL Gfx_ReadBackBuffer(int x, int y, int width, int height, DWORD *pixels)
{
    s_readback.requested = true;
    s_readback.x = x;
    s_readback.y = y;
    s_readback.width = width;
    s_readback.height = height;
    if (!s_readback.ready || s_readback.readyWidth != width || s_readback.readyHeight != height)
        return FALSE;
    memcpy(pixels, s_readback.pixels.data(), (size_t)width * height * 4);
    return TRUE;
}

// ---- BC decoding -----------------------------------------------------------------------

namespace {

uint32_t Rgb565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return b | g << 8 | r << 16;
}

uint32_t Mix(uint32_t a, uint32_t b, int wa, int wb, int div)
{
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        uint32_t ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        out |= ((ca * wa + cb * wb) / div) << s;
    }
    return out;
}

} // namespace

void Gfx_DecodeBlock(int format, const uint8_t *block, uint32_t out[16])
{
    const uint8_t *colour = format == GFX_FORMAT_BC3 ? block + 8 : block;
    uint16_t c0 = (uint16_t)(colour[0] | colour[1] << 8), c1 = (uint16_t)(colour[2] | colour[3] << 8);
    uint32_t palette[4];
    palette[0] = Rgb565(c0) | 0xff000000;
    palette[1] = Rgb565(c1) | 0xff000000;
    if (c0 > c1 || format == GFX_FORMAT_BC3) {
        palette[2] = Mix(palette[0], palette[1], 2, 1, 3) | 0xff000000;
        palette[3] = Mix(palette[0], palette[1], 1, 2, 3) | 0xff000000;
    } else {
        palette[2] = Mix(palette[0], palette[1], 1, 1, 2) | 0xff000000;
        palette[3] = 0;     // transparent black
    }
    uint32_t bits = colour[4] | colour[5] << 8 | colour[6] << 16 | (uint32_t)colour[7] << 24;
    for (int i = 0; i < 16; i++)
        out[i] = palette[(bits >> (i * 2)) & 3];

    if (format == GFX_FORMAT_BC3) {
        uint8_t a0 = block[0], a1 = block[1];
        uint8_t alphas[8];
        alphas[0] = a0;
        alphas[1] = a1;
        if (a0 > a1) {
            for (int i = 1; i < 7; i++)
                alphas[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        } else {
            for (int i = 1; i < 5; i++)
                alphas[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
            alphas[6] = 0;
            alphas[7] = 255;
        }
        uint64_t abits = 0;
        for (int i = 0; i < 6; i++)
            abits |= (uint64_t)block[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++)
            out[i] = (out[i] & 0x00ffffff) | (uint32_t)alphas[(abits >> (3 * i)) & 7] << 24;
    }
}

namespace {

uint16_t To565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xff) >> 3) << 11 | (((c >> 8) & 0xff) >> 2) << 5 | ((c & 0xff) >> 3));
}

int Distance(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);
        d += x * x;
    }
    return d;
}

} // namespace

void Gfx_EncodeBlock(int format, const uint32_t in[16], uint8_t *block)
{
    uint8_t *colour = format == GFX_FORMAT_BC3 ? block + 8 : block;
    // Endpoints: the two opaque pixels farthest apart.
    bool transparent = false;
    uint32_t lo = 0, hi = 0;
    int farthest = -1;
    for (int i = 0; i < 16; i++) {
        if (format == GFX_FORMAT_BC1 && (in[i] >> 24) < 128) {
            transparent = true;
            continue;
        }
        for (int j = i; j < 16; j++) {
            if (format == GFX_FORMAT_BC1 && (in[j] >> 24) < 128)
                continue;
            int d = Distance(in[i], in[j]);
            if (d > farthest) {
                farthest = d;
                hi = in[i];
                lo = in[j];
            }
        }
    }
    uint16_t c0 = To565(hi), c1 = To565(lo);
    // Four colours need c0 > c1, three (and transparency) c0 <= c1.
    if (transparent ? c0 > c1 : c0 < c1)
        std::swap(c0, c1);
    if (!transparent && c0 == c1 && c0 > 0)
        c1 = (uint16_t)(c0 - 1);
    colour[0] = (uint8_t)c0;
    colour[1] = (uint8_t)(c0 >> 8);
    colour[2] = (uint8_t)c1;
    colour[3] = (uint8_t)(c1 >> 8);
    // The palette as the decoder builds it: a block of the endpoints with
    // every index k, decoded.
    uint32_t palette[4];
    uint8_t probe[16] = {};
    memcpy(format == GFX_FORMAT_BC3 ? probe + 8 : probe, colour, 4);
    uint32_t bits = 0;
    for (int k = 0; k < 4; k++) {
        uint8_t *pb = format == GFX_FORMAT_BC3 ? probe + 12 : probe + 4;
        uint32_t all = 0;
        for (int i = 0; i < 16; i++)
            all |= (uint32_t)k << (i * 2);
        pb[0] = (uint8_t)all; pb[1] = (uint8_t)(all >> 8); pb[2] = (uint8_t)(all >> 16); pb[3] = (uint8_t)(all >> 24);
        uint32_t out[16];
        Gfx_DecodeBlock(format, probe, out);
        palette[k] = out[0];
    }
    for (int i = 0; i < 16; i++) {
        int best = 0, bestDistance = 1 << 30;
        for (int k = 0; k < 4; k++) {
            if (format == GFX_FORMAT_BC1 && transparent && k == 3)
                continue;
            int d = Distance(in[i], palette[k]);
            if (d < bestDistance) {
                bestDistance = d;
                best = k;
            }
        }
        if (format == GFX_FORMAT_BC1 && transparent && (in[i] >> 24) < 128)
            best = 3;
        bits |= (uint32_t)best << (i * 2);
    }
    colour[4] = (uint8_t)bits;
    colour[5] = (uint8_t)(bits >> 8);
    colour[6] = (uint8_t)(bits >> 16);
    colour[7] = (uint8_t)(bits >> 24);

    if (format == GFX_FORMAT_BC3) {
        // Alpha: eight steps between the extremes (a0 > a1).
        uint8_t a0 = 0, a1 = 255;
        for (int i = 0; i < 16; i++) {
            a0 = std::max(a0, (uint8_t)(in[i] >> 24));
            a1 = std::min(a1, (uint8_t)(in[i] >> 24));
        }
        if (a0 == a1) {
            if (a0 < 255) a0++; else a1--;
        }
        uint8_t alphas[8] = { a0, a1 };
        for (int i = 1; i < 7; i++)
            alphas[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        uint64_t abits = 0;
        for (int i = 0; i < 16; i++) {
            int a = (int)(in[i] >> 24), best = 0, bestDistance = 1 << 30;
            for (int k = 0; k < 8; k++) {
                int d = (a - alphas[k]) * (a - alphas[k]);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = k;
                }
            }
            abits |= (uint64_t)best << (3 * i);
        }
        block[0] = a0;
        block[1] = a1;
        for (int i = 0; i < 6; i++)
            block[2 + i] = (uint8_t)(abits >> (8 * i));
    }
}

// ---- the null executor (headless runs) ---------------------------------------------------

namespace {

struct NullExecutor : GfxExecutor {
    bool SetVideoMode(int, int, bool) override { return true; }
    void SetFullscreen(bool) override {}
    void Execute(GfxFrame &, std::vector<GfxTexture *> &textures, GfxReadback &readback, bool present, bool,
                 bool) override
    {
        for (GfxTexture *t : textures)
            t->dirty = false;
        if (present && readback.requested) {
            readback.pixels.assign((size_t)readback.width * readback.height, 0xff000000);
            readback.readyWidth = readback.width;
            readback.readyHeight = readback.height;
            readback.ready = true;
            readback.requested = false;
        }
    }
    void DestroyTexture(GfxTexture *) override {}
    void ReadTexture(GfxTexture *) override {}
};

} // namespace

GfxExecutor *Gfx_CreateNullExecutor(void)
{
    return new NullExecutor;
}
