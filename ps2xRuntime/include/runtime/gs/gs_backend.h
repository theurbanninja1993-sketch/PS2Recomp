#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    // Frame interpolation: warp the last composed frame forward by half the last
    // camera motion and return the texture to draw (0 if it cannot make one),
    // with its size. This is a *blocking* round trip where Present is not: the
    // caller has to have the texture before it draws. Backends that cannot do it
    // leave it alone.
    virtual uint32_t SynthesizeFrame(uint32_t &width, uint32_t &height)
    {
        (void)width;
        (void)height;
        return 0;
    }

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // Restrict drawing to the 8-row strips with (y / 8) % count == index, for
    // banded multi-threaded rasterization. Transfers and clears are unaffected.
    virtual void SetRowBand(uint32_t index, uint32_t count) { (void)index; (void)count; }
    // Banded workers share VRAM: only one of them performs a host->local
    // upload's writes; the others call UploadImage with writes disabled so
    // their transfer bookkeeping still advances.
    virtual void SetUploadWrites(bool enabled) { (void)enabled; }
};
