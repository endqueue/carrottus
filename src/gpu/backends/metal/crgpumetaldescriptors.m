//
// Carrottus GPU Metal backend - descriptor sets / argument buffers.
// Every descriptor set is a real Metal 2 argument buffer (see crgpu.h:
// "On Metal it creates an MTLArgumentEncoder schema" / "MTLBuffer argument
// buffer") rather than discrete per-resource bindings - SPIRV-Cross pins
// each SPIR-V descriptor set's whole argument buffer to a fixed MSL buffer
// index in crgpumetalshader.m (CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE),
// and this file builds/writes/binds the actual MTLArgumentEncoder/MTLBuffer
// pair per set. A CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER is two
// arguments: its texture at its binding, its sampler at
// CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE past it (crgpumetal.h).
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include <Metal/Metal.h>
#include "../../../crstructbase.h"

static MTLDataType crgpumetal___internal_descriptor_argument_data_type(CRGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		return MTLDataTypePointer;
	case CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return MTLDataTypeTexture;
	case CRGPU_DESCRIPTOR_TYPE_SAMPLER:
		return MTLDataTypeSampler;
	// A top level: an instance acceleration structure.
	case CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
		return MTLDataTypeInstanceAccelerationStructure;
	default:
		return MTLDataTypeNone;
	}
}

static MTLSamplerMinMagFilter crgpumetal___internal_sampler_filter(CRGPU_FILTER filter) {
	return filter == CRGPU_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
}

static MTLSamplerMipFilter crgpumetal___internal_sampler_mip_filter(CRGPU_FILTER filter) {
	return filter == CRGPU_FILTER_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
}

static MTLSamplerAddressMode crgpumetal___internal_sampler_address_mode(CRGPU_ADDRESS_MODE mode) {
	switch (mode) {
	case CRGPU_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
	case CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE:   return MTLSamplerAddressModeClampToEdge;
	case CRGPU_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
	case CRGPU_ADDRESS_MODE_REPEAT:
	default:                                   return MTLSamplerAddressModeRepeat;
	}
}

CR_RESULT crgpu_sampler_create(
    crgpu_device device,
    const crgpu_sampler_desc *desc,
    crgpu_sampler *out_sampler) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_sampler_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SAMPLER_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_sampler)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;

	MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
	sampler_desc.minFilter             = crgpumetal___internal_sampler_filter(desc->min_filter);
	sampler_desc.magFilter             = crgpumetal___internal_sampler_filter(desc->mag_filter);
	sampler_desc.mipFilter             = crgpumetal___internal_sampler_mip_filter(desc->mipmap_filter);
	sampler_desc.sAddressMode          = crgpumetal___internal_sampler_address_mode(desc->address_mode_u);
	sampler_desc.tAddressMode          = crgpumetal___internal_sampler_address_mode(desc->address_mode_v);
	sampler_desc.rAddressMode          = crgpumetal___internal_sampler_address_mode(desc->address_mode_w);
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
		return CR_RESULT_API_SPECIFIC_FAILURE;

	crgpu_sampler_metal *sampler_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_sampler_metal);
	if (!sampler_metal) {
		[state release];
		return CR_RESULT_OUT_OF_MEMORY;
	}
	sampler_metal->head.allocator     = device_metal->head.allocator;
	CR_BIRTH_IN(sampler_metal, &device_metal->_quarantine);
	sampler_metal->_sampler_state_mtl = [state retain];
	sampler_metal->_parent_device     = device_metal;
	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_SAMPLER);

	if (cr_debug_name_set(sampler_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_sampler_destroy((crgpu_sampler)sampler_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	*out_sampler = (crgpu_sampler)sampler_metal;
	return CR_SUCCESS;
}

void crgpu_sampler_destroy(crgpu_sampler sampler) {
	crgpu_sampler_metal *sampler_metal = (crgpu_sampler_metal *)sampler;
	if (!sampler_metal)
		return;
	if (CR_STALE(sampler, "crgpu_sampler_destroy", "sampler"))
		return;
	[sampler_metal->_sampler_state_mtl release];
	CRGPU___LIVE_REMOVE(&sampler_metal->_parent_device->_live, CRGPU___OBJECT_SAMPLER);
	CR_KILL(sampler);
	const CR_ALLOCATOR allocator = sampler_metal->head.allocator;
	cr_string_free(&allocator, sampler_metal->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(sampler_metal), &allocator, crgpu_sampler_metal, sampler_metal);
}

static MTLBindingAccess crgpumetal___internal_descriptor_argument_access(CRGPU_DESCRIPTOR_TYPE type) {
	switch (type) {
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return MTLBindingAccessReadWrite;
	default:
		return MTLBindingAccessReadOnly;
	}
}

uint32_t crgpumetal___set_ranges_normalize(crgpumetal_set_range *ranges, uint32_t count) {
	for (uint32_t i = 1; i < count; ++i)
		for (uint32_t j = i; j > 0 && ranges[j - 1].first_id > ranges[j].first_id; --j) {
			const crgpumetal_set_range swap = ranges[j];
			ranges[j]                         = ranges[j - 1];
			ranges[j - 1]                     = swap;
		}
	uint32_t joined = 0;
	for (uint32_t i = 0; i < count; ++i) {
		crgpumetal_set_range *last = joined ? &ranges[joined - 1] : NULL;
		if (last && last->data_type == ranges[i].data_type && (uint64_t)last->first_id + last->count == ranges[i].first_id)
			last->count += ranges[i].count;
		else
			ranges[joined++] = ranges[i];
	}
	return joined;
}

CR_RESULT crgpu_descriptor_set_layout_create(
    crgpu_device device,
    const crgpu_descriptor_set_layout_desc *desc,
    crgpu_descriptor_set_layout *out_layout) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_set_layout_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_layout)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->binding_count > CRGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_BINDINGS;
	// An unknown type, or one the device has no feature for, the same way
	// on every backend, before anything is made.
	for (uint32_t i = 0; i < desc->binding_count; ++i) {
		const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(desc->bindings[i].descriptor_type, ((crgpu_device_metal *)device)->_features);
		if (type_sr != CR_SUCCESS)
			return type_sr;
	}

	CR_RESULT sr = CR_SUCCESS;

	crgpu_descriptor_set_layout_metal *layout_metal =
	    CR_NEW_ZEROED(&device->head.allocator, crgpu_descriptor_set_layout_metal);
	if (!layout_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	layout_metal->head.allocator = device->head.allocator;
	CR_BIRTH_IN(layout_metal, &device->_quarantine);
	layout_metal->_parent_device = (crgpu_device_metal *)device;
	layout_metal->_desc          = *desc;

	{
		NSMutableArray<MTLArgumentDescriptor *> *argument_descs =
		    [NSMutableArray arrayWithCapacity:desc->binding_count];
		// A combined binding's samplers go past every binding's elements.
		bool has_combined = false;
		for (uint32_t i = 0; i < desc->binding_count; i++)
			if (desc->bindings[i].descriptor_type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				has_combined = true;
		for (uint32_t i = 0; i < desc->binding_count && has_combined; i++) {
			const crgpu_descriptor_binding_desc *b = &desc->bindings[i];
			const uint32_t count                     = b->count > 1 ? b->count : 1;
			if (b->binding >= CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE || count > CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE - b->binding) {
				sr = CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
				goto failedattempt;
			}
		}
		for (uint32_t i = 0; i < desc->binding_count; i++) {
			const crgpu_descriptor_binding_desc *b = &desc->bindings[i];

			MTLDataType data_type = crgpumetal___internal_descriptor_argument_data_type(b->descriptor_type);
			if (data_type == MTLDataTypeNone) {
				sr = CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
				goto failedattempt;
			}

			crgpumetal_set_range *range = &layout_metal->_ranges[layout_metal->_range_count++];
			range->first_id               = b->binding;
			range->count                  = b->count > 1 ? b->count : 1;
			range->data_type              = (uint32_t)data_type;

			MTLArgumentDescriptor *argument_desc = [MTLArgumentDescriptor argumentDescriptor];
			argument_desc.index                  = b->binding;
			// 0 means "not an array" to MTLArgumentDescriptor; b->count == 1
			// is the ordinary single-resource case.
			argument_desc.arrayLength = (b->count > 1) ? b->count : 0;
			argument_desc.dataType    = data_type;
			argument_desc.access      = crgpumetal___internal_descriptor_argument_access(b->descriptor_type);
			if (data_type == MTLDataTypeTexture) {
				// Matches the overwhelmingly common case (2D sampled/storage
				// images) - see crgpumetalimage.m's own texture-type
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
			const crgpu_descriptor_binding_desc *b = &desc->bindings[i];
			if (b->descriptor_type != CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				continue;
			crgpumetal_set_range *range = &layout_metal->_ranges[layout_metal->_range_count++];
			range->first_id               = CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE + b->binding;
			range->count                  = b->count > 1 ? b->count : 1;
			range->data_type              = (uint32_t)MTLDataTypeSampler;

			MTLArgumentDescriptor *sampler_desc = [MTLArgumentDescriptor argumentDescriptor];
			sampler_desc.index                  = CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE + b->binding;
			sampler_desc.arrayLength            = (b->count > 1) ? b->count : 0;
			sampler_desc.dataType               = MTLDataTypeSampler;
			sampler_desc.access                 = MTLBindingAccessReadOnly;
			[argument_descs addObject:sampler_desc];
		}

		layout_metal->_range_count = crgpumetal___set_ranges_normalize(layout_metal->_ranges, layout_metal->_range_count);

		layout_metal->_argument_encoder_mtl =
		    [layout_metal->_parent_device->_device_mtl newArgumentEncoderWithArguments:argument_descs];
		if (!layout_metal->_argument_encoder_mtl) {
			sr = CR_RESULT_API_SPECIFIC_FAILURE;
			goto failedattempt;
		}
	}

	CRGPU___LIVE_ADD(&layout_metal->_parent_device->_live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
	if (cr_debug_name_set(layout_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_descriptor_set_layout_destroy((crgpu_descriptor_set_layout)layout_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	layout_metal->_desc.debug_name = layout_metal->head.debug_name;

	*out_layout = (crgpu_descriptor_set_layout)layout_metal;

	return sr;

failedattempt:
	if (layout_metal) {
		if (layout_metal->_argument_encoder_mtl) {
			[layout_metal->_argument_encoder_mtl release];
		}
		CR_DELETE(&device->head.allocator, crgpu_descriptor_set_layout_metal, layout_metal);
	}
	return sr;
}

void crgpu_descriptor_set_layout_destroy(crgpu_descriptor_set_layout layout) {
	crgpu_descriptor_set_layout_metal *layout_metal = (crgpu_descriptor_set_layout_metal *)layout;
	if (layout_metal) {
		if (CR_STALE(layout, "crgpu_descriptor_set_layout_destroy", "layout"))
			return;
		if (layout_metal->_argument_encoder_mtl) {
			[layout_metal->_argument_encoder_mtl release];
		}
		CRGPU___LIVE_REMOVE(&layout_metal->_parent_device->_live, CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT);
		CR_KILL(layout);
		const CR_ALLOCATOR allocator = layout_metal->head.allocator;
		cr_string_free(&allocator, layout_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(layout_metal), &allocator, crgpu_descriptor_set_layout_metal, layout_metal);
	}
}

CR_RESULT crgpu_descriptor_pool_create(
    crgpu_device device,
    const crgpu_descriptor_pool_desc *desc,
    crgpu_descriptor_pool *out_pool) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_pool_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_pool)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (desc->pool_size_count > CRGPU_MAX_DESCRIPTOR_POOL_SIZES)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_POOLS;
	for (uint32_t i = 0; i < desc->pool_size_count; ++i) {
		const CR_RESULT type_sr = crgpu___internal_check_descriptor_type(desc->pool_sizes[i].descriptor_type, ((crgpu_device_metal *)device)->_features);
		if (type_sr != CR_SUCCESS)
			return type_sr;
	}
	// What the pool takes out of the device's budget, the same count on
	// every backend; given back when the pool is destroyed.
	uint64_t budget_descriptors = 0;
	uint64_t budget_samplers    = 0;
	crgpu___internal_pool_descriptor_counts(desc, &budget_descriptors, &budget_samplers);
	CR_RESULT sr = crgpu___internal_budget_take(&device->_budget, budget_descriptors, budget_samplers);
	if (sr != CR_SUCCESS)
		return sr;

	crgpu_descriptor_pool_metal *pool_metal = CR_NEW_ZEROED(&device->head.allocator, crgpu_descriptor_pool_metal);
	if (!pool_metal) {
		sr = CR_RESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}
	pool_metal->head.allocator      = device->head.allocator;
	CR_BIRTH_IN(pool_metal, &device->_quarantine);
	pool_metal->_parent_device      = (crgpu_device_metal *)device;
	pool_metal->_desc               = *desc;
	pool_metal->_budget_descriptors = budget_descriptors;
	pool_metal->_budget_samplers    = budget_samplers;

	// Metal needs no real backing arena the way Vulkan's VkDescriptorPool
	// does (see crgpu_descriptor_pool_metal's struct comment) - this array
	// only exists so crgpu_descriptor_pool_reset/crgpu_descriptor_pool_destroy
	// can free every set's MTLBuffer in bulk, matching those calls' "free
	// all sets allocated from this pool" contract instead of leaking one
	// small GPU buffer per set on every per-frame reset.
	// _desc.max_sets long, which is the count it is freed with.
	pool_metal->_tracked_sets_mtl =
	    CR_NEW_ARRAY_ZEROED(&pool_metal->head.allocator, crgpu_descriptor_set_metal *, desc->max_sets);
	if (!pool_metal->_tracked_sets_mtl) {
		sr = CR_RESULT_OUT_OF_MEMORY;
		goto failedattempt;
	}

	// From here crgpu_descriptor_pool_destroy frees it, gives its budget
	// back and takes the count back.
	CRGPU___LIVE_ADD(&pool_metal->_parent_device->_live, CRGPU___OBJECT_DESCRIPTOR_POOL);
	if (cr_debug_name_set(pool_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_descriptor_pool_destroy((crgpu_descriptor_pool)pool_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pool_metal->_desc.debug_name = pool_metal->head.debug_name;

	*out_pool = (crgpu_descriptor_pool)pool_metal;

	return sr;

failedattempt:
	crgpu___internal_budget_give_back(&device->_budget, budget_descriptors, budget_samplers);
	if (pool_metal) {
		CR_DELETE_ARRAY(
		    &device->head.allocator, crgpu_descriptor_set_metal *, pool_metal->_tracked_sets_mtl,
		    pool_metal->_desc.max_sets);
		CR_DELETE(&device->head.allocator, crgpu_descriptor_pool_metal, pool_metal);
	}
	return sr;
}

static void crgpumetal___internal_free_tracked_sets(crgpu_descriptor_pool_metal *pool_metal) {
	for (uint32_t i = 0; i < pool_metal->_allocated_set_count; i++) {
		crgpu_descriptor_set_metal *set_metal = pool_metal->_tracked_sets_mtl[i];
		if (!set_metal)
			continue;
		CR_KILL(set_metal);
		if (set_metal->_argument_buffer_mtl) {
			[set_metal->_argument_buffer_mtl release];
		}
		const CR_ALLOCATOR allocator = set_metal->head.allocator;
		cr_string_free(&allocator, set_metal->head.debug_name);
		CR_RETIRE(CR_QUARANTINE_OF(set_metal), &allocator, crgpu_descriptor_set_metal, set_metal);
	}
	pool_metal->_allocated_set_count = 0;
}

void crgpu_descriptor_pool_reset(crgpu_descriptor_pool pool) {
	crgpu_descriptor_pool_metal *pool_metal = (crgpu_descriptor_pool_metal *)pool;
	if (!pool_metal)
		return;
	if (CR_STALE(pool, "crgpu_descriptor_pool_reset", "pool"))
		return;
	crgpumetal___internal_free_tracked_sets(pool_metal);
}

void crgpu_descriptor_pool_destroy(crgpu_descriptor_pool pool) {
	crgpu_descriptor_pool_metal *pool_metal = (crgpu_descriptor_pool_metal *)pool;
	if (!pool_metal)
		return;
	if (CR_STALE(pool, "crgpu_descriptor_pool_destroy", "pool"))
		return;
	crgpumetal___internal_free_tracked_sets(pool_metal);
	crgpu___internal_budget_give_back(&pool_metal->_parent_device->_budget, pool_metal->_budget_descriptors, pool_metal->_budget_samplers);
	CRGPU___LIVE_REMOVE(&pool_metal->_parent_device->_live, CRGPU___OBJECT_DESCRIPTOR_POOL);
	CR_KILL(pool);
	const CR_ALLOCATOR allocator = pool_metal->head.allocator;
	CR_DELETE_ARRAY(&allocator, crgpu_descriptor_set_metal *, pool_metal->_tracked_sets_mtl, pool_metal->_desc.max_sets);
	cr_string_free(&allocator, pool_metal->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(pool_metal), &allocator, crgpu_descriptor_pool_metal, pool_metal);
}

CR_RESULT crgpu_descriptor_set_create(
    crgpu_device device,
    const crgpu_descriptor_set_desc *desc,
    crgpu_descriptor_set *out_sets) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_descriptor_set_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_sets)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->pool)
		return CR_RESULT_GPU_INVALID_DESCRIPTOR_POOL;
	if (desc->set_count == 0)
		return CR_RESULT_GPU_ZERO_DESCRIPTOR_SET_LAYOUTS;
	if (desc->set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return CR_RESULT_GPU_TOO_MANY_DESCRIPTOR_SET_LAYOUTS;

	crgpu_descriptor_pool_metal *pool_metal = (crgpu_descriptor_pool_metal *)desc->pool;
	if (pool_metal->_allocated_set_count + desc->set_count > pool_metal->_desc.max_sets)
		return CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;

	for (uint32_t i = 0; i < desc->set_count; i++) {
		crgpu_descriptor_set_layout_metal *layout_metal =
		    (crgpu_descriptor_set_layout_metal *)desc->set_layouts[i];
		if (!layout_metal || !layout_metal->_argument_encoder_mtl)
			return CR_RESULT_GPU_INVALID_DESCRIPTOR_SET_LAYOUT;

		crgpu_descriptor_set_metal *set_metal =
		    CR_NEW_ZEROED(&pool_metal->head.allocator, crgpu_descriptor_set_metal);
		if (!set_metal)
			return CR_RESULT_GPU_INTERNAL_DESCRIPTOR_SET_ALLOCATION_FAIL;

		set_metal->head.allocator = pool_metal->head.allocator;
		CR_BIRTH_IN(set_metal, &pool_metal->_parent_device->_quarantine);
		set_metal->_parent_pool   = pool_metal;
		set_metal->_layout      = layout_metal;
		set_metal->_argument_buffer_mtl =
		    [pool_metal->_parent_device->_device_mtl newBufferWithLength:layout_metal->_argument_encoder_mtl.encodedLength
		                                                          options:MTLResourceStorageModeShared];
		if (!set_metal->_argument_buffer_mtl) {
			CR_DELETE(&pool_metal->head.allocator, crgpu_descriptor_set_metal, set_metal);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}

		pool_metal->_tracked_sets_mtl[pool_metal->_allocated_set_count] = set_metal;
		pool_metal->_allocated_set_count++;

		out_sets[i] = (crgpu_descriptor_set)set_metal;
	}

	return CR_SUCCESS;
}

// Tracks [resource] (and [structure], for an acceleration structure) as
// what [set]'s [argument_index] references, replacing what that index
// tracked before, if anything: the argument buffer no longer references the
// old resource, and it may since have been destroyed.
static void crgpumetal___track_argument(
    crgpu_descriptor_set_metal *set,
    uint32_t argument_index,
    id<MTLResource> resource,
    MTLResourceUsage usage,
    crgpu_acceleration_structure_metal *structure) {
	uint32_t slot = 0;
	while (slot < set->_tracked_resource_count && set->_tracked_argument_indices[slot] != argument_index)
		slot++;
	if (slot < CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET) {
		set->_tracked_argument_indices[slot]    = argument_index;
		set->_tracked_resources_mtl[slot]       = resource;
		set->_tracked_resource_usages_mtl[slot] = usage;
		set->_tracked_structures[slot]          = structure;
		if (slot == set->_tracked_resource_count)
			set->_tracked_resource_count++;
	}
}

void crgpu_device_update_descriptor_sets(
    crgpu_device device,
    const crgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	if (!device || !writes || write_count == 0)
		return;
	if (CR_STALE(device, "crgpu_device_update_descriptor_sets", "device"))
		return;
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
	if (!crgpu___internal_check_acceleration_structure_writes(((crgpu_device_metal *)device)->_features, writes, write_count))
		return;

	for (uint32_t i = 0; i < write_count; i++) {
		const crgpu_write_descriptor_set *w = &writes[i];
		if (!w->dst_set)
			continue;

		crgpu_descriptor_set_metal *set_metal = (crgpu_descriptor_set_metal *)w->dst_set;
		crgpu_descriptor_set_layout_metal *layout_metal = set_metal->_layout;
		if (!layout_metal || !layout_metal->_argument_encoder_mtl)
			continue;

		id<MTLArgumentEncoder> encoder = layout_metal->_argument_encoder_mtl;
		[encoder setArgumentBuffer:set_metal->_argument_buffer_mtl offset:0];
		uint32_t argument_index = w->dst_binding + w->dst_array_element;

		id<MTLResource> tracked_resource   = nil;
		MTLResourceUsage tracked_usage     = MTLResourceUsageRead;
		crgpu_acceleration_structure_metal *tracked_structure = NULL;

		switch (w->descriptor_type) {
		case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
			if (!w->buffer_info || !w->buffer_info->buffer)
				continue;
			crgpu_buffer_metal *buffer_metal = (crgpu_buffer_metal *)w->buffer_info->buffer;
			[encoder setBuffer:buffer_metal->_buffer_mtl offset:w->buffer_info->offset atIndex:argument_index];
			tracked_resource = buffer_metal->_buffer_mtl;
			tracked_usage    = (w->descriptor_type == CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			                       ? (MTLResourceUsageRead | MTLResourceUsageWrite)
			                       : MTLResourceUsageRead;
			break;
		}
		case CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
			if (!w->image_info || !w->image_info->image_view)
				continue;
			crgpu_image_view_metal *view_metal = (crgpu_image_view_metal *)w->image_info->image_view;
			[encoder setTexture:view_metal->_texture_view_mtl atIndex:argument_index];
			tracked_resource = view_metal->_texture_view_mtl;
			tracked_usage    = (w->descriptor_type == CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE)
			                       ? (MTLResourceUsageRead | MTLResourceUsageWrite)
			                       : MTLResourceUsageRead;
			// A combined descriptor's sampler is its second argument.
			if (w->descriptor_type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w->sampler) {
				crgpu_sampler_metal *sampler_metal = (crgpu_sampler_metal *)w->sampler;
				[encoder setSamplerState:sampler_metal->_sampler_state_mtl
				                 atIndex:CRGPU_METAL_COMBINED_SAMPLER_INDEX_BASE + argument_index];
			}
			break;
		}
		case CRGPU_DESCRIPTOR_TYPE_SAMPLER: {
			if (!w->sampler)
				continue;
			crgpu_sampler_metal *sampler_metal = (crgpu_sampler_metal *)w->sampler;
			[encoder setSamplerState:sampler_metal->_sampler_state_mtl atIndex:argument_index];
			// id<MTLSamplerState> isn't an id<MTLResource> - no residency
			// tracking (useResource:) applies to samplers on Metal.
			break;
		}
		case CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
			// Checked above: every element a live top level. Every element
			// is written, element k at the argument index after k others
			// (the binding's array), and each tracked.
			for (uint32_t k = 0; k < w->descriptor_count; ++k) {
				crgpu_acceleration_structure_metal *structure = w->acceleration_structures[k];
				[encoder setAccelerationStructure:structure->_structure_mtl atIndex:argument_index + k];
				crgpumetal___track_argument(set_metal, argument_index + k, structure->_structure_mtl, MTLResourceUsageRead, structure);
			}
			continue;
		}
		default:
			continue;
		}

		if (tracked_resource)
			crgpumetal___track_argument(set_metal, argument_index, tracked_resource, tracked_usage, tracked_structure);
	}
}

void crgpu_command_list_bind_graphics_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_graphics_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || set_count == 0)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_graphics_descriptor_sets", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_graphics_descriptor_sets", "pipeline"))
		return;
	if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!cmd_metal->_active_render_encoder)
		return;

	// A mesh pipeline has no vertex stage at all - bind to the mesh stage
	// instead (-setMeshBuffer:) via crgpu_shader_pipeline_metal::
	// _is_mesh_pipeline, set at pipeline-creation time (crgpumetalshader.m).
	// Fragment stays the same either way.
	crgpu_shader_pipeline_metal *pipeline_metal = (crgpu_shader_pipeline_metal *)pipeline;
	bool is_mesh_pipeline                         = pipeline_metal->_is_mesh_pipeline;
	MTLRenderStages non_fragment_stage             = is_mesh_pipeline ? MTLRenderStageMesh : MTLRenderStageVertex;

	for (uint32_t i = 0; i < set_count; i++) {
		crgpu_descriptor_set_metal *set_metal = (crgpu_descriptor_set_metal *)sets[i];
		if (!set_metal)
			continue;

		// Bound only to the stages the layout's bindings name, as Vulkan
		// and D3D12 make a binding visible only to its stage_flags. Binding
		// it to a stage whose function has no argument at the index is a
		// Metal validation error.
		const crgpu_descriptor_set_layout_desc *layout_desc = &set_metal->_layout->_desc;
		uint32_t set_stages                                     = 0;
		for (uint32_t b = 0; b < layout_desc->binding_count; b++)
			set_stages |= (uint32_t)layout_desc->bindings[b].stage_flags;
		uint32_t non_fragment_flags = is_mesh_pipeline ? (CRGPU_SHADER_STAGE_MESH | CRGPU_SHADER_STAGE_TASK) : CRGPU_SHADER_STAGE_VERTEX;
		// And only to a stage whose function in this pipeline reads the set
		// (crgpu_shader_pipeline_metal::_non_fragment_set_mask).
		uint32_t set_bit            = 1u << (first_set + i);
		bool to_non_fragment        = (set_stages & non_fragment_flags) != 0 && (pipeline_metal->_non_fragment_set_mask & set_bit) != 0;
		bool to_fragment            = (set_stages & CRGPU_SHADER_STAGE_FRAGMENT) != 0 && (pipeline_metal->_fragment_set_mask & set_bit) != 0;
		if (!to_non_fragment && !to_fragment)
			continue;

		// Set only where the stage doesn't have it already: Metal's
		// validation refuses setting the same buffer again, and a caller
		// binding one set for several pipelines in a pass is ordinary.
		const uint32_t set_index  = first_set + i;
		uint32_t buffer_index     = CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + set_index;
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
		// CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET in crgpumetal.h.
		for (uint32_t r = 0; r < set_metal->_tracked_resource_count; r++) {
			[cmd_metal->_active_render_encoder useResource:set_metal->_tracked_resources_mtl[r]
			                                          usage:set_metal->_tracked_resource_usages_mtl[r]
			                                         stages:resident_stages];
			if (set_metal->_tracked_structures[r])
				crgpumetal___use_bottom_levels(cmd_metal->_active_render_encoder, true, resident_stages, set_metal->_tracked_structures[r]);
		}
	}
}

void crgpu_command_list_bind_compute_descriptor_sets(
    crgpu_command_list cmd,
    crgpu_compute_pipeline pipeline,
    uint32_t first_set,
    const crgpu_descriptor_set *sets,
    uint32_t set_count) {
	if (!cmd || !pipeline || !sets || set_count == 0)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_bind_compute_descriptor_sets", "cmd"))
		return;
	if (CR_STALE(pipeline, "crgpu_command_list_bind_compute_descriptor_sets", "pipeline"))
		return;
	if (set_count > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
		return;
	if (first_set > CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS - set_count)
		return;

	crgpu_command_list_metal *cmd_metal = (crgpu_command_list_metal *)cmd;
	if (!cmd_metal->_active_compute_encoder)
		return;

	for (uint32_t i = 0; i < set_count; i++) {
		crgpu_descriptor_set_metal *set_metal = (crgpu_descriptor_set_metal *)sets[i];
		if (!set_metal)
			continue;

		// The set already bound at this index on this encoder: its buffer
		// is set and its resources resident, and Metal's validation refuses
		// setting the same buffer again.
		if (cmd_metal->_encoder_compute_sets[first_set + i] == set_metal->_argument_buffer_mtl)
			continue;
		uint32_t buffer_index = CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + first_set + i;
		[cmd_metal->_active_compute_encoder setBuffer:set_metal->_argument_buffer_mtl offset:0 atIndex:buffer_index];
		cmd_metal->_encoder_compute_sets[first_set + i] = set_metal->_argument_buffer_mtl;

		// Every resource reached only indirectly through this argument
		// buffer must be marked resident on this encoder - see
		// CRGPU_METAL_MAX_TRACKED_RESOURCES_PER_SET in crgpumetal.h.
		// MTLComputeCommandEncoder's -useResource:usage: has no stages:
		// parameter (unlike the render encoder's) - there's only one stage.
		for (uint32_t r = 0; r < set_metal->_tracked_resource_count; r++) {
			[cmd_metal->_active_compute_encoder useResource:set_metal->_tracked_resources_mtl[r]
			                                           usage:set_metal->_tracked_resource_usages_mtl[r]];
			if (set_metal->_tracked_structures[r])
				crgpumetal___use_bottom_levels(cmd_metal->_active_compute_encoder, false, 0, set_metal->_tracked_structures[r]);
		}
	}
}

#endif // CRGPU_COMPILE_METAL_API
