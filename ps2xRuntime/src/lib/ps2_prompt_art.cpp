// Runtime prompt art: keyboard keycaps that follow the live key bindings.
//
// See ps2_prompt_art.h. The keyboard set's keycap labels come from the same
// actions the shipped art names (W/LMB/S/E, the arrow keys, SPACE/Q/RMB for
// the map icons), so a rebind updates the art the same way the text labels
// already update via vcsButtonLabel() in game_overrides.cpp.
//
// The runtime cannot reuse the Python tools, so the keycaps are drawn with
// raylib's image API and exported as PNGs. Alpha is written in the GS scale
// (0x80 = opaque) the replacement pipeline expects.
#include "runtime/ps2_prompt_art.h"

#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_mods.h"

#include "raylib.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    // Which bound action each shipped keyboard prompt texture shows.
    struct KeyArt
    {
        const char *fileName;
        int action;
    };
    const KeyArt kKeyArt[] = {
        {"60a3a644467171e5-acbe9ae672fed823-00001113.png", ps2controls::MoveForward},   // cross
        {"d0d9f396c36b7411-bd20758f94447ef8-00001113.png", ps2controls::EnterVehicle},  // triangle
        {"a1b7cc92002291c5-b97a81b80c22e4c8-00001113.png", ps2controls::MenuUp},        // d-pad
        {"47e8e408d74357d8-b97a81b80c22e4c8-00001113.png", ps2controls::MenuDown},
        {"784c33b4a3241b19-b97a81b80c22e4c8-00001113.png", ps2controls::MenuLeft},
        {"3c60f292c5015937-b97a81b80c22e4c8-00001113.png", ps2controls::MenuRight},
        {"c075546801b74bf1-6b7c470bee19b607-00001113.png", ps2controls::Jump},     // map target
        {"1c274bfc4808cd11-21fc390fe03ad8c0-00001113.png", ps2controls::Interact}, // map zoom L
        {"23ece69c64b71407-f6ce098a91a7ac7b-00001113.png", ps2controls::Aim},      // map zoom R
    };

    // The font-atlas cells the tutorial prompts live in (make_font_sheet.py).
    const char *kSheetName = "3ece7b57667e958b-f663bcea3bca5023-00002213.png";
    struct SheetCell
    {
        int x, y, w, h;
        int action;
    };
    const SheetCell kSheetCells[] = {
        {0, 230, 24, 22, ps2controls::MoveBackward},   // square
        {24, 230, 22, 22, ps2controls::MoveForward},   // cross
        {46, 230, 22, 22, ps2controls::Fire},          // circle
        {212, 206, 22, 22, ps2controls::EnterVehicle}, // triangle
    };

    constexpr int kTrackedActions[] = {
        ps2controls::MoveForward, ps2controls::MoveBackward, ps2controls::Fire, ps2controls::EnterVehicle,
        ps2controls::Jump, ps2controls::Interact, ps2controls::Aim,
        ps2controls::MenuUp, ps2controls::MenuDown, ps2controls::MenuLeft, ps2controls::MenuRight};

    std::string configBase()
    {
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            return xdg;
        if (const char *appData = std::getenv("APPDATA"); appData && *appData)
            return appData;
        if (const char *home = std::getenv("HOME"); home && *home)
            return (fs::path(home) / ".config").string();
        return ".";
    }

    const Font &promptFont()
    {
        static const Font s_font = []()
        {
            static const char *kPaths[] = {
                "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/TTF/DejaVuSans.ttf",
                "C:/Windows/Fonts/segoeuib.ttf",
                "C:/Windows/Fonts/arialbd.ttf",
                "C:/Windows/Fonts/segoeui.ttf",
            };
            // The default set is ASCII only; the d-pad keycaps want arrows.
            int codepoints[99];
            int count = 0;
            for (int c = 32; c <= 126; ++c)
                codepoints[count++] = c;
            for (int c = 0x2190; c <= 0x2193; ++c)
                codepoints[count++] = c;
            for (const char *path : kPaths)
                if (FileExists(path))
                {
                    Font font = LoadFontEx(path, 64, codepoints, count);
                    if (font.texture.id != 0)
                        return font;
                }
            Font none{};
            return none; // not loaded: the shipped art stays in use
        }();
        return s_font;
    }

    // rounded rectangle = inner rectangles + four corner circles
    void roundedRect(Image *img, Rectangle rec, float radius, Color color)
    {
        ImageDrawRectangleRec(img, (Rectangle){rec.x + radius, rec.y, rec.width - 2 * radius, rec.height}, color);
        ImageDrawRectangleRec(img, (Rectangle){rec.x, rec.y + radius, rec.width, rec.height - 2 * radius}, color);
        ImageDrawCircleV(img, (Vector2){rec.x + radius, rec.y + radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + rec.width - radius, rec.y + radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + radius, rec.y + rec.height - radius}, radius, color);
        ImageDrawCircleV(img, (Vector2){rec.x + rec.width - radius, rec.y + rec.height - radius}, radius, color);
    }

    // Keycap: dark rounded square, gray rim, white label (matches the tools).
    Image makeKeycap(const std::string &label, int size, const Font &font)
    {
        Image img = GenImageColor(size, size, BLANK);
        const float pad = static_cast<float>(std::max(2, size / 16));
        const float rim = static_cast<float>(std::max(2, size / 16));
        const Rectangle rec = {pad, pad, static_cast<float>(size) - 2 * pad - 1, static_cast<float>(size) - 2 * pad - 1};
        roundedRect(&img, rec, rec.width * 0.22f, (Color){138, 148, 166, 255});
        const Rectangle inner = {rec.x + rim, rec.y + rim, rec.width - 2 * rim, rec.height - 2 * rim};
        roundedRect(&img, inner, inner.width * 0.2f, (Color){32, 38, 46, 255});
        if (font.texture.id != 0)
        {
            float fs = size * 0.5f;
            Vector2 m = MeasureTextEx(font, label.c_str(), fs, 0);
            const float target = inner.width * 0.8f;
            if (m.x > target)
                fs *= target / m.x;
            m = MeasureTextEx(font, label.c_str(), fs, 0);
            ImageDrawTextEx(&img, font, label.c_str(),
                            (Vector2){(size - m.x) / 2, (size - m.y) / 2}, fs, 0, (Color){232, 236, 242, 255});
        }
        return img;
    }

    // 0..255 authored alpha -> the GS scale the replacement pipeline uses.
    void toGsAlpha(Image &img)
    {
        if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        unsigned char *p = static_cast<unsigned char *>(img.data);
        for (int i = 0; i < img.width * img.height; ++i, p += 4)
            p[3] = static_cast<unsigned char>((p[3] * 128 + 127) / 255);
    }

    std::string bindingLabel(int action)
    {
        const auto &keys = ps2HostSettings().keys[action];
        const int binding = keys[0] != ps2controls::kNone ? keys[0] : keys[1];
        switch (binding)
        {
        case KEY_UP: return "\u2191";    // the d-pad keycaps use arrows, like the
        case KEY_DOWN: return "\u2193";  // shipped art (promptBindingName spells
        case KEY_LEFT: return "\u2190";  // them out for menu text instead)
        case KEY_RIGHT: return "\u2192";
        default: break;
        }
        std::string s = ps2controls::promptBindingName(binding);
        return s.empty() ? std::string("?") : s;
    }

    uint64_t bindingSignature()
    {
        uint64_t h = 1469598103934665603ull;
        for (int action : kTrackedActions)
            for (int i = 0; i < 2; ++i)
            {
                h ^= static_cast<uint64_t>(static_cast<uint32_t>(ps2HostSettings().keys[action][i]));
                h *= 1099511628211ull;
            }
        return h;
    }
} // namespace

std::string ps2PromptArtDirectory()
{
    return (fs::path(configBase()) / "ps2recomp" / "prompts_rt").string();
}

bool ps2PromptArtRefreshIfNeeded()
{
    const std::string set = ps2CurrentPromptSet();
    if (set != "keyboard")
        return false; // console sets keep the shipped art for now
    const Font &font = promptFont();
    if (font.texture.id == 0)
        return false; // no usable font: keep the shipped art
    static uint64_t s_appliedSig = 0;
    static std::string s_appliedSet;
    const uint64_t sig = bindingSignature();
    if (sig == s_appliedSig && set == s_appliedSet)
        return false;

    std::error_code ec;
    const fs::path outDir = fs::path(ps2PromptArtDirectory()) / set;
    fs::create_directories(outDir, ec);
    for (const KeyArt &art : kKeyArt)
    {
        Image img = makeKeycap(bindingLabel(art.action), 64, font);
        toGsAlpha(img);
        ExportImage(img, (outDir / art.fileName).string().c_str());
        UnloadImage(img);
    }
    const fs::path shipped = fs::path(ps2PromptsDirectory()) / set / kSheetName;
    if (Image sheet = LoadImage(shipped.string().c_str()); sheet.data != nullptr)
    {
        for (const SheetCell &cell : kSheetCells)
        {
            ImageDrawRectangleRec(&sheet, {(float)cell.x, (float)cell.y, (float)cell.w, (float)cell.h}, BLANK);
            Image cap = makeKeycap(bindingLabel(cell.action), 40, font);
            ImageResize(&cap, cell.w, cell.h);
            toGsAlpha(cap);
            ImageDraw(&sheet, cap, {0, 0, (float)cap.width, (float)cap.height},
                      {(float)cell.x, (float)cell.y, (float)cell.w, (float)cell.h}, WHITE);
            UnloadImage(cap);
        }
        ExportImage(sheet, (outDir / kSheetName).string().c_str());
        UnloadImage(sheet);
    }
    s_appliedSig = sig;
    s_appliedSet = set;
    if (std::getenv("PS2X_PROMPT_TRACE") != nullptr)
        std::cerr << "[prompts] runtime keycap art written to " << outDir.string() << std::endl;
    return true;
}
