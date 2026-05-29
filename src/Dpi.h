#pragma once
#include <windows.h>

inline int GetWindowDpi(HWND hwnd)
{
    if (hwnd && IsWindow(hwnd))
        return static_cast<int>(GetDpiForWindow(hwnd));
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi;
}

inline int Scale(int px, int dpi)
{
    return MulDiv(px, dpi, 96);
}
