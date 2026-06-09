#include "SystemTools.h"
#include "LaunchHelper.h"
#include <shldisp.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <thread>

// Parsing name of the "All Tasks" / God-Mode shell folder. Enumerating it yields
// the same classic tools the Start menu surfaces (Device Manager, Task Manager,
// Disk Management, every Control Panel task, …).
static const wchar_t kGodModePath[] =
    L"shell:::{ED7BA470-8E54-465E-825C-99712043E01C}";

// ─────────────────────────────────────────────────────────────────────────────
// Curated fallback table.
// Commands are single tokens that ShellExecute "open" resolves directly: .msc
// snap-ins and .cpl applets live in System32 (on the default search path), and
// the bare exe names resolve via App Paths / PATH.
// ─────────────────────────────────────────────────────────────────────────────
const SystemToolDef kSystemTools[] = {
    { L"Device Manager",            L"devmgmt.msc",   L"device manager hardware drivers peripherals" },
    { L"Disk Management",           L"diskmgmt.msc",  L"disk management partition format volume drive" },
    { L"Computer Management",       L"compmgmt.msc",  L"computer management console" },
    { L"Services",                  L"services.msc",  L"services background daemons" },
    { L"Event Viewer",              L"eventvwr.msc",  L"event viewer logs" },
    { L"Task Scheduler",            L"taskschd.msc",  L"task scheduler scheduled jobs" },
    { L"Performance Monitor",       L"perfmon.msc",   L"performance monitor counters" },
    { L"Group Policy Editor",       L"gpedit.msc",    L"group policy editor gpedit local" },
    { L"Local Users and Groups",    L"lusrmgr.msc",   L"local users groups accounts" },
    { L"Local Security Policy",     L"secpol.msc",    L"local security policy" },
    { L"Windows Defender Firewall (Advanced)", L"wf.msc", L"firewall advanced security inbound outbound rules" },
    { L"Certificate Manager",       L"certmgr.msc",   L"certificate manager certificates" },
    { L"Shared Folders",            L"fsmgmt.msc",    L"shared folders shares" },
    { L"Programs and Features",     L"appwiz.cpl",    L"programs features uninstall add remove software" },
    { L"Network Connections",       L"ncpa.cpl",      L"network connections adapter ethernet wifi" },
    { L"System Properties",         L"sysdm.cpl",     L"system properties advanced environment variables performance" },
    { L"Power Options",             L"powercfg.cpl",  L"power options plan battery sleep" },
    { L"Sound",                     L"mmsys.cpl",     L"sound audio playback recording devices" },
    { L"Mouse Properties",          L"main.cpl",      L"mouse pointer buttons cursor" },
    { L"Date and Time",             L"timedate.cpl",  L"date time clock timezone" },
    { L"Region",                    L"intl.cpl",      L"region locale format language" },
    { L"Internet Options",          L"inetcpl.cpl",   L"internet options proxy connections" },
    { L"Display (legacy)",          L"desk.cpl",      L"display resolution screen" },
    { L"User Accounts",             L"netplwiz",      L"user accounts netplwiz autologon" },
    { L"Registry Editor",           L"regedit",       L"registry editor regedit" },
    { L"System Configuration",      L"msconfig",      L"system configuration msconfig boot startup" },
    { L"System Information",        L"msinfo32",      L"system information specs msinfo" },
    { L"Task Manager",              L"taskmgr",       L"task manager processes performance" },
    { L"Resource Monitor",          L"resmon",        L"resource monitor cpu memory disk network" },
    { L"DirectX Diagnostic Tool",   L"dxdiag",        L"directx diagnostic dxdiag gpu" },
    { L"Disk Cleanup",              L"cleanmgr",      L"disk cleanup free space temporary files" },
    { L"Command Prompt",            L"cmd.exe",       L"command prompt cmd console terminal" },
    { L"Windows PowerShell",        L"powershell.exe",L"powershell shell terminal" },
    { L"Control Panel",             L"control.exe",   L"control panel" },
    { L"On-Screen Keyboard",        L"osk",           L"on screen keyboard osk accessibility" },
    { L"Character Map",             L"charmap",       L"character map unicode symbols glyphs" },
    { L"Remote Desktop Connection", L"mstsc",         L"remote desktop connection rdp mstsc" },
    { L"Snipping Tool",             L"snippingtool",  L"snipping tool screenshot capture" },
};
const int kSystemToolsCount = static_cast<int>(_countof(kSystemTools));

// ─────────────────────────────────────────────────────────────────────────────
// Matching helpers (mirror the app-menu's own substring / subsequence matching).
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;
    for (auto& ch : r) ch = towlower(ch);
    return r;
}

static bool FuzzySubseq(const std::wstring& lowerHay, const std::wstring& lowerNeedle)
{
    size_t qi = 0;
    for (size_t i = 0; i < lowerHay.size() && qi < lowerNeedle.size(); ++i)
        if (lowerHay[i] == lowerNeedle[qi]) ++qi;
    return qi == lowerNeedle.size();
}

// Relevance score for a candidate; higher is better, <0 means no match. Ordering
// the hits by this turns enumeration-order noise (e.g. a fuzzy hit landing above
// "Disk Management") into a sensible best-match-first list.
static int MatchScore(const std::wstring& lowerName, const std::wstring& lowerKeywords,
                      const std::wstring& lowerQuery, bool fuzzy)
{
    if (lowerName == lowerQuery)            return 100;                 // exact
    size_t pos = lowerName.find(lowerQuery);
    if (pos == 0)                           return 90;                  // name prefix
    if (pos != std::wstring::npos)          return 70;                  // name substring
    if (!lowerKeywords.empty() && lowerKeywords.find(lowerQuery) != std::wstring::npos)
                                            return 50;                  // keyword substring
    if (fuzzy && FuzzySubseq(lowerName, lowerQuery)) return 20;         // fuzzy subsequence
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic enumeration of the God-Mode folder, performed once on a background
// thread (see PrewarmSystemTools) so the UI thread never blocks on ~200 COM
// round-trips. Each entry caches both the display name and its lowercase form,
// so per-keystroke filtering does no allocation.
//
// g_enumState sequences the hand-off:
//   0 = idle (not started, or a prior attempt failed → retry allowed)
//   1 = a background enumeration is in progress
//   2 = enumeration finished; g_godMode is published and immutable
// A reader that observes state 2 is guaranteed (release/acquire ordering on the
// atomic) to see the fully-written g_godMode, so it needs no lock.
// ─────────────────────────────────────────────────────────────────────────────
struct GodModeEntry { std::wstring name; std::wstring lower; };
static std::vector<GodModeEntry> g_godMode;        // written once, then read-only
static std::atomic<int>          g_enumState{0};

// Read-only enumeration via an in-process Shell.Application — sufficient and
// avoids depending on Explorer being reachable. (Launching, which must
// de-elevate, uses Explorer's dispatch instead — see LaunchSystemToolByName.)
// Returns false if the shell automation calls themselves failed (so the caller
// can allow a later retry); an empty-but-successful enumeration returns true.
static bool EnumerateGodMode(std::vector<GodModeEntry>& out)
{
    IShellDispatch* pShell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pShell))))
        return false;

    bool ok = false;
    VARIANT vDir; vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(kGodModePath);
    Folder* pFolder = nullptr;
    if (vDir.bstrVal && SUCCEEDED(pShell->NameSpace(vDir, &pFolder)) && pFolder) {
        FolderItems* pItems = nullptr;
        if (SUCCEEDED(pFolder->Items(&pItems)) && pItems) {
            ok = true;  // shell namespace reachable; whatever count we get is authoritative
            long count = 0;
            pItems->get_Count(&count);
            for (long i = 0; i < count; ++i) {
                VARIANT vi; vi.vt = VT_I4; vi.lVal = i;
                FolderItem* pItem = nullptr;
                if (SUCCEEDED(pItems->Item(vi, &pItem)) && pItem) {
                    BSTR bName = nullptr;
                    if (SUCCEEDED(pItem->get_Name(&bName)) && bName) {
                        std::wstring name(bName, SysStringLen(bName));
                        out.push_back({ name, ToLower(name) });
                        SysFreeString(bName);
                    }
                    pItem->Release();
                }
            }
            pItems->Release();
        }
        pFolder->Release();
    }
    VariantClear(&vDir);
    pShell->Release();
    return ok;
}

void PrewarmSystemTools()
{
    int expected = 0;
    if (!g_enumState.compare_exchange_strong(expected, 1))
        return;  // already running, or already done — nothing to do

    std::thread([] {
        // Own COM init for this worker thread; MTA is fine for read-only shell
        // automation and doesn't disturb the UI thread's apartment.
        bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        std::vector<GodModeEntry> names;
        bool ok = EnumerateGodMode(names);
        if (comInit) CoUninitialize();

        if (ok) {
            g_godMode = std::move(names);
            g_enumState.store(2);  // publish (release): readers seeing 2 see g_godMode
        } else {
            g_enumState.store(0);  // failed — leave cache empty, allow a later retry
        }
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
namespace {
struct ScoredHit { int score; SystemToolHit hit; };

std::vector<SystemToolHit> TakeTop(std::vector<ScoredHit>& scored, int maxResults)
{
    // Best match first; ties broken by shorter (i.e. more specific) name.
    std::stable_sort(scored.begin(), scored.end(),
        [](const ScoredHit& a, const ScoredHit& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.hit.name.size() < b.hit.name.size();
        });
    std::vector<SystemToolHit> hits;
    for (auto& s : scored) {
        if (static_cast<int>(hits.size()) >= maxResults) break;
        hits.push_back(std::move(s.hit));
    }
    return hits;
}
} // namespace

std::vector<SystemToolHit> SearchSystemTools(const std::wstring& lowerQuery,
                                             bool dynamic, bool fuzzy, int maxResults)
{
    if (lowerQuery.empty()) return {};

    if (dynamic) {
        PrewarmSystemTools();  // idempotent; first call starts the background warm
        if (g_enumState.load() == 2 && !g_godMode.empty()) {
            std::vector<ScoredHit> scored;
            for (const auto& e : g_godMode) {
                int s = MatchScore(e.lower, L"", lowerQuery, fuzzy);
                if (s >= 0) scored.push_back({ s, { e.name, L"", /*shellItem=*/true } });
            }
            return TakeTop(scored, maxResults);
        }
        // Cache still warming or enumeration failed — fall through to curated.
    }

    std::vector<ScoredHit> scored;
    for (int i = 0; i < kSystemToolsCount; ++i) {
        const SystemToolDef& t = kSystemTools[i];
        int s = MatchScore(ToLower(t.displayName), t.keywords, lowerQuery, fuzzy);
        if (s >= 0) scored.push_back({ s, { t.displayName, t.command, /*shellItem=*/false } });
    }
    return TakeTop(scored, maxResults);
}

// ─────────────────────────────────────────────────────────────────────────────
bool LaunchSystemToolByName(const std::wstring& name)
{
    // Prefer Explorer's dispatch so the tool launches at the user's integrity
    // level; fall back to an in-process Shell.Application (launches elevated when
    // winzoo is elevated, but still works).
    IShellDispatch2* psd = GetExplorerShellDispatch();
    if (!psd) {
        IShellDispatch* p = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&p))) && p) {
            p->QueryInterface(IID_PPV_ARGS(&psd));
            p->Release();
        }
    }
    if (!psd) return false;

    bool launched = false;
    VARIANT vDir; vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(kGodModePath);
    Folder* pFolder = nullptr;
    if (vDir.bstrVal && SUCCEEDED(psd->NameSpace(vDir, &pFolder)) && pFolder) {
        FolderItems* pItems = nullptr;
        if (SUCCEEDED(pFolder->Items(&pItems)) && pItems) {
            long count = 0;
            pItems->get_Count(&count);
            for (long i = 0; i < count && !launched; ++i) {
                VARIANT vi; vi.vt = VT_I4; vi.lVal = i;
                FolderItem* pItem = nullptr;
                if (SUCCEEDED(pItems->Item(vi, &pItem)) && pItem) {
                    BSTR bName = nullptr;
                    if (SUCCEEDED(pItem->get_Name(&bName)) && bName) {
                        // Case-insensitive: the cached display name and this
                        // enumeration both come from the same shell namespace,
                        // but compare leniently to tolerate any casing drift.
                        if (_wcsicmp(name.c_str(),
                                     std::wstring(bName, SysStringLen(bName)).c_str()) == 0) {
                            VARIANT vVerb; VariantInit(&vVerb);  // default verb
                            if (SUCCEEDED(pItem->InvokeVerb(vVerb))) launched = true;
                            VariantClear(&vVerb);
                        }
                        SysFreeString(bName);
                    }
                    pItem->Release();
                }
            }
            pItems->Release();
        }
        pFolder->Release();
    }
    VariantClear(&vDir);
    psd->Release();
    return launched;
}
