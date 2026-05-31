#include "Renderer.h"

Renderer::~Renderer()
{
    DestroyResources();
}

void Renderer::DestroyResources()
{
    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }
    if (hdcMem_)  { DeleteDC(hdcMem_);      hdcMem_  = nullptr; }
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

void Renderer::Resize(int w, int h, HDC hdcRef)
{
    if (w == width_ && h == height_ && hdcMem_) return;

    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }
    if (hdcMem_)  { DeleteDC(hdcMem_);      hdcMem_  = nullptr; }

    width_  = w;
    height_ = h;

    hdcMem_  = CreateCompatibleDC(hdcRef);
    hBitmap_ = CreateCompatibleBitmap(hdcRef, w, h);
    SelectObject(hdcMem_, hBitmap_);
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
                     int pinnedSepX)
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
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        if (i == dragIdx) continue;
        const RECT& r = buttons[i].rect;
        if (r.left == 0 && r.right == 0 && r.top == 0 && r.bottom == 0) continue;
        buttons[i].Draw(hdcMem_, colors,
                        i == hoveredIdx, i == pressedIdx,
                        false, dpi, indicator);
    }

    // Drag ghost
    if (dragIdx >= 0 && dragIdx < static_cast<int>(buttons.size())) {
        const auto& btn = buttons[dragIdx];
        int bw = btn.rect.right  - btn.rect.left;
        int bh = btn.rect.bottom - btn.rect.top;
        TaskButton ghost = btn;
        ghost.rect = {
            ghostPt.x - bw / 2, ghostPt.y - bh / 2,
            ghostPt.x + bw / 2, ghostPt.y + bh / 2
        };
        ghost.Draw(hdcMem_, colors, false, false, true, dpi, indicator);

        // Drop indicator line — skip hidden buttons (rect={})
        for (int i = 0; i <= static_cast<int>(buttons.size()); ++i) {
            int cx;
            if (i < static_cast<int>(buttons.size())) {
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
        HPEN divPen = CreatePen(PS_SOLID, 1, colors.separator);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, divPen));
        MoveToEx(hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.top,    nullptr);
        LineTo(  hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.bottom);
        SelectObject(hdcMem_, oldPen);
        DeleteObject(divPen);

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
        HPEN sepPen = CreatePen(PS_SOLID, 1, colors.separator);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, sepPen));
        // Vertical separator (assuming horizontal bar; works for vertical too)
        MoveToEx(hdcMem_, startBtn.rect.right, startBtn.rect.top,    nullptr);
        LineTo(  hdcMem_, startBtn.rect.right, startBtn.rect.bottom);
        SelectObject(hdcMem_, oldPen);
        DeleteObject(sepPen);
    }

    // Pinned zone separator
    if (pinnedSepX > 0) {
        HPEN sepPen = CreatePen(PS_SOLID, 1, colors.separator);
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

    BitBlt(hdcTarget, 0, 0, w, h, hdcMem_, 0, 0, SRCCOPY);
}
