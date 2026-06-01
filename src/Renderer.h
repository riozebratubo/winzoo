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

struct StartButtonInfo {
    bool visible = false;
    RECT rect    = {};
    bool hovered = false;
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

struct StatusZoneInfo {
    bool visible = false;
    RECT rect    = {};          // full zone rect (client coords)

    bool volAvailable = false;
    RECT volRect      = {};
    float volLevel    = 0.0f;
    bool  volMuted    = false;
    bool  volHovered  = false;

    bool netAvailable = false;
    RECT netRect      = {};
    bool netConnected = false;
    bool netHovered   = false;

    bool batAvailable = false;
    RECT batRect      = {};
    bool batOnAC      = false;
    bool batCharging  = false;
    int  batPercent   = 0;
    bool batHovered   = false;
};

struct TrayZoneInfo {
    bool visible = false;

    struct Icon {
        RECT  rect    = {};
        HICON hIcon   = nullptr;
        bool  hovered = false;
    };
    std::vector<Icon> icons;

    int   dragGhostIdx = -1;  // index of icon being dragged (-1 = none)
    POINT ghostPt      = {};  // screen-relative cursor while dragging
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
               const ScrollInfo& scroll,
               const StartButtonInfo& startBtn,
               const MinimizedIndicatorOptions& indicator = {},
               int pinnedSepX = 0,
               const StatusZoneInfo& status = {},
               const TrayZoneInfo& tray = {},
               const ProgressBarOptions& progressBar = {});

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
