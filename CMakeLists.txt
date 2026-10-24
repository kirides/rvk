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
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIR stb_image_write.h REQUIRED)

function(rvk_warnings target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
endfunction()

# ---------------------------------------------------------------------------------------------
# Hand-written GLSL shaders (shaders/**, docs/research/injection.md 2): GLSL 4.60 -> SPIR-V 1.6 at build time by rvk_glslc
# (vcpkg glslang + SPIRV-Tools' validator: nothing beyond vcpkg needed), embedded into rvk_core as generated headers. Each
# command writes a depfile of the files it included, so editing shaders/include/*.glsl rebuilds what uses it.
# ---------------------------------------------------------------------------------------------
add_executable(rvk_glslc tools/glslc/main.cpp)
target_link_libraries(rvk_glslc PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits SPIRV-Tools-static)
rvk_warnings(rvk_glslc)
set_target_properties(rvk_glslc PROPERTIES FOLDER tools)

set(RVK_SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(RVK_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
# new shaders go here (path below shaders/ = the name shader::embedded::spirv() finds them by)
set(RVK_SHADERS
    debug/fullscreen.vert
    debug/inject_view.comp
    debug/inject_view.frag
    gi/gi_blend.comp
    gi/gi_trace.comp
    gi/sp_resolve.comp
    gi/sp_trace.comp
    gbuffer/ao_white.frag
    gbuffer/gbuffer_debug.frag
    gbuffer/gtao.comp
    upscale/prep.comp
    gbuffer/gtao_filter.comp
    occlusion/hzb_down.comp
    occlusion/probe.comp
    test/abi_layout.comp
    test/abi_pass_layout.comp
    test/abi_rt_layout.comp
    test/rt_query.comp
    rt/rt_debug.comp
    rt/rt_debug_depth.comp
    rt/rt_debug.frag
    shadows/shadow_mask.frag
    shadows/shadow_mask.vert
    water/ocean_evolve.comp
    water/ocean_fft.comp
    water/ocean_foam.comp
    water/ocean_resolve.comp
    water/ocean_spectrum.comp
    water/water.frag
    water/water_ocean.vert
    water/water_rt.comp
    water/wet_shore.frag
)
# variants: "<name>=<source>:<DEFINE>[,<DEFINE>[=<value>]]..." (the source compiled again with these defines, embedded under
# <name>; rvk_glslc -D); none at the moment
set(RVK_SHADER_VARIANTS)
set(RVK_SHADER_OUTPUTS)
set(RVK_SHADER_INCLUDES "")
set(RVK_SHADER_TABLE "")
foreach(shader ${RVK_SHADERS})
    string(MAKE_C_IDENTIFIER ${shader} ident)
    set(out ${RVK_GENERATED_DIR}/shaders/${shader}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND rvk_glslc compile ${RVK_SHADER_DIR}/${shader} -o ${out} --name ${ident} -I ${RVK_SHADER_DIR}/include --depfile ${out}.d
        DEPENDS rvk_glslc ${RVK_SHADER_DIR}/${shader}
        DEPFILE ${out}.d
        COMMENT "GLSL shaders/${shader}"
        VERBATIM)
    list(APPEND RVK_SHADER_OUTPUTS ${out})
    string(APPEND RVK_SHADER_INCLUDES "#include \"shaders/${shader}.h\"\n")
    string(APPEND RVK_SHADER_TABLE "    {\"${shader}\", data::${ident}},\n")
endforeach()
foreach(variant ${RVK_SHADER_VARIANTS})
    string(REGEX MATCH "^([^=]+)=([^:]+):(.+)$" _ ${variant})
    set(vname ${CMAKE_MATCH_1})
    set(vsrc ${CMAKE_MATCH_2})
    string(REPLACE "," ";" vdefs ${CMAKE_MATCH_3})
    set(vdefargs)
    foreach(d ${vdefs})
        list(APPEND vdefargs -D ${d})
    endforeach()
    string(MAKE_C_IDENTIFIER ${vname} ident)
    set(out ${RVK_GENERATED_DIR}/shaders/${vname}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND rvk_glslc compile ${RVK_SHADER_DIR}/${vsrc} -o ${out} --name ${ident} -I ${RVK_SHADER_DIR}/include --depfile ${out}.d ${vdefargs}
        DEPENDS rvk_glslc ${RVK_SHADER_DIR}/${vsrc}
        DEPFILE ${out}.d
        COMMENT "GLSL shaders/${vsrc} (${vdefs}) -> ${vname}"
        VERBATIM)
    list(APPEND RVK_SHADER_OUTPUTS ${out})
    string(APPEND RVK_SHADER_INCLUDES "#include \"shaders/${vname}.h\"\n")
    string(APPEND RVK_SHADER_TABLE "    {\"${vname}\", data::${ident}},\n")
endforeach()
# the ABI header as text: the D3D9 translator puts it into every translated shader
add_custom_command(
    OUTPUT ${RVK_GENERATED_DIR}/shaders/rvk_abi_glsl.h
    COMMAND rvk_glslc text ${RVK_SHADER_DIR}/include/rvk_abi.glsl -o ${RVK_GENERATED_DIR}/shaders/rvk_abi_glsl.h --name rvk_abi_glsl
    DEPENDS rvk_glslc ${RVK_SHADER_DIR}/include/rvk_abi.glsl
    COMMENT "GLSL ABI text shaders/include/rvk_abi.glsl"
    VERBATIM)
list(APPEND RVK_SHADER_OUTPUTS ${RVK_GENERATED_DIR}/shaders/rvk_abi_glsl.h)
add_custom_command(
    OUTPUT ${RVK_GENERATED_DIR}/shaders/rvk_gi_glsl.h
    COMMAND rvk_glslc text ${RVK_SHADER_DIR}/include/rvk_gi.glsl -o ${RVK_GENERATED_DIR}/shaders/rvk_gi_glsl.h --name rvk_gi_glsl
    DEPENDS rvk_glslc ${RVK_SHADER_DIR}/include/rvk_gi.glsl
    COMMENT "GLSL GI lookup text shaders/include/rvk_gi.glsl"
    VERBATIM)
list(APPEND RVK_SHADER_OUTPUTS ${RVK_GENERATED_DIR}/shaders/rvk_gi_glsl.h)
add_custom_command(
    OUTPUT ${RVK_GENERATED_DIR}/shaders/rvk_material_variant_glsl.h
    COMMAND rvk_glslc text ${RVK_SHADER_DIR}/include/rvk_material_variant.glsl -o ${RVK_GENERATED_DIR}/shaders/rvk_material_variant_glsl.h --name rvk_material_variant_glsl
    DEPENDS rvk_glslc ${RVK_SHADER_DIR}/include/rvk_material_variant.glsl
    COMMENT "GLSL material variant text shaders/include/rvk_material_variant.glsl"
    VERBATIM)
list(APPEND RVK_SHADER_OUTPUTS ${RVK_GENERATED_DIR}/shaders/rvk_material_variant_glsl.h)
# the GLSL material shader as text: the renderer assembles it with the engine's macros at run time (src/shader/uber.cpp)
foreach(utext ge_default.glsl ge_default.frag)
    string(MAKE_C_IDENTIFIER "${utext}" uident)
    add_custom_command(
        OUTPUT ${RVK_GENERATED_DIR}/shaders/uber_${uident}.h
        COMMAND rvk_glslc text ${RVK_SHADER_DIR}/material/${utext} -o ${RVK_GENERATED_DIR}/shaders/uber_${uident}.h --name uber_${uident}
        DEPENDS rvk_glslc ${RVK_SHADER_DIR}/material/${utext}
        COMMENT "GLSL material text shaders/material/${utext}"
        VERBATIM)
    list(APPEND RVK_SHADER_OUTPUTS ${RVK_GENERATED_DIR}/shaders/uber_${uident}.h)
endforeach()
configure_file(src/shader/embedded.cpp.in ${RVK_GENERATED_DIR}/embedded_shaders.cpp @ONLY)
file(GLOB RVK_SHADER_SOURCES CONFIGURE_DEPENDS ${RVK_SHADER_DIR}/*/*.glsl ${RVK_SHADER_DIR}/*/*.vert ${RVK_SHADER_DIR}/*/*.frag ${RVK_SHADER_DIR}/*/*.comp)
source_group(TREE ${RVK_SHADER_DIR} PREFIX shaders FILES ${RVK_SHADER_SOURCES})

# ---------------------------------------------------------------------------------------------
# rvk_core: everything except the DLL entry points (linked into the DLL and the tests)
# ---------------------------------------------------------------------------------------------
add_library(rvk_core STATIC
    include/rvk_api.h
    src/api/capture.cpp
    src/api/capture.h
    src/api/settings_api.cpp
    src/api/settings_api.h
    src/core/config.cpp
    src/core/config.h
    src/core/exports.cpp
    src/core/exports.h
    src/core/caps.cpp
    src/core/caps.h
    src/core/hooks.cpp
    src/core/hooks.h
    src/core/log.cpp
    src/core/log.h
    src/core/png.cpp
    src/core/png.h
    src/core/paths.cpp
    src/core/paths.h
    src/core/settings.cpp
    src/core/settings.h
    src/core/stall_sampler.cpp
    src/core/stall_sampler.h
    src/d3d9/convert.cpp
    src/d3d9/convert.h
    src/d3d9/defs.h
    src/d3d9/device.cpp
    src/d3d9/device.h
    src/d3d9/draw.cpp
    src/d3d9/draws.cpp
    src/d3d9/record.cpp
    src/d3d9/draws.h
    src/d3d9/ffshader.cpp
    src/d3d9/ffshader.h
    src/d3d9/fvf.cpp
    src/d3d9/fvf.h
    src/d3d9/identity.cpp
    src/d3d9/identity.h
    src/d3d9/inject.cpp
    src/d3d9/inject.h
    src/d3d9/resources.cpp
    src/d3d9/scene.cpp
    src/d3d9/scene.h
    src/d3d9/semantics.cpp
    src/d3d9/semantics.h
    src/enh/debug_view.cpp
    src/enh/debug_view.h
    src/enh/gi.cpp
    src/enh/gi.h
    src/enh/gbuffer.cpp
    src/enh/gbuffer.h
    src/enh/object_motion.cpp
    src/enh/object_motion.h
    src/enh/upscale.cpp
    src/enh/upscale.h
    src/enh/material_variant.cpp
    src/enh/material_variant.h
    src/enh/rt_debug.cpp
    src/enh/rt_debug.h
    src/enh/rt_material.cpp
    src/enh/rt_material.h
    src/enh/rt_registry.cpp
    src/enh/rt_registry.h
    src/enh/occlusion.cpp
    src/enh/occlusion.h
    src/enh/rt_scene.cpp
    src/enh/rt_scene.h
    src/enh/ocean.cpp
    src/enh/ocean.h
    src/enh/ocean_spectrum.cpp
    src/enh/ocean_spectrum.h
    src/enh/sea_floor.cpp
    src/enh/sea_floor.h
    src/enh/shadows.cpp
    src/enh/shadows.h
    src/enh/shore_field.cpp
    src/enh/shore_field.h
    src/enh/water.cpp
    src/enh/water.h
    src/enh/water_profile.cpp
    src/enh/water_rt.cpp
    src/enh/water_profile.h
    src/engine/materials.cpp
    src/engine/materials.h
    src/engine/memread.cpp
    src/engine/memread.h
    src/engine/scene_hooks.cpp
    src/engine/scene_hooks.h
    src/engine/high_fps.cpp
    src/engine/high_fps.h
    src/engine/lod.cpp
    src/engine/material_source.cpp
    src/engine/material_source.h
    src/engine/lod.h
    src/engine/menu_state.cpp
    src/engine/menu_state.h
    src/engine/shadow_maps.cpp
    src/engine/shadow_maps.h
    src/engine/symbols.cpp
    src/engine/symbols.h
    src/engine/vfs.cpp
    src/engine/vfs.h
    src/engine/weather.cpp
    src/engine/weather.h
    src/engine/window_mode.cpp
    src/engine/window_mode.h
    src/fx/effect_binary.cpp
    src/fx/effect_binary.h
    src/hal/api_generated.h
    src/hal/engine_alloc.cpp
    src/hal/slots_generated.h
    src/hal/stub_generated.h
    src/hal/stub_support.cpp
    src/hal/stub_support.h
    src/hal/trace.cpp
    src/hal/trace.h
    src/hal/trace_generated.h
    src/hal/trace_support.h
    src/hal/types.h
    src/hal/types_base.h
    src/hal/vulkan/device.cpp
    src/hal/vulkan/effect.cpp
    src/hal/vulkan/hal_vulkan.h
    src/hal/vulkan/instance.cpp
    src/hal/vulkan/resources.cpp
    src/overlay/imgui_vulkan.cpp
    src/overlay/keys.cpp
    src/overlay/keys.h
    src/overlay/overlay.cpp
    src/overlay/overlay.h
    src/res/dds.cpp
    src/res/dds.h
    src/shader/abi.h
    src/shader/embedded.cpp.in
    src/shader/embedded.h
    src/shader/glsl.cpp
    src/shader/glsl.h
    src/shader/sm_decode.cpp
    src/shader/sm_disasm.cpp
    src/shader/sm_eval.cpp
    src/shader/sm_eval.h
    src/shader/sm_ir.h
    src/shader/sm_tokens.cpp
    src/shader/sm_tokens.h
    src/shader/spirv.cpp
    src/shader/uber.cpp
    src/shader/uber.h
    src/shader/spirv.h
    src/vk/bindless.cpp
    src/vk/bindless.h
    src/vk/context.cpp
    src/vk/context.h
    src/vk/formats.cpp
    src/vk/formats.h
    src/vk/frames.cpp
    src/vk/fsr.cpp
    src/vk/fsr.h
    src/vk/frames.h
    src/vk/mesh_arena.cpp
    src/vk/mesh_arena.h
    src/vk/gputimer.cpp
    src/vk/gputimer.h
    src/vk/image.cpp
    src/vk/image.h
    src/vk/pipeline.cpp
    src/vk/pipeline_library.cpp
    src/vk/pipeline.h
    src/vk/pacer.cpp
    src/vk/pacer.h
    src/vk/passes.cpp
    src/vk/passes.h
    src/vk/presenter.cpp
    src/vk/presenter.h
    src/vk/probe.cpp
    src/vk/probe.h
    src/vk/raytracing.cpp
    src/vk/raytracing.h
    src/vk/swapchain.cpp
    src/vk/swapchain.h
    src/vk/vk.h
    src/vk/vma_impl.cpp
    src/vk/volk_impl.c
    ${RVK_GENERATED_DIR}/embedded_shaders.cpp
    ${RVK_SHADER_OUTPUTS}
    ${RVK_SHADER_SOURCES}
)
target_include_directories(rvk_core PUBLIC src include)
target_include_directories(rvk_core PRIVATE ${RVK_GENERATED_DIR})
target_include_directories(rvk_core PRIVATE ${STB_INCLUDE_DIR} third_party/imgui_vulkan)
# AMD FidelityFX API headers (MIT); the signed DLL is loaded at run time, never linked (third_party/amd_fidelityfx)
target_include_directories(rvk_core PRIVATE third_party/amd_fidelityfx/ffx-api/include)
# volk is compiled here (not vcpkg's volk.lib): it needs VK_USE_PLATFORM_WIN32_KHR for the Win32 surface entry points
target_compile_definitions(rvk_core PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
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
    imgui::imgui
)
rvk_warnings(rvk_core)
# ccache cannot cache files built with a PCH: CI configures with RVK_PCH=OFF and force-includes the same headers instead
# (rvk_core's sources rely on them)
option(RVK_PCH "Precompiled header for rvk_core" ON)
set(RVK_CORE_PCH
    <algorithm> <array> <cstdint> <filesystem> <memory> <optional> <span> <string> <string_view> <vector>
    <spdlog/spdlog.h>
)
if(RVK_PCH)
    target_precompile_headers(rvk_core PRIVATE ${RVK_CORE_PCH})
else()
    foreach(header ${RVK_CORE_PCH})
        string(REGEX REPLACE "^<(.*)>$" "\\1" header ${header})
        target_compile_options(rvk_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/FI${header}>)
    endforeach()
endif()
set_source_files_properties(src/vk/volk_impl.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# Dear ImGui's Vulkan backend, vendored from the imgui release vcpkg installs (third_party/imgui_vulkan: why there)
set_source_files_properties(src/overlay/imgui_vulkan.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/W3")

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
# rvk_vktest: GPU smoke test of the Vulkan core + shader contract (off-screen, validation on)
# ---------------------------------------------------------------------------------------------
add_executable(rvk_vktest tools/vktest/main.cpp tools/common/d3dx.h)
target_include_directories(rvk_vktest PRIVATE tools ${STB_INCLUDE_DIR})
target_link_libraries(rvk_vktest PRIVATE rvk_core)
target_compile_definitions(rvk_vktest PRIVATE RVK_FFX_DLL="${CMAKE_SOURCE_DIR}/third_party/amd_fidelityfx/presigned_dll/amd_fidelityfx_vk.dll")
rvk_warnings(rvk_vktest)

# rvk_shadereq: two pixel shaders on the GPU with identical random inputs, outputs compared (translated game shader vs the GLSL port)
add_executable(rvk_shadereq tools/shadereq/main.cpp)
target_include_directories(rvk_shadereq PRIVATE tools)
target_link_libraries(rvk_shadereq PRIVATE rvk_core)
rvk_warnings(rvk_shadereq)

# rvk_fxtool: fx_2_0 effect binaries (dump, corpus check against D3DX, compile)
add_executable(rvk_fxtool tools/fxtool/main.cpp tools/common/d3dx.h)
target_include_directories(rvk_fxtool PRIVATE tools)
target_link_libraries(rvk_fxtool PRIVATE rvk_core)
rvk_warnings(rvk_fxtool)

# rvk_d3d9test: the D3D9 device model against D3D9 semantics (off-screen, validation on)
add_executable(rvk_d3d9test tools/d3d9test/main.cpp tools/common/d3dx.h)
target_include_directories(rvk_d3d9test PRIVATE tools ${STB_INCLUDE_DIR})
target_link_libraries(rvk_d3d9test PRIVATE rvk_core)
rvk_warnings(rvk_d3d9test)

# ---------------------------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------------------------
if(RVK_BUILD_TESTS)
    add_executable(rvk_tests tests/test.h tests/test_main.cpp tests/shader_tests.cpp tests/hal_types_tests.cpp tests/fx_tests.cpp tests/format_tests.cpp tests/hal_stub_tests.cpp tests/api_tests.cpp tests/fvf_tests.cpp tests/settings_tests.cpp tests/settings_api_tests.cpp tests/scene_tests.cpp tests/draws_tests.cpp tests/inject_tests.cpp tests/caps_tests.cpp tests/water_tests.cpp tests/ocean_tests.cpp tests/shadows_tests.cpp tests/lod_tests.cpp tests/high_fps_tests.cpp tests/rt_tests.cpp tests/shader_eval_tests.cpp tests/window_mode_tests.cpp tests/keys_tests.cpp tests/upscale_tests.cpp)
    target_link_libraries(rvk_tests PRIVATE rvk_core SPIRV-Tools-static)
    rvk_warnings(rvk_tests)
    add_test(NAME rvk_tests COMMAND rvk_tests)
    # needs a Vulkan 1.3 GPU (and d3dx9_39.dll): ctest -L gpu
    add_test(NAME rvk_vktest COMMAND rvk_vktest ${CMAKE_BINARY_DIR}/vktest.png)
    set_tests_properties(rvk_vktest PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_passes COMMAND rvk_vktest --passes)
    set_tests_properties(rvk_vktest_passes PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_rt COMMAND rvk_vktest --rt)
    set_tests_properties(rvk_vktest_rt PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_fsr COMMAND rvk_vktest --fsr)
    set_tests_properties(rvk_vktest_fsr PROPERTIES LABELS gpu)
    add_test(NAME rvk_d3d9test COMMAND rvk_d3d9test)
    set_tests_properties(rvk_d3d9test PROPERTIES LABELS gpu)
endif()
