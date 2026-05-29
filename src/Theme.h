#pragma once
#include <windows.h>

enum class ThemePreset { Dark, Light, Accent, Forest, Sunset };

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
};

inline const ThemeColors& GetThemeColors(ThemePreset p)
{
    static constexpr ThemeColors presets[] = {
        // Dark
        {
            RGB(30, 30, 30),    RGB(50, 50, 50),    RGB(70, 70, 70),
            RGB(0, 84, 153),    RGB(80, 80, 80),    RGB(240, 240, 240),
            RGB(110, 110, 110), RGB(40, 40, 40),    RGB(0, 120, 212),
            RGB(240, 240, 240), RGB(70, 70, 70),    L"Dark"
        },
        // Light
        {
            RGB(235, 235, 235), RGB(215, 215, 215), RGB(195, 195, 195),
            RGB(0, 120, 212),   RGB(185, 185, 185), RGB(30, 30, 30),
            RGB(150, 150, 150), RGB(245, 245, 245), RGB(0, 120, 212),
            RGB(30, 30, 30),    RGB(180, 180, 180), L"Light"
        },
        // Accent (Windows blue)
        {
            RGB(0, 42, 84),     RGB(0, 60, 120),    RGB(0, 90, 160),
            RGB(0, 120, 212),   RGB(0, 80, 140),    RGB(255, 255, 255),
            RGB(130, 170, 210), RGB(0, 50, 100),    RGB(0, 100, 180),
            RGB(255, 255, 255), RGB(0, 70, 130),    L"Accent"
        },
        // Forest (green)
        {
            RGB(20, 45, 20),    RGB(30, 65, 30),    RGB(45, 90, 45),
            RGB(60, 140, 60),   RGB(40, 80, 40),    RGB(200, 240, 200),
            RGB(100, 150, 100), RGB(25, 55, 25),    RGB(50, 120, 50),
            RGB(200, 240, 200), RGB(40, 80, 40),    L"Forest"
        },
        // Sunset (orange)
        {
            RGB(50, 25, 0),     RGB(75, 40, 0),     RGB(110, 60, 0),
            RGB(200, 100, 0),   RGB(100, 55, 0),    RGB(255, 230, 200),
            RGB(180, 130, 80),  RGB(60, 30, 0),     RGB(180, 90, 0),
            RGB(255, 230, 200), RGB(100, 55, 0),    L"Sunset"
        },
    };
    return presets[static_cast<int>(p)];
}
