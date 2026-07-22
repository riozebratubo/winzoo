#pragma once
#include <vector>
#include "AppEntry.h"

class AppScanner {
public:
    // Walks the per-user and all-users Start Menu Programs folders for .lnk
    // entries (AppFolderWatcher watches the same roots to trigger rescans).
    // Returns entries sorted by (folder, name), deduplicated across the roots.
    static std::vector<AppEntry> Scan();
};
