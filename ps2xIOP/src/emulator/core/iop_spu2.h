#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2: two cores of 24 ADPCM voices sharing 2 MiB of sound RAM, mixed to
    // 48 kHz stereo. Registers live at 0x1F900000-0x1F9007FF (16-bit). Sound
    // RAM is fed by IOP DMA channel 4 (core 0) / 7 (core 1), either as plain
    // transfers to the transfer start address (TSA) or as AutoDMA streams that
    // the cores play directly as their sound input.
    //
    // Not emulated: reverb, noise, pitch modulation, volume sweeps (treated as
    // fixed), SPDIF.
    class Spu2
    {
    public:
        static constexpr uint32_t RamBytes = 2u * 1024u * 1024u;
        static constexpr uint32_t SampleRate = 48000u;
        static constexpr uint32_t IopCyclesPerSample = 768u; // 36.864 MHz / 48 kHz
        static constexpr uint32_t RegisterBase = 0x1F900000u;
        static constexpr uint32_t RegisterEnd = 0x1F900800u;
        static constexpr int IrqLine = 9;

        Spu2();

        void reset();

        [[nodiscard]] uint16_t read16(uint32_t phys);
        void write16(uint32_t phys, uint16_t value);

        // IOP DMA channel 4/7 start. `ram` is IOP RAM. Returns the cycle delay
        // after which a plain transfer completes (0 for AutoDMA, which signals
        // completion through takeDmaIrqMask()).
        uint64_t startDma(uint32_t core, const uint8_t *ram, size_t ramSize, uint32_t madr, uint32_t bcr, uint32_t chcr);
        void dmaCompleted(uint32_t core);
        // AutoDMA progress as seen through the IOP DMA registers.
        [[nodiscard]] bool autoDmaActive(uint32_t core) const { return m_cores[core & 1u].adma.active; }
        [[nodiscard]] uint32_t autoDmaAddress(uint32_t core) const { return m_cores[core & 1u].adma.address; }

        // Produces one 48 kHz stereo frame.
        void tick(const uint8_t *ram, size_t ramSize, int16_t &outLeft, int16_t &outRight);

        // Cores whose AutoDMA transfer finished since the last call (bit per core).
        [[nodiscard]] uint32_t takeDmaIrqMask();
        // True if the SPU2 interrupt (IRQA hit) was raised since the last call.
        [[nodiscard]] bool takeSpuIrq();

    private:
        enum class EnvelopePhase : uint8_t
        {
            Off,
            Attack,
            Decay,
            Sustain,
            Release,
        };

        struct Voice
        {
            uint16_t volL = 0;
            uint16_t volR = 0;
            uint16_t pitch = 0;
            uint16_t adsr1 = 0;
            uint16_t adsr2 = 0;
            uint32_t ssa = 0; // halfword addresses
            uint32_t lsa = 0;
            uint32_t nax = 0;
            bool lsaSetByRegister = false;

            EnvelopePhase phase = EnvelopePhase::Off;
            int32_t envelope = 0; // 0..0x7FFF
            uint32_t envelopeCounter = 0;

            uint32_t counter = 0; // 4.12 position within the decoded block
            std::array<int16_t, 28> decoded{};
            int16_t history[4]{};  // last four output samples for interpolation
            int32_t prev1 = 0;     // ADPCM filter state
            int32_t prev2 = 0;
            uint32_t blockAddress = 0; // halfword address of the decoded block
            uint8_t blockFlags = 0;
            uint32_t blockPosition = 0; // next decoded sample to consume
            bool needBlock = true;
        };

        struct AutoDma
        {
            bool active = false;
            uint32_t address = 0;   // next IOP RAM address to fetch
            uint32_t remaining = 0; // bytes left in the DMA transfer
            std::array<int16_t, 256> left{};
            std::array<int16_t, 256> right{};
            uint32_t position = 256; // samples played from the current block
            bool haveBlock = false;
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint16_t attr = 0;
            uint16_t admas = 0;
            uint16_t statx = 0x80;
            uint16_t mmix = 0;
            uint32_t irqa = 0;
            uint32_t tsa = 0;
            uint32_t endx = 0;
            uint32_t vmixl = 0;
            uint32_t vmixr = 0;
            uint16_t mvolL = 0;
            uint16_t mvolR = 0;
            uint16_t extVolL = 0; // AVOL
            uint16_t extVolR = 0;
            uint16_t inpVolL = 0; // BVOL
            uint16_t inpVolR = 0;
            AutoDma adma{};
            std::array<uint16_t, 0x400> raw{}; // last written value per halfword register
        };

        uint16_t readCoreRegister(uint32_t core, uint32_t offset);
        void writeCoreRegister(uint32_t core, uint32_t offset, uint16_t value);
        void keyOn(uint32_t core, uint32_t voiceMask);
        void keyOff(uint32_t core, uint32_t voiceMask);
        void decodeBlock(uint32_t core, Voice &voice);
        int32_t voiceSample(uint32_t core, uint32_t index, Voice &voice);
        void advanceEnvelope(Voice &voice);
        void checkIrq(uint32_t core, uint32_t halfwordAddress, uint32_t halfwordCount);
        void fetchAutoDmaBlock(uint32_t core, const uint8_t *ram, size_t ramSize);

        std::vector<uint8_t> m_ram;
        std::array<Core, 2> m_cores{};
        std::array<uint16_t, 0x40> m_global{}; // 0x7C0-0x7FF
        // PS2X_SPU_TRACE diagnostics.
        bool m_trace = false;
        bool m_mixAutoDma = false; // PS2X_SPU_ADMA: movie streams are still timing-garbled
        uint32_t m_traceWrites = 0;
        uint64_t m_traceTicks = 0;
        uint64_t m_statKeyOns = 0;
        uint64_t m_statDmaPlain = 0;
        uint64_t m_statDmaAuto = 0;
        uint64_t m_statNonZero = 0;
        void traceReport();

        uint16_t m_irqInfo = 0;
        uint32_t m_dmaIrqMask = 0;
        bool m_spuIrq = false;
    };
}
