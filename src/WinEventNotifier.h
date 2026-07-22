#pragma once
#include <windows.h>
#include <functional>

// Cloak events are absent from older SDK headers.
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED   0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

// Process-wide out-of-context WinEvent hooks (no DLL injection): window
// show/hide/destroy, cloak/uncloak (virtual desktops, UWP), title changes,
// foreground, minimize, and move/size-end. This is the push replacement for
// the old fixed 250ms EnumWindows reconcile sweep. Callbacks are delivered on
// the registering thread's message loop; sinks must stay cheap (dirty-mark and
// defer to a debounced timer) because these events fire for every top-level
// window system-wide. Out-of-context events can be dropped under load, so a
// slow reconcile heartbeat must remain as the safety net.
struct WinEventInfo {
    DWORD event;
    HWND  hwnd;
};

namespace WinEventNotifier {
    using Callback = std::function<void(const WinEventInfo&)>;

    // Register a sink keyed by `key` (one per TaskbarWindow). The hooks are
    // installed when the first sink registers. UI-thread only: hooks deliver
    // via the installing thread's message loop, and the sink list is unlocked.
    void Register(void* key, Callback cb);

    // Remove a sink; the hooks are uninstalled when the last sink leaves.
    void Unregister(void* key);
}
