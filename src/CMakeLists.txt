cmake_minimum_required(VERSION 3.25)
project(H4CN VERSION 3.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# heroes4.exe is 32-bit and every address in game_addrs.h belongs to that image:
# this target must never be built for x64. The presets pin x86.
add_library(H4CN SHARED
    blit.cc
    blit.h
    config.cc
    config.h
    diagnostics.cc
    diagnostics.h
    font_cache.cc
    font_cache.h
    game_addrs.h
    game_types.h
    hooks.cc
    hooks.h
    inline_hook.cc
    inline_hook.h
    main.cc
    wrap.cc
    wrap.h
)

# The startup line in H4CN.log carries this; a stale-looking version string in a
# player's log is the cheapest way to spot an outdated deployment.
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

target_compile_definitions(H4CN PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# generated/ carries version.h; ../deps carries the single-header toml++ that
# only config.cc includes.
target_include_directories(H4CN PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps
)

if(MSVC)
    target_compile_options(H4CN PRIVATE /W4 /permissive-)
endif()

# gdi32 renders the glyphs, user32 hands out the screen DC (GetDC/ReleaseDC) and
# kernel32 provides the memory and code-patching APIs. Nothing else: the blitter
# is hand-written, so no USER32 drawing call sneaks in.
target_link_libraries(H4CN PRIVATE
    gdi32
    kernel32
    user32
)

set_target_properties(H4CN PROPERTIES
    OUTPUT_NAME "H4CN"
    SUFFIX ".asi"
    PREFIX ""
    DEBUG_POSTFIX ""
)

# Host unit tests for the pure line-breaking logic. wrap.cc measures through
# the Advancer interface, so this target links neither GDI nor the game code;
# it is a console exe, never part of the shipped .asi.
option(H4CN_BUILD_TESTS "Build the WrapText host tests (console exe)" OFF)
if(H4CN_BUILD_TESTS)
    enable_testing()
    add_executable(wrap_tests
        tests/wrap_test.cc
        wrap.cc
        wrap.h
    )
    target_compile_definitions(wrap_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(wrap_tests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME wrap_tests COMMAND wrap_tests)

    # Pure half of the config layer: ParseConfig/Resolve against in-memory
    # TOML text. config.cc still drags in diagnostics.cc (file reads are not
    # exercised), which needs no libraries beyond the defaults.
    add_executable(config_tests
        tests/config_test.cc
        config.cc
        diagnostics.cc
    )
    target_include_directories(config_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../deps
    )
    target_compile_definitions(config_tests PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(config_tests PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME config_tests COMMAND config_tests)
endif()

# Optional: copy the plugin into the ASI loader's scan folder after every build.
# The loader's version.ini sets LoadFromScriptsOnly=1, so that folder is
# <game>\plugins - not the game directory itself.
#   ./build.ps1 -Config Release -DeployDir 'D:\Games\Heroes4\plugins'
set(H4CN_DEPLOY_DIR "" CACHE PATH
    "ASI plugin directory (usually <game>\\plugins) to copy H4CN.asi into (empty = do not copy)")
if(H4CN_DEPLOY_DIR)
    add_custom_command(TARGET H4CN POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:H4CN>" "${H4CN_DEPLOY_DIR}/H4CN.asi"
        COMMENT "Deploying H4CN.asi to ${H4CN_DEPLOY_DIR}"
        VERBATIM
    )
endif()
