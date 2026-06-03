#pragma once
#include <windows.h>
#include <unknwn.h>
#include <initguid.h>

// Undocumented NOTIFYITEM structure — one entry per Shell_NotifyIcon registration.
// Layout verified against Windows 10 21H2 and Windows 11 22H2 Shell32.dll.
struct NOTIFYITEM {
    PWSTR  pszExeName;   // basename of owner process, e.g. L"Taskmgr.exe"
    PWSTR  pszTip;       // tooltip text
    HICON  hIcon;        // current icon (owned by Explorer; callers must CopyIcon)
    HWND   hWnd;         // callback window == NOTIFYICONDATA::hWnd
    DWORD  dwPreference; // 0=auto, 1=always show, 2=always hide
    UINT   uID;          // == NOTIFYICONDATA::uID
    GUID   guidItem;     // unique per-registration GUID (Vista+)
};

// {D133F648-CF7F-4E4D-BF2C-E03C77F37E29}
MIDL_INTERFACE("D133F648-CF7F-4E4D-BF2C-E03C77F37E29")
INotificationCB : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Notify(ULONG_PTR hItem,
                                             NOTIFYITEM* pItem) = 0;
};

// {D782CCBA-AFB0-43F1-94DB-FDA3779EACCB}
MIDL_INTERFACE("D782CCBA-AFB0-43F1-94DB-FDA3779EACCB")
ITrayNotify : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* pCB,
                                                       ULONG_PTR* pHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONG_PTR* pHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE Refresh() = 0;
};

// Explorer registers CLSID_TrayNotify as a local (out-of-process) COM server.
// {56FDF344-FD6D-11D0-958A-006097C9A090}
DEFINE_GUID(CLSID_TrayNotify,
    0x56FDF344, 0xFD6D, 0x11D0,
    0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90);
