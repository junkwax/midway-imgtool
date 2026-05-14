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
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        # Try Homebrew (macOS) and standard Linux paths
        set(SDL2_INCLUDE_DIRS "/opt/homebrew/include/SDL2;/usr/local/include/SDL2;/usr/include/SDL2;/usr/include/x86_64-linux-gnu")
        set(SDL2_LIBRARIES "-L/opt/homebrew/lib -L/usr/local/lib -L/usr/lib/x86_64-linux-gnu -lSDL2")
        add_library(SDL2::SDL2 INTERFACE IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${SDL2_LIBRARIES}")
        add_library(SDL2::SDL2main INTERFACE IMPORTED)
    endif()
endif()

# ---- Sources ---------------------------------------------------------------
if(WIN32)
    set(PLATFORM_RC platform/imgtool.rc)
else()
    set(PLATFORM_RC "")
endif()

add_executable(imgtool
    platform/main.cpp
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
    platform/img_io.cpp
    platform/load2_verify.cpp
    platform/lod_parser.cpp
    platform/mk2_hitbox.cpp
    ${PLATFORM_RC}
)

target_include_directories(imgtool PRIVATE
    platform
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

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
        target_compile_options(imgtool PRIVATE -m64)
        target_link_options(imgtool PRIVATE -m64)
    endif()
    set_target_properties(imgtool PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
elseif(NOT WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
        target_compile_options(imgtool PRIVATE -m64)
        target_link_options(imgtool PRIVATE -m64 -no-pie -Wl,-z,noexecstack)
    else()
        target_link_options(imgtool PRIVATE -no-pie -Wl,-z,noexecstack)
    endif()
endif()

# ---- Tests -----------------------------------------------------------------
# mk2_roundtrip_test: load + idempotent round-trip check on a real MKSTK.ASM.
# Only built when MK2_MKSTK_PATH is set (so a fresh checkout without the mk2
# source tree doesn't fail to configure). Run after build with:
#   ctest --output-on-failure
# or directly:
#   build\test\Release\mk2_roundtrip_test path\to\MKSTK.ASM
enable_testing()
set(MK2_MKSTK_PATH "" CACHE FILEPATH
    "Path to MKSTK.ASM for the mk2_roundtrip_test. Empty disables the test.")
if(MK2_MKSTK_PATH AND EXISTS "${MK2_MKSTK_PATH}")
    add_executable(mk2_roundtrip_test
        test/mk2_roundtrip_test.cpp
        platform/mk2_hitbox.cpp
    )
    target_include_directories(mk2_roundtrip_test PRIVATE platform)
    if(MSVC)
        target_compile_options(mk2_roundtrip_test PRIVATE /W3)
        target_compile_definitions(mk2_roundtrip_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME mk2_roundtrip
             COMMAND mk2_roundtrip_test "${MK2_MKSTK_PATH}")
endif()
