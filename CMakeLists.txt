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
    platform/paint_tools.cpp
    platform/ui_modals.cpp
    platform/ui_undo.cpp
    platform/ui_autochop.cpp
    platform/ui_reactions.cpp
    platform/ui_browse.cpp
    platform/reaction_class.cpp
    platform/strike_bind.cpp
    platform/ui_bodysplit.cpp
    platform/body_split.cpp
    platform/digitize_import.cpp
    platform/video_decode.cpp
    platform/ui_video_import.cpp
    platform/digitize_matte.cpp
    platform/material_mask.cpp
    platform/palette_ramp.cpp
    platform/ramp_remap.cpp
    platform/sprite_gradient.cpp
    platform/palette_transfer.cpp
    platform/palette_layout.cpp
    platform/ui_ramp_transfer.cpp
    platform/ui_alt_costume.cpp
    platform/ui_palette_preview.cpp
    platform/world_render.cpp
    platform/bdd_bg.cpp
    platform/anipoint.cpp
    platform/anipoint_edit.cpp
    platform/anipoint_level.cpp
    platform/anipoint_mirror.cpp
    platform/tbl_diff.cpp
    platform/img_util.cpp
    platform/color_ops.cpp
    platform/image_ops.cpp
    platform/canvas_ops.cpp
    platform/stamp_erase.cpp
    platform/subframe_align.cpp
    platform/ui_stamp_erase.cpp
    platform/gif_trim.cpp
    platform/palette_math.cpp
    platform/dma_pack.cpp
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

# The video importer decodes on a worker thread (ui_video_import.cpp).
find_package(Threads REQUIRED)
target_link_libraries(imgtool PRIVATE Threads::Threads)
target_link_libraries(imgtool-cli PRIVATE Threads::Threads)

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

# ---- Studio: independent editor, shared format and math code -----------------
add_library(imgtool_studio_document STATIC
    platform/studio/studio_document.cpp platform/studio/studio_img_codec.cpp
    platform/studio/studio_animation.cpp
    platform/studio/studio_editing.cpp
    platform/studio/studio_selection.cpp
    platform/studio/studio_selection_transform.cpp
    platform/studio/studio_filters.cpp
    platform/studio/studio_repair.cpp
    platform/studio/studio_batch.cpp
    platform/studio/studio_batch_rename.cpp
    platform/studio/studio_batch_palette.cpp
    platform/studio/studio_batch_delete.cpp
    platform/studio/studio_sprite_scope.cpp
    platform/studio/studio_library_copy.cpp
    platform/studio/studio_restore.cpp
    platform/studio/studio_lod.cpp
    platform/studio/studio_export.cpp
    platform/studio/studio_variant.cpp
    platform/studio/studio_paint.cpp
    platform/studio/studio_regions.cpp platform/body_split.cpp
    platform/studio/studio_project.cpp
    platform/studio/studio_stage.cpp
    platform/studio/studio_anchor_link.cpp
    platform/studio/studio_points.cpp
    platform/studio/studio_tbl_compare.cpp
    platform/studio/studio_strikes.cpp platform/mk2_hitbox.cpp
    platform/studio/studio_asm.cpp platform/strike_bind.cpp
    platform/studio/studio_bindings.cpp
    platform/studio/studio_strike_phases.cpp
    platform/studio/studio_animation_copy.cpp
    platform/studio/studio_animation_clipboard.cpp
    platform/studio/studio_animation_split.cpp
    platform/studio/studio_sequences.cpp
    platform/studio/studio_palette.cpp platform/color_ops.cpp
    platform/studio/studio_palette_merge.cpp
    platform/studio/studio_palette_cleanup.cpp
    platform/studio/studio_palette_compaction.cpp
    platform/studio/studio_palette_ramps.cpp platform/studio/studio_file_io.cpp
    platform/studio/studio_import.cpp platform/studio/studio_legacy_media.cpp platform/studio/studio_sheet.cpp
    platform/studio/studio_digitize.cpp platform/digitize_matte.cpp
    platform/studio/studio_digitize_job.cpp
    platform/studio/studio_video.cpp platform/video_decode.cpp
    platform/material_mask.cpp platform/palette_ramp.cpp platform/ramp_remap.cpp
    platform/document.cpp platform/img_io.cpp platform/globals.c
    platform/anipoint.cpp platform/anipoint_mirror.cpp platform/img_util.cpp
    platform/load2_verify.cpp platform/palette_math.cpp platform/tbl_diff.cpp
    platform/gif_trim.cpp platform/canvas_ops.cpp platform/sprite_resize_ops.cpp)
target_include_directories(imgtool_studio_document PUBLIC platform platform/studio)
target_compile_features(imgtool_studio_document PUBLIC cxx_std_17)
target_link_libraries(imgtool_studio_document PUBLIC SDL2::SDL2 Threads::Threads)
if(MSVC)
    target_compile_definitions(imgtool_studio_document PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(imgtool_studio_document PUBLIC /utf-8)
    target_compile_options(imgtool_studio_document PRIVATE "$<$<CONFIG:Release>:/Zi>")
endif()

add_executable(imgtool-studio
    platform/studio/studio_main.cpp platform/studio/studio_theme.cpp
    imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_sdl2.cpp imgui/backends/imgui_impl_sdlrenderer2.cpp
    ${PLATFORM_RC})
target_include_directories(imgtool-studio PRIVATE imgui imgui/backends)
target_link_libraries(imgtool-studio PRIVATE imgtool_studio_document SDL2::SDL2main)
target_compile_definitions(imgtool-studio PRIVATE IMGTOOL_VERSION="${IMGTOOL_VERSION}")
if(WIN32)
    set_target_properties(imgtool-studio PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(imgtool-studio PRIVATE comdlg32 dbghelp)
    add_custom_command(TARGET imgtool-studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:imgtool-studio>"
        VERBATIM)
endif()
# Share the existing icon family with Studio, relative to the executable.
add_custom_command(TARGET imgtool-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:imgtool-studio>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/assets/MaterialSymbolsSharp-Regular.ttf"
        "$<TARGET_FILE_DIR:imgtool-studio>/assets/MaterialSymbolsSharp-Regular.ttf"
    VERBATIM)
if(MSVC)
    target_compile_options(imgtool-studio PRIVATE /W3)
    target_compile_options(imgtool-studio PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_link_options(imgtool-studio PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>" "$<$<CONFIG:Release>:/INCREMENTAL:NO>")
    target_compile_definitions(imgtool-studio PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- Tests -----------------------------------------------------------------
enable_testing()
if(WIN32)
    add_executable(studio_startup_test test/studio_startup_test.cpp)
    target_link_libraries(studio_startup_test PRIVATE SDL2::SDL2 dbghelp)
    target_compile_features(studio_startup_test PRIVATE cxx_std_17)
    target_compile_definitions(studio_startup_test PRIVATE SDL_MAIN_HANDLED NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(studio_startup_test PRIVATE /utf-8)
    endif()
    add_test(NAME studio_startup COMMAND studio_startup_test)
endif()
add_executable(studio_document_test test/studio_document_test.cpp)
target_link_libraries(studio_document_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_document_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_document COMMAND studio_document_test)
add_executable(studio_sheet_test test/studio_sheet_test.cpp)
target_link_libraries(studio_sheet_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_sheet_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_sheet COMMAND studio_sheet_test)
add_executable(studio_stage_test test/studio_stage_test.cpp platform/bdd_bg.cpp)
target_link_libraries(studio_stage_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_stage_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_stage COMMAND studio_stage_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data")
add_executable(studio_anchor_link_test test/studio_anchor_link_test.cpp)
target_link_libraries(studio_anchor_link_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_anchor_link_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_anchor_link COMMAND studio_anchor_link_test)
add_executable(studio_points_test test/studio_points_test.cpp)
target_link_libraries(studio_points_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_points_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_points COMMAND studio_points_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAVEYES.IMG")
add_executable(studio_tbl_compare_test test/studio_tbl_compare_test.cpp)
target_link_libraries(studio_tbl_compare_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_tbl_compare_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_tbl_compare COMMAND studio_tbl_compare_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main")
add_executable(studio_strikes_test test/studio_strikes_test.cpp)
target_link_libraries(studio_strikes_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_strikes_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_strikes COMMAND studio_strikes_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/src/MKSTK.ASM")
add_executable(studio_asm_test test/studio_asm_test.cpp)
target_link_libraries(studio_asm_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_asm_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_asm COMMAND studio_asm_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/src/MKJC.ASM" "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_bindings_test test/studio_bindings_test.cpp)
target_link_libraries(studio_bindings_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_bindings_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_bindings COMMAND studio_bindings_test)
add_executable(studio_strike_phases_test test/studio_strike_phases_test.cpp)
target_link_libraries(studio_strike_phases_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_strike_phases_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_strike_phases COMMAND studio_strike_phases_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/src/MKSTK.ASM")
add_executable(studio_legacy_media_test test/studio_legacy_media_test.cpp)
target_link_libraries(studio_legacy_media_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_legacy_media_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_legacy_media COMMAND studio_legacy_media_test)
add_executable(studio_legacy_img_test test/studio_legacy_img_test.cpp)
target_link_libraries(studio_legacy_img_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_legacy_img_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_legacy_img COMMAND studio_legacy_img_test)
add_executable(studio_animation_test test/studio_animation_test.cpp)
target_link_libraries(studio_animation_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_animation_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_animation COMMAND studio_animation_test)
add_executable(studio_editing_test test/studio_editing_test.cpp)
target_link_libraries(studio_editing_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_editing_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_editing COMMAND studio_editing_test)
add_executable(studio_project_test test/studio_project_test.cpp)
target_link_libraries(studio_project_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_project_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_project COMMAND studio_project_test)
add_executable(studio_motion_test test/studio_motion_test.cpp)
target_link_libraries(studio_motion_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_motion_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_motion COMMAND studio_motion_test)
add_executable(studio_trigger_test test/studio_trigger_test.cpp)
target_link_libraries(studio_trigger_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_trigger_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_trigger COMMAND studio_trigger_test)
add_executable(studio_sequences_test test/studio_sequences_test.cpp)
target_link_libraries(studio_sequences_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_sequences_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_sequences COMMAND studio_sequences_test)
add_executable(studio_palette_test test/studio_palette_test.cpp)
target_link_libraries(studio_palette_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_palette_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_palette COMMAND studio_palette_test)
add_executable(studio_palette_merge_test test/studio_palette_merge_test.cpp)
target_link_libraries(studio_palette_merge_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_palette_merge_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_palette_merge COMMAND studio_palette_merge_test)
add_executable(studio_palette_compaction_test test/studio_palette_compaction_test.cpp)
target_link_libraries(studio_palette_compaction_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_palette_compaction_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_palette_compaction COMMAND studio_palette_compaction_test)
add_executable(studio_palette_ramps_test test/studio_palette_ramps_test.cpp)
target_link_libraries(studio_palette_ramps_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_palette_ramps_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_palette_ramps COMMAND studio_palette_ramps_test)
add_executable(studio_import_test test/studio_import_test.cpp)
target_link_libraries(studio_import_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_import_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_import COMMAND studio_import_test)
add_executable(studio_digitize_test test/studio_digitize_test.cpp)
target_link_libraries(studio_digitize_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_digitize_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_digitize COMMAND studio_digitize_test)
add_executable(studio_digitize_job_test test/studio_digitize_job_test.cpp)
target_link_libraries(studio_digitize_job_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_digitize_job_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_digitize_job COMMAND studio_digitize_job_test)
add_executable(studio_selection_test test/studio_selection_test.cpp)
target_link_libraries(studio_selection_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_selection_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_selection COMMAND studio_selection_test)
add_executable(studio_regions_test test/studio_regions_test.cpp)
target_link_libraries(studio_regions_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_regions_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_regions COMMAND studio_regions_test)
add_executable(studio_selection_transform_test test/studio_selection_transform_test.cpp)
target_link_libraries(studio_selection_transform_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_selection_transform_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_selection_transform COMMAND studio_selection_transform_test)
add_executable(studio_paint_test test/studio_paint_test.cpp)
target_link_libraries(studio_paint_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_paint_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_paint COMMAND studio_paint_test)
add_executable(studio_filters_test test/studio_filters_test.cpp)
target_link_libraries(studio_filters_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_filters_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_filters COMMAND studio_filters_test)
add_executable(studio_repair_test test/studio_repair_test.cpp)
target_link_libraries(studio_repair_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_repair_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_repair COMMAND studio_repair_test)
add_executable(studio_batch_test test/studio_batch_test.cpp)
target_link_libraries(studio_batch_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_batch_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_batch COMMAND studio_batch_test)
add_executable(studio_batch_rename_test test/studio_batch_rename_test.cpp)
target_link_libraries(studio_batch_rename_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_batch_rename_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_batch_rename COMMAND studio_batch_rename_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_batch_palette_test test/studio_batch_palette_test.cpp)
target_link_libraries(studio_batch_palette_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_batch_palette_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_batch_palette COMMAND studio_batch_palette_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_batch_delete_test test/studio_batch_delete_test.cpp)
target_link_libraries(studio_batch_delete_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_batch_delete_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_batch_delete COMMAND studio_batch_delete_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_palette_cleanup_test test/studio_palette_cleanup_test.cpp)
target_link_libraries(studio_palette_cleanup_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_palette_cleanup_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_palette_cleanup COMMAND studio_palette_cleanup_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_sprite_scope_test test/studio_sprite_scope_test.cpp)
target_link_libraries(studio_sprite_scope_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_sprite_scope_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_sprite_scope COMMAND studio_sprite_scope_test)
add_executable(studio_sprite_clipboard_test test/studio_sprite_clipboard_test.cpp)
target_link_libraries(studio_sprite_clipboard_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_sprite_clipboard_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_sprite_clipboard COMMAND studio_sprite_clipboard_test)
add_executable(studio_library_split_test test/studio_library_split_test.cpp)
target_link_libraries(studio_library_split_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_library_split_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_library_split COMMAND studio_library_split_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_animation_copy_test test/studio_animation_copy_test.cpp)
target_link_libraries(studio_animation_copy_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_animation_copy_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_animation_copy COMMAND studio_animation_copy_test)
add_executable(studio_animation_artwork_test test/studio_animation_artwork_test.cpp)
target_link_libraries(studio_animation_artwork_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_animation_artwork_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_animation_artwork COMMAND studio_animation_artwork_test)
add_executable(studio_animation_clipboard_test test/studio_animation_clipboard_test.cpp)
target_link_libraries(studio_animation_clipboard_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_animation_clipboard_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_animation_clipboard COMMAND studio_animation_clipboard_test)
add_executable(studio_animation_split_test test/studio_animation_split_test.cpp)
target_link_libraries(studio_animation_split_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_animation_split_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_animation_split COMMAND studio_animation_split_test)
add_executable(studio_library_copy_test test/studio_library_copy_test.cpp)
target_link_libraries(studio_library_copy_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_library_copy_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_library_copy COMMAND studio_library_copy_test "${CMAKE_CURRENT_SOURCE_DIR}/../mk2-main/data/CAGE1.IMG")
add_executable(studio_export_test test/studio_export_test.cpp)
target_link_libraries(studio_export_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_export_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_export COMMAND studio_export_test)
add_executable(studio_lod_test test/studio_lod_test.cpp)
target_link_libraries(studio_lod_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_lod_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_lod COMMAND studio_lod_test)
add_executable(studio_restore_test test/studio_restore_test.cpp)
target_link_libraries(studio_restore_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_restore_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_restore COMMAND studio_restore_test)
add_executable(studio_variant_test test/studio_variant_test.cpp)
target_link_libraries(studio_variant_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_variant_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_variant COMMAND studio_variant_test)
add_executable(studio_clone_test test/studio_clone_test.cpp)
target_link_libraries(studio_clone_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_clone_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_clone COMMAND studio_clone_test)
add_executable(studio_video_test test/studio_video_test.cpp)
target_link_libraries(studio_video_test PRIVATE imgtool_studio_document)
target_compile_definitions(studio_video_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME studio_video COMMAND studio_video_test)
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
foreach(pure_test palette_math color_ops sprite_resize_ops anipoint anipoint_level
                  anipoint_mirror tbl_diff img_util body_split gif_trim dma_pack
                  stamp_erase bdd_bg document reaction_class strike_bind
                  digitize_matte palette_ramp material_mask sprite_gradient
                  palette_transfer palette_layout video_decode)
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

# subframe_align rides on stamp_erase's matcher, so it needs two sources and
# cannot use the one-source foreach above.
add_executable(subframe_align_test
    test/subframe_align_test.cpp
    platform/subframe_align.cpp
    platform/stamp_erase.cpp
)
target_include_directories(subframe_align_test PRIVATE platform)
target_link_libraries(subframe_align_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(subframe_align_test PRIVATE /W3)
    target_compile_definitions(subframe_align_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(subframe_align_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME subframe_align COMMAND subframe_align_test)

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

# ramp_remap calls RampColorWord, so it links palette_ramp too.
add_executable(ramp_remap_test
    test/ramp_remap_test.cpp
    platform/ramp_remap.cpp
    platform/palette_ramp.cpp
)
target_include_directories(ramp_remap_test PRIVATE platform)
target_link_libraries(ramp_remap_test PRIVATE SDL2::SDL2)
if(MSVC)
    target_compile_options(ramp_remap_test PRIVATE /W3)
    target_compile_definitions(ramp_remap_test PRIVATE _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
else()
    target_compile_definitions(ramp_remap_test PRIVATE SDL_MAIN_HANDLED)
endif()
add_test(NAME ramp_remap COMMAND ramp_remap_test)

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
