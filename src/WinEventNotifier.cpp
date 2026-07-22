#include "WinEventNotifier.h"
#include <vector>

namespace {

struct Sink {
    void*                       key;
    WinEventNotifier::Callback  cb;
};

std::vector<Sink>          g_sinks;   // UI-thread access only
std::vector<HWINEVENTHOOK> g_hooks;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD)
{
    // Only whole top-level windows. This filter must stay dirt-cheap:
    // SHOW/HIDE/DESTROY/NAMECHANGE fire for every object system-wide.
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    // A destroyed window can't be queried; forward it as-is and let sinks
    // match it against their tracked handles.
    if (event != EVENT_OBJECT_DESTROY && GetAncestor(hwnd, GA_ROOT) != hwnd)
        return;

    WinEventInfo info{ event, hwnd };
    for (const auto& s : g_sinks)
        s.cb(info);
}

void InstallHooks()
{
    auto add = [](DWORD lo, DWORD hi) {
        HWINEVENTHOOK h = SetWinEventHook(lo, hi, nullptr, WinEventProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) g_hooks.push_back(h);
    };
    add(EVENT_SYSTEM_FOREGROUND,    EVENT_SYSTEM_FOREGROUND);
    add(EVENT_SYSTEM_MOVESIZEEND,   EVENT_SYSTEM_MOVESIZEEND);
    add(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    add(EVENT_OBJECT_DESTROY,       EVENT_OBJECT_HIDE);        // DESTROY, SHOW, HIDE
    add(EVENT_OBJECT_NAMECHANGE,    EVENT_OBJECT_NAMECHANGE);
    add(EVENT_OBJECT_CLOAKED,       EVENT_OBJECT_UNCLOAKED);
}

void RemoveHooks()
{
    for (HWINEVENTHOOK h : g_hooks)
        UnhookWinEvent(h);
    g_hooks.clear();
}

} // namespace

void WinEventNotifier::Register(void* key, Callback cb)
{
    Unregister(key);
    g_sinks.push_back({ key, std::move(cb) });
    if (g_hooks.empty())
        InstallHooks();
}

void WinEventNotifier::Unregister(void* key)
{
    for (auto it = g_sinks.begin(); it != g_sinks.end(); ++it) {
        if (it->key == key) {
            g_sinks.erase(it);
            break;
        }
    }
    if (g_sinks.empty())
        RemoveHooks();
}
