
#if SPUDGPU_COMPILE_VULKAN_API

#include <stdio.h>
#include <string.h>

#include "spudgpuvulkan.h"
#include "stdlib.h"
#include "stdbool.h"
#include "../../../spudstructbase.h"
#include "../../spudgpushared.h"

VkFormat convert_spud_to_vulkan_format(SPUDGPU_FORMAT format) {
    switch (format) {
        // ── 128-bit ──────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R32G32B32A32_TYPELESS: return VK_FORMAT_R32G32B32A32_SFLOAT;
        // no typeless in Vulkan; treat as float
        case SPUDGPU_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case SPUDGPU_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
        case SPUDGPU_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;

        // ── 96-bit ───────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R32G32B32_TYPELESS: return VK_FORMAT_R32G32B32_SFLOAT;
        case SPUDGPU_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case SPUDGPU_FORMAT_R32G32B32_UINT: return VK_FORMAT_R32G32B32_UINT;
        case SPUDGPU_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32_SINT;

        // ── 64-bit ───────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R16G16B16A16_TYPELESS: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case SPUDGPU_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case SPUDGPU_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case SPUDGPU_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
        case SPUDGPU_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case SPUDGPU_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;

        case SPUDGPU_FORMAT_R32G32_TYPELESS: return VK_FORMAT_R32G32_SFLOAT;
        case SPUDGPU_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case SPUDGPU_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
        case SPUDGPU_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SINT;

        // ── Depth/stencil packed (64-bit) ────────────────────────────────────
        case SPUDGPU_FORMAT_R32G8X24_TYPELESS: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case SPUDGPU_FORMAT_R32_FLOAT_X8X24_TYPELESS: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        // no direct equivalent; sampled depth view
        case SPUDGPU_FORMAT_X32_TYPELESS_G8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT; // sampled stencil view

        // ── 32-bit ───────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R10G10B10A2_TYPELESS: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case SPUDGPU_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case SPUDGPU_FORMAT_R10G10B10A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case SPUDGPU_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

        case SPUDGPU_FORMAT_R8G8B8A8_TYPELESS: return VK_FORMAT_R8G8B8A8_UNORM;
        case SPUDGPU_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case SPUDGPU_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case SPUDGPU_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
        case SPUDGPU_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
        case SPUDGPU_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;

        case SPUDGPU_FORMAT_R16G16_TYPELESS: return VK_FORMAT_R16G16_SFLOAT;
        case SPUDGPU_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case SPUDGPU_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
        case SPUDGPU_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
        case SPUDGPU_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case SPUDGPU_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;

        case SPUDGPU_FORMAT_R32_TYPELESS: return VK_FORMAT_R32_SFLOAT;
        case SPUDGPU_FORMAT_D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
        case SPUDGPU_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case SPUDGPU_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        case SPUDGPU_FORMAT_R32_SINT: return VK_FORMAT_R32_SINT;

        // ── Depth/stencil packed (32-bit) ────────────────────────────────────
        case SPUDGPU_FORMAT_R24G8_TYPELESS: return VK_FORMAT_D24_UNORM_S8_UINT;
        case SPUDGPU_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
        case SPUDGPU_FORMAT_R24_UNORM_X8_TYPELESS: return VK_FORMAT_D24_UNORM_S8_UINT;
        case SPUDGPU_FORMAT_X24_TYPELESS_G8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;

        // ── 16-bit ───────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R8G8_TYPELESS: return VK_FORMAT_R8G8_UNORM;
        case SPUDGPU_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case SPUDGPU_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
        case SPUDGPU_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
        case SPUDGPU_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SINT;

        case SPUDGPU_FORMAT_R16_TYPELESS: return VK_FORMAT_R16_SFLOAT;
        case SPUDGPU_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case SPUDGPU_FORMAT_D16_UNORM: return VK_FORMAT_D16_UNORM;
        case SPUDGPU_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
        case SPUDGPU_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
        case SPUDGPU_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case SPUDGPU_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;

        // ── 8-bit ────────────────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R8_TYPELESS: return VK_FORMAT_R8_UNORM;
        case SPUDGPU_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case SPUDGPU_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
        case SPUDGPU_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
        case SPUDGPU_FORMAT_R8_SINT: return VK_FORMAT_R8_SINT;
        case SPUDGPU_FORMAT_A8_UNORM: return VK_FORMAT_R8_UNORM;
        // Vulkan has no dedicated A8; use R8 and swizzle in the image view

        // ── Exotic / packed ──────────────────────────────────────────────────
        case SPUDGPU_FORMAT_R1_UNORM: return VK_FORMAT_UNDEFINED; // no Vulkan equivalent
        case SPUDGPU_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case SPUDGPU_FORMAT_R8G8_B8G8_UNORM: return VK_FORMAT_B8G8R8G8_422_UNORM; // DXGI packed 4:2:2
        case SPUDGPU_FORMAT_G8R8_G8B8_UNORM: return VK_FORMAT_G8B8G8R8_422_UNORM;

        // ── BGR / swapchain formats ───────────────────────────────────────────
        case SPUDGPU_FORMAT_B5G6R5_UNORM: return VK_FORMAT_B5G6R5_UNORM_PACK16;
        case SPUDGPU_FORMAT_B5G5R5A1_UNORM: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
        case SPUDGPU_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case SPUDGPU_FORMAT_B8G8R8X8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM; // X = ignored alpha; same bits
        case SPUDGPU_FORMAT_B8G8R8A8_TYPELESS: return VK_FORMAT_B8G8R8A8_UNORM;
        case SPUDGPU_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case SPUDGPU_FORMAT_B8G8R8X8_TYPELESS: return VK_FORMAT_B8G8R8A8_UNORM;
        case SPUDGPU_FORMAT_B8G8R8X8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case SPUDGPU_FORMAT_B4G4R4A4_UNORM: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
        case SPUDGPU_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return VK_FORMAT_UNDEFINED;
        // XR bias is display-only; no Vulkan match

        // ── BC block compression ──────────────────────────────────────────────
        case SPUDGPU_FORMAT_BC1_TYPELESS: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC1_UNORM_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case SPUDGPU_FORMAT_BC2_TYPELESS: return VK_FORMAT_BC2_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC2_UNORM_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
        case SPUDGPU_FORMAT_BC3_TYPELESS: return VK_FORMAT_BC3_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC3_UNORM_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
        case SPUDGPU_FORMAT_BC4_TYPELESS: return VK_FORMAT_BC4_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
        case SPUDGPU_FORMAT_BC5_TYPELESS: return VK_FORMAT_BC5_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
        case SPUDGPU_FORMAT_BC6H_TYPELESS: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case SPUDGPU_FORMAT_BC6H_UF16: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case SPUDGPU_FORMAT_BC6H_SF16: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case SPUDGPU_FORMAT_BC7_TYPELESS: return VK_FORMAT_BC7_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
        case SPUDGPU_FORMAT_BC7_UNORM_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;

        // ── YUV / video formats ───────────────────────────────────────────────
        case SPUDGPU_FORMAT_NV12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        case SPUDGPU_FORMAT_P010: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
        case SPUDGPU_FORMAT_P016: return VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
        case SPUDGPU_FORMAT_YUY2: return VK_FORMAT_G8B8G8R8_422_UNORM;
        case SPUDGPU_FORMAT_420_OPAQUE: return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
        case SPUDGPU_FORMAT_Y210: return VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16;
        case SPUDGPU_FORMAT_Y216: return VK_FORMAT_G16B16G16R16_422_UNORM;
        case SPUDGPU_FORMAT_P208: return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
        // AYUV/Y410/Y416/NV11/AI44/IA44/P8/A8P8/V208/V408 have no standard
        // Vulkan equivalent (packed 4:4:4 YUVA, 4:1:1 chroma subsampling, or
        // legacy 8-bit palette formats aren't representable by any core or
        // KHR_sampler_ycbcr_conversion VkFormat) — explicitly UNDEFINED
        // rather than falling through, so the gap is visible here rather
        // than being indistinguishable from a genuinely unmapped value.
        case SPUDGPU_FORMAT_AYUV:
        case SPUDGPU_FORMAT_Y410:
        case SPUDGPU_FORMAT_Y416:
        case SPUDGPU_FORMAT_NV11:
        case SPUDGPU_FORMAT_AI44:
        case SPUDGPU_FORMAT_IA44:
        case SPUDGPU_FORMAT_P8:
        case SPUDGPU_FORMAT_A8P8:
        case SPUDGPU_FORMAT_V208:
        case SPUDGPU_FORMAT_V408:
            return VK_FORMAT_UNDEFINED;

        // SPUDGPU_FORMAT_UNKNOWN (0) and SPUDGPU_FORMAT_FORCE_UINT (sentinel,
        // not a real format) fall through to here along with any value that
        // isn't a defined SPUDGPU_FORMAT at all.
        default: return VK_FORMAT_UNDEFINED;
    }
}


// Whether the Vulkan loader offers instance extension [name].
static bool spudgpuvulkan__instance_extension_available(const SPUD_ALLOCATOR *allocator, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    const uint32_t capacity = count;
    VkExtensionProperties *properties = SPUD_NEW_ARRAY(allocator, VkExtensionProperties, capacity);
    if (!properties)
        return false;
    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, properties) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(properties[i].extensionName, name) == 0;
    SPUD_DELETE_ARRAY(allocator, VkExtensionProperties, properties, capacity);
    return found;
}

// The instance extensions, into [names] (room for 8), and their count.
// [*out_surface_maintenance1_khr] and [_ext] say which of those two went in.
static void spudgpuvulkan__determine_vulkan_extensions(
    const SPUD_ALLOCATOR *allocator,
    VkInstanceCreateInfo *pOutput,
    const char **names,
    bool *out_surface_maintenance1_khr,
    bool *out_surface_maintenance1_ext) {
    *out_surface_maintenance1_khr = false;
    *out_surface_maintenance1_ext = false;
    if (!pOutput) return;
    uint32_t count = 0;
#if defined(_WIN32)
    names[count++] = "VK_KHR_surface";
    names[count++] = "VK_KHR_win32_surface";
#elif defined(__linux__)
    // SDL picks Wayland vs Xlib at runtime (independent of whichever one
    // SPUDGPU_PLATFORM_* was compiled against), so both surface extensions
    // must be enabled on the instance rather than just one.
    names[count++] = "VK_KHR_surface";
    names[count++] = "VK_KHR_wayland_surface";
    names[count++] = "VK_KHR_xlib_surface";
#endif
    // Debug labels (spudgpu_cmd_begin_debug_label) and object names: only
    // read by tools, and enabled wherever the loader has them.
    if (spudgpuvulkan__instance_extension_available(allocator, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        names[count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    // The surface half of a device's VK_KHR_swapchain_maintenance1 (or its
    // EXT original, which some drivers still list alone), for present
    // scaling: both where the loader has them, so that an adapter with
    // either device extension can enable it. Each needs
    // VK_KHR_get_surface_capabilities2.
    if (spudgpuvulkan__instance_extension_available(allocator, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        const bool khr = spudgpuvulkan__instance_extension_available(allocator, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        const bool ext = spudgpuvulkan__instance_extension_available(allocator, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if (khr || ext)
            names[count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        if (khr)
            names[count++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        if (ext)
            names[count++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        *out_surface_maintenance1_khr = khr;
        *out_surface_maintenance1_ext = ext;
    }
    pOutput->enabledExtensionCount   = count;
    pOutput->ppEnabledExtensionNames = count ? names : NULL;
}

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

// Whether [physicalDevice] offers device extension [name].
static bool spudgpuvulkan___device_extension_available(const SPUD_ALLOCATOR *allocator, VkPhysicalDevice physicalDevice, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    const uint32_t capacity = count;
    VkExtensionProperties *extensions = SPUD_NEW_ARRAY_ZEROED(allocator, VkExtensionProperties, capacity);
    if (!extensions)
        return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &count, extensions) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(extensions[i].extensionName, name) == 0;
    SPUD_DELETE_ARRAY(allocator, VkExtensionProperties, extensions, capacity);
    return found;
}

// Picks each kind's family by the rule in spudgpu.h and fills in the queue
// pools and the timestamp feature from them. False when the GPU has no
// family with graphics, which this backend does not list.
static bool spudgpuvulkan___read_queue_families(const SPUD_ALLOCATOR *allocator, spudgpu_adapter_vulkan *adapter, SPUDGPU_FEATURES *features) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(adapter->_physical_device_vk, &count, NULL);
    if (count == 0)
        return false;
    const uint32_t capacity = count;
    VkQueueFamilyProperties *families = SPUD_NEW_ARRAY_ZEROED(allocator, VkQueueFamilyProperties, capacity);
    if (!families)
        return false;
    vkGetPhysicalDeviceQueueFamilyProperties(adapter->_physical_device_vk, &count, families);

    uint32_t direct = UINT32_MAX, compute = UINT32_MAX, copy = UINT32_MAX;
    for (uint32_t i = 0; i < count && direct == UINT32_MAX; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            direct = i;
    for (uint32_t i = 0; i < count && compute == UINT32_MAX; i++)
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            compute = i;
    for (uint32_t i = 0; i < count && copy == UINT32_MAX; i++)
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            copy = i;
    if (direct == UINT32_MAX) {
        SPUD_DELETE_ARRAY(allocator, VkQueueFamilyProperties, families, capacity);
        return false;
    }
    if (compute == UINT32_MAX)
        compute = direct;
    if (copy == UINT32_MAX)
        copy = compute;
    adapter->_family_direct  = direct;
    adapter->_family_compute = compute;
    adapter->_family_copy    = copy;

    // One pool per family the rule picked, its kinds the ones made from it.
    SPUDGPU_ADAPTER_PROPERTIES *properties = &adapter->_facts.properties;
    const uint32_t kind_family[3]          = {direct, compute, copy};
    const SPUDGPU_QUEUE_KINDS kind_bit[3]  = {SPUDGPU_QUEUE_KIND_DIRECT, SPUDGPU_QUEUE_KIND_COMPUTE, SPUDGPU_QUEUE_KIND_COPY};
    uint32_t pool_family[3]                = {0};
    properties->queue_pool_count           = 0;
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t pool = 0;
        while (pool < properties->queue_pool_count && pool_family[pool] != kind_family[k])
            pool++;
        if (pool == properties->queue_pool_count) {
            pool_family[pool] = kind_family[k];
            const VkQueueFamilyProperties *family    = &families[kind_family[k]];
            properties->queue_pools[pool].kinds      = 0;
            properties->queue_pools[pool].max_queues = family->queueCount;
            properties->queue_pools[pool].engine     = (family->queueFlags & VK_QUEUE_GRAPHICS_BIT)
                                                           ? SPUDGPU_QUEUE_POOL_ENGINE_GRAPHICS
                                                           : SPUDGPU_QUEUE_POOL_ENGINE_SEPARATE;
            properties->queue_pool_count++;
        }
        properties->queue_pools[pool].kinds |= kind_bit[k];
    }

    // Timestamps are written by command lists on the direct queues, whose
    // family says whether it counts time at all.
    if (families[direct].timestampValidBits > 0)
        *features |= SPUDGPU_FEATURE_PASS_TIMESTAMPS;
    SPUD_DELETE_ARRAY(allocator, VkQueueFamilyProperties, families, capacity);
    return true;
}

// Fills in what [adapter] is and can do. False when the GPU can't have a
// device of this backend (below Vulkan 1.3, no shaderDrawParameters, or no
// graphics family), which is then not listed. Creates nothing.
static bool spudgpuvulkan___read_adapter_facts(
    const SPUD_ALLOCATOR *allocator,
    const spudgpu_instance_vulkan *instance,
    spudgpu_adapter_vulkan *adapter) {
    VkPhysicalDevice physicalDevice        = adapter->_physical_device_vk;
    spudgpu___adapter_facts *facts         = &adapter->_facts;
    SPUDGPU_ADAPTER_PROPERTIES *properties = &facts->properties;
    properties->base                       = spud_struct_base_out(SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);

    vkGetPhysicalDeviceProperties(physicalDevice, &adapter->_properties_vk);
    const VkPhysicalDeviceProperties *props = &adapter->_properties_vk;
    // What SpudGPU itself relies on (dynamicRendering, synchronization2,
    // timelineSemaphore, bufferDeviceAddress) is required by Vulkan 1.3;
    // shaderDrawParameters, below, by SpudGPU.
    if (props->apiVersion < VK_API_VERSION_1_3)
        return false;
    // A shader's vertex and instance index (HLSL's SV_VertexID and
    // SV_InstanceID, as a compiler writes them into SPIR-V) declares
    // DrawParameters, which needs shaderDrawParameters: part of the base set
    // (card S22), so a GPU without it is not listed.
    VkPhysicalDeviceVulkan11Features vk11Available = {0};
    vk11Available.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 baseFeatures2 = {0};
    baseFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    baseFeatures2.pNext = &vk11Available;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &baseFeatures2);
    if (!vk11Available.shaderDrawParameters)
        return false;

    SPUDGPU_FEATURES features = 0;
    if (!spudgpuvulkan___read_queue_families(allocator, adapter, &features))
        return false;

    size_t nameLen = strnlen(props->deviceName, sizeof(properties->description) - 1);
    memcpy(properties->description, props->deviceName, nameLen);
    properties->description[nameLen] = '\0';
    properties->vendor_id            = props->vendorID;
    properties->device_id            = props->deviceID;
    // Vulkan has no subsystem ID or revision.
    properties->subsystem_id = 0;
    properties->revision     = 0;

    switch (props->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: properties->adapter_type = SPUDGPU_ADAPTER_TYPE_INTEGRATED; break;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: properties->adapter_type = SPUDGPU_ADAPTER_TYPE_DISCRETE; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: properties->adapter_type = SPUDGPU_ADAPTER_TYPE_VIRTUAL; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: properties->adapter_type = SPUDGPU_ADAPTER_TYPE_SOFTWARE; break;
    default: properties->adapter_type = SPUDGPU_ADAPTER_TYPE_OTHER; break;
    }

    // deviceUUID names the GPU; deviceLUID is the Windows LUID, where there
    // is one.
    VkPhysicalDeviceIDProperties idProperties = {0};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceVulkan12Properties vk12Properties = {0};
    vk12Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    vk12Properties.pNext = &idProperties;
    VkPhysicalDeviceProperties2 properties2 = {0};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &vk12Properties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    memcpy(properties->adapter_id, idProperties.deviceUUID, sizeof(properties->adapter_id));
    memcpy(properties->luid, idProperties.deviceLUID, sizeof(properties->luid));
    properties->luid_valid = idProperties.deviceLUIDValid == VK_TRUE;

    VkPhysicalDeviceMemoryProperties memProps = {0};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint64_t deviceLocalMemory = 0;
    uint64_t hostVisibleMemory = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            deviceLocalMemory += memProps.memoryHeaps[i].size;
        else
            hostVisibleMemory += memProps.memoryHeaps[i].size;
    }
    properties->dedicated_video_memory = deviceLocalMemory;
    // Vulkan does not tell a GPU-private system memory pool from shared
    // system memory the way DXGI does.
    properties->dedicated_system_memory = 0;
    properties->shared_system_memory    = hostVisibleMemory;
    // Vulkan doesn't say whether a GPU's memory is the system's. The device
    // type is what it does say, and an integrated GPU is the type that has
    // none of its own.
    properties->unified_memory = props->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    // Powers of two by Vulkan's rules, and never above 256.
    properties->uniform_buffer_offset_alignment = (uint32_t) props->limits.minUniformBufferOffsetAlignment;
    properties->storage_buffer_offset_alignment = (uint32_t) props->limits.minStorageBufferOffsetAlignment;
    properties->max_draw_indirect_count         = props->limits.maxDrawIndirectCount;
    // Descriptor pools have no device-wide count to keep under.
    properties->max_descriptors = UINT32_MAX;
    properties->max_samplers    = UINT32_MAX;

    VkPhysicalDeviceFeatures available = {0};
    vkGetPhysicalDeviceFeatures(physicalDevice, &available);
    if (available.depthBiasClamp) features |= SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP;
    if (available.independentBlend) features |= SPUDGPU_FEATURE_INDEPENDENT_BLEND;
    if (available.multiDrawIndirect) features |= SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT;
    if (available.drawIndirectFirstInstance) features |= SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
    if (available.imageCubeArray) features |= SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS;
    if (available.depthBounds) features |= SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST;
    if (available.geometryShader) features |= SPUDGPU_FEATURE_GEOMETRY_SHADER;
    if (available.tessellationShader) features |= SPUDGPU_FEATURE_TESSELLATION;

    VkPhysicalDeviceVulkan12Features vk12 = {0};
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features2 = {0};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vk12;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    // shaderOutputLayer covers SPIR-V 1.5's ShaderLayer; a vertex shader's
    // gl_Layer in earlier SPIR-V (glslc's default) is ShaderViewportIndexLayerEXT,
    // which needs the extension enabled even on Vulkan 1.2 and later; and
    // the extension, enabled beside VkPhysicalDeviceVulkan12Features, needs
    // shaderOutputViewportIndex on as well (VUID-VkDeviceCreateInfo-
    // ppEnabledExtensionNames-02835).
    if (vk12.shaderOutputLayer && vk12.shaderOutputViewportIndex &&
        spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME))
        features |= SPUDGPU_FEATURE_LAYERED_RENDERING;
    // Every descriptor indexing feature the bindless table switches on, and
    // room for the table's three classes in one stage.
    const uint32_t slots = SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
    if (vk12.descriptorIndexing && vk12.shaderSampledImageArrayNonUniformIndexing &&
        vk12.shaderStorageImageArrayNonUniformIndexing && vk12.shaderStorageBufferArrayNonUniformIndexing &&
        vk12.descriptorBindingSampledImageUpdateAfterBind && vk12.descriptorBindingStorageImageUpdateAfterBind &&
        vk12.descriptorBindingStorageBufferUpdateAfterBind && vk12.descriptorBindingPartiallyBound &&
        vk12.descriptorBindingVariableDescriptorCount && vk12.runtimeDescriptorArray &&
        vk12Properties.maxPerStageDescriptorUpdateAfterBindSampledImages >= slots &&
        vk12Properties.maxPerStageDescriptorUpdateAfterBindStorageImages >= slots &&
        vk12Properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= slots &&
        vk12Properties.maxDescriptorSetUpdateAfterBindSampledImages >= slots &&
        vk12Properties.maxDescriptorSetUpdateAfterBindStorageImages >= slots &&
        vk12Properties.maxDescriptorSetUpdateAfterBindStorageBuffers >= slots) {
        features |= SPUDGPU_FEATURE_BINDLESS;
        facts->bindless_max_sampled_images  = slots;
        facts->bindless_max_storage_images  = slots;
        facts->bindless_max_storage_buffers = slots;
    }

    // SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE needs a memory type that is the
    // GPU's and can be mapped, and is only offered where the GPU's memory is
    // the system's.
    bool deviceLocalHostVisibleType = false;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags both = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if ((memProps.memoryTypes[i].propertyFlags & both) == both)
            deviceLocalHostVisibleType = true;
    }
    if (properties->unified_memory && deviceLocalHostVisibleType)
        features |= SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
    // A heap of any kind is a vkAllocateMemory of that kind's memory type,
    // which every kind has wherever spudgpu_create_buffer can make it.
    features |= SPUDGPU_FEATURE_MAPPABLE_HEAPS;

    if (spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        features |= SPUDGPU_FEATURE_PRESENT;
        // Present scaling (spudgpu_get_surface_present_scalings): the KHR
        // extension, or its EXT original, where the instance has the surface
        // extension it needs and the driver has swapchainMaintenance1. No
        // feature bit of its own: what it offers is per surface.
        const char *maintenance1 = NULL;
        if (instance->_surface_maintenance1_khr &&
            spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            maintenance1 = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else if (instance->_surface_maintenance1_ext &&
                 spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            maintenance1 = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        if (maintenance1) {
            VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance1Features = {0};
            maintenance1Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
            VkPhysicalDeviceFeatures2 maintenance1Features2 = {0};
            maintenance1Features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            maintenance1Features2.pNext = &maintenance1Features;
            vkGetPhysicalDeviceFeatures2(physicalDevice, &maintenance1Features2);
            if (maintenance1Features.swapchainMaintenance1) {
                adapter->_swapchain_maintenance1_extension = maintenance1;
                adapter->_vkGetPhysicalDeviceSurfaceCapabilities2KHR = instance->_vkGetPhysicalDeviceSurfaceCapabilities2KHR;
            }
        }
    }

    // VK_EXT_mesh_shader: the extension being listed doesn't by itself mean
    // the driver has meshShader, so the feature bit is read too.
    if (spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures = {0};
        meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 meshFeatures2 = {0};
        meshFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        meshFeatures2.pNext = &meshFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &meshFeatures2);
        if (meshFeatures.meshShader) {
            features |= SPUDGPU_FEATURE_MESH_SHADING;
            VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties = {0};
            meshProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 meshProperties2 = {0};
            meshProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            meshProperties2.pNext = &meshProperties;
            vkGetPhysicalDeviceProperties2(physicalDevice, &meshProperties2);
            facts->mesh_max_output_vertices       = meshProperties.maxMeshOutputVertices;
            facts->mesh_max_output_primitives     = meshProperties.maxMeshOutputPrimitives;
            facts->mesh_max_workgroup_invocations = meshProperties.maxMeshWorkGroupInvocations;
        }
    }

    // Ray queries: both extensions and the one the first requires, each
    // listed, and both features on (a listed extension doesn't say the
    // driver has the feature). Deferred host operations are enabled with
    // them because VK_KHR_acceleration_structure requires it, and never
    // used: every build is recorded on the device.
    if (spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        spudgpuvulkan___device_extension_available(allocator, physicalDevice, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {0};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR structureFeatures = {0};
        structureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        structureFeatures.pNext = &rayQueryFeatures;
        VkPhysicalDeviceFeatures2 rayFeatures2 = {0};
        rayFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        rayFeatures2.pNext = &structureFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &rayFeatures2);
        if (structureFeatures.accelerationStructure && rayQueryFeatures.rayQuery) {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR structureProperties = {0};
            structureProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 rayProperties2 = {0};
            rayProperties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            rayProperties2.pNext = &structureProperties;
            vkGetPhysicalDeviceProperties2(physicalDevice, &rayProperties2);
            features |= SPUDGPU_FEATURE_RAY_QUERY;
            facts->ray_query_max_geometries           = structureProperties.maxGeometryCount;
            facts->ray_query_max_instances            = structureProperties.maxInstanceCount;
            facts->ray_query_max_primitives           = structureProperties.maxPrimitiveCount;
            facts->ray_query_scratch_offset_alignment = structureProperties.minAccelerationStructureScratchOffsetAlignment;
            // A structure's offset in its buffer is a multiple of 256
            // (VkAccelerationStructureCreateInfoKHR::offset). A placed one
            // is a buffer of its own at the heap's offset, which must also
            // meet the buffer's memory alignment, checked when it is placed.
            facts->ray_query_placement_alignment = 256;
        }
    }

    properties->supported_features = features;
    return true;
}

static void spudgpuvulkan___destroy_adapters(spudgpu_instance_vulkan *instance) {
    const SPUD_ALLOCATOR allocator = instance->head.allocator;
    for (uint32_t i = 0; i < instance->_adapter_count; i++) {
        spudgpu_adapter_vulkan *adapter = (spudgpu_adapter_vulkan *) instance->_adapters[i];
        if (!adapter) continue;
        SPUD_KILL(adapter);
        spud_string_free(&allocator, adapter->head.debug_name);
        SPUD_DELETE(&allocator, spudgpu_adapter_vulkan, adapter);
    }
    SPUD_DELETE_ARRAY(&allocator, spudgpu_adapter, instance->_adapters, instance->_adapter_count);
    instance->_adapters      = NULL;
    instance->_adapter_count = 0;
}

// Lists the physical devices the loader gives, in its order, that a device
// of this backend can be made on. Creates nothing on any of them.
static SPUDRESULT spudgpuvulkan___list_adapters(spudgpu_instance_vulkan *instance) {
    const SPUD_ALLOCATOR *allocator = &instance->head.allocator;
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance->_instance_vk, &count, NULL) != VK_SUCCESS)
        return SPUDRESULT_GPU_DEVICE_ENUMERATION_FAILURE;
    if (count == 0)
        return SPUD_SUCCESS;
    // A count vkEnumeratePhysicalDevices may lower; the arrays are freed
    // with the one they were made with.
    const uint32_t capacity = count;
    VkPhysicalDevice *physicalDevices = SPUD_NEW_ARRAY(allocator, VkPhysicalDevice, capacity);
    spudgpu_adapter *adapters = SPUD_NEW_ARRAY_ZEROED(allocator, spudgpu_adapter, capacity);
    if (!physicalDevices || !adapters) {
        SPUD_DELETE_ARRAY(allocator, VkPhysicalDevice, physicalDevices, capacity);
        SPUD_DELETE_ARRAY(allocator, spudgpu_adapter, adapters, capacity);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    VkResult r = vkEnumeratePhysicalDevices(instance->_instance_vk, &count, physicalDevices);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        SPUD_DELETE_ARRAY(allocator, VkPhysicalDevice, physicalDevices, capacity);
        SPUD_DELETE_ARRAY(allocator, spudgpu_adapter, adapters, capacity);
        return SPUDRESULT_GPU_DEVICE_ENUMERATION_FAILURE;
    }

    // The instance's from here, so a failure below frees what was made.
    instance->_adapters      = adapters;
    instance->_adapter_count = capacity;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < count; i++) {
        spudgpu_adapter_vulkan *adapter = SPUD_NEW_ZEROED(allocator, spudgpu_adapter_vulkan);
        if (!adapter) {
            spudgpuvulkan___destroy_adapters(instance);
            SPUD_DELETE_ARRAY(allocator, VkPhysicalDevice, physicalDevices, capacity);
            return SPUDRESULT_OUT_OF_MEMORY;
        }
        adapter->head.allocator      = instance->head.allocator;
        SPUD_BIRTH(adapter);
#if _DEBUG
        adapter->_debug_instance     = instance;
#endif
        adapter->_physical_device_vk = physicalDevices[i];
        atomic_init(&adapter->_in_use, false);
        if (!spudgpuvulkan___read_adapter_facts(allocator, instance, adapter)) {
            SPUD_DELETE(allocator, spudgpu_adapter_vulkan, adapter);
            continue;
        }
        adapters[listed++] = (spudgpu_adapter) adapter;
    }
    SPUD_DELETE_ARRAY(allocator, VkPhysicalDevice, physicalDevices, capacity);
    // The array keeps the capacity it was made with, and the count of the
    // listed: the slots past it are NULL, which destroy skips.
    instance->_adapter_count = listed;
    if (listed < capacity) {
        // Freed with the count it is kept with: shrink it to that.
        spudgpu_adapter *exact = listed ? SPUD_NEW_ARRAY(allocator, spudgpu_adapter, listed) : NULL;
        if (listed && !exact) {
            instance->_adapter_count = capacity;
            spudgpuvulkan___destroy_adapters(instance);
            return SPUDRESULT_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < listed; i++)
            exact[i] = adapters[i];
        SPUD_DELETE_ARRAY(allocator, spudgpu_adapter, adapters, capacity);
        instance->_adapters = exact;
    }
    return SPUD_SUCCESS;
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_create_instance(
    const spudgpu_instance_desc *desc,
    spudgpu_instance *out_instance) {
    const SPUDRESULT desc_result = spudgpu___internal_check_instance_desc(desc, out_instance);
    if (desc_result != SPUD_SUCCESS) return desc_result;
    // Only for the scratch arrays below, until the instance holds its copy.
    const SPUD_ALLOCATOR *allocator = desc->allocator;

    VkApplicationInfo appInfo = {0};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext = NULL;
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.pApplicationName = desc->application_name;
    appInfo.applicationVersion = desc->application_version;
    appInfo.pEngineName = desc->engine_name;
    appInfo.engineVersion = desc->engine_version;

    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pNext = NULL;
    createInfo.flags = 0;
    createInfo.pApplicationInfo = &appInfo;
    const char *extensionNames[8] = {0};
    bool surfaceMaintenance1Khr = false;
    bool surfaceMaintenance1Ext = false;
    spudgpuvulkan__determine_vulkan_extensions(allocator, &createInfo, extensionNames, &surfaceMaintenance1Khr, &surfaceMaintenance1Ext);

    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = NULL;

#if _DEBUG
    // Without this, invalid API usage tends to surface as an access violation
    // deep inside the ICD (e.g. igvk64.dll) instead of a VK_ERROR_* return,
    // since the driver isn't required to validate its own inputs.
    static const char *const validationLayer = "VK_LAYER_KHRONOS_validation";
    uint32_t availableLayerCount = 0;
    vkEnumerateInstanceLayerProperties(&availableLayerCount, NULL);
    const uint32_t availableLayerCapacity = availableLayerCount;
    VkLayerProperties *availableLayers = SPUD_NEW_ARRAY(allocator, VkLayerProperties, availableLayerCapacity);
    if (!availableLayers)
        return SPUDRESULT_OUT_OF_MEMORY;
    vkEnumerateInstanceLayerProperties(&availableLayerCount, availableLayers);
    for (uint32_t i = 0; i < availableLayerCount; i++) {
        if (strcmp(availableLayers[i].layerName, validationLayer) == 0) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
            break;
        }
    }
    SPUD_DELETE_ARRAY(allocator, VkLayerProperties, availableLayers, availableLayerCapacity);
#endif

    spudgpu_instance_vulkan *instance = SPUD_NEW_ZEROED(allocator, spudgpu_instance_vulkan);
    if (!instance)
        return SPUDRESULT_OUT_OF_MEMORY;
    instance->head.allocator = *desc->allocator;
    SPUD_BIRTH(instance);
    SPUD_QUARANTINE_INIT(&instance->_quarantine, &instance->head.allocator);
    if (vkCreateInstance(&createInfo, NULL, &instance->_instance_vk) != VK_SUCCESS) {
        SPUD_DELETE(allocator, spudgpu_instance_vulkan, instance);
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    }
    // Before the adapters are listed: they, and every device and surface
    // made later (each keeps a copy of this struct), read these.
    if (surfaceMaintenance1Khr || surfaceMaintenance1Ext) {
        instance->_vkGetPhysicalDeviceSurfaceCapabilities2KHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
            vkGetInstanceProcAddr(instance->_instance_vk, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        if (instance->_vkGetPhysicalDeviceSurfaceCapabilities2KHR) {
            instance->_surface_maintenance1_khr = surfaceMaintenance1Khr;
            instance->_surface_maintenance1_ext = surfaceMaintenance1Ext;
        }
    }
    const SPUDRESULT list_sr = spudgpuvulkan___list_adapters(instance);
    if (list_sr != SPUD_SUCCESS) {
        vkDestroyInstance(instance->_instance_vk, NULL);
        SPUD_DELETE(allocator, spudgpu_instance_vulkan, instance);
        return list_sr;
    }
    *out_instance = instance;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_destroy_instance(spudgpu_instance instance) {
    if (!instance) return SPUD_SUCCESS;
    if (SPUD_STALE(instance, "spudgpu_destroy_instance", "instance")) return SPUD_SUCCESS;
    SPUD_KILL(instance);
    const SPUD_ALLOCATOR allocator = instance->head.allocator;
    spudgpuvulkan___destroy_adapters(instance);
    vkDestroyInstance(instance->_instance_vk, NULL);
    SPUD_QUARANTINE_RELEASE(&instance->_quarantine);
    spud_string_free(&allocator, instance->head.debug_name);
    SPUD_DELETE(&allocator, spudgpu_instance_vulkan, instance);
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_enumerate_adapters(
    spudgpu_instance instance,
    const spudgpu_adapter **out_adapters,
    uint32_t *out_adapters_count) {
    if (!instance) return SPUDRESULT_GPU_INVALID_INSTANCE;
    if (SPUD_STALE(instance, "spudgpu_enumerate_adapters", "instance")) return SPUDRESULT_GPU_INVALID_INSTANCE;
    if (!out_adapters) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    if (!out_adapters_count) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_adapters       = instance->_adapters;
    *out_adapters_count = instance->_adapter_count;
    return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_adapter_properties(
    spudgpu_adapter adapter,
    SPUDGPU_ADAPTER_PROPERTIES *out_properties) {
    if (!adapter) return SPUDRESULT_GPU_INVALID_ADAPTER;
    if (SPUD_STALE(adapter, "spudgpu_get_adapter_properties", "adapter")) return SPUDRESULT_GPU_INVALID_ADAPTER;
    return spudgpu___internal_get_adapter_properties(&adapter->_facts, out_properties);
}

SPUDRESULT spudgpu_adapter_supports_surface(
    spudgpu_adapter adapter,
    spudgpu_surface surface,
    bool *out_supported) {
    if (!adapter) return SPUDRESULT_GPU_INVALID_ADAPTER;
    if (SPUD_STALE(adapter, "spudgpu_adapter_supports_surface", "adapter")) return SPUDRESULT_GPU_INVALID_ADAPTER;
    if (!surface) return SPUDRESULT_GPU_INVALID_SURFACE;
    if (SPUD_STALE(surface, "spudgpu_adapter_supports_surface", "surface")) return SPUDRESULT_GPU_INVALID_SURFACE;
    if (!out_supported) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_supported = false;
    // Without VK_KHR_swapchain no device on the adapter presents at all.
    if (!(adapter->_facts.properties.supported_features & SPUDGPU_FEATURE_PRESENT))
        return SPUD_SUCCESS;
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(adapter->_physical_device_vk, adapter->_family_direct, surface->_surface_vk, &supported) !=
        VK_SUCCESS)
        return SPUDRESULT_API_SPECIFIC_FAILURE;
    *out_supported = supported == VK_TRUE;
    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

// The queue family [type] is made from on [adapter]; a bundle's lists run
// inside a direct list, so its allocator is of the direct family.
uint32_t spudgpuvulkan___family_of(const spudgpu_adapter_vulkan *adapter, SPUDGPU_COMMAND_LIST_TYPE type) {
    switch (type) {
    case SPUDGPU_COMMAND_LIST_TYPE_COMPUTE: return adapter->_family_compute;
    case SPUDGPU_COMMAND_LIST_TYPE_COPY: return adapter->_family_copy;
    default: return adapter->_family_direct;
    }
}

static void spudgpuvulkan___destroy_queues(const SPUD_ALLOCATOR *allocator, struct spudgpu_command_queue_t **queues, uint32_t count) {
    if (!queues) return;
    for (uint32_t i = 0; i < count; i++) {
        spudgpu_command_queue_vulkan *queue = queues[i];
        if (!queue) continue;
        SPUD_KILL(queue);
        eq_array_free(&queue->_scratch);
        spud_string_free(allocator, queue->head.debug_name);
        SPUD_DELETE(allocator, spudgpu_command_queue_vulkan, queue);
    }
    SPUD_DELETE_ARRAY(allocator, struct spudgpu_command_queue_t *, queues, count);
}

// Hands out [count] queues of [family], from [first] on, as a new array at
// [*out_queues]: all or none.
static SPUDRESULT spudgpuvulkan___take_queues(
    spudgpu_device_vulkan *device,
    uint32_t family,
    uint32_t first,
    uint32_t count,
    struct spudgpu_command_queue_t ***out_queues) {
    const SPUD_ALLOCATOR *allocator = &device->head.allocator;
    *out_queues = NULL;
    if (count == 0) return SPUD_SUCCESS;
    struct spudgpu_command_queue_t **queues = SPUD_NEW_ARRAY_ZEROED(allocator, struct spudgpu_command_queue_t *, count);
    if (!queues) return SPUDRESULT_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        spudgpu_command_queue_vulkan *queue = SPUD_NEW_ZEROED(allocator, spudgpu_command_queue_vulkan);
        if (!queue) {
            spudgpuvulkan___destroy_queues(allocator, queues, count);
            return SPUDRESULT_OUT_OF_MEMORY;
        }
        queue->head.allocator      = device->head.allocator;
        SPUD_BIRTH(queue);
        queue->_queue_family_index = family;
        queue->_eq_allocator       = spud_eq_allocator(&queue->head.allocator);
        eq_array_init(&queue->_scratch, &queue->_eq_allocator);
        vkGetDeviceQueue(device->_logical_device_vk, family, first + i, &queue->_queue_vk);
        queues[i] = queue;
    }
    *out_queues = queues;
    return SPUD_SUCCESS;
}

// The VkDevice of [device], with its queues' families and the base set
// SpudGPU relies on, plus exactly what [desc] asks for.
static VkResult spudgpuvulkan___create_logical_device(
    spudgpu_device_vulkan *device,
    const spudgpu_device_desc *desc) {
    const spudgpu_adapter_vulkan *adapter = device->_adapter;
    const SPUD_ALLOCATOR *allocator       = &device->head.allocator;
    const SPUDGPU_FEATURES features       = desc->required_features;

    // One VkDeviceQueueCreateInfo per distinct family, its count the sum of
    // the kinds made from it. SpudGPU sets no priority: every queue the same
    // value, which in Vulkan's relative scheme is no preference.
    const uint32_t families[3] = {adapter->_family_direct, adapter->_family_compute, adapter->_family_copy};
    const uint32_t counts[3]   = {desc->direct_queue_count, desc->compute_queue_count, desc->copy_queue_count};
    VkDeviceQueueCreateInfo queueInfos[3];
    uint32_t queueInfoCount = 0;
    uint32_t largest        = 0;
    for (uint32_t k = 0; k < 3; k++) {
        if (counts[k] == 0) continue;
        uint32_t q = 0;
        while (q < queueInfoCount && queueInfos[q].queueFamilyIndex != families[k]) q++;
        if (q == queueInfoCount) {
            memset(&queueInfos[q], 0, sizeof(queueInfos[q]));
            queueInfos[q].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfos[q].queueFamilyIndex = families[k];
            queueInfoCount++;
        }
        queueInfos[q].queueCount += counts[k];
        if (queueInfos[q].queueCount > largest) largest = queueInfos[q].queueCount;
    }
    float *priorities = SPUD_NEW_ARRAY(allocator, float, largest);
    if (!priorities) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < largest; i++) priorities[i] = 1.0f;
    for (uint32_t q = 0; q < queueInfoCount; q++) queueInfos[q].pQueuePriorities = priorities;

    VkPhysicalDeviceFeatures deviceFeatures = {0};
    deviceFeatures.depthBiasClamp    = (features & SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP) ? VK_TRUE : VK_FALSE;
    deviceFeatures.independentBlend  = (features & SPUDGPU_FEATURE_INDEPENDENT_BLEND) ? VK_TRUE : VK_FALSE;
    deviceFeatures.multiDrawIndirect = (features & SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT) ? VK_TRUE : VK_FALSE;
    deviceFeatures.drawIndirectFirstInstance = (features & SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE) ? VK_TRUE : VK_FALSE;
    deviceFeatures.imageCubeArray    = (features & SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS) ? VK_TRUE : VK_FALSE;
    deviceFeatures.depthBounds       = (features & SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST) ? VK_TRUE : VK_FALSE;
    deviceFeatures.geometryShader    = (features & SPUDGPU_FEATURE_GEOMETRY_SHADER) ? VK_TRUE : VK_FALSE;
    deviceFeatures.tessellationShader = (features & SPUDGPU_FEATURE_TESSELLATION) ? VK_TRUE : VK_FALSE;

    // The base set: every buffer has an address (spudgpu_get_buffer_gpu_address),
    // spudgpu_fence is a timeline semaphore (spudgpuvulkansync.c).
    VkPhysicalDeviceVulkan12Features vk12Features = {0};
    vk12Features.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vk12Features.bufferDeviceAddress = VK_TRUE;
    vk12Features.timelineSemaphore   = VK_TRUE;
    vk12Features.shaderOutputLayer   = (features & SPUDGPU_FEATURE_LAYERED_RENDERING) ? VK_TRUE : VK_FALSE;
    vk12Features.shaderOutputViewportIndex = vk12Features.shaderOutputLayer;
    // The bindless table (spudgpuvulkandescriptors.c), and only with it.
    if (features & SPUDGPU_FEATURE_BINDLESS) {
        vk12Features.descriptorIndexing                            = VK_TRUE;
        vk12Features.shaderSampledImageArrayNonUniformIndexing     = VK_TRUE;
        vk12Features.shaderStorageImageArrayNonUniformIndexing     = VK_TRUE;
        vk12Features.shaderStorageBufferArrayNonUniformIndexing    = VK_TRUE;
        vk12Features.descriptorBindingSampledImageUpdateAfterBind  = VK_TRUE;
        vk12Features.descriptorBindingStorageImageUpdateAfterBind  = VK_TRUE;
        vk12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        vk12Features.descriptorBindingPartiallyBound               = VK_TRUE;
        vk12Features.descriptorBindingVariableDescriptorCount      = VK_TRUE;
        vk12Features.runtimeDescriptorArray                        = VK_TRUE;
    }

    // The base set too: a shader's vertex and instance index (card S22).
    VkPhysicalDeviceVulkan11Features vk11Features = {0};
    vk11Features.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vk11Features.shaderDrawParameters = VK_TRUE;
    vk12Features.pNext                = &vk11Features;

    VkPhysicalDeviceVulkan13Features vk13Features = {0};
    vk13Features.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vk13Features.pNext            = &vk12Features;
    vk13Features.dynamicRendering = VK_TRUE;
    vk13Features.synchronization2 = VK_TRUE;

    const char *deviceExtensions[7];
    uint32_t deviceExtensionCount = 0;
    if (features & SPUDGPU_FEATURE_PRESENT)
        deviceExtensions[deviceExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    // With PRESENT, the adapter's swapchain_maintenance1 if it has one: it
    // changes nothing for a swap chain that names no present scaling.
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maintenance1Features = {0};
    device->_swapchain_maintenance1 = (features & SPUDGPU_FEATURE_PRESENT) && adapter->_swapchain_maintenance1_extension;
    if (device->_swapchain_maintenance1) {
        deviceExtensions[deviceExtensionCount++] = adapter->_swapchain_maintenance1_extension;
        maintenance1Features.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
        maintenance1Features.swapchainMaintenance1 = VK_TRUE;
        maintenance1Features.pNext                 = vk13Features.pNext;
        vk13Features.pNext                         = &maintenance1Features;
    }
    if (features & SPUDGPU_FEATURE_LAYERED_RENDERING)
        deviceExtensions[deviceExtensionCount++] = VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME;
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = {0};
    if (features & SPUDGPU_FEATURE_MESH_SHADING) {
        deviceExtensions[deviceExtensionCount++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        // meshShader alone: taskShader stays off, since nothing here uses
        // amplification shaders yet, and the bits that need other features
        // (multiview, fragment shading rate) are never asked for.
        meshShaderFeatures.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        meshShaderFeatures.meshShader = VK_TRUE;
        meshShaderFeatures.pNext      = vk13Features.pNext;
        vk13Features.pNext            = &meshShaderFeatures;
    }
    // Ray queries: the two extensions and the one the first requires, and
    // the two features, only for a device created with the feature.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR structureFeatures = {0};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {0};
    if (features & SPUDGPU_FEATURE_RAY_QUERY) {
        deviceExtensions[deviceExtensionCount++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        deviceExtensions[deviceExtensionCount++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        deviceExtensions[deviceExtensionCount++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        structureFeatures.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        structureFeatures.accelerationStructure = VK_TRUE;
        rayQueryFeatures.sType                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryFeatures.rayQuery               = VK_TRUE;
        structureFeatures.pNext                 = &rayQueryFeatures;
        rayQueryFeatures.pNext                  = vk13Features.pNext;
        vk13Features.pNext                      = &structureFeatures;
    }

    VkDeviceCreateInfo createInfo = {0};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext                   = &vk13Features;
    createInfo.pQueueCreateInfos       = queueInfos;
    createInfo.queueCreateInfoCount    = queueInfoCount;
    createInfo.pEnabledFeatures        = &deviceFeatures;
    createInfo.enabledExtensionCount   = deviceExtensionCount;
    createInfo.ppEnabledExtensionNames = deviceExtensionCount ? deviceExtensions : NULL;

    const VkResult r = vkCreateDevice(device->_physical_device_vk, &createInfo, NULL, &device->_logical_device_vk);
    SPUD_DELETE_ARRAY(allocator, float, priorities, largest);
    if (r != VK_SUCCESS)
        device->_logical_device_vk = VK_NULL_HANDLE;
    return r;
}

// Frees [device] and everything it owns, and gives its adapter back. Every
// member is made or NULL, so a device that failed part way is freed too.
static void spudgpuvulkan___free_device(spudgpu_device_vulkan *device) {
    const SPUD_ALLOCATOR allocator = device->head.allocator;
    spudgpuvulkan___destroy_queues(&allocator, device->_cmd_queues_direct, device->_cmd_queue_count_direct);
    spudgpuvulkan___destroy_queues(&allocator, device->_cmd_queues_compute, device->_cmd_queue_count_compute);
    spudgpuvulkan___destroy_queues(&allocator, device->_cmd_queues_copy, device->_cmd_queue_count_copy);
    spudgpuvulkan___destroy_bindless_state((spudgpu_device) device);
    if (device->_logical_device_vk)
        vkDestroyDevice(device->_logical_device_vk, NULL);
#if _DEBUG
    if (device->_shared)
        spud_quarantine_release(&device->_shared->quarantine);
#endif
    SPUD_DELETE(&allocator, spudgpu_device_shared_vulkan, device->_shared);
    spudgpu_adapter_vulkan *adapter = device->_adapter;
    spud_string_free(&allocator, device->head.debug_name);
    SPUD_RETIRE(SPUD_QUARANTINE_OF(device), &allocator, spudgpu_device_vulkan, device);
    atomic_store(&adapter->_in_use, false);
}

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu_create_device(
    spudgpu_adapter adapter,
    const spudgpu_device_desc *desc,
    spudgpu_device *out_device) {
    if (!adapter) return SPUDRESULT_GPU_INVALID_ADAPTER;
    if (SPUD_STALE(adapter, "spudgpu_create_device", "adapter")) return SPUDRESULT_GPU_INVALID_ADAPTER;
    const SPUDRESULT check_sr = spudgpu___internal_check_device_desc(&adapter->_facts, desc, out_device);
    if (check_sr != SPUD_SUCCESS) return check_sr;
    // Last: one device per adapter at a time.
    if (atomic_exchange(&adapter->_in_use, true)) return SPUDRESULT_GPU_ADAPTER_IN_USE;

    const SPUD_ALLOCATOR *allocator = &adapter->head.allocator;
    // Zeroed: every member a failure below leaves unset is NULL, which
    // spudgpuvulkan___free_device frees.
    spudgpu_device_vulkan *device = SPUD_NEW_ZEROED(allocator, spudgpu_device_vulkan);
    if (!device) {
        atomic_store(&adapter->_in_use, false);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    device->head.allocator              = adapter->head.allocator;
    SPUD_BIRTH_IN(device, &adapter->_debug_instance->_quarantine);
    device->_adapter                    = adapter;
    device->_physical_device_vk         = adapter->_physical_device_vk;
    device->_features                   = desc->required_features;
    device->_properties_vk              = adapter->_properties_vk;
    device->_graphics_queue_family_index = adapter->_family_direct;

    device->_shared = SPUD_NEW_ZEROED(allocator, spudgpu_device_shared_vulkan);
    if (!device->_shared) {
        spudgpuvulkan___free_device(device);
        return SPUDRESULT_OUT_OF_MEMORY;
    }
    if (!spudgpu___internal_budget_init(&device->_shared->budget, desc->max_descriptors, desc->max_samplers)) {
        spudgpuvulkan___free_device(device);
        return SPUDRESULT_GENERAL_FAILURE;
    }
#if _DEBUG
    spud_quarantine_init(&device->_shared->quarantine, &device->head.allocator);
    if (!spudgpu___internal_live_init(&device->_shared->live)) {
        spudgpu___internal_budget_destroy(&device->_shared->budget);
        spudgpuvulkan___free_device(device);
        return SPUDRESULT_GENERAL_FAILURE;
    }
#endif

    SPUDRESULT sr = SPUD_SUCCESS;
    const VkResult r = spudgpuvulkan___create_logical_device(device, desc);
    if (r == VK_ERROR_OUT_OF_HOST_MEMORY) sr = SPUDRESULT_OUT_OF_MEMORY;
    else if (r != VK_SUCCESS) sr = SPUDRESULT_API_SPECIFIC_FAILURE;

    if (sr == SPUD_SUCCESS) {
        // NULL where the instance has no VK_EXT_debug_utils: the label calls
        // then record nothing.
        device->_vkCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)
            vkGetDeviceProcAddr(device->_logical_device_vk, "vkCmdBeginDebugUtilsLabelEXT");
        device->_vkCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)
            vkGetDeviceProcAddr(device->_logical_device_vk, "vkCmdEndDebugUtilsLabelEXT");
        if (desc->required_features & SPUDGPU_FEATURE_MESH_SHADING)
            device->_vkCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
                vkGetDeviceProcAddr(device->_logical_device_vk, "vkCmdDrawMeshTasksEXT");
        if (desc->required_features & SPUDGPU_FEATURE_RAY_QUERY) {
            VkDevice vk = device->_logical_device_vk;
            device->_vkCreateAccelerationStructureKHR =
                (PFN_vkCreateAccelerationStructureKHR) vkGetDeviceProcAddr(vk, "vkCreateAccelerationStructureKHR");
            device->_vkDestroyAccelerationStructureKHR =
                (PFN_vkDestroyAccelerationStructureKHR) vkGetDeviceProcAddr(vk, "vkDestroyAccelerationStructureKHR");
            device->_vkGetAccelerationStructureBuildSizesKHR =
                (PFN_vkGetAccelerationStructureBuildSizesKHR) vkGetDeviceProcAddr(vk, "vkGetAccelerationStructureBuildSizesKHR");
            device->_vkGetAccelerationStructureDeviceAddressKHR =
                (PFN_vkGetAccelerationStructureDeviceAddressKHR) vkGetDeviceProcAddr(vk, "vkGetAccelerationStructureDeviceAddressKHR");
            device->_vkCmdBuildAccelerationStructuresKHR =
                (PFN_vkCmdBuildAccelerationStructuresKHR) vkGetDeviceProcAddr(vk, "vkCmdBuildAccelerationStructuresKHR");
            device->_vkCmdCopyAccelerationStructureKHR =
                (PFN_vkCmdCopyAccelerationStructureKHR) vkGetDeviceProcAddr(vk, "vkCmdCopyAccelerationStructureKHR");
            device->_vkCmdWriteAccelerationStructuresPropertiesKHR =
                (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR) vkGetDeviceProcAddr(vk, "vkCmdWriteAccelerationStructuresPropertiesKHR");
            // A device made with the extension has every one of them; a
            // driver that broke that fails the device here, never a call.
            if (!device->_vkCreateAccelerationStructureKHR || !device->_vkDestroyAccelerationStructureKHR ||
                !device->_vkGetAccelerationStructureBuildSizesKHR || !device->_vkGetAccelerationStructureDeviceAddressKHR ||
                !device->_vkCmdBuildAccelerationStructuresKHR || !device->_vkCmdCopyAccelerationStructureKHR ||
                !device->_vkCmdWriteAccelerationStructuresPropertiesKHR)
                sr = SPUDRESULT_API_SPECIFIC_FAILURE;
        }

        // Kinds made from one family take consecutive queues of it, in the
        // order direct, compute, copy.
        const uint32_t families[3] = {adapter->_family_direct, adapter->_family_compute, adapter->_family_copy};
        const uint32_t counts[3]   = {desc->direct_queue_count, desc->compute_queue_count, desc->copy_queue_count};
        uint32_t first[3];
        for (uint32_t k = 0; k < 3; k++) {
            first[k] = 0;
            for (uint32_t j = 0; j < k; j++)
                if (families[j] == families[k]) first[k] += counts[j];
        }
        if (sr == SPUD_SUCCESS)
            sr = spudgpuvulkan___take_queues(device, families[0], first[0], counts[0], &device->_cmd_queues_direct);
        if (sr == SPUD_SUCCESS) {
            device->_cmd_queue_count_direct = counts[0];
            sr = spudgpuvulkan___take_queues(device, families[1], first[1], counts[1], &device->_cmd_queues_compute);
        }
        if (sr == SPUD_SUCCESS) {
            device->_cmd_queue_count_compute = counts[1];
            sr = spudgpuvulkan___take_queues(device, families[2], first[2], counts[2], &device->_cmd_queues_copy);
        }
        if (sr == SPUD_SUCCESS)
            device->_cmd_queue_count_copy = counts[2];
    }
    // The bindless table comes out of the budget, which the desc check found
    // holds it.
    if (sr == SPUD_SUCCESS && (desc->required_features & SPUDGPU_FEATURE_BINDLESS)) {
        const uint64_t table = (uint64_t) adapter->_facts.bindless_max_sampled_images +
                               adapter->_facts.bindless_max_storage_images + adapter->_facts.bindless_max_storage_buffers;
        sr = spudgpu___internal_budget_take(&device->_shared->budget, table, 0);
        if (sr == SPUD_SUCCESS)
            sr = spudgpuvulkan___create_bindless_state((spudgpu_device) device);
    }
    if (sr == SPUD_SUCCESS && spud_debug_name_set(device, desc->debug_name) != SPUD_SUCCESS)
        sr = SPUDRESULT_OUT_OF_MEMORY;
    if (sr != SPUD_SUCCESS) {
#if _DEBUG
        spudgpu___internal_live_destroy(&device->_shared->live);
#endif
        spudgpu___internal_budget_destroy(&device->_shared->budget);
        spudgpuvulkan___free_device(device);
        return sr;
    }

    *out_device = device;
    return SPUD_SUCCESS;
}

void spudgpu_destroy_device(spudgpu_device device) {
    if (!device) return;
    if (SPUD_STALE(device, "spudgpu_destroy_device", "device")) return;
#if _DEBUG
    spudgpu___internal_live_check_none(&device->_shared->live, device->head.debug_name);
    spudgpu___internal_live_destroy(&device->_shared->live);
#endif
    spudgpu___internal_budget_destroy(&device->_shared->budget);
    SPUD_KILL(device);
    spudgpuvulkan___free_device(device);
}

spudgpu_adapter spudgpu_get_device_adapter(spudgpu_device device) {
    if (!device) return NULL;
    if (SPUD_STALE(device, "spudgpu_get_device_adapter", "device")) return NULL;
    return (spudgpu_adapter) device->_adapter;
}

SPUDGPU_FEATURES spudgpu_get_device_features(spudgpu_device device) {
    if (!device) return 0;
    if (SPUD_STALE(device, "spudgpu_get_device_features", "device")) return 0;
    return device->_features;
}

SPUDGPU_NATIVE_API spudgpu_get_native_gpu_api(spudgpu_instance instance) {
    if (!instance) return SPUDGPU_NATIVE_API_NONE;
    if (SPUD_STALE(instance, "spudgpu_get_native_gpu_api", "instance")) return SPUDGPU_NATIVE_API_NONE;
    return SPUDGPU_NATIVE_API_VULKAN;
}

#if __cplusplus
}
#endif

#endif //SPUDGPU_COMPILE_VULKAN_API
