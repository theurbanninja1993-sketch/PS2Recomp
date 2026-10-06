#include "iop_spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamHalfwords = Spu2::RamBytes / 2u;
        constexpr uint32_t kAddressMask = kRamHalfwords - 1u;
        constexpr uint32_t kVoiceCount = 24u;

        // Register offsets within a core (core 1 adds 0x400).
        constexpr uint32_t kRegPmon = 0x180u;
        constexpr uint32_t kRegNon = 0x184u;
        constexpr uint32_t kRegVmixl = 0x188u;
        constexpr uint32_t kRegVmixel = 0x18Cu;
        constexpr uint32_t kRegVmixr = 0x190u;
        constexpr uint32_t kRegVmixer = 0x194u;
        constexpr uint32_t kRegMmix = 0x198u;
        constexpr uint32_t kRegAttr = 0x19Au;
        constexpr uint32_t kRegIrqa = 0x19Cu;
        constexpr uint32_t kRegKon = 0x1A0u;
        constexpr uint32_t kRegKoff = 0x1A4u;
        constexpr uint32_t kRegTsa = 0x1A8u;
        constexpr uint32_t kRegData = 0x1ACu;
        constexpr uint32_t kRegAdmas = 0x1B0u;
        constexpr uint32_t kRegVoiceAddr = 0x1C0u;
        constexpr uint32_t kRegVoiceAddrEnd = kRegVoiceAddr + kVoiceCount * 12u;
        constexpr uint32_t kRegEndx = 0x340u;
        constexpr uint32_t kRegStatx = 0x344u;

        // Core volume blocks (absolute offsets from the register base).
        constexpr uint32_t kVolumeBase[2] = {0x760u, 0x788u};
        constexpr uint32_t kVolumeBlockSize = 0x28u;
        constexpr uint32_t kGlobalBase = 0x7C0u;
        constexpr uint32_t kRegIrqInfo = 0x7C2u;

        constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};

        int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        // Fixed-mode volume (bit 15 clear): bits 14-0 are volume / 2, signed.
        // Sweep mode isn't emulated; approximate it by its direction.
        int32_t volumeValue(uint16_t reg)
        {
            if ((reg & 0x8000u) == 0u)
                return static_cast<int16_t>(static_cast<uint16_t>(reg << 1u));
            return (reg & 0x2000u) != 0u ? 0 : 0x7FFF;
        }

        void setLow16(uint32_t &value, uint16_t low) { value = (value & 0xFFFF0000u) | low; }
        void setHigh8(uint32_t &value, uint16_t high) { value = (value & 0x0000FFFFu) | (static_cast<uint32_t>(high & 0xFFu) << 16u); }
        void setAddrHigh(uint32_t &value, uint16_t high) { value = ((value & 0xFFFFu) | (static_cast<uint32_t>(high & 0xFu) << 16u)) & kAddressMask; }
        void setAddrLow(uint32_t &value, uint16_t low) { value = ((value & 0xF0000u) | low) & kAddressMask; }
    }

    Spu2::Spu2()
        : m_ram(RamBytes)
    {
        m_trace = std::getenv("PS2X_SPU_TRACE") != nullptr;
        m_mixAutoDma = std::getenv("PS2X_SPU_ADMA") != nullptr;
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        for (uint32_t index = 0; index < 2u; ++index)
        {
            // State the BIOS leaves behind: every gate and voice route open,
            // input volumes at maximum (cf. PCSX2 V_Core::Init).
            Core &core = m_cores[index];
            core = Core{};
            core.mmix = index == 0u ? 0xFF0u : 0xFFCu;
            core.vmixl = 0xFFFFFFu;
            core.vmixr = 0xFFFFFFu;
            core.extVolL = core.extVolR = 0x7FFFu;
            core.inpVolL = core.inpVolR = 0x7FFFu;
        }
        m_global.fill(0u);
        m_irqInfo = 0;
        m_dmaIrqMask = 0;
        m_spuIrq = false;
    }

    uint16_t Spu2::read16(uint32_t phys)
    {
        const uint32_t offset = (phys - RegisterBase) & 0x7FEu;
        for (uint32_t core = 0; core < 2u; ++core)
        {
            if (offset >= kVolumeBase[core] && offset < kVolumeBase[core] + kVolumeBlockSize)
            {
                const Core &c = m_cores[core];
                switch (offset - kVolumeBase[core])
                {
                case 0x10u: // MVOLXL: current master volume
                    return static_cast<uint16_t>(volumeValue(c.mvolL));
                case 0x12u:
                    return static_cast<uint16_t>(volumeValue(c.mvolR));
                default:
                    return c.raw[(offset & 0x3FEu) >> 1u];
                }
            }
        }
        if (offset >= kGlobalBase)
        {
            if (offset == kRegIrqInfo)
            {
                const uint16_t info = m_irqInfo;
                m_irqInfo = 0;
                return info;
            }
            return m_global[(offset - kGlobalBase) >> 1u];
        }
        return readCoreRegister(offset >= 0x400u ? 1u : 0u, offset & 0x3FEu);
    }

    void Spu2::write16(uint32_t phys, uint16_t value)
    {
        const uint32_t offset = (phys - RegisterBase) & 0x7FEu;
        if (m_trace && m_traceWrites < 400u)
        {
            ++m_traceWrites;
            std::cerr << "[spu2:w] off=0x" << std::hex << offset << " val=0x" << value << std::dec << std::endl;
        }
        for (uint32_t core = 0; core < 2u; ++core)
        {
            if (offset >= kVolumeBase[core] && offset < kVolumeBase[core] + kVolumeBlockSize)
            {
                Core &c = m_cores[core];
                c.raw[(offset & 0x3FEu) >> 1u] = value;
                switch (offset - kVolumeBase[core])
                {
                case 0x0u: c.mvolL = value; break;
                case 0x2u: c.mvolR = value; break;
                case 0x8u: c.extVolL = value; break; // AVOL: external input (core 0 -> core 1)
                case 0xAu: c.extVolR = value; break;
                case 0xCu: c.inpVolL = value; break; // BVOL: sound data input (AutoDMA)
                case 0xEu: c.inpVolR = value; break;
                default: break; // effect volumes / reverb coefficients
                }
                return;
            }
        }
        if (offset >= kGlobalBase)
        {
            m_global[(offset - kGlobalBase) >> 1u] = value;
            return;
        }
        writeCoreRegister(offset >= 0x400u ? 1u : 0u, offset & 0x3FEu, value);
    }

    uint16_t Spu2::readCoreRegister(uint32_t core, uint32_t offset)
    {
        Core &c = m_cores[core];
        if (offset < kVoiceCount * 16u)
        {
            const Voice &v = c.voices[offset >> 4u];
            switch (offset & 0xFu)
            {
            case 0x0: return v.volL;
            case 0x2: return v.volR;
            case 0x4: return v.pitch;
            case 0x6: return v.adsr1;
            case 0x8: return v.adsr2;
            case 0xA: return static_cast<uint16_t>(v.envelope);
            case 0xC: return static_cast<uint16_t>(volumeValue(v.volL));
            default: return static_cast<uint16_t>(volumeValue(v.volR));
            }
        }
        if (offset >= kRegVoiceAddr && offset < kRegVoiceAddrEnd)
        {
            const Voice &v = c.voices[(offset - kRegVoiceAddr) / 12u];
            switch ((offset - kRegVoiceAddr) % 12u)
            {
            case 0: return static_cast<uint16_t>(v.ssa >> 16u);
            case 2: return static_cast<uint16_t>(v.ssa);
            case 4: return static_cast<uint16_t>(v.lsa >> 16u);
            case 6: return static_cast<uint16_t>(v.lsa);
            case 8: return static_cast<uint16_t>(v.nax >> 16u);
            default: return static_cast<uint16_t>(v.nax);
            }
        }
        switch (offset)
        {
        case kRegAttr: return c.attr;
        case kRegIrqa: return static_cast<uint16_t>(c.irqa >> 16u);
        case kRegIrqa + 2u: return static_cast<uint16_t>(c.irqa);
        case kRegTsa: return static_cast<uint16_t>(c.tsa >> 16u);
        case kRegTsa + 2u: return static_cast<uint16_t>(c.tsa);
        case kRegAdmas: return c.admas;
        case kRegEndx: return static_cast<uint16_t>(c.endx);
        case kRegEndx + 2u: return static_cast<uint16_t>(c.endx >> 16u);
        case kRegStatx: return c.statx;
        default: return c.raw[offset >> 1u];
        }
    }

    void Spu2::writeCoreRegister(uint32_t core, uint32_t offset, uint16_t value)
    {
        Core &c = m_cores[core];
        c.raw[offset >> 1u] = value;
        if (offset < kVoiceCount * 16u)
        {
            Voice &v = c.voices[offset >> 4u];
            switch (offset & 0xFu)
            {
            case 0x0: v.volL = value; break;
            case 0x2: v.volR = value; break;
            case 0x4: v.pitch = value; break;
            case 0x6: v.adsr1 = value; break;
            case 0x8: v.adsr2 = value; break;
            case 0xA: v.envelope = static_cast<int16_t>(value) < 0 ? 0 : value; break;
            default: break;
            }
            return;
        }
        if (offset >= kRegVoiceAddr && offset < kRegVoiceAddrEnd)
        {
            Voice &v = c.voices[(offset - kRegVoiceAddr) / 12u];
            switch ((offset - kRegVoiceAddr) % 12u)
            {
            case 0: setAddrHigh(v.ssa, value); break;
            case 2: setAddrLow(v.ssa, value); break;
            case 4: setAddrHigh(v.lsa, value); v.lsaSetByRegister = true; break;
            case 6: setAddrLow(v.lsa, value); v.lsaSetByRegister = true; break;
            case 8: setAddrHigh(v.nax, value); break;
            default: setAddrLow(v.nax, value); break;
            }
            return;
        }
        switch (offset)
        {
        case kRegVmixl: setLow16(c.vmixl, value); break;
        case kRegVmixl + 2u: setHigh8(c.vmixl, value); break;
        case kRegVmixel: case kRegVmixel + 2u: break;
        case kRegVmixr: setLow16(c.vmixr, value); break;
        case kRegVmixr + 2u: setHigh8(c.vmixr, value); break;
        case kRegVmixer: case kRegVmixer + 2u: break;
        case kRegPmon: case kRegPmon + 2u: case kRegNon: case kRegNon + 2u: break;
        case kRegMmix: c.mmix = value; break;
        case kRegAttr:
            c.attr = value;
            if ((value & 0x40u) == 0u)
                m_irqInfo = static_cast<uint16_t>(m_irqInfo & ~(4u << core)); // IRQ acknowledged
            break;
        case kRegIrqa: setAddrHigh(c.irqa, value); break;
        case kRegIrqa + 2u: setAddrLow(c.irqa, value); break;
        case kRegKon: keyOn(core, value); break;
        case kRegKon + 2u: keyOn(core, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case kRegKoff: keyOff(core, value); break;
        case kRegKoff + 2u: keyOff(core, static_cast<uint32_t>(value & 0xFFu) << 16u); break;
        case kRegTsa: setAddrHigh(c.tsa, value); break;
        case kRegTsa + 2u: setAddrLow(c.tsa, value); break;
        case kRegData:
        {
            // Programmed I/O write to sound RAM at TSA.
            const uint32_t byteAddress = (c.tsa & kAddressMask) * 2u;
            std::memcpy(m_ram.data() + byteAddress, &value, sizeof(value));
            checkIrq(core, c.tsa, 1u);
            c.tsa = (c.tsa + 1u) & kAddressMask;
            break;
        }
        case kRegAdmas:
            c.admas = value;
            if (value == 0u)
            {
                c.adma.active = false;
                c.adma.haveBlock = false;
                c.adma.position = 256u;
            }
            break;
        case kRegEndx: c.endx &= 0xFF0000u; break;
        case kRegEndx + 2u: c.endx &= 0x00FFFFu; break;
        case kRegStatx: break;
        default: break;
        }
    }

    void Spu2::keyOn(uint32_t core, uint32_t voiceMask)
    {
        Core &c = m_cores[core];
        for (uint32_t index = 0; index < kVoiceCount; ++index)
        {
            if ((voiceMask & (1u << index)) == 0u)
                continue;
            Voice &v = c.voices[index];
            ++m_statKeyOns;
            if (m_trace && m_statKeyOns <= 200u)
            {
                const uint8_t *block = m_ram.data() + (v.ssa & kAddressMask & ~7u) * 2u;
                std::cerr << "[spu2:kon] core=" << core << " v=" << index << std::hex << " ssa=0x" << v.ssa
                          << " pitch=0x" << v.pitch << " adsr=" << v.adsr1 << "/" << v.adsr2 << " vol=" << v.volL << "/"
                          << v.volR << " hdr=" << static_cast<uint32_t>(block[0]) << "," << static_cast<uint32_t>(block[1])
                          << " mvol1=" << m_cores[1].mvolL << std::dec << std::endl;
            }
            v.phase = EnvelopePhase::Attack;
            v.envelope = 0;
            v.envelopeCounter = 0;
            v.counter = 0;
            v.nax = v.ssa;
            v.needBlock = true;
            v.lsaSetByRegister = false;
            v.prev1 = 0;
            v.prev2 = 0;
            std::fill(std::begin(v.history), std::end(v.history), int16_t{0});
            c.endx &= ~(1u << index);
        }
    }

    void Spu2::keyOff(uint32_t core, uint32_t voiceMask)
    {
        Core &c = m_cores[core];
        for (uint32_t index = 0; index < kVoiceCount; ++index)
        {
            if ((voiceMask & (1u << index)) == 0u)
                continue;
            Voice &v = c.voices[index];
            if (v.phase != EnvelopePhase::Off)
            {
                v.phase = EnvelopePhase::Release;
                v.envelopeCounter = 0;
            }
        }
    }

    void Spu2::checkIrq(uint32_t core, uint32_t halfwordAddress, uint32_t halfwordCount)
    {
        (void)core;
        for (uint32_t index = 0; index < 2u; ++index)
        {
            const Core &c = m_cores[index];
            if ((c.attr & 0x40u) == 0u)
                continue;
            const uint32_t distance = (c.irqa - halfwordAddress) & kAddressMask;
            if (distance < halfwordCount)
            {
                m_irqInfo = static_cast<uint16_t>(m_irqInfo | (4u << index));
                m_spuIrq = true;
            }
        }
    }

    void Spu2::decodeBlock(uint32_t core, Voice &voice)
    {
        const uint32_t blockAddress = voice.nax & kAddressMask & ~7u;
        const uint8_t *block = m_ram.data() + blockAddress * 2u;
        checkIrq(core, blockAddress, 8u);

        uint32_t shift = block[0] & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((block[0] >> 4u) & 0x7u, 4u);
        voice.blockFlags = block[1];
        voice.blockAddress = blockAddress;
        if ((voice.blockFlags & 0x4u) != 0u && !voice.lsaSetByRegister)
            voice.lsa = blockAddress;

        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint8_t byte = block[2u + i / 2u];
            const int32_t nibble = (i & 1u) ? (byte >> 4u) : (byte & 0xFu);
            int32_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u)) >> shift;
            sample += (voice.prev1 * kFilterPos[filter] + voice.prev2 * kFilterNeg[filter] + 32) >> 6;
            const int16_t clamped = clamp16(sample);
            voice.decoded[i] = clamped;
            voice.prev2 = voice.prev1;
            voice.prev1 = clamped;
        }
    }

    void Spu2::advanceEnvelope(Voice &voice)
    {
        int32_t shift = 0;
        int32_t step = 0;
        bool exponential = false;
        bool decreasing = false;
        switch (voice.phase)
        {
        case EnvelopePhase::Attack:
            shift = (voice.adsr1 >> 10u) & 0x1Fu;
            step = 7 - static_cast<int32_t>((voice.adsr1 >> 8u) & 0x3u);
            exponential = (voice.adsr1 & 0x8000u) != 0u;
            break;
        case EnvelopePhase::Decay:
            shift = (voice.adsr1 >> 4u) & 0xFu;
            step = -8;
            exponential = true;
            decreasing = true;
            break;
        case EnvelopePhase::Sustain:
            shift = (voice.adsr2 >> 8u) & 0x1Fu;
            decreasing = (voice.adsr2 & 0x4000u) != 0u;
            step = decreasing ? -8 + static_cast<int32_t>((voice.adsr2 >> 6u) & 0x3u)
                              : 7 - static_cast<int32_t>((voice.adsr2 >> 6u) & 0x3u);
            exponential = (voice.adsr2 & 0x8000u) != 0u;
            break;
        case EnvelopePhase::Release:
            shift = voice.adsr2 & 0x1Fu;
            step = -8;
            exponential = (voice.adsr2 & 0x20u) != 0u;
            decreasing = true;
            break;
        case EnvelopePhase::Off:
            return;
        }

        uint32_t cycles = 1u << std::max(0, shift - 11);
        int32_t delta = step << std::max(0, 11 - shift);
        if (exponential && !decreasing && voice.envelope > 0x6000)
            cycles *= 4u;
        if (exponential && decreasing)
            delta = (delta * voice.envelope) >> 15;

        if (++voice.envelopeCounter >= cycles)
        {
            voice.envelopeCounter = 0;
            voice.envelope = std::clamp(voice.envelope + delta, 0, 0x7FFF);
        }

        switch (voice.phase)
        {
        case EnvelopePhase::Attack:
            if (voice.envelope >= 0x7FFF)
            {
                voice.phase = EnvelopePhase::Decay;
                voice.envelopeCounter = 0;
            }
            break;
        case EnvelopePhase::Decay:
        {
            const int32_t sustainLevel = static_cast<int32_t>(((voice.adsr1 & 0xFu) + 1u) * 0x800u);
            if (voice.envelope <= sustainLevel)
            {
                voice.phase = EnvelopePhase::Sustain;
                voice.envelopeCounter = 0;
            }
            break;
        }
        case EnvelopePhase::Release:
            if (voice.envelope <= 0)
                voice.phase = EnvelopePhase::Off;
            break;
        default:
            break;
        }
    }

    int32_t Spu2::voiceSample(uint32_t core, uint32_t index, Voice &voice)
    {
        Core &c = m_cores[core];
        voice.counter += std::min<uint32_t>(voice.pitch, 0x3FFFu);
        while (voice.counter >= 0x1000u && voice.phase != EnvelopePhase::Off)
        {
            voice.counter -= 0x1000u;
            if (voice.needBlock)
            {
                decodeBlock(core, voice);
                voice.needBlock = false;
                voice.blockPosition = 0u;
            }
            voice.history[0] = voice.history[1];
            voice.history[1] = voice.history[2];
            voice.history[2] = voice.history[3];
            voice.history[3] = voice.decoded[voice.blockPosition];
            if (++voice.blockPosition < 28u)
                continue;

            // End of block: follow its loop flags.
            voice.needBlock = true;
            if ((voice.blockFlags & 0x1u) == 0u)
            {
                voice.nax = (voice.blockAddress + 8u) & kAddressMask;
                continue;
            }
            c.endx |= 1u << index;
            if ((voice.blockFlags & 0x2u) != 0u)
            {
                voice.nax = voice.lsa;
            }
            else
            {
                voice.phase = EnvelopePhase::Off;
                voice.envelope = 0;
                voice.nax = voice.blockAddress;
            }
        }

        const int32_t a = voice.history[2];
        const int32_t b = voice.history[3];
        return a + (((b - a) * static_cast<int32_t>(voice.counter)) >> 12);
    }

    uint64_t Spu2::startDma(uint32_t core, const uint8_t *ram, size_t ramSize, uint32_t madr, uint32_t bcr, uint32_t chcr)
    {
        Core &c = m_cores[core & 1u];
        const uint32_t blockWords = std::max<uint32_t>(bcr & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(bcr >> 16u, 1u);
        const uint32_t bytes = blockWords * blockCount * 4u;
        madr &= 0x1FFFFCu;

        if (m_trace)
            std::cerr << "[spu2:dma] core=" << core << std::hex << " madr=0x" << madr << " bcr=0x" << bcr
                      << " chcr=0x" << chcr << " admas=0x" << c.admas << " tsa=0x" << c.tsa << std::dec << std::endl;
        if (c.admas != 0u)
        {
            ++m_statDmaAuto;
            // AutoDMA: the core consumes the data as its sound input, 1 KiB
            // (256 left + 256 right samples) at a time.
            c.adma.active = bytes != 0u;
            c.adma.address = madr;
            c.adma.remaining = bytes;
            if (!c.adma.active)
                m_dmaIrqMask |= 1u << core;
            return 0u;
        }

        ++m_statDmaPlain;
        c.statx = static_cast<uint16_t>(c.statx & ~0x80u);
        if ((chcr & 1u) != 0u && ram != nullptr)
        {
            // Plain transfer from IOP RAM to sound RAM at TSA.
            const uint32_t halfwords = bytes / 2u;
            checkIrq(core, c.tsa, halfwords);
            for (uint32_t i = 0; i < halfwords; ++i)
            {
                const uint32_t source = madr + i * 2u;
                if (source + 2u > ramSize)
                    break;
                const uint32_t destination = ((c.tsa + i) & kAddressMask) * 2u;
                m_ram[destination] = ram[source];
                m_ram[destination + 1u] = ram[source + 1u];
            }
            c.tsa = (c.tsa + halfwords) & kAddressMask;
        }
        return std::max<uint64_t>(static_cast<uint64_t>(bytes / 4u) * 2u, 64u);
    }

    void Spu2::dmaCompleted(uint32_t core)
    {
        Core &c = m_cores[core & 1u];
        c.statx = static_cast<uint16_t>(c.statx | 0x80u);
    }

    void Spu2::fetchAutoDmaBlock(uint32_t core, const uint8_t *ram, size_t ramSize)
    {
        AutoDma &adma = m_cores[core].adma;
        if (!adma.active || adma.remaining == 0u)
        {
            adma.haveBlock = false;
            return;
        }
        // PS2X_ADMA_DUMP=<file>: raw 1 KiB AutoDMA blocks as fetched from IOP RAM.
        static FILE *s_admaDump = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_ADMA_DUMP");
            return path ? std::fopen(path, "wb") : nullptr;
        }();
        if (s_admaDump && adma.address + 0x400u <= ramSize)
        {
            std::fwrite(ram + adma.address, 1, 0x400u, s_admaDump);
            std::fflush(s_admaDump);
        }
        for (uint32_t i = 0; i < 256u; ++i)
        {
            const uint32_t left = adma.address + i * 2u;
            const uint32_t right = adma.address + 0x200u + i * 2u;
            int16_t l = 0;
            int16_t r = 0;
            if (right + 2u <= ramSize)
            {
                std::memcpy(&l, ram + left, sizeof(l));
                std::memcpy(&r, ram + right, sizeof(r));
            }
            adma.left[i] = l;
            adma.right[i] = r;
        }
        adma.address += 0x400u;
        adma.remaining = adma.remaining > 0x400u ? adma.remaining - 0x400u : 0u;
        adma.position = 0u;
        adma.haveBlock = true;
        if (adma.remaining == 0u)
        {
            // Last block taken: the transfer is done (the driver queues the next).
            adma.active = false;
            m_dmaIrqMask |= 1u << core;
            if (m_trace)
                std::cerr << "[spu2:adma-done] core=" << core << std::endl;
        }
    }

    void Spu2::tick(const uint8_t *ram, size_t ramSize, int16_t &outLeft, int16_t &outRight)
    {
        int32_t coreOutL[2] = {0, 0};
        int32_t coreOutR[2] = {0, 0};
        for (uint32_t core = 0; core < 2u; ++core)
        {
            Core &c = m_cores[core];
            int32_t voiceL = 0;
            int32_t voiceR = 0;
            for (uint32_t index = 0; index < kVoiceCount; ++index)
            {
                Voice &v = c.voices[index];
                if (v.phase == EnvelopePhase::Off)
                    continue;
                const int32_t sample = voiceSample(core, index, v);
                advanceEnvelope(v);
                const int32_t enveloped = (sample * v.envelope) >> 15;
                const uint32_t bit = 1u << index;
                if ((c.vmixl & bit) != 0u)
                    voiceL += (enveloped * volumeValue(v.volL)) >> 15;
                if ((c.vmixr & bit) != 0u)
                    voiceR += (enveloped * volumeValue(v.volR)) >> 15;
            }

            // Sound data input (AutoDMA stream).
            int32_t inputL = 0;
            int32_t inputR = 0;
            if (c.adma.active || c.adma.haveBlock)
            {
                if (c.adma.position >= 256u)
                    fetchAutoDmaBlock(core, ram, ramSize);
                if (c.adma.haveBlock && c.adma.position < 256u && !m_mixAutoDma)
                {
                    // Consumed for timing (IRQs, DMA progress) but not mixed.
                    ++c.adma.position;
                }
                else if (c.adma.haveBlock && c.adma.position < 256u)
                {
                    inputL = (c.adma.left[c.adma.position] * static_cast<int32_t>(c.inpVolL & 0x7FFFu)) >> 15;
                    inputR = (c.adma.right[c.adma.position] * static_cast<int32_t>(c.inpVolR & 0x7FFFu)) >> 15;
                    ++c.adma.position;
                }
            }

            // Core 0's output is core 1's external input.
            int32_t extL = 0;
            int32_t extR = 0;
            if (core == 1u)
            {
                extL = (coreOutL[0] * static_cast<int32_t>(c.extVolL & 0x7FFFu)) >> 15;
                extR = (coreOutR[0] * static_cast<int32_t>(c.extVolR & 0x7FFFu)) >> 15;
            }

            // Dry gates from MMIX (reverb/wet paths aren't emulated).
            int32_t dryL = 0;
            int32_t dryR = 0;
            if ((c.mmix & 0x800u) != 0u) dryL += voiceL;
            if ((c.mmix & 0x400u) != 0u) dryR += voiceR;
            if ((c.mmix & 0x080u) != 0u) dryL += inputL;
            if ((c.mmix & 0x040u) != 0u) dryR += inputR;
            if ((c.mmix & 0x008u) != 0u) dryL += extL;
            if ((c.mmix & 0x004u) != 0u) dryR += extR;

            coreOutL[core] = (clamp16(dryL) * volumeValue(c.mvolL)) >> 15;
            coreOutR[core] = (clamp16(dryR) * volumeValue(c.mvolR)) >> 15;
        }
        outLeft = clamp16(coreOutL[1]);
        outRight = clamp16(coreOutR[1]);
        if (m_trace)
        {
            if (outLeft != 0 || outRight != 0)
                ++m_statNonZero;
            if (++m_traceTicks % (SampleRate * 2u) == 0u)
                traceReport();
        }
    }

    void Spu2::traceReport()
    {
        std::cerr << "[spu2:stats] t=" << m_traceTicks / SampleRate << "s kon=" << m_statKeyOns
                  << " dma=" << m_statDmaPlain << " adma=" << m_statDmaAuto << " nonzero=" << m_statNonZero;
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const Core &c = m_cores[core];
            uint32_t active = 0;
            for (const Voice &v : c.voices)
                active += v.phase != EnvelopePhase::Off ? 1u : 0u;
            std::cerr << std::hex << " | c" << core << " attr=" << c.attr << " mvol=" << c.mvolL << "," << c.mvolR
                      << " inp=" << c.inpVolL << " ext=" << c.extVolL << " mmix=" << c.mmix << " vmixl=" << c.vmixl
                      << " admas=" << c.admas << std::dec << " active=" << active;
        }
        std::cerr << std::endl;
    }

    uint32_t Spu2::takeDmaIrqMask()
    {
        const uint32_t mask = m_dmaIrqMask;
        m_dmaIrqMask = 0;
        return mask;
    }

    bool Spu2::takeSpuIrq()
    {
        const bool raised = m_spuIrq;
        m_spuIrq = false;
        return raised;
    }
}
