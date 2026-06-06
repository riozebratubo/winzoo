#pragma once
#include <windows.h>
#include <unordered_map>
#include <vector>
#include <functional>
#include "AppEntry.h"
#include "AppTreeNode.h"
#include "Settings.h"
#include "Theme.h"
#include "DragController.h"

enum class AppMenuCloseReason { Selection, Escape, ClickedOutside };

struct PowerOption {
    enum Action { Lock, SignOut, Sleep, Hibernate, Restart, Shutdown };
    std::wstring label;
    Action       action;
};

class AppMenuWindow {
public:
    // Query Windows for available power options and cache them for the run.
    // Call once at app startup.
    static void CachePowerOptions();

    // Show a native power-options popup anchored at ptScreen and execute the chosen action.
    static void ShowPowerSubmenu(HWND hwndOwner, POINT ptScreen);

    // Shows the app menu anchored to the start button rect.
    // Blocks until the menu (and any submenus) are dismissed.
    // Takes entries by value so the menu owns a stable snapshot.
    static void Show(HWND hwndOwner, RECT startBtnScreenRect,
                     TaskbarPosition position,
                     std::vector<AppEntry> entries,   // by value – owns snapshot
                     const Settings& settings,
                     const ThemeColors& colors, int dpi,
                     const std::wstring& monitorDeviceName = L"",
                     std::function<void(const Settings&)> onSettingsChanged = {});

private:
    // Internal Show used for both the root menu and recursive submenus.
    // anchorRect: screen rect of the element the menu should attach to.
    // isSubmenu:  true when opened as a folder sub-popup.
    // pChildHwnd: if non-null, receives the created HWND before the message
    //             loop starts so the parent can suppress its WM_KILLFOCUS.
    static AppMenuCloseReason ShowNodes(
        HWND hwndOwner, RECT anchorRect, bool isSubmenu,
        TaskbarPosition position,
        std::vector<AppTreeNode> nodes,
        const Settings& settings,
        const ThemeColors& colors, int dpi,
        HWND* pChildHwnd = nullptr,
        // Context for user-pinned apps. Submenus inherit the callback, monitor name and
        // shared-settings pointer from the root so pinning works at any level:
        int pinnedCount = 0,
        std::vector<AppEntry> entries = {},
        const std::wstring& monitorDeviceName = L"",
        std::function<void(const Settings&)> onSettingsChanged = {},
        Settings* pinSettings = nullptr,
        HWND rootHwnd = nullptr);

    static bool RegisterWndClass(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void Paint(HDC hdc, int w, int h);
    int  HitTestEntry(POINT ptClient) const;
    int  HitTestSidebarBtn(POINT ptClient) const;
    int  HitTestClassicRight(POINT ptClient) const;
    bool HitTestClassicFooter(POINT ptClient) const;
    void ActivateNode(int idx);
    void ActivateSidebarBtn(int idx);
    void ShowEntryContextMenu(int idx, POINT ptScreen);
    void OnPinsMutated();    // persist + (root only) rebuild the displayed list
    void RebuildRootNodes(); // rebuild ownedNodes_ (pinned section first) and re-layout
    int  ComputeDropIndex(POINT ptClient) const; // insertion index within the pinned section
    void ActivateClassicRight(int linkIdx);
    void ToggleAllPrograms();
    void Scroll(int delta);
    void EnsureVisible(int idx);
    void SetHoveredIdx(int idx);
    void UpdateMaxScroll(int contentH, int clientH);
    void BuildEntryRects(int menuW);
    RECT GetNodeScreenRect(int idx) const; // returns screen coords of entry i

    // Search
    void ApplyFilter();
    int  SearchBoxHeight() const;

    const std::vector<AppTreeNode>* nodes_    = nullptr;
    const Settings*                 settings_ = nullptr;
    ThemeColors                     colors_   = {};
    int                             dpi_      = 96;
    TaskbarPosition                 position_ = TaskbarPosition::Bottom;
    bool                            isSubmenu_ = false;

    std::vector<AppTreeNode> ownedNodes_;
    Settings                 ownedSettings_;

    // User-pinned apps.
    std::vector<AppEntry>                ownedEntries_;       // snapshot for rebuilding nodes (root)
    std::wstring                         monitorDeviceName_;  // for per-monitor pin list
    std::function<void(const Settings&)> onSettingsChanged_;  // persist + propagate callback
    // Authoritative settings used for reading/mutating the pin list. Points at the root
    // menu's ownedSettings_ so submenus mutate the same shared list.
    Settings*                            pinSettings_ = nullptr;
    HWND                                 rootHwnd_    = nullptr; // root menu window (for live refresh)
    int                                  pinnedCount_ = 0;    // # pinned nodes at front of ownedNodes_
    DragController                       drag_;
    int                                  dropIndex_ = -1;     // current drag insertion index

    std::vector<RECT> entryRects_;  // content-space (un-scrolled)

    int  scrollOffset_    = 0;
    int  maxScrollOffset_ = 0;
    int  hoveredIdx_      = -1;
    bool done_            = false;
    AppMenuCloseReason closeReason_ = AppMenuCloseReason::ClickedOutside;

    // Set to the child submenu's HWND while it is open; cleared on close.
    // Used by WM_KILLFOCUS to avoid closing while focus is in a child submenu.
    HWND subMenuHwnd_ = nullptr;

    HWND hwnd_  = nullptr;
    int  menuW_ = 0;  // content width (excludes sidebar)
    int  menuH_ = 0;

    // Sidebar
    int  sidebarW_          = 0;  // 0 if disabled
    int  sidebarHoveredBtn_ = -1; // 0=Explorer, 1=Settings, 2=Power, 3=WinzooSettings, 4=AllApps
    bool suppressKillFocus_ = false;

    // Search box
    bool                     searchEnabled_ = false;  // resolved from settings + isSubmenu
    std::wstring             searchText_;
    std::vector<AppTreeNode> filteredNodes_;
    int                      searchBoxH_ = 0;  // pixel height reserved for the search area
    HWND                     searchEdit_  = nullptr;  // child EDIT control
    HBRUSH                   editBgBrush_ = nullptr;  // background brush for the EDIT

    // Classic (Vista/7) two-panel layout state
    int  classicPanelW_        = 0;     // right panel width (0 when not classic)
    int  classicFooterH_       = 0;     // height of All Programs/Back footer row
    int  classicHoveredRight_  = -1;    // hovered link index in right panel
    bool classicFooterHovered_ = false;
    bool inAllPrograms_        = false;
    std::vector<AppTreeNode> allProgramsNodes_;  // pre-built flat list for All Programs
    HBITMAP profilePicBmp_     = nullptr;        // user account picture (may be null)

    // Cached folder icons (loaded lazily in Paint, destroyed in WM_DESTROY).
    HICON folderIconList_ = nullptr;
    HICON folderIconGrid_ = nullptr;

    // Icons for synthesised search results — freed in WM_DESTROY.
    HICON settingsIcon_ = nullptr;
    std::unordered_map<std::wstring, HICON> exeIconCache_;
};
