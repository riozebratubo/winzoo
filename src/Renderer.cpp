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

void Renderer::CreateFonts(int dpi)
{
    if (hFont_)   { DeleteObject(hFont_);   hFont_   = nullptr; }
    if (hFontSm_) { DeleteObject(hFontSm_); hFontSm_ = nullptr; }

    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(9, dpi, 72);
    lf.lfWeight  = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    hFont_ = CreateFontIndirectW(&lf);

    lf.lfHeight = -MulDiv(8, dpi, 72);
    hFontSm_ = CreateFontIndirectW(&lf);
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

void Renderer::Paint(HDC hdcTarget, int w, int h,
                     const std::vector<TaskButton>& buttons,
                     int hoveredIdx, int pressedIdx,
                     int dragIdx, POINT ghostPt,
                     const ThemeColors& colors, int dpi,
                     const ClockInfo& clock)
{
    if (!hdcMem_) return;

    if (!hFont_) CreateFonts(dpi);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdcMem_, hFont_));

    // Background
    RECT rc = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(colors.background);
    FillRect(hdcMem_, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Task buttons (skip the one being dragged)
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        if (i == dragIdx) continue;
        buttons[i].Draw(hdcMem_, colors,
                        i == hoveredIdx, i == pressedIdx,
                        false, dpi);
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
        ghost.Draw(hdcMem_, colors, false, false, true, dpi);

        // Drop indicator line
        for (int i = 0; i <= static_cast<int>(buttons.size()); ++i) {
            int cx = (i < static_cast<int>(buttons.size()))
                     ? buttons[i].rect.left
                     : buttons[i - 1].rect.right;
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

    // Clock
    if (clock.visible && (clock.rect.right > clock.rect.left)) {
        int clkH  = clock.rect.bottom - clock.rect.top;
        int halfH = clkH / 2;

        // Divider line on the leading edge of the clock area
        HPEN divPen = CreatePen(PS_SOLID, 1, colors.separator);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdcMem_, divPen));
        MoveToEx(hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.top,    nullptr);
        LineTo(  hdcMem_, clock.rect.left - MulDiv(3, dpi, 96), clock.rect.bottom);
        SelectObject(hdcMem_, oldPen);
        DeleteObject(divPen);

        SetBkMode(hdcMem_, TRANSPARENT);

        // Time (upper half) — normal font, full brightness
        RECT timeRect = { clock.rect.left, clock.rect.top,
                          clock.rect.right, clock.rect.top + halfH };
        SelectObject(hdcMem_, hFont_);
        SetTextColor(hdcMem_, colors.text);
        DrawTextW(hdcMem_, clock.timeLine.c_str(), -1, &timeRect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

        // Date (lower half) — small font, dimmed
        RECT dateRect = { clock.rect.left, clock.rect.top + halfH,
                          clock.rect.right, clock.rect.bottom };
        SelectObject(hdcMem_, hFontSm_);
        SetTextColor(hdcMem_, colors.textDimmed);
        DrawTextW(hdcMem_, clock.dateLine.c_str(), -1, &dateRect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    SelectObject(hdcMem_, oldFont);

    BitBlt(hdcTarget, 0, 0, w, h, hdcMem_, 0, 0, SRCCOPY);
}
