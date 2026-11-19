cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(BeatTheBeatQilinComp VERSION 0.15.0 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The DOL loader: runtime code too (no std::format) ----
add_library(qilin_dol STATIC recomp/src/dol.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_dol PUBLIC recomp/src)
target_link_libraries(qilin_dol PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/dol_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/lifter.cpp
    recomp/src/hwaccess.cpp
    recomp/src/libraries.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_dol PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Guest runtime (header-only) and the floating-point rules lifted code needs ----
# Lifted code must be bit-exact: no multiply-add contraction and no fast-math.
add_library(qilin_runtime INTERFACE)
target_include_directories(qilin_runtime INTERFACE recomp/runtime)
if(MSVC)
    target_compile_options(qilin_runtime INTERFACE /fp:precise)
else()
    target_compile_options(qilin_runtime INTERFACE -ffp-contract=off -fno-fast-math)
endif()

# Lifted code from `qilin lift` (needs your own main.dol):
#   qilin lift main.dol --out gen && cmake -B build -DQILIN_GEN_DIR=$PWD/gen
# qilin_difftest runs it against the original code (tools/difftest), with
# checked memory: an access outside the mapped guest memory stops.
set(QILIN_GEN_DIR "" CACHE PATH "qilin lift output directory")
# The differential test under UBSan: any undefined behaviour in lifted code
# or the runtime stops the harness (and so the test) with the report.
# MSVC has no UBSan; on Windows, tools/difftest/build_ubsan.py builds the
# harness with zig's Clang instead.
option(QILIN_UBSAN "Build lifted code and qilin_difftest with UBSan (GCC or Clang)" OFF)
if(QILIN_UBSAN AND MSVC)
    message(FATAL_ERROR "QILIN_UBSAN needs GCC or Clang; on Windows use tools/difftest/build_ubsan.py")
endif()
if(QILIN_GEN_DIR)
    file(GLOB QILIN_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_library(qilin_gen_checked STATIC ${QILIN_GEN_SOURCES} recomp/runtime/dispatch.cpp recomp/runtime/timebase.cpp)
    target_include_directories(qilin_gen_checked PUBLIC "${QILIN_GEN_DIR}")
    target_compile_definitions(qilin_gen_checked PUBLIC QILIN_CHECKED_MEMORY QILIN_DETERMINISTIC_TIME)
    target_link_libraries(qilin_gen_checked PUBLIC qilin_runtime PRIVATE qilin_warnings)
    if(MSVC)
        # C4702 (unreachable code) comes from MSVC's optimiser after inlining,
        # e.g. after a call to a lifted function that always stops: it follows
        # inlining decisions, not anything the lifter can see or avoid.
        target_compile_options(qilin_gen_checked PRIVATE /wd4702)
    endif()
    add_executable(qilin_difftest tools/difftest/harness.cpp)
    target_link_libraries(qilin_difftest PRIVATE qilin_gen_checked qilin_dol qilin_warnings)
    qilin_tool(qilin_difftest)
    if(QILIN_UBSAN)
        foreach(t qilin_gen_checked qilin_difftest qilin_dol)
            target_compile_options(${t} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
        endforeach()
        target_link_options(qilin_difftest PRIVATE -fsanitize=undefined)
    endif()
endif()

# The game: qilin_run, from `qilin lift --runtime` output (a second directory:
# the difftest's lifts every library, this one calls native code for the
# replaced ones).
#   qilin lift main.dol --out gen-rt --runtime --symbols pal_symbols.txt
#   cmake -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt
# Checked memory for now (bring-up): an access outside the memory lifted code
# may use stops with the address and the instruction.
set(QILIN_RUNTIME_GEN_DIR "" CACHE PATH "qilin lift --runtime output directory")
if(QILIN_RUNTIME_GEN_DIR)
    file(GLOB QILIN_RUNTIME_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_RUNTIME_GEN_DIR}/*.cpp")
    add_library(qilin_gen_runtime STATIC ${QILIN_RUNTIME_GEN_SOURCES} recomp/runtime/dispatch.cpp recomp/runtime/timebase.cpp)
    target_include_directories(qilin_gen_runtime PUBLIC "${QILIN_RUNTIME_GEN_DIR}")
    # QILIN_APPROX_ESTIMATES: frsqrte/fres approximated where Broadway's table
    # is needed (guest.hpp); the difftest's build keeps stopping there.
    target_compile_definitions(qilin_gen_runtime PUBLIC QILIN_CHECKED_MEMORY QILIN_APPROX_ESTIMATES)
    target_link_libraries(qilin_gen_runtime PUBLIC qilin_runtime PRIVATE qilin_warnings)
    if(MSVC)
        target_compile_options(qilin_gen_runtime PRIVATE /wd4702)  # as for qilin_gen_checked
    endif()
    # The native functions register themselves from static initialisers, so
    # they are the executable's own sources: a static library would drop them.
    add_executable(qilin_run
        recomp/runtime/run.cpp
        recomp/runtime/machine.cpp
        recomp/runtime/native.cpp
        recomp/runtime/os.cpp
        recomp/runtime/threads.cpp
        recomp/runtime/interrupts.cpp
        recomp/runtime/dvd.cpp
        recomp/runtime/gx.cpp
        recomp/runtime/gx_fifo.cpp
        recomp/runtime/vi.cpp
        recomp/runtime/wpad.cpp
        recomp/runtime/input.cpp
        recomp/runtime/audio.cpp
        recomp/runtime/nand.cpp
    )
    target_link_libraries(qilin_run PRIVATE qilin_gen_runtime qilin_dol qilin_warnings)
    if(WIN32)
        target_link_libraries(qilin_run PRIVATE winmm)  # timeBeginPeriod: 1 ms timers for the audio clock
    endif()

    # The host side: a window, Vulkan, audio and the keyboard. Third-party
    # code is fetched at configure time, pinned by hash, and never committed:
    # SDL3 (zlib licence; a system SDL3 is used if there is one) and Khronos'
    # Vulkan headers (Apache-2.0; the loader comes through SDL, so no Vulkan
    # SDK is needed). The shaders' SPIR-V is committed beside their GLSL
    # (recomp/runtime/shaders/spirv, our own code compiled); a stamp of the
    # GLSL's hashes stops the configure if they drift apart, and
    # QILIN_BUILD_SHADERS=ON fetches glslang (BSD-3/Apache-2.0) to regenerate
    # them. QILIN_HEADLESS_ONLY builds without any of it (QILIN_HEADLESS=1
    # also runs a full build without a window).
    option(QILIN_HEADLESS_ONLY "Build qilin_run without a window, Vulkan or audio" OFF)
    if(QILIN_HEADLESS_ONLY)
        target_compile_definitions(qilin_run PRIVATE QILIN_HEADLESS_ONLY)
    else()
        include(FetchContent)
        find_package(SDL3 CONFIG QUIET)
        if(NOT SDL3_FOUND)
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(SDL3
                URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.20.tar.gz
                URL_HASH SHA256=5cb56d4223a41283f3c2baf6f1263fba171692409ae405a50417a7c8e7e28deb)
            FetchContent_MakeAvailable(SDL3)
        endif()
        FetchContent_Declare(VulkanHeaders
            URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.299.tar.gz
            URL_HASH SHA256=95bb8fe731469b9e1be532b9d3b4d7d33e28ddbd99a926da7f0eca82a134b92f)
        FetchContent_MakeAvailable(VulkanHeaders)
        # The shaders: GLSL in recomp/runtime/shaders, SPIR-V headers
        # (uint32_t arrays) in its spirv folder, and the GLSL's SHA-256
        # (line endings normalised) in spirv/sources.sha256.
        set(QILIN_SHADERS gx.vert gx.frag copy.vert copy.frag)
        set(QILIN_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime/shaders")
        set(QILIN_SHADER_DIR "${QILIN_SHADER_SRC}/spirv")
        set(stamp "")
        foreach(shader ${QILIN_SHADERS})
            file(READ "${QILIN_SHADER_SRC}/${shader}" text)
            string(REPLACE "\r" "" text "${text}")
            string(SHA256 hash "${text}")
            string(APPEND stamp "${hash}  ${shader}\n")
            # an edited shader re-runs the configure, so the check (or the stamp) follows it
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${QILIN_SHADER_SRC}/${shader}")
        endforeach()
        option(QILIN_BUILD_SHADERS "Fetch glslang and regenerate the committed SPIR-V from the GLSL" OFF)
        if(QILIN_BUILD_SHADERS)
            set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
            set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
            set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
            set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
            set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
            set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(glslang
                URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
                URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
            FetchContent_MakeAvailable(glslang)
            set(QILIN_SHADER_HEADERS "")
            foreach(shader ${QILIN_SHADERS})
                string(REPLACE "." "_" name "${shader}")
                set(out "${QILIN_SHADER_DIR}/${name}.h")
                add_custom_command(OUTPUT "${out}"
                    COMMAND glslang-standalone -V --target-env vulkan1.3 --vn "k_${name}" -o "${out}" "${QILIN_SHADER_SRC}/${shader}"
                    DEPENDS "${QILIN_SHADER_SRC}/${shader}" glslang-standalone
                    VERBATIM)
                list(APPEND QILIN_SHADER_HEADERS "${out}")
            endforeach()
            file(WRITE "${QILIN_SHADER_DIR}/sources.sha256" "${stamp}")
            add_custom_target(qilin_shaders DEPENDS ${QILIN_SHADER_HEADERS})
            add_dependencies(qilin_run qilin_shaders)
        else()
            file(READ "${QILIN_SHADER_DIR}/sources.sha256" committed)
            string(REPLACE "\r" "" committed "${committed}")
            if(NOT committed STREQUAL stamp)
                message(FATAL_ERROR "The shaders' GLSL changed since their SPIR-V was generated: "
                                    "configure once with -DQILIN_BUILD_SHADERS=ON and commit recomp/runtime/shaders/spirv")
            endif()
        endif()

        target_sources(qilin_run PRIVATE recomp/runtime/host.cpp recomp/runtime/gx_vulkan.cpp recomp/runtime/gx_texture.cpp)
        target_include_directories(qilin_run PRIVATE "${QILIN_SHADER_DIR}")
        target_link_libraries(qilin_run PRIVATE SDL3::SDL3 Vulkan::Headers)
    endif()
endif()

# ---- Tests (no game files needed) ----
enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp recomp/runtime/native.cpp recomp/runtime/interrupts.cpp recomp/runtime/timebase.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_runtime qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
