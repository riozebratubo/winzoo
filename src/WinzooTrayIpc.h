#pragma once
// Shared wire format for the tray-icon push channel between winzoo_com.dll
// (injected into Explorer) and winzoo.exe.
//
// The injected DLL observes Shell_NotifyIcon traffic on Explorer's Shell_TrayWnd
// thread via a WH_CALLWNDPROC hook (WM_COPYDATA, dwData == 1, the SHELLTRAYDATA
// blob), converts each icon to BGRA pixels, and relays one WinzooTrayRecord per
// add/modify/delete to every WinzooTaskbar window with WM_COPYDATA.
//
// Keep this a flat POD: it is memcpy'd into a single WM_COPYDATA buffer as
// [WinzooTrayRecord][tooltip wchars (no null)][icon BGRA, top-down w*h*4].

#include <windows.h>

// COPYDATASTRUCT.dwData magic identifying a winzoo tray record. ('WZTR')
constexpr ULONG_PTR kWinzooTrayMagic = 0x575A5452;

// NIF/NIS constants used by both sides (avoids pulling in shellapi.h everywhere).
constexpr UINT kNIF_ICON   = 0x00000002;
constexpr UINT kNIF_STATE  = 0x00000008;
constexpr UINT kNIS_HIDDEN = 0x00000001;

#pragma pack(push, 4)
// Minimum record size for backward compat: receivers should accept records at
// least this large (the original layout without dwState fields).
constexpr size_t kWinzooTrayRecordMinSize = 64;

struct WinzooTrayRecord {
    DWORD  dwMessage;     // NIM_ADD / NIM_MODIFY / NIM_DELETE / NIM_SETVERSION
    DWORD  reserved;      // == sizeof(WinzooTrayRecord) from sender; 0 for old senders
    UINT64 ownerHwnd;     // NOTIFYICONDATA.hWnd (owner), 64-bit for x64
    UINT   uID;
    UINT   uCallbackMsg;  // 0 if NIF_MESSAGE not set
    UINT   uVersion;      // best-effort (from NIM_SETVERSION)
    UINT   uFlags;        // raw NOTIFYICONDATA.uFlags
    GUID   guidItem;      // valid only if uFlags & NIF_GUID
    INT    iconW;         // 0 if no icon in this record
    INT    iconH;
    UINT   cbTooltip;     // bytes of tooltip text that follow (no null terminator)
    UINT   cbIconBits;    // bytes of BGRA that follow (== iconW*iconH*4, or 0)
    // --- v2 fields (appended for backward compat) ---
    DWORD  dwState;       // NOTIFYICONDATA.dwState (NIS_HIDDEN etc.), 0 if unknown
    DWORD  dwStateMask;   // which bits of dwState are valid
    // followed by: wchar_t tooltip[cbTooltip/2], then BYTE bgra[cbIconBits]
};
#pragma pack(pop)

static_assert(kWinzooTrayRecordMinSize == 64, "old header size mismatch");
static_assert(sizeof(WinzooTrayRecord) == 72, "struct grew unexpectedly");
