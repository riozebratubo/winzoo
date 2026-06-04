#include "SettingsFile.h"
#include <windows.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ─── Encoding helpers ────────────────────────────────────────────────────────

static std::wstring Utf8ToWstr(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

// ─── JSON writing ─────────────────────────────────────────────────────────────

// Appends a JSON-escaped UTF-8 string literal (with surrounding quotes) to out.
static void AppendJsonEscaped(std::string& out, const std::wstring& val)
{
    out += '"';
    for (wchar_t wc : val) {
        switch (wc) {
        case L'"':  out += "\\\""; break;
        case L'\\': out += "\\\\"; break;
        case L'\n': out += "\\n";  break;
        case L'\r': out += "\\r";  break;
        case L'\t': out += "\\t";  break;
        default:
            if (wc < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(wc));
                out += buf;
            } else {
                char mb[5] = {};
                WideCharToMultiByte(CP_UTF8, 0, &wc, 1, mb, 4, nullptr, nullptr);
                out += mb;
            }
            break;
        }
    }
    out += '"';
}

// ─── JSON tokenizer (for safe key search) ────────────────────────────────────

static size_t SkipWS(const std::string& s, size_t pos)
{
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
    return pos;
}

// Skip a JSON string starting at the opening '"'. Returns pos after closing '"'.
static size_t SkipString(const std::string& s, size_t pos)
{
    ++pos; // skip opening '"'
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return pos;
        if (c == '\\' && pos < s.size()) ++pos; // skip escaped char
    }
    return pos;
}

// Skip any JSON value starting at pos.
static size_t SkipValue(const std::string& s, size_t pos)
{
    pos = SkipWS(s, pos);
    if (pos >= s.size()) return pos;

    char c = s[pos];
    if (c == '"') return SkipString(s, pos);

    if (c == '{') {
        ++pos;
        while (true) {
            pos = SkipWS(s, pos);
            if (pos >= s.size() || s[pos] == '}') return pos < s.size() ? pos + 1 : pos;
            if (s[pos] != '"') return pos;
            pos = SkipString(s, pos);          // skip key
            pos = SkipWS(s, pos);
            if (pos < s.size() && s[pos] == ':') ++pos;
            pos = SkipValue(s, pos);           // skip value
            pos = SkipWS(s, pos);
            if (pos < s.size() && s[pos] == ',') ++pos;
        }
    }

    if (c == '[') {
        ++pos;
        while (true) {
            pos = SkipWS(s, pos);
            if (pos >= s.size() || s[pos] == ']') return pos < s.size() ? pos + 1 : pos;
            pos = SkipValue(s, pos);
            pos = SkipWS(s, pos);
            if (pos < s.size() && s[pos] == ',') ++pos;
        }
    }

    // Number, bool, null
    while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
           s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' && s[pos] != '\r')
        ++pos;
    return pos;
}

// Find the start of a top-level JSON value for key. Returns npos if not found.
// Properly tokenizes the object so string values cannot match as keys.
static size_t FindValue(const std::string& json, const char* key)
{
    size_t pos = SkipWS(json, 0);
    if (pos >= json.size() || json[pos] != '{') return std::string::npos;
    ++pos;

    const size_t keyLen = strlen(key);

    while (true) {
        pos = SkipWS(json, pos);
        if (pos >= json.size() || json[pos] == '}') return std::string::npos;
        if (json[pos] != '"') return std::string::npos;

        // Check if this key matches: "key"
        size_t keyStart = pos + 1;
        bool match = (json.compare(keyStart, keyLen, key) == 0 &&
                      keyStart + keyLen < json.size() &&
                      json[keyStart + keyLen] == '"');

        pos = SkipString(json, pos); // advance past key
        pos = SkipWS(json, pos);
        if (pos >= json.size() || json[pos] != ':') return std::string::npos;
        ++pos;
        pos = SkipWS(json, pos);

        if (match) return pos; // value starts here

        pos = SkipValue(json, pos); // skip value, look at next member
        pos = SkipWS(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
}

// ─── JSON value parsers ───────────────────────────────────────────────────────

static bool ParseInt(const std::string& json, size_t pos, int& out)
{
    if (pos == std::string::npos || pos >= json.size()) return false;
    char* end = nullptr;
    long v = strtol(json.c_str() + pos, &end, 10);
    if (!end || end == json.c_str() + pos) return false;
    out = static_cast<int>(v);
    return true;
}

static bool ParseUInt(const std::string& json, size_t pos, unsigned int& out)
{
    if (pos == std::string::npos || pos >= json.size()) return false;
    char* end = nullptr;
    unsigned long v = strtoul(json.c_str() + pos, &end, 10);
    if (!end || end == json.c_str() + pos) return false;
    out = static_cast<unsigned int>(v);
    return true;
}

static bool ParseBool(const std::string& json, size_t pos, bool& out)
{
    if (pos == std::string::npos || pos >= json.size()) return false;
    if (json.compare(pos, 4, "true")  == 0) { out = true;  return true; }
    if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

// Parse a JSON string starting at the opening '"'. Advances endPos past the closing '"'.
static bool ParseString(const std::string& json, size_t pos,
                        std::wstring& out, size_t& endPos)
{
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"')
        return false;
    ++pos;

    std::string utf8;
    while (pos < json.size()) {
        unsigned char c = static_cast<unsigned char>(json[pos++]);
        if (c == '"') {
            out    = Utf8ToWstr(utf8);
            endPos = pos;
            return true;
        }
        if (c == '\\') {
            if (pos >= json.size()) return false;
            char esc = json[pos++];
            switch (esc) {
            case '"':  utf8 += '"';  break;
            case '\\': utf8 += '\\'; break;
            case '/':  utf8 += '/';  break;
            case 'n':  utf8 += '\n'; break;
            case 'r':  utf8 += '\r'; break;
            case 't':  utf8 += '\t'; break;
            case 'u': {
                if (pos + 4 > json.size()) return false;
                char hex[5] = { json[pos], json[pos+1], json[pos+2], json[pos+3], '\0' };
                pos += 4;
                char* ep = nullptr;
                wchar_t wc = static_cast<wchar_t>(strtoul(hex, &ep, 16));
                char mb[5] = {};
                WideCharToMultiByte(CP_UTF8, 0, &wc, 1, mb, 4, nullptr, nullptr);
                utf8 += mb;
                break;
            }
            default: utf8 += esc; break;
            }
        } else {
            utf8 += static_cast<char>(c);
        }
    }
    return false;
}

// Parse a JSON array of strings. pos must point at '['.
static bool ParseStringArray(const std::string& json, size_t pos,
                             std::vector<std::wstring>& out)
{
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '[')
        return false;
    ++pos;
    out.clear();
    while (true) {
        pos = SkipWS(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == ']') return true;
        if (json[pos] != '"') return false;

        std::wstring s;
        size_t endPos = 0;
        if (!ParseString(json, pos, s, endPos)) return false;
        out.push_back(std::move(s));
        pos = SkipWS(json, endPos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
}

// Parse a JSON object of the form {"key": ["str1", ...], ...}.
static bool ParseStringArrayMap(const std::string& json, size_t pos,
                                std::map<std::wstring, std::vector<std::wstring>>& out)
{
    pos = SkipWS(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    ++pos;
    out.clear();
    while (true) {
        pos = SkipWS(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == '}') return true;
        if (json[pos] != '"') return false;

        std::wstring key;
        size_t endPos = 0;
        if (!ParseString(json, pos, key, endPos)) return false;
        pos = endPos;
        pos = SkipWS(json, pos);
        if (pos >= json.size() || json[pos] != ':') return false;
        ++pos;
        pos = SkipWS(json, pos);

        std::vector<std::wstring> vals;
        if (pos < json.size() && json[pos] == '[')
            ParseStringArray(json, pos, vals);
        pos = SkipValue(json, pos); // advance past the value regardless
        out[std::move(key)] = std::move(vals);
        pos = SkipWS(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
}

} // anonymous namespace

// ─── Public API ───────────────────────────────────────────────────────────────

std::wstring GetSettingsFilePath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* last = wcsrchr(exePath, L'\\');
    if (last) *(last + 1) = L'\0';
    return std::wstring(exePath) + L"winzoo-settings.json";
}

bool ExportSettingsToFile(const Settings& s)
{
    std::string j;
    j.reserve(4096);
    j += "{\n";

    auto wInt = [&](const char* k, int v) {
        j += "  \""; j += k; j += "\": "; j += std::to_string(v); j += ",\n";
    };
    auto wUInt = [&](const char* k, unsigned int v) {
        j += "  \""; j += k; j += "\": "; j += std::to_string(v); j += ",\n";
    };
    auto wBool = [&](const char* k, bool v) {
        j += "  \""; j += k; j += "\": "; j += (v ? "true" : "false"); j += ",\n";
    };
    auto wStr = [&](const char* k, const std::wstring& v) {
        j += "  \""; j += k; j += "\": ";
        AppendJsonEscaped(j, v); j += ",\n";
    };

    wInt("position",  static_cast<int>(s.position));
    wInt("theme",     static_cast<int>(s.theme));
    wInt("thickness", s.thickness);
    wInt("floatX",     s.floatX);
    wInt("floatY",     s.floatY);
    wInt("floatWidth", s.floatWidth);
    wInt("taskbarMonitorMode",          static_cast<int>(s.taskbarMonitorMode));
    wBool("showAppMenuOnAllMonitors",   s.showAppMenuOnAllMonitors);
    wBool("showCurrentMonitorAppsOnly", s.showCurrentMonitorAppsOnly);
    wInt("maxButtonWidth",    s.maxButtonWidth);
    wInt("minButtonWidth",    s.minButtonWidth);
    wInt("appButtonIconSize", s.appButtonIconSize);
    wBool("middleClickClose",  s.middleClickClose);
    wBool("showRightClickGap", s.showRightClickGap);
    wBool("showMinimizedIndicator", s.showMinimizedIndicator);
    wInt("minimizedIndicatorType", static_cast<int>(s.minimizedIndicatorType));
    wInt("minimizedIndicatorW",    s.minimizedIndicatorW);
    wInt("minimizedIndicatorH",    s.minimizedIndicatorH);
    wBool("showStatusZone",   s.showStatusZone);
    wBool("showLangIndicator", s.showLangIndicator);
    wBool("showTrayIcons",    s.showTrayIcons);
    wBool("showWinzooCustomIcons", s.showWinzooCustomIcons);
    wBool("trayIconFallbackExe", s.trayIconFallbackExe);
    wBool("showOverflowTrayIcons", s.showOverflowTrayIcons);
    wInt("trayIconSize",      s.trayIconSize);
    wInt("trayIconPadding",   s.trayIconPadding);
    wInt("trayIconMargin",    s.trayIconMargin);
    wBool("showClock",        s.showClock);
    wInt("clockWidth",        s.clockWidth);
    wInt("clockLineSpacing",  s.clockLineSpacing);
    wStr("clockTimeFormat",   s.clockTimeFormat);
    wStr("clockDateFormat",   s.clockDateFormat);
    wInt("clockTimeFontSize", s.clockTimeFontSize);
    wInt("clockDateFontSize", s.clockDateFontSize);
    wUInt("clockTimeColor",   static_cast<unsigned int>(s.clockTimeColor));
    wUInt("clockDateColor",   static_cast<unsigned int>(s.clockDateColor));
    wInt("appMenuLayout",       static_cast<int>(s.appMenuLayout));
    wInt("appMenuWidth",        s.appMenuWidth);
    wInt("appMenuMaxHeight",    s.appMenuMaxHeight);
    wInt("appMenuEntryHeight",  s.appMenuEntryHeight);
    wInt("appMenuGridCols",     s.appMenuGridCols);
    wInt("appMenuGridRows",     s.appMenuGridRows);
    wInt("appMenuListFontSize", s.appMenuListFontSize);
    wInt("appMenuGridFontSize", s.appMenuGridFontSize);
    wInt("appMenuMargin",       s.appMenuMargin);
    wInt("appMenuPadding",      s.appMenuPadding);
    wInt("appMenuFlattenMode",  static_cast<int>(s.appMenuFlattenMode));
    wBool("appMenuSearchEnabled",          s.appMenuSearchEnabled);
    wBool("appMenuSearchFuzzy",            s.appMenuSearchFuzzy);
    wBool("appMenuSidebarEnabled",         s.appMenuSidebarEnabled);
    wInt("appMenuSidebarWidth",            s.appMenuSidebarWidth);
    wBool("appMenuSidebarShowExplorer",    s.appMenuSidebarShowExplorer);
    wBool("appMenuSidebarShowSettings",    s.appMenuSidebarShowSettings);
    wBool("appMenuSidebarShowPower",       s.appMenuSidebarShowPower);
    wBool("pinnedAppsAsButtonsWhenOpen",   s.pinnedAppsAsButtonsWhenOpen);
    wBool("pinnedAppsPerMonitor",          s.pinnedAppsPerMonitor);
    wInt("settingsDlgX", s.settingsDlgX);
    wInt("settingsDlgY", s.settingsDlgY);
    wInt("settingsDlgW", s.settingsDlgW);
    wInt("settingsDlgH", s.settingsDlgH);
    wBool("openAppsOnSameMonitor", s.openAppsOnSameMonitor);
    wInt("buttonOutlineRadius",        s.buttonOutlineRadius);
    wBool("showPinnedAppsAsButtons",   s.showPinnedAppsAsButtons);
    wBool("showTitlesOnVertical",      s.showTitlesOnVertical);
    wInt("leftRightHeight",            s.leftRightHeight);
    wBool("showSeparators",            s.showSeparators);
    wUInt("separatorColor",            static_cast<unsigned int>(s.separatorColor));
    wBool("showProgressBars",          s.showProgressBars);
    wBool("progressBarUseThemeColor", s.progressBarUseThemeColor);
    wUInt("progressBarColor",         static_cast<unsigned int>(s.progressBarColor));
    wInt("progressBarHeight",         s.progressBarHeight);
    wBool("useCustomTaskbarColor",    s.useCustomTaskbarColor);
    wUInt("customTaskbarColor",       static_cast<unsigned int>(s.customTaskbarColor));

    // pinnedExePaths
    j += "  \"pinnedExePaths\": [";
    if (!s.pinnedExePaths.empty()) {
        j += "\n";
        for (size_t i = 0; i < s.pinnedExePaths.size(); ++i) {
            j += "    ";
            AppendJsonEscaped(j, s.pinnedExePaths[i]);
            if (i + 1 < s.pinnedExePaths.size()) j += ",";
            j += "\n";
        }
        j += "  ";
    }
    j += "],\n";

    // trayIconOrder
    j += "  \"trayIconOrder\": [";
    if (!s.trayIconOrder.empty()) {
        j += "\n";
        for (size_t i = 0; i < s.trayIconOrder.size(); ++i) {
            j += "    ";
            AppendJsonEscaped(j, s.trayIconOrder[i]);
            if (i + 1 < s.trayIconOrder.size()) j += ",";
            j += "\n";
        }
        j += "  ";
    }
    j += "],\n";

    // pinnedExePathsPerMonitor — last field, no trailing comma
    j += "  \"pinnedExePathsPerMonitor\": {";
    if (!s.pinnedExePathsPerMonitor.empty()) {
        j += "\n";
        bool firstMon = true;
        for (const auto& [mon, paths] : s.pinnedExePathsPerMonitor) {
            if (!firstMon) j += ",\n";
            firstMon = false;
            j += "    ";
            AppendJsonEscaped(j, mon);
            j += ": [";
            if (!paths.empty()) {
                j += "\n";
                for (size_t k = 0; k < paths.size(); ++k) {
                    j += "      ";
                    AppendJsonEscaped(j, paths[k]);
                    if (k + 1 < paths.size()) j += ",";
                    j += "\n";
                }
                j += "    ";
            }
            j += "]";
        }
        j += "\n  ";
    }
    j += "}\n";
    j += "}\n";

    std::wstring path = GetSettingsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, j.c_str(), static_cast<DWORD>(j.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok && written == static_cast<DWORD>(j.size());
}

bool ImportAndDeleteSettingsFile(Settings& s)
{
    std::wstring path = GetSettingsFilePath();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || fileSize > 512 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    std::string content(static_cast<size_t>(fileSize), '\0');
    DWORD bytesRead = 0;
    bool readOk = ReadFile(hFile, content.data(), fileSize, &bytesRead, nullptr)
                  && bytesRead == fileSize;
    CloseHandle(hFile);
    if (!readOk) return false;

    // Parse into ns, initialised from current settings so missing fields keep
    // their loaded values rather than reverting to hard-coded defaults.
    Settings ns = s;

    // Helpers ────────────────────────────────────────────────────────────────
    auto ri = [&](const char* key, int& out) {
        size_t p = FindValue(content, key);
        int v = 0;
        if (p != std::string::npos && ParseInt(content, p, v)) out = v;
    };
    auto rb = [&](const char* key, bool& out) {
        size_t p = FindValue(content, key);
        bool v = false;
        if (p != std::string::npos && ParseBool(content, p, v)) out = v;
    };
    auto rStr = [&](const char* key, std::wstring& out) {
        size_t p = FindValue(content, key);
        if (p != std::string::npos && p < content.size() && content[p] == '"') {
            size_t ep = 0;
            ParseString(content, p, out, ep);
        }
    };
    // Enum with range guard
    auto rEnum = [&](const char* key, int lo, int hi, int& out) {
        size_t p = FindValue(content, key);
        int v = 0;
        if (p != std::string::npos && ParseInt(content, p, v) && v >= lo && v <= hi)
            out = v;
    };

    // Enums (validated) ──────────────────────────────────────────────────────
    { int v = static_cast<int>(ns.position);
      rEnum("position", 0, 4, v);
      ns.position = static_cast<TaskbarPosition>(v); }

    { int v = static_cast<int>(ns.theme);
      rEnum("theme", 0, kThemePresetCount - 1, v);
      ns.theme = static_cast<ThemePreset>(v); }

    { int v = static_cast<int>(ns.taskbarMonitorMode);
      rEnum("taskbarMonitorMode", 0, 1, v);
      ns.taskbarMonitorMode = static_cast<TaskbarMonitorMode>(v); }

    { int v = static_cast<int>(ns.appMenuLayout);
      rEnum("appMenuLayout", 0, 1, v);
      ns.appMenuLayout = static_cast<AppMenuLayout>(v); }

    { int v = static_cast<int>(ns.appMenuFlattenMode);
      rEnum("appMenuFlattenMode", 0, 2, v);
      ns.appMenuFlattenMode = static_cast<AppMenuFlattenMode>(v); }

    // Scalar fields ──────────────────────────────────────────────────────────
    ri("thickness",  ns.thickness);
    ri("floatX",     ns.floatX);
    ri("floatY",     ns.floatY);
    ri("floatWidth", ns.floatWidth);
    rb("showAppMenuOnAllMonitors",   ns.showAppMenuOnAllMonitors);
    rb("showCurrentMonitorAppsOnly", ns.showCurrentMonitorAppsOnly);
    ri("maxButtonWidth",    ns.maxButtonWidth);
    ri("minButtonWidth",    ns.minButtonWidth);
    ri("appButtonIconSize", ns.appButtonIconSize);
    rb("middleClickClose",  ns.middleClickClose);
    rb("showRightClickGap", ns.showRightClickGap);
    rb("showMinimizedIndicator", ns.showMinimizedIndicator);
    { int v = static_cast<int>(ns.minimizedIndicatorType);
      rEnum("minimizedIndicatorType", 0, 1, v);
      ns.minimizedIndicatorType = static_cast<MinimizedIndicatorType>(v); }
    ri("minimizedIndicatorW", ns.minimizedIndicatorW);
    ri("minimizedIndicatorH", ns.minimizedIndicatorH);
    rb("showStatusZone",   ns.showStatusZone);
    rb("showLangIndicator", ns.showLangIndicator);
    rb("showTrayIcons",    ns.showTrayIcons);
    rb("showWinzooCustomIcons", ns.showWinzooCustomIcons);
    rb("trayIconFallbackExe", ns.trayIconFallbackExe);
    rb("showOverflowTrayIcons", ns.showOverflowTrayIcons);
    ri("trayIconSize",     ns.trayIconSize);
    ri("trayIconPadding",  ns.trayIconPadding);
    ri("trayIconMargin",   ns.trayIconMargin);
    rb("showClock",        ns.showClock);
    ri("clockWidth",       ns.clockWidth);
    ri("clockLineSpacing", ns.clockLineSpacing);
    rStr("clockTimeFormat", ns.clockTimeFormat);
    rStr("clockDateFormat", ns.clockDateFormat);
    ri("clockTimeFontSize", ns.clockTimeFontSize);
    ri("clockDateFontSize", ns.clockDateFontSize);

    { size_t p = FindValue(content, "clockTimeColor");
      unsigned int v = 0;
      if (p != std::string::npos && ParseUInt(content, p, v))
          ns.clockTimeColor = static_cast<COLORREF>(v); }

    { size_t p = FindValue(content, "clockDateColor");
      unsigned int v = 0;
      if (p != std::string::npos && ParseUInt(content, p, v))
          ns.clockDateColor = static_cast<COLORREF>(v); }

    ri("appMenuWidth",        ns.appMenuWidth);
    ri("appMenuMaxHeight",    ns.appMenuMaxHeight);
    ri("appMenuEntryHeight",  ns.appMenuEntryHeight);
    ri("appMenuGridCols",     ns.appMenuGridCols);
    ri("appMenuGridRows",     ns.appMenuGridRows);
    ri("appMenuListFontSize", ns.appMenuListFontSize);
    ri("appMenuGridFontSize", ns.appMenuGridFontSize);
    ri("appMenuMargin",       ns.appMenuMargin);
    ri("appMenuPadding",      ns.appMenuPadding);
    rb("appMenuSearchEnabled",          ns.appMenuSearchEnabled);
    rb("appMenuSearchFuzzy",            ns.appMenuSearchFuzzy);
    rb("appMenuSidebarEnabled",         ns.appMenuSidebarEnabled);
    ri("appMenuSidebarWidth",           ns.appMenuSidebarWidth);
    rb("appMenuSidebarShowExplorer",    ns.appMenuSidebarShowExplorer);
    rb("appMenuSidebarShowSettings",    ns.appMenuSidebarShowSettings);
    rb("appMenuSidebarShowPower",       ns.appMenuSidebarShowPower);
    rb("pinnedAppsAsButtonsWhenOpen",   ns.pinnedAppsAsButtonsWhenOpen);
    rb("pinnedAppsPerMonitor",          ns.pinnedAppsPerMonitor);
    ri("settingsDlgX", ns.settingsDlgX);
    ri("settingsDlgY", ns.settingsDlgY);
    ri("settingsDlgW", ns.settingsDlgW);
    ri("settingsDlgH", ns.settingsDlgH);
    rb("openAppsOnSameMonitor", ns.openAppsOnSameMonitor);
    ri("buttonOutlineRadius",       ns.buttonOutlineRadius);
    rb("showPinnedAppsAsButtons",   ns.showPinnedAppsAsButtons);
    rb("showTitlesOnVertical",      ns.showTitlesOnVertical);
    ri("leftRightHeight",           ns.leftRightHeight);
    rb("showSeparators",            ns.showSeparators);
    { size_t p = FindValue(content, "separatorColor");
      unsigned int v = 0;
      if (p != std::string::npos && ParseUInt(content, p, v))
          ns.separatorColor = static_cast<COLORREF>(v); }
    rb("showProgressBars",          ns.showProgressBars);
    rb("progressBarUseThemeColor", ns.progressBarUseThemeColor);
    ri("progressBarHeight",        ns.progressBarHeight);
    { size_t p = FindValue(content, "progressBarColor");
      unsigned int v = 0;
      if (p != std::string::npos && ParseUInt(content, p, v))
          ns.progressBarColor = static_cast<COLORREF>(v); }
    rb("useCustomTaskbarColor", ns.useCustomTaskbarColor);
    { size_t p = FindValue(content, "customTaskbarColor");
      unsigned int v = 0;
      if (p != std::string::npos && ParseUInt(content, p, v))
          ns.customTaskbarColor = static_cast<COLORREF>(v); }

    { size_t p = FindValue(content, "pinnedExePaths");
      if (p != std::string::npos) ParseStringArray(content, p, ns.pinnedExePaths); }

    { size_t p = FindValue(content, "trayIconOrder");
      if (p != std::string::npos) ParseStringArray(content, p, ns.trayIconOrder); }

    { size_t p = FindValue(content, "pinnedExePathsPerMonitor");
      if (p != std::string::npos) ParseStringArrayMap(content, p, ns.pinnedExePathsPerMonitor); }

    s = ns;
    DeleteFileW(path.c_str());
    return true;
}
