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
    bool         visible    = false;
    RECT         rect       = {};
    std::wstring timeLine;
    std::wstring dateLine;
    int          timeFontPt   = 9;
    int          dateFontPt   = 8;
    int          lineSpacing  = 0;
    COLORREF     timeColor    = RGB(240, 240, 240);
    COLORREF     dateColor    = RGB(110, 110, 110);
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
    HDC     hdcMem_       = nullptr;
    HBITMAP hBitmap_      = nullptr;
    HFONT   hFont_        = nullptr;
    HFONT   hFontSm_      = nullptr;
    int     width_        = 0;
    int     height_       = 0;
    int     cachedTimePt_ = 0;
    int     cachedDatePt_ = 0;
    int     cachedDpi_    = 0;

    void CreateFonts(int timePt, int datePt, int dpi);
    void DestroyResources();
};
