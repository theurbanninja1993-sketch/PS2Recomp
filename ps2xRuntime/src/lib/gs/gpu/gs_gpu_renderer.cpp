#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include "raylib.h" // PS2X_TEXREPL_DUMP: ExportImage writes the dumped PNGs

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

std::atomic<uint32_t> g_gsGpuScaleRequest{0};
// The host window size, published by the main thread each frame (0 = unknown).
// The upscaled display readback is capped to it: the composed frame is only
// ever shown at window size, so transferring more than that is pure waste.
std::atomic<uint32_t> g_gsGpuPresentWidth{0};
std::atomic<uint32_t> g_gsGpuPresentHeight{0};
extern std::atomic<uint64_t> g_ps2PresentedFrameCounter; // EeScheduler.cpp (dump index)

// ------------------------------------------------------------- dump worker
// PS2X_TEXREPL_DUMP writes from a background thread: PNG encoding on the
// render thread caused visible stutter while hunting textures. The queue is
// bounded - under backlog new dumps are dropped (this is a hunting tool).
namespace
{
    struct DumpJob
    {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0;
        uint64_t frame = 0;
        char name[64] = {};
    };
    std::deque<DumpJob> g_dumpQueue;
    std::mutex g_dumpMutex;
    std::condition_variable g_dumpCv;
    std::atomic<bool> g_dumpStop{false};
    std::atomic<uint32_t> g_dumpQueued{0};

    void dumpWorkerThread()
    {
        const char *dir = std::getenv("PS2X_TEXREPL_DUMP");
        if (!dir || !*dir)
            return;
        FILE *index = std::fopen((std::string(dir) + "/_dump_index.log").c_str(), "a");
        for (;;)
        {
            DumpJob job;
            {
                std::unique_lock<std::mutex> lock(g_dumpMutex);
                g_dumpCv.wait(lock, []
                              { return g_dumpStop.load(std::memory_order_relaxed) || !g_dumpQueue.empty(); });
                if (g_dumpQueue.empty())
                {
                    if (g_dumpStop.load(std::memory_order_relaxed))
                        break;
                    continue;
                }
                job = std::move(g_dumpQueue.front());
                g_dumpQueue.pop_front();
            }
            ::Image image{job.rgba.data(), job.width, job.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            ExportImage(image, (std::string(dir) + "/" + job.name).c_str());
            if (index)
            {
                std::fprintf(index, "%s tbp0=%u tbw=%u psm=%u tw=%u th=%u frame=%llu\n", job.name, job.tbp0, job.tbw,
                             job.psm, job.tw, job.th, static_cast<unsigned long long>(job.frame));
                std::fflush(index);
            }
            g_dumpQueued.fetch_sub(1, std::memory_order_relaxed);
        }
        if (index)
            std::fclose(index);
    }

    void enqueueDump(DumpJob &&job)
    {
        static std::thread *s_worker = []()
        {
            auto *thread = new std::thread(dumpWorkerThread);
            thread->detach(); // runs until process exit
            return thread;
        }();
        (void)s_worker;
        {
            std::lock_guard<std::mutex> lock(g_dumpMutex);
            if (g_dumpQueued.load(std::memory_order_relaxed) > 256u)
                return; // backlog: drop rather than grow memory
            g_dumpQueue.push_back(std::move(job));
            g_dumpQueued.fetch_add(1, std::memory_order_relaxed);
        }
        g_dumpCv.notify_one();
    }
}

namespace gsgpu
{
    namespace
    {
        // Adds the scope's duration to a stats counter.
        struct ScopeTimer
        {
            double &total;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            ~ScopeTimer() { total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
        };

        // PS2X_TEXREPL_STATS: replacement-related render-thread time (seconds),
        // reported and reset on each stats line. mirror = decoding uploads into
        // the CPU copy that hashing reads; hash = texture/CLUT hashing.
        double g_texreplMirrorSecs = 0.0;
        double g_texreplHashSecs = 0.0;
        // Bytes behind those costs, per GS format (PSM & 0x3F): mirrored =
        // upload stream bytes decoded into the CPU copy; hashed = bytes the
        // texture hash read. Decides which formats an optimization helps.
        uint64_t g_texreplMirrorBytes[64] = {};
        uint64_t g_texreplHashBytes[64] = {};

        constexpr uint32_t kLocalMemoryBytes = 4u * 1024u * 1024u;
        constexpr uint32_t kPageBytes = 8192u;
        constexpr uint32_t kMaxTargetHeight = 1024u;

        // PSM codes (GS TEX0/FRAME/ZBUF encodings; Z formats carry 0x30).
        constexpr uint8_t PSM_CT32 = 0x00, PSM_CT24 = 0x01, PSM_CT16 = 0x02, PSM_CT16S = 0x0A;
        constexpr uint8_t PSM_T8 = 0x13, PSM_T4 = 0x14, PSM_T8H = 0x1B, PSM_T4HL = 0x24, PSM_T4HH = 0x2C;
        constexpr uint8_t PSM_Z32 = 0x30, PSM_Z24 = 0x31, PSM_Z16 = 0x32, PSM_Z16S = 0x3A;

        // Swizzle tables uploaded to the GPU, one per page layout: pixel index
        // within a page for (x, y) at block 0 (base blocks add whole blocks).
        struct SwizzleClass
        {
            uint32_t width, height, offset;
        };
        constexpr SwizzleClass kSwizzleClasses[8] = {
            {64u, 32u, 0u},      // C32 (also C24, T8H, T4HL, T4HH)
            {64u, 32u, 2048u},   // Z32 (also Z24)
            {64u, 64u, 4096u},   // C16
            {64u, 64u, 8192u},   // C16S
            {64u, 64u, 12288u},  // Z16
            {64u, 64u, 16384u},  // Z16S
            {128u, 64u, 20480u}, // T8
            {128u, 128u, 28672u} // T4
        };
        constexpr uint32_t kSwizzleEntries = 28672u + 16384u;

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

        uint32_t bitsPerPixel(uint8_t psm)
        {
            return static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm)));
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

        // Builds the swizzle tables by reading a page of memory whose pixel
        // units hold their own index, so they match GSMem exactly.
        std::vector<uint32_t> buildSwizzleTables()
        {
            std::vector<uint32_t> tables(kSwizzleEntries, 0u);
            std::vector<uint8_t> memory(GSMem::MEMORY_SIZE, 0u);
            using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
            struct Probe
            {
                Reader read;
                uint32_t unitBits;
            };
            const Probe probes[8] = {{GSMem::ReadCT32, 32u}, {GSMem::ReadZ32, 32u}, {GSMem::ReadCT16, 16u},
                                     {GSMem::ReadCT16S, 16u}, {GSMem::ReadZ16, 16u}, {GSMem::ReadZ16S, 16u},
                                     {GSMem::ReadP8, 8u}, {GSMem::ReadP4, 4u}};
            for (uint32_t cls = 0; cls < 8u; ++cls)
            {
                const SwizzleClass &shape = kSwizzleClasses[cls];
                const uint32_t units = shape.width * shape.height;
                const uint32_t bw = shape.width / 64u;
                // Each pass stores `unitBits` bits of every unit's index.
                const uint32_t bitsNeeded = 32u - static_cast<uint32_t>(__builtin_clz(units - 1u));
                for (uint32_t shift = 0; shift < bitsNeeded; shift += probes[cls].unitBits)
                {
                    for (uint32_t unit = 0; unit < units; ++unit)
                    {
                        const uint32_t part = (unit >> shift) & ((probes[cls].unitBits == 32u) ? 0xFFFFFFFFu : ((1u << probes[cls].unitBits) - 1u));
                        const uint32_t bit = unit * probes[cls].unitBits;
                        if (probes[cls].unitBits == 32u)
                            std::memcpy(&memory[bit / 8u], &part, 4u);
                        else if (probes[cls].unitBits == 16u)
                        {
                            const uint16_t v = static_cast<uint16_t>(part);
                            std::memcpy(&memory[bit / 8u], &v, 2u);
                        }
                        else if (probes[cls].unitBits == 8u)
                            memory[bit / 8u] = static_cast<uint8_t>(part);
                        else
                            memory[bit / 8u] = static_cast<uint8_t>((memory[bit / 8u] & ~(0xFu << (bit % 8u))) |
                                                                    (part << (bit % 8u)));
                    }
                    for (uint32_t y = 0; y < shape.height; ++y)
                        for (uint32_t x = 0; x < shape.width; ++x)
                        {
                            const uint32_t value = probes[cls].read(memory.data(), 0u, bw, x, y);
                            tables[shape.offset + y * shape.width + x] |= value << shift;
                        }
                }
            }
            return tables;
        }

        // ------------------------------------------------------------ shaders

        // VRAM access shared by every shader: swizzled pixel addressing and
        // field reads/writes, mirroring GSMem::PixelStorageTraits.
        const char *kVramGlsl = R"(
layout(std430, binding = 0) coherent buffer VramBuffer { uint vram[]; };
layout(std430, binding = 1) readonly buffer SwizzleBuffer { uint swizzle[]; };

uint psmClass(uint psm)
{
    switch (psm)
    {
    case 0x30u: case 0x31u: return 1u;
    case 0x02u: return 2u;
    case 0x0Au: return 3u;
    case 0x32u: return 4u;
    case 0x3Au: return 5u;
    case 0x13u: return 6u;
    case 0x14u: return 7u;
    default: return 0u;
    }
}

const uint kClassWidth[8] = uint[8](64u, 64u, 64u, 64u, 64u, 64u, 128u, 128u);
const uint kClassHeight[8] = uint[8](32u, 32u, 64u, 64u, 64u, 64u, 64u, 128u);
const uint kClassOffset[8] = uint[8](0u, 2048u, 4096u, 8192u, 12288u, 16384u, 20480u, 28672u);

// Word, bit shift and mask of pixel (x, y) of a buffer.
void vramField(uint psm, uint base, uint bw, uint x, uint y, out uint word, out uint shift, out uint mask)
{
    uint cls = psmClass(psm);
    uint pw = kClassWidth[cls], ph = kClassHeight[cls];
    uint page = base / 32u + (y / ph) * ((bw * 64u) / pw) + x / pw;
    uint addr = page * (pw * ph) + (base % 32u) * ((pw * ph) / 32u) + swizzle[kClassOffset[cls] + (y % ph) * pw + (x % pw)];
    if (cls <= 1u)
    {
        word = ((addr * 4u) & 0x3FFFFCu) >> 2;
        shift = 0u;
        mask = 0xFFFFFFFFu;
        if (psm == 0x01u || psm == 0x31u) mask = 0x00FFFFFFu;
        else if (psm == 0x1Bu) { shift = 24u; mask = 0xFFu; }
        else if (psm == 0x24u) { shift = 24u; mask = 0xFu; }
        else if (psm == 0x2Cu) { shift = 28u; mask = 0xFu; }
    }
    else if (cls <= 5u)
    {
        uint byteAddr = (addr * 2u) & 0x3FFFFEu;
        word = byteAddr >> 2;
        shift = (byteAddr & 2u) * 8u;
        mask = 0xFFFFu;
    }
    else if (cls == 6u)
    {
        uint byteAddr = addr & 0x3FFFFFu;
        word = byteAddr >> 2;
        shift = (byteAddr & 3u) * 8u;
        mask = 0xFFu;
    }
    else
    {
        uint bits = addr * 4u;
        uint byteAddr = (bits >> 3) & 0x3FFFFFu;
        word = byteAddr >> 2;
        shift = (byteAddr & 3u) * 8u + (bits & 7u);
        mask = 0xFu;
    }
}

uint readVram(uint psm, uint base, uint bw, uint x, uint y)
{
    uint word, shift, mask;
    vramField(psm, base, bw, x, y, word, shift, mask);
    return (vram[word] >> shift) & mask;
}

void writeVram(uint psm, uint base, uint bw, uint x, uint y, uint value)
{
    uint word, shift, mask;
    vramField(psm, base, bw, x, y, word, shift, mask);
    if (mask == 0xFFFFFFFFu)
    {
        vram[word] = value;
    }
    else
    {
        // Neighbouring fields of the word may be written concurrently.
        atomicAnd(vram[word], ~(mask << shift));
        atomicOr(vram[word], (value & mask) << shift);
    }
}

uint rgba5551To8888(uint c)
{
    return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (((c >> 15) & 1u) << 31);
}

uint rgba8888To5551(uint c)
{
    return ((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) | ((((c >> 16) & 0xFFu) >> 3) << 10) | ((c >> 31) << 15);
}

bool is16BitPsm(uint psm)
{
    return psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au;
}
)";

        const char *kVertexShader = R"(#version 450 core
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

        const char *kFragmentShader = R"(
in vec4 v_color;
noperspective in vec3 v_stq;
noperspective in float v_fog;
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;
layout(binding = 1) uniform sampler2D u_target;
layout(binding = 3) uniform sampler2D u_rtTex;
layout(binding = 4) uniform sampler2D u_replTex;
uniform int u_replace; // the texture is a replacement image (texels with GS-scale alpha)
uniform int u_rtScale; // 0: texture read from local memory; else sampled from a render target at this scale
layout(std430, binding = 2) readonly buffer ClutBuffer { uint clut[]; };
uniform int u_tme, u_tfx, u_tcc, u_fge;
uniform uvec4 u_tex;      // TBP0, TBW, PSM, CPSM
uniform uvec4 u_tex2;     // CSM, CSA, WMS | WMT << 2, linear
uniform ivec4 u_region;   // MINU, MAXU, MINV, MAXV
uniform ivec2 u_texSize;
uniform uvec3 u_texa;     // TA0, TA1, AEM
uniform int u_ate, u_atst, u_aref, u_afail;
uniform int u_abe, u_csel, u_pabe, u_date, u_datm, u_fba;
uniform vec4 u_blend; // s_k, s_c, d_k, d_c (alpha_s = C*s_k + s_c, alpha_d = C*d_k + d_c)
uniform float u_fix;
uniform vec3 u_fogColor;

uint applyTexa(uint psm, uint texel)
{
    if (psm == 0x00u)
        return texel;
    bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint a = texel >> 24;
    if (psm == 0x01u)
        a = (u_texa.z != 0u && rgbZero) ? 0u : u_texa.x;
    else if (psm == 0x02u || psm == 0x0Au)
        a = ((a & 0x80u) != 0u) ? u_texa.y : ((u_texa.z != 0u && rgbZero) ? 0u : u_texa.x);
    return (texel & 0x00FFFFFFu) | (a << 24);
}

uint lookupClut(uint index)
{
    uint tpsm = u_tex.z, cpsm = u_tex.w, csm = u_tex2.x, csa = u_tex2.y;
    bool sixteenBit = cpsm == 0x02u || cpsm == 0x0Au;
    bool fourBit = tpsm == 0x14u || tpsm == 0x24u || tpsm == 0x2Cu;
    bool eightBit = tpsm == 0x13u || tpsm == 0x1Bu;
    uint clutBase = (csa & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4;
    uint sourceIndex = fourBit ? (index & 0x0Fu) : index;
    uint clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && eightBit)
        clutIndex = min((sourceIndex & 0xF0u) + clutBase, 240u) + (sourceIndex & 0x0Fu);
    if (cpsm == 0x00u)
        return clut[clutIndex] | (clut[clutIndex + 256u] << 16);
    if (cpsm == 0x01u)
        return applyTexa(cpsm, (clut[clutIndex] | (clut[clutIndex + 256u] << 16)) & 0x00FFFFFFu);
    if (sixteenBit)
        return applyTexa(cpsm, rgba5551To8888(clut[clutIndex]));
    return 0xFFFF00FFu;
}

int wrapCoordinate(int coordinate, int size, uint mode, int regionMin, int regionMax)
{
    if (mode == 0u) return int(uint(coordinate) & uint(size - 1));
    if (mode == 1u) return clamp(coordinate, 0, size - 1);
    if (mode == 2u) return min(max(coordinate, regionMin), regionMax);
    return int((uint(coordinate) & uint(regionMin)) | uint(regionMax));
}

uint texel(int u, int v)
{
    u = wrapCoordinate(u, u_texSize.x, u_tex2.z & 3u, u_region.x, u_region.y);
    v = wrapCoordinate(v, u_texSize.y, (u_tex2.z >> 2) & 3u, u_region.z, u_region.w);
    uint psm = u_tex.z;
    uint raw = readVram(psm, u_tex.x, u_tex.y, uint(u), uint(v));
    switch (psm)
    {
    case 0x00u: case 0x30u: case 0x01u: case 0x31u:
        return applyTexa(psm, raw);
    case 0x02u: case 0x0Au: case 0x32u: case 0x3Au:
        return applyTexa(psm, rgba5551To8888(raw));
    case 0x13u: case 0x1Bu: case 0x14u: case 0x24u: case 0x2Cu:
        return lookupClut(raw & 0xFFu);
    }
    return 0xFFFF00FFu;
}

// Two 8-bit channels (bits 0-7 and 16-23) blended with an 8-bit weight.
uint lerp2(uint a, uint b, uint w)
{
    uint lo = ((a & 0xFFu) * (256u - w) + (b & 0xFFu) * w + 128u) >> 8;
    uint hi = (((a >> 16) & 0xFFu) * (256u - w) + ((b >> 16) & 0xFFu) * w + 128u) >> 8;
    return lo | (hi << 16);
}

// Texel of a render target used as the texture, in target pixels: GS wrap
// modes apply to the GS texel the pixel belongs to, and the colour goes
// through the same format conversion as a local-memory read.
uint rtTexel(int uh, int vh)
{
    int u = int(floor(float(uh) / float(u_rtScale)));
    int v = int(floor(float(vh) / float(u_rtScale)));
    int su = uh - u * u_rtScale, sv = vh - v * u_rtScale;
    u = wrapCoordinate(u, u_texSize.x, u_tex2.z & 3u, u_region.x, u_region.y);
    v = wrapCoordinate(v, u_texSize.y, (u_tex2.z >> 2) & 3u, u_region.z, u_region.w);
    uint c = packUnorm4x8(texelFetch(u_rtTex, ivec2(u * u_rtScale + su, v * u_rtScale + sv), 0));
    uint psm = u_tex.z;
    return applyTexa(psm, is16BitPsm(psm) ? rgba5551To8888(rgba8888To5551(c)) : c);
}

uint sampleTexture(vec2 uv)
{
    if (u_rtScale != 0)
    {
        // A pixel drawn at GS position p is texel position p + 0.5, so
        // target pixel k (drawn at k / scale) sits at k / scale + 0.5.
        vec2 s = (uv - 0.5) * float(u_rtScale);
        if (u_tex2.w == 0u)
            return rtTexel(int(floor(s.x + 0.5)), int(floor(s.y + 0.5)));
        int u0 = int(floor(s.x)), v0 = int(floor(s.y));
        uint wx = uint((s.x - float(u0)) * 256.0 + 0.5);
        uint wy = uint((s.y - float(v0)) * 256.0 + 0.5);
        uint c00 = rtTexel(u0, v0), c10 = rtTexel(u0 + 1, v0), c01 = rtTexel(u0, v0 + 1), c11 = rtTexel(u0 + 1, v0 + 1);
        uint even = lerp2(lerp2(c00 & 0x00FF00FFu, c10 & 0x00FF00FFu, wx), lerp2(c01 & 0x00FF00FFu, c11 & 0x00FF00FFu, wx), wy);
        uint odd = lerp2(lerp2((c00 >> 8) & 0x00FF00FFu, (c10 >> 8) & 0x00FF00FFu, wx),
                         lerp2((c01 >> 8) & 0x00FF00FFu, (c11 >> 8) & 0x00FF00FFu, wx), wy);
        return even | (odd << 8);
    }
    if (u_tex2.w == 0u)
        return texel(int(uv.x), int(uv.y));
    vec2 s = uv - 0.5;
    int u0 = int(floor(s.x)), v0 = int(floor(s.y));
    uint wx = uint((s.x - float(u0)) * 256.0 + 0.5);
    uint wy = uint((s.y - float(v0)) * 256.0 + 0.5);
    uint c00 = texel(u0, v0), c10 = texel(u0 + 1, v0), c01 = texel(u0, v0 + 1), c11 = texel(u0 + 1, v0 + 1);
    uint even = lerp2(lerp2(c00 & 0x00FF00FFu, c10 & 0x00FF00FFu, wx), lerp2(c01 & 0x00FF00FFu, c11 & 0x00FF00FFu, wx), wy);
    uint odd = lerp2(lerp2((c00 >> 8) & 0x00FF00FFu, (c10 >> 8) & 0x00FF00FFu, wx),
                     lerp2((c01 >> 8) & 0x00FF00FFu, (c11 >> 8) & 0x00FF00FFu, wx), wy);
    return even | (odd << 8);
}

void main()
{
    vec4 cf = floor(v_color * 255.0 + 0.5);
    vec4 c = cf;
    if (u_tme != 0)
    {
        float q = abs(v_stq.z) > 1.0e-8 ? v_stq.z : 1.0;
        vec2 uv = v_stq.xy * (1.0 / q) * vec2(u_texSize);
        vec4 ct;
        if (u_replace != 0)
        {
            ct = floor(texture(u_replTex, uv / vec2(u_texSize)) * 255.0 + 0.5);
        }
        else
        {
            uint t = sampleTexture(uv);
            ct = vec4(float(t & 0xFFu), float((t >> 8) & 0xFFu), float((t >> 16) & 0xFFu), float(t >> 24));
        }
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

        // Upscaled display output: the PCRTC merge of the two read circuits
        // (circuit 2 or the background below, circuit 1 blended over it).
        const char *kComposeShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D u_circuit1;
layout(binding = 1) uniform sampler2D u_circuit2;
uniform ivec4 u_rect1; // read origin and size in target pixels; size 0 = circuit off
uniform ivec4 u_rect2;
uniform vec3 u_bg;
uniform vec2 u_mode;   // MMOD ALP factor (or < 0: circuit 1's alpha), SLBG
out vec4 o_color;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 dst = u_bg;
    if (u_rect2.z > 0 && u_mode.y == 0.0 && p.x < u_rect2.z && p.y < u_rect2.w)
        dst = texelFetch(u_circuit2, u_rect2.xy + p, 0).rgb;
    if (u_rect1.z > 0 && p.x < u_rect1.z && p.y < u_rect1.w)
    {
        vec4 src = texelFetch(u_circuit1, u_rect1.xy + p, 0);
        float factor = u_mode.x >= 0.0 ? u_mode.x : min(1.0, src.a * 2.0);
        dst = mix(dst, src.rgb, factor);
    }
    o_color = vec4(dst, 1.0);
}
)";

        // Full-target triangle for load passes (rows limited by the scissor).
        const char *kFullscreenVertexShader = R"(#version 450 core
void main()
{
    vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

        // VRAM -> colour target.
        const char *kLoadColorShader = R"(
uniform uvec3 u_buffer; // block, bw, psm
uniform uint u_scale;   // target pixels per GS pixel (nearest upscale)
out vec4 o_color;
void main()
{
    uint x = uint(gl_FragCoord.x) / u_scale, y = uint(gl_FragCoord.y) / u_scale;
    uint raw = readVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y);
    uint c = is16BitPsm(u_buffer.z) ? rgba5551To8888(raw)
           : (u_buffer.z == 0x01u ? ((raw & 0x00FFFFFFu) | 0x80000000u) : raw);
    o_color = unpackUnorm4x8(c);
}
)";

        // VRAM -> depth target.
        const char *kLoadDepthShader = R"(
uniform uvec3 u_buffer; // block, bw, psm
uniform uint u_scale;
void main()
{
    uint x = uint(gl_FragCoord.x) / u_scale, y = uint(gl_FragCoord.y) / u_scale;
    uint raw = readVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y);
    gl_FragDepth = float(double(raw) * (1.0LF / 4294967296.0LF));
}
)";

        // Target rows -> VRAM.
        const char *kResolveShader = R"(
layout(local_size_x = 64, local_size_y = 4) in;
layout(binding = 0) uniform sampler2D u_source;
uniform uvec4 u_buffer; // block, bw, psm, depth
uniform uvec3 u_rows;   // first row, row count, width
uniform uint u_scale;   // each GS pixel takes the target pixel at its sample point
void main()
{
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= u_rows.z || y >= u_rows.y)
        return;
    y += u_rows.x;
    vec4 t = texelFetch(u_source, ivec2(x * u_scale, y * u_scale), 0);
    uint value;
    if (u_buffer.w != 0u)
    {
        double d = double(t.r) * 4294967296.0LF;
        value = d >= 4294967295.0LF ? 0xFFFFFFFFu : uint(floor(d + 0.5LF));
    }
    else
    {
        uint c = packUnorm4x8(t);
        value = is16BitPsm(u_buffer.z) ? rgba8888To5551(c) : c;
    }
    writeVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y, value);
}
)";

        // Host -> local transfer of `count` pixels from the staging stream.
        const char *kHostToLocalShader = R"(
layout(local_size_x = 64) in;
layout(std430, binding = 3) readonly buffer StagingBuffer { uint staging[]; };
uniform uvec4 u_dest;  // DBP, DBW, DPSM, RRW
uniform uvec4 u_pos;   // DSAX, DSAY, first pixel, count
uniform uint u_bits;   // stream bits per pixel
uint stagingByte(uint i) { return (staging[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu; }
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_pos.w)
        return;
    uint value;
    if (u_bits == 32u)
        value = staging[i];
    else if (u_bits == 24u)
        value = stagingByte(i * 3u) | (stagingByte(i * 3u + 1u) << 8) | (stagingByte(i * 3u + 2u) << 16);
    else if (u_bits == 16u)
        value = (staging[i >> 1] >> ((i & 1u) * 16u)) & 0xFFFFu;
    else if (u_bits == 8u)
        value = stagingByte(i);
    else
        value = (stagingByte(i >> 1) >> ((i & 1u) * 4u)) & 0xFu;
    uint pixel = u_pos.z + i;
    writeVram(u_dest.z, u_dest.x, u_dest.y, u_pos.x + pixel % u_dest.w, u_pos.y + pixel / u_dest.w, value);
}
)";

        // Local -> local transfer; `u_serial` copies in order on one thread
        // when source and destination overlap.
        const char *kLocalToLocalShader = R"(
layout(local_size_x = 64) in;
uniform uvec4 u_source; // SBP, SBW, SPSM, SSAX
uniform uvec4 u_dest;   // DBP, DBW, DPSM, DSAX
uniform uvec4 u_rect;   // RRW, RRH, SSAY, DSAY
uniform uvec2 u_mode;   // DIR, serial
void copyPixel(uint pixel)
{
    uint x = pixel % u_rect.x, y = pixel / u_rect.x;
    if ((u_mode.x & 2u) != 0u) x = u_rect.x - x - 1u;
    if ((u_mode.x & 1u) != 0u) y = u_rect.y - y - 1u;
    uint value = readVram(u_source.z, u_source.x, u_source.y, x + u_source.w, y + u_rect.z);
    writeVram(u_dest.z, u_dest.x, u_dest.y, x + u_dest.w, y + u_rect.w, value);
}
void main()
{
    uint total = u_rect.x * u_rect.y;
    if (u_mode.y != 0u)
    {
        if (gl_GlobalInvocationID.x == 0u)
            for (uint pixel = 0u; pixel < total; ++pixel)
                copyPixel(pixel);
        return;
    }
    if (gl_GlobalInvocationID.x < total)
        copyPixel(gl_GlobalInvocationID.x);
}
)";

        // CLUT buffer load (GSCpuBackend::LoadClutUnlocked); one entry per
        // invocation. The CLUT holds 512 16-bit entries, one per uint.
        const char *kClutLoadShader = R"(
layout(local_size_x = 256) in;
layout(std430, binding = 2) buffer ClutBuffer { uint clut[]; };
uniform uvec4 u_clut;  // CPSM, CBP, CSM, CSA
uniform uvec4 u_clut2; // CBW, COU, COV, four-bit
void main()
{
    uint entry = gl_LocalInvocationID.x;
    uint cpsm = u_clut.x;
    bool fourBit = u_clut2.w != 0u;
    bool sixteenBit = cpsm == 0x02u || cpsm == 0x0Au;
    uint entryCount = fourBit ? 16u : 256u;
    uint destinationBase = (u_clut.w & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4;
    bool csm1Suffix = u_clut.z == 0u && !sixteenBit && !fourBit;
    uint firstEntry = csm1Suffix ? destinationBase : 0u;
    if (entry < firstEntry || entry >= entryCount)
        return;
    uint sourceX, sourceY, sourceWidth = 1u;
    if (u_clut.z == 0u)
    {
        uint index = (entry & ~0x18u) | ((entry & 0x08u) << 1) | ((entry & 0x10u) >> 1);
        sourceX = index & 0x0Fu;
        sourceY = index >> 4;
    }
    else
    {
        sourceWidth = u_clut2.x != 0u ? u_clut2.x : 1u;
        sourceX = (u_clut2.y << 4) + entry;
        sourceY = u_clut2.z;
    }
    uint raw = readVram(cpsm, u_clut.y, sourceWidth, sourceX, sourceY);
    uint destination = (csm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (sixteenBit)
    {
        clut[destination] = raw & 0xFFFFu;
    }
    else
    {
        clut[destination] = raw & 0xFFFFu;
        clut[destination + 256u] = raw >> 16;
    }
}
)";

        GLuint compileShader(GL &gl, GLenum type, const std::string &source)
        {
            GLuint shader = gl.glCreateShader(type);
            const char *text = source.c_str();
            gl.glShaderSource(shader, 1, &text, nullptr);
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

        GLuint linkProgram(GL &gl, std::initializer_list<GLuint> shaders)
        {
            GLuint program = gl.glCreateProgram();
            for (GLuint shader : shaders)
                gl.glAttachShader(program, shader);
            gl.glLinkProgram(program);
            GLint ok = 0;
            gl.glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                std::cerr << "[gs:gpu] program link error:\n"
                          << log << std::endl;
            }
            for (GLuint shader : shaders)
                gl.glDeleteShader(shader);
            return program;
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
            if (const char *env = std::getenv("PS2X_GS_SCALE"))
                g_gsGpuScaleRequest.store(static_cast<uint32_t>(std::atoi(env)), std::memory_order_relaxed);
            // PS2X_GS_PRESENT_SIZE=<w>x<h>: pretend the host window is this
            // size (tests the readback cap without a window).
            if (const char *env = std::getenv("PS2X_GS_PRESENT_SIZE"))
            {
                unsigned w = 0, h = 0;
                if (std::sscanf(env, "%ux%u", &w, &h) == 2)
                {
                    g_gsGpuPresentWidth.store(w, std::memory_order_relaxed);
                    g_gsGpuPresentHeight.store(h, std::memory_order_relaxed);
                }
            }
            m_scale = std::clamp<uint32_t>(g_gsGpuScaleRequest.load(std::memory_order_relaxed), 1u, 8u);
            m_requestedScale.store(m_scale, std::memory_order_relaxed);
        }
        else
        {
            std::cerr << "[gs:gpu] no GPU context (" << error << "); using the CPU rasterizer" << std::endl;
        }
    }

    Renderer::~Renderer()
    {
        stopReplacementWorker();
        if (m_gpu && m_vram)
        {
            flushBatch();
            readbackVram(); // leave local memory complete for the owner
        }
        if (std::getenv("PS2X_TEXREPL_STATS") && replacementsActive())
            emitTexreplStats();
        if (std::getenv("PS2X_GS_GPU_STATS"))
        {
            std::cerr << "[gs:gpu] draws=" << m_statDraws << " resolves=" << m_statResolves << " loads=" << m_statLoads
                      << " readbacks=" << m_statReadbacks << " clutLoads=" << m_statClutLoads
                      << " transfers=" << m_statTransfers << std::endl;
            std::cerr << "  time: readback=" << m_timeReadback << " draw=" << m_timeDraw << " present=" << m_timePresent
                      << std::endl;
            for (auto &t : m_targets)
                std::cerr << "  target block=" << t->block << " bw=" << t->bw << " psm=" << int(t->psm) << " depth=" << t->depth
                          << " rows=" << t->usedRows << " bytes=[" << t->beginByte << "," << t->endByte << ")" << std::endl;
            for (auto &r : m_statResolveReasons)
                std::cerr << "  resolves[" << r.first << "]=" << r.second << std::endl;
        }
    }

    GLint Renderer::uniform(Program &program, const char *name)
    {
        auto it = program.locations.find(name);
        if (it != program.locations.end())
            return it->second;
        const GLint location = m_context.gl.glGetUniformLocation(program.id, name);
        program.locations.emplace(name, location);
        return location;
    }

    void Renderer::initGpuObjects()
    {
        GL &gl = m_context.gl;
        gl.glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

        const std::string header = std::string("#version 450 core\n") + kVramGlsl;
        m_draw.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kVertexShader),
                                     compileShader(gl, GL_FRAGMENT_SHADER, header + kFragmentShader)});
        m_loadColor.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                          compileShader(gl, GL_FRAGMENT_SHADER, header + kLoadColorShader)});
        m_loadDepth.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                          compileShader(gl, GL_FRAGMENT_SHADER, header + kLoadDepthShader)});
        m_resolve.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kResolveShader)});
        m_hostToLocal.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kHostToLocalShader)});
        m_localToLocal.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kLocalToLocalShader)});
        m_clutLoad.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kClutLoadShader)});
        m_compose.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                        compileShader(gl, GL_FRAGMENT_SHADER, kComposeShader)});

        gl.glCreateBuffers(1, &m_vramBuffer);
        gl.glNamedBufferStorage(m_vramBuffer, kLocalMemoryBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
        const std::vector<uint32_t> swizzle = buildSwizzleTables();
        gl.glCreateBuffers(1, &m_swizzleBuffer);
        gl.glNamedBufferStorage(m_swizzleBuffer, static_cast<GLsizeiptr>(swizzle.size() * sizeof(uint32_t)), swizzle.data(), 0);
        const std::vector<uint32_t> clut(512u, 0u);
        gl.glCreateBuffers(1, &m_clutBuffer);
        gl.glNamedBufferStorage(m_clutBuffer, static_cast<GLsizeiptr>(clut.size() * sizeof(uint32_t)), clut.data(), 0);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_vramBuffer);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swizzleBuffer);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_clutBuffer);

        gl.glCreateVertexArrays(1, &m_emptyVao);
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
            if (target->depth == depth && target->block == block && target->bw == std::max<uint32_t>(bw, 1u) &&
                is16Bit(target->psm) == bits16)
            {
                if (target->psm != psm)
                {
                    // Same layout class, different format (e.g. CT32 <-> CT24):
                    // the texture's interpretation changes.
                    resolveTarget(*target);
                    target->psm = psm;
                    if (target->usedRows != 0u)
                    {
                        target->staleY0 = 0u;
                        target->staleY1 = target->usedRows - 1u;
                    }
                }
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

        GL &gl = m_context.gl;
        gl.glCreateTextures(GL_TEXTURE_2D, 1, &target->texture);
        gl.glTextureStorage2D(target->texture, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8,
                              static_cast<GLsizei>(target->width * m_scale), static_cast<GLsizei>(target->height * m_scale));
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.glCreateFramebuffers(1, &target->loadFramebuffer);
        if (depth)
        {
            gl.glNamedFramebufferTexture(target->loadFramebuffer, GL_DEPTH_ATTACHMENT, target->texture, 0);
            gl.glNamedFramebufferDrawBuffer(target->loadFramebuffer, GL_NONE);
        }
        else
        {
            gl.glNamedFramebufferTexture(target->loadFramebuffer, GL_COLOR_ATTACHMENT0, target->texture, 0);
            gl.glNamedFramebufferDrawBuffer(target->loadFramebuffer, GL_COLOR_ATTACHMENT0);
        }
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

    void Renderer::loadRows(Target &target, uint32_t y0, uint32_t rows)
    {
        if (rows == 0u)
            return;
        ++m_statLoads;
        GL &gl = m_context.gl;
        Program &program = target.depth ? m_loadDepth : m_loadColor;
        gl.glUseProgram(program.id);
        gl.glProgramUniform3ui(program.id, uniform(program, "u_buffer"), target.block, target.bw, target.psm);
        gl.glProgramUniform1ui(program.id, uniform(program, "u_scale"), m_scale);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.loadFramebuffer);
        gl.glViewport(0, 0, static_cast<GLsizei>(target.width * m_scale), static_cast<GLsizei>(target.height * m_scale));
        gl.glEnable(GL_SCISSOR_TEST);
        gl.glScissor(0, static_cast<GLint>(y0 * m_scale), static_cast<GLsizei>(target.width * m_scale),
                     static_cast<GLsizei>(rows * m_scale));
        gl.glDisable(GL_BLEND);
        if (target.depth)
        {
            gl.glEnable(GL_DEPTH_TEST);
            gl.glDepthFunc(GL_ALWAYS);
            gl.glDepthMask(GL_TRUE);
        }
        else
        {
            gl.glDisable(GL_DEPTH_TEST);
            gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        gl.glBindVertexArray(m_emptyVao);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void Renderer::resolveTarget(Target &target)
    {
        if (!target.gpuDirty())
            return;
        ++m_statResolves;
        ++m_statResolveReasons[m_syncReason];
        GL &gl = m_context.gl;
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT); // VRAM readers and the target's draws
        const uint32_t y0 = target.dirtyY0;
        const uint32_t rows = std::min(target.dirtyY1, target.height - 1u) - y0 + 1u;
        gl.glUseProgram(m_resolve.id);
        gl.glProgramUniform4ui(m_resolve.id, uniform(m_resolve, "u_buffer"), target.block, target.bw, target.psm,
                               target.depth ? 1u : 0u);
        gl.glProgramUniform3ui(m_resolve.id, uniform(m_resolve, "u_rows"), y0, rows, target.width);
        gl.glProgramUniform1ui(m_resolve.id, uniform(m_resolve, "u_scale"), m_scale);
        gl.glBindTextureUnit(0, target.texture);
        gl.glDispatchCompute((target.width + 63u) / 64u, (rows + 3u) / 4u, 1u);
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
        if (replacementsActive())
        {
            uint32_t begin = 0, end = 0;
            rowsToBytes(target, y0, y0 + rows - 1u, begin, end);
            markPagesWritten(begin, end, true);
        }
        target.dirtyY0 = ~0u;
        target.dirtyY1 = 0;
    }

    void Renderer::rowsToBytes(const Target &target, uint32_t y0, uint32_t y1, uint32_t &begin, uint32_t &end) const
    {
        const PageShape shape = pageShape(target.psm);
        const uint32_t rowBytes = (target.bw * 64u + shape.width - 1u) / shape.width * kPageBytes;
        const uint32_t baseBytes = target.block * 256u;
        const uint32_t base = baseBytes & ~(kPageBytes - 1u);
        const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
        begin = static_cast<uint32_t>(std::min<uint64_t>(base + uint64_t(y0 / shape.height) * rowBytes, kLocalMemoryBytes));
        end = static_cast<uint32_t>(
            std::min<uint64_t>(base + uint64_t(y1 / shape.height + 1u) * rowBytes + misalign, kLocalMemoryBytes));
    }

    bool Renderer::bytesToRows(const Target &target, uint32_t begin, uint32_t end, uint32_t &y0, uint32_t &y1) const
    {
        const uint32_t lo = std::max(begin, target.beginByte), hi = std::min(end, target.endByte);
        if (lo >= hi || target.usedRows == 0u)
            return false;
        const PageShape shape = pageShape(target.psm);
        const uint32_t rowBytes = (target.bw * 64u + shape.width - 1u) / shape.width * kPageBytes;
        const uint32_t baseBytes = target.block * 256u;
        const uint32_t base = baseBytes & ~(kPageBytes - 1u);
        const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
        // With a misaligned base a memory page holds parts of two buffer pages.
        const uint32_t first = (std::max(lo, base + misalign) - misalign - base) / rowBytes;
        const uint32_t last = (hi - 1u - base) / rowBytes;
        y0 = first * shape.height;
        y1 = std::min((last + 1u) * shape.height - 1u, target.usedRows - 1u);
        return y0 <= y1;
    }

    void Renderer::resolveRange(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        if (!m_inFlush)
            flushBatch();
        for (auto &target : m_targets)
        {
            if (!target->gpuDirty())
                continue;
            uint32_t begin = 0, end = 0;
            rowsToBytes(*target, target->dirtyY0, target->dirtyY1, begin, end);
            if (begin < endByte && beginByte < end)
                resolveTarget(*target);
        }
    }

    void Renderer::markStaleRows(Target &target, uint32_t beginByte, uint32_t endByte)
    {
        uint32_t y0 = 0, y1 = 0;
        if (!bytesToRows(target, beginByte, endByte, y0, y1))
            return;
        if (target.gpuDirty())
            resolveTarget(target); // keep its drawn rows; VRAM there is current
        target.staleY0 = std::min(target.staleY0, y0);
        target.staleY1 = std::max(target.staleY1, y1);
    }

    void Renderer::loadStaleRows(Target &target)
    {
        if (!target.stale())
            return;
        const uint32_t y0 = target.staleY0, y1 = std::min(target.staleY1, target.usedRows - 1u);
        target.staleY0 = ~0u;
        target.staleY1 = 0;
        if (y0 > y1)
            return;
        uint32_t begin = 0, end = 0;
        rowsToBytes(target, y0, y1, begin, end);
        m_syncReason = "stale";
        resolveRange(begin, end); // other targets' data there
        loadRows(target, y0, y1 - y0 + 1u);
    }

    Renderer::Target *Renderer::textureSourceTarget(uint32_t tbp0, uint32_t tbw, uint32_t tpsm, uint32_t rows)
    {
        const bool tex16 = tpsm == PSM_CT16 || tpsm == PSM_CT16S;
        const bool tex32 = tpsm == PSM_CT32 || tpsm == PSM_CT24;
        if (!tex16 && !tex32)
            return nullptr; // palettized / Z reinterpretations read local memory
        for (auto &candidate : m_targets)
        {
            Target &target = *candidate;
            if (target.depth || target.block != tbp0 || target.bw != std::max<uint32_t>(tbw, 1u) || target.usedRows == 0u)
                continue;
            if (tex16 ? target.psm != tpsm : (target.psm != PSM_CT32 && target.psm != PSM_CT24))
                continue;
            if (rows > target.height)
                return nullptr;
            // Rows it doesn't mirror yet come from VRAM; rows another view of
            // the memory changed since (tracked as stale) are reloaded.
            growTarget(target, rows);
            loadStaleRows(target);
            return &target;
        }
        return nullptr;
    }

    GLuint Renderer::feedbackCopy(const Target &target)
    {
        GL &gl = m_context.gl;
        const uint32_t width = target.width * m_scale, height = target.height * m_scale;
        if (m_feedbackTexture == 0 || m_feedbackWidth < width || m_feedbackHeight < height)
        {
            if (m_feedbackTexture != 0)
                gl.glDeleteTextures(1, &m_feedbackTexture);
            m_feedbackWidth = std::max(m_feedbackWidth, width);
            m_feedbackHeight = std::max(m_feedbackHeight, height);
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_feedbackTexture);
            gl.glTextureStorage2D(m_feedbackTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_feedbackWidth),
                                  static_cast<GLsizei>(m_feedbackHeight));
        }
        gl.glCopyImageSubData(target.texture, GL_TEXTURE_2D, 0, 0, 0, 0, m_feedbackTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                              static_cast<GLsizei>(width), static_cast<GLsizei>(target.usedRows * m_scale), 1);
        return m_feedbackTexture;
    }

    void Renderer::vramWroteRange(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        for (auto &target : m_targets)
            markStaleRows(*target, beginByte, endByte);
    }

    void Renderer::growTarget(Target &target, uint32_t needRows)
    {
        const uint32_t oldRows = target.usedRows;
        const uint32_t newRows = std::min(needRows, target.height);
        if (newRows <= oldRows)
            return;
        uint32_t begin = 0, end = 0;
        rowsToBytes(target, oldRows, newRows - 1u, begin, end);
        m_syncReason = "grow";
        resolveRange(begin, end);
        target.usedRows = newRows;
        bufferRange(target.block, target.bw, newRows, target.psm, target.beginByte, target.endByte);
        loadRows(target, oldRows, newRows - oldRows);
    }

    void Renderer::prepareTargetForDraw(Target &target, int32_t drawY0, int32_t drawY1)
    {
        growTarget(target, static_cast<uint32_t>(std::max(0, drawY1 + 1)));
        if (drawY1 >= drawY0 && drawY1 >= 0 && target.usedRows != 0u)
        {
            uint32_t begin = 0, end = 0;
            rowsToBytes(target, static_cast<uint32_t>(std::max(0, drawY0)),
                        std::min(static_cast<uint32_t>(drawY1), target.usedRows - 1u), begin, end);

            // Other views of the memory drawn here: their drawn data reaches
            // VRAM first (oldest first, so the newest wins where they
            // overlap), this target picks it up, and they go stale.
            std::vector<Target *> pending;
            for (auto &other : m_targets)
            {
                if (other.get() == &target || other->usedRows == 0u || !other->gpuDirty())
                    continue;
                uint32_t y0 = 0, y1 = 0;
                if (bytesToRows(*other, begin, end, y0, y1))
                    pending.push_back(other.get());
            }
            if (!pending.empty())
            {
                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                for (Target *other : pending)
                {
                    uint32_t b = 0, e = 0;
                    rowsToBytes(*other, other->dirtyY0, other->dirtyY1, b, e);
                    ranges.emplace_back(b, e);
                }
                if (target.gpuDirty())
                    pending.push_back(&target);
                std::sort(pending.begin(), pending.end(),
                          [](const Target *a, const Target *b)
                          { return a->drawSeq < b->drawSeq; });
                m_syncReason = "alias";
                for (Target *t : pending)
                    resolveTarget(*t);
                for (const auto &range : ranges)
                    markStaleRows(target, range.first, range.second);
            }
            for (auto &other : m_targets)
                if (other.get() != &target)
                    markStaleRows(*other, begin, end);
        }
        loadStaleRows(target);
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
        // Rows this draw can touch: scissor clipped to the geometry.
        const int32_t drawY0 = std::max(key.scissor[1], m_batchY0);
        const int32_t drawY1 = std::min(key.scissor[3], m_batchY1);
        prepareTargetForDraw(color, drawY0, drawY1);
        if (depth)
            prepareTargetForDraw(*depth, drawY0, drawY1);
        m_drawColor = &color;
        m_drawDepth = depth;

        // Texture: a colour target holding it is sampled directly (keeps an
        // upscaled image's detail through render-to-texture passes);
        // otherwise it is read from VRAM, where targets drawn over it go first.
        GLuint textureSource = 0;
        GLuint replacement = 0;
        if (state.prim.tme)
        {
            if (replacementsActive())
                uploadLoadedReplacements();
            static const bool s_vramTextures = std::getenv("PS2X_GS_GPU_NO_RT_TEXTURE") != nullptr;
            Target *source = s_vramTextures ? nullptr
                                            : textureSourceTarget(key.tbp0, key.tbw, key.tpsm, state.textureHeight);
            if (source)
            {
                textureSource = source == &color ? feedbackCopy(color) : source->texture;
                // PS2X_TEXREPL_DUMP_ALL: a texture sampled as a render target
                // never reaches the matcher, so dump it here too (best effort
                // from the CPU copy when the target's content is GPU-drawn).
                static const bool s_dumpTargets =
                    std::getenv("PS2X_TEXREPL_DUMP_ALL") != nullptr && std::getenv("PS2X_TEXREPL_DUMP") != nullptr;
                if (s_dumpTargets)
                {
                    const GSTex0Reg &tex0 = ctx.tex0;
                    const uint32_t psm = tex0.psm & 0x3Fu;
                    const bool palettized = isPalettized(static_cast<uint8_t>(psm));
                    const bool texa = !palettized && psm != PSM_CT32;
                    gsrepl::Key key;
                    key.tex0Hash = hashTextureMemory(state);
                    key.clutHash = palettized ? hashClut(state) : 0u;
                    key.bits = gsrepl::makeBits(static_cast<uint8_t>(psm), tex0.tw, tex0.th, texa ? state.texa.ta0 : 0u,
                                                texa ? state.texa.aem : 0u, texa ? state.texa.ta1 : 0u);
                    if (m_dumpedTextures.insert(key).second)
                        dumpReplacementTexture(state, key);
                }
            }
            else if (replacementsActive() && (replacement = replacementTexture(state)) != 0)
            {
                // Sampled from the replacement image, not local memory.
            }
            else
            {
                uint32_t begin = 0, end = 0;
                bufferRange(key.tbp0, key.tbw, state.textureHeight, static_cast<uint8_t>(key.tpsm), begin, end);
                m_syncReason = "texture";
                resolveRange(begin, end);
            }
        }

        Program &p = m_draw;
        gl.glUseProgram(p.id);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebufferFor(color, depth));
        const int scale = static_cast<int>(m_scale);
        gl.glViewport(0, 0, static_cast<GLsizei>(color.width * m_scale), static_cast<GLsizei>(color.height * m_scale));
        gl.glEnable(GL_SCISSOR_TEST);
        const int x0 = std::max(0, key.scissor[0]), y0 = std::max(0, key.scissor[1]);
        const int x1 = std::min<int>(key.scissor[2], static_cast<int>(color.width) - 1);
        const int y1 = std::min<int>(key.scissor[3], static_cast<int>(color.height) - 1);
        gl.glScissor(x0 * scale, y0 * scale, std::max(0, x1 - x0 + 1) * scale, std::max(0, y1 - y0 + 1) * scale);
        m_drawY0 = std::max(y0, drawY0);
        m_drawY1 = std::min(y1, drawY1);

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
        auto set1 = [&](const char *name, int value)
        { gl.glProgramUniform1i(p.id, uniform(p, name), value); };
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
        gl.glProgramUniform4f(p.id, uniform(p, "u_blend"), sk, sc, dk, dc);
        set1("u_abe", state.prim.abe ? 1 : 0);
        set1("u_csel", static_cast<int>(c));
        set1("u_pabe", state.pabe ? 1 : 0);
        gl.glProgramUniform2f(p.id, uniform(p, "u_targetSize"), static_cast<float>(color.width),
                              static_cast<float>(color.height));
        gl.glProgramUniform1f(p.id, uniform(p, "u_fix"), static_cast<float>((alpha >> 32) & 0xFFu));

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
        gl.glProgramUniform3f(p.id, uniform(p, "u_fogColor"), float(state.fogR), float(state.fogG), float(state.fogB));
        if (state.prim.tme)
        {
            const uint64_t clamp = ctx.clamp;
            gl.glProgramUniform4ui(p.id, uniform(p, "u_tex"), ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, ctx.tex0.cpsm);
            gl.glProgramUniform4ui(p.id, uniform(p, "u_tex2"), ctx.tex0.csm, ctx.tex0.csa,
                                   static_cast<uint32_t>(clamp & 0xFu), state.linearFilter ? 1u : 0u);
            gl.glProgramUniform4i(p.id, uniform(p, "u_region"), static_cast<int>((clamp >> 4) & 0x3FFu),
                                  static_cast<int>((clamp >> 14) & 0x3FFu), static_cast<int>((clamp >> 24) & 0x3FFu),
                                  static_cast<int>((clamp >> 34) & 0x3FFu));
            gl.glProgramUniform2i(p.id, uniform(p, "u_texSize"), static_cast<int>(state.textureWidth),
                                  static_cast<int>(state.textureHeight));
            gl.glProgramUniform3ui(p.id, uniform(p, "u_texa"), state.texa.ta0, state.texa.ta1, state.texa.aem ? 1u : 0u);
        }
        set1("u_rtScale", textureSource != 0 ? scale : 0);
        if (textureSource != 0)
            gl.glBindTextureUnit(3, textureSource);
        set1("u_replace", replacement != 0 ? 1 : 0);
        if (replacement != 0)
        {
            // The game's filter and wrap modes (region modes never get here).
            const bool linear = state.linearFilter;
            gl.glTextureParameteri(replacement, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
            gl.glTextureParameteri(replacement, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            gl.glTextureParameteri(replacement, GL_TEXTURE_WRAP_S, (ctx.clamp & 3u) == 1u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            gl.glTextureParameteri(replacement, GL_TEXTURE_WRAP_T, ((ctx.clamp >> 2) & 3u) == 1u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            gl.glBindTextureUnit(4, replacement);
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
        if (((m_keyState.context.test >> 17) & 3u) == 0u) // ZTST NEVER: nothing drawn
        {
            m_vertices.clear();
            m_haveKey = false;
            return;
        }
        float minY = m_vertices[0].y, maxY = m_vertices[0].y;
        for (const Vertex &v : m_vertices)
        {
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
        }
        m_batchY0 = static_cast<int32_t>(std::floor(minY));
        m_batchY1 = static_cast<int32_t>(std::ceil(maxY)) - 1; // an edge at y covers rows < y
        GL &gl = m_context.gl;
        m_inFlush = true;
        applyDrawState(m_key, m_keyState);
        m_inFlush = false;

        ScopeTimer timer{m_timeDraw};
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

        // The targets now hold data VRAM lacks in the drawn rows.
        auto markDirty = [&](Target *target)
        {
            if (!target)
                return;
            target->drawSeq = ++m_drawSeq;
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
                key.clutVersion = m_clutVersion;
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

        // GS pixel centres are at integer coordinates, GL's at +0.5: shifting
        // vertices by half a pixel makes GL sample where the GS does (off by
        // half a pixel, every render-to-texture pass drifts the image).
        // Upscaled, the GS sample point of a pixel is its first sub-pixel's
        // centre: half a target pixel.
        const float halfPixel = 0.5f / static_cast<float>(m_scale);
        const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f - halfPixel;
        const float ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f - halfPixel;
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
                a.s = (static_cast<float>(v0.u) / 16.0f) / texW;
                a.t = (static_cast<float>(v0.v) / 16.0f) / texH;
                b.s = (static_cast<float>(v1.u) / 16.0f) / texW;
                b.t = (static_cast<float>(v1.v) / 16.0f) / texH;
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

    void Renderer::readbackVram()
    {
        if (!m_gpu || !m_vram)
            return;
        flushBatch();
        m_syncReason = "readback";
        resolveRange(0u, kLocalMemoryBytes);
        ++m_statReadbacks;
        ScopeTimer timer{m_timeReadback};
        GL &gl = m_context.gl;
        gl.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        gl.glGetNamedBufferSubData(m_vramBuffer, 0, kLocalMemoryBytes, m_vram);
        std::fill(m_pageCpuStale.begin(), m_pageCpuStale.end(), uint8_t{0});
    }

    void Renderer::uploadVram(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu || !m_vram || beginByte >= endByte)
            return;
        flushBatch();
        m_context.gl.glNamedBufferSubData(m_vramBuffer, beginByte, endByte - beginByte, m_vram + beginByte);
        vramWroteRange(beginByte, endByte);
        markPagesWritten(beginByte, endByte, false);
    }

    void Renderer::initialize(uint8_t *vram, uint32_t vramSize)
    {
        m_vram = vram;
        m_cpu->Initialize(vram, vramSize);
        if (m_gpu && m_vram)
            m_context.gl.glNamedBufferSubData(m_vramBuffer, 0, std::min(vramSize, kLocalMemoryBytes), m_vram);
    }

    void Renderer::reset()
    {
        flushBatch();
        m_cpu->Reset();
        m_clutCbp[0] = m_clutCbp[1] = 0u;
        m_transferState = {};
        m_transferState.direction = 3u;
        m_cpuTransfer = false;
        if (m_gpu && m_vram)
        {
            for (auto &target : m_targets)
            {
                target->dirtyY0 = ~0u;
                target->dirtyY1 = 0;
            }
            uploadVram(0u, kLocalMemoryBytes);
        }
    }

    void Renderer::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        if (!m_gpu)
        {
            m_cpu->LoadClut(tex0, texclut);
            return;
        }
        if (replacementsActive())
            m_cpu->LoadClut(tex0, texclut); // the palette hash reads the CPU CLUT
        const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
        const bool eightBit = tex0.psm == PSM_T8 || tex0.psm == PSM_T8H;
        if (!fourBit && !eightBit)
            return;
        switch (tex0.cld)
        {
        case 1u:
            break;
        case 2u:
            m_clutCbp[0] = tex0.cbp;
            break;
        case 3u:
            m_clutCbp[1] = tex0.cbp;
            break;
        case 4u:
            if (m_clutCbp[0] == tex0.cbp)
                return;
            m_clutCbp[0] = tex0.cbp;
            break;
        case 5u:
            if (m_clutCbp[1] == tex0.cbp)
                return;
            m_clutCbp[1] = tex0.cbp;
            break;
        default:
            return;
        }
        const bool sixteenBit = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
        const bool thirtyTwoBit = tex0.cpsm == PSM_CT32 || tex0.cpsm == PSM_CT24;
        if (!sixteenBit && !thirtyTwoBit)
            return;

        flushBatch(); // queued draws use the previous CLUT
        uint32_t begin = 0, end = 0;
        const uint32_t sourceWidth = tex0.csm == 0u ? 1u : std::max<uint32_t>(texclut.cbw, 1u);
        const uint32_t sourceRows = tex0.csm == 0u ? 16u : static_cast<uint32_t>(texclut.cov) + 1u;
        bufferRange(tex0.cbp, sourceWidth, sourceRows, tex0.cpsm, begin, end);
        m_syncReason = "clut";
        resolveRange(begin, end);

        ++m_statClutLoads;
        ++m_clutVersion;
        GL &gl = m_context.gl;
        // Draws already submitted still read the old CLUT/VRAM: without this
        // the compute write can overtake their fragment shaders (write after read).
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        gl.glUseProgram(m_clutLoad.id);
        gl.glProgramUniform4ui(m_clutLoad.id, uniform(m_clutLoad, "u_clut"), tex0.cpsm, tex0.cbp, tex0.csm, tex0.csa);
        gl.glProgramUniform4ui(m_clutLoad.id, uniform(m_clutLoad, "u_clut2"), texclut.cbw, texclut.cou, texclut.cov,
                               fourBit ? 1u : 0u);
        gl.glDispatchCompute(1u, 1u, 1u);
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    void Renderer::dispatchHostToLocal(const uint8_t *data, uint32_t pixels, uint32_t streamBytes)
    {
        GL &gl = m_context.gl;
        const size_t bytes = (streamBytes + 3u) & ~3u;
        if (bytes > m_stagingCapacity)
        {
            if (m_stagingBuffer)
                gl.glDeleteBuffers(1, &m_stagingBuffer);
            m_stagingCapacity = std::max<size_t>(bytes, 1u << 20);
            gl.glCreateBuffers(1, &m_stagingBuffer);
            gl.glNamedBufferStorage(m_stagingBuffer, static_cast<GLsizeiptr>(m_stagingCapacity), nullptr,
                                    GL_DYNAMIC_STORAGE_BIT);
            gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_stagingBuffer);
        }
        if (streamBytes & 3u)
        {
            std::vector<uint8_t> padded(bytes, 0u);
            std::memcpy(padded.data(), data, streamBytes);
            gl.glNamedBufferSubData(m_stagingBuffer, 0, static_cast<GLsizeiptr>(bytes), padded.data());
        }
        else
        {
            gl.glNamedBufferSubData(m_stagingBuffer, 0, static_cast<GLsizeiptr>(bytes), data);
        }
        const auto &buf = m_transfer.bitbltbuf;
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // earlier draws finish reading VRAM first
        gl.glUseProgram(m_hostToLocal.id);
        gl.glProgramUniform4ui(m_hostToLocal.id, uniform(m_hostToLocal, "u_dest"), buf.dbp,
                               std::max<uint32_t>(buf.dbw, 1u), buf.dpsm, m_transfer.trxreg.rrw);
        gl.glProgramUniform4ui(m_hostToLocal.id, uniform(m_hostToLocal, "u_pos"), m_transfer.trxpos.dsax,
                               m_transfer.trxpos.dsay, m_transferState.copiedPixels, pixels);
        gl.glProgramUniform1ui(m_hostToLocal.id, uniform(m_hostToLocal, "u_bits"), bitsPerPixel(buf.dpsm));
        gl.glDispatchCompute((pixels + 63u) / 64u, 1u, 1u);
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    void Renderer::dispatchLocalToLocal(const GSTransferCommand &command)
    {
        const auto &buf = command.bitbltbuf;
        const uint32_t rrw = command.trxreg.rrw, rrh = command.trxreg.rrh;
        if (rrw == 0u || rrh == 0u)
            return;
        uint32_t srcBegin = 0, srcEnd = 0, dstBegin = 0, dstEnd = 0;
        bufferRange(buf.sbp, buf.sbw, command.trxpos.ssay + rrh, buf.spsm, srcBegin, srcEnd);
        bufferRange(buf.dbp, buf.dbw, command.trxpos.dsay + rrh, buf.dpsm, dstBegin, dstEnd);
        const bool overlap = srcBegin < dstEnd && dstBegin < srcEnd;
        GL &gl = m_context.gl;
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // earlier draws finish reading VRAM first
        gl.glUseProgram(m_localToLocal.id);
        gl.glProgramUniform4ui(m_localToLocal.id, uniform(m_localToLocal, "u_source"), buf.sbp,
                               std::max<uint32_t>(buf.sbw, 1u), buf.spsm, command.trxpos.ssax);
        gl.glProgramUniform4ui(m_localToLocal.id, uniform(m_localToLocal, "u_dest"), buf.dbp,
                               std::max<uint32_t>(buf.dbw, 1u), buf.dpsm, command.trxpos.dsax);
        gl.glProgramUniform4ui(m_localToLocal.id, uniform(m_localToLocal, "u_rect"), rrw, rrh, command.trxpos.ssay,
                               command.trxpos.dsay);
        gl.glProgramUniform2ui(m_localToLocal.id, uniform(m_localToLocal, "u_mode"), command.trxpos.dir,
                               overlap ? 1u : 0u);
        gl.glDispatchCompute(overlap ? 1u : (rrw * rrh + 63u) / 64u, 1u, 1u);
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    void Renderer::beginTransfer(const GSTransferCommand &command)
    {
        if (!m_gpu)
        {
            m_cpu->BeginTransfer(command);
            return;
        }
        flushBatch();
        ++m_statTransfers;
        m_transfer = command;
        m_cpuTransfer = false;
        m_transferState = {};
        m_transferState.x = command.trxpos.dsax;
        m_transferState.y = command.trxpos.dsay;
        m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * command.trxreg.rrh;
        m_transferState.direction = command.direction;
        const auto &buf = command.bitbltbuf;

        if (command.direction == 0u || command.direction == 2u)
        {
            // Partial writes must merge with drawn data.
            uint32_t begin = 0, end = 0;
            bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh, buf.dpsm,
                        begin, end);
            m_syncReason = "transferDst";
            resolveRange(begin, end);
        }
        if (command.direction == 1u || command.direction == 2u)
        {
            uint32_t begin = 0, end = 0;
            bufferRange(buf.sbp, buf.sbw, static_cast<uint32_t>(command.trxpos.ssay) + command.trxreg.rrh, buf.spsm,
                        begin, end);
            m_syncReason = "transferSrc";
            resolveRange(begin, end);
        }

        if (replacementsActive() && (command.direction == 0u || command.direction == 2u))
            m_cpu->BeginTransfer(command); // keeps the CPU copy (texture hashing) in step
        if (command.direction == 1u)
        {
            // Local -> host: the CPU backend reads the refreshed local memory.
            readbackVram();
            m_cpu->BeginTransfer(command);
            m_cpuTransfer = true;
        }
        else if (command.direction == 2u)
        {
            dispatchLocalToLocal(command);
            uint32_t begin = 0, end = 0;
            bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh, buf.dpsm,
                        begin, end);
            vramWroteRange(begin, end);
            markPagesWritten(begin, end, true); // the source may hold GPU-drawn data
            m_transferState.copiedPixels = m_transferState.totalPixels;
            m_transferState.direction = 3u;
        }
        else if (command.direction == 0u && (m_transferState.totalPixels == 0u || command.trxreg.rrw == 0u))
        {
            m_transferState.direction = 3u;
        }
    }

    void Renderer::uploadImage(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!m_gpu)
        {
            m_cpu->UploadImage(data, sizeBytes);
            return;
        }
        if (!data || sizeBytes == 0u || m_transferState.direction != 0u)
            return;
        // Whole pixels only (a partial trailing pixel is dropped, as on the CPU path).
        const uint32_t bits = bitsPerPixel(m_transfer.bitbltbuf.dpsm);
        const uint32_t remaining = m_transferState.totalPixels - m_transferState.copiedPixels;
        uint32_t pixels = 0;
        if (bits == 4u)
            pixels = std::min<uint32_t>(remaining, sizeBytes * 2u);
        else
            pixels = std::min<uint32_t>(remaining, sizeBytes / (bits / 8u));
        if (pixels == 0u)
            return;
        const uint32_t streamBytes = bits == 4u ? (pixels + 1u) / 2u : pixels * (bits / 8u);
        flushBatch();
        dispatchHostToLocal(data, pixels, streamBytes);
        if (replacementsActive())
        {
            ScopeTimer timer{g_texreplMirrorSecs};
            m_cpu->UploadImage(data, sizeBytes);
            g_texreplMirrorBytes[m_transfer.bitbltbuf.dpsm & 0x3Fu] += streamBytes;
        }

        const auto &buf = m_transfer.bitbltbuf;
        uint32_t begin = 0, end = 0;
        bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(m_transfer.trxpos.dsay) + m_transfer.trxreg.rrh, buf.dpsm,
                    begin, end);
        vramWroteRange(begin, end);
        markPagesWritten(begin, end, false);

        m_transferState.copiedPixels += pixels;
        if (m_transferState.copiedPixels >= m_transferState.totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
        }
        else
        {
            const uint32_t rrw = m_transfer.trxreg.rrw;
            m_transferState.x = m_transfer.trxpos.dsax + (m_transferState.copiedPixels % rrw);
            m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
        }
    }

    void Renderer::textureFlush()
    {
        if (!m_gpu)
            m_cpu->TextureFlush();
    }

    void Renderer::syncLocalMemory()
    {
        readbackVram();
    }

    // ------------------------------------------------------ texture replacement

    void Renderer::markPagesWritten(uint32_t beginByte, uint32_t endByte, bool cpuStale)
    {
        if (!replacementsActive() || beginByte >= endByte)
            return;
        const uint32_t first = beginByte / kPageBytes, last = std::min<uint32_t>((endByte - 1u) / kPageBytes, 511u);
        // An upload makes the CPU copy current there again (a page only
        // partly rewritten hashes mixed data: a miss, never a wrong match).
        for (uint32_t page = first; page <= last; ++page)
        {
            ++m_pageGeneration[page];
            m_pageCpuStale[page] = cpuStale ? 1u : 0u;
        }
    }

    void Renderer::updateReplacements()
    {
        const uint64_t version = gsrepl::currentVersion();
        if (version == m_replVersion)
            return;
        m_replVersion = version;
        const bool wasActive = replacementsActive();
        m_replIndex = gsrepl::current();
        GL &gl = m_context.gl;
        {
            std::lock_guard lock(m_replMutex);
            m_replRequests.clear();
            m_replResults.clear();
            m_replResultCount.store(0u, std::memory_order_relaxed);
        }
        m_replPendingUploads.clear();
        // Keep the uploads whose file is unchanged: rewriting the prompt art
        // must not reload the (possibly huge) texture pack with it.
        for (auto it = m_replacements.begin(); it != m_replacements.end();)
        {
            bool keep = false;
            if (m_replIndex)
            {
                const auto file = m_replIndex->files.find(it->first);
                keep = file != m_replIndex->files.end() && it->second.texture != 0 &&
                       file->second.stamp == it->second.stamp;
            }
            if (keep)
            {
                ++it;
                continue;
            }
            if (it->second.texture != 0)
                gl.glDeleteTextures(1, &it->second.texture);
            it = m_replacements.erase(it);
        }
        m_textureHashes.clear();
        m_clutHashKey = ~0ull;
        if (!replacementsActive())
        {
            stopReplacementWorker();
            return;
        }
        std::cerr << "[gs:gpu] texture replacement: " << m_replIndex->files.size() << " textures" << std::endl;
        if (!wasActive)
        {
            // The CPU copy of local memory becomes the hashing source: bring
            // it up to date; uploads keep it so from here on.
            readbackVram();
            std::fill(m_pageGeneration.begin(), m_pageGeneration.end(), 0u);
        }
        if (!m_replThread.joinable())
        {
            m_replStop = false;
            m_replThread = std::thread([this]() { replacementWorker(); });
        }
    }

    void Renderer::stopReplacementWorker()
    {
        if (!m_replThread.joinable())
            return;
        {
            std::lock_guard lock(m_replMutex);
            m_replStop = true;
        }
        m_replCv.notify_all();
        m_replThread.join();
    }

    void Renderer::replacementWorker()
    {
        for (;;)
        {
            std::pair<gsrepl::Key, std::string> request;
            {
                std::unique_lock lock(m_replMutex);
                m_replCv.wait(lock, [this]() { return m_replStop || !m_replRequests.empty(); });
                if (m_replStop)
                    return;
                request = std::move(m_replRequests.front());
                m_replRequests.pop_front();
            }
            auto image = std::make_unique<gsrepl::Image>();
            std::string error;
            if (!gsrepl::loadImage(request.second, *image, error))
            {
                std::cerr << "[gs:gpu] replacement " << request.second << ": " << error << std::endl;
                image.reset();
            }
            std::lock_guard lock(m_replMutex);
            m_replResults.emplace_back(request.first, std::move(image));
            m_replResultCount.fetch_add(1u, std::memory_order_release);
        }
    }

    void Renderer::uploadLoadedReplacements()
    {
        // At most this much texture data per frame: entering a new area can
        // finish dozens of replacements at once, and uploading them all in one
        // frame is a visible hitch (the originals show a few frames longer).
        constexpr size_t kUploadBytesPerFrame = 6u * 1024u * 1024u;
        if (m_replResultCount.load(std::memory_order_acquire) != 0u)
        {
            std::lock_guard lock(m_replMutex);
            for (auto &result : m_replResults)
                m_replPendingUploads.push_back(std::move(result));
            m_replResults.clear();
            m_replResultCount.store(0u, std::memory_order_relaxed);
        }
        if (m_replPendingUploads.empty() || m_replUploadedThisFrame >= kUploadBytesPerFrame)
            return;
        GL &gl = m_context.gl;
        while (!m_replPendingUploads.empty() && m_replUploadedThisFrame < kUploadBytesPerFrame)
        {
            auto result = std::move(m_replPendingUploads.front());
            m_replPendingUploads.pop_front();
            auto it = m_replacements.find(result.first);
            if (it == m_replacements.end() || !result.second || result.second->levels.empty())
                continue; // index changed meanwhile, or the file failed (stays "requested")
            const gsrepl::Image &image = *result.second;
            for (const auto &l : image.levels)
                m_replUploadedThisFrame += l.data.size();
            GLuint texture = 0;
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            gl.glTextureStorage2D(texture, static_cast<GLsizei>(image.levels.size()), image.glFormat,
                                  static_cast<GLsizei>(image.levels[0].width), static_cast<GLsizei>(image.levels[0].height));
            for (size_t level = 0; level < image.levels.size(); ++level)
            {
                const auto &l = image.levels[level];
                if (image.compressed)
                    gl.glCompressedTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(l.width),
                                                     static_cast<GLsizei>(l.height), image.glFormat,
                                                     static_cast<GLsizei>(l.data.size()), l.data.data());
                else
                    gl.glTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(l.width),
                                           static_cast<GLsizei>(l.height), GL_RGBA, GL_UNSIGNED_BYTE, l.data.data());
            }
            gl.glTextureParameteri(texture, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(image.levels.size()) - 1);
            gl.glTextureParameterf(texture, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, 16.0f);
            it->second.texture = texture;
            ++m_statReplLoaded;
        }
    }

    uint64_t Renderer::hashTextureMemory(const GSDrawState &state)
    {
        // PCSX2 HashTextureLevel(): whole GS blocks of the texture in block
        // order when it covers whole blocks of a format using all 32 bits of
        // its words; otherwise the expanded texels (palette indices / RGBA).
        const GSTex0Reg &tex0 = state.context.tex0;
        const uint32_t psm = tex0.psm, tbp = tex0.tbp0, tbw = tex0.tbw;
        const uint32_t width = 1u << std::min<uint32_t>(tex0.tw, 10u), height = 1u << std::min<uint32_t>(tex0.th, 10u);
        uint32_t bsx = 8, bsy = 8;
        if (psm == PSM_CT16 || psm == PSM_CT16S || psm == 0x32u || psm == 0x3Au)
            bsx = 16, bsy = 8;
        else if (psm == PSM_T8)
            bsx = 16, bsy = 16;
        else if (psm == PSM_T4)
            bsx = 32, bsy = 16;
        const bool fullWords = psm == PSM_CT32 || psm == 0x30u || psm == PSM_T8 || psm == PSM_T4;
        ++m_statReplHashes;
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        if (width >= bsx && height >= bsy && fullWords)
        {
            for (uint32_t y = 0; y < height; y += bsy)
                for (uint32_t x = 0; x < width; x += bsx)
                    XXH3_64bits_update(&st, m_vram + (GSMem::PixelByteAddress(psm, tbp, tbw, x, y) & ~255u), 256u);
            g_texreplHashBytes[psm & 0x3Fu] += static_cast<uint64_t>(width / bsx) * (height / bsy) * 256u;
            return XXH3_64bits_digest(&st);
        }
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        std::vector<uint8_t> row(static_cast<size_t>(width) * (palettized ? 1u : 4u));
        const uint32_t ta0 = state.texa.ta0, ta1 = state.texa.ta1;
        const bool aem = state.texa.aem;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t raw = m_cpu->ReadVram(psm, tbp, tbw, x, y);
                if (palettized)
                {
                    row[x] = static_cast<uint8_t>(raw);
                    continue;
                }
                uint32_t c = raw;
                if (psm == PSM_CT24 || psm == 0x31u)
                    c = (raw & 0x00FFFFFFu) | ((aem && (raw & 0x00FFFFFFu) == 0u) ? 0u : (ta0 << 24));
                else if (is16Bit(static_cast<uint8_t>(psm)))
                {
                    const uint32_t a = (raw & 0x8000u) ? ta1 : ((aem && (raw & 0x7FFFu) == 0u) ? 0u : ta0);
                    c = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
                std::memcpy(row.data() + x * 4u, &c, 4u);
            }
            XXH3_64bits_update(&st, row.data(), row.size());
        }
        g_texreplHashBytes[psm & 0x3Fu] += static_cast<uint64_t>(width) * height * (palettized ? 1u : 4u);
        return XXH3_64bits_digest(&st);
    }

    uint64_t Renderer::hashClut(const GSDrawState &state)
    {
        // PCSX2 GSClut::Read32 buffer: 16 (4-bit) or 256 (8-bit) 32-bit
        // entries in index order; 16-bit palettes expanded with TEXA.
        const GSTex0Reg &tex0 = state.context.tex0;
        const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
        const bool sixteen = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
        const uint64_t key = (m_clutVersion << 24) ^ (uint64_t(tex0.csa) << 1) ^ (fourBit ? 1u : 0u) ^
                             (uint64_t(tex0.cpsm) << 8) ^
                             (sixteen ? (uint64_t(state.texa.ta0) << 40 ^ uint64_t(state.texa.ta1) << 48 ^ uint64_t(state.texa.aem) << 56) : 0u);
        if (key == m_clutHashKey)
            return m_clutHash;
        const auto &clut = m_cpu->clutTable();
        uint32_t entries[256];
        const uint32_t count = fourBit ? 16u : 256u;
        if (!sixteen)
        {
            const uint32_t csa = (tex0.csa & 15u) << 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = fourBit ? csa + i : std::min((i & ~15u) + csa, 240u) + (i & 15u);
                entries[i] = uint32_t(clut[e & 0xFFu]) | (uint32_t(clut[(e & 0xFFu) + 256u]) << 16);
            }
        }
        else
        {
            const uint32_t base = static_cast<uint32_t>(tex0.csa) << 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t raw = clut[(base + i) & 0x1FFu];
                const uint32_t a = (raw & 0x8000u) ? state.texa.ta1 : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                entries[i] = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
            }
        }
        m_clutHashKey = key;
        m_clutHash = XXH3_64bits(entries, count * 4u);
        return m_clutHash;
    }

    void Renderer::dumpReplacementTexture(const GSDrawState &state, const gsrepl::Key &key)
    {
        const char *dir = std::getenv("PS2X_TEXREPL_DUMP");
        if (!dir || !*dir || !m_cpu || !m_vram)
            return;
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex0 = ctx.tex0;
        const uint32_t psm = tex0.psm & 0x3Fu;
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        // PS2X_TEXREPL_DUMP_ALL=1: decode best effort even for formats the
        // matcher does not support (raw 32-bit words), so pack authoring can
        // see every drawn texture.
        static const bool s_anyFormat = std::getenv("PS2X_TEXREPL_DUMP_ALL") != nullptr;
        if (!palettized && psm != PSM_CT32 && psm != PSM_CT24 && psm != PSM_CT16 && psm != PSM_CT16S && !s_anyFormat)
            return;
        const uint32_t width = 1u << std::min<uint32_t>(tex0.tw, 10u);
        const uint32_t height = 1u << std::min<uint32_t>(tex0.th, 10u);
        if (width == 0u || height == 0u)
            return;
        // Skip very large textures while hunting glyphs: copying 1-4 MB
        // buffers per dump stutters the render thread. Raise via
        // PS2X_TEXREPL_DUMP_MAX (edge length, default 512) when needed.
        static const uint32_t s_maxEdge = []()
        {
            const char *env = std::getenv("PS2X_TEXREPL_DUMP_MAX");
            return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : 512u;
        }();
        if (width > s_maxEdge || height > s_maxEdge)
            return;
        // Palette for palettized textures, expanded like hashClut does.
        uint32_t palette[256] = {};
        if (palettized)
        {
            const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
            const bool sixteen = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
            const auto &clut = m_cpu->clutTable();
            const uint32_t count = fourBit ? 16u : 256u;
            if (!sixteen)
            {
                const uint32_t csa = (tex0.csa & 15u) << 4;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t e = fourBit ? csa + i : std::min((i & ~15u) + csa, 240u) + (i & 15u);
                    const uint32_t lo = clut[e & 0xFFu], hi = clut[(e & 0xFFu) + 256u];
                    palette[i] = (lo & 0xFFu) | ((lo >> 8) << 8) | ((hi & 0xFFu) << 16) | ((hi >> 8) << 24);
                }
            }
            else
            {
                const uint32_t base = static_cast<uint32_t>(tex0.csa) << 4;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t raw = clut[(base + i) & 0x1FFu];
                    const uint32_t a = (raw & 0x8000u) ? state.texa.ta1
                                                       : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                    palette[i] = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
            }
        }
        std::vector<uint8_t> rgba(size_t(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t raw = m_cpu->ReadVram(psm, tex0.tbp0, tex0.tbw, x, y);
                uint32_t c = raw;
                if (palettized)
                    c = palette[raw & 0xFFu];
                else if (psm == PSM_CT24)
                    c = (raw & 0x00FFFFFFu) | ((state.texa.aem && (raw & 0x00FFFFFFu) == 0u) ? 0u : (state.texa.ta0 << 24));
                else if (is16Bit(static_cast<uint8_t>(psm)))
                {
                    const uint32_t a = (raw & 0x8000u) ? state.texa.ta1
                                                       : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                    c = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
                std::memcpy(rgba.data() + (size_t(y) * width + x) * 4u, &c, 4u);
            }
        DumpJob job;
        job.rgba = std::move(rgba);
        job.width = static_cast<int>(width);
        job.height = static_cast<int>(height);
        job.tbp0 = tex0.tbp0;
        job.tbw = tex0.tbw;
        job.psm = tex0.psm;
        job.tw = tex0.tw;
        job.th = tex0.th;
        job.frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        std::snprintf(job.name, sizeof(job.name), "%016llx-%016llx-%08x.png", static_cast<unsigned long long>(key.tex0Hash),
                      static_cast<unsigned long long>(key.clutHash), key.bits);
        // Encoded off-thread: PNG compression on the render thread stutters.
        enqueueDump(std::move(job));
    }

    GLuint Renderer::replacementTexture(const GSDrawState &state)
    {
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex0 = ctx.tex0;
        const uint32_t psm = tex0.psm;
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        // PS2X_TEXREPL_DUMP_ALL=1: dump textures the matcher refuses
        // (unsupported formats, region clamp) so pack authoring sees every
        // drawn texture; GPU-drawn (CPU-stale) pages are still skipped - the
        // CPU copy holds stale bytes there.
        static const bool s_dumpAll = std::getenv("PS2X_TEXREPL_DUMP_ALL") != nullptr && std::getenv("PS2X_TEXREPL_DUMP") != nullptr;
        if (s_dumpAll)
        {
            const bool supported = palettized || psm == PSM_CT32 || psm == PSM_CT24 || psm == PSM_CT16 || psm == PSM_CT16S;
            const bool unclamped = (ctx.clamp & 3u) < 2u && ((ctx.clamp >> 2) & 3u) < 2u;
            if (!supported || !unclamped)
            {
                uint32_t begin = 0, end = 0;
                bufferRange(tex0.tbp0, tex0.tbw, 1u << std::min<uint32_t>(tex0.th, 10u), static_cast<uint8_t>(psm), begin, end);
                if (begin < end)
                {
                    const bool texa = !palettized && psm != PSM_CT32;
                    gsrepl::Key key;
                    key.tex0Hash = hashTextureMemory(state);
                    key.clutHash = palettized ? hashClut(state) : 0u;
                    key.bits = gsrepl::makeBits(static_cast<uint8_t>(psm), tex0.tw, tex0.th, texa ? state.texa.ta0 : 0u,
                                                texa ? state.texa.aem : 0u, texa ? state.texa.ta1 : 0u);
                    if (m_dumpedTextures.insert(key).second)
                        dumpReplacementTexture(state, key);
                }
            }
        }
        if (!palettized && psm != PSM_CT32 && psm != PSM_CT24 && psm != PSM_CT16 && psm != PSM_CT16S)
            return 0;
        if ((ctx.clamp & 3u) >= 2u || ((ctx.clamp >> 2) & 3u) >= 2u)
            return 0; // region clamp / repeat: not supported by replacements
        ++m_statReplDraws;
        uint32_t begin = 0, end = 0;
        bufferRange(tex0.tbp0, tex0.tbw, 1u << std::min<uint32_t>(tex0.th, 10u), static_cast<uint8_t>(psm), begin, end);
        if (begin >= end)
            return 0;
        uint64_t generation = 1;
        for (uint32_t page = begin / kPageBytes; page <= std::min<uint32_t>((end - 1u) / kPageBytes, 511u); ++page)
        {
            if (m_pageCpuStale[page])
                return 0; // drawn by the GPU (render-to-texture): never a replacement
            generation += m_pageGeneration[page];
        }
        const bool texa = !palettized && psm != PSM_CT32;
        const uint64_t texKey = uint64_t(tex0.tbp0) | uint64_t(tex0.tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tex0.tw) << 26 |
                                uint64_t(tex0.th) << 30 |
                                (texa ? (uint64_t(state.texa.ta0) << 34 | uint64_t(state.texa.aem) << 42 | uint64_t(state.texa.ta1) << 43) : 0u);
        HashedTexture &hashed = m_textureHashes[texKey];
        if (hashed.generation != generation)
        {
            ScopeTimer timer{g_texreplHashSecs};
            hashed.hash = hashTextureMemory(state);
            hashed.generation = generation;
        }
        gsrepl::Key key;
        key.tex0Hash = hashed.hash;
        if (palettized)
        {
            ScopeTimer timer{g_texreplHashSecs};
            key.clutHash = hashClut(state);
        }
        else
            key.clutHash = 0u;
        key.bits = gsrepl::makeBits(psm, tex0.tw, tex0.th, texa ? state.texa.ta0 : 0u, texa ? state.texa.aem : 0u,
                                    texa ? state.texa.ta1 : 0u);
        if (const char *dumpDir = std::getenv("PS2X_TEXREPL_DUMP"); dumpDir && *dumpDir &&
                                                                        m_dumpedTextures.insert(key).second)
            dumpReplacementTexture(state, key);
        const auto file = m_replIndex->files.find(key);
        if (file == m_replIndex->files.end())
        {
            ++m_statReplMisses;
            static const bool s_logMisses = std::getenv("PS2X_TEXREPL_LOG_MISSES") != nullptr;
            if (s_logMisses)
                std::fprintf(stderr, "[texrepl] miss %016llx-%016llx-%08x tbp=%u tbw=%u\n", (unsigned long long)key.tex0Hash,
                             (unsigned long long)key.clutHash, key.bits, tex0.tbp0, tex0.tbw);
            return 0;
        }
        ++m_statReplHits;
        Replacement &replacement = m_replacements[key];
        if (replacement.texture != 0)
            return replacement.texture;
        if (!replacement.requested)
        {
            replacement.requested = true;
            replacement.stamp = file->second.stamp;
            // The prompt art loads inline, so a pad context change swaps the
            // prompts within the same frame instead of showing the previous
            // keys until the worker catches up. Pack textures always load on
            // the worker: loading them here would stall the render thread
            // while new textures stream in.
            if (file->second.inlineLoad)
            {
                gsrepl::Image image;
                std::string error;
                if (gsrepl::loadImage(file->second.path, image, error) && !image.levels.empty())
                {
                    GL &gl = m_context.gl;
                    GLuint texture = 0;
                    gl.glCreateTextures(GL_TEXTURE_2D, 1, &texture);
                    gl.glTextureStorage2D(texture, static_cast<GLsizei>(image.levels.size()), image.glFormat,
                                          static_cast<GLsizei>(image.levels[0].width),
                                          static_cast<GLsizei>(image.levels[0].height));
                    for (size_t level = 0; level < image.levels.size(); ++level)
                    {
                        const auto &l = image.levels[level];
                        if (image.compressed)
                            gl.glCompressedTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0,
                                                             static_cast<GLsizei>(l.width),
                                                             static_cast<GLsizei>(l.height), image.glFormat,
                                                             static_cast<GLsizei>(l.data.size()), l.data.data());
                        else
                            gl.glTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0,
                                                   static_cast<GLsizei>(l.width), static_cast<GLsizei>(l.height), GL_RGBA,
                                                   GL_UNSIGNED_BYTE, l.data.data());
                    }
                    gl.glTextureParameteri(texture, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(image.levels.size()) - 1);
                    gl.glTextureParameterf(texture, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, 16.0f);
                    replacement.texture = texture;
                    ++m_statReplLoaded;
                    return replacement.texture;
                }
                std::cerr << "[gs:gpu] replacement " << file->second.path << ": " << error << std::endl;
                return 0; // stays "requested": the game's own art shows
            }
            {
                std::lock_guard lock(m_replMutex);
                m_replRequests.emplace_back(key, file->second.path);
            }
            m_replCv.notify_one();
        }
        return 0; // loading: the original texture this time
    }

    void Renderer::emitTexreplStats()
    {
        std::cerr << "[texrepl] draws=" << m_statReplDraws << " hits=" << m_statReplHits << " misses=" << m_statReplMisses
                  << " loaded=" << m_statReplLoaded << " hashed=" << m_statReplHashes
                  << " textures=" << m_textureHashes.size() << " pendingUploads=" << m_replPendingUploads.size()
                  << " mirrorMs=" << static_cast<int>(g_texreplMirrorSecs * 1000.0)
                  << " hashMs=" << static_cast<int>(g_texreplHashSecs * 1000.0);
        for (int pass = 0; pass < 2; ++pass)
        {
            uint64_t *bytes = pass == 0 ? g_texreplMirrorBytes : g_texreplHashBytes;
            std::cerr << (pass == 0 ? " mirrorTop=" : " hashTop=");
            for (int n = 0; n < 3; ++n)
            {
                int best = -1;
                for (int psm = 0; psm < 64; ++psm)
                    if (bytes[psm] != 0u && (best < 0 || bytes[psm] > bytes[best]))
                        best = psm;
                if (best < 0)
                    break;
                std::cerr << (n ? "," : "") << "psm" << std::hex << best << std::dec << ":" << (bytes[best] >> 20)
                          << "MB";
                bytes[best] = 0u;
            }
        }
        std::memset(g_texreplMirrorBytes, 0, sizeof(g_texreplMirrorBytes));
        std::memset(g_texreplHashBytes, 0, sizeof(g_texreplHashBytes));
        g_texreplMirrorSecs = 0.0;
        g_texreplHashSecs = 0.0;
        std::cerr << std::endl;
    }

    PresentationFrame Renderer::present(const GSPresentationRequest &request)
    {
        // Presentation reads local memory (both circuits, preferred source).
        m_syncReason = "present";
        m_replUploadedThisFrame = 0;
        updateReplacements();
        if (replacementsActive())
        {
            static const bool s_stats = std::getenv("PS2X_TEXREPL_STATS") != nullptr;
            static const uint32_t s_interval = []()
            {
                const char *env = std::getenv("PS2X_TEXREPL_STATS");
                if (!env || !*env || std::strcmp(env, "1") == 0)
                    return 300u;
                return static_cast<uint32_t>(std::strtoul(env, nullptr, 10));
            }();
            static uint32_t s_presents = 0;
            if (s_stats && ++s_presents % s_interval == 0u)
                emitTexreplStats();
        }
        PresentationFrame frame;
        // PS2X_GS_GPU_COMPOSE=1 (default on): compose the display on the GPU
        // at scale 1 as well. The CPU fallback costs a full local-memory
        // readback plus a CPU merge every frame; the GPU path composes into a
        // texture and transfers only the display. Set to 0 for the CPU
        // reference path.
        static const bool s_gpuComposeLowScale = []()
        {
            const char *env = std::getenv("PS2X_GS_GPU_COMPOSE");
            return env == nullptr || std::atoi(env) != 0;
        }();
        if (m_gpu && (m_scale > 1u || s_gpuComposeLowScale))
        {
            // Upscaled: compose from the render targets; local memory (and
            // the CPU composition) only for displays the GPU didn't draw.
            flushBatch();
            ScopeTimer timer{m_timePresent};
            upscalePresentation(request, frame);
        }
        if (!frame)
        {
            readbackVram();
            m_cpu->Sync(GSSyncReason::Presentation);
            m_cpu->TextureFlush();
            ScopeTimer timer{m_timePresent};
            frame = m_cpu->Present(request);
        }
        if (const uint32_t requested = g_gsGpuScaleRequest.load(std::memory_order_relaxed); requested != 0u)
            setScale(requested);
        applyPendingScale(); // between frames
        return frame;
    }

    void Renderer::upscalePresentation(const GSPresentationRequest &request, PresentationFrame &frame)
    {
        // When every enabled circuit reads a buffer the GPU drew, compose
        // the display from the targets at full resolution (the CPU path's
        // merge); anything else is left to the CPU composition.
        static const bool s_fieldBob = std::getenv("PS2X_FIELD_BOB") != nullptr;
        if (s_fieldBob)
            return;
        struct Circuit
        {
            Target *target = nullptr;
            uint32_t x = 0, y = 0, width = 0, height = 0;
        };
        auto circuit = [&](uint64_t dispfb, uint64_t display, Circuit &out) -> bool
        {
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
            const uint8_t psm = static_cast<uint8_t>((dispfb >> 15) & 0x1Fu);
            out.x = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
            out.y = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
            const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0x0FFFu);
            const uint32_t dh = static_cast<uint32_t>((display >> 44) & 0x07FFu);
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0x0Fu);
            out.width = (dw + 1u) / (magh + 1u);
            out.height = dh + 1u;
            if (out.width < 64u || out.height < 64u || out.width > 640u || out.height > 512u)
                return false; // the CPU path substitutes defaults here
            for (auto &candidate : m_targets)
            {
                Target &target = *candidate;
                if (target.depth || target.block != fbp * 32u || target.bw != std::max<uint32_t>(fbw, 1u) ||
                    is16Bit(target.psm) != is16Bit(psm))
                    continue;
                if (out.x + out.width > target.width || out.y + out.height > target.usedRows)
                    return false;
                out.target = &target;
                return true;
            }
            return false;
        };
        const bool crt1 = (request.pmode & 1u) != 0u, crt2 = (request.pmode & 2u) != 0u;
        Circuit c1, c2;
        if ((crt1 && !circuit(request.dispfb1, request.display1, c1)) ||
            (crt2 && !circuit(request.dispfb2, request.display2, c2)) || (!crt1 && !crt2))
            return;
        // A game-specific display source substitution (see GS frontend) is
        // the CPU path's.
        if (request.hasPreferredSource && !(crt1 && crt2) &&
            request.preferredDestFbp == (crt1 ? c1 : c2).target->block / 32u)
            return;

        GL &gl = m_context.gl;
        const uint32_t s = m_scale;
        const uint32_t width = std::max(crt1 ? c1.width : 0u, crt2 ? c2.width : 0u) * s;
        const uint32_t height = std::max(crt1 ? c1.height : 0u, crt2 ? c2.height : 0u) * s;
        if (m_presentTexture == 0 || m_presentWidth < width || m_presentHeight < height)
        {
            if (m_presentTexture != 0)
            {
                gl.glDeleteFramebuffers(1, &m_presentFramebuffer);
                gl.glDeleteTextures(1, &m_presentTexture);
            }
            m_presentWidth = std::max(m_presentWidth, width);
            m_presentHeight = std::max(m_presentHeight, height);
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_presentTexture);
            gl.glTextureStorage2D(m_presentTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_presentWidth),
                                  static_cast<GLsizei>(m_presentHeight));
            gl.glCreateFramebuffers(1, &m_presentFramebuffer);
            gl.glNamedFramebufferTexture(m_presentFramebuffer, GL_COLOR_ATTACHMENT0, m_presentTexture, 0);
            gl.glNamedFramebufferDrawBuffer(m_presentFramebuffer, GL_COLOR_ATTACHMENT0);
        }
        if (crt1)
            loadStaleRows(*c1.target);
        if (crt2)
            loadStaleRows(*c2.target);

        const bool both = crt1 && crt2;
        const Circuit &top = crt1 ? c1 : c2; // a lone circuit is drawn as "circuit 1" at full weight
        Program &p = m_compose;
        gl.glUseProgram(p.id);
        gl.glProgramUniform4i(p.id, uniform(p, "u_rect1"), static_cast<GLint>(top.x * s), static_cast<GLint>(top.y * s),
                              static_cast<GLint>(top.width * s), static_cast<GLint>(top.height * s));
        if (both)
            gl.glProgramUniform4i(p.id, uniform(p, "u_rect2"), static_cast<GLint>(c2.x * s), static_cast<GLint>(c2.y * s),
                                  static_cast<GLint>(c2.width * s), static_cast<GLint>(c2.height * s));
        else
            gl.glProgramUniform4i(p.id, uniform(p, "u_rect2"), 0, 0, 0, 0);
        gl.glProgramUniform3f(p.id, uniform(p, "u_bg"), float(request.bgcolor & 0xFFu) / 255.0f,
                              float((request.bgcolor >> 8) & 0xFFu) / 255.0f, float((request.bgcolor >> 16) & 0xFFu) / 255.0f);
        const bool mmod = ((request.pmode >> 5) & 1u) != 0u, slbg = ((request.pmode >> 7) & 1u) != 0u;
        const float alp = static_cast<float>((request.pmode >> 8) & 0xFFu) / 255.0f;
        gl.glProgramUniform2f(p.id, uniform(p, "u_mode"), both ? (mmod ? alp : -1.0f) : 1.0f, both && slbg ? 1.0f : 0.0f);
        gl.glBindTextureUnit(0, top.target->texture);
        gl.glBindTextureUnit(1, both ? c2.target->texture : top.target->texture);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_presentFramebuffer);
        gl.glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glDisable(GL_BLEND);
        gl.glDisable(GL_DEPTH_TEST);
        gl.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.glBindVertexArray(m_emptyVao);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);

        // The readback is the expensive part of the present and the frame is
        // only ever shown at the host window's size: when the window is
        // smaller than the upscaled frame, downscale on the GPU first (the
        // same bilinear filter the host would apply) and transfer only that.
        // At internal_scale 4 into a 1280x960 window this is ~4x less data.
        uint32_t readWidth = width, readHeight = height;
        GLuint readTexture = m_presentTexture;
        const uint32_t hostWidth = g_gsGpuPresentWidth.load(std::memory_order_relaxed);
        const uint32_t hostHeight = g_gsGpuPresentHeight.load(std::memory_order_relaxed);
        if (hostWidth != 0u && hostHeight != 0u && hostWidth < readWidth && hostHeight < readHeight)
        {
            const double fit = std::min(static_cast<double>(hostWidth) / static_cast<double>(width),
                                        static_cast<double>(hostHeight) / static_cast<double>(height));
            readWidth = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(width) * fit) & ~1u);
            readHeight = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(height) * fit) & ~1u);
            if (m_readbackTexture == 0 || m_readbackWidth < readWidth || m_readbackHeight < readHeight)
            {
                if (m_readbackTexture != 0)
                {
                    gl.glDeleteFramebuffers(1, &m_readbackFramebuffer);
                    gl.glDeleteTextures(1, &m_readbackTexture);
                }
                m_readbackWidth = std::max(m_readbackWidth, readWidth);
                m_readbackHeight = std::max(m_readbackHeight, readHeight);
                gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_readbackTexture);
                gl.glTextureStorage2D(m_readbackTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_readbackWidth),
                                      static_cast<GLsizei>(m_readbackHeight));
                gl.glCreateFramebuffers(1, &m_readbackFramebuffer);
                gl.glNamedFramebufferTexture(m_readbackFramebuffer, GL_COLOR_ATTACHMENT0, m_readbackTexture, 0);
                gl.glNamedFramebufferDrawBuffer(m_readbackFramebuffer, GL_COLOR_ATTACHMENT0);
            }
            gl.glBlitNamedFramebuffer(m_presentFramebuffer, m_readbackFramebuffer, 0, 0, static_cast<GLint>(width),
                                      static_cast<GLint>(height), 0, 0, static_cast<GLint>(readWidth),
                                      static_cast<GLint>(readHeight), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            readTexture = m_readbackTexture;
        }
        std::vector<uint8_t> out(static_cast<size_t>(readWidth) * readHeight * 4u);
        gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
        gl.glGetTextureSubImage(readTexture, 0, 0, 0, 0, static_cast<GLsizei>(readWidth), static_cast<GLsizei>(readHeight),
                                1, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(out.size()), out.data());
        frame.pixels = std::move(out);
        frame.width = readWidth;
        frame.height = readHeight;
        frame.rowPixels = readWidth;
        frame.displayFbp = frame.sourceFbp = (crt1 ? c1 : c2).target->block / 32u;
        // PS2X_GS_PRESENT_DUMP=<prefix>: write presented frames 20..80 (every
        // other one) as binary PPMs <prefix>_NNN.ppm, for A/B checks.
        if (const char *dumpPath = std::getenv("PS2X_GS_PRESENT_DUMP"))
        {
            static int s_presents = 0;
            ++s_presents;
            if (s_presents >= 20 && s_presents <= 80 && (s_presents % 2) == 0)
            {
                char path[1024];
                std::snprintf(path, sizeof(path), "%s_%03d.ppm", dumpPath, s_presents);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", readWidth, readHeight);
                    for (size_t i = 0; i < frame.pixels.size(); i += 4u)
                    {
                        const uint8_t rgb[3] = {frame.pixels[i], frame.pixels[i + 1u], frame.pixels[i + 2u]};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
        }
    }

    void Renderer::setScale(uint32_t scale)
    {
        m_requestedScale.store(std::clamp<uint32_t>(scale, 1u, 8u), std::memory_order_relaxed);
    }

    void Renderer::applyPendingScale()
    {
        const uint32_t scale = m_requestedScale.load(std::memory_order_relaxed);
        if (!m_gpu || scale == m_scale)
            return;
        // Targets carry everything VRAM lacks: put it there, then rebuild
        // them at the new size from VRAM as they are drawn again.
        flushBatch();
        m_syncReason = "rescale";
        resolveRange(0u, kLocalMemoryBytes);
        destroyTargets();
        m_scale = scale;
        std::cerr << "[gs:gpu] internal resolution x" << m_scale << std::endl;
    }

    void Renderer::destroyTargets()
    {
        GL &gl = m_context.gl;
        for (auto &entry : m_framebuffers)
            gl.glDeleteFramebuffers(1, &entry.second);
        m_framebuffers.clear();
        for (auto &target : m_targets)
        {
            gl.glDeleteFramebuffers(1, &target->loadFramebuffer);
            gl.glDeleteTextures(1, &target->texture);
        }
        m_targets.clear();
        if (m_feedbackTexture != 0)
            gl.glDeleteTextures(1, &m_feedbackTexture);
        m_feedbackTexture = 0;
        m_feedbackWidth = m_feedbackHeight = 0;
        m_drawColor = m_drawDepth = nullptr;
    }

    bool Renderer::clearFramebuffer(const GSContext &context, uint32_t rgba)
    {
        readbackVram();
        const bool cleared = m_cpu->ClearFramebuffer(context, rgba);
        uploadVram(0u, kLocalMemoryBytes);
        return cleared;
    }

    uint32_t Renderer::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
    {
        return m_cpu->ConsumeLocalToHostBytes(dst, maxBytes);
    }

    uint32_t Renderer::readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        readbackVram();
        m_cpu->TextureFlush();
        return m_cpu->ReadVram(psm, base, bw, x, y);
    }

    void Renderer::writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        readbackVram();
        m_cpu->WriteVram(psm, base, bw, x, y, value);
        uploadVram(0u, kLocalMemoryBytes);
    }

    void Renderer::snapshotVram(std::vector<uint8_t> &out)
    {
        readbackVram();
        m_cpu->SnapshotVram(out);
    }

    GSTransferSnapshot Renderer::transferSnapshot()
    {
        if (!m_gpu || m_cpuTransfer)
            return m_cpu->GetTransferSnapshot();
        return m_transferState;
    }
}
