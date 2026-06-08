#pragma once
#include <windows.h>

// Snapshot of system status indicators polled each timer tick.
// PollSystemStatus() is safe to call from the main STA thread after
// CoInitializeEx has been called.
struct SystemStatusData {
    // Volume
    bool  volAvailable = false;
    float volLevel     = 0.0f;  // 0.0–1.0
    bool  volMuted     = false;

    // Network
    bool netAvailable  = false; // indicator should be shown at all
    bool netLink       = false; // a network link is present (wired up or Wi-Fi associated)
    bool netInternet   = false; // has IPv4/IPv6 internet connectivity (not just local)
    bool netWired      = false; // active link is wired ethernet (vs Wi-Fi)
    int  wifiSignal    = -1;    // Wi-Fi signal quality 0–100; -1 when not on Wi-Fi

    // Battery (absent on desktops)
    bool batAvailable  = false;
    bool batOnAC       = false; // plugged in to AC power
    bool batCharging   = false;
    int  batPercent    = 0;     // 0–100
};

SystemStatusData PollSystemStatus();
