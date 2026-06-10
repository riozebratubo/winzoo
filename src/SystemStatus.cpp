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

static std::mutex       g_statusMx;
static SystemStatusData g_latestStatus;
static bool             g_statusValid = false;
static HANDLE           g_statusStop  = nullptr;   // manual-reset stop event
static std::thread      g_statusThread;

bool TryGetLatestSystemStatus(SystemStatusData& out)
{
    std::lock_guard<std::mutex> lock(g_statusMx);
    if (!g_statusValid) return false;
    out = g_latestStatus;
    return true;
}

void StartSystemStatusPoller()
{
    if (g_statusThread.joinable()) return;

    g_statusStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_statusStop) return;

    g_statusThread = std::thread([]() {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const UINT updateMsg = RegisterWindowMessageW(L"WinzooStatusUpdate");
        do {
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
        } while (WaitForSingleObject(g_statusStop, 1000) == WAIT_TIMEOUT);
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
}
