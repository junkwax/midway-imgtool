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

set(IMGTOOL_VERSION "0.0.0-dev")
if(DEFINED ENV{IMGTOOL_VERSION} AND NOT "$ENV{IMGTOOL_VERSION}" STREQUAL "")
    set(IMGTOOL_VERSION "$ENV{IMGTOOL_VERSION}")
elseif(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
       AND DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
    set(IMGTOOL_VERSION "$ENV{GITHUB_REF_NAME}")
elseif(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE IMGTOOL_EXACT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(IMGTOOL_EXACT_TAG)
        set(IMGTOOL_VERSION "${IMGTOOL_EXACT_TAG}")
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --abbrev=0
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE IMGTOOL_BASE_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(IMGTOOL_BASE_TAG)
            string(REGEX REPLACE "^v" "" IMGTOOL_BASE_VERSION "${IMGTOOL_BASE_TAG}")
            set(IMGTOOL_VERSION "${IMGTOOL_BASE_VERSION}-dev+${IMGTOOL_GIT_REV}")
        elseif(NOT IMGTOOL_GIT_REV STREQUAL "unknown")
            set(IMGTOOL_VERSION "dev+${IMGTOOL_GIT_REV}")
        endif()
    endif()
endif()
string(REGEX REPLACE "^v" "" IMGTOOL_VERSION "${IMGTOOL_VERSION}")
message(STATUS "ImgTool version: ${IMGTOOL_VERSION} (${IMGTOOL_GIT_REV})")

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

set(IMGTOOL_SOURCES
    platform/main.cpp
    platform/document.cpp
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
    platform/ui_state.cpp
    platform/ui_main.cpp
    platform/ui_canvas.cpp
    platform/ui_timeline.cpp
    platform/ui_palette.cpp
    platform/ui_tools.cpp
    platform/ui_modals.cpp
    platform/ui_undo.cpp
    platform/ui_autochop.cpp
    platform/ui_bodysplit.cpp
    platform/body_split.cpp
    platform/world_render.cpp
    platform/anipoint.cpp
    platform/anipoint_edit.cpp
    platform/anipoint_mirror.cpp
    platform/tbl_diff.cpp
    platform/img_util.cpp
    platform/color_ops.cpp
    platform/image_ops.cpp
    platform/canvas_ops.cpp
    platform/gif_trim.cpp
    platform/palette_math.cpp
    platform/sprite_resize_ops.cpp
    platform/img_io.cpp
    platform/load2_verify.cpp
    platform/lod_parser.cpp
    platform/mk2_hitbox.cpp
    platform/mk2_fatality.cpp
    ${PLATFORM_RC}
)

add_executable(imgtool ${IMGTOOL_SOURCES})
add_executable(imgtool-cli ${IMGTOOL_SOURCES})

target_include_directories(imgtool PRIVATE
    platform
    imgui
    imgui/backends
)
target_include_directories(imgtool-cli PRIVATE
    platform
    imgui
    imgui/backends
)

target_link_libraries(imgtool PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)
target_link_libraries(imgtool-cli PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
)

# ---- Platform-specific flags -----------------------------------------------
if(WIN32)
    set_target_properties(imgtool PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(MSVC)
    target_compile_options(imgtool PRIVATE /W3)
    target_compile_options(imgtool-cli PRIVATE /W3)
    target_compile_definitions(imgtool PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(imgtool-cli PRIVATE _CRT_SECURE_NO_WARNINGS)

    # Emit a PDB for Release builds too (optimizations stay on; this only adds
    # debug symbols alongside the exe) so the in-app crash handler's stack
    # walk and any .dmp from a Release crash can resolve real function
    # names/lines instead of just module+offset.
    target_compile_options(imgtool PRIVATE $<$<CONFIG:Release>:/Zi /FS>)
    target_link_options(imgtool PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

target_compile_definitions(imgtool PRIVATE
    IMGTOOL_VERSION="${IMGTOOL_VERSION}"
    IMGTOOL_GIT_REV="${IMGTOOL_GIT_REV}"
)
target_compile_definitions(imgtool-cli PRIVATE
    IMGTOOL_VERSION="${IMGTOOL_VERSION}"
    IMGTOOL_GIT_REV="${IMGTOOL_GIT_REV}"
    IMGTOOL_CLI_ONLY
)

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
        target_compile_options(imgtool-cli PRIVATE -m64)
        target_link_options(imgtool-cli PRIVATE -m64)
    endif()
    set_target_properties(imgtool PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
    set_target_properties(imgtool-cli PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
elseif(NOT WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
        target_compile_options(imgtool PRIVATE -m64)
        target_link_options(imgtool PRIVATE -m64 -no-pie -Wl,-z,noexecstack)
        target_compile_options(imgtool-cli PRIVATE -m64)
        target_link_options(imgtool-cli PRIVATE -m64 -no-pie -Wl,-z,noexecstack)
    else()
        target_link_options(imgtool PRIVATE -no-pie -Wl,-z,noexecstack)
        target_link_options(imgtool-cli PRIVATE -no-pie -Wl,-z,noexecstack)
    endif()
endif()

# ---- Tests -----------------------------------------------------------------
enable_testing()
add_executable(sprite_sheet_import_test
    test/sprite_sheet_import_test.cpp
    platform/img_io.cpp
    platform/anipoint.cpp
    platform/anipoint_mirror.cpp
    platform/img_util.cpp
    platform/tbl_diff.cpp
    platform/load2_verify.cpp
    platform/palette_math.cpp
    platform/gif_trim.cpp
    platform/document.cpp
    platform/globals.c
)
target_include_directories(sprite_sheet_import_test PRIVATE platform)
target_link_libraries(sprite_sheet_import_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(sprite_sheet_import_test PRIVATE /W3)
    target_compile_definitions(sprite_sheet_import_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(sprite_sheet_import_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME sprite_sheet_import
         COMMAND sprite_sheet_import_test)

# world_blit_test: the CPU sprite compositor behind the World View PNG export,
# plus the RGBA->PNG writer it feeds. Needs the imgui include dir because
# world_render.cpp pulls ui_internal.h for g_imgui_renderer, but links no imgui
# sources — it only ever touches SDL and the document lists.
add_executable(world_blit_test
    test/world_blit_test.cpp
    platform/world_render.cpp
    platform/img_io.cpp
    platform/anipoint.cpp
    platform/anipoint_mirror.cpp
    platform/img_util.cpp
    platform/tbl_diff.cpp
    platform/load2_verify.cpp
    platform/palette_math.cpp
    platform/gif_trim.cpp
    platform/document.cpp
    platform/globals.c
)
target_include_directories(world_blit_test PRIVATE platform imgui)
target_link_libraries(world_blit_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(world_blit_test PRIVATE /W3)
    target_compile_definitions(world_blit_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(world_blit_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME world_blit COMMAND world_blit_test)

# Pure-module unit tests. Each links only the extracted module under test (no
# app/UI sources), so they're small and fast. They still pull SDL2 for headers
# only: img_format.h includes document.h which includes SDL.h. SDL_MAIN_HANDLED
# keeps SDL from redefining main(). Add new pure-logic modules here as the
# overlay split proceeds.
foreach(pure_test palette_math color_ops sprite_resize_ops anipoint anipoint_mirror
                  tbl_diff img_util body_split gif_trim)
    add_executable(${pure_test}_test
        test/${pure_test}_test.cpp
        platform/${pure_test}.cpp
    )
    target_include_directories(${pure_test}_test PRIVATE platform)
    target_link_libraries(${pure_test}_test PRIVATE SDL2::SDL2)
    if(MSVC)
        target_compile_options(${pure_test}_test PRIVATE /W3)
        target_compile_definitions(${pure_test}_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
    else()
        target_compile_definitions(${pure_test}_test PRIVATE SDL_MAIN_HANDLED)
    endif()
    add_test(NAME ${pure_test} COMMAND ${pure_test}_test)
endforeach()

# image_ops depends on palette_math, so it links both modules.
add_executable(image_ops_test
    test/image_ops_test.cpp
    platform/image_ops.cpp
    platform/palette_math.cpp
)
target_include_directories(image_ops_test PRIVATE platform)
target_link_libraries(image_ops_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(image_ops_test PRIVATE /W3)
    target_compile_definitions(image_ops_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(image_ops_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME image_ops COMMAND image_ops_test)

# canvas_ops reads palette words when planning a cross-palette paste, so it
# links palette_math alongside.
add_executable(canvas_ops_test
    test/canvas_ops_test.cpp
    platform/canvas_ops.cpp
    platform/palette_math.cpp
)
target_include_directories(canvas_ops_test PRIVATE platform)
target_link_libraries(canvas_ops_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(canvas_ops_test PRIVATE /W3)
    target_compile_definitions(canvas_ops_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(canvas_ops_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME canvas_ops COMMAND canvas_ops_test)

# mk2_roundtrip_test: load + idempotent round-trip check on a real MKSTK.ASM.
# Only built when MK2_MKSTK_PATH is set (so a fresh checkout without the mk2
# source tree doesn't fail to configure). Run after build with:
#   ctest --output-on-failure
# or directly:
#   build\test\Release\mk2_roundtrip_test path\to\MKSTK.ASM
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
