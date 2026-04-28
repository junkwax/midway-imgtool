cmake_minimum_required(VERSION 3.20)

project(imgtool C CXX)

# Capture git short SHA at configure time so the About modal can show it.
# Falls back to "unknown" if git isn't on PATH or this isn't a git checkout.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE IMGTOOL_GIT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT IMGTOOL_GIT_REV)
    set(IMGTOOL_GIT_REV "unknown")
endif()

# SDL2
if(WIN32)
    find_package(SDL2 REQUIRED)
else()
    set(SDL2_INCLUDE_DIRS "/usr/include/SDL2;/usr/include/x86_64-linux-gnu")
    set(SDL2_LIBRARIES "-L/usr/lib/x86_64-linux-gnu -lSDL2")
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}")
    add_library(SDL2::SDL2main INTERFACE IMPORTED)
endif()

# ---- Sources ---------------------------------------------------------------
if(WIN32)
    set(PLATFORM_RC platform/imgtool.rc)
else()
    set(PLATFORM_RC "")
endif()

add_executable(imgtool
    IT/it.c
    platform/globals.c
    platform/shim_vid.c
    platform/shim_input.c
    platform/shim_file.c
    platform/shim_dialog.c
    # ImGui overlay
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl2.cpp
    imgui/backends/imgui_impl_sdlrenderer2.cpp
    platform/imgui_overlay.cpp
    ${PLATFORM_RC}
)

target_include_directories(imgtool PRIVATE
    platform
    IT
    imgui
    imgui/backends
)

target_link_libraries(imgtool PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

# ---- Platform-specific flags -----------------------------------------------
if(WIN32)
    set_target_properties(imgtool PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(MSVC)
    target_compile_options(imgtool PRIVATE /W3)
    target_compile_definitions(imgtool PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_definitions(imgtool PRIVATE IMGTOOL_GIT_REV="${IMGTOOL_GIT_REV}")

# Copy bundled icon font next to the exe
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/MaterialSymbolsSharp-Regular.ttf")
    add_custom_command(TARGET imgtool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:imgtool>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/MaterialSymbolsSharp-Regular.ttf"
            "$<TARGET_FILE_DIR:imgtool>/assets/MaterialSymbolsSharp-Regular.ttf"
        VERBATIM
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/DMA2.txt")
    add_custom_command(TARGET imgtool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/DMA2.txt"
            "$<TARGET_FILE_DIR:imgtool>/DMA2.txt"
        VERBATIM
    )
endif()

if(MSVC)
    set_source_files_properties(
        imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
        imgui/backends/imgui_impl_sdl2.cpp imgui/backends/imgui_impl_sdlrenderer2.cpp
        PROPERTIES
        COMPILE_FLAGS "/W0"
    )
endif()

if(NOT WIN32)
    target_compile_options(imgtool PRIVATE -m64)
    target_link_options(imgtool PRIVATE -m64 -no-pie -Wl,-z,noexecstack)
endif()
