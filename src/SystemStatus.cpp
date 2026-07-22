#include "SystemStatus.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wlanapi.h>
// Define COM GUIDs for INetworkListManager inline so no extra lib is needed.
#define INITGUID
#include <initguid.h>
#include <netlistmgr.h>
#include <mutex>
#include <thread>
#include <atomic>

SystemStatusData PollSystemStatus()
{
    SystemStatusData s;

    // ── Battery ──────────────────────────────────────────────────────────────
    {
        SYSTEM_POWER_STATUS sps = {};
        if (GetSystemPowerStatus(&sps)) {
            // BatteryFlag 0x80 = no system battery, 0xFF = unknown
            const bool noBattery = (sps.BatteryFlag & 0x80) || (sps.BatteryFlag == 0xFF);
            if (!noBattery) {
                s.batAvailable = true;
                s.batOnAC      = (sps.ACLineStatus == 1);
                s.batCharging  = (sps.BatteryFlag & 0x08) != 0;
                s.batPercent   = (sps.BatteryLifePercent != 0xFF)
                               ? static_cast<int>(sps.BatteryLifePercent) : 50;
            }
        }
    }

    // ── Network (INetworkListManager via COM) ─────────────────────────────────
    {
        INetworkListManager* pNLM = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr,
                                       CLSCTX_ALL, IID_INetworkListManager,
                                       reinterpret_cast<void**>(&pNLM)))) {
            NLM_CONNECTIVITY conn = NLM_CONNECTIVITY_DISCONNECTED;
            if (SUCCEEDED(pNLM->GetConnectivity(&conn))) {
                s.netAvailable = true;
                s.netInternet  = (conn & (NLM_CONNECTIVITY_IPV4_INTERNET |
                                          NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
                s.netLink      = (conn != NLM_CONNECTIVITY_DISCONNECTED);
            }
            pNLM->Release();
        }
    }

    // ── Wi-Fi signal strength (Native WiFi / WLAN API) ────────────────────────
    // Whether the active connection is wireless is decided from the interface
    // *state* (enumerating interfaces and their state needs no special rights).
    // The actual signal quality comes from WlanQueryInterface, which is gated
    // behind the Windows "Location" privacy permission — when the user has
    // denied it that call fails, so we still know it's Wi-Fi (correct glyph)
    // but leave wifiSignal at -1 and the renderer falls back to "connected".
    {
        bool   wifiConnected = false;
        HANDLE hWlan = nullptr;
        DWORD  negotiatedVer = 0;
        if (WlanOpenHandle(2, nullptr, &negotiatedVer, &hWlan) == ERROR_SUCCESS) {
            WLAN_INTERFACE_INFO_LIST* pIfList = nullptr;
            if (WlanEnumInterfaces(hWlan, nullptr, &pIfList) == ERROR_SUCCESS && pIfList) {
                for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i) {
                    const WLAN_INTERFACE_INFO& info = pIfList->InterfaceInfo[i];
                    if (info.isState != wlan_interface_state_connected) continue;

                    // Link is wireless regardless of whether we can read details.
                    wifiConnected = true;
                    s.netLink     = true;   // associated even if NLM lags

                    WLAN_CONNECTION_ATTRIBUTES* pAttr = nullptr;
                    DWORD attrSize = 0;
                    if (WlanQueryInterface(hWlan, &info.InterfaceGuid,
                                           wlan_intf_opcode_current_connection,
                                           nullptr, &attrSize,
                                           reinterpret_cast<void**>(&pAttr),
                                           nullptr) == ERROR_SUCCESS && pAttr) {
                        s.wifiSignal = static_cast<int>(
                            pAttr->wlanAssociationAttributes.wlanSignalQuality);
                        WlanFreeMemory(pAttr);
                    }
                    break;
                }
                WlanFreeMemory(pIfList);
            }
            WlanCloseHandle(hWlan, nullptr);
        }

        // Wired if there's a link but no associated wireless interface.
        s.netWired = s.netLink && !wifiConnected;
    }

    // ── Volume (IAudioEndpointVolume via MMDeviceAPI) ─────────────────────────
    {
        IMMDeviceEnumerator* pEnum = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&pEnum)))) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev))) {
                IAudioEndpointVolume* pVol = nullptr;
                if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume),
                                              CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(&pVol)))) {
                    BOOL  muted = FALSE;
                    float level = 0.0f;
                    if (SUCCEEDED(pVol->GetMute(&muted)) &&
                        SUCCEEDED(pVol->GetMasterVolumeLevelScalar(&level))) {
                        s.volAvailable = true;
                        s.volMuted     = (muted != FALSE);
                        s.volLevel     = level;
                    }
                    pVol->Release();
                }
                pDev->Release();
            }
            pEnum->Release();
        }
    }

    return s;
}

// ── Background poller ────────────────────────────────────────────────────────
//
// Event-driven: instead of re-polling every second, the thread sleeps until one
// of the push sources below signals g_statusPoke (or the 60s safety re-poll
// fires), then takes one fresh snapshot and posts WinzooStatusUpdate:
//   volume  — IAudioEndpointVolumeCallback on the default render endpoint
//             (+ IMMNotificationClient to re-bind when the default changes)
//   network — INetworkListManagerEvents::ConnectivityChanged connection point
//   Wi-Fi   — WlanRegisterNotification (ACM connect/disconnect + MSM signal)
//   battery — the taskbar forwards WM_POWERBROADCAST via
//             RequestSystemStatusRefresh()
// All callbacks arrive on RPC/threadpool threads and only signal the event.
// Every subscription is best-effort; a source that fails to subscribe is still
// covered by the safety re-poll.

static std::mutex        g_statusMx;
static SystemStatusData  g_latestStatus;
static bool              g_statusValid = false;
static HANDLE            g_statusStop  = nullptr;   // manual-reset stop event
static HANDLE            g_statusPoke  = nullptr;   // auto-reset "re-poll now"
static std::atomic<bool> g_audioDeviceChanged{ false };
static std::thread       g_statusThread;

bool TryGetLatestSystemStatus(SystemStatusData& out)
{
    std::lock_guard<std::mutex> lock(g_statusMx);
    if (!g_statusValid) return false;
    out = g_latestStatus;
    return true;
}

void RequestSystemStatusRefresh()
{
    if (g_statusPoke) SetEvent(g_statusPoke);
}

namespace {

// The callback objects are statically allocated and outlive every subscription
// (unregistered before the thread exits), so COM refcounting is a formality.

class VolumeCallback : public IAudioEndpointVolumeCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        RequestSystemStatusRefresh();
        return S_OK;
    }
};

class DeviceCallback : public IMMNotificationClient {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_audioDeviceChanged.store(true, std::memory_order_relaxed);
            RequestSystemStatusRefresh();
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override            { return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override                          { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override                        { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

class NetEvents : public INetworkListManagerEvents {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_INetworkListManagerEvents) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override  { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP ConnectivityChanged(NLM_CONNECTIVITY) override {
        RequestSystemStatusRefresh();
        return S_OK;
    }
};

VolumeCallback g_volCb;
DeviceCallback g_devCb;
NetEvents      g_netCb;

void CALLBACK WlanNotifyCb(PWLAN_NOTIFICATION_DATA data, PVOID)
{
    if (!data) return;
    if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM) {
        switch (data->NotificationCode) {
        case wlan_notification_acm_connection_complete:
        case wlan_notification_acm_disconnected:
        case wlan_notification_acm_interface_arrival:
        case wlan_notification_acm_interface_removal:
            RequestSystemStatusRefresh();
            break;
        default:
            break;
        }
    } else if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_MSM) {
        if (data->NotificationCode == wlan_notification_msm_signal_quality_change)
            RequestSystemStatusRefresh();
    }
}

// (Re)bind the volume-change callback to the current default render endpoint.
// Called at startup and whenever IMMNotificationClient reports a new default.
void BindVolumeCallback(IMMDeviceEnumerator* pEnum, IAudioEndpointVolume** pVolInOut)
{
    if (*pVolInOut) {
        (*pVolInOut)->UnregisterControlChangeNotify(&g_volCb);
        (*pVolInOut)->Release();
        *pVolInOut = nullptr;
    }
    if (!pEnum) return;
    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
        return;
    IAudioEndpointVolume* pVol = nullptr;
    if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&pVol)))) {
        if (SUCCEEDED(pVol->RegisterControlChangeNotify(&g_volCb)))
            *pVolInOut = pVol;
        else
            pVol->Release();
    }
    pDev->Release();
}

} // namespace

void StartSystemStatusPoller()
{
    if (g_statusThread.joinable()) return;

    g_statusStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_statusPoke = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_statusStop || !g_statusPoke) {
        if (g_statusStop) { CloseHandle(g_statusStop); g_statusStop = nullptr; }
        if (g_statusPoke) { CloseHandle(g_statusPoke); g_statusPoke = nullptr; }
        return;
    }

    g_statusThread = std::thread([]() {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const UINT updateMsg = RegisterWindowMessageW(L"WinzooStatusUpdate");

        // --- Subscribe to the push sources ---
        IMMDeviceEnumerator*  pEnum = nullptr;
        IAudioEndpointVolume* pVol  = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&pEnum)))) {
            pEnum->RegisterEndpointNotificationCallback(&g_devCb);
            BindVolumeCallback(pEnum, &pVol);
        }

        INetworkListManager* pNLM      = nullptr;
        IConnectionPoint*    pNetCP    = nullptr;
        DWORD                netCookie = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                       IID_INetworkListManager,
                                       reinterpret_cast<void**>(&pNLM)))) {
            IConnectionPointContainer* pCPC = nullptr;
            if (SUCCEEDED(pNLM->QueryInterface(IID_IConnectionPointContainer,
                                               reinterpret_cast<void**>(&pCPC)))) {
                if (SUCCEEDED(pCPC->FindConnectionPoint(IID_INetworkListManagerEvents,
                                                        &pNetCP))) {
                    if (FAILED(pNetCP->Advise(&g_netCb, &netCookie))) {
                        pNetCP->Release();
                        pNetCP    = nullptr;
                        netCookie = 0;
                    }
                }
                pCPC->Release();
            }
        }

        HANDLE hWlan   = nullptr;
        DWORD  wlanVer = 0;
        if (WlanOpenHandle(2, nullptr, &wlanVer, &hWlan) == ERROR_SUCCESS) {
            // Failure is fine — the safety re-poll still tracks signal changes.
            WlanRegisterNotification(hWlan,
                WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM,
                TRUE, WlanNotifyCb, nullptr, nullptr, nullptr);
        }

        const HANDLE waits[2] = { g_statusStop, g_statusPoke };
        for (;;) {
            if (g_audioDeviceChanged.exchange(false, std::memory_order_relaxed))
                BindVolumeCallback(pEnum, &pVol);

            SystemStatusData fresh = PollSystemStatus();
            {
                std::lock_guard<std::mutex> lock(g_statusMx);
                g_latestStatus = fresh;
                g_statusValid  = true;
            }
            if (updateMsg) {
                for (HWND h = FindWindowExW(nullptr, nullptr, L"WinzooTaskbar", nullptr); h;
                     h = FindWindowExW(nullptr, h, L"WinzooTaskbar", nullptr))
                    PostMessageW(h, updateMsg, 0, 0);
            }

            DWORD r = WaitForMultipleObjects(2, waits, FALSE, 60000);
            if (r == WAIT_OBJECT_0)
                break;                       // stop
            if (r == WAIT_OBJECT_0 + 1) {
                // Poked. Coalesce the burst (a volume-slider drag fires many
                // notifications per second) before taking one snapshot.
                if (WaitForSingleObject(g_statusStop, 120) != WAIT_TIMEOUT)
                    break;
            }
            // else: 60s safety re-poll for anything a subscription missed
        }

        // --- Teardown ---
        if (hWlan) WlanCloseHandle(hWlan, nullptr);   // also unregisters the callback
        if (pNetCP) { pNetCP->Unadvise(netCookie); pNetCP->Release(); }
        if (pNLM) pNLM->Release();
        if (pVol) { pVol->UnregisterControlChangeNotify(&g_volCb); pVol->Release(); }
        if (pEnum) { pEnum->UnregisterEndpointNotificationCallback(&g_devCb); pEnum->Release(); }
        if (SUCCEEDED(hrCom)) CoUninitialize();
    });
}

void StopSystemStatusPoller()
{
    if (!g_statusThread.joinable()) return;
    SetEvent(g_statusStop);
    g_statusThread.join();
    CloseHandle(g_statusStop);
    g_statusStop = nullptr;
    CloseHandle(g_statusPoke);
    g_statusPoke = nullptr;
}
