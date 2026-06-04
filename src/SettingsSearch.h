#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct SettingsPageDef {
    const wchar_t* displayName;
    const wchar_t* uri;
    const wchar_t* keywords; // space-separated lowercase search terms
};

static const SettingsPageDef kSettingsPages[] = {
    { L"Display settings",          L"ms-settings:display",                  L"display screen resolution brightness scaling monitor" },
    { L"Sound settings",            L"ms-settings:sound",                    L"sound audio volume speaker microphone output input" },
    { L"Bluetooth settings",        L"ms-settings:bluetooth",                L"bluetooth wireless pairing devices" },
    { L"Wi-Fi settings",            L"ms-settings:network-wifi",             L"wifi wireless network internet connection" },
    { L"Network settings",          L"ms-settings:network-status",           L"network internet ethernet status connection" },
    { L"Apps & features",           L"ms-settings:appsfeatures",             L"apps features programs install uninstall software" },
    { L"Default apps",              L"ms-settings:defaultapps",              L"default apps browser email programs" },
    { L"Windows Update",            L"ms-settings:windowsupdate",            L"update windows upgrade patch" },
    { L"Privacy settings",          L"ms-settings:privacy",                  L"privacy permissions" },
    { L"Storage settings",          L"ms-settings:storagesense",             L"storage disk space cleanup" },
    { L"Power & sleep",             L"ms-settings:powersleep",               L"power sleep battery energy saving" },
    { L"Battery settings",          L"ms-settings:batterysaver",             L"battery saver power charge" },
    { L"Accounts settings",         L"ms-settings:accounts",                 L"accounts user profile sign in login" },
    { L"Date & time",               L"ms-settings:dateandtime",              L"date time clock timezone" },
    { L"Language & region",         L"ms-settings:regionlanguage",           L"language region keyboard input locale" },
    { L"Mouse settings",            L"ms-settings:mousetouchpad",            L"mouse touchpad pointer cursor" },
    { L"Keyboard settings",         L"ms-settings:easeofaccess-keyboard",    L"keyboard shortcut keys" },
    { L"Notifications",             L"ms-settings:notifications",            L"notifications alerts toasts banners" },
    { L"Taskbar settings",          L"ms-settings:taskbar",                  L"taskbar" },
    { L"Personalization",           L"ms-settings:personalization",          L"theme personalization appearance" },
    { L"Background settings",       L"ms-settings:personalization-background", L"background wallpaper desktop" },
    { L"Color settings",            L"ms-settings:personalization-colors",   L"color accent dark light mode theme" },
    { L"Lock screen",               L"ms-settings:lockscreen",               L"lock screen" },
    { L"VPN settings",              L"ms-settings:network-vpn",              L"vpn virtual private network" },
    { L"Proxy settings",            L"ms-settings:network-proxy",            L"proxy network" },
    { L"Camera privacy",            L"ms-settings:privacy-webcam",           L"camera privacy webcam" },
    { L"Microphone privacy",        L"ms-settings:privacy-microphone",       L"microphone privacy recording" },
    { L"Location settings",         L"ms-settings:privacy-location",         L"location gps" },
    { L"Windows Security",          L"ms-settings:windowsdefender",          L"security defender antivirus firewall protection" },
    { L"System info",               L"ms-settings:about",                    L"about system info device name version" },
    { L"Troubleshoot",              L"ms-settings:troubleshoot",             L"troubleshoot fix problems" },
    { L"Accessibility",             L"ms-settings:easeofaccess",             L"accessibility ease access" },
    { L"Startup apps",              L"ms-settings:startupapps",              L"startup apps autostart boot" },
    { L"Focus assist",              L"ms-settings:quiethours",               L"focus assist do not disturb quiet hours" },
    { L"Disk management",           L"ms-settings:storagesense",             L"disk management partition format drive" },
};

// Returns pages whose lowercased display name or keywords contain lowerQuery as a substring.
// maxResults caps the output size.
inline std::vector<const SettingsPageDef*>
SearchSettingsPages(const std::wstring& lowerQuery, int maxResults = 4)
{
    std::vector<const SettingsPageDef*> results;
    for (const auto& p : kSettingsPages) {
        std::wstring lowerName(p.displayName);
        for (auto& ch : lowerName) ch = towlower(ch);
        bool match = lowerName.find(lowerQuery) != std::wstring::npos ||
                     std::wstring(p.keywords).find(lowerQuery) != std::wstring::npos;
        if (match) {
            results.push_back(&p);
            if (static_cast<int>(results.size()) >= maxResults) break;
        }
    }
    return results;
}
