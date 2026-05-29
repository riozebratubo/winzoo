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
}

void Renderer::CreateFont(int dpi)
{
    if (hFont_) { DeleteObject(hFont_); hFont_ = nullptr; }

    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(9, dpi, 72);
    lf.lfWeight  = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    hFont_ = CreateFontIndirectW(&lf);
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
                     const ThemeColors& colors, int dpi)
{
    if (!hdcMem_) return;

    if (!hFont_) CreateFont(dpi);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdcMem_, hFont_));

    // Background
    RECT rc = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(colors.background);
    FillRect(hdcMem_, &rc, bgBrush);
    DeleteObject(bgBrush);

    // Draw all buttons except the one being dragged
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        if (i == dragIdx) continue;
        const auto& btn = buttons[i];
        btn.Draw(hdcMem_, colors,
                 i == hoveredIdx, i == pressedIdx,
                 false, dpi);
    }

    // Draw drag ghost at cursor position
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

        // Drop indicator line between buttons
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

    SelectObject(hdcMem_, oldFont);

    BitBlt(hdcTarget, 0, 0, w, h, hdcMem_, 0, 0, SRCCOPY);
}
