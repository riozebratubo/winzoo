#include "SystemStatus.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
// Define COM GUIDs for INetworkListManager inline so no extra lib is needed.
#define INITGUID
#include <initguid.h>
#include <netlistmgr.h>

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
                s.netConnected = (conn & (NLM_CONNECTIVITY_IPV4_INTERNET |
                                          NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
            }
            pNLM->Release();
        }
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
