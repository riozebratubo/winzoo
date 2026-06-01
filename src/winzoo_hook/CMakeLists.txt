cmake_minimum_required(VERSION 3.20)

add_library(winzoo_hook SHARED winzoo_hook.cpp)

target_compile_definitions(winzoo_hook PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

if(MSVC)
    target_compile_options(winzoo_hook PRIVATE /W4 /permissive- /Zc:preprocessor /MP /utf-8)
endif()

target_link_libraries(winzoo_hook PRIVATE user32)

# Place the DLL next to winzoo.exe so TaskbarProxy::Install() can find it.
set_target_properties(winzoo_hook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
