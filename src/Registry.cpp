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

    if (key.ReadDword(L"MiddleClickClose", val)) s.middleClickClose = val != 0;

    key.ReadMultiString(L"PinnedPaths", s.pinnedExePaths);
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
    key.WriteDword(L"MiddleClickClose", s.middleClickClose ? 1u : 0u);
    key.WriteMultiString(L"PinnedPaths", s.pinnedExePaths);
}
