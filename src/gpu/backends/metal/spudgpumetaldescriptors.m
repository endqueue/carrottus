//
// SpudGPU Metal backend - descriptor sets / argument buffers.
// Every descriptor set is a real Metal 2 argument buffer (see spudgpu.h:
// "On Metal it creates an MTLArgumentEncoder schema" / "MTLBuffer argument
// buffer") rather than discrete per-resource bindings - SPIRV-Cross pins
// each SPIR-V descriptor set's whole argument buffer to a fixed MSL buffer
// index in spudgpumetalshader.m (SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE),
// and this file builds/writes/binds the actual MTLArgumentEncoder/MTLBuffer
// pair per set. A SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER is two
// arguments: its texture at its binding, its sampler at
// SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE past it (spudgpumetal.h).
//

#if SPUDGPU_COMPILE_METAL_API

#include "spudgpu.h"
#include "spudcore.h"
#include "spudgpumetal.h"
#include <Metal/Metal.h>
#include "../../../spudstructbase.h"

static MTLDataType spudgpumetal___internal_descriptor_argument_data_type(SPUDGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		return MTLDataTypePointer;
	case SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return MTLDataTypeTexture;
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLER:
		return MTLDataTypeSampler;
	// A top level: an instance acceleration structure.
	case SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
		return MTLDataTypeInstanceAccelerationStructure;
	default:
		return MTLDataTypeNone;
	}
}

static MTLSamplerMinMagFilter spudgpumetal___internal_sampler_filter(SPUDGPU_FILTER filter) {
	return filter == SPUDGPU_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

static MTLSamplerMipFilter spudgpumetal___internal_sampler_mip_filter(SPUDGPU_FILTER filter) {
	return filter == SPUDGPU_FILTER_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
}

static MTLSamplerAddressMode spudgpumetal___internal_sampler_address_mode(SPUDGPU_ADDRESS_MODE mode) {
	switch (mode) {
	case SPUDGPU_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
	case SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE:   return MTLSamplerAddressModeClampToEdge;
	case SPUDGPU_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
	case SPUDGPU_ADDRESS_MODE_REPEAT:
	default:                                   return MTLSamplerAddressModeRepeat;
	}
}

SPUDRESULT spudgpu_create_sampler(
    spudgpu_device device,
    const spudgpu_sampler_desc *desc,
    spudgpu_sampler *out_sampler) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_sampler", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_sampler)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;

	MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
	sampler_desc.minFilter             = spudgpumetal___internal_sampler_filter(desc->min_filter);
	sampler_desc.magFilter             = spudgpumetal___internal_sampler_filter(desc->mag_filter);
	sampler_desc.mipFilter             = spudgpumetal___internal_sampler_mip_filter(desc->mipmap_filter);
	sampler_desc.sAddressMode          = spudgpumetal___internal_sampler_address_mode(desc->address_mode_u);
	sampler_desc.tAddressMode          = spudgpumetal___internal_sampler_address_mode(desc->address_mode_v);
	sampler_desc.rAddressMode          = spudgpumetal___internal_sampler_address_mode(desc->address_mode_w);
	sampler_desc.lodMinClamp           = desc->min_lod;
	sampler_desc.lodMaxClamp           = desc->max_lod;
	sampler_desc.maxAnisotropy         = desc->max_anisotropy > 1.0f ? (NSUInteger)desc->max_anisotropy : 1;
	// Every descriptor set is an argument buffer, so any sampler may be
	// encoded into one; Metal requires the sampler to say so at creation.
	sampler_desc.supportArgumentBuffers = YES;
	if (desc->compare_enable) {
		static const MTLCompareFunction functions[8] = {
		    MTLCompareFunctionNever,   MTLCompareFunctionLess,     MTLCompareFunctionEqual,        MTLCompareFunctionLessEqual,
		    MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways,
		};
		sampler_desc.compareFunction = desc->compare_op < 8 ? functions[desc->compare_op] : MTLCompareFunctionAlways;
	}

	id<MTLSamplerState> state = [device_metal->_device_mtl newSamplerStateWithDescriptor:sampler_desc];
	[sampler_desc release];
	if (!state)
		return SPUDRESULT_API_SPECIFIC_FAILURE;

	spudgpu_sampler_metal *sampler_metal = SPUD_NEW_ZEROED(&device_metal->head.allocator, spudgpu_sampler_metal);
	if (!sampler_metal) {
		[state release];
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	sampler_metal->head.allocator     = device_metal->head.allocator;
	SPUD_BIRTH(sampler_metal);
	sampler_metal->_sampler_state_mtl = [state retain];
	sampler_metal->_parent_device     = device_metal;
	SPUDGPU___LIVE_ADD(&device_metal->_live, SPUDGPU___OBJECT_SAMPLER);

	if (spud_debug_name_set(sampler_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_sampler((spudgpu_sampler)sampler_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	*out_sampler = (spudgpu_sampler)sampler_metal;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_sampler(spudgpu_sampler sampler) {
	spudgpu_sampler_metal *sampler_metal = (spudgpu_sampler_metal *)sampler;
	if (!sampler_metal)
		return;
	if (SPUD_STALE(sampler, "spudgpu_destroy_sampler", "sampler"))
		return;
	[sampler_metal->_sampler_state_mtl release];
	SPUDGPU___LIVE_REMOVE(&sampler_metal->_parent_device->_live, SPUDGPU___OBJECT_SAMPLER);
	SPUD_KILL(sampler);
	const SPUD_ALLOCATOR allocator = sampler_metal->head.allocator;
	spud_string_free(&allocator, sampler_metal->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_sampler_metal, sampler_metal);
}

static MTLBindingAccess spudgpumetal___internal_descriptor_argument_access(SPUDGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return MTLBindingAccessReadWrite;
	default:
		return MTLBindingAccessReadOnly;
	}
}

SPUDRESULT spudgpu_create_descriptor_set_layout(
    spudgpu_device device,
    const spudgpu_descriptor_set_layout_desc *desc,
    spudgpu_descriptor_set_layout *out_layout) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_set_layout", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_layout)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->binding_count > SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS;
	// An unknown type, or one the device has no feature for, the same way
	// on every backend, before anything is made.
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const SPUDRESULT type_sr = spudgpu___internal_check_descriptor_type(desc->bindings[i].descriptor_type, ((spudgpu_device_metal *)device)->_features);
		if (type_sr != SPUD_SUCCESS)
			return type_sr;
	}

	SPUDRESULT sr = SPUD_SUCCESS;

	spudgpu_descriptor_set_layout_metal *layout_metal =
	    SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_descriptor_set_layout_metal);
	if (!layout_metal) {
		sr = SPUDRESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	layout_metal->head.allocator = device->head.allocator;
	SPUD_BIRTH(layout_metal);
	layout_metal->_parent_device = (spudgpu_device_metal *)device;
	layout_metal->_desc          = *desc;

	{
		NSMutableArray<MTLArgumentDescriptor *> *argument_descs =
		    [NSMutableArray arrayWithCapacity:desc->binding_count];
		// A combined binding's samplers go past every binding's elements.
		bool has_combined = false;
		for (uint32_t i = 0; i < desc->binding_count; i++)
			if (desc->bindings[i].descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				has_combined = true;
		for (uint32_t i = 0; i < desc->binding_count && has_combined; i++) {
			const spudgpu_descriptor_binding_desc *b = &desc->bindings[i];
			const uint32_t count                     = b->count > 1 ? b->count : 1;
			if (b->binding >= SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE || count > SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE - b->binding) {
				sr = SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
				goto failedattempt;
			}
		}
		for (uint32_t i = 0; i < desc->binding_count; i++) {
			const spudgpu_descriptor_binding_desc *b = &desc->bindings[i];

			MTLDataType data_type = spudgpumetal___internal_descriptor_argument_data_type(b->descriptor_type);
			if (data_type == MTLDataTypeNone) {
				sr = SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
				goto failedattempt;
			}

			MTLArgumentDescriptor *argument_desc = [MTLArgumentDescriptor argumentDescriptor];
			argument_desc.index                  = b->binding;
			// 0 means "not an array" to MTLArgumentDescriptor; b->count == 1
			// is the ordinary single-resource case.
			argument_desc.arrayLength = (b->count > 1) ? b->count : 0;
			argument_desc.dataType    = data_type;
			argument_desc.access      = spudgpumetal___internal_descriptor_argument_access(b->descriptor_type);
			if (data_type == MTLDataTypeTexture) {
				// Matches the overwhelmingly common case (2D sampled/storage
				// images) - see spudgpumetalimage.m's own texture-type
				// inference for the same assumption elsewhere in this
				// backend. A caller needing a different texture type here
				// (cube/array/3D) isn't supported yet.
				argument_desc.textureType = MTLTextureType2D;
			}
			[argument_descs addObject:argument_desc];
		}
		// The combined bindings' samplers, after every other argument: their
		// ids are past every binding's, and the arguments go in id order.
		for (uint32_t i = 0; i < desc->binding_count; i++) {
			const spudgpu_descriptor_binding_desc *b = &desc->bindings[i];
			if (b->descriptor_type != SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				continue;
			MTLArgumentDescriptor *sampler_desc = [MTLArgumentDescriptor argumentDescriptor];
			sampler_desc.index                  = SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE + b->binding;
			sampler_desc.arrayLength            = (b->count > 1) ? b->count : 0;
			sampler_desc.dataType               = MTLDataTypeSampler;
			sampler_desc.access                 = MTLBindingAccessReadOnly;
			[argument_descs addObject:sampler_desc];
		}

		layout_metal->_argument_encoder_mtl =
		    [layout_metal->_parent_device->_device_mtl newArgumentEncoderWithArguments:argument_descs];
		if (!layout_metal->_argument_encoder_mtl) {
			sr = SPUDRESULT_API_SPECIFIC_FAILURE;
			goto failedattempt;
		}
	}

	SPUDGPU___LIVE_ADD(&layout_metal->_parent_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	if (spud_debug_name_set(layout_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_descriptor_set_layout((spudgpu_descriptor_set_layout)layout_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	layout_metal->_desc.debug_name = layout_metal->head.debug_name;

	*out_layout = (spudgpu_descriptor_set_layout)layout_metal;

	return sr;

failedattempt:
	if (layout_metal) {
		if (layout_metal->_argument_encoder_mtl) {
			[layout_metal->_argument_encoder_mtl release];
		}
		SPUD_DELETE(&device->head.allocator, spudgpu_descriptor_set_layout_metal, layout_metal);
	}
	return sr;
}

void spudgpu_destroy_descriptor_set_layout(spudgpu_descriptor_set_layout layout) {
	spudgpu_descriptor_set_layout_metal *layout_metal = (spudgpu_descriptor_set_layout_metal *)layout;
	if (layout_metal) {
		if (SPUD_STALE(layout, "spudgpu_destroy_descriptor_set_layout", "layout"))
			return;
		if (layout_metal->_argument_encoder_mtl) {
			[layout_metal->_argument_encoder_mtl release];
		}
		SPUDGPU___LIVE_REMOVE(&layout_metal->_parent_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
		SPUD_KILL(layout);
		const SPUD_ALLOCATOR allocator = layout_metal->head.allocator;
		spud_string_free(&allocator, layout_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_descriptor_set_layout_metal, layout_metal);
	}
}

SPUDRESULT spudgpu_create_descriptor_pool(
    spudgpu_device device,
    const spudgpu_descriptor_pool_desc *desc,
    spudgpu_descriptor_pool *out_pool) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_pool", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_pool)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (desc->pool_size_count > SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS;
	for (uint32_t i = 0; i < desc->pool_size_count; ++i) {
		const SPUDRESULT type_sr = spudgpu___internal_check_descriptor_type(desc->pool_sizes[i].descriptor_type, ((spudgpu_device_metal *)device)->_features);
		if (type_sr != SPUD_SUCCESS)
			return type_sr;
	}
	// What the pool takes out of the device's budget, the same count on
	// every backend; given back when the pool is destroyed.
	uint64_t budget_descriptors = 0;
	uint64_t budget_samplers    = 0;
	spudgpu___internal_pool_descriptor_counts(desc, &budget_descriptors, &budget_samplers);
	SPUDRESULT sr = spudgpu___internal_budget_take(&device->_budget, budget_descriptors, budget_samplers);
	if (sr != SPUD_SUCCESS)
		return sr;

	spudgpu_descriptor_pool_metal *pool_metal = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_descriptor_pool_metal);
	if (!pool_metal) {
		sr = SPUDRESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}
	pool_metal->head.allocator      = device->head.allocator;
	SPUD_BIRTH(pool_metal);
	pool_metal->_parent_device      = (spudgpu_device_metal *)device;
	pool_metal->_desc               = *desc;
	pool_metal->_budget_descriptors = budget_descriptors;
	pool_metal->_budget_samplers    = budget_samplers;

	// Metal needs no real backing arena the way Vulkan's VkDescriptorPool
	// does (see spudgpu_descriptor_pool_metal's struct comment) - this array
	// only exists so spudgpu_reset_descriptor_pool/spudgpu_destroy_descriptor_pool
	// can free every set's MTLBuffer in bulk, matching those calls' "free
	// all sets allocated from this pool" contract instead of leaking one
	// small GPU buffer per set on every per-frame reset.
	// _desc.max_sets long, which is the count it is freed with.
	pool_metal->_tracked_sets_mtl =
	    SPUD_NEW_ARRAY_ZEROED(&pool_metal->head.allocator, spudgpu_descriptor_set_metal *, desc->max_sets);
	if (!pool_metal->_tracked_sets_mtl) {
		sr = SPUDRESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}

	// From here spudgpu_destroy_descriptor_pool frees it, gives its budget
	// back and takes the count back.
	SPUDGPU___LIVE_ADD(&pool_metal->_parent_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_POOL);
	if (spud_debug_name_set(pool_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_descriptor_pool((spudgpu_descriptor_pool)pool_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pool_metal->_desc.debug_name = pool_metal->head.debug_name;

	*out_pool = (spudgpu_descriptor_pool)pool_metal;

	return sr;

failedattempt:
	spudgpu___internal_budget_give_back(&device->_budget, budget_descriptors, budget_samplers);
	if (pool_metal) {
		SPUD_DELETE_ARRAY(
		    &device->head.allocator, spudgpu_descriptor_set_metal *, pool_metal->_tracked_sets_mtl,
		    pool_metal->_desc.max_sets);
		SPUD_DELETE(&device->head.allocator, spudgpu_descriptor_pool_metal, pool_metal);
	}
	return sr;
}

static void spudgpumetal___internal_free_tracked_sets(spudgpu_descriptor_pool_metal *pool_metal) {
	for (uint32_t i = 0; i < pool_metal->_allocated_set_count; i++) {
		spudgpu_descriptor_set_metal *set_metal = pool_metal->_tracked_sets_mtl[i];
		if (!set_metal)
			continue;
		SPUD_KILL(set_metal);
		if (set_metal->_argument_buffer_mtl) {
			[set_metal->_argument_buffer_mtl release];
		}
		const SPUD_ALLOCATOR allocator = set_metal->head.allocator;
		spud_string_free(&allocator, set_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_descriptor_set_metal, set_metal);
	}
	pool_metal->_allocated_set_count = 0;
}

void spudgpu_reset_descriptor_pool(spudgpu_descriptor_pool pool) {
	spudgpu_descriptor_pool_metal *pool_metal = (spudgpu_descriptor_pool_metal *)pool;
	if (!pool_metal)
		return;
	if (SPUD_STALE(pool, "spudgpu_reset_descriptor_pool", "pool"))
		return;
	spudgpumetal___internal_free_tracked_sets(pool_metal);
}

void spudgpu_destroy_descriptor_pool(spudgpu_descriptor_pool pool) {
	spudgpu_descriptor_pool_metal *pool_metal = (spudgpu_descriptor_pool_metal *)pool;
	if (!pool_metal)
		return;
	if (SPUD_STALE(pool, "spudgpu_destroy_descriptor_pool", "pool"))
		return;
	spudgpumetal___internal_free_tracked_sets(pool_metal);
	spudgpu___internal_budget_give_back(&pool_metal->_parent_device->_budget, pool_metal->_budget_descriptors, pool_metal->_budget_samplers);
	SPUDGPU___LIVE_REMOVE(&pool_metal->_parent_device->_live, SPUDGPU___OBJECT_DESCRIPTOR_POOL);
	SPUD_KILL(pool);
	const SPUD_ALLOCATOR allocator = pool_metal->head.allocator;
	SPUD_DELETE_ARRAY(&allocator, spudgpu_descriptor_set_metal *, pool_metal->_tracked_sets_mtl, pool_metal->_desc.max_sets);
	spud_string_free(&allocator, pool_metal->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_descriptor_pool_metal, pool_metal);
}

SPUDRESULT spudgpu_create_descriptor_sets(
    spudgpu_device device,
    const spudgpu_descriptor_set_desc *desc,
    spudgpu_descriptor_set *out_sets) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_descriptor_sets", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_sets)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->pool)
		return SPUDRESULT_GPU_INVALID_DESCRIPTOR_POOL;
	if (desc->set_count == 0)
		return SPUDRESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS;
	if (desc->set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return SPUDRESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS;

	spudgpu_descriptor_pool_metal *pool_metal = (spudgpu_descriptor_pool_metal *)desc->pool;
	if (pool_metal->_allocated_set_count + desc->set_count > pool_metal->_desc.max_sets)
		return SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;

	for (uint32_t i = 0; i < desc->set_count; i++) {
		spudgpu_descriptor_set_layout_metal *layout_metal =
		    (spudgpu_descriptor_set_layout_metal *)desc->set_layouts[i];
		if (!layout_metal || !layout_metal->_argument_encoder_mtl)
			return SPUDRESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT;

		spudgpu_descriptor_set_metal *set_metal =
		    SPUD_NEW_ZEROED(&pool_metal->head.allocator, spudgpu_descriptor_set_metal);
		if (!set_metal)
			return SPUDRESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;

		set_metal->head.allocator = pool_metal->head.allocator;
		SPUD_BIRTH(set_metal);
		set_metal->_parent_pool   = pool_metal;
		set_metal->_layout      = layout_metal;
		set_metal->_argument_buffer_mtl =
		    [pool_metal->_parent_device->_device_mtl newBufferWithLength:layout_metal->_argument_encoder_mtl.encodedLength
		                                                          options:MTLResourceStorageModeShared];
		if (!set_metal->_argument_buffer_mtl) {
			SPUD_DELETE(&pool_metal->head.allocator, spudgpu_descriptor_set_metal, set_metal);
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}

		pool_metal->_tracked_sets_mtl[pool_metal->_allocated_set_count] = set_metal;
		pool_metal->_allocated_set_count++;

		out_sets[i] = (spudgpu_descriptor_set)set_metal;
	}

	return SPUD_SUCCESS;
}

// Tracks [resource] (and [structure], for an acceleration structure) as
// what [set]'s [argument_index] references, replacing what that index
// tracked before, if anything: the argument buffer no longer references the
// old resource, and it may since have been destroyed.
static void spudgpumetal___track_argument(
    spudgpu_descriptor_set_metal *set,
    uint32_t argument_index,
    id<MTLResource> resource,
    MTLResourceUsage usage,
    spudgpu_acceleration_structure_metal *structure) {
	uint32_t slot = 0;
	while (slot < set->_tracked_resource_count && set->_tracked_argument_indices[slot] != argument_index)
		slot++;
	if (slot < SPUDGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET) {
		set->_tracked_argument_indices[slot]    = argument_index;
		set->_tracked_resources_mtl[slot]       = resource;
		set->_tracked_resource_usages_mtl[slot] = usage;
		set->_tracked_structures[slot]          = structure;
		if (slot == set->_tracked_resource_count)
			set->_tracked_resource_count++;
	}
}

void spudgpu_update_descriptor_sets(
    spudgpu_device device,
    const spudgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	if (!device || !writes || write_count == 0)
		return;
	if (SPUD_STALE(device, "spudgpu_update_descriptor_sets", "device"))
		return;
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i)
		if (spud_struct_check(&writes[writes_i].base, SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET) != SPUD_SUCCESS)
			return;
	// A storage view of a cube is a 2D array view of its faces on every
	// backend (spudgpu.h): D3D12 has no storage cube. Checked before the
	// first write, so a refused call writes nothing.
	for (uint32_t writes_i = 0; writes_i < write_count; ++writes_i) {
		const spudgpu_write_descriptor_set *write = &writes[writes_i];
		if (write->descriptor_type != SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || !write->image_info)
			continue;
		for (uint32_t k = 0; k < write->descriptor_count; ++k) {
			const spudgpu_image_view view = write->image_info[k].image_view;
			if (view && (view->_desc.type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE || view->_desc.type == SPUDGPU_IMAGE_VIEW_TYPE_CUBE_ARRAY))
				return;
		}
	}
	// Acceleration structure writes: the feature, and every element a live
	// top level (spudgpu.h), checked the same way on every backend.
	if (!spudgpu___internal_check_acceleration_structure_writes(((spudgpu_device_metal *)device)->_features, writes, write_count))
		return;

	for (uint32_t i = 0; i < write_count; i++) {
		const spudgpu_write_descriptor_set *w = &writes[i];
		if (!w->dst_set)
			continue;

		spudgpu_descriptor_set_metal *set_metal = (spudgpu_descriptor_set_metal *)w->dst_set;
		spudgpu_descriptor_set_layout_metal *layout_metal = set_metal->_layout;
		if (!layout_metal || !layout_metal->_argument_encoder_mtl)
			continue;

		id<MTLArgumentEncoder> encoder = layout_metal->_argument_encoder_mtl;
		[encoder setArgumentBuffer:set_metal->_argument_buffer_mtl offset:0];
		uint32_t argument_index = w->dst_binding + w->dst_array_element;

		id<MTLResource> tracked_resource   = nil;
		MTLResourceUsage tracked_usage     = MTLResourceUsageRead;
		spudgpu_acceleration_structure_metal *tracked_structure = NULL;

		switch (w->descriptor_type) {
		case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
			if (!w->buffer_info || !w->buffer_info->buffer)
				continue;
			spudgpu_buffer_metal *buffer_metal = (spudgpu_buffer_metal *)w->buffer_info->buffer;
			[encoder setBuffer:buffer_metal->_buffer_mtl offset:w->buffer_info->offset atIndex:argument_index];
			tracked_resource = buffer_metal->_buffer_mtl;
			tracked_usage    = (w->descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			                       ? (MTLResourceUsageRead | MTLResourceUsageWrite)
			                       : MTLResourceUsageRead;
			break;
		}
		case SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
			if (!w->image_info || !w->image_info->image_view)
				continue;
			spudgpu_image_view_metal *view_metal = (spudgpu_image_view_metal *)w->image_info->image_view;
			[encoder setTexture:view_metal->_texture_view_mtl atIndex:argument_index];
			tracked_resource = view_metal->_texture_view_mtl;
			tracked_usage    = (w->descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE)
			                       ? (MTLResourceUsageRead | MTLResourceUsageWrite)
			                       : MTLResourceUsageRead;
			// A combined descriptor's sampler is its second argument.
			if (w->descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w->sampler) {
				spudgpu_sampler_metal *sampler_metal = (spudgpu_sampler_metal *)w->sampler;
				[encoder setSamplerState:sampler_metal->_sampler_state_mtl
				                 atIndex:SPUDGPU_METAL_COMBINED_SAMPLER_INDEX_BASE + argument_index];
			}
			break;
		}
		case SPUDGPU_DESCRIPTOR_TYPE_SAMPLER: {
			if (!w->sampler)
				continue;
			spudgpu_sampler_metal *sampler_metal = (spudgpu_sampler_metal *)w->sampler;
			[encoder setSamplerState:sampler_metal->_sampler_state_mtl atIndex:argument_index];
			// id<MTLSamplerState> isn't an id<MTLResource> - no residency
			// tracking (useResource:) applies to samplers on Metal.
			break;
		}
		case SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
			// Checked above: every element a live top level. Every element
			// is written, element k at the argument index after k others
			// (the binding's array), and each tracked.
			for (uint32_t k = 0; k < w->descriptor_count; ++k) {
				spudgpu_acceleration_structure_metal *structure = w->acceleration_structures[k];
				[encoder setAccelerationStructure:structure->_structure_mtl atIndex:argument_index + k];
				spudgpumetal___track_argument(set_metal, argument_index + k, structure->_structure_mtl, MTLResourceUsageRead, structure);
			}
			continue;
		}
		default:
			continue;
		}

		if (tracked_resource)
			spudgpumetal___track_argument(set_metal, argument_index, tracked_resource, tracked_usage, tracked_structure);
	}
}

void spudgpu_cmd_bind_descriptor_sets(
    spudgpu_command_list cmd,
    spudgpu_shader_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || set_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_descriptor_sets", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_descriptor_sets", "pipeline"))
		return;
	if (set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_metal->_active_render_encoder)
		return;

	// A mesh pipeline has no vertex stage at all - bind to the mesh stage
	// instead (-setMeshBuffer:) via spudgpu_shader_pipeline_metal::
	// _is_mesh_pipeline, set at pipeline-creation time (spudgpumetalshader.m).
	// Fragment stays the same either way.
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)pipeline;
	bool is_mesh_pipeline                         = pipeline_metal->_is_mesh_pipeline;
	MTLRenderStages non_fragment_stage             = is_mesh_pipeline ? MTLRenderStageMesh : MTLRenderStageVertex;

	for (uint32_t i = 0; i < set_count; i++) {
		spudgpu_descriptor_set_metal *set_metal = (spudgpu_descriptor_set_metal *)sets[i];
		if (!set_metal)
			continue;

		// Bound only to the stages the layout's bindings name, as Vulkan
		// and D3D12 make a binding visible only to its stage_flags. Binding
		// it to a stage whose function has no argument at the index is a
		// Metal validation error.
		const spudgpu_descriptor_set_layout_desc *layout_desc = &set_metal->_layout->_desc;
		uint32_t set_stages                                     = 0;
		for (uint32_t b = 0; b < layout_desc->binding_count; b++)
			set_stages |= (uint32_t)layout_desc->bindings[b].stage_flags;
		uint32_t non_fragment_flags = is_mesh_pipeline ? (SPUDGPU_SHADER_STAGE_MESH | SPUDGPU_SHADER_STAGE_TASK) : SPUDGPU_SHADER_STAGE_VERTEX;
		// And only to a stage whose function in this pipeline reads the set
		// (spudgpu_shader_pipeline_metal::_non_fragment_set_mask).
		uint32_t set_bit            = 1u << (first_set + i);
		bool to_non_fragment        = (set_stages & non_fragment_flags) != 0 && (pipeline_metal->_non_fragment_set_mask & set_bit) != 0;
		bool to_fragment            = (set_stages & SPUDGPU_SHADER_STAGE_FRAGMENT) != 0 && (pipeline_metal->_fragment_set_mask & set_bit) != 0;
		if (!to_non_fragment && !to_fragment)
			continue;

		// Set only where the stage doesn't have it already: Metal's
		// validation refuses setting the same buffer again, and a caller
		// binding one set for several pipelines in a pass is ordinary.
		const uint32_t set_index  = first_set + i;
		uint32_t buffer_index     = SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + set_index;
		id<MTLBuffer> argument_mtl = set_metal->_argument_buffer_mtl;
		if (to_non_fragment) {
			if (is_mesh_pipeline) {
				if (cmd_metal->_encoder_mesh_sets[set_index] != argument_mtl) {
					[cmd_metal->_active_render_encoder setMeshBuffer:argument_mtl offset:0 atIndex:buffer_index];
					cmd_metal->_encoder_mesh_sets[set_index] = argument_mtl;
				}
			} else if (cmd_metal->_encoder_vertex_sets[set_index] != argument_mtl) {
				[cmd_metal->_active_render_encoder setVertexBuffer:argument_mtl offset:0 atIndex:buffer_index];
				cmd_metal->_encoder_vertex_sets[set_index] = argument_mtl;
			}
		}
		if (to_fragment && cmd_metal->_encoder_fragment_sets[set_index] != argument_mtl) {
			[cmd_metal->_active_render_encoder setFragmentBuffer:argument_mtl offset:0 atIndex:buffer_index];
			cmd_metal->_encoder_fragment_sets[set_index] = argument_mtl;
		}
		MTLRenderStages resident_stages = (to_non_fragment ? non_fragment_stage : 0) | (to_fragment ? MTLRenderStageFragment : 0);

		// Every resource reached only indirectly through this argument
		// buffer must be marked resident on this encoder - see
		// SPUDGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET in spudgpumetal.h.
		for (uint32_t r = 0; r < set_metal->_tracked_resource_count; r++) {
			[cmd_metal->_active_render_encoder useResource:set_metal->_tracked_resources_mtl[r]
			                                          usage:set_metal->_tracked_resource_usages_mtl[r]
			                                         stages:resident_stages];
			if (set_metal->_tracked_structures[r])
				spudgpumetal___use_bottom_levels(cmd_metal->_active_render_encoder, true, resident_stages, set_metal->_tracked_structures[r]);
		}
	}
}

void spudgpu_cmd_bind_descriptor_sets_compute(
    spudgpu_command_list cmd,
    spudgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const spudgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || set_count == 0)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_bind_descriptor_sets_compute", "cmd"))
		return;
	if (SPUD_STALE(pipeline, "spudgpu_cmd_bind_descriptor_sets_compute", "pipeline"))
		return;
	if (set_count > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	spudgpu_command_list_metal *cmd_metal = (spudgpu_command_list_metal *)cmd;
	if (!cmd_metal->_active_compute_encoder)
		return;

	for (uint32_t i = 0; i < set_count; i++) {
		spudgpu_descriptor_set_metal *set_metal = (spudgpu_descriptor_set_metal *)sets[i];
		if (!set_metal)
			continue;

		// The set already bound at this index on this encoder: its buffer
		// is set and its resources resident, and Metal's validation refuses
		// setting the same buffer again.
		if (cmd_metal->_encoder_compute_sets[first_set + i] == set_metal->_argument_buffer_mtl)
			continue;
		uint32_t buffer_index = SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + first_set + i;
		[cmd_metal->_active_compute_encoder setBuffer:set_metal->_argument_buffer_mtl offset:0 atIndex:buffer_index];
		cmd_metal->_encoder_compute_sets[first_set + i] = set_metal->_argument_buffer_mtl;

		// Every resource reached only indirectly through this argument
		// buffer must be marked resident on this encoder - see
		// SPUDGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET in spudgpumetal.h.
		// MTLComputeCommandEncoder's -useResource:usage: has no stages:
		// parameter (unlike the render encoder's) - there's only one stage.
		for (uint32_t r = 0; r < set_metal->_tracked_resource_count; r++) {
			[cmd_metal->_active_compute_encoder useResource:set_metal->_tracked_resources_mtl[r]
			                                           usage:set_metal->_tracked_resource_usages_mtl[r]];
			if (set_metal->_tracked_structures[r])
				spudgpumetal___use_bottom_levels(cmd_metal->_active_compute_encoder, false, 0, set_metal->_tracked_structures[r]);
		}
	}
}

#endif // SPUDGPU_COMPILE_METAL_API
