#pragma once
#include <windows.h>
#include <vector>
#include "AppEntry.h"
#include "Settings.h"
#include "Theme.h"

class AppMenuWindow {
public:
    // Shows the app menu anchored to the start button rect.
    // Blocks until the menu is dismissed.
    // Takes entries by value so the menu owns a stable snapshot.
    static void Show(HWND hwndOwner, RECT startBtnScreenRect,
                     TaskbarPosition position,
                     std::vector<AppEntry> entries,   // by value – owns snapshot
                     const Settings& settings,
                     const ThemeColors& colors, int dpi);

private:
    static bool RegisterWndClass(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, int w, int h);
    int  HitTestEntry(POINT ptClient) const;
    void LaunchEntry(int idx);
    void Scroll(int delta);
    void UpdateMaxScroll(int contentH, int clientH);

    // Entry rects for hit-testing (content-space, not scrolled)
    void BuildEntryRects(int menuW);

    const std::vector<AppEntry>*  entries_  = nullptr;
    const Settings*               settings_ = nullptr;
    ThemeColors                   colors_   = {};
    int                           dpi_      = 96;

    std::vector<AppEntry>  ownedEntries_; // snapshot owned by this window
    Settings               ownedSettings_;

    std::vector<RECT> entryRects_;   // in content space (un-scrolled)

    int  scrollOffset_    = 0;  // pixel offset for list; row offset for grid
    int  maxScrollOffset_ = 0;
    int  hoveredIdx_      = -1;
    bool done_            = false;
    HWND hwnd_            = nullptr;

    int menuW_ = 0;
    int menuH_ = 0;  // actual client height
};
