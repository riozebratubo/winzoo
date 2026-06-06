#include "Renderer.h"
#include <utility>

Renderer::~Renderer()
{
    DestroyResources();
}

void Renderer::DestroyResources()
{
    if (hdcMem_)  {
        if (hOldBitmap_) { SelectObject(hdcMem_, hOldBitmap_); hOldBitmap_ = nullptr; }
        DeleteDC(hdcMem_);      hdcMem_  = nullptr;
    }
    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }
    if (hFont_)   { DeleteObject(hFont_);   hFont_   = nullptr; }
    if (hFontSm_) { DeleteObject(hFontSm_); hFontSm_ = nullptr; }
}

void Renderer::CreateFonts(int timePt, int datePt, int dpi)
{
    if (hFont_)   { DeleteObject(hFont_);   hFont_   = nullptr; }
    if (hFontSm_) { DeleteObject(hFontSm_); hFontSm_ = nullptr; }

    LOGFONTW lf = {};
    lf.lfWeight  = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");

    lf.lfHeight = -MulDiv(timePt, dpi, 72);
    hFont_ = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(datePt, dpi, 72);
    hFontSm_ = CreateFontIndirectW(&lf);

    cachedTimePt_ = timePt;
    cachedDatePt_ = datePt;
    cachedDpi_    = dpi;
}

int Renderer::MeasureSmallText(const std::wstring& text, int datePt, int dpi)
{
    if (text.empty()) return 0;

    HFONT font    = nullptr;
    bool  ownFont = false;
    if (hFontSm_ && datePt == cachedDatePt_ && dpi == cachedDpi_) {
        font = hFontSm_;
    } else {
        LOGFONTW lf  = {};
        lf.lfHeight  = -MulDiv(datePt, dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        font    = CreateFontIndirectW(&lf);
        ownFont = (font != nullptr);
    }
    if (!font) return 0;

    HDC  dc    = hdcMem_;
    bool ownDC = false;
    if (!dc) {
        dc    = CreateCompatibleDC(nullptr);
        ownDC = (dc != nullptr);
        if (!ownDC) { if (ownFont) DeleteObject(font); return 0; }
    }

    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SIZE  sz  = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    if (ownFont) DeleteObject(font);
    if (ownDC)   DeleteDC(dc);
    return sz.cx;
}

void Renderer::Resize(int w, int h, HDC hdcRef)
{
    if (w == width_ && h == height_ && hdcMem_) return;

    if (hdcMem_)  {
        if (hOldBitmap_) { SelectObject(hdcMem_, hOldBitmap_); hOldBitmap_ = nullptr; }
        DeleteDC(hdcMem_);      hdcMem_  = nullptr;
    }
    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }

    width_  = w;
    height_ = h;

    hdcMem_  = CreateCompatibleDC(hdcRef);
    hBitmap_ = CreateCompatibleBitmap(hdcRef, w, h);
    hOldBitmap_ = static_cast<HBITMAP>(SelectObject(hdcMem_, hBitmap_));
}

static void DrawVolumeIcon(HDC hdc, RECT r, float level, bool muted, COLORREF col)
{
    int rw = r.right - r.left;
    int rh = r.bottom - r.top;
    int cy = r.top + rh / 2;
    int sz = (std::min(rw, rh) - 4) / 2;
    if (sz < 2) return;

    HPEN   pen  = CreatePen(PS_SOLID, 1, col);
    HBRUSH br   = CreateSolidBrush(col);
    HPEN   oldP = static_cast<HPEN>  (SelectObject(hdc, pen));
    HBRUSH oldB = static_cast<HBRUSH>(SelectObject(hdc, br));

    // 6-point speaker polygon: rectangular body + trapezoidal cone
    int bodyW  = sz * 2 / 5;  // width of the rectangular body
    int bodyHH = sz * 2 / 5;  // half-height of the body (narrow)
    int coneHH = sz;           // half-height of the cone tip (wide)
    int ox     = r.left + 2;   // left edge of speaker

    POINT pts[6] = {
        { ox,              cy - bodyHH },  // top-left of body
        { ox + bodyW,      cy - bodyHH },  // top body-cone junction
        { ox + bodyW + sz, cy - coneHH },  // top of cone
        { ox + bodyW + sz, cy + coneHH },  // bottom of cone
        { ox + bodyW,      cy + bodyHH },  // bottom body-cone junction
        { ox,              cy + bodyHH },  // bottom-left of body (was missing)
    };
    Polygon(hdc, pts, 6);

    // Switch to no fill for arc outlines
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    SelectObject(hdc, oldB);
    DeleteObject(br);

    // tipX: rightmost point of the speaker cone
    int tipX = ox + bodyW + sz;

    if (!muted && level > 0.05f) {
        // Sound wave arcs: right semicircle centered at tipX.
        // GDI Arc in MM_TEXT (Y-down) draws CW on screen.
        // Start=12-o'clock, End=6-o'clock → arc travels 12→3→6 through right side.
        int r1 = sz * 2 / 3;
        Arc(hdc, tipX - r1, cy - r1, tipX + r1, cy + r1,
            tipX, cy - r1,   // start: 12 o'clock
            tipX, cy + r1);  // end:   6 o'clock

        if (level > 0.55f) {
            int r2 = sz + sz / 4;
            Arc(hdc, tipX - r2, cy - r2, tipX + r2, cy + r2,
                tipX, cy - r2,
                tipX, cy + r2);
        }
    } else if (muted) {
        int xs = sz / 3 + 1;
        int mx = tipX + 3;
        HPEN xp = CreatePen(PS_SOLID, 1, RGB(220, 60, 60));
        SelectObject(hdc, xp);
        MoveToEx(hdc, mx,          cy - xs, nullptr);
        LineTo  (hdc, mx + xs * 2, cy + xs);
        MoveToEx(hdc, mx + xs * 2, cy - xs, nullptr);
        LineTo  (hdc, mx,          cy + xs);
        SelectObject(hdc, pen);
        DeleteObject(xp);
    }

    SelectObject(hdc, oldP);
    DeleteObject(pen);
}

static void DrawNetworkIcon(HDC hdc, RECT r, bool connected, COLORREF col)
{
    int rw = r.right  - r.left;
    int rh = r.bottom - r.top;
    int sz = std::min(rw, rh);  // work within the square
    int bars = 4;
    // Leave 2px inset on each side; divide remaining width evenly
    int inner  = sz - 4;
    int barW   = std::max(2, inner / (bars * 2 - 1));
    int gap    = std::max(1, barW / 2);
    int totalW = bars * barW + (bars - 1) * gap;
    int ox     = r.left  + (rw - totalW) / 2;
    int base   = r.bottom - 2;
    int maxH   = sz - 4;

    HPEN   pen  = CreatePen(PS_SOLID, 1, col);
    HBRUSH brOn = CreateSolidBrush(col);

    for (int i = 0; i < bars; ++i) {
        int barH = std::max(2, maxH * (i + 1) / bars);
        RECT bar = {
            ox + i * (barW + gap),
            base - barH,
            ox + i * (barW + gap) + barW,
            base
        };
        if (connected) {
            FillRect(hdc, &bar, brOn);
        } else {
            // Disconnected: smallest bar filled, rest outlined
            if (i == 0) {
                FillRect(hdc, &bar, brOn);
            } else {
                HPEN   oldP = static_cast<HPEN>  (SelectObject(hdc, pen));
                HBRUSH oldB = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                Rectangle(hdc, bar.left, bar.top, bar.right, bar.bottom);
                SelectObject(hdc, oldP);
                SelectObject(hdc, oldB);
            }
        }
    }

    DeleteObject(brOn);
    DeleteObject(pen);
}

static void DrawBatteryIcon(HDC hdc, RECT r, int percent, bool onAC, bool charging, COLORREF col)
{
    int rw = r.right  - r.left;
    int rh = r.bottom - r.top;
    int bw = rw - 6;
    int bh = rh / 3;
    int bx = r.left + 3;
    int by = r.top  + (rh - bh) / 2;

    // Outline
    HPEN pen  = CreatePen(PS_SOLID, 1, col);
    HPEN oldP = static_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldB = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    Rectangle(hdc, bx, by, bx + bw, by + bh);

    // Nub
    int nubW = std::max(2, bw / 10);
    int nubH = std::max(2, bh / 3);
    RECT nub = { bx + bw, by + (bh - nubH) / 2, bx + bw + nubW, by + (bh + nubH) / 2 };
    HBRUSH nubBr = CreateSolidBrush(col);
    FillRect(hdc, &nub, nubBr);
    DeleteObject(nubBr);

    // Fill
    COLORREF fillCol = (percent > 20) ? col : RGB(220, 60, 60);
    if (onAC || charging) fillCol = RGB(80, 200, 80);
    int fillW = std::max(0, (bw - 4) * percent / 100);
    if (fillW > 0) {
        RECT fill = { bx + 2, by + 2, bx + 2 + fillW, by + bh - 2 };
        HBRUSH fillBr = CreateSolidBrush(fillCol);
        FillRect(hdc, &fill, fillBr);
        DeleteObject(fillBr);
    }

    SelectObject(hdc, oldP);
    SelectObject(hdc, oldB);
    DeleteObject(pen);
}

static void DrawScrollArrow(HDC hdc, RECT r, bool left, bool horiz,
                             bool enabled, bool hovered,
                             const ThemeColors& colors, int dpi)
{
    COLORREF bg = enabled ? (hovered ? colors.buttonHover : colors.buttonNormal)
                          : colors.background;
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &r, br);
    DeleteObject(br);

    COLORREF fg = enabled ? colors.text : colors.textDimmed;
    int cx = (r.left + r.right)  / 2;
    int cy = (r.top  + r.bottom) / 2;
    int sz = MulDiv(4, dpi, 96);

    POINT pts[3];
    if (horiz) {
        if (left) { pts[0]={cx+sz,cy-sz}; pts[1]={cx-sz,cy}; pts[2]={cx+sz,cy+sz}; }
        else      { pts[0]={cx-sz,cy-sz}; pts[1]={cx+sz,cy}; pts[2]={cx-sz,cy+sz}; }
    } else {
        if (left) { pts[0]={cx-sz,cy+sz}; pts[1]={cx,cy-sz}; pts[2]={cx+sz,cy+sz}; } // up
        else      { pts[0]={cx-sz,cy-sz}; pts[1]={cx,cy+sz}; pts[2]={cx+sz,cy-sz}; } // down
    }

    HBRUSH ap = CreateSolidBrush(fg);
    HPEN   pp = CreatePen(PS_SOLID, 1, fg);
    auto*  ob = static_cast<HBRUSH>(SelectObject(hdc, ap));
    auto*  op = static_cast<HPEN>(SelectObject(hdc, pp));
    Polygon(hdc, pts, 3);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(ap);
    DeleteObject(pp);
}

void Renderer::Paint(HDC hdcTarget, int w, int h,
                     const std::vector<TaskButton>& buttons,
                     int hoveredIdx, int pressedIdx,
                     int dragIdx, POINT ghostPt,
                     const ThemeColors& colors, int dpi,
                     const ClockInfo& clock,
                     const ScrollInfo& scroll,
                     const StartButtonInfo& startBtn,
                     const MinimizedIndicatorOptions& indicator,
                     int pinnedSepX,
                     const StatusZoneInfo& status,
                     const TrayZoneInfo& tray,
                     const ProgressBarOptions& progressBar,
                     int buttonBorderRadius,
                     bool showPinnedAsButtons,
                     bool showSeparators,
                     COLORREF separatorColor)
{
    if (!hdcMem_) return;

    if (!hFont_ || clock.timeFontPt != cachedTimePt_ ||
        clock.dateFontPt != cachedDatePt_ || dpi != cachedDpi_)
        CreateFonts(clock.timeFontPt, clock.dateFontPt, dpi);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdcMem_, hFont_));

    // Background
    RECT rc = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(colors.background);
    FillRect(hdcMem_, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Task buttons (skip the one being dragged and hidden buttons with zero rect)
    for (int i = 0; std::cmp_less(i, buttons.size()); ++i) {
        if (i == dragIdx) continue;
        const RECT& r = buttons[i].rect;
        if (r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0) continue;
        buttons[i].Draw(hdcMem_, colors,
                        i == hoveredIdx, i == pressedIdx,
                        false, dpi, indicator, progressBar, buttonBorderRadius, showPinnedAsButtons);
    }

    // Drag ghost
    if (dragIdx >= 0 && std::cmp_less(dragIdx, buttons.size())) {
        const auto& btn = buttons[dragIdx];
        int bw = btn.rect.right  - btn.rect.left;
        int bh = btn.rect.bottom - btn.rect.top;
        TaskButton ghost = btn;
        ghost.rect = {
            ghostPt.x - bw / 2, ghostPt.y - bh / 2,
            ghostPt.x + bw / 2, ghostPt.y + bh / 2
        };
        ghost.Draw(hdcMem_, colors, false, false, true, dpi, indicator, progressBar, buttonBorderRadius, showPinnedAsButtons);

        // Drop indicator line — skip hidden buttons (rect={})
        for (int i = 0; std::cmp_less_equal(i, buttons.size()); ++i) {
            int cx = 0;
            if (std::cmp_less(i, buttons.size())) {
                const RECT& r = buttons[i].rect;
                if (r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0) continue;
                cx = r.left;
            } else {
                const RECT& r = buttons[i - 1].rect;
                if (r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0) continue;
                cx = r.right;
            }
            if (std::abs(cx - ghostPt.x) < bw / 2) {
                HPEN linePen = CreatePen(PS_SOLID, Scale(2, dpi), colors.buttonActive);
                HPEN old = static_cast<HPEN>(SelectObject(hdcMem_, linePen));
                MoveToEx(hdcMem_, cx, 0, nullptr);
                LineTo(hdcMem_, cx, h);
                SelectObject(hdcMem_, old);
                DeleteObject(linePen);
                break;
            }
        }
    }

    // Scroll arrows
    if (scroll.needed) {
        DrawScrollArrow(hdcMem_, scroll.leftRect,  true,  scroll.isHoriz,
                        scroll.canLeft,  scroll.hovered == 1, colors, dpi);
        DrawScrollArrow(hdcMem_, scroll.rightRect, false, scroll.isHoriz,
                        scroll.canRight, scroll.hovered == 2, colors, dpi);
    }

    // Clock
    if (clock.visible && (clock.rect.right > clock.rect.left)) {
        int clkH  = clock.rect.bottom - clock.rect.top;
        int halfH = clkH / 2;
        int gap   = MulDiv(clock.lineSpacing, dpi, 96) / 2; // half-gap applied to each side

        // Divider line on the leading edge of the clock area
        if (showSeparators) {
            HPEN divPen = CreatePen(PS_SOLID, 1, separatorColor);
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, divPen));
            MoveToEx(hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.top,    nullptr);
            LineTo(  hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.bottom);
            SelectObject(hdcMem_, oldPen);
            DeleteObject(divPen);
        }

        SetBkMode(hdcMem_, TRANSPARENT);

        // Time (upper half) — normal font
        RECT timeRect = { clock.rect.left, clock.rect.top,
                          clock.rect.right, clock.rect.top + halfH - gap };
        SelectObject(hdcMem_, hFont_);
        SetTextColor(hdcMem_, clock.timeColor);
        DrawTextW(hdcMem_, clock.timeLine.c_str(), -1, &timeRect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

        // Date (lower half) — small font
        RECT dateRect = { clock.rect.left, clock.rect.top + halfH + gap,
                          clock.rect.right, clock.rect.bottom };
        SelectObject(hdcMem_, hFontSm_);
        SetTextColor(hdcMem_, clock.dateColor);
        DrawTextW(hdcMem_, clock.dateLine.c_str(), -1, &dateRect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    SelectObject(hdcMem_, oldFont);

    // Start button (drawn last so it's on top of background)
    if (startBtn.visible) {
        COLORREF bg = startBtn.hovered ? colors.buttonHover : colors.buttonNormal;
        HBRUSH sbBr = CreateSolidBrush(bg);
        FillRect(hdcMem_, &startBtn.rect, sbBr);
        DeleteObject(sbBr);

        // 3×3 grid of small squares, centered in the button rect
        int bw     = startBtn.rect.right  - startBtn.rect.left;
        int bh     = startBtn.rect.bottom - startBtn.rect.top;
        int dotSz  = MulDiv(4, dpi, 96);   // dot size in pixels
        int gap    = MulDiv(3, dpi, 96);   // gap between dots
        int total  = 3 * dotSz + 2 * gap; // 3 cols/rows
        int ox     = startBtn.rect.left + (bw - total) / 2;
        int oy     = startBtn.rect.top  + (bh - total) / 2;

        HBRUSH dotBr = CreateSolidBrush(colors.text);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                RECT dot = {
                    ox + col * (dotSz + gap),
                    oy + row * (dotSz + gap),
                    ox + col * (dotSz + gap) + dotSz,
                    oy + row * (dotSz + gap) + dotSz
                };
                FillRect(hdcMem_, &dot, dotBr);
            }
        }
        DeleteObject(dotBr);

        // Thin separator on the trailing edge
        if (showSeparators) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, separatorColor);
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, sepPen));
            MoveToEx(hdcMem_, startBtn.rect.right, startBtn.rect.top,    nullptr);
            LineTo(  hdcMem_, startBtn.rect.right, startBtn.rect.bottom);
            SelectObject(hdcMem_, oldPen);
            DeleteObject(sepPen);
        }
    }

    // Pinned zone separator
    if (showSeparators && pinnedSepX > 0) {
        HPEN sepPen = CreatePen(PS_SOLID, 1, separatorColor);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, sepPen));
        if (scroll.isHoriz) {
            MoveToEx(hdcMem_, pinnedSepX, 0, nullptr);
            LineTo  (hdcMem_, pinnedSepX, h);
        } else {
            MoveToEx(hdcMem_, 0,          pinnedSepX, nullptr);
            LineTo  (hdcMem_, w,          pinnedSepX);
        }
        SelectObject(hdcMem_, oldPen);
        DeleteObject(sepPen);
    }

    // Status zone (volume / network / battery)
    if (status.visible) {
        // Divider line on the leading edge of the status zone
        if (showSeparators) {
            HPEN divPen = CreatePen(PS_SOLID, 1, separatorColor);
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, divPen));
            int divX = status.rect.left - MulDiv(3, dpi, 96);
            MoveToEx(hdcMem_, divX, status.rect.top,    nullptr);
            LineTo  (hdcMem_, divX, status.rect.bottom);
            SelectObject(hdcMem_, oldPen);
            DeleteObject(divPen);
        }

        COLORREF iconCol = colors.textDimmed;
        if (status.volAvailable) {
            if (status.volHovered) {
                HBRUSH hb = CreateSolidBrush(colors.buttonHover);
                FillRect(hdcMem_, &status.volRect, hb);
                DeleteObject(hb);
            }
            DrawVolumeIcon(hdcMem_, status.volRect, status.volLevel, status.volMuted, iconCol);
        }
        if (status.netAvailable) {
            if (status.netHovered) {
                HBRUSH hb = CreateSolidBrush(colors.buttonHover);
                FillRect(hdcMem_, &status.netRect, hb);
                DeleteObject(hb);
            }
            DrawNetworkIcon(hdcMem_, status.netRect, status.netConnected, iconCol);
        }
        if (status.batAvailable) {
            if (status.batHovered) {
                HBRUSH hb = CreateSolidBrush(colors.buttonHover);
                FillRect(hdcMem_, &status.batRect, hb);
                DeleteObject(hb);
            }
            DrawBatteryIcon(hdcMem_, status.batRect,
                            status.batPercent, status.batOnAC, status.batCharging, iconCol);
        }
        if (status.langAvailable && !status.langText.empty()) {
            if (status.langHovered) {
                HBRUSH hb = CreateSolidBrush(colors.buttonHover);
                FillRect(hdcMem_, &status.langRect, hb);
                DeleteObject(hb);
            }
            SetBkMode(hdcMem_, TRANSPARENT);
            SelectObject(hdcMem_, hFontSm_);
            SetTextColor(hdcMem_, iconCol);
            SIZE sz = {};
            GetTextExtentPoint32W(hdcMem_, status.langText.c_str(),
                                  static_cast<int>(status.langText.size()), &sz);
            RECT lr   = status.langRect;
            int  rectW = static_cast<int>(lr.right - lr.left);
            int  textW = static_cast<int>(sz.cx);
            int  hOff  = rectW > textW ? (rectW - textW) / 2 : 0;
            lr.left  += hOff;
            lr.right  = lr.left + sz.cx;
            DrawTextW(hdcMem_, status.langText.c_str(), -1, &lr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        }
    }

    // Tray zone (notification-area icons)
    if (tray.visible && !tray.icons.empty()) {
        for (int i = 0; std::cmp_less(i, tray.icons.size()); ++i) {
            if (i == tray.dragGhostIdx) continue; // drawn separately as ghost
            const auto& ti = tray.icons[i];
            if (!ti.hIcon) continue;
            if (ti.hovered) {
                HBRUSH hb = CreateSolidBrush(colors.buttonHover);
                FillRect(hdcMem_, &ti.rect, hb);
                DeleteObject(hb);
            }
            int iw = ti.rect.right  - ti.rect.left;
            int ih = ti.rect.bottom - ti.rect.top;
            DrawIconEx(hdcMem_, ti.rect.left, ti.rect.top, ti.hIcon, iw, ih,
                       0, nullptr, DI_NORMAL);
        }

        // Ghost icon while dragging
        if (tray.dragGhostIdx >= 0 &&
            std::cmp_less(tray.dragGhostIdx, tray.icons.size()))
        {
            const auto& ti = tray.icons[tray.dragGhostIdx];
            if (ti.hIcon) {
                int iw = ti.rect.right  - ti.rect.left;
                int ih = ti.rect.bottom - ti.rect.top;
                // Draw semi-transparent ghost at cursor; use GDI alpha via DrawIconEx
                DrawIconEx(hdcMem_, tray.ghostPt.x - iw / 2, tray.ghostPt.y - ih / 2,
                           ti.hIcon, iw, ih, 0, nullptr, DI_NORMAL);
            }
        }
    }

    BitBlt(hdcTarget, 0, 0, w, h, hdcMem_, 0, 0, SRCCOPY);
}
