cmake_minimum_required(VERSION 3.20)
project(winzoo VERSION 0.4.5 LANGUAGES CXX)

# Version is sourced from the project(... VERSION ...) line above. project()
# only populates three components, so default the 4th (tweak) to 0 and build a
# full dotted string for the manifest / VERSIONINFO.
set(WINZOO_VERSION_TWEAK 0)
set(WINZOO_VERSION_FULL
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${WINZOO_VERSION_TWEAK}")

if(NOT WIN32)
    message(FATAL_ERROR "winzoo is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENABLE_MSVC_ANALYZE OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /MP /utf-8)
    add_compile_definitions(
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )
endif()

# Optional MSVC PREfast static analysis. Enable with:
#   cmake -B build -DENABLE_MSVC_ANALYZE=ON -G "Visual Studio 18 2026" -A x64
#   cmake --build build --config Release
# Results appear in the build output and in .analyze XML files next to each .obj.
option(ENABLE_MSVC_ANALYZE "Enable MSVC /analyze static analysis (PREfast)" OFF)
if(MSVC AND ENABLE_MSVC_ANALYZE)
    add_compile_options(/analyze)
endif()

# Last-resort crash reporter: on an unhandled fault, append a symbolized stack
# trace to winzoo-crash.txt next to the exe. Disable with -DWINZOO_CRASH_HANDLER=OFF.
option(WINZOO_CRASH_HANDLER "Build the unhandled-exception crash reporter" ON)

configure_file(
    ${CMAKE_SOURCE_DIR}/res/winzoo_embed.rc.in
    ${CMAKE_BINARY_DIR}/winzoo_embed.rc
    @ONLY
)

# Generated version header (consumed by both C++ sources and the .rc) and the
# application manifest, both stamped from the project version above.
configure_file(
    ${CMAKE_SOURCE_DIR}/res/winzoo_version.h.in
    ${CMAKE_BINARY_DIR}/winzoo_version.h
    @ONLY
)
configure_file(
    ${CMAKE_SOURCE_DIR}/res/manifest.xml.in
    ${CMAKE_BINARY_DIR}/manifest.xml
    @ONLY
)

set(SOURCES
    src/main.cpp
    src/App.cpp
    src/TaskbarWindow.cpp
    src/TaskButton.cpp
    src/WindowTracker.cpp
    src/WinEventNotifier.cpp
    src/AppFolderWatcher.cpp
    src/DragController.cpp
    src/PopupMenu.cpp
    src/SettingsDialog.cpp
    src/InputDialog.cpp
    src/Registry.cpp
    src/AppBar.cpp
    src/Renderer.cpp
    src/IconCache.cpp
    src/AppScanner.cpp
    src/AppIconCache.cpp
    src/AppMenuWindow.cpp
    src/SettingsFile.cpp
    src/SystemTools.cpp
    src/SystemStatus.cpp
    src/TrayIconProvider.cpp
    src/LaunchHelper.cpp
    src/JumpList.cpp
    src/TaskbarProxy.cpp
    src/TaskbarRelocate.cpp
    src/ExplorerRibbon.cpp
    res/winzoo.rc
    ${CMAKE_BINARY_DIR}/winzoo_embed.rc
)

if(WINZOO_CRASH_HANDLER)
    list(APPEND SOURCES src/CrashHandler.cpp)
endif()

add_subdirectory(src/winzoo_com)

add_executable(winzoo WIN32 ${SOURCES})
add_dependencies(winzoo winzoo_com)
target_include_directories(winzoo PRIVATE src res ${CMAKE_BINARY_DIR})
target_link_libraries(winzoo PRIVATE
    user32 gdi32 shell32 comctl32 advapi32 shlwapi dwmapi uxtheme comdlg32 ole32 uuid psapi propsys wlanapi
)

if(WINZOO_CRASH_HANDLER)
    target_compile_definitions(winzoo PRIVATE WINZOO_CRASH_HANDLER)
    target_link_libraries(winzoo PRIVATE dbghelp)
    # Emit a .pdb even for Release so the crash-report stack trace carries real
    # function names + source lines (without it, frames show only module+offset).
    # /OPT:REF,ICF undo the optimization /DEBUG would otherwise disable.
    if(MSVC)
        target_compile_options(winzoo PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(winzoo PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
endif()

set_target_properties(winzoo PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LINK_FLAGS "/MANIFEST:NO"
)
