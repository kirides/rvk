#pragma once

// The one include for Vulkan in this project: volk (function pointers, VK_NO_PROTOTYPES) + VMA configured for volk.

#include <volk.h>

#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#endif
#include <vma/vk_mem_alloc.h>
