#pragma once
#include <windows.h>

// Snapshot of system status indicators polled by the background poller thread.
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

// Background status poller. PollSystemStatus() activates three out-of-process
// COM/RPC services (Network List Manager, WLAN, audio endpoint); each can block
// for hundreds of ms when those services are cold at logon, so it must never run
// on the UI thread. The poller polls once a second on its own MTA thread, stores
// the snapshot, and posts the registered "WinzooStatusUpdate" message to every
// WinzooTaskbar window after each poll.
void StartSystemStatusPoller();
void StopSystemStatusPoller();

// Copies the poller's most recent snapshot into `out`. Returns false (leaving
// `out` untouched) until the first poll has completed.
bool TryGetLatestSystemStatus(SystemStatusData& out);
