#pragma once
#include <windows.h>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include "Settings.h"
#include "Theme.h"
#include "AppBar.h"
#include "WindowTracker.h"
#include "DragController.h"
#include "Renderer.h"
#include "IconCache.h"
#include "PopupMenu.h"
#include "SettingsDialog.h"
#include "Dpi.h"
#include "AppEntry.h"
#include "AppScanner.h"
#include "AppIconCache.h"
#include "AppMenuWindow.h"
#include "SystemStatus.h"
#include "TrayIconProvider.h"
#include "TaskbarProxy.h"
#include <vector>

class TaskbarWindow {
public:
    bool Create(HINSTANCE hInst, const Settings& settings,
                HMONITOR hMonitor = nullptr, bool isPrimary = true);
    void SetMonitor(HMONITOR hMonitor, bool isPrimary);
    void Show();
    void Destroy();
    void ApplySettings(const Settings& s);

    HWND Hwnd() const { return hwnd_; }

private:
    static bool RegisterWndClass(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void LayoutButtons();
    // Keep every tracked window's ptMinPosition pointed at its taskbar button so that
    // ALL minimize paths (title-bar button, Win+Down, our own button) animate toward
    // winzoo and tuck the minimized stub behind the bar — instead of falling back to
    // the legacy "no taskbar" behavior (desktop title-bar stubs / wrong direction) now
    // that winzoo hides Explorer's taskbar. Called at the end of LayoutButtons.
    void UpdateMinimizeTargets();
    int  HitTestButton(POINT pt) const;
    // Insertion slot (gap index) nearest the cursor, restricted to the
    // combined-index range [zoneStart, zoneEnd]. Mirrors the drop-indicator
    // line so a drag always lands where the line is shown. Returns a value in
    // [zoneStart, zoneEnd] (clamped), so a drop near a zone edge is never lost.
    int  HitTestInsertIndex(POINT pt, int zoneStart, int zoneEnd) const;
    // Live-reorder the currently-dragged button to the insertion slot nearest
    // `pt` (client coords) within its own zone. Updates the drag index and
    // relayouts; returns true if the order changed. Does not persist.
    bool DragReorderTo(POINT pt);
    // Rebuild the persisted pinned-app path list(s) to match the current
    // pinnedButtons_ order, then save. Called once on drop.
    void PersistPinnedOrder();
    bool HitTestStartButton(POINT pt) const;
    void ActivateButton(int combinedIdx);
    void ShowButtonMenu(int combinedIdx, POINT ptScreen);
    void ShowBackgroundMenu(POINT ptScreen);
    void ShowStatusIconMenu(int which, POINT ptScreen);  // 1=vol, 2=net, 3=bat
    void ShowLangMenu(POINT ptScreen);
    void ShowAppMenu();
    void StartScanThread(bool isFirstScan);
    void StartIconLoadThread();

    // Background workers (app scan / icon load) that PostMessage results back to
    // hwnd_. Tracked rather than detached so they can be joined at shutdown —
    // otherwise an in-flight worker could post to (or leak its result against) a
    // destroyed/reused window. SpawnTracked reaps finished workers on each call to
    // keep the list bounded; JoinAllWorkers blocks until all have finished.
    void SpawnTracked(std::function<void()> work);
    void JoinAllWorkers();
    void LaunchApp(const wchar_t* exe, const wchar_t* args = nullptr, int nShow = SW_SHOWNORMAL);
    void OpenCalendarFlyout();   // Win+N → Win11 Notification Center + Calendar flyout
    RECT ClockHitRect() const;   // clockRect_ grown to full thickness + trailing padding
    RECT CalculateWindowRect() const;
    RECT GetStartBtnScreenRect() const;

    // Pinned buttons management
    void RebuildPinnedButtons();
    void UpdateMonitorDeviceName();

    // Pinned folders
    void ShowFolderMenu(int combinedIdx, POINT ptScreen);   // right-click menu for a folder button
    void ShowFolderPopup(int combinedIdx);                  // open the folder's grid popup
    void CreatePinnedFolder(POINT ptScreen);                // prompt for a name + add an empty folder
    void MoveAppIntoFolder(int appIdx, int folderIdx);      // drag-drop an app into a folder
    void DeletePinnedFolder(int folderIdx, bool keepChildren);
    void RemoveAppFromFolder(const std::wstring& folderToken, const std::wstring& appPath);
    void SetFolderCover(const std::wstring& folderToken, const std::wstring& appPath); // cover = app's icon
    void ResetFolderIcon(int folderIdx);                                               // back to default icon
    // Combined index of a pinned *folder* button under `pt` (excluding draggedIdx),
    // only when the dragged button is an app; otherwise -1.
    int  HitTestFolderTarget(POINT pt, int draggedIdx) const;
    // Pointers to the pin list(s) this taskbar currently draws from (global, the
    // current monitor's list, or — in floating mode — every per-monitor list).
    std::vector<std::vector<std::wstring>*> ActivePinLists(Settings& s) const;

    // Tray icon management
    void RefreshTrayIcons();                       // legacy cross-process scrape (fallback)
    void OnTrayPush(const struct WinzooTrayRecord& rec, const wchar_t* tip,
                    size_t tipChars, const BYTE* bgra);
    void InsertTrayIconOrdered(TrayIconEntry&& e);
    HICON TrayIconFromOwner(HWND owner);
    void PruneDeadTrayIcons();
    // Win11's network icon is XAML-only (uncapturable) — synthesize one from live state
    // and keep it pinned to the trailing end of the captured tray row.
    bool ShowNetInTrayRow() const;
    void EnsureNetworkTrayIcon();

    void ComputeClockFontSizes();
    int  EffectiveThicknessPx() const;

    // Combined-index helpers (0..P-1 = pinned, P..P+T-1 = task)
    int             TotalCount()        const;
    bool            IsPinnedIdx(int i)  const;
    const TaskButton& GetButtonByIdx(int i) const;
    TaskButton&       GetMutableButtonByIdx(int i);

    // Returns the HWND of the first task button whose exePath matches (case-insensitive)
    HWND            FindHwndByExePath(const std::wstring& exePath) const;
    // Returns true if any task button has a matching exePath
    bool            IsExeRunning(const std::wstring& exePath) const;

    // Returns exe file name without extension (for pinned button titles)
    static std::wstring ExeBaseName(const std::wstring& exePath);

    // Pointer-sized and container fields first (8-byte aligned on 64-bit)
    HWND            hwnd_           = nullptr;
    HINSTANCE       hInst_          = nullptr;
    HMONITOR        hMonitor_       = nullptr;
    std::vector<TaskButton>        pinnedButtons_;   // rebuilt from settings_.pinnedExePaths
    std::vector<AppEntry>          appEntries_;
    std::vector<TrayIconEntry>     trayIcons_;       // owns the HICONs (visible only)
    std::vector<TrayIconEntry>     hiddenTrayIcons_; // NIS_HIDDEN icons (tracked for state transitions)
    std::vector<RECT>              trayIconRects_;
    // In-flight background workers (UI-thread access only). Each carries a `done`
    // flag it sets on exit so SpawnTracked() can reap completed ones cheaply.
    struct AsyncWorker {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<AsyncWorker>       asyncWorkers_;
    std::wstring    monitorDeviceName_;
    std::wstring    currentLangText_;    // e.g. "EN-US" — updated by kTimerActiveWindow
    std::wstring    clockFitTimeFmt_;
    std::wstring    clockFitDateFmt_;
    HKL             currentHkl_      = nullptr;
    HICON           folderIcon_      = nullptr;  // cached stock folder icon (owned; DestroyIcon on close)
    ThemeColors     colors_         = {};
    Renderer        renderer_;
    IconCache       iconCache_;
    AppIconCache    appIconCache_;
    AppBar          appBar_;
    WindowTracker   tracker_;
    TaskbarProxy    proxy_;
    Settings        settings_;
    DragController  drag_;
    SystemStatusData statusData_    = {};
    POINT           trayDragPt_     = {}; // client coords

    // 4-byte aligned fields
    int             dpi_            = 96;
    int             fittedTimePt_   = 0;
    int             fittedDatePt_   = 0;
    int             clockFitDpi_    = 0;
    int             clockFitW_      = 0;
    int             clockFitTimePt_ = 0;
    int             clockFitDatePt_ = 0;
    int             hoveredIdx_     = -1;  // combined index
    int             dropFolderIdx_  = -1;  // combined index of folder under cursor during a drag
    int             hoveredScroll_  = 0;
    int             hoveredStatus_  = 0;   // 0=none 1=vol 2=net 3=bat
    int             scrollOffset_   = 0;
    int             maxScrollOffset_= 0;
    int             pinnedSepX_     = 0;   // x position of pinned/task zone separator
    UINT            shellHookMsg_      = 0;
    UINT            taskbarCreatedMsg_ = 0;
    UINT            appBarCallbackMsg_ = 0;
    UINT            progressRelayMsg_  = 0;
    UINT            statusUpdateMsg_   = 0;  // "WinzooStatusUpdate" from the status poller thread
    DWORD           menuLastClosedTick_= 0;
    int             hoveredTrayIdx_ = -1;
    int             trayDragStart_  = -1;  // index pressed
    RECT            clockRect_      = {};
    RECT            statusZoneRect_ = {};
    RECT            volIconRect_    = {};
    RECT            netIconRect_    = {};
    RECT            batIconRect_    = {};
    RECT            langIconRect_   = {};
    RECT            scrollLeftRect_ = {};
    RECT            scrollRightRect_= {};
    RECT            startBtnRect_   = {};
    RECT            trayZoneRect_   = {};

    // 1-byte aligned bools last
    bool            isPrimary_      = true;
    bool            showStartButton_= true;
    bool            hoveredStart_   = false;
    bool            scrollNeeded_   = false;
    bool            menuOpen_       = false;
    bool            trayDragging_   = false;
    bool            shutdownPending_= false;
    bool            trayPushActive_ = false;  // first WinzooTray push received → stop scraping
    // True between WTS_SESSION_LOCK and WTS_SESSION_UNLOCK. While locked, the default
    // desktop's windows are cloaked/hidden by the system, so they fail ShouldTrack() and
    // the reconcile sweep would cull every button. We freeze the sweep while locked and
    // re-seed on unlock so the buttons survive a lock/sleep cycle.
    bool            sessionLocked_  = false;

    // Generation counters to discard stale background-thread results
    unsigned        scanGen_        = 0;
    unsigned        iconGen_        = 0;

    static constexpr UINT_PTR kTimerActiveWindow = 1;
    static constexpr UINT_PTR kTimerAppScanFirst = 2;
    static constexpr UINT_PTR kTimerAppScan      = 3;
    static constexpr UINT_PTR kTimerStatus       = 4;
    static constexpr UINT_PTR kTimerTray         = 5;
    static constexpr UINT_PTR kTimerTrayFirst    = 6;  // one-shot: first tray scrape off the WM_CREATE path
    static constexpr UINT     kTimerIntervalMs   = 250;
    static constexpr UINT     kTimerAppScanMs    = 30000;
    static constexpr UINT     kTimerStatusMs     = 1000;
    static constexpr UINT     kTimerTrayMs       = 2000;

    // Custom WM_APP messages posted by background threads
    static constexpr UINT WM_APP_SCAN_DONE  = WM_APP + 1;
    static constexpr UINT WM_APP_ICONS_DONE = WM_APP + 2;
    static constexpr UINT WM_APP_SHOW_MENU  = WM_APP + 3;
    static constexpr UINT WM_APP_WIN_ICON   = WM_APP + 4;  // IconCache worker → resolved window icon
};
