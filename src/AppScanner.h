#pragma once
#include <vector>
#include "AppEntry.h"

class AppScanner {
public:
    // Scans installed applications from Windows Uninstall registry keys.
    // Returns entries sorted by name, filtered to user-visible apps only.
    static std::vector<AppEntry> Scan();
};
