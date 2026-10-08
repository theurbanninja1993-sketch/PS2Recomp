#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace gsgpu
{
    namespace
    {
        constexpr uint32_t kLocalMemoryBytes = 4u * 1024u * 1024u;
        constexpr uint32_t kPageBytes = 8192u;
        constexpr uint32_t kMaxTargetHeight = 1024u;

        // PSM codes (GS TEX0/FRAME/ZBUF encodings; Z formats carry 0x30).
        constexpr uint8_t PSM_CT32 = 0x00, PSM_CT24 = 0x01, PSM_CT16 = 0x02, PSM_CT16S = 0x0A;
        constexpr uint8_t PSM_T8 = 0x13, PSM_T4 = 0x14, PSM_T8H = 0x1B, PSM_T4HL = 0x24, PSM_T4HH = 0x2C;
        constexpr uint8_t PSM_Z32 = 0x30, PSM_Z24 = 0x31, PSM_Z16 = 0x32, PSM_Z16S = 0x3A;

        struct PageShape
        {
            uint32_t width, height;
        };

        PageShape pageShape(uint8_t psm)
        {
            switch (psm)
            {
            case PSM_CT16:
            case PSM_CT16S:
            case PSM_Z16:
            case PSM_Z16S:
                return {64u, 64u};
            case PSM_T8:
                return {128u, 64u};
            case PSM_T4:
                return {128u, 128u};
            default:
                return {64u, 32u};
            }
        }

        bool is16Bit(uint8_t psm)
        {
            return psm == PSM_CT16 || psm == PSM_CT16S || psm == PSM_Z16 || psm == PSM_Z16S;
        }

        bool isPalettized(uint8_t psm)
        {
            return psm == PSM_T8 || psm == PSM_T4 || psm == PSM_T8H || psm == PSM_T4HL || psm == PSM_T4HH;
        }

        // Byte range of a buffer: whole pages from the base page.
        void bufferRange(uint32_t block, uint32_t bw, uint32_t height, uint8_t psm, uint32_t &begin, uint32_t &end)
        {
            const PageShape shape = pageShape(psm);
            const uint32_t strideWidth = std::max<uint32_t>(bw, 1u) * 64u;
            const uint32_t pagesWide = (strideWidth + shape.width - 1u) / shape.width;
            const uint32_t pagesHigh = (std::max<uint32_t>(height, 1u) + shape.height - 1u) / shape.height;
            const uint32_t baseBytes = block * 256u;
            begin = baseBytes & ~(kPageBytes - 1u);
            const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
            const uint64_t bytes = static_cast<uint64_t>(pagesWide) * pagesHigh * kPageBytes + misalign;
            end = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(begin) + bytes, kLocalMemoryBytes));
        }

        uint32_t rgba5551To8888(uint32_t c)
        {
            const uint32_t r = ((c >> 0) & 0x1Fu) << 3;
            const uint32_t g = ((c >> 5) & 0x1Fu) << 3;
            const uint32_t b = ((c >> 10) & 0x1Fu) << 3;
            const uint32_t a = ((c >> 15) & 0x01u) << 7;
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        uint32_t rgba8888To5551(uint32_t c)
        {
            const uint32_t r = ((c >> 0) & 0xFFu) >> 3;
            const uint32_t g = ((c >> 8) & 0xFFu) >> 3;
            const uint32_t b = ((c >> 16) & 0xFFu) >> 3;
            const uint32_t a = ((c >> 24) & 0xFFu) >> 7;
            return r | (g << 5) | (b << 10) | (a << 15);
        }

        uint64_t fnv(uint64_t hash, uint64_t value)
        {
            for (int i = 0; i < 8; ++i)
            {
                hash ^= (value >> (8 * i)) & 0xFFu;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        const char *kVertexShader = R"(#version 460 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in uint a_z;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_stq;
layout(location = 4) in float a_fog;
uniform vec2 u_targetSize;
out vec4 v_color;
noperspective out vec3 v_stq;
noperspective out float v_fog;
void main()
{
    vec2 ndc = a_pos / u_targetSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, float(a_z) * (1.0 / 4294967296.0), 1.0);
    v_color = a_color;
    v_stq = a_stq;
    v_fog = a_fog;
}
)";

        const char *kFragmentShader = R"(#version 460 core
in vec4 v_color;
noperspective in vec3 v_stq;
noperspective in float v_fog;
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;
layout(binding = 0) uniform sampler2D u_tex;
layout(binding = 1) uniform sampler2D u_target;
uniform int u_tme, u_tfx, u_tcc, u_fge;
uniform int u_ate, u_atst, u_aref, u_afail;
uniform int u_abe, u_csel, u_pabe, u_date, u_datm, u_fba;
uniform vec4 u_blend; // s_k, s_c, d_k, d_c (alpha_s = C*s_k + s_c, alpha_d = C*d_k + d_c)
uniform float u_fix;
uniform vec3 u_fogColor;

void main()
{
    vec4 cf = floor(v_color * 255.0 + 0.5);
    vec4 c = cf;
    if (u_tme != 0)
    {
        vec4 ct = floor(texture(u_tex, v_stq.xy / v_stq.z) * 255.0 + 0.5);
        if (u_tfx == 0)
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0), 255.0);
            c.a = (u_tcc != 0) ? min(floor(ct.a * cf.a / 128.0), 255.0) : cf.a;
        }
        else if (u_tfx == 1)
        {
            c.rgb = ct.rgb;
            c.a = (u_tcc != 0) ? ct.a : cf.a;
        }
        else
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0) + cf.a, 255.0);
            c.a = (u_tcc != 0) ? ((u_tfx == 2) ? min(ct.a + cf.a, 255.0) : ct.a) : cf.a;
        }
    }
    if (u_fge != 0)
    {
        float f = floor(v_fog * 255.0 + 0.5);
        c.rgb = floor(f * c.rgb / 256.0) + floor((255.0 - f) * u_fogColor / 256.0);
    }

    if (u_ate != 0)
    {
        float aref = float(u_aref);
        bool pass = true;
        switch (u_atst)
        {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = c.a < aref; break;
        case 3: pass = c.a <= aref; break;
        case 4: pass = c.a == aref; break;
        case 5: pass = c.a >= aref; break;
        case 6: pass = c.a > aref; break;
        default: pass = c.a != aref; break;
        }
        // AFAIL: 0 KEEP; FB_ONLY / RGB_ONLY still write colour (Z write is
        // not per-fragment controllable here); ZB_ONLY drops colour.
        if (!pass && (u_afail == 0 || u_afail == 2))
            discard;
    }

    if (u_date != 0)
    {
        float ad = texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).a * 255.0;
        if ((ad >= 128.0) != (u_datm != 0))
            discard;
    }

    float storedAlpha = c.a;
    if (u_fba != 0)
        storedAlpha = max(storedAlpha, mod(storedAlpha, 128.0) + 128.0);

    if (u_abe != 0)
    {
        float cAlpha = (u_csel == 0) ? c.a
                     : (u_csel == 1) ? floor(texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5)
                     : u_fix;
        float k = cAlpha / 128.0;
        float alphaS = k * u_blend.x + u_blend.y;
        float alphaD = k * u_blend.z + u_blend.w;
        if (u_pabe != 0 && c.a < 128.0)
        {
            alphaS = 1.0;
            alphaD = 0.0;
        }
        o_color = vec4(min(c.rgb * abs(alphaS), 255.0) / 255.0, storedAlpha / 255.0);
        o_factor = vec4(clamp(abs(alphaD), 0.0, 1.0));
    }
    else
    {
        o_color = vec4(c.rgb / 255.0, storedAlpha / 255.0);
        o_factor = vec4(0.0);
    }
}
)";

        GLuint compileShader(GL &gl, GLenum type, const char *source)
        {
            GLuint shader = gl.glCreateShader(type);
            gl.glShaderSource(shader, 1, &source, nullptr);
            gl.glCompileShader(shader);
            GLint ok = 0;
            gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                std::cerr << "[gs:gpu] shader compile error:\n"
                          << log << std::endl;
            }
            return shader;
        }
    }

    bool Renderer::DrawKey::operator==(const DrawKey &other) const
    {
        return std::memcmp(this, &other, sizeof(DrawKey)) == 0;
    }

    Renderer::Renderer()
        : m_cpu(std::make_unique<GSCpuBackend>())
    {
        std::string error;
        if (m_context.create(error))
        {
            const auto *renderer = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_RENDERER));
            const auto *version = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_VERSION));
            std::cerr << "[gs:gpu] OpenGL " << (version ? version : "?") << " on " << (renderer ? renderer : "?")
                      << std::endl;
            m_gpu = std::getenv("PS2X_GS_GPU_CPU") == nullptr; // debug: keep the CPU path
            if (m_gpu)
                initGpuObjects();
        }
        else
        {
            std::cerr << "[gs:gpu] no GPU context (" << error << "); using the CPU rasterizer" << std::endl;
        }
    }

    Renderer::~Renderer()
    {
        if (std::getenv("PS2X_GS_GPU_STATS"))
            std::cerr << "[gs:gpu] draws=" << m_statDraws << " downloads=" << m_statDownloads << " uploads=" << m_statUploads
                      << " textureDecodes=" << m_statTextureDecodes << std::endl;
    }

    void Renderer::initGpuObjects()
    {
        GL &gl = m_context.gl;
        gl.glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

        GLuint vs = compileShader(gl, GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = compileShader(gl, GL_FRAGMENT_SHADER, kFragmentShader);
        m_program = gl.glCreateProgram();
        gl.glAttachShader(m_program, vs);
        gl.glAttachShader(m_program, fs);
        gl.glLinkProgram(m_program);
        GLint ok = 0;
        gl.glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[4096];
            gl.glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
            std::cerr << "[gs:gpu] program link error:\n"
                      << log << std::endl;
        }
        gl.glDeleteShader(vs);
        gl.glDeleteShader(fs);

        gl.glCreateVertexArrays(1, &m_vao);
        gl.glEnableVertexArrayAttrib(m_vao, 0);
        gl.glVertexArrayAttribFormat(m_vao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, x));
        gl.glEnableVertexArrayAttrib(m_vao, 1);
        gl.glVertexArrayAttribIFormat(m_vao, 1, 1, GL_UNSIGNED_INT, offsetof(Vertex, z));
        gl.glEnableVertexArrayAttrib(m_vao, 2);
        gl.glVertexArrayAttribFormat(m_vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(Vertex, rgba));
        gl.glEnableVertexArrayAttrib(m_vao, 3);
        gl.glVertexArrayAttribFormat(m_vao, 3, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, s));
        gl.glEnableVertexArrayAttrib(m_vao, 4);
        gl.glVertexArrayAttribFormat(m_vao, 4, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, fog));
        for (GLuint attr = 0; attr < 5; ++attr)
            gl.glVertexArrayAttribBinding(m_vao, attr, 0);
    }

    // ------------------------------------------------------------------ targets

    Renderer::Target &Renderer::findOrCreateTarget(bool depth, uint32_t block, uint32_t bw, uint8_t psm)
    {
        const bool bits16 = is16Bit(psm);
        for (auto &target : m_targets)
        {
            if (target->depth == depth && target->block == block && target->bw == bw && is16Bit(target->psm) == bits16)
            {
                target->psm = psm;
                return *target;
            }
        }
        auto target = std::make_unique<Target>();
        target->depth = depth;
        target->block = block;
        target->bw = std::max<uint32_t>(bw, 1u);
        target->psm = psm;
        target->width = target->bw * 64u;
        const uint32_t bytesPerPixel = bits16 ? 2u : 4u;
        const uint32_t available = kLocalMemoryBytes - std::min(kLocalMemoryBytes, block * 256u);
        target->height = std::clamp<uint32_t>(available / (target->width * bytesPerPixel), 1u, kMaxTargetHeight);
        bufferRange(block, target->bw, target->height, psm, target->beginByte, target->endByte);

        GL &gl = m_context.gl;
        gl.glCreateTextures(GL_TEXTURE_2D, 1, &target->texture);
        gl.glTextureStorage2D(target->texture, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8,
                              static_cast<GLsizei>(target->width), static_cast<GLsizei>(target->height));
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        target->owner = Owner::CpuNewer;
        m_targets.push_back(std::move(target));
        return *m_targets.back();
    }

    Renderer::Target &Renderer::colorTarget(const GSFrameReg &frame)
    {
        return findOrCreateTarget(false, frame.fbp * 32u, frame.fbw, frame.psm);
    }

    Renderer::Target &Renderer::depthTarget(const GSZbufReg &zbuf, uint32_t bw)
    {
        return findOrCreateTarget(true, zbuf.zbp * 32u, bw, zbuf.psm);
    }

    GLuint Renderer::framebufferFor(const Target &color, const Target *depth)
    {
        const uint64_t key = (static_cast<uint64_t>(color.texture) << 32) | (depth ? depth->texture : 0u);
        auto it = m_framebuffers.find(key);
        if (it != m_framebuffers.end())
            return it->second;
        GL &gl = m_context.gl;
        GLuint fbo = 0;
        gl.glCreateFramebuffers(1, &fbo);
        gl.glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, color.texture, 0);
        if (depth)
            gl.glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth->texture, 0);
        gl.glNamedFramebufferDrawBuffer(fbo, GL_COLOR_ATTACHMENT0);
        if (gl.glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "[gs:gpu] incomplete framebuffer (color " << color.width << "x" << color.height << ", depth "
                      << (depth ? depth->width : 0u) << "x" << (depth ? depth->height : 0u) << ")" << std::endl;
        m_framebuffers.emplace(key, fbo);
        return fbo;
    }

    void Renderer::uploadTarget(Target &target)
    {
        ++m_statUploads;
        GL &gl = m_context.gl;
        std::vector<uint32_t> raw(static_cast<size_t>(target.width) * target.height);
        m_cpu->ReadVramRect(target.psm, target.block, target.bw, 0u, 0u, target.width, target.height, raw.data());
        if (target.depth)
        {
            std::vector<float> depth(raw.size());
            for (size_t i = 0; i < raw.size(); ++i)
                depth[i] = static_cast<float>(static_cast<double>(raw[i]) * (1.0 / 4294967296.0));
            gl.glTextureSubImage2D(target.texture, 0, 0, 0, static_cast<GLsizei>(target.width),
                                   static_cast<GLsizei>(target.height), GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
        }
        else
        {
            for (uint32_t &pixel : raw)
            {
                if (is16Bit(target.psm))
                    pixel = rgba5551To8888(pixel);
                else if (target.psm == PSM_CT24)
                    pixel = (pixel & 0x00FFFFFFu) | 0x80000000u;
            }
            gl.glTextureSubImage2D(target.texture, 0, 0, 0, static_cast<GLsizei>(target.width),
                                   static_cast<GLsizei>(target.height), GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        }
        target.owner = Owner::Synced;
        target.dirtyY0 = ~0u;
        target.dirtyY1 = 0;
    }

    void Renderer::downloadTarget(Target &target)
    {
        if (target.owner != Owner::GpuNewer || target.dirtyY0 > target.dirtyY1)
        {
            if (target.owner == Owner::GpuNewer)
                target.owner = Owner::Synced;
            return;
        }
        ++m_statDownloads;
        GL &gl = m_context.gl;
        const uint32_t y0 = target.dirtyY0;
        const uint32_t rows = std::min(target.dirtyY1, target.height - 1u) - y0 + 1u;
        std::vector<uint32_t> raw(static_cast<size_t>(target.width) * rows);
        if (target.depth)
        {
            std::vector<float> depth(raw.size());
            gl.glGetTextureSubImage(target.texture, 0, 0, static_cast<GLint>(y0), 0, static_cast<GLsizei>(target.width),
                                    static_cast<GLsizei>(rows), 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                                    static_cast<GLsizei>(depth.size() * sizeof(float)), depth.data());
            for (size_t i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<uint32_t>(std::min(std::llround(static_cast<double>(depth[i]) * 4294967296.0),
                                                        static_cast<long long>(0xFFFFFFFFu)));
        }
        else
        {
            gl.glGetTextureSubImage(target.texture, 0, 0, static_cast<GLint>(y0), 0, static_cast<GLsizei>(target.width),
                                    static_cast<GLsizei>(rows), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    static_cast<GLsizei>(raw.size() * sizeof(uint32_t)), raw.data());
            if (is16Bit(target.psm))
                for (uint32_t &pixel : raw)
                    pixel = rgba8888To5551(pixel);
        }
        m_cpu->WriteVramRect(target.psm, target.block, target.bw, 0u, y0, target.width, rows, raw.data());
        target.owner = Owner::Synced;
        target.dirtyY0 = ~0u;
        target.dirtyY1 = 0;

        // CPU memory changed: cached textures over it are stale.
        uint32_t begin = 0, end = 0;
        bufferRange(target.block, target.bw, target.height, target.psm, begin, end);
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (it->second.beginByte < end && begin < it->second.endByte)
            {
                gl.glDeleteTextures(1, &it->second.texture);
                it = m_textures.erase(it);
            }
            else
                ++it;
        }
    }

    void Renderer::syncRangeToCpu(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        if (!m_inFlush)
            flushBatch();
        for (auto &target : m_targets)
            if (target->owner == Owner::GpuNewer && target->beginByte < endByte && beginByte < target->endByte)
                downloadTarget(*target);
    }

    void Renderer::cpuWroteRange(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        for (auto &target : m_targets)
            if (target->beginByte < endByte && beginByte < target->endByte)
                target->owner = Owner::CpuNewer;
        GL &gl = m_context.gl;
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (it->second.beginByte < endByte && beginByte < it->second.endByte)
            {
                gl.glDeleteTextures(1, &it->second.texture);
                it = m_textures.erase(it);
            }
            else
                ++it;
        }
    }

    void Renderer::prepareTargetForDraw(Target &target)
    {
        // Other views of the same memory: their GPU data must reach the CPU
        // copy first, and they go stale once we draw here.
        bool aliasDownloaded = false;
        for (auto &other : m_targets)
        {
            if (other.get() == &target || other->depth != target.depth)
                continue;
            if (other->beginByte < target.endByte && target.beginByte < other->endByte)
            {
                if (other->owner == Owner::GpuNewer)
                {
                    downloadTarget(*other);
                    aliasDownloaded = true;
                }
                other->owner = Owner::CpuNewer;
            }
        }
        if (aliasDownloaded && target.owner != Owner::CpuNewer)
            target.owner = Owner::CpuNewer;
        if (target.owner == Owner::CpuNewer)
            uploadTarget(target);
    }

    // ----------------------------------------------------------------- textures

    GLuint Renderer::textureFor(const GSDrawState &state, const DrawKey &key)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint64_t v : {uint64_t(key.tbp0), uint64_t(key.tbw), uint64_t(key.tpsm), uint64_t(key.tw), uint64_t(key.th),
                           uint64_t(key.cbp), uint64_t(key.cpsm), uint64_t(key.csm), uint64_t(key.csa), key.clutVersion,
                           uint64_t(key.ta0), uint64_t(key.ta1), uint64_t(key.aem)})
            hash = fnv(hash, v);
        auto it = m_textures.find(hash);
        if (it != m_textures.end())
            return it->second.texture;

        const uint32_t width = state.textureWidth;
        const uint32_t height = state.textureHeight;
        uint32_t begin = 0, end = 0;
        bufferRange(key.tbp0, key.tbw, height, static_cast<uint8_t>(key.tpsm), begin, end);
        syncRangeToCpu(begin, end); // render-to-texture: GPU data must reach memory first
        if (isPalettized(static_cast<uint8_t>(key.tpsm)))
        {
            const uint32_t clutBegin = key.cbp * 256u;
            syncRangeToCpu(clutBegin, clutBegin + 1024u);
        }

        ++m_statTextureDecodes;
        std::vector<uint32_t> texels(static_cast<size_t>(width) * height);
        m_cpu->DecodeTexture(state, width, height, texels.data());
        GL &gl = m_context.gl;
        CachedTexture cached;
        cached.width = width;
        cached.height = height;
        cached.beginByte = begin;
        cached.endByte = end;
        gl.glCreateTextures(GL_TEXTURE_2D, 1, &cached.texture);
        gl.glTextureStorage2D(cached.texture, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        gl.glTextureSubImage2D(cached.texture, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                               GL_UNSIGNED_BYTE, texels.data());
        m_textures.emplace(hash, cached);
        return cached.texture;
    }

    // -------------------------------------------------------------------- draws

    void Renderer::applyDrawState(const DrawKey &key, const GSDrawState &state)
    {
        GL &gl = m_context.gl;
        const GSContext &ctx = state.context;

        Target &color = colorTarget(ctx.frame);
        const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        const bool useDepth = ztst != 1u || !ctx.zbuf.zmask;
        Target *depth = useDepth ? &depthTarget(ctx.zbuf, ctx.frame.fbw) : nullptr;
        prepareTargetForDraw(color);
        if (depth)
            prepareTargetForDraw(*depth);
        m_drawColor = &color;
        m_drawDepth = depth;

        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebufferFor(color, depth));
        gl.glViewport(0, 0, static_cast<GLsizei>(color.width), static_cast<GLsizei>(color.height));
        gl.glEnable(GL_SCISSOR_TEST);
        const int x0 = std::max(0, key.scissor[0]), y0 = std::max(0, key.scissor[1]);
        const int x1 = std::min<int>(key.scissor[2], static_cast<int>(color.width) - 1);
        const int y1 = std::min<int>(key.scissor[3], static_cast<int>(color.height) - 1);
        gl.glScissor(x0, y0, std::max(0, x1 - x0 + 1), std::max(0, y1 - y0 + 1));
        m_drawY0 = y0;
        m_drawY1 = y1;

        // Depth: GS Z grows towards the viewer.
        if (depth)
        {
            gl.glEnable(GL_DEPTH_TEST);
            static const GLenum funcs[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
            gl.glDepthFunc(funcs[ztst]);
            gl.glDepthMask(ctx.zbuf.zmask ? GL_FALSE : GL_TRUE);
        }
        else
        {
            gl.glDisable(GL_DEPTH_TEST);
        }

        // Frame write mask (whole channels only for now).
        const uint32_t fbmsk = ctx.frame.fbmsk;
        gl.glColorMask((fbmsk & 0xFFu) != 0xFFu, (fbmsk & 0xFF00u) != 0xFF00u, (fbmsk & 0xFF0000u) != 0xFF0000u,
                       (fbmsk & 0xFF000000u) != 0xFF000000u && ctx.frame.psm != PSM_CT24);

        // Blend: Cv = (A - B) * C / 128 + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}.
        gl.glUseProgram(m_program);
        auto set1 = [&](const char *name, int value)
        { gl.glProgramUniform1i(m_program, gl.glGetUniformLocation(m_program, name), value); };
        const uint64_t alpha = ctx.alpha;
        const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
        const float sk = float(a == 0u) - float(b == 0u), sc = float(d == 0u);
        const float dk = float(a == 1u) - float(b == 1u), dc = float(d == 1u);
        if (state.prim.abe)
        {
            gl.glEnable(GL_BLEND);
            GLenum equation = GL_FUNC_ADD;
            if (dk < 0.0f && dc == 0.0f)
                equation = GL_FUNC_SUBTRACT; // Cs*as - Cd*|ad|
            else if (sk < 0.0f && sc == 0.0f)
                equation = GL_FUNC_REVERSE_SUBTRACT; // Cd*ad - Cs*|as|
            gl.glBlendEquationSeparate(equation, GL_FUNC_ADD);
            gl.glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        }
        else
        {
            gl.glDisable(GL_BLEND);
        }
        gl.glProgramUniform4f(m_program, gl.glGetUniformLocation(m_program, "u_blend"), sk, sc, dk, dc);
        gl.glProgramUniform1i(m_program, gl.glGetUniformLocation(m_program, "u_abe"), state.prim.abe ? 1 : 0);
        set1("u_csel", static_cast<int>(c));
        gl.glProgramUniform1i(m_program, gl.glGetUniformLocation(m_program, "u_pabe"), state.pabe ? 1 : 0);
        gl.glProgramUniform2f(m_program, gl.glGetUniformLocation(m_program, "u_targetSize"), static_cast<float>(color.width),
                              static_cast<float>(color.height));
        gl.glProgramUniform4f(m_program, gl.glGetUniformLocation(m_program, "u_blend"), sk, sc, dk, dc);
        {
            const float fix = static_cast<float>((alpha >> 32) & 0xFFu);
            gl.glProgramUniform1f(m_program, gl.glGetUniformLocation(m_program, "u_fix"), fix);
        }

        // Tests.
        const uint64_t test = ctx.test;
        set1("u_ate", static_cast<int>(test & 1u));
        set1("u_atst", static_cast<int>((test >> 1) & 7u));
        set1("u_aref", static_cast<int>((test >> 4) & 0xFFu));
        set1("u_afail", static_cast<int>((test >> 12) & 3u));
        const bool date = ((test >> 14) & 1u) != 0u;
        set1("u_date", date ? 1 : 0);
        set1("u_datm", static_cast<int>((test >> 15) & 1u));
        set1("u_fba", (ctx.fba & 1u) != 0u && ctx.frame.psm == PSM_CT32 ? 1 : 0);

        // Destination reads (DATE, Ad blending) sample the bound target.
        const bool readsTarget = date || (state.prim.abe && c == 1u);
        if (readsTarget)
        {
            gl.glBindTextureUnit(1, color.texture);
            gl.glTextureBarrier();
        }

        // Texture.
        set1("u_tme", state.prim.tme ? 1 : 0);
        set1("u_fge", state.prim.fge ? 1 : 0);
        set1("u_tfx", static_cast<int>(ctx.tex0.tfx));
        set1("u_tcc", static_cast<int>(ctx.tex0.tcc));
        gl.glProgramUniform3f(m_program, gl.glGetUniformLocation(m_program, "u_fogColor"), float(state.fogR),
                              float(state.fogG), float(state.fogB));
        if (state.prim.tme)
        {
            GLuint texture = textureFor(state, key);
            // textureFor may download targets and change bindings; rebind.
            gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebufferFor(color, depth));
            const GLint filter = state.linearFilter ? GL_LINEAR : GL_NEAREST;
            gl.glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, filter);
            gl.glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, filter);
            const uint32_t wms = ctx.clamp & 3u, wmt = (ctx.clamp >> 2) & 3u;
            gl.glTextureParameteri(texture, GL_TEXTURE_WRAP_S, (wms == 0u || wms == 3u) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            gl.glTextureParameteri(texture, GL_TEXTURE_WRAP_T, (wmt == 0u || wmt == 3u) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
            gl.glBindTextureUnit(0, texture);
        }
        gl.glBindVertexArray(m_vao);
    }

    void Renderer::flushBatch()
    {
        if (!m_gpu || m_vertices.empty())
        {
            m_vertices.clear();
            m_haveKey = false;
            return;
        }
        GL &gl = m_context.gl;
        m_inFlush = true;
        applyDrawState(m_key, m_keyState);
        m_inFlush = false;

        const size_t bytes = m_vertices.size() * sizeof(Vertex);
        if (bytes > m_vboCapacity)
        {
            if (m_vbo)
                gl.glDeleteBuffers(1, &m_vbo);
            m_vboCapacity = std::max<size_t>(bytes * 2u, 1u << 20);
            gl.glCreateBuffers(1, &m_vbo);
            gl.glNamedBufferStorage(m_vbo, static_cast<GLsizeiptr>(m_vboCapacity), nullptr, GL_DYNAMIC_STORAGE_BIT);
        }
        gl.glNamedBufferSubData(m_vbo, 0, static_cast<GLsizeiptr>(bytes), m_vertices.data());
        gl.glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, sizeof(Vertex));
        gl.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size()));
        ++m_statDraws;

        // The targets now hold GPU-only data in the drawn rows.
        auto markDirty = [&](Target *target)
        {
            if (!target)
                return;
            target->owner = Owner::GpuNewer;
            target->dirtyY0 = std::min<uint32_t>(target->dirtyY0, static_cast<uint32_t>(std::max(0, m_drawY0)));
            target->dirtyY1 = std::max<uint32_t>(target->dirtyY1, static_cast<uint32_t>(std::max(0, m_drawY1)));
        };
        markDirty(m_drawColor);
        if (m_drawDepth && !m_keyState.context.zbuf.zmask)
            markDirty(m_drawDepth);

        m_vertices.clear();
        m_haveKey = false;
    }

    void Renderer::submit(const GSPrimitiveBatch &batch)
    {
        if (!m_gpu)
        {
            m_cpu->Submit(batch);
            return;
        }
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        const GSPrimType type = state.prim.type;
        if (type == GS_PRIM_POINT || type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP)
            return; // TODO: points and lines (unused by VCS so far)

        DrawKey key{};
        key.frameBlock = ctx.frame.fbp * 32u;
        key.frameBw = ctx.frame.fbw;
        key.framePsm = ctx.frame.psm;
        key.fbmsk = ctx.frame.fbmsk;
        key.zBlock = ctx.zbuf.zbp * 32u;
        key.zPsm = ctx.zbuf.psm;
        key.zmask = ctx.zbuf.zmask;
        key.zte = static_cast<uint32_t>((ctx.test >> 16) & 1u);
        key.scissor[0] = ctx.scissor.x0;
        key.scissor[1] = ctx.scissor.y0;
        key.scissor[2] = ctx.scissor.x1;
        key.scissor[3] = ctx.scissor.y1;
        key.test = ctx.test;
        key.alpha = state.prim.abe ? ctx.alpha : 0u;
        key.fba = ctx.fba;
        key.tme = state.prim.tme;
        key.fge = state.prim.fge;
        key.abe = state.prim.abe;
        key.pabe = state.pabe;
        if (state.prim.tme)
        {
            key.clamp = ctx.clamp;
            key.tex1 = ctx.tex1;
            key.tbp0 = ctx.tex0.tbp0;
            key.tbw = ctx.tex0.tbw;
            key.tpsm = ctx.tex0.psm;
            key.tw = ctx.tex0.tw;
            key.th = ctx.tex0.th;
            key.tcc = ctx.tex0.tcc;
            key.tfx = ctx.tex0.tfx;
            if (isPalettized(ctx.tex0.psm))
            {
                key.cbp = ctx.tex0.cbp;
                key.cpsm = ctx.tex0.cpsm;
                key.csm = ctx.tex0.csm;
                key.csa = ctx.tex0.csa;
                key.clutVersion = m_cpu->ClutVersion();
            }
            key.ta0 = state.texa.ta0;
            key.ta1 = state.texa.ta1;
            key.aem = state.texa.aem;
            key.linear = state.linearFilter;
        }
        if (state.prim.fge)
            key.fogColor = state.fogR | (state.fogG << 8) | (state.fogB << 16);

        if (m_haveKey && !(key == m_key))
            flushBatch();
        if (!m_haveKey)
        {
            m_key = key;
            m_keyState = state;
            m_haveKey = true;
        }

        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        const float texW = static_cast<float>(state.textureWidth);
        const float texH = static_cast<float>(state.textureHeight);
        auto makeVertex = [&](const GSVertex &v, const GSVertex &colorSource) -> Vertex
        {
            Vertex out{};
            out.x = v.x - ofx;
            out.y = v.y - ofy;
            out.z = static_cast<uint32_t>(std::clamp(v.z + 0.5, 0.0, 4294967295.0));
            out.rgba = colorSource.r | (colorSource.g << 8) | (colorSource.b << 16) | (static_cast<uint32_t>(colorSource.a) << 24);
            if (state.prim.fst)
            {
                out.s = (static_cast<float>(v.u) / 16.0f) / texW;
                out.t = (static_cast<float>(v.v) / 16.0f) / texH;
                out.q = 1.0f;
            }
            else
            {
                out.s = v.s;
                out.t = v.t;
                out.q = v.q;
            }
            out.fog = static_cast<float>(v.fog) / 255.0f;
            return out;
        };

        if (type == GS_PRIM_SPRITE)
        {
            if (batch.vertexCount < 2u)
                return;
            const GSVertex &v0 = batch.vertices[0];
            const GSVertex &v1 = batch.vertices[1];
            Vertex a = makeVertex(v0, v1);
            Vertex b = makeVertex(v1, v1);
            // Sprite corners: texel coordinates per corner (STQ divided per vertex).
            if (state.prim.fst)
            {
                a.s = static_cast<float>(v0.u >> 4) / texW;
                a.t = static_cast<float>(v0.v >> 4) / texH;
                b.s = static_cast<float>(v1.u >> 4) / texW;
                b.t = static_cast<float>(v1.v >> 4) / texH;
            }
            else
            {
                const float q0 = std::fabs(v0.q) > 0.0f ? std::fabs(v0.q) : 1.0f;
                const float q1 = std::fabs(v1.q) > 0.0f ? std::fabs(v1.q) : 1.0f;
                a.s = v0.s / q0;
                a.t = v0.t / q0;
                b.s = v1.s / q1;
                b.t = v1.t / q1;
            }
            a.q = b.q = 1.0f;
            a.z = b.z;
            a.fog = b.fog;
            Vertex c = a, d = a;
            c.x = b.x; // (x1, y0)
            c.s = b.s;
            d.y = b.y; // (x0, y1)
            d.t = b.t;
            m_vertices.push_back(a);
            m_vertices.push_back(c);
            m_vertices.push_back(d);
            m_vertices.push_back(c);
            m_vertices.push_back(b);
            m_vertices.push_back(d);
            return;
        }

        if (batch.vertexCount < 3u)
            return;
        // Flat shading uses the last vertex's colour.
        const GSVertex &flat = batch.vertices[2];
        for (int i = 0; i < 3; ++i)
            m_vertices.push_back(makeVertex(batch.vertices[i], state.prim.iip ? batch.vertices[i] : flat));
    }

    // ------------------------------------------------------------- memory side

    void Renderer::initialize(uint8_t *vram, uint32_t vramSize)
    {
        m_vram = vram;
        m_cpu->Initialize(vram, vramSize);
    }

    void Renderer::reset()
    {
        flushBatch();
        m_cpu->Reset();
        for (auto &target : m_targets)
            target->owner = Owner::CpuNewer;
    }

    void Renderer::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        if (m_gpu && tex0.cld != 0u)
        {
            const uint32_t begin = tex0.cbp * 256u;
            syncRangeToCpu(begin, begin + 1024u);
        }
        m_cpu->LoadClut(tex0, texclut);
    }

    void Renderer::beginTransfer(const GSTransferCommand &command)
    {
        if (m_gpu)
        {
            flushBatch();
            const auto &buf = command.bitbltbuf;
            if (command.direction == 0u || command.direction == 2u)
            {
                bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh, buf.dpsm,
                            m_transferBegin, m_transferEnd);
                // Partial writes must merge with GPU-rendered data.
                syncRangeToCpu(m_transferBegin, m_transferEnd);
                m_transferActive = true;
            }
            if (command.direction == 1u || command.direction == 2u)
            {
                uint32_t begin = 0, end = 0;
                bufferRange(buf.sbp, buf.sbw, static_cast<uint32_t>(command.trxpos.ssay) + command.trxreg.rrh, buf.spsm,
                            begin, end);
                syncRangeToCpu(begin, end);
            }
        }
        m_cpu->BeginTransfer(command);
        if (m_gpu && command.direction == 2u)
        {
            cpuWroteRange(m_transferBegin, m_transferEnd); // local->local runs immediately
            m_transferActive = false;
        }
    }

    void Renderer::uploadImage(const uint8_t *data, uint32_t sizeBytes)
    {
        m_cpu->UploadImage(data, sizeBytes);
        if (m_gpu && m_transferActive)
            cpuWroteRange(m_transferBegin, m_transferEnd);
    }

    void Renderer::textureFlush()
    {
        m_cpu->TextureFlush();
    }

    void Renderer::syncLocalMemory()
    {
        syncRangeToCpu(0u, kLocalMemoryBytes);
    }

    PresentationFrame Renderer::present(const GSPresentationRequest &request)
    {
        // Presentation reads local memory (both circuits, preferred source).
        syncLocalMemory();
        m_cpu->Sync(GSSyncReason::Presentation);
        return m_cpu->Present(request);
    }

    bool Renderer::clearFramebuffer(const GSContext &context, uint32_t rgba)
    {
        syncLocalMemory();
        const bool cleared = m_cpu->ClearFramebuffer(context, rgba);
        cpuWroteRange(0u, kLocalMemoryBytes);
        return cleared;
    }

    uint32_t Renderer::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
    {
        return m_cpu->ConsumeLocalToHostBytes(dst, maxBytes);
    }

    uint32_t Renderer::readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        syncLocalMemory();
        return m_cpu->ReadVram(psm, base, bw, x, y);
    }

    void Renderer::writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        syncLocalMemory();
        m_cpu->WriteVram(psm, base, bw, x, y, value);
        cpuWroteRange(0u, kLocalMemoryBytes);
    }

    void Renderer::snapshotVram(std::vector<uint8_t> &out)
    {
        syncLocalMemory();
        m_cpu->SnapshotVram(out);
    }

    GSTransferSnapshot Renderer::transferSnapshot()
    {
        return m_cpu->GetTransferSnapshot();
    }
}
