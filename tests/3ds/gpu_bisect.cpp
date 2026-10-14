// Finds which drawing feature stops a console's GPU (render/gfx_c3d.cpp):
// draws one feature at a time, 30 frames each, from the safest up, and
// writes "phase N ok" to sdmc:/opencmr2-gpu.log after each phase's frames.
// The last line on the card names the last feature that worked; the next
// phase is the one the GPU does not take.

#include "platform/ctr.h"
#include "port/gfx.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

FILE *s_log;

void Note(const char *text)
{
    fprintf(s_log, "%s\n", text);
    fflush(s_log);
    fsync(fileno(s_log));
}

GfxMatrix Identity()
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

GfxMatrix Perspective()
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    m._11 = 1.3f;
    m._22 = 1.73f;
    m._33 = 100.0f / 99.0f;
    m._34 = 1.0f;
    m._43 = -100.0f / 99.0f;
    return m;
}

GfxTexture *Texture(int mips)
{
    GfxTexture *t = Gfx_CreateTexture(64, 64, GFX_FORMAT_BGRA8, mips, 0);
    GfxLockedRect rect;
    if (Gfx_LockTexture(t, 0, 0, &rect)) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                ((DWORD *)((BYTE *)rect.pixels + y * rect.pitch))[x] = ((x / 8 + y / 8) & 1) ? 0xffff8000 : 0xff0040ff;
        Gfx_UnlockTexture(t, 0, 0);
    }
    if (mips > 1)
        Gfx_GenerateMipmaps(t);
    return t;
}

GfxTLVertex TL(float x, float y, float u, float v)
{
    GfxTLVertex t = { x, y, 0.5f, 1.0f, 0xff40c040, 0, u, v };
    return t;
}

GfxVertex V(float x, float y, float z, float u, float v)
{
    GfxVertex t = { x, y, z, 0, 0, -1, 0xffc0c0c0, 0, u, v, 0, 0 };
    return t;
}

const char *s_phases[] = {
    "1 clear only",
    "2 2D untextured strip",
    "3 2D textured strip",
    "4 3D strip, no texture, no lighting (DrawArrays)",
    "5 3D indexed triangles, no texture, no lighting (DrawElements)",
    "6 3D indexed, textured",
    "7 3D indexed, textured with mipmaps",
    "8 3D indexed, lit",
    "9 3D indexed, fog",
    "10 3D indexed, alpha blending and alpha test",
    "11 everything together",
};

void Draw(int phase, GfxTexture *plain, GfxTexture *mipped)
{
    GfxViewport vp = { 0, 0, 640, 480, 0.0f, 1.0f };
    Gfx_SetViewport(&vp);
    Gfx_Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER, 0xff202040, 1.0f);
    Gfx_SetCullMode(GFX_CULL_NONE);
    Gfx_SetLighting(FALSE);
    Gfx_SetFog(FALSE);
    Gfx_SetAlphaBlend(FALSE);
    Gfx_SetAlphaTest(FALSE);
    Gfx_SetTexture(0, NULL);
    Gfx_SetStageColorOp(0, GFX_TOP_MODULATE);
    Gfx_SetStageAlphaOp(0, GFX_TOP_SELECTARG1);
    Gfx_SetStageAlphaArg1(0, GFX_TA_DIFFUSE);
    Gfx_SetStageMipFilter(0, GFX_MIPFILTER_NONE);
    if (phase == 1)
        return;
    if (phase <= 3) {
        Gfx_SetDepthTest(FALSE);
        if (phase == 3)
            Gfx_SetTexture(0, plain);
        GfxTLVertex q[4] = { TL(100, 100, 0, 0), TL(500, 100, 1, 0), TL(100, 380, 0, 1), TL(500, 380, 1, 1) };
        Gfx_DrawPrimitiveTL(GFX_TRIANGLESTRIP, q, 4);
        return;
    }
    GfxMatrix id = Identity(), proj = Perspective();
    Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &proj);
    Gfx_SetTransform(GFX_TRANSFORM_VIEW, &id);
    Gfx_SetTransform(GFX_TRANSFORM_WORLD, &id);
    Gfx_SetDepthTest(TRUE);
    Gfx_SetDepthWrite(TRUE);
    GfxVertex v[4] = { V(-2, -2, 5, 0, 1), V(2, -2, 5, 1, 1), V(-2, 2, 5, 0, 0), V(2, 2, 5, 1, 0) };
    if (phase == 4) {
        Gfx_DrawPrimitive(GFX_TRIANGLESTRIP, v, 4);
        return;
    }
    bool all = phase == 11;
    if (phase == 6 || all)
        Gfx_SetTexture(0, plain);
    if (phase == 7 || all) {
        Gfx_SetTexture(0, mipped);
        Gfx_SetStageMipFilter(0, GFX_MIPFILTER_LINEAR);
    }
    if (phase == 8 || all) {
        Gfx_SetLighting(TRUE);
        Gfx_SetAmbient(0xff404040);
        GfxMaterial m;
        memset(&m, 0, sizeof(m));
        m.diffuse.r = m.diffuse.g = m.diffuse.b = m.diffuse.a = 1.0f;
        m.ambient = m.diffuse;
        Gfx_SetMaterial(&m);
        GfxLight l;
        memset(&l, 0, sizeof(l));
        l.type = GFX_LIGHT_DIRECTIONAL;
        l.diffuse.r = l.diffuse.g = l.diffuse.b = 1.0f;
        l.direction.z = 1.0f;
        Gfx_SetLight(0, &l);
        Gfx_EnableLight(0, TRUE);
    }
    if (phase == 9 || all) {
        Gfx_SetFog(TRUE);
        Gfx_SetFogColor(0xff808080);
        Gfx_SetFogTableMode(GFX_FOG_LINEAR);
        Gfx_SetFogStart(2.0f);
        Gfx_SetFogEnd(10.0f);
    }
    if (phase == 10 || all) {
        Gfx_SetAlphaBlend(TRUE);
        Gfx_SetSrcBlend(GFX_BLEND_SRCALPHA);
        Gfx_SetDestBlend(GFX_BLEND_INVSRCALPHA);
        Gfx_SetAlphaTest(TRUE);
        Gfx_SetAlphaFunc(GFX_CMP_GREATER);
        Gfx_SetAlphaRef(8);
    }
    WORD idx[6] = { 0, 2, 1, 1, 2, 3 };
    Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, v, 4, idx, 6);
}

} // namespace

int main(int, char **)
{
    s_log = fopen("sdmc:/opencmr2-gpu.log", "w");
    Ctr_Init();
    Note("start");
    if (!Gfx_SetVideoMode(640, 480, TRUE)) {
        Note("Gfx_SetVideoMode failed");
        return 1;
    }
    GfxTexture *plain = Texture(1);
    GfxTexture *mipped = Texture(7);
    Note("textures made");
    const int phases = sizeof(s_phases) / sizeof(s_phases[0]);
    for (int phase = 1; phase <= phases && Ctr_Update(); phase++) {
        char text[128];
        snprintf(text, sizeof(text), "phase %s: drawing", s_phases[phase - 1]);
        Note(text);
        printf("%s\n", text);
        for (int frame = 0; frame < 30; frame++) {
            Draw(phase, plain, mipped);
            Gfx_Present(TRUE);
        }
        snprintf(text, sizeof(text), "phase %s: ok", s_phases[phase - 1]);
        Note(text);
    }
    Note("all phases ok");
    printf("All phases ok. START to exit.\n");
    while (Ctr_Update() && !(hidKeysDown() & KEY_START))
        gspWaitForVBlank();
    Gfx_Shutdown();
    Ctr_Shutdown();
    return 0;
}
