#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

class RegistryKey {
public:
    static RegistryKey OpenAppKey(REGSAM access = KEY_READ | KEY_WRITE);

    ~RegistryKey();
    RegistryKey(RegistryKey&&) noexcept;
    RegistryKey& operator=(RegistryKey&&) noexcept;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    bool IsOpen() const { return hKey_ != nullptr; }
    HKEY GetHKey() const { return hKey_; }

    bool ReadDword(const wchar_t* name, DWORD& out) const;
    bool ReadString(const wchar_t* name, std::wstring& out) const;
    bool ReadMultiString(const wchar_t* name, std::vector<std::wstring>& out) const;
    bool WriteDword(const wchar_t* name, DWORD val);
    bool WriteString(const wchar_t* name, std::wstring_view val);
    bool WriteMultiString(const wchar_t* name, const std::vector<std::wstring>& vals);

private:
    RegistryKey() = default;
    HKEY hKey_ = nullptr;
};

struct Settings;
Settings LoadSettings();
void     SaveSettings(const Settings& s);
