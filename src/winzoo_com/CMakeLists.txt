cmake_minimum_required(VERSION 3.20)

add_library(winzoo_com SHARED winzoo_com.cpp winzoo_com_exports.cpp winzoo_com.def)

target_compile_definitions(winzoo_com PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

if(MSVC)
    target_compile_options(winzoo_com PRIVATE /W4 /permissive- /Zc:preprocessor /MP /utf-8)
    set_target_properties(winzoo_com PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

target_link_libraries(winzoo_com PRIVATE user32 ole32 gdi32)

# Place the DLL next to winzoo.exe
set_target_properties(winzoo_com PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Stage a config-independent copy for embedding into winzoo.exe resources
add_custom_command(TARGET winzoo_com POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:winzoo_com>
        ${CMAKE_BINARY_DIR}/winzoo_com_embed.bin
    COMMENT "Staging winzoo_com.dll for embedding"
)
