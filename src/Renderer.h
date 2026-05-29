#pragma once
#include <windows.h>
#include <vector>
#include "Theme.h"
#include "TaskButton.h"

class Renderer {
public:
    void Resize(int w, int h, HDC hdcRef);
    void Paint(HDC hdcTarget, int w, int h,
               const std::vector<TaskButton>& buttons,
               int hoveredIdx, int pressedIdx,
               int dragIdx, POINT ghostPt,
               const ThemeColors& colors, int dpi);

    ~Renderer();

private:
    HDC     hdcMem_  = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HFONT   hFont_   = nullptr;
    int     width_   = 0;
    int     height_  = 0;

    void CreateFont(int dpi);
    void DestroyResources();
};
