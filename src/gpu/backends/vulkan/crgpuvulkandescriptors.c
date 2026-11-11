
#if CRGPU_COMPILE_VULKAN_API

#include <vulkan/vulkan.h>
#include "crgpu.h"
#include "crgpuvulkan.h"
#include <stdlib.h>
#include <string.h>
#include "../../../crstructbase.h"

#if __cplusplus
extern "C" {

#endif

static VkDescriptorType crgpuvulkan___descriptor_type_internal(
    CRGPU_DESCRIPTOR_TYPE type) {
    switch (type) {
        case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case CRGPU_DESCRIPTOR_TYPE_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

static VkFilter crgpuvulkan___filter_internal(CRGPU_FILTER filter) {
    return filter == CRGPU_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static VkSamplerMipmapMode crgpuvulkan___mipmap_mode_internal(CRGPU_FILTER filter) {
    return filter == CRGPU_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

static VkSamplerAddressMode crgpuvulkan___address_mode_internal(CRGPU_ADDRESS_MODE mode) {
    switch (mode) {
        case CRGPU_ADDRESS_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case CRGPU_ADDRESS_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case CRGPU_ADDRESS_MODE_REPEAT:
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

CR_RESULT crgpu_sampler_create(
    crgpu_device device,
    const crgpu_sampler_desc *desc,
    crgpu_sampler *out_sampler) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_sampler_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SAMPLER_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_sampler) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    VkSamplerCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = crgpuvulkan___filter_internal(desc->mag_filter);
    info.minFilter = crgpuvulkan___filter_internal(desc->min_filter);
    info.mipmapMode = crgpuvulkan___mipmap_mode_internal(desc->mipmap_filter);
    info.addressModeU = crgpuvulkan___address_mode_internal(desc->address_mode_u);
    info.addressModeV = crgpuvulkan___address_mode_internal(desc->address_mode_v);
    info.addressModeW = crgpuvulkan___address_mode_internal(desc->address_mode_w);
    info.mipLodBias = desc->mip_lod_bias;
    info.minLod = desc->min_lod;
    info.maxLod = desc->max_lod;
    info.anisotropyEnable = desc->max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = desc->max_anisotropy;
    // Carrottus GPU's compare ops are Vulkan's, value for value.
    info.compareEnable = desc->compare_enable ? VK_TRUE : VK_FALSE;
    info.compareOp = desc->compare_enable ? (VkCompareOp)desc->compare_op : VK_COMPARE_OP_ALWAYS;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    crgpu_sampler_vulkan *pResult = CR_NEW_ZEROED(&device->head.allocator, crgpu_sampler_vulkan);
    if (!pResult) return CR_RESULT_OUT_OF_MEMORY;
    pResult->head.allocator = device->head.allocator;
    CR_BIRTH_IN(pResult, &device->_shared->quarantine);
    pResult->_device = *device;

    if (vkCreateSampler(device->_logical_device_vk, &info, NULL, &pResult->_sampler_vk) != VK_SUCCESS) {
        CR_DELETE(&device->head.allocator, crgpu_sampler_vulkan, pResult);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    // From here crgpu_sampler_destroy frees it, and takes the count back.
    CRGPU___LIVE_ADD(&device->_shared->live, CRGPU___OBJECT_SAMPLER);
    if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
        crgpu_sampler_destroy(pResult);
        return CR_RESULT_OUT_OF_MEMORY;
    }

    *out_sampler = pResult;
    return CR_SUCCESS;
}

void crgpu_sampler_destroy(crgpu_sampler sampler) {
    if (!sampler) return;
    if (CR_STALE(sampler, "crgpu_sampler_destroy", "sampler")) return;
    vkDestroySampler(sampler->_device._logical_device_vk, sampler->_sampler_vk, NULL);
    CRGPU___LIVE_REMOVE(&sampler->_device._shared->live, CRGPU___OBJECT_SAMPLER);
    CR_KILL(sampler);
    const CR_ALLOCATOR allocator = sampler->head.allocator;
    cr_string_free(&allocator, sampler->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(sampler), &allocator, crgpu_sampler_vulkan, sampler);
}

CR_RESULT crgpu_descriptor_set_layout_create(
    crgpu_device device,
    const crgpu_descriptor_set_layout_desc *desc,
	crgpu_descriptor_set_layout *out_layout) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_descriptor_set_layout_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_layout) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    if (desc->binding_count > CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET) return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS;

    crgpu_descriptor_set_layout_vulkan result = {0};
    result.head.allocator = device->head.allocator;
    CR_BIRTH_IN(&result, &device->_shared->quarantine);
    result._device = *device;
    result._desc = *desc;

    VkDevice vk_device = result._device._logical_device_vk;

    // Translate each binding entry to VkDescriptorSetLayoutBinding.
    VkDescriptorSetLayoutBinding vk_bindings[CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET] = {0};
    for (uint32_t i = 0; i < desc->binding_count; i++) {
        const crgpu_descriptor_binding_desc *b = &desc->bindings[i];

        // An unknown type, or one the device has no feature for (an
        // acceleration structure needs the extension), the same way on every
        // backend.
        const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(b->descriptor_type, device->_features);
        if (type_sr != CR_SUCCESS) return type_sr;
        VkDescriptorType vk_type = crgpuvulkan___descriptor_type_internal(b->descriptor_type);

        vk_bindings[i].binding = b->binding;
        vk_bindings[i].descriptorType = vk_type;
        vk_bindings[i].descriptorCount = b->count;
        // Translated: Carrottus GPU's stage bits are not Vulkan's (FRAGMENT is
        // 1 << 1, which is VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT).
        vk_bindings[i].stageFlags = crgpuvulkan___shader_stages(b->stage_flags);
        // Immutable samplers aren't supported yet — see the "no static/immutable
        // sampler support" gap in ../../../CLAUDE.md. Every sampler goes through
        // the dynamic, descriptor-bound crgpu_sampler path today.
        vk_bindings[i].pImmutableSamplers = NULL;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {0};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = desc->binding_count;
    layoutInfo.pBindings = vk_bindings;

    if (vkCreateDescriptorSetLayout(
        vk_device, &layoutInfo, NULL,
        &result._layout_vk) != VK_SUCCESS) {
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

#if _DEBUG
    // If VK_EXT_debug_utils is loaded, tag the layout object for RenderDoc / validation layers.
    if (desc->debug_name) {
        VkDebugUtilsObjectNameInfoEXT nameInfo = {0};
        nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;
        nameInfo.objectHandle = (uint64_t) result._layout_vk;
        nameInfo.pObjectName = desc->debug_name;
        // Best-effort: ignore the return value — not all drivers load the extension.
        PFN_vkSetDebugUtilsObjectNameEXT fn =
                (PFN_vkSetDebugUtilsObjectNameEXT) vkGetDeviceProcAddr(
                    vk_device, "vkSetDebugUtilsObjectNameEXT");
        if (fn) fn(vk_device, &nameInfo);
    }
#endif

    crgpu_descriptor_set_layout_vulkan *pResult =
            CR_NEW(&result.head.allocator, crgpu_descriptor_set_layout_vulkan);
    if (!pResult) {
        vkDestroyDescriptorSetLayout(vk_device, result._layout_vk, NULL);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(crgpu_descriptor_set_layout_vulkan));
    CRGPU___LIVE_ADD(&device->_shared->live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
    if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
        crgpu_descriptor_set_layout_destroy(pResult);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    pResult->_desc.debug_name = pResult->head.debug_name;

    *out_layout = pResult;
    return CR_SUCCESS;
}

void crgpu_descriptor_set_layout_destroy(
    crgpu_descriptor_set_layout layout) {
    if (!layout) return;
    if (CR_STALE(layout, "crgpu_descriptor_set_layout_destroy", "layout")) return;
    vkDestroyDescriptorSetLayout(
        layout->_device._logical_device_vk,
        layout->_layout_vk,
        NULL);
    CRGPU___LIVE_REMOVE(&layout->_device._shared->live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
    CR_KILL(layout);
    const CR_ALLOCATOR allocator = layout->head.allocator;
    cr_string_free(&allocator, layout->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(layout), &allocator, crgpu_descriptor_set_layout_vulkan, layout);
}

CR_RESULT crgpu_descriptor_pool_create(
    crgpu_device device,
    const crgpu_descriptor_pool_desc *desc,
	crgpu_descriptor_pool *out_pool) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_descriptor_pool_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_pool) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    if (desc->pool_size_count > CRGPU_MAX_DESCRIPTOR_POOL_SIZES) return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS;

    crgpu_descriptor_pool_vulkan result = {0};
    result.head.allocator = device->head.allocator;
    CR_BIRTH_IN(&result, &device->_shared->quarantine);
    result._device = *device;
    result._desc = *desc;

    VkDevice vk_device = result._device._logical_device_vk;

    VkDescriptorPoolSize vk_pool_sizes[CRGPU_MAX_DESCRIPTOR_POOL_SIZES] = {0};
    for (uint32_t i = 0; i < desc->pool_size_count; i++) {
        const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(desc->pool_sizes[i].descriptor_type, device->_features);
        if (type_sr != CR_SUCCESS) return type_sr;
        VkDescriptorType vk_type =
                crgpuvulkan___descriptor_type_internal(desc->pool_sizes[i].descriptor_type);

        vk_pool_sizes[i].type = vk_type;
        vk_pool_sizes[i].descriptorCount = desc->pool_sizes[i].count;
    }

    // What the pool takes out of the device's budget, the same count on
    // every backend; given back when the pool is destroyed.
    crgpu___internal_pool_descriptor_counts(desc, &result._budget_descriptors, &result._budget_samplers);
    const CR_RESULT budget_sr =
        crgpu___internal_budget_take(&device->_shared->budget, result._budget_descriptors, result._budget_samplers);
    if (budget_sr != CR_SUCCESS) return budget_sr;

    VkDescriptorPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // FREE_DESCRIPTOR_SET_BIT is not set: individual free is not supported,
    // which is the intended usage — reset the whole pool each frame instead.
    poolInfo.maxSets = desc->max_sets;
    poolInfo.poolSizeCount = desc->pool_size_count;
    poolInfo.pPoolSizes = vk_pool_sizes;

    if (vkCreateDescriptorPool(
        vk_device, &poolInfo, NULL,
        &result._pool_vk) != VK_SUCCESS) {
        crgpu___internal_budget_give_back(&device->_shared->budget, result._budget_descriptors, result._budget_samplers);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

#if _DEBUG
    if (desc->debug_name) {
        VkDebugUtilsObjectNameInfoEXT nameInfo = {0};
        nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType = VK_OBJECT_TYPE_DESCRIPTOR_POOL;
        nameInfo.objectHandle = (uint64_t) result._pool_vk;
        nameInfo.pObjectName = desc->debug_name;
        CR_RESULT sr = CR_SUCCESS;
        PFN_vkSetDebugUtilsObjectNameEXT fn =
                (PFN_vkSetDebugUtilsObjectNameEXT) vkGetDeviceProcAddr(
                    vk_device, "vkSetDebugUtilsObjectNameEXT");
		if (fn)
			if (fn(vk_device, &nameInfo))
				sr = CR_RESULT_API_SPECIFIC_FAILURE;
		if (sr != CR_SUCCESS) {
			vkDestroyDescriptorPool(vk_device, result._pool_vk, NULL);
			crgpu___internal_budget_give_back(&device->_shared->budget, result._budget_descriptors, result._budget_samplers);
			return sr;
		}
	}
#endif

    crgpu_descriptor_pool_vulkan *pResult = CR_NEW(&result.head.allocator, crgpu_descriptor_pool_vulkan);
    if (!pResult) {
        vkDestroyDescriptorPool(vk_device, result._pool_vk, NULL);
        crgpu___internal_budget_give_back(&device->_shared->budget, result._budget_descriptors, result._budget_samplers);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    memcpy(pResult, &result, sizeof(crgpu_descriptor_pool_vulkan));
    // From here crgpu_descriptor_pool_destroy frees it, gives its budget
    // back and takes the count back.
    CRGPU___LIVE_ADD(&device->_shared->live, CRGPU___OBJECT_DESCRIPTOR_POOL);
    if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
        crgpu_descriptor_pool_destroy(pResult);
        return CR_RESULT_OUT_OF_MEMORY;
    }
    pResult->_desc.debug_name = pResult->head.debug_name;

    *out_pool = pResult;
    return CR_SUCCESS;
}

// Frees every set handle [pool] made, names included: the Vulkan sets go
// with the pool's reset or destruction.
static void crgpuvulkan___release_descriptor_sets(crgpu_descriptor_pool_vulkan *pool) {
    crgpu_descriptor_set_vulkan *set = pool->_sets;
    while (set) {
        crgpu_descriptor_set_vulkan *next = set->_next;
        CR_KILL(set);
        const CR_ALLOCATOR allocator = set->head.allocator;
        cr_string_free(&allocator, set->head.debug_name);
        CR_RETIRE(CR_QUARANTINE_OF(set), &allocator, crgpu_descriptor_set_vulkan, set);
        set = next;
    }
    pool->_sets = NULL;
}

void crgpu_descriptor_pool_reset(
    crgpu_descriptor_pool pool) {
    if (!pool) return;
    if (CR_STALE(pool, "crgpu_descriptor_pool_reset", "pool")) return;
    // Bulk-free all sets allocated from this pool. The 'flags' parameter is reserved and must be 0.
	vkResetDescriptorPool(pool->_device._logical_device_vk, pool->_pool_vk, 0);
	crgpuvulkan___release_descriptor_sets(pool);
}

void crgpu_descriptor_pool_destroy(
    crgpu_descriptor_pool pool) {
    if (!pool) return;
    if (CR_STALE(pool, "crgpu_descriptor_pool_destroy", "pool")) return;
    // vkDestroyDescriptorPool implicitly frees all descriptor sets allocated from it.
    vkDestroyDescriptorPool(
        pool->_device._logical_device_vk,
        pool->_pool_vk,
        NULL);
    crgpuvulkan___release_descriptor_sets(pool);
    crgpu___internal_budget_give_back(&pool->_device._shared->budget, pool->_budget_descriptors, pool->_budget_samplers);
    CRGPU___LIVE_REMOVE(&pool->_device._shared->live, CRGPU___OBJECT_DESCRIPTOR_POOL);
    CR_KILL(pool);
    const CR_ALLOCATOR allocator = pool->head.allocator;
    cr_string_free(&allocator, pool->head.debug_name);
    CR_RETIRE(CR_QUARANTINE_OF(pool), &allocator, crgpu_descriptor_pool_vulkan, pool);
}

CR_RESULT crgpu_descriptor_set_create(
    crgpu_device device,
    const crgpu_descriptor_set_desc *desc,
    crgpu_descriptor_set *out_sets) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_descriptor_set_create", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!desc) return CR_RESULT_NULL_DESC;
    const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC);
    if (base_sr != CR_SUCCESS) return base_sr;
    if (!out_sets) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    if (!desc->pool) return CR_RESULT_GPU_INVALID_DESCRIPTOR_POOL;
	if (!desc->set_count)
		return CR_RESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS;
	if (desc->set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS;

    VkDevice vk_device = desc->pool->_device._logical_device_vk;

    // Gather the raw VkDescriptorSetLayout handles from the typed wrappers.
    VkDescriptorSetLayout vk_layouts[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS] = {0};
    for (uint32_t i = 0; i < desc->set_count; i++) {
        if (!desc->set_layouts[i]) return CR_RESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT;
        crgpu_descriptor_set_layout_vulkan *vkLayout =
                (crgpu_descriptor_set_layout_vulkan *) desc->set_layouts[i];
        vk_layouts[i] = vkLayout->_layout_vk;
    }

    // Allocate all sets in a single driver call.
    VkDescriptorSet vk_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS] = {0};

    VkDescriptorSetAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = desc->pool->_pool_vk;
    allocInfo.descriptorSetCount = desc->set_count;
    allocInfo.pSetLayouts = vk_layouts;

    if (vkAllocateDescriptorSets(vk_device, &allocInfo, vk_sets) != VK_SUCCESS) {
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    // Wrap each raw VkDescriptorSet in a heap-allocated Carrottus GPU handle.
    for (uint32_t i = 0; i < desc->set_count; i++) {
        crgpu_descriptor_set_vulkan *pSet =
                CR_NEW(&desc->pool->head.allocator, crgpu_descriptor_set_vulkan);
        if (!pSet) {
            // Partial allocation — caller's out_sets is partially populated.
            // The pool reset at the next frame will reclaim the Vulkan-side
            // sets, and the handles made so far, which are on its list.
            return CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;
        }
        pSet->head.debug_name = NULL;
        pSet->head.allocator = desc->pool->head.allocator;
        CR_BIRTH_IN(pSet, &desc->pool->_device._shared->quarantine);
        pSet->_pool = *desc->pool;
        pSet->_set_vk = vk_sets[i];
        pSet->_next = desc->pool->_sets;
        desc->pool->_sets = pSet;
        out_sets[i] = (crgpu_descriptor_set) pSet;
    }

    return CR_SUCCESS;
}


void crgpu_device_update_descriptor_sets(
    crgpu_device device,
    const crgpu_write_descriptor_set *writes,
    uint32_t write_count) {
    if (!device || !writes || !write_count) return;
    if (CR_STALE(device, "crgpu_device_update_descriptor_sets", "device")) return;
    for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i)
        if (cr_struct_check(&writes[writes_i].base, CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET) != CR_SUCCESS)
            return;
    // A storage view of a cube is a 2D array view of its faces on every
    // backend (crgpu.h): D3D12 has no storage cube. Checked before the
    // first write, so a refused call writes nothing.
    for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i) {
        const crgpu_write_descriptor_set *write = &writes[writes_i];
        if (write->descriptor_type != CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || !write->image_info)
            continue;
        for (uint32_t k = 0; k < write->descriptor_count; ++k) {
            const crgpu_image_view view = write->image_info[k].image_view;
            if (view && (view->_desc.type == CRGPU_IMAGE_VIEW_TYPE_CUBE || view->_desc.type == CRGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY))
                return;
        }
    }
    // Acceleration structure writes: the feature, and every element a live
    // top level (crgpu.h), checked the same way on every backend.
    if (!crgpu___internal_check_acceleration_structure_writes(device->_features, writes, write_count))
        return;

    // We translate each Carrottus GPU write into a VkWriteDescriptorSet on the stack.
    // For larger write counts a heap allocation would be safer, but the per-frame
    // update pattern rarely exceeds a few dozen writes.
    VkWriteDescriptorSet vk_writes[64] = {0};
    VkDescriptorBufferInfo vk_buf_infos[64] = {0};
    VkDescriptorImageInfo vk_img_infos[64] = {0};

    uint32_t clamped = write_count < 64 ? write_count : 64;

    for (uint32_t i = 0; i < clamped; i++) {
        const crgpu_write_descriptor_set *w = &writes[i];
        if (!w->dst_set) continue;

        crgpu_descriptor_set_vulkan *vkSet =
                (crgpu_descriptor_set_vulkan *) w->dst_set;

        VkDescriptorType vk_type =
                crgpuvulkan___descriptor_type_internal(w->descriptor_type);
        if (vk_type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        // Written below, on their own: each needs its handles in an array.
        if (w->descriptor_type == CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE) continue;

        vk_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vk_writes[i].dstSet = vkSet->_set_vk;
        vk_writes[i].dstBinding = w->dst_binding;
        vk_writes[i].dstArrayElement = w->dst_array_element;
        vk_writes[i].descriptorCount = w->descriptor_count;
        vk_writes[i].descriptorType = vk_type;

        if (w->buffer_info) {
            crgpu_buffer_vulkan *vkBuf =
                    (crgpu_buffer_vulkan *) w->buffer_info->buffer;
            vk_buf_infos[i].buffer = vkBuf->_buffer_vk;
            vk_buf_infos[i].offset = w->buffer_info->offset;
            // A range of 0 from the caller means "bind the whole buffer".
            vk_buf_infos[i].range = w->buffer_info->range
                                        ? w->buffer_info->range
                                        : VK_WHOLE_SIZE;
            vk_writes[i].pBufferInfo = &vk_buf_infos[i];
        }

        if (w->image_info) {
            crgpu_image_view_vulkan *vkView =
                    (crgpu_image_view_vulkan *) w->image_info->image_view;
            vk_img_infos[i].imageView = vkView->_image_view_vk;
            // A Carrottus GPU layout, translated: the two enums' values differ
            // (SHADER_READ_ONLY is 4, VkImageLayout 4 is a depth layout).
            vk_img_infos[i].imageLayout = cr_layout_to_vk(w->image_info->image_layout);
            // Non-NULL only for COMBINED_IMAGE_SAMPLER writes.
            vk_img_infos[i].sampler = w->sampler
                                           ? ((crgpu_sampler_vulkan *) w->sampler)->_sampler_vk
                                           : VK_NULL_HANDLE;
            vk_writes[i].pImageInfo = &vk_img_infos[i];
        } else if (w->sampler) {
            // Pure SAMPLER descriptor — no image_info involved.
            vk_img_infos[i].sampler = ((crgpu_sampler_vulkan *) w->sampler)->_sampler_vk;
            vk_writes[i].pImageInfo = &vk_img_infos[i];
        }
    }

    // The writes skipped above stay zeroed and are left out: Vulkan takes a
    // packed array, so the written ones are moved together first.
    uint32_t packed = 0;
    for (uint32_t i = 0; i < clamped; i++) {
        if (vk_writes[i].sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET) continue;
        if (packed != i) {
            vk_writes[packed] = vk_writes[i];
            // The info pointers point into the per-write arrays at index i,
            // which stay where they are.
        }
        packed++;
    }
    if (packed > 0)
        vkUpdateDescriptorSets(device->_logical_device_vk, packed, vk_writes, 0, NULL);

    // Each acceleration structure write, its handles from a fixed array in
    // pieces of as many as it holds: no allocation.
    for (uint32_t i = 0; i < write_count; i++) {
        const crgpu_write_descriptor_set *w = &writes[i];
        if (w->descriptor_type != CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE || !w->dst_set) continue;
        const crgpu_descriptor_set_vulkan *vkSet = (const crgpu_descriptor_set_vulkan *) w->dst_set;
        VkAccelerationStructureKHR handles[64];
        for (uint32_t first = 0; first < w->descriptor_count; first += 64) {
            const uint32_t n = (w->descriptor_count - first < 64) ? w->descriptor_count - first : 64;
            for (uint32_t k = 0; k < n; k++)
                handles[k] = w->acceleration_structures[first + k]->_structure_vk;
            VkWriteDescriptorSetAccelerationStructureKHR structures = {0};
            structures.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            structures.accelerationStructureCount = n;
            structures.pAccelerationStructures = handles;
            VkWriteDescriptorSet write = {0};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.pNext = &structures;
            write.dstSet = vkSet->_set_vk;
            write.dstBinding = w->dst_binding;
            write.dstArrayElement = w->dst_array_element + first;
            write.descriptorCount = n;
            write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(device->_logical_device_vk, 1, &write, 0, NULL);
        }
    }
}

void crgpu_command_list_bind_graphics_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
    if (!cmd || !pipeline || !sets || !set_count) return;
    if (CR_STALE(cmd, "crgpu_command_list_bind_graphics_descriptor_sets", "cmd")) return;
    if (CR_STALE(pipeline, "crgpu_command_list_bind_graphics_descriptor_sets", "pipeline")) return;
    if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS) return;
    if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count) return;

    // The command list's internal VkCommandBuffer lives at the front of the struct,
    // matching the pattern used in crgpuvulkanbuffer.c / the command list implementation.
    VkCommandBuffer vk_cmd = cmd->_command_buffer_vk;

    // Unwrap each Carrottus GPU handle to its raw VkDescriptorSet.
    VkDescriptorSet vk_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS] = {0};
    for (uint32_t i = 0; i < set_count; i++) {
        if (!sets[i]) return;
        crgpu_descriptor_set_vulkan *vkSet =
                (crgpu_descriptor_set_vulkan *) sets[i];
        vk_sets[i] = vkSet->_set_vk;
    }

    vkCmdBindDescriptorSets(
        vk_cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline->_pipeline_layout_vk,
        first_set,
        set_count,
        vk_sets,
        0, // No dynamic offsets
        NULL);
}

void crgpu_command_list_bind_compute_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
    if (!cmd || !pipeline || !sets || !set_count) return;
    if (CR_STALE(cmd, "crgpu_command_list_bind_compute_descriptor_sets", "cmd")) return;
    if (CR_STALE(pipeline, "crgpu_command_list_bind_compute_descriptor_sets", "pipeline")) return;
    if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS) return;
    if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count) return;

    VkDescriptorSet vk_sets[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS] = {0};
    for (uint32_t i = 0; i < set_count; i++) {
        if (!sets[i]) return;
        crgpu_descriptor_set_vulkan *vkSet =
                (crgpu_descriptor_set_vulkan *) sets[i];
        vk_sets[i] = vkSet->_set_vk;
    }

    vkCmdBindDescriptorSets(
        cmd->_command_buffer_vk,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline->_pipeline_layout_vk,
        first_set,
        set_count,
        vk_sets,
        0,
        NULL);
}


// ============================================================================
//  Bindless / Descriptor Indexing
// ============================================================================

#define CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING 0
#define CRGPU_BINDLESS_STORAGE_IMAGE_BINDING 1
#define CRGPU_BINDLESS_STORAGE_BUFFER_BINDING 2

// Called once, by crgpu_device_create when CRGPU_FEATURE_BINDLESS is
// asked for, right after the logical device is made — every other struct in
// this backend embeds crgpu_device_vulkan BY VALUE (a snapshot copy taken
// at creation time), so _bindless must be settled before anything can copy
// the device. The bindless calls never make it later.
CR_RESULT crgpuvulkan___create_bindless_state(crgpu_device device) {
    if (device->_bindless) return CR_SUCCESS;

    const CR_ALLOCATOR *allocator = &device->head.allocator;
    crgpu_bindless_state_vulkan *state = CR_NEW_ZEROED(allocator, crgpu_bindless_state_vulkan);
    if (!state) return CR_RESULT_OUT_OF_MEMORY;

    VkDescriptorSetLayoutBinding bindings[3] = {0};
    bindings[CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING].binding = CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING;
    bindings[CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING].descriptorCount = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
    bindings[CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[CRGPU_BINDLESS_STORAGE_IMAGE_BINDING].binding = CRGPU_BINDLESS_STORAGE_IMAGE_BINDING;
    bindings[CRGPU_BINDLESS_STORAGE_IMAGE_BINDING].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[CRGPU_BINDLESS_STORAGE_IMAGE_BINDING].descriptorCount = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
    bindings[CRGPU_BINDLESS_STORAGE_IMAGE_BINDING].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[CRGPU_BINDLESS_STORAGE_BUFFER_BINDING].binding = CRGPU_BINDLESS_STORAGE_BUFFER_BINDING;
    bindings[CRGPU_BINDLESS_STORAGE_BUFFER_BINDING].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[CRGPU_BINDLESS_STORAGE_BUFFER_BINDING].descriptorCount = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
    bindings[CRGPU_BINDLESS_STORAGE_BUFFER_BINDING].stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorBindingFlags bindingFlags[3];
    for (uint32_t i = 0; i < 3; i++)
        bindingFlags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                           VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo = {0};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = 3;
    bindingFlagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {0};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    crgpu_descriptor_set_layout_vulkan *layoutWrapper =
            CR_NEW_ZEROED(allocator, crgpu_descriptor_set_layout_vulkan);
    if (!layoutWrapper) {
        CR_DELETE(allocator, crgpu_bindless_state_vulkan, state);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }
    layoutWrapper->head.allocator = device->head.allocator;
    CR_BIRTH(layoutWrapper);
    layoutWrapper->_device = *device;

    if (vkCreateDescriptorSetLayout(
            device->_logical_device_vk, &layoutInfo, NULL,
            &layoutWrapper->_layout_vk) != VK_SUCCESS) {
        CR_DELETE(allocator, crgpu_descriptor_set_layout_vulkan, layoutWrapper);
        CR_DELETE(allocator, crgpu_bindless_state_vulkan, state);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    VkDescriptorPoolSize poolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS},
    };

    VkDescriptorPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device->_logical_device_vk, &poolInfo, NULL, &state->pool_vk) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device->_logical_device_vk, layoutWrapper->_layout_vk, NULL);
        CR_DELETE(allocator, crgpu_descriptor_set_layout_vulkan, layoutWrapper);
        CR_DELETE(allocator, crgpu_bindless_state_vulkan, state);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    VkDescriptorSetAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = state->pool_vk;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layoutWrapper->_layout_vk;

    if (vkAllocateDescriptorSets(device->_logical_device_vk, &allocInfo, &state->set_vk) != VK_SUCCESS) {
        vkDestroyDescriptorPool(device->_logical_device_vk, state->pool_vk, NULL);
        vkDestroyDescriptorSetLayout(device->_logical_device_vk, layoutWrapper->_layout_vk, NULL);
        CR_DELETE(allocator, crgpu_descriptor_set_layout_vulkan, layoutWrapper);
        CR_DELETE(allocator, crgpu_bindless_state_vulkan, state);
        return CR_RESULT_API_SPECIFIC_FAILURE;
    }

    if (!cr_mutex_init(&state->lock)) {
        // Destroying the pool frees the set allocated from it.
        vkDestroyDescriptorPool(device->_logical_device_vk, state->pool_vk, NULL);
        vkDestroyDescriptorSetLayout(device->_logical_device_vk, layoutWrapper->_layout_vk, NULL);
        CR_DELETE(allocator, crgpu_descriptor_set_layout_vulkan, layoutWrapper);
        CR_DELETE(allocator, crgpu_bindless_state_vulkan, state);
        return CR_RESULT_GENERAL_FAILURE;
    }

    state->layout = (crgpu_descriptor_set_layout) layoutWrapper;
    device->_bindless = state;
    return CR_SUCCESS;
}

// Counterpart of crgpuvulkan___create_bindless_state, called by
// crgpu_instance_destroy before it destroys the device.
void crgpuvulkan___destroy_bindless_state(crgpu_device device) {
    if (!device || !device->_bindless) return;
    crgpu_bindless_state_vulkan *state = device->_bindless;
    const CR_ALLOCATOR allocator = device->head.allocator;

    cr_mutex_destroy(&state->lock);
    // Destroying the pool frees the set allocated from it.
    vkDestroyDescriptorPool(device->_logical_device_vk, state->pool_vk, NULL);
    crgpu_descriptor_set_layout_vulkan *layoutWrapper = (crgpu_descriptor_set_layout_vulkan *) state->layout;
    if (layoutWrapper) {
        CR_KILL(layoutWrapper);
        vkDestroyDescriptorSetLayout(device->_logical_device_vk, layoutWrapper->_layout_vk, NULL);
        cr_string_free(&allocator, layoutWrapper->head.debug_name);
        CR_DELETE(&allocator, crgpu_descriptor_set_layout_vulkan, layoutWrapper);
    }
    CR_DELETE(&allocator, crgpu_bindless_state_vulkan, state);
    device->_bindless = NULL;
}

// Called with the state's lock held.
static CR_RESULT crgpuvulkan___bindless_alloc_index(
        uint32_t *next_unused,
        uint32_t *free_stack,
        uint32_t *free_count,
        uint32_t *out_index) {
    if (*free_count > 0) {
        *out_index = free_stack[--(*free_count)];
        return CR_SUCCESS;
    }
    if (*next_unused >= CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
        return CR_RESULT_GPU_BINDLESS_OUT_OF_SLOTS;
    *out_index = (*next_unused)++;
    return CR_SUCCESS;
}

crgpu_descriptor_set_layout crgpu_device_get_bindless_descriptor_set_layout(crgpu_device device) {
    if (!device) return NULL;
    if (CR_STALE(device, "crgpu_device_get_bindless_descriptor_set_layout", "device")) return NULL;
    if (!device->_bindless) return NULL;
    return device->_bindless->layout;
}

CR_RESULT crgpu_bindless_register_sampled_image(
        crgpu_device device,
        crgpu_image_view view,
        uint32_t *out_index) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_bindless_register_sampled_image", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!view) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (CR_STALE(view, "crgpu_bindless_register_sampled_image", "view")) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (!out_index) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    crgpu_bindless_state_vulkan *state = device->_bindless;
    if (!state) return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

    // Held across the write too: set_vk is externally synchronized.
    cr_mutex_lock(&state->lock);
    CR_RESULT sr = crgpuvulkan___bindless_alloc_index(
            &state->sampled_image_next_unused,
            state->sampled_image_free_stack,
            &state->sampled_image_free_count,
            out_index);
    if (sr != CR_SUCCESS) {
        cr_mutex_unlock(&state->lock);
        return sr;
    }

    crgpu_image_view_vulkan *vkView = (crgpu_image_view_vulkan *) view;
    VkDescriptorImageInfo imageInfo = {0};
    imageInfo.imageView = vkView->_image_view_vk;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = state->set_vk;
    write.dstBinding = CRGPU_BINDLESS_SAMPLED_IMAGE_BINDING;
    write.dstArrayElement = *out_index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device->_logical_device_vk, 1, &write, 0, NULL);
    cr_mutex_unlock(&state->lock);
    return CR_SUCCESS;
}

void crgpu_bindless_unregister_sampled_image(crgpu_device device, uint32_t index) {
    if (!device) return;
    if (CR_STALE(device, "crgpu_bindless_unregister_sampled_image", "device")) return;
    if (!device->_bindless) return;
    if (index == CRGPU_BINDLESS_INVALID_INDEX) return;
    crgpu_bindless_state_vulkan *state = device->_bindless;
    cr_mutex_lock(&state->lock);
    if (state->sampled_image_free_count < CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
        state->sampled_image_free_stack[state->sampled_image_free_count++] = index;
    cr_mutex_unlock(&state->lock);
}

CR_RESULT crgpu_bindless_register_storage_image(
        crgpu_device device,
        crgpu_image_view view,
        uint32_t *out_index) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_bindless_register_storage_image", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!view) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (CR_STALE(view, "crgpu_bindless_register_storage_image", "view")) return CR_RESULT_GPU_INVALID_IMAGE_VIEW;
    if (!out_index) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    crgpu_bindless_state_vulkan *state = device->_bindless;
    if (!state) return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

    // Held across the write too: set_vk is externally synchronized.
    cr_mutex_lock(&state->lock);
    CR_RESULT sr = crgpuvulkan___bindless_alloc_index(
            &state->storage_image_next_unused,
            state->storage_image_free_stack,
            &state->storage_image_free_count,
            out_index);
    if (sr != CR_SUCCESS) {
        cr_mutex_unlock(&state->lock);
        return sr;
    }

    crgpu_image_view_vulkan *vkView = (crgpu_image_view_vulkan *) view;
    VkDescriptorImageInfo imageInfo = {0};
    imageInfo.imageView = vkView->_image_view_vk;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = state->set_vk;
    write.dstBinding = CRGPU_BINDLESS_STORAGE_IMAGE_BINDING;
    write.dstArrayElement = *out_index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device->_logical_device_vk, 1, &write, 0, NULL);
    cr_mutex_unlock(&state->lock);
    return CR_SUCCESS;
}

void crgpu_bindless_unregister_storage_image(crgpu_device device, uint32_t index) {
    if (!device) return;
    if (CR_STALE(device, "crgpu_bindless_unregister_storage_image", "device")) return;
    if (!device->_bindless) return;
    if (index == CRGPU_BINDLESS_INVALID_INDEX) return;
    crgpu_bindless_state_vulkan *state = device->_bindless;
    cr_mutex_lock(&state->lock);
    if (state->storage_image_free_count < CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
        state->storage_image_free_stack[state->storage_image_free_count++] = index;
    cr_mutex_unlock(&state->lock);
}

CR_RESULT crgpu_bindless_register_storage_buffer(
        crgpu_device device,
        crgpu_buffer_view view,
        uint32_t *out_index) {
    if (!device) return CR_RESULT_GPU_INVALID_DEVICE;
    if (CR_STALE(device, "crgpu_bindless_register_storage_buffer", "device")) return CR_RESULT_GPU_INVALID_DEVICE;
    if (!view) return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
    if (CR_STALE(view, "crgpu_bindless_register_storage_buffer", "view")) return CR_RESULT_GPU_INVALID_BUFFER_VIEW;
    if (!out_index) return CR_RESULT_NULL_OUTPUT_PARAMETER;

    crgpu_bindless_state_vulkan *state = device->_bindless;
    if (!state) return CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED;

    // Held across the write too: set_vk is externally synchronized.
    cr_mutex_lock(&state->lock);
    CR_RESULT sr = crgpuvulkan___bindless_alloc_index(
            &state->storage_buffer_next_unused,
            state->storage_buffer_free_stack,
            &state->storage_buffer_free_count,
            out_index);
    if (sr != CR_SUCCESS) {
        cr_mutex_unlock(&state->lock);
        return sr;
    }

    crgpu_buffer_view_vulkan *vkView = (crgpu_buffer_view_vulkan *) view;

    // crgpu_buffer_view has no owning crgpu_buffer handle on its own —
    // it only wraps a VkBufferView, which typed/texel storage buffers need
    // but raw byte-range SSBO bindings (the common bindless case) don't.
    // Bind the raw buffer range this view describes via its VkBuffer parent.
    crgpu_buffer_vulkan *vkBuffer =
            (crgpu_buffer_vulkan *) vkView->_desc.parent_buffer;

    VkDescriptorBufferInfo bufferInfo = {0};
    bufferInfo.buffer = vkBuffer->_buffer_vk;
    bufferInfo.offset = vkView->_desc.offset_from_parent_buffer;
    bufferInfo.range = vkView->_desc.size ? vkView->_desc.size : VK_WHOLE_SIZE;

    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = state->set_vk;
    write.dstBinding = CRGPU_BINDLESS_STORAGE_BUFFER_BINDING;
    write.dstArrayElement = *out_index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device->_logical_device_vk, 1, &write, 0, NULL);
    cr_mutex_unlock(&state->lock);
    return CR_SUCCESS;
}

void crgpu_bindless_unregister_storage_buffer(crgpu_device device, uint32_t index) {
    if (!device) return;
    if (CR_STALE(device, "crgpu_bindless_unregister_storage_buffer", "device")) return;
    if (!device->_bindless) return;
    if (index == CRGPU_BINDLESS_INVALID_INDEX) return;
    crgpu_bindless_state_vulkan *state = device->_bindless;
    cr_mutex_lock(&state->lock);
    if (state->storage_buffer_free_count < CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS)
        state->storage_buffer_free_stack[state->storage_buffer_free_count++] = index;
    cr_mutex_unlock(&state->lock);
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
