cmake_minimum_required(VERSION 3.20)
project(winzoo VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "winzoo is Windows-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor /MP /utf-8)
    add_compile_definitions(
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )
endif()

set(SOURCES
    src/main.cpp
    src/App.cpp
    src/TaskbarWindow.cpp
    src/TaskButton.cpp
    src/WindowTracker.cpp
    src/DragController.cpp
    src/PopupMenu.cpp
    src/SettingsDialog.cpp
    src/Registry.cpp
    src/AppBar.cpp
    src/Renderer.cpp
    src/IconCache.cpp
    res/winzoo.rc
)

add_executable(winzoo WIN32 ${SOURCES})
target_include_directories(winzoo PRIVATE src res)
target_link_libraries(winzoo PRIVATE
    user32 gdi32 shell32 comctl32 advapi32 shlwapi dwmapi uxtheme
)

set_target_properties(winzoo PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    LINK_FLAGS "/MANIFEST:NO"
)
