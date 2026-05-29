#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "Theme.h"
#include "TaskButton.h"

struct ScrollInfo {
    bool needed   = false;
    bool isHoriz  = true;
    RECT leftRect = {};
    RECT rightRect= {};
    bool canLeft  = false;
    bool canRight = false;
    int  hovered  = 0;   // 0=none 1=left/up 2=right/down
};

struct ClockInfo {
    bool         visible  = false;
    RECT         rect     = {};
    std::wstring timeLine;
    std::wstring dateLine;
};

class Renderer {
public:
    void Resize(int w, int h, HDC hdcRef);
    void Paint(HDC hdcTarget, int w, int h,
               const std::vector<TaskButton>& buttons,
               int hoveredIdx, int pressedIdx,
               int dragIdx, POINT ghostPt,
               const ThemeColors& colors, int dpi,
               const ClockInfo& clock,
               const ScrollInfo& scroll);

    ~Renderer();

private:
    HDC     hdcMem_  = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HFONT   hFont_   = nullptr;
    HFONT   hFontSm_ = nullptr;  // slightly smaller font for the date line
    int     width_   = 0;
    int     height_  = 0;

    void CreateFonts(int dpi);
    void DestroyResources();
};
