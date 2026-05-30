#pragma once
#include "Settings.h"
#include <string>

// Returns the path to the settings file: <exe_dir>\winzoo-settings.json
std::wstring GetSettingsFilePath();

// Serializes all settings to winzoo-settings.json next to the exe.
// Returns true on success.
bool ExportSettingsToFile(const Settings& s);

// If winzoo-settings.json exists, parses it into s and deletes the file.
// Returns true if the file was found and parsed successfully.
// On failure (file missing or malformed), s is unchanged and the file is left in place.
bool ImportAndDeleteSettingsFile(Settings& s);
