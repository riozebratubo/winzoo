#include "Registry.h"
#include "Settings.h"
#include <map>

static constexpr wchar_t kRegPath[] = L"Software\\Winzoo";

RegistryKey RegistryKey::OpenAppKey(REGSAM access)
{
    RegistryKey key;
    DWORD disp = 0;
    RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr,
                    REG_OPTION_NON_VOLATILE, access, nullptr, &key.hKey_, &disp);
    return key;
}

RegistryKey::~RegistryKey()
{
    if (hKey_) { RegCloseKey(hKey_); hKey_ = nullptr; }
}

RegistryKey::RegistryKey(RegistryKey&& o) noexcept : hKey_(o.hKey_) { o.hKey_ = nullptr; }

RegistryKey& RegistryKey::operator=(RegistryKey&& o) noexcept
{
    if (this != &o) {
        if (hKey_) RegCloseKey(hKey_);
        hKey_ = o.hKey_;
        o.hKey_ = nullptr;
    }
    return *this;
}

bool RegistryKey::ReadDword(const wchar_t* name, DWORD& out) const
{
    if (!hKey_) return false;
    DWORD size = sizeof(DWORD), type = 0;
    return RegQueryValueExW(hKey_, name, nullptr, &type,
                            reinterpret_cast<BYTE*>(&out), &size) == ERROR_SUCCESS
           && type == REG_DWORD;
}

bool RegistryKey::ReadString(const wchar_t* name, std::wstring& out) const
{
    if (!hKey_) return false;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(hKey_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    if (type != REG_SZ) return false;

    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey_, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS)
        return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = std::move(buf);
    return true;
}

bool RegistryKey::WriteString(const wchar_t* name, std::wstring_view val)
{
    if (!hKey_) return false;
    return RegSetValueExW(hKey_, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(val.data()),
                          static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegistryKey::ReadMultiString(const wchar_t* name, std::vector<std::wstring>& out) const
{
    if (!hKey_) return false;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(hKey_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    if (type != REG_MULTI_SZ) return false;

    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey_, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS)
        return false;

    out.clear();
    const wchar_t* p   = buf.data();
    const wchar_t* end = buf.data() + (size / sizeof(wchar_t));
    while (p < end && *p) {
        out.emplace_back(p);
        p += out.back().size() + 1;
    }
    return true;
}

bool RegistryKey::WriteDword(const wchar_t* name, DWORD val)
{
    if (!hKey_) return false;
    return RegSetValueExW(hKey_, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS;
}

bool RegistryKey::WriteMultiString(const wchar_t* name, const std::vector<std::wstring>& vals)
{
    if (!hKey_) return false;
    std::wstring buf;
    for (const auto& s : vals) { buf += s; buf += L'\0'; }
    buf += L'\0';
    return RegSetValueExW(hKey_, name, 0, REG_MULTI_SZ,
                          reinterpret_cast<const BYTE*>(buf.data()),
                          static_cast<DWORD>(buf.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// ─── Per-monitor pinned-paths helpers ────────────────────────────────────────

static constexpr wchar_t kPerMonPrefix[]  = L"PinnedPaths_";
static constexpr size_t  kPerMonPrefixLen = 12;
// Separate namespace for per-monitor Apps-Menu pins.
static constexpr wchar_t kAppMenuPerMonPrefix[]  = L"AppMenuPins_";
static constexpr size_t  kAppMenuPerMonPrefixLen = 12;

static void LoadPerMonitorPins(HKEY hKey,
                               std::map<std::wstring, std::vector<std::wstring>>& out,
                               const wchar_t* prefix    = kPerMonPrefix,
                               size_t         prefixLen = kPerMonPrefixLen)
{
    DWORD valueCount = 0, maxNameLen = 0;
    if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &valueCount, &maxNameLen, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        return;

    std::wstring nameBuf(maxNameLen + 1, L'\0');
    for (DWORD i = 0; i < valueCount; ++i) {
        DWORD nameLen = static_cast<DWORD>(nameBuf.size());
        DWORD type    = 0;
        if (RegEnumValueW(hKey, i, nameBuf.data(), &nameLen,
                          nullptr, &type, nullptr, nullptr) != ERROR_SUCCESS)
            continue;
        if (type != REG_MULTI_SZ) continue;
        if (wcsncmp(nameBuf.data(), prefix, prefixLen) != 0) continue;

        std::wstring monName = nameBuf.data() + prefixLen;
        if (monName.empty()) continue;

        // Read data by name (avoids double-enumeration index mismatch)
        DWORD dataSize = 0;
        if (RegQueryValueExW(hKey, nameBuf.data(), nullptr, nullptr,
                             nullptr, &dataSize) != ERROR_SUCCESS || dataSize == 0) {
            out[monName] = {};
            continue;
        }
        std::wstring data(dataSize / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hKey, nameBuf.data(), nullptr, &type,
                             reinterpret_cast<BYTE*>(data.data()), &dataSize) != ERROR_SUCCESS)
            continue;

        std::vector<std::wstring> paths;
        const wchar_t* p = data.data();
        while (*p) {
            paths.emplace_back(p);
            p += paths.back().size() + 1;
        }
        out[monName] = std::move(paths);
    }
}

static void SavePerMonitorPins(HKEY hKey,
                               const std::map<std::wstring, std::vector<std::wstring>>& pins,
                               const wchar_t* prefix    = kPerMonPrefix,
                               size_t         prefixLen = kPerMonPrefixLen)
{
    // Collect and delete existing PinnedPaths_* values before writing new ones.
    DWORD valueCount = 0, maxNameLen = 0;
    if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &valueCount, &maxNameLen, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        return;

    if (valueCount > 0 && maxNameLen > 0) {
        std::wstring nameBuf(maxNameLen + 1, L'\0');
        std::vector<std::wstring> toDelete;
        for (DWORD i = 0; i < valueCount; ++i) {
            DWORD nameLen = static_cast<DWORD>(nameBuf.size());
            if (RegEnumValueW(hKey, i, nameBuf.data(), &nameLen,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                continue;
            if (wcsncmp(nameBuf.data(), prefix, prefixLen) == 0)
                toDelete.push_back(nameBuf.data());
        }
        for (const auto& name : toDelete)
            RegDeleteValueW(hKey, name.c_str());
    }

    for (const auto& [monName, paths] : pins) {
        std::wstring valueName = prefix;
        valueName += monName;
        std::wstring buf;
        for (const auto& p : paths) { buf += p; buf += L'\0'; }
        buf += L'\0';
        RegSetValueExW(hKey, valueName.c_str(), 0, REG_MULTI_SZ,
                       reinterpret_cast<const BYTE*>(buf.data()),
                       static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    }
}

Settings LoadSettings()
{
    Settings s;
    auto key = RegistryKey::OpenAppKey(KEY_READ);
    if (!key.IsOpen()) return s;

    DWORD val = 0;
    if (key.ReadDword(L"Position", val))  s.position  = static_cast<TaskbarPosition>(val);
    if (key.ReadDword(L"Theme", val) && val < static_cast<DWORD>(kThemePresetCount))
        s.theme = static_cast<ThemePreset>(val);
    if (key.ReadDword(L"Thickness", val)) s.thickness = static_cast<int>(val);
    if (key.ReadDword(L"FloatX", val))     s.floatX     = static_cast<int>(val);
    if (key.ReadDword(L"FloatY", val))     s.floatY     = static_cast<int>(val);
    if (key.ReadDword(L"FloatWidth", val)) s.floatWidth = static_cast<int>(val);

    if (key.ReadDword(L"TaskbarMonitorMode", val) && val <= 1)
        s.taskbarMonitorMode = static_cast<TaskbarMonitorMode>(val);
    if (key.ReadDword(L"TaskbarHookMethod", val) && val <= 1)
        s.taskbarHookMethod = static_cast<TaskbarHookMethod>(val);  // 2 = reserved "After Explorer" (unimplemented)
    if (key.ReadDword(L"ShowAppMenuOnAllMonitors",   val)) s.showAppMenuOnAllMonitors   = val != 0;
    if (key.ReadDword(L"ShowCurrentMonitorAppsOnly", val)) s.showCurrentMonitorAppsOnly = val != 0;

    // Clamp thickness
    if (s.thickness < 28) s.thickness = 28;
    if (s.thickness > 120) s.thickness = 120;
    if (s.floatWidth < 100) s.floatWidth = 100;

    if (key.ReadDword(L"MaxButtonWidth", val)) s.maxButtonWidth = static_cast<int>(val);
    if (key.ReadDword(L"MinButtonWidth", val)) s.minButtonWidth = static_cast<int>(val);
    if (s.maxButtonWidth < 48)  s.maxButtonWidth = 48;
    if (s.maxButtonWidth > 400) s.maxButtonWidth = 400;
    if (s.minButtonWidth < 24)  s.minButtonWidth = 24;
    if (s.minButtonWidth > s.maxButtonWidth) s.minButtonWidth = s.maxButtonWidth;
    if (key.ReadDword(L"AppButtonIconSize", val)) s.appButtonIconSize = static_cast<int>(val);
    if (s.appButtonIconSize < 16) s.appButtonIconSize = 16;
    if (s.appButtonIconSize > 64) s.appButtonIconSize = 64;

    if (key.ReadDword(L"MiddleClickClose",  val)) s.middleClickClose  = val != 0;
    if (key.ReadDword(L"ShowRightClickGap", val)) s.showRightClickGap = val != 0;
    if (key.ReadDword(L"ShowMinimizedIndicator", val)) s.showMinimizedIndicator = val != 0;
    if (key.ReadDword(L"MinimizedIndicatorType", val)) {
        if (val <= 1) s.minimizedIndicatorType = static_cast<MinimizedIndicatorType>(val);
    }
    if (key.ReadDword(L"MinimizedIndicatorW", val)) s.minimizedIndicatorW = static_cast<int>(val);
    if (key.ReadDword(L"MinimizedIndicatorH", val)) s.minimizedIndicatorH = static_cast<int>(val);
    if (s.minimizedIndicatorW < 2)  s.minimizedIndicatorW = 2;
    if (s.minimizedIndicatorW > 40) s.minimizedIndicatorW = 40;
    if (s.minimizedIndicatorH < 1)  s.minimizedIndicatorH = 1;
    if (s.minimizedIndicatorH > 20) s.minimizedIndicatorH = 20;
    if (key.ReadDword(L"MinimizedIndicatorDim", val)) s.minimizedIndicatorDim = static_cast<int>(val);
    if (s.minimizedIndicatorDim < 0)   s.minimizedIndicatorDim = 0;
    if (s.minimizedIndicatorDim > 100) s.minimizedIndicatorDim = 100;
    if (key.ReadDword(L"ShowStatusZone",    val)) s.showStatusZone    = val != 0;
    if (key.ReadDword(L"ShowLangIndicator", val)) s.showLangIndicator = val != 0;
    if (key.ReadDword(L"ShowTrayIcons",     val)) s.showTrayIcons     = val != 0;
    if (key.ReadDword(L"ShowWinzooCustomIcons", val)) s.showWinzooCustomIcons = val != 0;
    if (key.ReadDword(L"ShowOverflowTrayIcons", val)) s.showOverflowTrayIcons = val != 0;
    if (key.ReadDword(L"TrayIconFallbackExe",  val)) s.trayIconFallbackExe  = val != 0;
    if (key.ReadDword(L"TrayIconSize",      val)) s.trayIconSize      = static_cast<int>(val);
    if (s.trayIconSize    < 12) s.trayIconSize    = 12;
    if (s.trayIconSize    > 48) s.trayIconSize    = 48;
    if (key.ReadDword(L"TrayIconPadding",   val)) s.trayIconPadding   = static_cast<int>(val);
    if (s.trayIconPadding <  0) s.trayIconPadding =  0;
    if (s.trayIconPadding > 20) s.trayIconPadding = 20;
    if (key.ReadDword(L"TrayIconMargin",    val)) s.trayIconMargin    = static_cast<int>(val);
    if (s.trayIconMargin  <  0) s.trayIconMargin  =  0;
    if (s.trayIconMargin  > 20) s.trayIconMargin  = 20;
    key.ReadMultiString(L"TrayIconOrder", s.trayIconOrder);
    if (key.ReadDword(L"ShowClock",         val)) s.showClock         = val != 0;
    if (key.ReadDword(L"ClockWidth",       val)) s.clockWidth       = static_cast<int>(val);
    if (key.ReadDword(L"ClockLineSpacing", val)) s.clockLineSpacing = static_cast<int>(val);
    key.ReadString(L"ClockTimeFormat", s.clockTimeFormat);
    key.ReadString(L"ClockDateFormat", s.clockDateFormat);

    if (key.ReadDword(L"ClockTimeFontSize", val)) s.clockTimeFontSize = static_cast<int>(val);
    if (key.ReadDword(L"ClockDateFontSize", val)) s.clockDateFontSize = static_cast<int>(val);
    if (key.ReadDword(L"ClockTimeColor",    val)) s.clockTimeColor    = static_cast<COLORREF>(val);
    if (key.ReadDword(L"ClockDateColor",    val)) s.clockDateColor    = static_cast<COLORREF>(val);

    if (s.clockWidth < 40)  s.clockWidth = 40;
    if (s.clockWidth > 400) s.clockWidth = 400;

    if (s.clockLineSpacing < 0)  s.clockLineSpacing = 0;
    if (s.clockLineSpacing > 20) s.clockLineSpacing = 20;

    if (s.clockTimeFontSize < 6)  s.clockTimeFontSize = 6;
    if (s.clockTimeFontSize > 36) s.clockTimeFontSize = 36;
    if (s.clockDateFontSize < 6)  s.clockDateFontSize = 6;
    if (s.clockDateFontSize > 36) s.clockDateFontSize = 36;

    key.ReadMultiString(L"PinnedPaths", s.pinnedExePaths);
    if (key.ReadDword(L"PinnedAppsAsButtonsWhenOpen", val)) s.pinnedAppsAsButtonsWhenOpen = val != 0;
    if (key.ReadDword(L"PinnedAppsPerMonitor",        val)) s.pinnedAppsPerMonitor        = val != 0;
    LoadPerMonitorPins(key.GetHKey(), s.pinnedExePathsPerMonitor);

    key.ReadMultiString(L"AppMenuPinnedPaths", s.appMenuPinnedPaths);
    if (key.ReadDword(L"AppMenuPinnedPerMonitor", val)) s.appMenuPinnedPerMonitor = val != 0;
    LoadPerMonitorPins(key.GetHKey(), s.appMenuPinnedPathsPerMonitor,
                       kAppMenuPerMonPrefix, kAppMenuPerMonPrefixLen);

    DWORD amVal = 0;
    if (key.ReadDword(L"AppMenuLayout",       amVal)) s.appMenuLayout       = static_cast<AppMenuLayout>(amVal);
    if (key.ReadDword(L"AppMenuWidth",        amVal)) s.appMenuWidth        = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuMaxHeight",    amVal)) s.appMenuMaxHeight    = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuEntryHeight",  amVal)) s.appMenuEntryHeight  = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuGridCols",     amVal)) s.appMenuGridCols     = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuGridRows",     amVal)) s.appMenuGridRows     = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuListFontSize", amVal)) s.appMenuListFontSize = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuGridFontSize", amVal)) s.appMenuGridFontSize = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuMargin",       amVal)) s.appMenuMargin       = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuPadding",      amVal)) s.appMenuPadding      = static_cast<int>(amVal);

    // App Menu Sidebar
    if (key.ReadDword(L"AppMenuSearchEnabled",          amVal)) s.appMenuSearchEnabled          = amVal != 0;
    if (key.ReadDword(L"AppMenuSearchFuzzy",            amVal)) s.appMenuSearchFuzzy            = amVal != 0;
    if (key.ReadDword(L"AppMenuSearchSystem",           amVal)) s.appMenuSearchSystem           = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarEnabled",         amVal)) s.appMenuSidebarEnabled         = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarWidth",           amVal)) s.appMenuSidebarWidth           = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuSidebarShowExplorer",    amVal)) s.appMenuSidebarShowExplorer    = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarShowSettings",    amVal)) s.appMenuSidebarShowSettings    = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarShowPower",       amVal)) s.appMenuSidebarShowPower       = amVal != 0;
    if (key.ReadDword(L"AppMenuFlattenMode", amVal) && amVal <= 2)
        s.appMenuFlattenMode = static_cast<AppMenuFlattenMode>(amVal);

    // Classic (Vista/7) layout
    if (key.ReadDword(L"AppMenuClassicPanelWidth",      amVal)) s.appMenuClassicPanelWidth      = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuClassicShowDocuments",   amVal)) s.appMenuClassicShowDocuments   = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowPictures",    amVal)) s.appMenuClassicShowPictures    = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowMusic",       amVal)) s.appMenuClassicShowMusic       = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowDownloads",   amVal)) s.appMenuClassicShowDownloads   = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowRecentItems", amVal)) s.appMenuClassicShowRecentItems = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowThisPC",      amVal)) s.appMenuClassicShowThisPC      = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowControlPanel",amVal)) s.appMenuClassicShowControlPanel= amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowWinSettings", amVal)) s.appMenuClassicShowWinSettings = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowRun",         amVal)) s.appMenuClassicShowRun         = amVal != 0;
    if (key.ReadDword(L"AppMenuClassicShowShutDown",    amVal)) s.appMenuClassicShowShutDown    = amVal != 0;
    if (s.appMenuClassicPanelWidth < 80)  s.appMenuClassicPanelWidth = 80;
    if (s.appMenuClassicPanelWidth > 400) s.appMenuClassicPanelWidth = 400;

    // Settings dialog geometry
    if (key.ReadDword(L"SettingsDlgX", val)) s.settingsDlgX = static_cast<int>(val);
    if (key.ReadDword(L"SettingsDlgY", val)) s.settingsDlgY = static_cast<int>(val);
    if (key.ReadDword(L"SettingsDlgW", val)) s.settingsDlgW = static_cast<int>(val);
    if (key.ReadDword(L"SettingsDlgH", val)) s.settingsDlgH = static_cast<int>(val);

    DWORD bval = 0;
    if (key.ReadDword(L"OpenAppsOnSameMonitor", bval)) s.openAppsOnSameMonitor = (bval != 0);

    if (key.ReadDword(L"ShowProgressBars",         val)) s.showProgressBars         = val != 0;
    if (key.ReadDword(L"ProgressBarUseThemeColor", val)) s.progressBarUseThemeColor = val != 0;
    if (key.ReadDword(L"ProgressBarColor",         val)) s.progressBarColor         = static_cast<COLORREF>(val);
    if (key.ReadDword(L"ProgressBarHeight",        val)) {
        s.progressBarHeight = static_cast<int>(val);
        if (s.progressBarHeight < 1)  s.progressBarHeight = 1;
        if (s.progressBarHeight > 10) s.progressBarHeight = 10;
    }
    if (key.ReadDword(L"ButtonOutlineRadius", val)) {
        s.buttonOutlineRadius = static_cast<int>(val);
        if (s.buttonOutlineRadius < 0)  s.buttonOutlineRadius = 0;
        if (s.buttonOutlineRadius > 32) s.buttonOutlineRadius = 32;
    }
    if (key.ReadDword(L"ShowPinnedAppsAsButtons", val)) s.showPinnedAppsAsButtons = val != 0;
    if (key.ReadDword(L"ShowSeparators",          val)) s.showSeparators          = val != 0;
    if (key.ReadDword(L"SeparatorColor",          val)) s.separatorColor          = static_cast<COLORREF>(val);
    if (key.ReadDword(L"UseCustomTaskbarColor", val)) s.useCustomTaskbarColor = val != 0;
    if (key.ReadDword(L"CustomTaskbarColor",    val)) s.customTaskbarColor    = static_cast<COLORREF>(val);
    if (key.ReadDword(L"ShowTitlesOnVertical",  val)) s.showTitlesOnVertical  = val != 0;
    if (key.ReadDword(L"LeftRightHeight",       val)) {
        s.leftRightHeight = static_cast<int>(val);
        if (s.leftRightHeight < 20)  s.leftRightHeight = 20;
        if (s.leftRightHeight > 600) s.leftRightHeight = 600;
    }

    // Clamp App Menu values
    if (s.appMenuWidth        < 120)  s.appMenuWidth        = 120;
    if (s.appMenuWidth        > 800)  s.appMenuWidth        = 800;
    if (s.appMenuMaxHeight    < 100)  s.appMenuMaxHeight    = 100;
    if (s.appMenuMaxHeight    > 2000) s.appMenuMaxHeight    = 2000;
    if (s.appMenuEntryHeight  < 20)   s.appMenuEntryHeight  = 20;
    if (s.appMenuEntryHeight  > 80)   s.appMenuEntryHeight  = 80;
    if (s.appMenuGridCols     < 1)    s.appMenuGridCols     = 1;
    if (s.appMenuGridCols     > 12)   s.appMenuGridCols     = 12;
    if (s.appMenuGridRows     < 1)    s.appMenuGridRows     = 1;
    if (s.appMenuGridRows     > 20)   s.appMenuGridRows     = 20;
    if (s.appMenuListFontSize < 6)    s.appMenuListFontSize = 6;
    if (s.appMenuListFontSize > 36)   s.appMenuListFontSize = 36;
    if (s.appMenuGridFontSize < 6)    s.appMenuGridFontSize = 6;
    if (s.appMenuGridFontSize > 36)   s.appMenuGridFontSize = 36;
    if (s.appMenuMargin < 0)  s.appMenuMargin = 0;
    if (s.appMenuMargin > 40) s.appMenuMargin = 40;
    if (s.appMenuPadding < 0)  s.appMenuPadding = 0;
    if (s.appMenuPadding > 40) s.appMenuPadding = 40;
    if (s.appMenuSidebarWidth < 4)   s.appMenuSidebarWidth = 4;
    if (s.appMenuSidebarWidth > 120) s.appMenuSidebarWidth = 120;

    return s;
}

void SaveSettings(const Settings& s)
{
    auto key = RegistryKey::OpenAppKey(KEY_READ | KEY_WRITE);
    if (!key.IsOpen()) return;

    key.WriteDword(L"Position",  static_cast<DWORD>(s.position));
    key.WriteDword(L"Theme",     static_cast<DWORD>(s.theme));
    key.WriteDword(L"Thickness", static_cast<DWORD>(s.thickness));
    key.WriteDword(L"FloatX",     static_cast<DWORD>(s.floatX));
    key.WriteDword(L"FloatY",     static_cast<DWORD>(s.floatY));
    key.WriteDword(L"FloatWidth", static_cast<DWORD>(s.floatWidth));
    key.WriteDword(L"TaskbarMonitorMode",        static_cast<DWORD>(s.taskbarMonitorMode));
    key.WriteDword(L"TaskbarHookMethod",         static_cast<DWORD>(s.taskbarHookMethod));
    key.WriteDword(L"ShowAppMenuOnAllMonitors",   s.showAppMenuOnAllMonitors   ? 1u : 0u);
    key.WriteDword(L"ShowCurrentMonitorAppsOnly", s.showCurrentMonitorAppsOnly ? 1u : 0u);
    key.WriteDword(L"MaxButtonWidth",    static_cast<DWORD>(s.maxButtonWidth));
    key.WriteDword(L"MinButtonWidth",    static_cast<DWORD>(s.minButtonWidth));
    key.WriteDword(L"AppButtonIconSize", static_cast<DWORD>(s.appButtonIconSize));
    key.WriteDword(L"MiddleClickClose",  s.middleClickClose  ? 1u : 0u);
    key.WriteDword(L"ShowRightClickGap", s.showRightClickGap ? 1u : 0u);
    key.WriteDword(L"ShowMinimizedIndicator", s.showMinimizedIndicator ? 1u : 0u);
    key.WriteDword(L"MinimizedIndicatorType", static_cast<DWORD>(s.minimizedIndicatorType));
    key.WriteDword(L"MinimizedIndicatorW",    static_cast<DWORD>(s.minimizedIndicatorW));
    key.WriteDword(L"MinimizedIndicatorH",    static_cast<DWORD>(s.minimizedIndicatorH));
    key.WriteDword(L"MinimizedIndicatorDim",  static_cast<DWORD>(s.minimizedIndicatorDim));
    key.WriteDword(L"ShowStatusZone",    s.showStatusZone    ? 1u : 0u);
    key.WriteDword(L"ShowLangIndicator", s.showLangIndicator ? 1u : 0u);
    key.WriteDword(L"ShowTrayIcons",     s.showTrayIcons     ? 1u : 0u);
    key.WriteDword(L"ShowWinzooCustomIcons", s.showWinzooCustomIcons ? 1u : 0u);
    key.WriteDword(L"ShowOverflowTrayIcons", s.showOverflowTrayIcons ? 1u : 0u);
    key.WriteDword(L"TrayIconFallbackExe",  s.trayIconFallbackExe  ? 1u : 0u);
    key.WriteDword(L"TrayIconSize",      static_cast<DWORD>(s.trayIconSize));
    key.WriteDword(L"TrayIconPadding",   static_cast<DWORD>(s.trayIconPadding));
    key.WriteDword(L"TrayIconMargin",    static_cast<DWORD>(s.trayIconMargin));
    key.WriteMultiString(L"TrayIconOrder", s.trayIconOrder);
    key.WriteDword(L"ShowClock",         s.showClock         ? 1u : 0u);
    key.WriteDword(L"ClockWidth",       static_cast<DWORD>(s.clockWidth));
    key.WriteDword(L"ClockLineSpacing", static_cast<DWORD>(s.clockLineSpacing));
    key.WriteString(L"ClockTimeFormat", s.clockTimeFormat);
    key.WriteString(L"ClockDateFormat", s.clockDateFormat);
    key.WriteDword(L"ClockTimeFontSize", static_cast<DWORD>(s.clockTimeFontSize));
    key.WriteDword(L"ClockDateFontSize", static_cast<DWORD>(s.clockDateFontSize));
    key.WriteDword(L"ClockTimeColor",    static_cast<DWORD>(s.clockTimeColor));
    key.WriteDword(L"ClockDateColor",    static_cast<DWORD>(s.clockDateColor));
    key.WriteMultiString(L"PinnedPaths", s.pinnedExePaths);
    key.WriteDword(L"PinnedAppsAsButtonsWhenOpen", s.pinnedAppsAsButtonsWhenOpen ? 1u : 0u);
    key.WriteDword(L"PinnedAppsPerMonitor",        s.pinnedAppsPerMonitor        ? 1u : 0u);
    SavePerMonitorPins(key.GetHKey(), s.pinnedExePathsPerMonitor);

    key.WriteMultiString(L"AppMenuPinnedPaths", s.appMenuPinnedPaths);
    key.WriteDword(L"AppMenuPinnedPerMonitor", s.appMenuPinnedPerMonitor ? 1u : 0u);
    SavePerMonitorPins(key.GetHKey(), s.appMenuPinnedPathsPerMonitor,
                       kAppMenuPerMonPrefix, kAppMenuPerMonPrefixLen);
    key.WriteDword(L"AppMenuLayout",       static_cast<DWORD>(s.appMenuLayout));
    key.WriteDword(L"AppMenuWidth",        static_cast<DWORD>(s.appMenuWidth));
    key.WriteDword(L"AppMenuMaxHeight",    static_cast<DWORD>(s.appMenuMaxHeight));
    key.WriteDword(L"AppMenuEntryHeight",  static_cast<DWORD>(s.appMenuEntryHeight));
    key.WriteDword(L"AppMenuGridCols",     static_cast<DWORD>(s.appMenuGridCols));
    key.WriteDword(L"AppMenuGridRows",     static_cast<DWORD>(s.appMenuGridRows));
    key.WriteDword(L"AppMenuListFontSize", static_cast<DWORD>(s.appMenuListFontSize));
    key.WriteDword(L"AppMenuGridFontSize", static_cast<DWORD>(s.appMenuGridFontSize));
    key.WriteDword(L"AppMenuMargin",       static_cast<DWORD>(s.appMenuMargin));
    key.WriteDword(L"AppMenuPadding",      static_cast<DWORD>(s.appMenuPadding));
    key.WriteDword(L"AppMenuSearchEnabled",          s.appMenuSearchEnabled          ? 1u : 0u);
    key.WriteDword(L"AppMenuSearchFuzzy",            s.appMenuSearchFuzzy            ? 1u : 0u);
    key.WriteDword(L"AppMenuSearchSystem",           s.appMenuSearchSystem           ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarEnabled",         s.appMenuSidebarEnabled         ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarWidth",           static_cast<DWORD>(s.appMenuSidebarWidth));
    key.WriteDword(L"AppMenuSidebarShowExplorer",    s.appMenuSidebarShowExplorer    ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarShowSettings",    s.appMenuSidebarShowSettings    ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarShowPower",       s.appMenuSidebarShowPower       ? 1u : 0u);
    key.WriteDword(L"AppMenuFlattenMode",            static_cast<DWORD>(s.appMenuFlattenMode));
    key.WriteDword(L"AppMenuClassicPanelWidth",      static_cast<DWORD>(s.appMenuClassicPanelWidth));
    key.WriteDword(L"AppMenuClassicShowDocuments",   s.appMenuClassicShowDocuments   ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowPictures",    s.appMenuClassicShowPictures    ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowMusic",       s.appMenuClassicShowMusic       ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowDownloads",   s.appMenuClassicShowDownloads   ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowRecentItems", s.appMenuClassicShowRecentItems ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowThisPC",      s.appMenuClassicShowThisPC      ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowControlPanel",s.appMenuClassicShowControlPanel? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowWinSettings", s.appMenuClassicShowWinSettings ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowRun",         s.appMenuClassicShowRun         ? 1u : 0u);
    key.WriteDword(L"AppMenuClassicShowShutDown",    s.appMenuClassicShowShutDown    ? 1u : 0u);

    // Settings dialog geometry
    key.WriteDword(L"SettingsDlgX", static_cast<DWORD>(s.settingsDlgX));
    key.WriteDword(L"SettingsDlgY", static_cast<DWORD>(s.settingsDlgY));
    key.WriteDword(L"SettingsDlgW", static_cast<DWORD>(s.settingsDlgW));
    key.WriteDword(L"SettingsDlgH", static_cast<DWORD>(s.settingsDlgH));
    key.WriteDword(L"OpenAppsOnSameMonitor", s.openAppsOnSameMonitor ? 1u : 0u);
    key.WriteDword(L"ShowProgressBars",         s.showProgressBars         ? 1u : 0u);
    key.WriteDword(L"ProgressBarUseThemeColor", s.progressBarUseThemeColor ? 1u : 0u);
    key.WriteDword(L"ProgressBarColor",         static_cast<DWORD>(s.progressBarColor));
    key.WriteDword(L"ProgressBarHeight",        static_cast<DWORD>(s.progressBarHeight));
    key.WriteDword(L"ButtonOutlineRadius",      static_cast<DWORD>(s.buttonOutlineRadius));
    key.WriteDword(L"ShowPinnedAppsAsButtons",  s.showPinnedAppsAsButtons ? 1u : 0u);
    key.WriteDword(L"ShowSeparators",           s.showSeparators ? 1u : 0u);
    key.WriteDword(L"SeparatorColor",           static_cast<DWORD>(s.separatorColor));
    key.WriteDword(L"UseCustomTaskbarColor", s.useCustomTaskbarColor ? 1u : 0u);
    key.WriteDword(L"CustomTaskbarColor",    static_cast<DWORD>(s.customTaskbarColor));
    key.WriteDword(L"ShowTitlesOnVertical",  s.showTitlesOnVertical ? 1u : 0u);
    key.WriteDword(L"LeftRightHeight",       static_cast<DWORD>(s.leftRightHeight));
}
