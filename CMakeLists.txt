cmake_minimum_required(VERSION 3.25)

project(risen_enhanced_graphics VERSION 0.1.0 LANGUAGES C CXX)

# Risen Enhanced is a 64-bit Windows game; the renderer is a DLL loaded into it.
if(NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "risen-enhanced-graphics targets 64-bit Windows only (Risen Enhanced is x64).")
endif()

option(RVK_BUILD_TESTS "Build the unit tests" ON)
if(RVK_BUILD_TESTS)
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
endif()
# debug info inside the objects (/Z7): no mspdbsrv contention between parallel cl.exe; the linker still writes the .pdb
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

# vcpkg only builds Debug and Release
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "Release;")  # "" = imported targets without per-config locations (FindVulkan tools)
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "Release;")

find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glslang CONFIG REQUIRED)
find_package(SPIRV-Tools CONFIG REQUIRED)

function(rvk_warnings target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
endfunction()

# ---------------------------------------------------------------------------------------------
# rvk_core: everything except the DLL entry points (linked into the DLL and the tests)
# ---------------------------------------------------------------------------------------------
add_library(rvk_core STATIC
    src/core/config.cpp
    src/core/config.h
    src/core/exports.cpp
    src/core/exports.h
    src/core/hooks.cpp
    src/core/hooks.h
    src/core/log.cpp
    src/core/log.h
    src/core/paths.cpp
    src/core/paths.h
    src/engine/symbols.cpp
    src/engine/symbols.h
    src/hal/api_generated.h
    src/hal/engine_alloc.cpp
    src/hal/trace.cpp
    src/hal/trace.h
    src/hal/trace_generated.h
    src/hal/trace_support.h
    src/hal/types_base.h
    src/shader/abi.h
    src/shader/glsl.cpp
    src/shader/glsl.h
    src/shader/sm_decode.cpp
    src/shader/sm_disasm.cpp
    src/shader/sm_ir.h
    src/shader/sm_tokens.cpp
    src/shader/sm_tokens.h
    src/shader/spirv.cpp
    src/shader/spirv.h
    src/vk/probe.cpp
    src/vk/probe.h
    src/vk/volk_impl.c
)
target_include_directories(rvk_core PUBLIC src)
# volk is compiled here (not vcpkg's volk.lib): it needs VK_USE_PLATFORM_WIN32_KHR for the Win32 surface entry points
target_compile_definitions(rvk_core PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(rvk_core PUBLIC
    spdlog::spdlog
    minhook::minhook
    tomlplusplus::tomlplusplus
    volk::volk_headers
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    glslang::glslang
    glslang::SPIRV
    glslang::glslang-default-resource-limits
)
rvk_warnings(rvk_core)
target_precompile_headers(rvk_core PRIVATE
    <algorithm> <array> <cstdint> <filesystem> <memory> <optional> <span> <string> <string_view> <vector>
    <spdlog/spdlog.h>
)
set_source_files_properties(src/vk/volk_impl.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# ---------------------------------------------------------------------------------------------
# d3d9.dll: the proxy the game loads (Engine.dll imports d3d9.dll!Direct3DCreate9)
# ---------------------------------------------------------------------------------------------
add_library(rvk_d3d9 SHARED
    src/proxy/dllmain.cpp
    src/proxy/d3d9_exports.cpp
    src/proxy/d3d9_exports.h
    src/proxy/d3d9.def
    src/bootstrap.cpp
    src/bootstrap.h
)
set_target_properties(rvk_d3d9 PROPERTIES OUTPUT_NAME d3d9)
target_link_libraries(rvk_d3d9 PRIVATE rvk_core)
rvk_warnings(rvk_d3d9)

# ---------------------------------------------------------------------------------------------
# rvk_shadertool: offline shader translator driver (corpus validation, listings)
# ---------------------------------------------------------------------------------------------
add_executable(rvk_shadertool tools/shadertool/main.cpp)
target_link_libraries(rvk_shadertool PRIVATE rvk_core SPIRV-Tools-static)
rvk_warnings(rvk_shadertool)

# ---------------------------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------------------------
if(RVK_BUILD_TESTS)
    add_executable(rvk_tests tests/test.h tests/test_main.cpp tests/shader_tests.cpp)
    target_link_libraries(rvk_tests PRIVATE rvk_core SPIRV-Tools-static)
    rvk_warnings(rvk_tests)
    add_test(NAME rvk_tests COMMAND rvk_tests)
endif()
