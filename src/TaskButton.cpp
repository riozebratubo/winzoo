#include "TaskButton.h"

void TaskButton::Draw(HDC hdc, const ThemeColors& colors,
                      bool hovered, bool pressed, bool isDragGhost, int dpi,
                      const MinimizedIndicatorOptions& indicator) const
{
    COLORREF bgColor;
    if (!IsRunning())
        bgColor = colors.buttonNormal;
    else if (isActive)
        bgColor = colors.buttonActive;
    else if (pressed)
        bgColor = colors.buttonHover;
    else if (hovered)
        bgColor = colors.buttonHover;
    else
        bgColor = colors.buttonNormal;

    COLORREF textColor = IsRunning() ? colors.text : colors.textDimmed;

    if (isDragGhost) {
        // Slightly lighter for ghost
        auto blend = [](COLORREF c) -> COLORREF {
            int r = (GetRValue(c) + 255) / 2;
            int g = (GetGValue(c) + 255) / 2;
            int b = (GetBValue(c) + 255) / 2;
            return RGB(r, g, b);
        };
        bgColor   = blend(bgColor);
        textColor = blend(textColor);
    }

    int radius = Scale(4, dpi);
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    HPEN   borderPen = CreatePen(PS_SOLID, 1, colors.buttonBorder);
    HPEN   oldPen   = static_cast<HPEN>(SelectObject(hdc, borderPen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, bgBrush));

    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bgBrush);
    DeleteObject(borderPen);

    int h = rect.bottom - rect.top;
    int iconSz = Scale(16, dpi);
    int pad    = Scale(4, dpi);

    if (icon) {
        int iconY = rect.top + (h - iconSz) / 2;
        DrawIconEx(hdc, rect.left + pad, iconY,
                   icon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
    }

    RECT textRect = {
        rect.left + pad + iconSz + pad,
        rect.top,
        rect.right - pad,
        rect.bottom
    };

    if (textRect.left < textRect.right) {
        SetTextColor(hdc, textColor);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, title.c_str(), -1, &textRect,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    // Minimized indicator: small accent-colored rectangle at bottom-right
    if (indicator.enabled && !isDragGhost && IsRunning()
        && IsWindow(hwnd) && IsIconic(hwnd))
    {
        int iw     = Scale(indicator.width,  dpi);
        int ih     = Scale(indicator.height, dpi);
        int margin = Scale(2, dpi);
        RECT indRect = {
            rect.right  - iw - margin,
            rect.bottom - ih - margin,
            rect.right  - margin,
            rect.bottom - margin
        };
        HBRUSH indBrush = CreateSolidBrush(colors.buttonActive);
        FillRect(hdc, &indRect, indBrush);
        DeleteObject(indBrush);
    }
}
