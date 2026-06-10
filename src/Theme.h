#pragma once
#include <windows.h>

static constexpr int kThemePresetCount = 11;

enum class ThemePreset {
    Default = 0, Light, Ocean, Forest, Sunset,
    Midnight, Rose, Nord, Slate, Mocha, Custom
};

struct ThemeColors {
    COLORREF background;
    COLORREF buttonNormal;
    COLORREF buttonHover;
    COLORREF buttonActive;
    COLORREF buttonBorder;
    COLORREF text;
    COLORREF textDimmed;
    COLORREF menuBg;
    COLORREF menuHover;
    COLORREF menuText;
    COLORREF separator;
    const wchar_t* name;
    COLORREF taskbarBase;  // base taskbar color (used in swatch UI)
    COLORREF accentBase;   // base accent color  (used in swatch UI)
};

struct ThemeBase { COLORREF taskbar, accent; const wchar_t* name; };

namespace ThemeDetail {

inline int Clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

inline float Lum(COLORREF c) {
    return (0.299f * GetRValue(c) + 0.587f * GetGValue(c) + 0.114f * GetBValue(c)) / 255.0f;
}

inline COLORREF Adj(COLORREF c, int d) {
    return RGB(Clamp(GetRValue(c)+d), Clamp(GetGValue(c)+d), Clamp(GetBValue(c)+d));
}

inline COLORREF Blend(COLORREF a, COLORREF b, float t) {
    float u = 1.0f - t;
    return RGB(Clamp(int(GetRValue(a)*u + GetRValue(b)*t)),
               Clamp(int(GetGValue(a)*u + GetGValue(b)*t)),
               Clamp(int(GetBValue(a)*u + GetBValue(b)*t)));
}

inline ThemeColors Derive(COLORREF taskbar, COLORREF accent, const wchar_t* name) {
    bool dark = Lum(taskbar) < 0.45f;
    int  lgt  = dark ? 20 : -20;

    COLORREF text     = dark ? RGB(232,232,232) : RGB(20,20,20);
    COLORREF textDim  = Blend(taskbar, text, 0.50f);
    COLORREF btnNorm  = Adj(taskbar, lgt);
    COLORREF btnHov   = Blend(btnNorm, accent, 0.25f);
    COLORREF btnBord  = Adj(taskbar, lgt / 2);
    COLORREF menuBg   = Adj(taskbar, lgt / 3);
    COLORREF menuHov  = Blend(menuBg, accent, 0.30f);
    COLORREF sep      = Adj(taskbar, lgt);

    return { taskbar, btnNorm, btnHov, accent, btnBord,
             text, textDim, menuBg, menuHov, text, sep, name,
             taskbar, accent };
}

} // namespace ThemeDetail

inline const ThemeBase& GetThemeBase(ThemePreset p)
{
    static constexpr ThemeBase kBases[kThemePresetCount] = {
        { RGB( 30, 30, 30),   RGB(  0,120,212), L"Default"  },
        { RGB(235,235,235),   RGB(  0,120,212), L"Light"    },
        { RGB(  0, 30, 60),   RGB(  0,160,210), L"Ocean"    },
        { RGB( 20, 42, 20),   RGB( 60,160, 60), L"Forest"   },
        { RGB( 45, 20,  5),   RGB(220,100, 20), L"Sunset"   },
        { RGB( 12, 10, 22),   RGB(110, 70,220), L"Midnight" },
        { RGB( 35, 12, 22),   RGB(220, 60,120), L"Rose"     },
        { RGB( 46, 52, 64),   RGB(136,192,208), L"Nord"     },
        { RGB( 22, 28, 36),   RGB( 38,166,154), L"Slate"    },
        { RGB( 28, 20, 14),   RGB(180,130, 85), L"Mocha"    },
        // "Custom": these are only fallback defaults — the live values come from
        // Settings::customThemeTaskbar / customThemeAccent via GetThemeColorsEx().
        { RGB( 40, 40, 55),   RGB(120, 90,200), L"Custom"   },
    };
    return kBases[static_cast<int>(p)];
}

inline ThemeColors GetThemeColors(ThemePreset p)
{
    const auto& b = GetThemeBase(p);
    return ThemeDetail::Derive(b.taskbar, b.accent, b.name);
}

// Resolves the chosen preset to its two base colors, honoring the user-selectable
// "Custom" theme, then applies the optional user taskbar color override on top.
//   customThemeTaskbar / customThemeAccent — used only when p == ThemePreset::Custom.
inline ThemeColors GetThemeColorsEx(ThemePreset p, bool useCustom, COLORREF customTaskbar,
                                    COLORREF customThemeTaskbar = RGB(40, 40, 55),
                                    COLORREF customThemeAccent  = RGB(120, 90, 200))
{
    const auto& b = GetThemeBase(p);
    bool isCustom = (p == ThemePreset::Custom);
    COLORREF baseTaskbar = isCustom ? customThemeTaskbar : b.taskbar;
    COLORREF accent      = isCustom ? customThemeAccent  : b.accent;
    COLORREF taskbar = useCustom ? customTaskbar : baseTaskbar;
    return ThemeDetail::Derive(taskbar, accent, b.name);
}
