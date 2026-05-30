#include "Registry.h"
#include "Settings.h"

static constexpr wchar_t kRegPath[] = L"Software\\Winzoo";

RegistryKey RegistryKey::OpenAppKey(REGSAM access)
{
    RegistryKey key;
    DWORD disp;
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

bool RegistryKey::ReadDword(std::wstring_view name, DWORD& out) const
{
    if (!hKey_) return false;
    DWORD size = sizeof(DWORD), type;
    return RegQueryValueExW(hKey_, name.data(), nullptr, &type,
                            reinterpret_cast<BYTE*>(&out), &size) == ERROR_SUCCESS
           && type == REG_DWORD;
}

bool RegistryKey::ReadString(std::wstring_view name, std::wstring& out) const
{
    if (!hKey_) return false;
    DWORD size = 0, type;
    if (RegQueryValueExW(hKey_, name.data(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    if (type != REG_SZ) return false;

    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey_, name.data(), nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS)
        return false;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    out = std::move(buf);
    return true;
}

bool RegistryKey::WriteString(std::wstring_view name, std::wstring_view val)
{
    if (!hKey_) return false;
    return RegSetValueExW(hKey_, name.data(), 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(val.data()),
                          static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegistryKey::ReadMultiString(std::wstring_view name, std::vector<std::wstring>& out) const
{
    if (!hKey_) return false;
    DWORD size = 0, type;
    if (RegQueryValueExW(hKey_, name.data(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    if (type != REG_MULTI_SZ) return false;

    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(hKey_, name.data(), nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &size) != ERROR_SUCCESS)
        return false;

    out.clear();
    const wchar_t* p = buf.data();
    while (*p) {
        out.emplace_back(p);
        p += out.back().size() + 1;
    }
    return true;
}

bool RegistryKey::WriteDword(std::wstring_view name, DWORD val)
{
    if (!hKey_) return false;
    return RegSetValueExW(hKey_, name.data(), 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS;
}

bool RegistryKey::WriteMultiString(std::wstring_view name, const std::vector<std::wstring>& vals)
{
    if (!hKey_) return false;
    std::wstring buf;
    for (const auto& s : vals) { buf += s; buf += L'\0'; }
    buf += L'\0';
    return RegSetValueExW(hKey_, name.data(), 0, REG_MULTI_SZ,
                          reinterpret_cast<const BYTE*>(buf.data()),
                          static_cast<DWORD>(buf.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

Settings LoadSettings()
{
    Settings s;
    auto key = RegistryKey::OpenAppKey(KEY_READ);
    if (!key.IsOpen()) return s;

    DWORD val;
    if (key.ReadDword(L"Position", val))  s.position  = static_cast<TaskbarPosition>(val);
    if (key.ReadDword(L"Theme", val))     s.theme     = static_cast<ThemePreset>(val);
    if (key.ReadDword(L"Thickness", val)) s.thickness = static_cast<int>(val);
    if (key.ReadDword(L"FloatX", val))    s.floatX    = static_cast<int>(val);
    if (key.ReadDword(L"FloatY", val))    s.floatY    = static_cast<int>(val);

    // Clamp thickness
    if (s.thickness < 28) s.thickness = 28;
    if (s.thickness > 120) s.thickness = 120;

    if (key.ReadDword(L"MaxButtonWidth", val)) s.maxButtonWidth = static_cast<int>(val);
    if (key.ReadDword(L"MinButtonWidth", val)) s.minButtonWidth = static_cast<int>(val);
    if (s.maxButtonWidth < 48)  s.maxButtonWidth = 48;
    if (s.maxButtonWidth > 400) s.maxButtonWidth = 400;
    if (s.minButtonWidth < 24)  s.minButtonWidth = 24;
    if (s.minButtonWidth > s.maxButtonWidth) s.minButtonWidth = s.maxButtonWidth;

    if (key.ReadDword(L"MiddleClickClose",  val)) s.middleClickClose  = val != 0;
    if (key.ReadDword(L"ShowRightClickGap", val)) s.showRightClickGap = val != 0;
    if (key.ReadDword(L"ShowMinimizedIndicator", val)) s.showMinimizedIndicator = val != 0;
    if (key.ReadDword(L"MinimizedIndicatorW", val)) s.minimizedIndicatorW = static_cast<int>(val);
    if (key.ReadDword(L"MinimizedIndicatorH", val)) s.minimizedIndicatorH = static_cast<int>(val);
    if (s.minimizedIndicatorW < 2)  s.minimizedIndicatorW = 2;
    if (s.minimizedIndicatorW > 40) s.minimizedIndicatorW = 40;
    if (s.minimizedIndicatorH < 1)  s.minimizedIndicatorH = 1;
    if (s.minimizedIndicatorH > 20) s.minimizedIndicatorH = 20;
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

    DWORD amVal;
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
    if (key.ReadDword(L"AppMenuSidebarEnabled",         amVal)) s.appMenuSidebarEnabled         = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarWidth",           amVal)) s.appMenuSidebarWidth           = static_cast<int>(amVal);
    if (key.ReadDword(L"AppMenuSidebarShowExplorer",    amVal)) s.appMenuSidebarShowExplorer    = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarShowSettings",    amVal)) s.appMenuSidebarShowSettings    = amVal != 0;
    if (key.ReadDword(L"AppMenuSidebarShowPower",       amVal)) s.appMenuSidebarShowPower       = amVal != 0;
    if (key.ReadDword(L"AppMenuFlattenMode", amVal) && amVal <= 2)
        s.appMenuFlattenMode = static_cast<AppMenuFlattenMode>(amVal);

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
    auto key = RegistryKey::OpenAppKey(KEY_WRITE);
    if (!key.IsOpen()) return;

    key.WriteDword(L"Position",  static_cast<DWORD>(s.position));
    key.WriteDword(L"Theme",     static_cast<DWORD>(s.theme));
    key.WriteDword(L"Thickness", static_cast<DWORD>(s.thickness));
    key.WriteDword(L"FloatX",    static_cast<DWORD>(s.floatX));
    key.WriteDword(L"FloatY",          static_cast<DWORD>(s.floatY));
    key.WriteDword(L"MaxButtonWidth", static_cast<DWORD>(s.maxButtonWidth));
    key.WriteDword(L"MinButtonWidth", static_cast<DWORD>(s.minButtonWidth));
    key.WriteDword(L"MiddleClickClose",  s.middleClickClose  ? 1u : 0u);
    key.WriteDword(L"ShowRightClickGap", s.showRightClickGap ? 1u : 0u);
    key.WriteDword(L"ShowMinimizedIndicator", s.showMinimizedIndicator ? 1u : 0u);
    key.WriteDword(L"MinimizedIndicatorW",    static_cast<DWORD>(s.minimizedIndicatorW));
    key.WriteDword(L"MinimizedIndicatorH",    static_cast<DWORD>(s.minimizedIndicatorH));
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
    key.WriteDword(L"AppMenuSidebarEnabled",         s.appMenuSidebarEnabled         ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarWidth",           static_cast<DWORD>(s.appMenuSidebarWidth));
    key.WriteDword(L"AppMenuSidebarShowExplorer",    s.appMenuSidebarShowExplorer    ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarShowSettings",    s.appMenuSidebarShowSettings    ? 1u : 0u);
    key.WriteDword(L"AppMenuSidebarShowPower",       s.appMenuSidebarShowPower       ? 1u : 0u);
    key.WriteDword(L"AppMenuFlattenMode",            static_cast<DWORD>(s.appMenuFlattenMode));
}
