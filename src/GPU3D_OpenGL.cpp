/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU3D_OpenGL.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unordered_set>
#include "NDS.h"
#include "GPU.h"
#include "GPU3D_OpenGL_shaders.h"
#include "stb_image_write.h"

namespace melonDS
{

static std::unordered_map<const melonDS::ReplacementTex*, GLuint> gReplGL;

static GLuint GetOrCreateGLTex(const melonDS::ReplacementTex* R){
    auto it = gReplGL.find(R);
    if (it != gReplGL.end()) return it->second;

    if (!R || R->w <= 0 || R->h <= 0 || R->rgba.empty()) return 0;

    // save active unit
    GLint prevActive; glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, R->w, R->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, R->rgba.data());

    // restore active unit
    glActiveTexture(prevActive);

    gReplGL[R] = tex;
    return tex;
}

bool GLRenderer::BuildRenderShader(u32 flags, const std::string& vs, const std::string& fs)
{
    char shadername[32];
    snprintf(shadername, sizeof(shadername), "RenderShader%02X", flags);

    std::string vsbuf = std::string(kShaderHeader) + kRenderVSCommon + vs;
    std::string fsbuf = std::string(kShaderHeader) + kRenderFSCommon + fs;

    GLuint prog;
    bool ret = OpenGL::CompileVertexFragmentProgram(
        prog,
        vsbuf, fsbuf,
        shadername,
        {{"vPosition", 0}, {"vColor", 1}, {"vTexcoord", 2}, {"vPolygonAttr", 3}},
        {{"oColor", 0}, {"oAttr", 1}}
    );
    if (!ret) return false;

    // Привязка UBO
    GLint loc = glGetUniformBlockIndex(prog, "uConfig");
    if (loc >= 0) glUniformBlockBinding(prog, loc, 0);

    glUseProgram(prog);

   // sampler units
    if (GLint loc = glGetUniformLocation(prog, "TexMem");    loc >= 0) glUniform1i(loc, 0);
    if (GLint loc = glGetUniformLocation(prog, "TexPalMem"); loc >= 0) glUniform1i(loc, 1);
    if (GLint loc = glGetUniformLocation(prog, "ReplTex");   loc >= 0) glUniform1i(loc, 2);

    // cache locs + defaults
    ReplSizeLoc[flags] = glGetUniformLocation(prog, "ReplSize");
    ReplUseLoc [flags] = glGetUniformLocation(prog, "uUseRepl");
    if (ReplUseLoc[flags]  >= 0) glUniform1i(ReplUseLoc[flags], 0);
    if (ReplSizeLoc[flags] >= 0) glUniform2f(ReplSizeLoc[flags], 1.f, 1.f);

    GLint uni_id = glGetUniformBlockIndex(prog, "uConfig");
    glUniformBlockBinding(prog, uni_id, 0);

    // уже было:
    uni_id = glGetUniformLocation(prog, "TexMem");    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(prog, "TexPalMem"); glUniform1i(uni_id, 1);

    // добавь:
    if (GLint loc = glGetUniformLocation(prog, "ReplTex"); loc >= 0) {
        glUniform1i(loc, 2); // sampler2D для замен — всегда unit 2
    }
    ReplSizeLoc[flags] = glGetUniformLocation(prog, "ReplSize");   // может быть -1
    ReplUseLoc [flags] = glGetUniformLocation(prog, "uUseRepl");   // может быть -1

    RenderShader[flags] = prog;
    return true;
}

void GLRenderer::UseRenderShader(u32 flags)
{
    if (CurShaderID == flags) return;

    GLuint prog = RenderShader[flags];
    if (!prog) return;

    glUseProgram(prog);
    CurShaderID = flags;

    // ВСЕГДА ставим безопасные значения: замена выключена
    if (ReplUseLoc[flags]  >= 0) glUniform1i(ReplUseLoc[flags], 0);
    if (ReplSizeLoc[flags] >= 0) glUniform2f(ReplSizeLoc[flags], 1.f, 1.f);

    // И подстраховка: unit 2 = fallback (на случай «залипшей» текстуры)
    if (BoundReplTex != ReplFallbackTex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ReplFallbackTex);
        glActiveTexture(GL_TEXTURE0);
        BoundReplTex = ReplFallbackTex;
    }
    // если melonDS::TexReplace_ReplaceEnabled() == false — вообще ничего не трогаем:
    // шейдер всё равно не будет читать ReplTex (uUseRepl=0 в батчах),
    // а fallback у нас уже создан и, как правило, один раз был забинден на старте.
}

void SetupDefaultTexParams(GLuint tex)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

GLRenderer::GLRenderer(GLCompositor&& compositor) noexcept :
    Renderer3D(true),
    CurGLCompositor(std::move(compositor))
{
    // GLRenderer::New() will be used to actually initialize the renderer;
    // The various glDelete* functions silently ignore invalid IDs,
    // so we can just let the destructor clean up a half-initialized renderer.
}

std::unique_ptr<GLRenderer> GLRenderer::New() noexcept
{
    assert(glEnable != nullptr);

    std::optional<GLCompositor> compositor =  GLCompositor::New();
    if (!compositor)
        return nullptr;

    // Will be returned if the initialization succeeds,
    // or cleaned up via RAII if it fails.
    std::unique_ptr<GLRenderer> result = std::unique_ptr<GLRenderer>(new GLRenderer(std::move(*compositor)));
    compositor = std::nullopt;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);

    glDepthRange(0, 1);
    glClearDepth(1.0);

    if (!OpenGL::CompileVertexFragmentProgram(result->ClearShaderPlain,
            kClearVS, kClearFS,
            "ClearShader",
            {{"vPosition", 0}},
            {{"oColor", 0}, {"oAttr", 1}}))
        return nullptr;

    result->ClearUniformLoc[0] = glGetUniformLocation(result->ClearShaderPlain, "uColor");
    result->ClearUniformLoc[1] = glGetUniformLocation(result->ClearShaderPlain, "uDepth");
    result->ClearUniformLoc[2] = glGetUniformLocation(result->ClearShaderPlain, "uOpaquePolyID");
    result->ClearUniformLoc[3] = glGetUniformLocation(result->ClearShaderPlain, "uFogFlag");

    memset(result->RenderShader, 0, sizeof(RenderShader));

    if (!result->BuildRenderShader(0, kRenderVS_Z, kRenderFS_ZO))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_WBuffer, kRenderVS_W, kRenderFS_WO))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_Edge, kRenderVS_Z, kRenderFS_ZE))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_Edge | RenderFlag_WBuffer, kRenderVS_W, kRenderFS_WE))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_Trans, kRenderVS_Z, kRenderFS_ZT))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_Trans | RenderFlag_WBuffer, kRenderVS_W, kRenderFS_WT))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_ShadowMask, kRenderVS_Z, kRenderFS_ZSM))
        return nullptr;

    if (!result->BuildRenderShader(RenderFlag_ShadowMask | RenderFlag_WBuffer, kRenderVS_W, kRenderFS_WSM))
        return nullptr;

    if (!OpenGL::CompileVertexFragmentProgram(result->FinalPassEdgeShader,
            kFinalPassVS, kFinalPassEdgeFS,
            "FinalPassEdgeShader",
            {{"vPosition", 0}},
            {{"oColor", 0}}))
        return nullptr;
    if (!OpenGL::CompileVertexFragmentProgram(result->FinalPassFogShader,
            kFinalPassVS, kFinalPassFogFS,
            "FinalPassFogShader",
            {{"vPosition", 0}},
            {{"oColor", 0}}))
        return nullptr;

    GLuint uni_id = glGetUniformBlockIndex(result->FinalPassEdgeShader, "uConfig");
    glUniformBlockBinding(result->FinalPassEdgeShader, uni_id, 0);

    glUseProgram(result->FinalPassEdgeShader);
    uni_id = glGetUniformLocation(result->FinalPassEdgeShader, "DepthBuffer");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(result->FinalPassEdgeShader, "AttrBuffer");
    glUniform1i(uni_id, 1);

    uni_id = glGetUniformBlockIndex(result->FinalPassFogShader, "uConfig");
    glUniformBlockBinding(result->FinalPassFogShader, uni_id, 0);

    glUseProgram(result->FinalPassFogShader);
    uni_id = glGetUniformLocation(result->FinalPassFogShader, "DepthBuffer");
    glUniform1i(uni_id, 0);
    uni_id = glGetUniformLocation(result->FinalPassFogShader, "AttrBuffer");
    glUniform1i(uni_id, 1);


    memset(&result->ShaderConfig, 0, sizeof(ShaderConfig));

    glGenBuffers(1, &result->ShaderConfigUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, result->ShaderConfigUBO);
    static_assert((sizeof(ShaderConfig) & 15) == 0);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ShaderConfig), &result->ShaderConfig, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, result->ShaderConfigUBO);


    float clearvtx[6*2] =
    {
        -1.0, -1.0,
        1.0, 1.0,
        -1.0, 1.0,

        -1.0, -1.0,
        1.0, -1.0,
        1.0, 1.0
    };

    glGenBuffers(1, &result->ClearVertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->ClearVertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(clearvtx), clearvtx, GL_STATIC_DRAW);

    glGenVertexArrays(1, &result->ClearVertexArrayID);
    glBindVertexArray(result->ClearVertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)(0));


    glGenBuffers(1, &result->VertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, result->VertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VertexBuffer), nullptr, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &result->VertexArrayID);
    glBindVertexArray(result->VertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_SHORT, 7*4, (void*)(0));
    glEnableVertexAttribArray(1); // color
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, 7*4, (void*)(2*4));
    glEnableVertexAttribArray(2); // texcoords
    glVertexAttribIPointer(2, 2, GL_SHORT, 7*4, (void*)(3*4));
    glEnableVertexAttribArray(3); // attrib
    glVertexAttribIPointer(3, 3, GL_UNSIGNED_INT, 7*4, (void*)(4*4));

    glGenBuffers(1, &result->IndexBufferID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, result->IndexBufferID);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(IndexBuffer), nullptr, GL_DYNAMIC_DRAW);

    glGenFramebuffers(1, &result->MainFramebuffer);
    glGenFramebuffers(1, &result->DownscaleFramebuffer);

    // color buffers
    glGenTextures(1, &result->ColorBufferTex);
    SetupDefaultTexParams(result->ColorBufferTex);

    // depth/stencil buffer
    glGenTextures(1, &result->DepthBufferTex);
    SetupDefaultTexParams(result->DepthBufferTex);

    // attribute buffer
    // R: opaque polyID (for edgemarking)
    // G: edge flag
    // B: fog flag
    glGenTextures(1, &result->AttrBufferTex);
    SetupDefaultTexParams(result->AttrBufferTex);

    // downscale framebuffer for display capture (always 256x192)
    glGenTextures(1, &result->DownScaleBufferTex);
    SetupDefaultTexParams(result->DownScaleBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);

    glGenBuffers(1, &result->PixelbufferID);

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &result->TexMemID);
    glBindTexture(GL_TEXTURE_2D, result->TexMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);

    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &result->TexPalMemID);
    glBindTexture(GL_TEXTURE_2D, result->TexPalMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 48, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    // --- Fallback для ReplTex (unit 2), чтобы sampler2D всегда имел валидную float-текстуру
    glActiveTexture(GL_TEXTURE2);
    glGenTextures(1, &result->ReplFallbackTex);
    glBindTexture(GL_TEXTURE_2D, result->ReplFallbackTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // белый RGBA8
    const uint32_t white = 0xFFFFFFFFu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);

    glActiveTexture(GL_TEXTURE0);
    result->BoundReplTex = result->ReplFallbackTex;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return result;
}

GLRenderer::~GLRenderer()
{
    assert(glDeleteTextures != nullptr);

    glDeleteTextures(1, &TexMemID);
    glDeleteTextures(1, &TexPalMemID);
    glDeleteTextures(1, &ReplFallbackTex);

    glDeleteFramebuffers(1, &MainFramebuffer);
    glDeleteFramebuffers(1, &DownscaleFramebuffer);
    glDeleteTextures(1, &ColorBufferTex);
    glDeleteTextures(1, &DepthBufferTex);
    glDeleteTextures(1, &AttrBufferTex);
    glDeleteTextures(1, &DownScaleBufferTex);

    glDeleteVertexArrays(1, &VertexArrayID);
    glDeleteBuffers(1, &VertexBufferID);
    glDeleteVertexArrays(1, &ClearVertexArrayID);
    glDeleteBuffers(1, &ClearVertexBufferID);

    glDeleteBuffers(1, &ShaderConfigUBO);

    for (int i = 0; i < 16; i++)
    {
        if (!RenderShader[i]) continue;
        glDeleteProgram(RenderShader[i]);
    }
}

void GLRenderer::ApplyReplUniformsForBatch(const RendererPolygon* rp) const
{
    if (!melonDS::TexReplace_ReplaceEnabled()) return;

    // что хотим видеть в unit=2
    GLuint want = ReplFallbackTex;
    int w = 1, h = 1;

    if (rp && rp->ReplTex) {
        want = GetOrCreateGLTex(rp->ReplTex); // <-- ленивое создание тут
        if (want) { w = rp->ReplTex->w; h = rp->ReplTex->h; }
        else      { want = ReplFallbackTex;  w = 1; h = 1; }
    }

    if (want != BoundReplTex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, want);
        glActiveTexture(GL_TEXTURE0);
        BoundReplTex = want;
    }

    if (CurShaderID >= 0 && CurShaderID < 16) {
        if (ReplUseLoc[CurShaderID]  >= 0) glUniform1i(ReplUseLoc[CurShaderID],  (rp && rp->ReplTex) ? 1 : 0);
        if (ReplSizeLoc[CurShaderID] >= 0) glUniform2f(ReplSizeLoc[CurShaderID], float(w), float(h));
    }
}

void GLRenderer::Reset(GPU& gpu)
{
    // This is where the compositor's Reset() method would be called,
    // except there's no such method right now.
}

void GLRenderer::SetBetterPolygons(bool betterpolygons) noexcept
{
    SetRenderSettings(betterpolygons, ScaleFactor);
}

void GLRenderer::SetScaleFactor(int scale) noexcept
{
    SetRenderSettings(BetterPolygons, scale);
}


void GLRenderer::SetRenderSettings(bool betterpolygons, int scale) noexcept
{
    if (betterpolygons == BetterPolygons && scale == ScaleFactor)
        return;

    CurGLCompositor.SetScaleFactor(scale);
    ScaleFactor = scale;
    BetterPolygons = betterpolygons;

    ScreenW = 256 * scale;
    ScreenH = 192 * scale;

    glBindTexture(GL_TEXTURE_2D, ColorBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindTexture(GL_TEXTURE_2D, DepthBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, ScreenW, ScreenH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glBindTexture(GL_TEXTURE_2D, AttrBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, ScreenW, ScreenH, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, DownscaleFramebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, DownScaleBufferTex, 0);

    GLenum fbassign[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

    glBindFramebuffer(GL_FRAMEBUFFER, MainFramebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ColorBufferTex, 0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, DepthBufferTex, 0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, AttrBufferTex, 0);
    glDrawBuffers(2, fbassign);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
    glBufferData(GL_PIXEL_PACK_BUFFER, 256*192*4, NULL, GL_DYNAMIC_READ);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    //glLineWidth(scale);
    //glLineWidth(1.5);
}


void GLRenderer::SetupPolygon(GLRenderer::RendererPolygon* rp, Polygon* polygon) const
{
    rp->PolyData = polygon;

    // render key: depending on what we're drawing
    // opaque polygons:
    // - depthfunc
    // -- alpha=0
    // regular translucent polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID
    // ---- need opaque
    // shadow mask polygons:
    // - depthfunc?????
    // shadow polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID

    rp->RenderKey = (polygon->Attr >> 14) & 0x1; // bit14 - depth func
    if (!polygon->IsShadowMask)
    {
        if (polygon->Translucent)
        {
            if (polygon->IsShadow) rp->RenderKey |= 0x20000;
            else                   rp->RenderKey |= 0x10000;
            rp->RenderKey |= (polygon->Attr >> 10) & 0x2; // bit11 - depth write
            rp->RenderKey |= (polygon->Attr >> 13) & 0x4; // bit15 - fog
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16; // polygon ID
            if ((polygon->Attr & 0x001F0000) == 0x001F0000) // need opaque
                rp->RenderKey |= 0x4000;
        }
        else
        {
            if ((polygon->Attr & 0x001F0000) == 0)
                rp->RenderKey |= 0x2;
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16; // polygon ID
        }
    }
    else
    {
        rp->RenderKey |= 0x30000;
    }

    // ключ VRAM
    u32 texparam = polygon->TexParam;
    u32 fmt = (texparam >> 26) & 7;
    if (fmt == 0 || !melonDS::TexReplace_ReplaceEnabled()) {
        rp->ReplTex = nullptr; // у полигона нет текстуры → замены быть не может
    } else {
        u32 vramaddr = (texparam & 0xFFFF) << 3;
        auto R = GetBound(vramaddr, texparam, polygon->TexPalette);
        rp->ReplTex = R ? R.get() : nullptr;
    }
}

u32* GLRenderer::SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32* vptr) const
{
    u32 z = poly->FinalZ[vid];
    u32 w = poly->FinalW[vid];

    u32 alpha = (poly->Attr >> 16) & 0x1F;

    // Z should always fit within 16 bits, so it's okay to do this
    u32 zshift = 0;
    while (z > 0xFFFF) { z >>= 1; zshift++; }

    u32 x, y;
    if (ScaleFactor > 1)
    {
        x = (vtx->HiresPosition[0] * ScaleFactor) >> 4;
        y = (vtx->HiresPosition[1] * ScaleFactor) >> 4;
    }
    else
    {
        x = vtx->FinalPosition[0];
        y = vtx->FinalPosition[1];
    }

    // correct nearly-vertical edges that would look vertical on the DS
    /*{
        int vtopid = vid - 1;
        if (vtopid < 0) vtopid = poly->NumVertices-1;
        Vertex* vtop = poly->Vertices[vtopid];
        if (vtop->FinalPosition[1] >= vtx->FinalPosition[1])
        {
            vtopid = vid + 1;
            if (vtopid >= poly->NumVertices) vtopid = 0;
            vtop = poly->Vertices[vtopid];
        }
        if ((vtop->FinalPosition[1] < vtx->FinalPosition[1]) &&
            (vtx->FinalPosition[0] == vtop->FinalPosition[0]-1))
        {
            if (ScaleFactor > 1)
                x = (vtop->HiresPosition[0] * ScaleFactor) >> 4;
            else
                x = vtop->FinalPosition[0];
        }
    }*/

    *vptr++ = x | (y << 16);
    *vptr++ = z | (w << 16);

    *vptr++ =  (vtx->FinalColor[0] >> 1) |
              ((vtx->FinalColor[1] >> 1) << 8) |
              ((vtx->FinalColor[2] >> 1) << 16) |
              (alpha << 24);

    *vptr++ = (u16)vtx->TexCoords[0] | ((u16)vtx->TexCoords[1] << 16);

    *vptr++ = vtxattr | (zshift << 16);
    *vptr++ = poly->TexParam;
    *vptr++ = poly->TexPalette;

    return vptr;
}

void GLRenderer::BuildPolygons(GLRenderer::RendererPolygon* polygons, int npolys)
{
    u32* vptr = &VertexBuffer[0];
    u32 vidx = 0;

    u32 iidx = 0;
    u32 eidx = EdgeIndicesOffset;

    for (int i = 0; i < npolys; i++)
    {
        RendererPolygon* rp = &polygons[i];
        Polygon* poly = rp->PolyData;

        rp->IndicesOffset = iidx;
        rp->NumIndices = 0;

        u32 vidx_first = vidx;

        u32 polyattr = poly->Attr;

        u32 alpha = (polyattr >> 16) & 0x1F;

        u32 vtxattr = polyattr & 0x1F00C8F0;
        if (poly->FacingView) vtxattr |= (1<<8);
        if (poly->WBuffer)    vtxattr |= (1<<9);

        // assemble vertices
        if (poly->Type == 1) // line
        {
            rp->PrimType = GL_LINES;

            u32 lastx, lasty;
            int nout = 0;
            for (u32 j = 0; j < poly->NumVertices; j++)
            {
                Vertex* vtx = poly->Vertices[j];

                if (j > 0)
                {
                    if (lastx == vtx->FinalPosition[0] &&
                        lasty == vtx->FinalPosition[1]) continue;
                }

                lastx = vtx->FinalPosition[0];
                lasty = vtx->FinalPosition[1];

                vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

                IndexBuffer[iidx++] = vidx;
                rp->NumIndices++;

                vidx++;
                nout++;
                if (nout >= 2) break;
            }
        }
        else if (poly->NumVertices == 3) // regular triangle
        {
            rp->PrimType = GL_TRIANGLES;

            for (int j = 0; j < 3; j++)
            {
                Vertex* vtx = poly->Vertices[j];

                vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);
                vidx++;
            }

            // build a triangle
            IndexBuffer[iidx++] = vidx_first;
            IndexBuffer[iidx++] = vidx - 2;
            IndexBuffer[iidx++] = vidx - 1;
            rp->NumIndices += 3;
        }
        else // quad, pentagon, etc
        {
            rp->PrimType = GL_TRIANGLES;

            if (!BetterPolygons)
            {
                // regular triangle-splitting

                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

                    if (j >= 2)
                    {
                        // build a triangle
                        IndexBuffer[iidx++] = vidx_first;
                        IndexBuffer[iidx++] = vidx - 1;
                        IndexBuffer[iidx++] = vidx;
                        rp->NumIndices += 3;
                    }

                    vidx++;
                }
            }
            else
            {
                // attempt at 'better' splitting
                // this doesn't get rid of the error while splitting a bigger polygon into triangles
                // but we can attempt to reduce it

                u32 cX = 0, cY = 0;
                float cZ = 0;
                float cW = 0;

                float cR = 0, cG = 0, cB = 0;
                float cS = 0, cT = 0;

                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    cX += vtx->HiresPosition[0];
                    cY += vtx->HiresPosition[1];

                    float fw = (float)poly->FinalW[j] * poly->NumVertices;
                    cW += 1.0f / fw;

                    if (poly->WBuffer) cZ += poly->FinalZ[j] / fw;
                    else               cZ += poly->FinalZ[j];

                    cR += (vtx->FinalColor[0] >> 1) / fw;
                    cG += (vtx->FinalColor[1] >> 1) / fw;
                    cB += (vtx->FinalColor[2] >> 1) / fw;

                    cS += vtx->TexCoords[0] / fw;
                    cT += vtx->TexCoords[1] / fw;
                }

                cX /= poly->NumVertices;
                cY /= poly->NumVertices;

                cW = 1.0f / cW;

                if (poly->WBuffer) cZ *= cW;
                else               cZ /= poly->NumVertices;

                cR *= cW;
                cG *= cW;
                cB *= cW;

                cS *= cW;
                cT *= cW;

                cX = (cX * ScaleFactor) >> 4;
                cY = (cY * ScaleFactor) >> 4;

                u32 w = (u32)cW;

                u32 z = (u32)cZ;
                u32 zshift = 0;
                while (z > 0xFFFF) { z >>= 1; zshift++; }

                // build center vertex
                *vptr++ = cX | (cY << 16);
                *vptr++ = z | (w << 16);

                *vptr++ =  (u32)cR |
                          ((u32)cG << 8) |
                          ((u32)cB << 16) |
                          (alpha << 24);

                *vptr++ = (u16)cS | ((u16)cT << 16);

                *vptr++ = vtxattr | (zshift << 16);
                *vptr++ = poly->TexParam;
                *vptr++ = poly->TexPalette;

                vidx++;

                // build the final polygon
                for (u32 j = 0; j < poly->NumVertices; j++)
                {
                    Vertex* vtx = poly->Vertices[j];

                    vptr = SetupVertex(poly, j, vtx, vtxattr, vptr);

                    if (j >= 1)
                    {
                        // build a triangle
                        IndexBuffer[iidx++] = vidx_first;
                        IndexBuffer[iidx++] = vidx - 1;
                        IndexBuffer[iidx++] = vidx;
                        rp->NumIndices += 3;
                    }

                    vidx++;
                }

                IndexBuffer[iidx++] = vidx_first;
                IndexBuffer[iidx++] = vidx - 1;
                IndexBuffer[iidx++] = vidx_first + 1;
                rp->NumIndices += 3;
            }
        }

        rp->EdgeIndicesOffset = eidx;
        rp->NumEdgeIndices = 0;

        u32 vidx_cur = vidx_first;
        for (u32 j = 1; j < poly->NumVertices; j++)
        {
            IndexBuffer[eidx++] = vidx_cur;
            IndexBuffer[eidx++] = vidx_cur + 1;
            vidx_cur++;
            rp->NumEdgeIndices += 2;
        }
        IndexBuffer[eidx++] = vidx_cur;
        IndexBuffer[eidx++] = vidx_first;
        rp->NumEdgeIndices += 2;
    }

    NumVertices = vidx;
    NumIndices = iidx;
    NumEdgeIndices = eidx - EdgeIndicesOffset;
}

/*inline void GLRenderer::ApplyReplUniforms(u32 flags, const RendererPolygon* rp) const
{
    if (rp->ReplTex) {
        // ленивый аплоад в GL
        if (rp->ReplTex->gltex == 0) {
            GLuint tex;
            glGenTextures(1, &tex);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rp->ReplTex->w, rp->ReplTex->h,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, rp->ReplTex->rgba.data());
            rp->ReplTex->gltex = tex;
        } else {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, rp->ReplTex->gltex);
        }
        glUniform1i(ReplUseLoc[flags], 1);
        glUniform2f(ReplSizeLoc[flags], (float)rp->ReplTex->w, (float)rp->ReplTex->h);
    } else {
        glUniform1i(ReplUseLoc[flags], 0);
    }
}*/

void GLRenderer::ApplyReplUniforms(u32 flags, const RendererPolygon* rp) const
{
    if (!melonDS::TexReplace_ReplaceEnabled()) return;
    if (rp && rp->ReplTex) {
        GLuint tex = GetOrCreateGLTex(rp->ReplTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (ReplUseLoc[flags] >= 0) glUniform1i(ReplUseLoc[flags], 1);
        if (ReplSizeLoc[flags] >= 0) glUniform2f(ReplSizeLoc[flags],
                                                 float(rp->ReplTex->w), float(rp->ReplTex->h));
    } else {
        if (ReplUseLoc[flags] >= 0) glUniform1i(ReplUseLoc[flags], 0);
        if (ReplSizeLoc[flags] >= 0) glUniform2f(ReplSizeLoc[flags], 1.f, 1.f);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ReplFallbackTex);
    }
    // ВСЕГДА возвращаемся на 0
    glActiveTexture(GL_TEXTURE0);
}

int GLRenderer::RenderSinglePolygon(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];
    if (melonDS::TexReplace_ReplaceEnabled()) {
        ApplyReplUniforms(CurShaderID, rp);
    }

    glDrawElements(rp->PrimType, rp->NumIndices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->IndicesOffset * 2));

    return 1;
}

int GLRenderer::RenderPolygonBatch(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];
    const GLuint primtype = rp->PrimType;
    const u32 key = rp->RenderKey;

    // Если замены включены — склеиваем батчи только для одинакового ReplTex (включая nullptr)
    const bool replFeature = melonDS::TexReplace_ReplaceEnabled();
    const void* replKey = replFeature ? static_cast<const void*>(rp->ReplTex) : nullptr;

    int numpolys = 0;
    u32 numindices = 0;

    for (int iend = i; iend < NumFinalPolys; ++iend)
    {
        const RendererPolygon* cur = &PolygonList[iend];
        if (cur->PrimType != primtype) break;
        if (cur->RenderKey != key) break;
        if (replFeature && (cur->ReplTex != replKey)) break; // сменился ReplTex — новый батч

        numpolys++;
        numindices += cur->NumIndices;
    }

    // ВАЖНО: всегда проставляем униформы/текстуру для текущего батча,
    // даже если ReplTex == nullptr — тогда функция выключит замену и забиндит fallback.
    if (replFeature) {
        ApplyReplUniformsForBatch(rp);
    }

    glDrawElements(
        primtype,
        numindices,
        GL_UNSIGNED_SHORT,
        (void*)(uintptr_t)(rp->IndicesOffset * sizeof(u16))
    );

    return numpolys;
}

int GLRenderer::RenderPolygonEdgeBatch(int i) const
{
    const RendererPolygon* rp = &PolygonList[i];
    u32 key = rp->RenderKey;
    int numpolys = 0;
    u32 numindices = 0;

    for (int iend = i; iend < NumFinalPolys; iend++)
    {
        const RendererPolygon* cur_rp = &PolygonList[iend];
        if (cur_rp->RenderKey != key) break;

        numpolys++;
        numindices += cur_rp->NumEdgeIndices;
    }

    glDrawElements(GL_LINES, numindices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->EdgeIndicesOffset * 2));
    return numpolys;
}

void GLRenderer::RenderSceneChunk(const GPU3D& gpu3d, int y, int h)
{
    u32 flags = 0;
    if (gpu3d.RenderPolygonRAM[0]->WBuffer) flags |= RenderFlag_WBuffer;

    if (h != 192) glScissor(0, y<<ScaleFactor, 256<<ScaleFactor, h<<ScaleFactor);

    GLboolean fogenable = (gpu3d.RenderDispCnt & (1<<7)) ? GL_TRUE : GL_FALSE;

    // TODO: proper 'equal' depth test!
    // (has margin of +-0x200 in Z-buffer mode, +-0xFF in W-buffer mode)
    // for now we're using GL_LEQUAL to make it work to some extent

    // pass 1: opaque pixels

    UseRenderShader(flags);
    glLineWidth(1.0);

    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glBindVertexArray(VertexArrayID);

    for (int i = 0; i < NumFinalPolys; )
    {
        RendererPolygon* rp = &PolygonList[i];

        if (rp->PolyData->IsShadowMask) { i++; continue; }
        if (rp->PolyData->Translucent) { i++; continue; }

        if (rp->PolyData->Attr & (1<<14))
            glDepthFunc(GL_LEQUAL);
        else
            glDepthFunc(GL_LESS);

        u32 polyattr = rp->PolyData->Attr;
        u32 polyid = (polyattr >> 24) & 0x3F;

        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glStencilMask(0xFF);

        i += RenderPolygonBatch(i);
    }

    // if edge marking is enabled, mark all opaque edges
    // TODO BETTER EDGE MARKING!!! THIS SUCKS
    /*if (RenderDispCnt & (1<<5))
    {
        UseRenderShader(flags | RenderFlag_Edge);
        glLineWidth(1.5);

        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);

        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_FALSE);

        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);

        for (int i = 0; i < NumFinalPolys; )
        {
            RendererPolygon* rp = &PolygonList[i];

            if (rp->PolyData->IsShadowMask) { i++; continue; }

            i += RenderPolygonEdgeBatch(i);
        }

        glDepthMask(GL_TRUE);
    }*/

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);

    if (gpu3d.RenderDispCnt & (1<<3))
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    else
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);

    glLineWidth(1.0);

    if (NumOpaqueFinalPolys > -1)
    {
        // pass 2: if needed, render translucent pixels that are against background pixels
        // when background alpha is zero, those need to be rendered with blending disabled

        if ((gpu3d.RenderClearAttr1 & 0x001F0000) == 0)
        {
            glDisable(GL_BLEND);

            for (int i = 0; i < NumFinalPolys; )
            {
                RendererPolygon* rp = &PolygonList[i];

                if (rp->PolyData->IsShadowMask)
                {
                    // draw actual shadow mask

                    UseRenderShader(flags | RenderFlag_ShadowMask);
                    if (ReplUseLoc[CurShaderID] >= 0) glUniform1i(ReplUseLoc[CurShaderID], 0);

                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);

                    glDepthFunc(GL_LESS);
                    glStencilFunc(GL_EQUAL, 0xFF, 0xFF);
                    glStencilOp(GL_KEEP, GL_INVERT, GL_KEEP);
                    glStencilMask(0x01);

                    i += RenderPolygonBatch(i);
                }
                else if (rp->PolyData->Translucent)
                {
                    bool needopaque = ((rp->PolyData->Attr & 0x001F0000) == 0x001F0000);

                    u32 polyattr = rp->PolyData->Attr;
                    u32 polyid = (polyattr >> 24) & 0x3F;

                    if (polyattr & (1<<14))
                        glDepthFunc(GL_LEQUAL);
                    else
                        glDepthFunc(GL_LESS);

                    if (needopaque)
                    {
                        UseRenderShader(flags);

                        glDisable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

                        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                        glStencilMask(0xFF);

                        glDepthMask(GL_TRUE);

                        RenderSinglePolygon(i);
                    }

                    UseRenderShader(flags | RenderFlag_Trans);

                    GLboolean transfog;
                    if (!(polyattr & (1<<15))) transfog = fogenable;
                    else                       transfog = GL_FALSE;

                    if (rp->PolyData->IsShadow)
                    {
                        // shadow against clear-plane will only pass if its polyID matches that of the clear plane
                        u32 clrpolyid = (gpu3d.RenderClearAttr1 >> 24) & 0x3F;
                        if (polyid != clrpolyid) { i++; continue; }

                        glEnable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                        glStencilFunc(GL_EQUAL, 0xFE, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40|polyid)); // heheh

                        if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                        else                    glDepthMask(GL_FALSE);

                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    }
                    else
                    {
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                        glStencilFunc(GL_EQUAL, 0xFF, 0xFE);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40|polyid)); // heheh

                        if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                        else                    glDepthMask(GL_FALSE);

                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    }
                }
                else
                    i++;
            }

            glEnable(GL_BLEND);
            glStencilMask(0xFF);
        }

        // pass 3: translucent pixels

        for (int i = 0; i < NumFinalPolys; )
        {
            RendererPolygon* rp = &PolygonList[i];

            if (rp->PolyData->IsShadowMask)
            {
                // clear shadow bits in stencil buffer

                glStencilMask(0x80);
                glClear(GL_STENCIL_BUFFER_BIT);

                // draw actual shadow mask

                UseRenderShader(flags | RenderFlag_ShadowMask);

                glDisable(GL_BLEND);
                glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);

                glDepthFunc(GL_LESS);
                glStencilFunc(GL_ALWAYS, 0x80, 0x80);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);

                i += RenderPolygonBatch(i);
            }
            else if (rp->PolyData->Translucent)
            {
                bool needopaque = ((rp->PolyData->Attr & 0x001F0000) == 0x001F0000);

                u32 polyattr = rp->PolyData->Attr;
                u32 polyid = (polyattr >> 24) & 0x3F;

                if (polyattr & (1<<14))
                    glDepthFunc(GL_LEQUAL);
                else
                    glDepthFunc(GL_LESS);

                if (needopaque)
                {
                    UseRenderShader(flags);

                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);

                    glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0xFF);

                    glDepthMask(GL_TRUE);

                    RenderSinglePolygon(i);
                }

                UseRenderShader(flags | RenderFlag_Trans);

                GLboolean transfog;
                if (!(polyattr & (1<<15))) transfog = fogenable;
                else                       transfog = GL_FALSE;

                if (rp->PolyData->IsShadow)
                {
                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glStencilFunc(GL_EQUAL, polyid, 0x3F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                    glStencilMask(0x80);

                    RenderSinglePolygon(i);

                    glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                    glStencilFunc(GL_EQUAL, 0xC0|polyid, 0x80);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);

                    i += RenderSinglePolygon(i);
                }
                else
                {
                    glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);

                    glStencilFunc(GL_NOTEQUAL, 0x40|polyid, 0x7F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);

                    i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                }
            }
            else
                i++;
        }
    }

    if (gpu3d.RenderDispCnt & 0x00A0) // fog/edge enabled
    {
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_FALSE);
        glStencilFunc(GL_ALWAYS, 0, 0);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, DepthBufferTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, AttrBufferTex);

        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);

        if (gpu3d.RenderDispCnt & (1<<5))
        {
            // edge marking
            // TODO: depth/polyid values at screen edges

            glUseProgram(FinalPassEdgeShader);

            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }

        if (gpu3d.RenderDispCnt & (1<<7))
        {
            // fog

            glUseProgram(FinalPassFogShader);

            if (gpu3d.RenderDispCnt & (1<<6))
                glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            else
                glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);

            {
                u32 c = gpu3d.RenderFogColor;
                u32 r = c & 0x1F;
                u32 g = (c >> 5) & 0x1F;
                u32 b = (c >> 10) & 0x1F;
                u32 a = (c >> 16) & 0x1F;

                glBlendColor((float)b/31.0, (float)g/31.0, (float)r/31.0, (float)a/31.0);
            }

            glDrawArrays(GL_TRIANGLES, 0, 2*3);
        }
    }
}

// CPU-сэмплер текстуры DS (как в SoftRenderer::TextureLookup)
// s,t — 4.12 fixed, texparam/texpal — регистры DS.
static inline void TextureLookup_CPU(const GPU& gpu,
                                     u32 texparam, u32 texpal,
                                     s16 s, s16 t,
                                     u16* outColor, u8* outAlpha)
{
    u32 vramaddr = (texparam & 0xFFFF) << 3;

    s32 width  = 8 << ((texparam >> 20) & 0x7);
    s32 height = 8 << ((texparam >> 23) & 0x7);

    // -> DS texel coords
    s >>= 4;
    t >>= 4;

    // wrapping зерк/клэмп как на DS
    auto wrap1 = [](int v, int size, bool wrap, bool mirror) {
        if (wrap) {
            if (mirror) {
                if (v & size) v = (size - 1) - (v & (size - 1));
                else          v = (v & (size - 1));
            } else {
                v &= (size - 1);
            }
        } else {
            if (v < 0) v = 0;
            else if (v >= size) v = size - 1;
        }
        return v;
    };

    bool wrapS   = (texparam & (1<<16));
    bool wrapT   = (texparam & (1<<17));
    bool mirrorS = (texparam & (1<<18));
    bool mirrorT = (texparam & (1<<19));
    s = wrap1(s, width,  wrapS, mirrorS);
    t = wrap1(t, height, wrapT, mirrorT);

    u8 alpha0 = (texparam & (1<<29)) ? 0 : 31;

    switch ((texparam >> 26) & 0x7)
    {
    case 1: // A3I5
    {
        vramaddr += ((t * width) + s);
        u8 px = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
        texpal <<= 4;
        *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + ((px & 0x1F) << 1));
        *outAlpha = ((px >> 3) & 0x1C) + (px >> 6);
        break;
    }
    case 2: // 4-color
    {
        vramaddr += (((t * width) + s) >> 2);
        u8 px = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
        px >>= ((s & 0x3) << 1);
        px &= 0x3;
        texpal <<= 3;
        *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + (px << 1));
        *outAlpha = (px==0) ? alpha0 : 31;
        break;
    }
    case 3: // 16-color
    {
        vramaddr += (((t * width) + s) >> 1);
        u8 px = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
        if (s & 0x1) px >>= 4; else px &= 0xF;
        texpal <<= 4;
        *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + (px << 1));
        *outAlpha = (px==0) ? alpha0 : 31;
        break;
    }
    case 4: // 256-color
    {
        vramaddr += ((t * width) + s);
        u8 px = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
        texpal <<= 4;
        *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + (px << 1));
        *outAlpha = (px==0) ? alpha0 : 31;
        break;
    }
    case 5: // compressed (DS 4x4)
    {
        vramaddr += ((t & 0x3FC) * (width>>2)) + (s & 0x3FC);
        vramaddr += (t & 0x3);
        vramaddr &= 0x7FFFF; // wrap after slot 3

        u32 slot1addr = 0x20000 + ((vramaddr & 0x1FFFC) >> 1);
        if (vramaddr >= 0x40000) slot1addr += 0x10000;

        u8 val;
        if (vramaddr >= 0x20000 && vramaddr < 0x40000)
            val = 0;
        else {
            val = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
            val >>= (2 * (s & 0x3));
        }

        u16 palinfo = gpu.ReadVRAMFlat_Texture<u16>(slot1addr);
        u32 paloffset = (palinfo & 0x3FFF) << 2;
        texpal <<= 4;

        auto blend555 = [](u16 c0, u16 c1, int w0, int w1, int denom) {
            u32 r0 =  c0        & 0x001F, r1 =  c1        & 0x001F;
            u32 g0 = (c0 >> 5)  & 0x001F, g1 = (c1 >> 5)  & 0x001F;
            u32 b0 = (c0 >> 10) & 0x001F, b1 = (c1 >> 10) & 0x001F;
            u32 r = (r0*w0 + r1*w1) / denom;
            u32 g = (g0*w0 + g1*w1) / denom;
            u32 b = (b0*w0 + b1*w1) / denom;
            return (u16)(r | (g<<5) | (b<<10));
        };

        switch (val & 0x3)
        {
        case 0:
            *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset);
            *outAlpha = 31;
            break;
        case 1:
            *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset + 2);
            *outAlpha = 31;
            break;
        case 2:
        {
            u16 c0 = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset);
            u16 c1 = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset + 2);
            switch (palinfo >> 14) {
            case 1: *outColor = blend555(c0, c1, 1,1,2); break;        // avg
            case 3: *outColor = blend555(c0, c1, 5,3,8); break;        // 5/3
            default:*outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset + 4); break;
            }
            *outAlpha = 31;
            break;
        }
        case 3:
            if ((palinfo >> 14) == 2) {
                *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset + 6);
                *outAlpha = 31;
            } else if ((palinfo >> 14) == 3) {
                u16 c0 = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset);
                u16 c1 = gpu.ReadVRAMFlat_TexPal<u16>(texpal + paloffset + 2);
                *outColor = blend555(c0, c1, 3,5,8); // 3/5
                *outAlpha = 31;
            } else {
                *outColor = 0; *outAlpha = 0;
            }
            break;
        }
        break;
    }
    case 6: // A5I3
    {
        vramaddr += ((t * width) + s);
        u8 px = gpu.ReadVRAMFlat_Texture<u8>(vramaddr);
        texpal <<= 4;
        *outColor = gpu.ReadVRAMFlat_TexPal<u16>(texpal + ((px & 0x7) << 1));
        *outAlpha = (px >> 3);
        break;
    }
    case 7: // direct color 15bpp + 1bit alpha
    {
        vramaddr += (((t * width) + s) << 1);
        *outColor = gpu.ReadVRAMFlat_Texture<u16>(vramaddr);
        *outAlpha = (*outColor & 0x8000) ? 31 : 0;
        break;
    }
    default:
        *outColor = 0; *outAlpha = 0;
    }
}

// Канонический декод в RGBA (без записи PNG)
bool GLRenderer::Decode3DTextureToRGBA(const GPU& gpu, u32 texparam, u32 texpal,
                                  std::vector<uint8_t>& rgba,
                                  int& outW, int& outH, u32& outFmt)
{
    uint32_t fmt = (texparam >> 26) & 7;
    if (fmt == 0) return false;

    int w = 8 << ((texparam >> 20) & 7);
    int h = 8 << ((texparam >> 23) & 7);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;

    rgba.assign(w * h * 4, 0);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            // (s,t) берём по центру texel: 4.12 fixed = (x<<4,y<<4)
            uint16_t col15 = 0;
            uint8_t  a5    = 0;
            TextureLookup_CPU(gpu, texparam, texpal, (s16)(x<<4), (s16)(y<<4), &col15, &a5);

            uint8_t r5 = (col15      ) & 0x1F;
            uint8_t g5 = (col15 >>  5) & 0x1F;
            uint8_t b5 = (col15 >> 10) & 0x1F;

            uint8_t r = (r5 << 3) | (r5 >> 2);
            uint8_t g = (g5 << 3) | (g5 >> 2);
            uint8_t b = (b5 << 3) | (b5 >> 2);
            uint8_t a = (uint8_t)((a5 * 255) / 31); // как у вас

            int off = (y * w + x) * 4;
            rgba[off + 0] = r;
            rgba[off + 1] = g;
            rgba[off + 2] = b;
            rgba[off + 3] = a;
        }
    }

    outW   = w;
    outH   = h;
    outFmt = fmt;
    return true;
}

void GLRenderer::RenderFrame(GPU& gpu)
{
    // как в софте
    auto textureDirty = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
    auto texPalDirty  = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);
    gpu.MakeVRAMFlat_TextureCoherent(textureDirty);
    gpu.MakeVRAMFlat_TexPalCoherent(texPalDirty);

    if (melonDS::TexReplace_ReplaceEnabled()) {
        ClearBindings();
    }

    if (melonDS::TexReplace_DumpEnabled() || melonDS::TexReplace_ReplaceEnabled()) {
        static thread_local std::unordered_set<uint64_t> seenFrame;
        seenFrame.clear();

        EnsureDump3DDir();

        for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++) {
            auto* p = gpu.GPU3D.RenderPolygonRAM[i];
            if (p->Degenerate) continue;

            u32 texparam = p->TexParam;
            u32 fmt = (texparam >> 26) & 7;
            if (fmt == 0) continue;

            u32 vramaddr = (texparam & 0xFFFF) << 3;

            // уникальность в кадре (vramaddr+texparam+texpal важно!)
            uint64_t key = (uint64_t)vramaddr
                        ^ (uint64_t)texparam * 0x9e3779b185ebca87ULL
                        ^ (uint64_t)p->TexPalette * 0xC2B2AE3D27D4EB4FULL;
            if (!seenFrame.insert(key).second) continue;

            // декод в RGBA (см. пункт 6)
            std::vector<uint8_t> rgba; int w=0,h=0; u32 f=0;
            if (!Decode3DTextureToRGBA(gpu, texparam, p->TexPalette, rgba, w, h, f)) continue;

            uint64_t h64 = fnv1a64_quarterTL_rgba(rgba.data(), w, h);

            if (melonDS::TexReplace_ReplaceEnabled()) {
                if (auto rep = FindOrLoadByHash(h64, f, w, h)) {
                    BindReplacement(vramaddr, texparam, p->TexPalette, rep);
                }
            }

            if (melonDS::TexReplace_DumpEnabled()) {
                // чтобы не забивать дублями — учти формат/размер в сигнатуре
                uint64_t sig = h64 ^ (uint64_t(f) << 56)
                                    ^ (uint64_t(uint16_t(w)) << 32)
                                    ^ (uint64_t(uint16_t(h)) << 16);

                if (!gSeen3DTex.insert(sig).second) continue;

                char fname[256];
                std::snprintf(fname, sizeof(fname),
                            "text_replace/dump/%016llX_fmt%u_%dx%d.png",
                            (unsigned long long)h64, f, w, h);
                stbi_write_png(fname, w, h, 4, rgba.data(), w*4);
            }
        }
    }

    CurShaderID = -1;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, MainFramebuffer);

    ShaderConfig.uScreenSize[0] = ScreenW;
    ShaderConfig.uScreenSize[1] = ScreenH;
    ShaderConfig.uDispCnt = gpu.GPU3D.RenderDispCnt;

    for (int i = 0; i < 32; i++)
    {
        u16 c = gpu.GPU3D.RenderToonTable[i];
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;

        ShaderConfig.uToonColors[i][0] = (float)r / 31.0;
        ShaderConfig.uToonColors[i][1] = (float)g / 31.0;
        ShaderConfig.uToonColors[i][2] = (float)b / 31.0;
    }

    for (int i = 0; i < 8; i++)
    {
        u16 c = gpu.GPU3D.RenderEdgeTable[i];
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;

        ShaderConfig.uEdgeColors[i][0] = (float)r / 31.0;
        ShaderConfig.uEdgeColors[i][1] = (float)g / 31.0;
        ShaderConfig.uEdgeColors[i][2] = (float)b / 31.0;
    }

    {
        u32 c = gpu.GPU3D.RenderFogColor;
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;
        u32 a = (c >> 16) & 0x1F;

        ShaderConfig.uFogColor[0] = (float)r / 31.0;
        ShaderConfig.uFogColor[1] = (float)g / 31.0;
        ShaderConfig.uFogColor[2] = (float)b / 31.0;
        ShaderConfig.uFogColor[3] = (float)a / 31.0;
    }

    for (int i = 0; i < 34; i++)
    {
        u8 d = gpu.GPU3D.RenderFogDensityTable[i];
        ShaderConfig.uFogDensity[i][0] = (float)d / 127.0;
    }

    ShaderConfig.uFogOffset = gpu.GPU3D.RenderFogOffset;
    ShaderConfig.uFogShift = gpu.GPU3D.RenderFogShift;

    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ShaderConfig), nullptr, GL_STREAM_DRAW); // orphan
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ShaderConfig), &ShaderConfig);

    // SUCKY!!!!!!!!!!!!!!!!!!
    // TODO: detect when VRAM blocks are modified!
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, TexMemID);
    for (int i = 0; i < 4; i++)
    {
        u32 mask = gpu.VRAMMap_Texture[i];
        u8* vram;
        if (!mask) continue;
        else if (mask & (1<<0)) vram = gpu.VRAM_A;
        else if (mask & (1<<1)) vram = gpu.VRAM_B;
        else if (mask & (1<<2)) vram = gpu.VRAM_C;
        else if (mask & (1<<3)) vram = gpu.VRAM_D;

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i*128, 1024, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, vram);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, TexPalMemID);
    for (int i = 0; i < 6; i++)
    {
        // 6 x 16K chunks
        u32 mask = gpu.VRAMMap_TexPal[i];
        u8* vram;
        if (!mask) continue;
        else if (mask & (1<<4)) vram = &gpu.VRAM_E[(i&3)*0x4000];
        else if (mask & (1<<5)) vram = gpu.VRAM_F;
        else if (mask & (1<<6)) vram = gpu.VRAM_G;

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i*8, 1024, 8, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram);
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);

    glViewport(0, 0, ScreenW, ScreenH);

    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);

    // clear buffers
    // TODO: clear bitmap
    // TODO: check whether 'clear polygon ID' affects translucent polyID
    // (for example when alpha is 1..30)
    {
        glUseProgram(ClearShaderPlain);
        glDepthFunc(GL_ALWAYS);

        u32 r = gpu.GPU3D.RenderClearAttr1 & 0x1F;
        u32 g = (gpu.GPU3D.RenderClearAttr1 >> 5) & 0x1F;
        u32 b = (gpu.GPU3D.RenderClearAttr1 >> 10) & 0x1F;
        u32 fog = (gpu.GPU3D.RenderClearAttr1 >> 15) & 0x1;
        u32 a = (gpu.GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        u32 polyid = (gpu.GPU3D.RenderClearAttr1 >> 24) & 0x3F;
        u32 z = ((gpu.GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;

        glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        /*if (r) r = r*2 + 1;
        if (g) g = g*2 + 1;
        if (b) b = b*2 + 1;*/

        glUniform4ui(ClearUniformLoc[0], r, g, b, a);
        glUniform1ui(ClearUniformLoc[1], z);
        glUniform1ui(ClearUniformLoc[2], polyid);
        glUniform1ui(ClearUniformLoc[3], fog);

        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);
        glDrawArrays(GL_TRIANGLES, 0, 2*3);
    }

    if (gpu.GPU3D.RenderNumPolygons)
    {
        // render shit here
        u32 flags = 0;
        if (gpu.GPU3D.RenderPolygonRAM[0]->WBuffer) flags |= RenderFlag_WBuffer;

        int npolys = 0;
        int firsttrans = -1;
        for (u32 i = 0; i < gpu.GPU3D.RenderNumPolygons; i++)
        {
            if (gpu.GPU3D.RenderPolygonRAM[i]->Degenerate) continue;

            SetupPolygon(&PolygonList[npolys], gpu.GPU3D.RenderPolygonRAM[i]);
            if (firsttrans < 0 && gpu.GPU3D.RenderPolygonRAM[i]->Translucent)
                firsttrans = npolys;

            npolys++;
        }
        NumFinalPolys = npolys;
        NumOpaqueFinalPolys = firsttrans;

        BuildPolygons(&PolygonList[0], npolys);
        
        // VBO (orphan + один SubData)
        glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
        GLsizeiptr vbytes = NumVertices * 7 * 4;
        glBufferData(GL_ARRAY_BUFFER, vbytes, nullptr, GL_STREAM_DRAW); // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, VertexBuffer);

        // IBO (orphan + один SubData на весь используемый диапазон)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBufferID);
        GLsizeiptr ibytes = (EdgeIndicesOffset + NumEdgeIndices) * sizeof(u16);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, nullptr, GL_STREAM_DRAW); // orphan
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ibytes, IndexBuffer);

        RenderSceneChunk(gpu.GPU3D, 0, 192);
    }
}

void GLRenderer::Stop(const GPU& gpu)
{
    CurGLCompositor.Stop(gpu);
}

void GLRenderer::PrepareCaptureFrame()
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, MainFramebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DownscaleFramebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, ScreenW, ScreenH, 0, 0, 256, 192, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, DownscaleFramebuffer);
    glReadPixels(0, 0, 256, 192, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
}

void GLRenderer::Blit(const GPU& gpu)
{
    CurGLCompositor.RenderFrame(gpu, *this);
}

void GLRenderer::BindOutputTexture(int buffer)
{
    CurGLCompositor.BindOutputTexture(buffer);
}

u32* GLRenderer::GetLine(int line)
{
    int stride = 256;

    if (line == 0)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
        u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (data) memcpy(&Framebuffer[stride*0], data, 4*stride*192);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    u64* ptr = (u64*)&Framebuffer[stride * line];
    for (int i = 0; i < stride; i+=2)
    {
        u64 rgb = *ptr & 0x00FCFCFC00FCFCFC;
        u64 a = *ptr & 0xF8000000F8000000;

        *ptr++ = (rgb >> 2) | (a >> 3);
    }

    return &Framebuffer[stride * line];
}

void GLRenderer::SetupAccelFrame()
{
    glBindTexture(GL_TEXTURE_2D, ColorBufferTex);
}

}
