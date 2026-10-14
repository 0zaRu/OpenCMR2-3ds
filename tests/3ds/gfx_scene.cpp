// A test scene for the 3DS renderer (render/gfx_c3d.cpp), drawn through
// port/gfx.h as the game draws: run opencmr2_gfx_scene.3dsx on a console or
// an emulator and compare with the description in docs/3DS.md.
//
//  - background: clear to blue; the picture is 4:3 in the middle of the
//    top screen;
//  - top left: a 2D quad with a texture whose corners are red (top left),
//    green (top right), blue (bottom left) and white (bottom right);
//  - over it, a half-transparent yellow quad (alpha blending);
//  - top right: two triangles, the left one clockwise (shown), the right
//    one counter-clockwise (culled);
//  - middle: a white cube lit from the upper left: its front face at 0.52
//    (133 of 255: ambient 0.19 + diffuse 0.33), turning while L is held;
//  - bottom: a checkered ground fading into grey fog with distance.

#include "platform/ctr.h"
#include "port/gfx.h"

#include <3ds.h>

#include <math.h>
#include <string.h>

namespace {

GfxMatrix Identity()
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

// Direct3D's left-handed perspective (D3DXMatrixPerspectiveFovLH).
GfxMatrix Perspective(float fovY, float aspect, float zn, float zf)
{
    GfxMatrix m;
    memset(&m, 0, sizeof(m));
    float h = 1.0f / tanf(fovY / 2), w = h / aspect;
    m._11 = w;
    m._22 = h;
    m._33 = zf / (zf - zn);
    m._34 = 1.0f;
    m._43 = -zn * zf / (zf - zn);
    return m;
}

GfxTexture *MakeTexture(int size, DWORD (*colour)(int x, int y, int size), int mips)
{
    GfxTexture *t = Gfx_CreateTexture(size, size, GFX_FORMAT_BGRA8, mips, 0);
    GfxLockedRect rect;
    if (Gfx_LockTexture(t, 0, 0, &rect)) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                ((DWORD *)((BYTE *)rect.pixels + y * rect.pitch))[x] = colour(x, y, size);
        Gfx_UnlockTexture(t, 0, 0);
    }
    if (mips > 1)
        Gfx_GenerateMipmaps(t);
    return t;
}

DWORD Corners(int x, int y, int size)
{
    bool right = x >= size / 2, bottom = y >= size / 2;
    if (!right && !bottom)
        return 0xffff0000;
    if (right && !bottom)
        return 0xff00ff00;
    if (!right && bottom)
        return 0xff0000ff;
    return 0xffffffff;
}

DWORD Checker(int x, int y, int size)
{
    return ((x / (size / 8) + y / (size / 8)) & 1) ? 0xffc0c0c0 : 0xff404040;
}

GfxTLVertex TL(float x, float y, DWORD colour, float u, float v)
{
    GfxTLVertex t = { x, y, 0.0f, 1.0f, colour, 0, u, v };
    return t;
}

GfxVertex V(float x, float y, float z, float nx, float ny, float nz, float u, float v)
{
    GfxVertex t = { x, y, z, nx, ny, nz, 0xffffffff, 0, u, v, 0, 0 };
    return t;
}

void DrawScene(GfxTexture *corners, GfxTexture *checker, float angle)
{
    Gfx_SetRenderTarget(NULL);
    GfxViewport vp = { 0, 0, 640, 480, 0.0f, 1.0f };
    Gfx_SetViewport(&vp);
    Gfx_Clear(GFX_CLEAR_TARGET | GFX_CLEAR_ZBUFFER, 0xff2040a0, 1.0f);

    GfxMatrix id = Identity(), proj = Perspective(1.0f, 4.0f / 3.0f, 1.0f, 100.0f);
    Gfx_SetTransform(GFX_TRANSFORM_PROJECTION, &proj);
    Gfx_SetTransform(GFX_TRANSFORM_VIEW, &id);
    Gfx_SetDepthTest(TRUE);
    Gfx_SetDepthWrite(TRUE);
    Gfx_SetCullMode(GFX_CULL_CCW);

    // Ground: z 3..60 at y = -2, checkered, with linear fog 5..40.
    Gfx_SetTransform(GFX_TRANSFORM_WORLD, &id);
    Gfx_SetLighting(FALSE);
    Gfx_SetFog(TRUE);
    Gfx_SetFogColor(0xff808080);
    Gfx_SetFogTableMode(GFX_FOG_LINEAR);
    Gfx_SetFogStart(5.0f);
    Gfx_SetFogEnd(40.0f);
    Gfx_SetTexture(0, checker);
    Gfx_SetStageColorOp(0, GFX_TOP_MODULATE);
    Gfx_SetStageMinFilter(0, GFX_FILTER_LINEAR);
    Gfx_SetStageMagFilter(0, GFX_FILTER_LINEAR);
    Gfx_SetStageMipFilter(0, GFX_MIPFILTER_LINEAR);
    GfxVertex ground[4] = { V(-20, -2, 3, 0, 1, 0, 0, 0), V(20, -2, 3, 0, 1, 0, 8, 0),
                            V(-20, -2, 60, 0, 1, 0, 0, 12), V(20, -2, 60, 0, 1, 0, 8, 12) };
    // Clockwise seen from above the ground (Direct3D's front faces).
    WORD groundIdx[6] = { 0, 2, 1, 1, 2, 3 };
    Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, ground, 4, groundIdx, 6);
    Gfx_SetFog(FALSE);

    // Cube at z = 6, lit by a directional light from the upper left.
    GfxMatrix world = Identity();
    float c = cosf(angle), s = sinf(angle);
    world._11 = c;
    world._13 = -s;
    world._31 = s;
    world._33 = c;
    world._41 = 0.0f;
    world._42 = 0.0f;
    world._43 = 6.0f;
    Gfx_SetTransform(GFX_TRANSFORM_WORLD, &world);
    Gfx_SetTexture(0, NULL);
    Gfx_SetLighting(TRUE);
    Gfx_SetAmbient(0xff303030);
    GfxMaterial material;
    memset(&material, 0, sizeof(material));
    material.diffuse.r = material.diffuse.g = material.diffuse.b = material.diffuse.a = 1.0f;
    material.ambient = material.diffuse;
    Gfx_SetMaterial(&material);
    Gfx_SetColorVertex(FALSE);
    GfxLight light;
    memset(&light, 0, sizeof(light));
    light.type = GFX_LIGHT_DIRECTIONAL;
    light.diffuse.r = light.diffuse.g = light.diffuse.b = 1.0f;
    light.direction.x = 1.0f;
    light.direction.y = -1.0f;
    light.direction.z = 0.5f;
    Gfx_SetLight(0, &light);
    Gfx_EnableLight(0, TRUE);
    GfxVertex cube[24];
    WORD cubeIdx[36];
    static const float faces[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; f++) {
        float n[3] = { faces[f][0], faces[f][1], faces[f][2] };
        // Two axes across the face, chosen so that (u, v, n) is left-handed.
        float a[3] = { n[1] + n[2], 0, -n[0] }, b[3];
        if (n[1] != 0) {
            a[0] = 1; a[1] = 0; a[2] = 0;
        }
        b[0] = n[1] * a[2] - n[2] * a[1];
        b[1] = n[2] * a[0] - n[0] * a[2];
        b[2] = n[0] * a[1] - n[1] * a[0];
        for (int i = 0; i < 4; i++) {
            float su = (i & 1) ? 1.0f : -1.0f, sv = (i & 2) ? 1.0f : -1.0f;
            cube[f * 4 + i] = V(n[0] + a[0] * su + b[0] * sv, n[1] + a[1] * su + b[1] * sv, n[2] + a[2] * su + b[2] * sv,
                                n[0], n[1], n[2], 0, 0);
        }
        WORD base = (WORD)(f * 4);
        WORD idx[6] = { base, (WORD)(base + 1), (WORD)(base + 2), (WORD)(base + 2), (WORD)(base + 1), (WORD)(base + 3) };
        memcpy(cubeIdx + f * 6, idx, sizeof(idx));
    }
    Gfx_SetCullMode(GFX_CULL_NONE);
    Gfx_DrawIndexedPrimitive(GFX_TRIANGLELIST, cube, 24, cubeIdx, 36);
    Gfx_SetCullMode(GFX_CULL_CCW);
    Gfx_SetLighting(FALSE);

    // 2D: textured corners quad, a transparent quad, culling triangles.
    Gfx_SetDepthTest(FALSE);
    Gfx_SetTexture(0, corners);
    Gfx_SetStageMipFilter(0, GFX_MIPFILTER_NONE);
    Gfx_SetStageMinFilter(0, GFX_FILTER_POINT);
    Gfx_SetStageMagFilter(0, GFX_FILTER_POINT);
    GfxTLVertex quad[4] = { TL(20, 20, 0xffffffff, 0, 0), TL(220, 20, 0xffffffff, 1, 0), TL(20, 170, 0xffffffff, 0, 1),
                            TL(220, 170, 0xffffffff, 1, 1) };
    Gfx_DrawPrimitiveTL(GFX_TRIANGLESTRIP, quad, 4);
    Gfx_SetTexture(0, NULL);
    Gfx_SetStageAlphaArg1(0, GFX_TA_DIFFUSE);
    Gfx_SetAlphaBlend(TRUE);
    Gfx_SetSrcBlend(GFX_BLEND_SRCALPHA);
    Gfx_SetDestBlend(GFX_BLEND_INVSRCALPHA);
    GfxTLVertex glass[4] = { TL(120, 90, 0x80ffff00, 0, 0), TL(300, 90, 0x80ffff00, 0, 0), TL(120, 220, 0x80ffff00, 0, 0),
                             TL(300, 220, 0x80ffff00, 0, 0) };
    Gfx_DrawPrimitiveTL(GFX_TRIANGLESTRIP, glass, 4);
    Gfx_SetAlphaBlend(FALSE);
    Gfx_SetStageAlphaArg1(0, GFX_TA_TEXTURE);
    // Clockwise on screen (y down): shown.
    GfxTLVertex cw[3] = { TL(400, 40, 0xff00ff00, 0, 0), TL(500, 160, 0xff00ff00, 0, 0), TL(400, 160, 0xff00ff00, 0, 0) };
    Gfx_DrawPrimitiveTL(GFX_TRIANGLELIST, cw, 3);
    // Counter-clockwise: culled.
    GfxTLVertex ccw[3] = { TL(520, 40, 0xffff0000, 0, 0), TL(520, 160, 0xffff0000, 0, 0), TL(620, 160, 0xffff0000, 0, 0) };
    Gfx_DrawPrimitiveTL(GFX_TRIANGLELIST, ccw, 3);
    Gfx_Present(TRUE);
}

} // namespace

int main(int, char **)
{
    Ctr_Init();
    if (!Gfx_SetVideoMode(640, 480, TRUE)) {
        Ctr_ShowMessage("gfx scene", "Gfx_SetVideoMode failed");
        return 1;
    }
    GfxTexture *corners = MakeTexture(64, Corners, 1);
    GfxTexture *checker = MakeTexture(256, Checker, 9);
    float angle = 0.0f;
    while (Ctr_Update()) {
        DrawScene(corners, checker, angle);
        // Turns while L is held (still otherwise, for comparisons).
        if (hidKeysHeld() & KEY_L)
            angle += 0.02f;
    }
    Gfx_Shutdown();
    Ctr_Shutdown();
    return 0;
}
