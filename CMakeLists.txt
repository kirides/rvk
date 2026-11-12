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
find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

# Tracy profiler client (wolfpld/tracy, vcpkg port without the GUI / CLI tools): the zones and frame marks of src/core/profile.h.
# Off by default: the shipped d3d9.dll is unchanged. ON = TRACY_ENABLE on every target that links rvk_core, the client connects to
# a running Tracy profiler (default port 8086, the profiler's own "Connect" window; the game does not wait for it).
option(RVK_TRACY "Build with the Tracy profiler client (zones, frame marks; see src/core/profile.h)" OFF)
if(RVK_TRACY)
    find_package(Tracy CONFIG REQUIRED)
endif()
find_path(STB_INCLUDE_DIR stb_image_write.h REQUIRED)

# RVK_PEDANTIC (preset clang-pedantic, docs/research/clang-pedantic.md): clang-cl only, extra warnings that catch UB-class bugs; MSVC unchanged
option(RVK_PEDANTIC "clang-cl: -Wall -Wextra -Wpedantic + UB-class warnings" OFF)
option(RVK_UBSAN "clang-cl: -fsanitize=undefined" OFF)
set(RVK_PEDANTIC_FLAGS "")
if(RVK_PEDANTIC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(RVK_PEDANTIC_FLAGS
        # every -W goes through /clang: (clang-cl's own -Wall means -Weverything; /clang: options come last, so the -Wno- ones win over -Wextra)
        /clang:-Wall /clang:-Wextra /clang:-Wpedantic
        /clang:-Wuninitialized /clang:-Wsometimes-uninitialized /clang:-Wconditional-uninitialized
        /clang:-Wcast-align /clang:-Wshadow-uncaptured-local /clang:-Wimplicit-fallthrough /clang:-Wformat=2 /clang:-Wnull-dereference
        /clang:-Wshorten-64-to-32 /clang:-Wundefined-reinterpret-cast /clang:-Warray-bounds-pointer-arithmetic /clang:-Wunreachable-code-aggressive
        # noise for this codebase: /Zc: flags clang ignores; Vulkan structs are brace-initialised with the fields that matter (designated or not);
        # SEH (__try) / __uuidof (IID_PPV_ARGS) are intended MSVC extensions; AMD's headers carry MSVC pragmas; the CRT's *_s advice
        -Wno-unused-command-line-argument /clang:-Wno-missing-field-initializers /clang:-Wno-missing-designated-field-initializers
        /clang:-Wno-language-extension-token /clang:-Wno-unknown-pragmas -D_CRT_SECURE_NO_WARNINGS
        /clang:-Wno-c++98-compat /clang:-Wno-c++98-compat-pedantic /clang:-Wno-nonportable-system-include-path)
    # vcpkg headers are not ours: system includes (/imsvc) silence them
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/imsvc${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include>)
endif()
if(RVK_UBSAN AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Trap mode: clang's standalone UBSan runtime is built /MT and lld-link rejects it against our /MD (RuntimeLibrary mismatch), so a
    # failed check is a ud2 (STATUS_ILLEGAL_INSTRUCTION; run under a debugger / WER dump for the location). vptr needs the runtime: off; "function" reads the bytes before a callee, which the tests' hand-made machine-code stubs (engine / JIT-like code called through a typed pointer) do not have: off.
    add_compile_options(-fsanitize=undefined -fno-sanitize=vptr,function -fsanitize-trap=undefined)
endif()
function(rvk_warnings target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc ${RVK_PEDANTIC_FLAGS})
endfunction()

# ---------------------------------------------------------------------------------------------
# Shaders (docs/research/hlsl-rhi.md): shaders/**/*.hlsl(i) are embedded as text by rvk_embed and compiled at run time by shader/dxc;
# nothing is compiled at build time
# ---------------------------------------------------------------------------------------------
add_executable(rvk_embed tools/embed/main.cpp)
rvk_warnings(rvk_embed)
set_target_properties(rvk_embed PROPERTIES FOLDER tools)

# HLSL (docs/research/hlsl-rhi.md): shaders/**/*.hlsl(i) are embedded as text and compiled at run time by shader/dxc (dxcompiler.dll from
# the vcpkg directx-dxc port, loaded dynamically: only its headers are used here)
find_path(RVK_DXC_INCLUDE_DIR directx-dxc/dxcapi.h REQUIRED)
find_program(RVK_DXC_EXE dxc PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/directx-dxc NO_DEFAULT_PATH)
find_file(RVK_DXC_DLL dxcompiler.dll PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/directx-dxc ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin NO_DEFAULT_PATH)
find_file(RVK_DXC_LICENSE copyright PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/directx-dxc NO_DEFAULT_PATH)
find_file(RVK_DXIL_DLL dxil.dll PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/directx-dxc ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin NO_DEFAULT_PATH)

# The DirectX 12 Agility SDK (vcpkg directx12-agility; docs/research/hlsl-rhi.md "Agility SDK"): the DX12 backend activates D3D12Core.dll from a "d3d12" directory
# next to the exe / d3d9.dll at run time (no link to it); headers + GUIDs come from the directx-headers port (the Windows SDK's d3d12.h is older)
find_package(directx-headers CONFIG REQUIRED)
find_file(RVK_D3D12CORE_DLL D3D12Core.dll PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin NO_DEFAULT_PATH)
find_file(RVK_D3D12LAYERS_DLL d3d12SDKLayers.dll PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin NO_DEFAULT_PATH)
find_file(RVK_D3D12_LICENSE copyright PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/directx12-agility NO_DEFAULT_PATH)

set(RVK_SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(RVK_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
set(RVK_SHADER_OUTPUTS)
# HLSL sources as text (name = path below shaders/)
file(GLOB_RECURSE RVK_HLSL_FILES CONFIGURE_DEPENDS RELATIVE ${RVK_SHADER_DIR} ${RVK_SHADER_DIR}/*.hlsl ${RVK_SHADER_DIR}/*.hlsli)
set(RVK_HLSL_INCLUDES "")
set(RVK_HLSL_TABLE "")
foreach(hlsl ${RVK_HLSL_FILES})
    string(MAKE_C_IDENTIFIER "hlsl_${hlsl}" hident)
    add_custom_command(
        OUTPUT ${RVK_GENERATED_DIR}/shaders/${hident}.h
        COMMAND rvk_embed ${RVK_SHADER_DIR}/${hlsl} -o ${RVK_GENERATED_DIR}/shaders/${hident}.h --name ${hident}
        DEPENDS rvk_embed ${RVK_SHADER_DIR}/${hlsl}
        COMMENT "HLSL text shaders/${hlsl}"
        VERBATIM)
    list(APPEND RVK_SHADER_OUTPUTS ${RVK_GENERATED_DIR}/shaders/${hident}.h)
    string(APPEND RVK_HLSL_INCLUDES "#include \"shaders/${hident}.h\"
")
    string(APPEND RVK_HLSL_TABLE "    {\"${hlsl}\", std::string_view(data::${hident}, sizeof(data::${hident}) - 1)},
")
endforeach()
configure_file(src/shader/embedded.cpp.in ${RVK_GENERATED_DIR}/embedded_shaders.cpp @ONLY)
set(RVK_HLSL_SOURCES_ABS ${RVK_HLSL_FILES})
list(TRANSFORM RVK_HLSL_SOURCES_ABS PREPEND "${RVK_SHADER_DIR}/")
source_group(TREE ${RVK_SHADER_DIR} PREFIX shaders FILES ${RVK_HLSL_SOURCES_ABS})

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
    src/core/f2c4_measure.cpp
    src/core/f2c4_measure.h
    src/core/iat_patch.cpp
    src/core/iat_patch.h
    src/core/f5_measure.cpp
    src/core/f5_measure.h
    src/core/profile.h
    src/core/cluster_build.cpp
    src/core/cluster_build.h
    src/core/cluster_cull.cpp
    src/core/cluster_cull.h
    src/core/crash_dump.cpp
    src/core/crash_dump.h
    src/core/gpu_events.cpp
    src/core/gpu_events.h
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
    src/core/perf_window.cpp
    src/core/perf_window.h
    src/core/stall_sampler.cpp
    src/core/stall_sampler.h
    src/d3d9/convert.cpp
    src/d3d9/convert.h
    src/d3d9/defs.h
    src/d3d9/device.cpp
    src/d3d9/device.h
    src/d3d9/draw.cpp
    src/d3d9/f5_note.cpp
    src/d3d9/cluster_draw.cpp
    src/d3d9/scene_draws.cpp
    src/d3d9/bone_motion.cpp
    src/d3d9/tree_motion.cpp
    src/d3d9/particle_gpu.cpp
    src/d3d9/owned_submit.cpp
    src/gpuscene/billboard_table.cpp
    src/gpuscene/bone_table.cpp
    src/gpuscene/billboard_table.h
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
    src/d3d9/owned_windows.h
    src/d3d9/resources.cpp
    src/d3d9/scene.cpp
    src/d3d9/scene.h
    src/d3d9/semantics.cpp
    src/d3d9/semantics.h
    src/d3d9/target_names.cpp
    src/d3d9/target_names.h
    src/enh/debug_view.cpp
    src/enh/picture_check.cpp
    src/enh/picture_check.h
    src/enh/depth_z.cpp
    src/enh/depth_z.h
    src/enh/debug_view.h
    src/enh/gi.cpp
    src/enh/gi_reference.cpp
    src/enh/gi.h
    src/enh/framegen.cpp
    src/enh/framegen.h
    src/enh/gbuffer.cpp
    src/enh/material_rt.cpp
    src/enh/gbuffer.h
    src/enh/object_motion.cpp
    src/enh/object_motion.h
    src/enh/upscale.cpp
    src/enh/upscale.h
    src/enh/material_pack.cpp
    src/enh/material_pack.h
    src/enh/material_variant.cpp
    src/enh/material_variant.h
    src/enh/rt_debug.cpp
    src/enh/rt_debug.h
    src/enh/rt_material.cpp
    src/enh/rt_material.h
    src/native/coverage.cpp
    src/native/coverage.h
    src/native/effect_compose.cpp
    src/native/effect_compose.h
    src/native/effect_ops.cpp
    src/native/effect_ops_setup.cpp
    src/native/effect_ops_post.cpp
    src/native/effect_ops_gui.cpp
    src/native/effect_ops_gui_owned.cpp
    src/native/effect_ops_pssm.cpp
    src/native/effect_ops_lean.cpp
    src/native/own_post.cpp
    src/native/own_post_groups.cpp
    src/native/own_post_units.cpp
    src/native/own_post_fx.cpp
    src/native/effect_ops_fx.cpp
    src/native/own_post.h
    src/native/effect_ops.h
    src/native/frame_desc.cpp
    src/native/frame_desc.h
    src/native/f2c_measure.cpp
    src/native/f2c_measure.h
    src/native/f4_measure.cpp
    src/native/f10_collect.cpp
    src/native/f10_collect.h
    src/native/f10_request.cpp
    src/native/f10_request.h
    src/native/f10_extract.cpp
    src/native/f10_extract.h
    src/native/f10_occluder.h
    src/native/f10_math.h
    src/native/f10_portal.h
    src/native/f4_measure.h
    src/native/frame_tap.cpp
    src/native/frame_tap.h
    src/native/owned_observe.cpp
    src/native/owned_observe.h
    src/native/static_compose.cpp
    src/native/static_ops_lean.cpp
    src/native/static_ops_retained.cpp
    src/native/static_ops_records.cpp
    src/native/static_ops_owned.cpp
    src/native/static_ops_deps.cpp
    src/native/op_records.h
    src/native/bb_records.h
    src/native/static_ops_bb.cpp
    src/native/static_ops_trees_owned.cpp
    src/native/static_ops_stages.cpp
    src/native/static_ops_veg_owned.cpp
    src/native/static_ops_nonop.cpp
    src/native/static_ops_fx.cpp
    src/native/static_ops_sky.cpp
    src/native/static_ops_actors.cpp
    src/native/static_ops_actors_owned.cpp
    src/native/static_ops_inst_owned.cpp
    src/native/static_ops_ordered.cpp
    src/native/static_ops_trees.cpp
    src/native/static_compose.h
    src/native/static_ops.cpp
    src/native/static_ops.h
    src/enh/rt_registry.cpp
    src/enh/rt_registry.h
    src/enh/rt_lod.cpp
    src/enh/rt_lod.h
    src/enh/rt_scene.cpp
    src/enh/rt_gpu_build.cpp
    src/enh/rt_gpu_tables.cpp
    src/enh/rt_scene.h
    src/enh/rt_shadows.cpp
    src/enh/rt_shadows.h
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
    src/enh/light_table.cpp
    src/enh/water_profile.h
    src/engine/materials.cpp
    src/engine/materials.h
    src/engine/memread.cpp
    src/engine/memread.h
    src/engine/scene_hooks.cpp
    src/engine/scene_hooks.h
    src/engine/exposure.cpp
    src/engine/exposure.h
    src/engine/image_effects.cpp
    src/engine/image_effects.h
    src/engine/hemisphere.cpp
    src/engine/hemisphere.h
    src/engine/exit_guard.cpp
    src/engine/exit_guard.h
    src/engine/high_fps.cpp
    src/engine/high_fps.h
    src/engine/lod.cpp
    src/engine/fast_sort.cpp
    src/engine/fast_sort.h
    src/engine/load_accel.cpp
    src/engine/load_accel.h
    src/engine/xinput_throttle.cpp
    src/engine/xinput_throttle.h
    src/engine/material_source.cpp
    src/engine/material_source.h
    src/engine/lights.cpp
    src/engine/scene_table.cpp
    src/engine/illum_census.cpp
    src/gpuscene/instance_buffer.cpp
    src/gpuscene/illumination_table.cpp
    src/gpuscene/illumination_table.h
    src/gpuscene/material_table.cpp
    src/gpuscene/material_table.h
    src/gpuscene/material_compose.h
    src/gpuscene/material_graph.h
    src/gpuscene/window_compose.h
    src/gpuscene/window_tables.cpp
    src/gpuscene/view_table.cpp
    src/gpuscene/draw_record_table.cpp
    src/gpuscene/draw_record_table.h
    src/gpuscene/cluster_table.cpp
    src/gpuscene/cluster_table.h
    src/gpuscene/owned_compact.cpp
    src/gpuscene/owned_compact.h
    src/gpuscene/shutdown.cpp
    src/gpuscene/shutdown.h
    src/d3d9/owned_persist.cpp
    src/d3d9/owned_trees.cpp
    src/gpuscene/view_table.h
    src/gpuscene/window_tables.h
    src/gpuscene/object_table.cpp
    src/gpuscene/object_table.h
    src/gpuscene/row_table.cpp
    src/gpuscene/row_table.h
    src/gpuscene/object_compose.h
    src/gpuscene/illum_compose.h
    src/gpuscene/lighting_images.cpp
    src/gpuscene/lighting_images.h
    src/gpuscene/mesh_layout.h
    src/gpuscene/mesh_parts.cpp
    src/gpuscene/mesh_parts.h
    src/gpuscene/instance_buffer.h
    src/engine/scene_table.h
    src/engine/illum_census.h
    src/engine/dep_events.h
    src/engine/dep_events.cpp
    src/engine/lights.h
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
    src/engine/display_resolve.cpp
    src/engine/display_resolve.h
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
    src/hal/vulkan/buffer_lookup.cpp
    src/hal/vulkan/buffer_lookup.h
    src/hal/vulkan/device.cpp
    src/hal/vulkan/effect.cpp
    src/hal/vulkan/hal_vulkan.h
    src/hal/vulkan/instance.cpp
    src/hal/vulkan/resources.cpp
    src/overlay/keys.cpp
    src/overlay/keys.h
    src/overlay/overlay.cpp
    src/overlay/overlay.h
    src/overlay/overlay_ui.cpp
    src/res/dds.cpp
    src/res/dds.h
    src/shader/abi.h
    src/shader/abi_blocks.cpp
    src/shader/abi_blocks.h
    src/shader/pass_push.h
    src/rhi/game_device.h
    src/rhi/rhi.cpp
    src/rhi/rhi.h
    src/rhi/modules.h
    src/rhi/names.h
    src/rhi/vulkan/adapters.cpp
    src/rhi/vulkan/game_present.cpp
    src/rhi/vulkan/game_present.h
    src/rhi/vulkan/imgui_backend.cpp
    src/rhi/vulkan/imgui_vulkan_impl.cpp
    src/rhi/vulkan/modules.cpp
    src/rhi/vulkan/vulkan_device.cpp
    src/rhi/vulkan/wrap.h
    src/shader/dxc.cpp
    src/shader/dxc.h
    src/shader/embedded.cpp.in
    src/shader/embedded.h
    src/shader/translate.h
    src/shader/hlsl.cpp
    src/shader/hlsl.h
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
    src/vk/crash_markers.cpp
    src/vk/crash_markers.h
    src/vk/debug_names.cpp
    src/vk/debug_names.h
    src/rhi/d3d_formats.cpp
    src/rhi/d3d_formats.h
    src/vk/frames.cpp
    src/vk/framegen.cpp
    src/vk/framegen.h
    src/vk/fg_swapchain.cpp
    src/vk/fg_trace.cpp
    src/vk/fg_swapchain.h
    src/vk/fg_trace.h
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
    src/vk/game_record.cpp
    src/vk/pipeline.h
    src/core/pacer.cpp
    src/core/pacer.h
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
target_include_directories(rvk_core PRIVATE ${RVK_GENERATED_DIR} ${RVK_DXC_INCLUDE_DIR})
target_include_directories(rvk_core PRIVATE ${STB_INCLUDE_DIR} third_party/imgui_vulkan)
# AMD FSR SDK 2.4.0 API headers (MIT); the signed DLLs are loaded at run time, never linked (third_party/amd_fidelityfx)
target_include_directories(rvk_core PRIVATE third_party/amd_fidelityfx/api/include third_party/amd_fidelityfx/upscalers/include third_party/amd_fidelityfx/framegeneration/include)
# volk is compiled here (not vcpkg's volk.lib): it needs VK_USE_PLATFORM_WIN32_KHR for the Win32 surface entry points
target_compile_definitions(rvk_core PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
# Tracy: PUBLIC so every target that links rvk_core sees the same TRACY_ENABLE (its inline functions differ with it: one ODR)
if(RVK_TRACY)
    target_compile_definitions(rvk_core PUBLIC TRACY_ENABLE)
    target_link_libraries(rvk_core PUBLIC Tracy::TracyClient)
endif()
target_link_libraries(rvk_core PUBLIC
    spdlog::spdlog
    minhook::minhook
    tomlplusplus::tomlplusplus
    volk::volk_headers
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    imgui::imgui
    meshoptimizer::meshoptimizer
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
# Vendored / third-party sources stay optimized in the no-optimization preset (clang-o0 appends /Od /Ob0 to RelWithDebInfo; the later /O2 wins), only our own code is /Od
set(RVK_VENDORED_OPT "$<$<NOT:$<CONFIG:Debug>>:/O2;/Ob2>")
set_source_files_properties(src/vk/volk_impl.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "${RVK_VENDORED_OPT}")
# Dear ImGui's Vulkan backend, vendored from the imgui release vcpkg installs (third_party/imgui_vulkan: why there)
set_source_files_properties(src/rhi/vulkan/imgui_vulkan_impl.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/W3;${RVK_VENDORED_OPT}")

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
# dxcompiler.dll (+ dxil.dll for DXIL) next to every binary that compiles HLSL: the runtime compiler loads it from its own directory
function(rvk_deploy_dxc target)
    if(RVK_DXC_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RVK_DXC_DLL} ${RVK_DXIL_DLL} ${RVK_DXC_LICENSE} $<TARGET_FILE_DIR:${target}>
            COMMENT "dxcompiler.dll -> $<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
# D3D12Core.dll (+ the debug layer d3d12SDKLayers.dll) into <target dir>\d3d12\: scripts/run.ps1 copies that folder to bin\rvk\d3d12 (package.ps1 does not
# ship it yet: the game does not use the DX12 backend)
function(rvk_deploy_agility target)
    if(RVK_D3D12CORE_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/d3d12
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RVK_D3D12CORE_DLL} ${RVK_D3D12LAYERS_DLL} ${RVK_D3D12_LICENSE} $<TARGET_FILE_DIR:${target}>/d3d12
            COMMENT "D3D12Core.dll -> $<TARGET_FILE_DIR:${target}>/d3d12"
            VERBATIM)
    endif()
endfunction()
set_target_properties(rvk_d3d9 PROPERTIES OUTPUT_NAME d3d9)
rvk_deploy_dxc(rvk_d3d9)
rvk_deploy_agility(rvk_d3d9)
target_link_libraries(rvk_d3d9 PRIVATE rvk_core)
rvk_warnings(rvk_d3d9)

# ---------------------------------------------------------------------------------------------
# rvk_shadertool: offline shader translator driver (corpus validation, listings)
# ---------------------------------------------------------------------------------------------
add_executable(rvk_shadertool tools/shadertool/main.cpp)
target_link_libraries(rvk_shadertool PRIVATE rvk_core SPIRV-Tools-static)
rvk_deploy_dxc(rvk_shadertool)
rvk_warnings(rvk_shadertool)

# ---------------------------------------------------------------------------------------------
# The slim RHI's DX12 backend (src/rhi/d3d12, docs/research/hlsl-rhi.md 5, 6) and its test: a separate library so that d3d9.dll does not import
# d3d12.dll / dxgi.dll until the renderer uses the backend. rvk_rhitest runs the same code on both backends.
# ---------------------------------------------------------------------------------------------
add_library(rvk_rhi_d3d12 STATIC src/rhi/d3d12/d3d12_device.cpp src/rhi/d3d12/d3d12_ffx.cpp src/rhi/d3d12/d3d12_ffx.h third_party/imgui_dx12/imgui_impl_dx12.cpp)
target_include_directories(rvk_rhi_d3d12 PUBLIC src include)
target_include_directories(rvk_rhi_d3d12 PRIVATE third_party/imgui_dx12 third_party/amd_fidelityfx/api/include third_party/amd_fidelityfx/upscalers/include third_party/amd_fidelityfx/framegeneration/include)
target_link_libraries(rvk_rhi_d3d12 PUBLIC rvk_core Microsoft::DirectX-Guids d3d12 dxgi dxguid d3dcompiler imgui::imgui)
# Dear ImGui's DX12 backend, vendored from the imgui release vcpkg installs (third_party/imgui_dx12: built on the Agility headers like the rest of the backend)
set_source_files_properties(third_party/imgui_dx12/imgui_impl_dx12.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/W3;${RVK_VENDORED_OPT}")
if(RVK_PEDANTIC)
    set_property(SOURCE third_party/imgui_dx12/imgui_impl_dx12.cpp APPEND PROPERTY COMPILE_OPTIONS "-w")  # vendored: imgui's own warnings
endif()
# d3d12.dll / dxgi.dll are delay-loaded by everything that links the backend: the Vulkan-only path never loads them
target_link_libraries(rvk_rhi_d3d12 PUBLIC delayimp)
target_link_options(rvk_rhi_d3d12 INTERFACE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll)
# rvk_core's createGameDevice(Backend::D3D12) calls into the backend (a cycle between two static libraries: allowed)
target_link_libraries(rvk_core PUBLIC rvk_rhi_d3d12)
rvk_warnings(rvk_rhi_d3d12)
add_executable(rvk_rhitest tools/rhitest/main.cpp tools/rhitest/abi_probe.cpp tools/rhitest/parity.cpp tools/rhitest/ffx.cpp tools/common/d3dx.h)
target_compile_definitions(rvk_rhitest PRIVATE RVK_FFX_DLL_DX12="${CMAKE_SOURCE_DIR}/third_party/amd_fidelityfx/signedbin/amd_fidelityfx_loader_dx12.dll")
target_include_directories(rvk_rhitest PRIVATE tools)
target_link_libraries(rvk_rhitest PRIVATE rvk_core rvk_rhi_d3d12)
rvk_deploy_dxc(rvk_rhitest)
rvk_deploy_agility(rvk_rhitest)
rvk_warnings(rvk_rhitest)
set_target_properties(rvk_rhi_d3d12 rvk_rhitest PROPERTIES FOLDER tools)
if(BUILD_TESTING)
    add_test(NAME rvk_rhitest COMMAND rvk_rhitest)
endif()

# ---------------------------------------------------------------------------------------------
# rvk_vktest: GPU smoke test of the Vulkan core + shader contract (off-screen, validation on)
# ---------------------------------------------------------------------------------------------
add_executable(rvk_vktest tools/vktest/main.cpp tools/common/d3dx.h)
target_include_directories(rvk_vktest PRIVATE tools ${STB_INCLUDE_DIR})
target_link_libraries(rvk_vktest PRIVATE rvk_core)
rvk_deploy_dxc(rvk_vktest)
target_compile_definitions(rvk_vktest PRIVATE RVK_FFX_DLL="${CMAKE_SOURCE_DIR}/third_party/amd_fidelityfx/signedbin/amd_fidelityfx_loader_vk.dll")
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
    add_executable(rvk_tests tests/test.h tests/test_main.cpp tests/shader_tests.cpp tests/hal_types_tests.cpp tests/fx_tests.cpp tests/format_tests.cpp tests/hal_stub_tests.cpp tests/api_tests.cpp tests/fvf_tests.cpp tests/settings_tests.cpp tests/settings_api_tests.cpp tests/scene_tests.cpp tests/draws_tests.cpp tests/inject_tests.cpp tests/caps_tests.cpp tests/water_tests.cpp tests/ocean_tests.cpp tests/shadows_tests.cpp tests/lod_tests.cpp tests/high_fps_tests.cpp tests/rt_tests.cpp tests/shader_eval_tests.cpp tests/window_mode_tests.cpp tests/display_resolve_tests.cpp tests/keys_tests.cpp tests/upscale_tests.cpp tests/native_tests.cpp tests/names_tests.cpp tests/framegen_tests.cpp tests/dxc_tests.cpp tests/gpu_ref_tests.cpp tests/d3d12_tests.cpp tests/material_pack_tests.cpp tests/light_table_tests.cpp tests/illum_table_tests.cpp tests/mesh_parts_tests.cpp tests/material_table_tests.cpp tests/window_tables_tests.cpp tests/crash_dump_tests.cpp tests/gpu_events_tests.cpp tests/f5_tests.cpp tests/cluster_tests.cpp tests/cluster_cull_tests.cpp tests/f10_tests.cpp tests/f10_portal_tests.cpp tests/f10_request_tests.cpp tests/f10_extract_tests.cpp tests/abi_blocks_tests.cpp tests/perf_window_tests.cpp)
    target_link_libraries(rvk_tests PRIVATE rvk_core SPIRV-Tools-static)
    rvk_warnings(rvk_tests)
    if(MSVC)
        # the incremental test PDB reached the 4 GiB limit of the default 4 KiB page size (LNK1140): 8 KiB pages raise it to 8 GiB
        target_link_options(rvk_tests PRIVATE /PDBPAGESIZE:8192)
    endif()
    rvk_deploy_dxc(rvk_tests)
    add_test(NAME rvk_tests COMMAND rvk_tests)
    # needs a Vulkan 1.3 GPU (and d3dx9_39.dll): ctest -L gpu
    add_test(NAME rvk_vktest COMMAND rvk_vktest ${CMAKE_BINARY_DIR}/vktest.png)
    set_tests_properties(rvk_vktest PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_passes COMMAND rvk_vktest --passes)
    set_tests_properties(rvk_vktest_passes PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_passes_rhi COMMAND rvk_vktest --passes --rhi)
    set_tests_properties(rvk_vktest_passes_rhi PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_rt COMMAND rvk_vktest --rt)
    set_tests_properties(rvk_vktest_rt PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_fsr COMMAND rvk_vktest --fsr)
    set_tests_properties(rvk_vktest_fsr PROPERTIES LABELS gpu)
    add_test(NAME rvk_vktest_fg COMMAND rvk_vktest --fg)
    set_tests_properties(rvk_vktest_fg PROPERTIES LABELS gpu)
    add_test(NAME rvk_d3d9test COMMAND rvk_d3d9test)
    set_tests_properties(rvk_d3d9test PROPERTIES LABELS gpu)
endif()
