#include "MiniTest.h"
#include "runtime/ps2_mods.h"

#include <cstdint>
#include <string>
#include <vector>

// PCSX2 .pnach support: the parser turns the enabled "patch=1,EE,ADDR,TYPE,VALUE"
// lines into memory writes; the runtime writes them to EE RAM every frame.
void register_ps2_mods_tests()
{
    MiniTest::Case("Mods", [](TestCase &tc)
    {
        tc.Run("pnach parses enabled byte, short and word writes", [](TestCase &t)
        {
            const std::string text =
                "gametitle=Test game\n"
                "comment=ignored\n"
                "\n"
                "patch=1,EE,00100000,byte,2A\n"
                "patch=1,EE,00100002,short,BEEF\n"
                "patch=1,EE,00100004,word,CAFEBABE\n";
            const std::vector<PS2PnachEntry> entries = ps2PnachParse(text);
            t.Equals(entries.size(), static_cast<size_t>(3), "three enabled writes");
            t.Equals(entries[0].address, 0x00100000u, "byte address");
            t.Equals(static_cast<int>(entries[0].size), 1, "byte size");
            t.Equals(entries[0].value, 0x2Au, "byte value");
            t.Equals(static_cast<int>(entries[1].size), 2, "short size");
            t.Equals(entries[1].value, 0xBEEFu, "short value");
            t.Equals(static_cast<int>(entries[2].size), 4, "word size");
            t.Equals(entries[2].value, 0xCAFEBABEu, "word value");
        });

        tc.Run("pnach maps the 0x20xxxxxx uncached RAM mirror to physical RAM", [](TestCase &t)
        {
            const std::vector<PS2PnachEntry> entries = ps2PnachParse("patch=1,EE,20100004,word,00000001\n");
            t.Equals(entries.size(), static_cast<size_t>(1), "one write");
            t.Equals(entries[0].address, 0x00100004u, "mirror masked to physical RAM");
        });

        tc.Run("pnach skips disabled, unknown-type and out-of-range lines", [](TestCase &t)
        {
            const std::string text =
                "patch=0,EE,00100000,word,00000001\n"   // disabled
                "patch=1,EE,00100000,extended,00000001\n" // not supported
                "patch=1,EE,02000000,byte,01\n"          // past the end of RAM
                "patch=1,EE,FFFFFFFE,word,00000001\n"    // wraps past RAM after masking
                "patch=1,EE,00100008,byte,7F\n";         // the only valid one
            const std::vector<PS2PnachEntry> entries = ps2PnachParse(text);
            t.Equals(entries.size(), static_cast<size_t>(1), "only the valid line");
            t.Equals(entries[0].address, 0x00100008u, "valid address");
            t.Equals(entries[0].value, 0x7Fu, "valid value");
        });

        tc.Run("pnach ignores conditional/pointer code words", [](TestCase &t)
        {
            const std::string text =
                "patch=1,EE,00100000,word,D0123456\n" // conditional
                "patch=1,EE,00100004,word,E0123456\n" // conditional
                "patch=1,EE,00100008,word,50000001\n" // pointer
                "patch=1,EE,0010000C,word,00000005\n";
            const std::vector<PS2PnachEntry> entries = ps2PnachParse(text);
            t.Equals(entries.size(), static_cast<size_t>(1), "only the plain write");
            t.Equals(entries[0].value, 0x5u, "plain value kept");
        });

        tc.Run("pnach tolerates whitespace and CRLF line endings", [](TestCase &t)
        {
            const std::vector<PS2PnachEntry> entries = ps2PnachParse("patch=1, EE , 00100010 , word , 00400000 \r\n");
            t.Equals(entries.size(), static_cast<size_t>(1), "parsed despite spacing");
            t.Equals(entries[0].address, 0x00100010u, "address trimmed");
            t.Equals(entries[0].value, 0x400000u, "value trimmed");
        });

        tc.Run("pnach writes land little-endian and are bounds-checked", [](TestCase &t)
        {
            std::vector<uint8_t> ram(64, 0);
            const std::vector<PS2PnachEntry> entries = {
                {0x04u, 1, 0xABu},
                {0x08u, 2, 0x1234u},
                {0x0Cu, 4, 0xDEADBEEFu},
                {0x3Eu, 4, 0x11223344u}, // 0x3E + 4 > 64: skipped
            };
            ps2PnachApply(entries, ram.data(), ram.size());
            t.Equals(static_cast<int>(ram[0x04]), 0xAB, "byte written");
            t.Equals(static_cast<int>(ram[0x08]), 0x34, "short low byte");
            t.Equals(static_cast<int>(ram[0x09]), 0x12, "short high byte");
            t.Equals(static_cast<int>(ram[0x0C]), 0xEF, "word byte 0");
            t.Equals(static_cast<int>(ram[0x0F]), 0xDE, "word byte 3");
            t.Equals(static_cast<int>(ram[0x3E]), 0, "out-of-range write skipped");
        });
    });
}
