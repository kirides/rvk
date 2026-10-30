// Dear ImGui's Vulkan backend (third_party/imgui_vulkan: imgui_impl_vulkan of the same release as vcpkg's imgui), compiled
// against volk's function pointers. vcpkg's imgui[vulkan-binding] is not used: it calls the Vulkan prototypes and links
// vulkan-1.lib, whose symbols collide with volk's global function pointers of the same names (src/vk/volk_impl.c).
// The approach (compile the backend with IMGUI_IMPL_VULKAN_USE_VOLK) is GD3D11's D3D11Engine/VulkanEngine/VulkanImGui.cpp,
// by the same author as this repository.
#define IMGUI_IMPL_VULKAN_USE_VOLK
#include "imgui_impl_vulkan.cpp"
