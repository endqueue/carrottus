#include "spudgpu.h"
#include "spudgpushared.h"
#include "../spudallocator.h"
#include "../spudstructbase.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

SPUDRESULT spudgpu___internal_check_instance_desc(
    const spudgpu_instance_desc *desc,
    spudgpu_instance *out_instance) {
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_instance)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_instance = NULL;
	return spud_allocator_check(desc->allocator);
}

bool spudgpu___internal_blend_attachments_equal(
    const spudgpu_blend_attachment_desc *a,
    const spudgpu_blend_attachment_desc *b) {
	if (a->blend_enable != b->blend_enable)
		return false;
	if (!a->blend_enable)
		return true;
	return a->src_color_blend_factor == b->src_color_blend_factor && a->dst_color_blend_factor == b->dst_color_blend_factor &&
	       a->color_blend_op == b->color_blend_op && a->src_alpha_blend_factor == b->src_alpha_blend_factor &&
	       a->dst_alpha_blend_factor == b->dst_alpha_blend_factor && a->alpha_blend_op == b->alpha_blend_op;
}

SPUDGPU_SHADER_FORMAT spudgpu___internal_shader_format(void) {
#if SPUDGPU_COMPILE_VULKAN_API
	return SPUDGPU_SHADER_FORMAT_SPIRV;
#elif SPUDGPU_COMPILE_D3D12_API
	return SPUDGPU_SHADER_FORMAT_DXIL;
#else
	return SPUDGPU_SHADER_FORMAT_METALLIB;
#endif
}

SPUDRESULT spudgpu___internal_check_shader_module_desc(
    const spudgpu_shader_module_desc *desc,
    SPUDGPU_FEATURES features) {
	const SPUDGPU_SHADER_STAGE stage = desc->stage;
	// One known stage: a single bit of the enum.
	if (stage == SPUDGPU_SHADER_STAGE_NONE || (stage & (stage - 1)) != 0 || stage > SPUDGPU_SHADER_STAGE_TASK)
		return SPUDRESULT_GPU_INVALID_SHADER_STAGE;
	if (desc->format != spudgpu___internal_shader_format())
		return SPUDRESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED;
	if (!desc->code || desc->code_size == 0)
		return SPUDRESULT_GPU_NULL_SHADER_CODE;
	if ((uintptr_t)desc->code % 4 != 0)
		return SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT;
	if (desc->format != SPUDGPU_SHADER_FORMAT_METALLIB && desc->code_size % 4 != 0)
		return SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT;
	if (!desc->entry_point || !desc->entry_point[0])
		return SPUDRESULT_GPU_NULL_ENTRY_POINT;
	const bool has_workgroup =
	    stage == SPUDGPU_SHADER_STAGE_COMPUTE || stage == SPUDGPU_SHADER_STAGE_MESH || stage == SPUDGPU_SHADER_STAGE_TASK;
	for (uint32_t i = 0; i < 3; ++i) {
		if (has_workgroup && desc->workgroup_size[i] == 0)
			return SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE;
		if (!has_workgroup && desc->workgroup_size[i] != 0)
			return SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE;
	}
	// A mesh or task module needs the feature even as a module: its SPIR-V
	// declares MeshShadingEXT, which Vulkan refuses without it, and the rule
	// holds on every backend.
	if ((stage == SPUDGPU_SHADER_STAGE_MESH || stage == SPUDGPU_SHADER_STAGE_TASK) &&
	    !(features & SPUDGPU_FEATURE_MESH_SHADING))
		return SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	// Geometry and tessellation SPIR-V declares capabilities Vulkan refuses
	// without the features; the rule holds on every backend, and on Metal,
	// whose GPUs never have the bits.
	if (stage == SPUDGPU_SHADER_STAGE_GEOMETRY && !(features & SPUDGPU_FEATURE_GEOMETRY_SHADER))
		return SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED;
	if ((stage == SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL || stage == SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION) &&
	    !(features & SPUDGPU_FEATURE_TESSELLATION))
		return SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu___internal_check_pipeline_outputs(
    const spudgpu_shader_pipeline_desc *desc,
    SPUDGPU_FEATURES features) {
	if (desc->geometry_module && !(features & SPUDGPU_FEATURE_GEOMETRY_SHADER))
		return SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED;
	if ((desc->tess_control_module || desc->tess_eval_module) && !(features & SPUDGPU_FEATURE_TESSELLATION))
		return SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED;
	if (desc->color_attachment_count > SPUDGPU_MAX_COLOR_ATTACHMENTS)
		return SPUDRESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS;
	if (desc->depth_bias_clamp != 0.0f && !(features & SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP))
		return SPUDRESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED;
	if (!(features & SPUDGPU_FEATURE_INDEPENDENT_BLEND))
		for (uint32_t i = 1; i < desc->color_attachment_count; ++i)
			if (!spudgpu___internal_blend_attachments_equal(&desc->blend_attachments[0], &desc->blend_attachments[i]))
				return SPUDRESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED;
	return SPUD_SUCCESS;
}

// ---------------------------------------------------------------------------
// Adapters and devices
// ---------------------------------------------------------------------------

SPUDRESULT spudgpu___internal_get_adapter_properties(
    const spudgpu___adapter_facts *facts,
    SPUDGPU_ADAPTER_PROPERTIES *out_properties) {
	if (!out_properties)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (out_properties->base.type != SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES)
		return SPUDRESULT_STRUCT_WRONG_TYPE;

	// The whole chain is checked before anything is written: a call that
	// fails leaves every struct of it as it was.
	bool seen_mesh      = false;
	bool seen_bindless  = false;
	bool seen_ray_query = false;
	for (const SPUD_STRUCT_BASE_OUT *link = out_properties->base.next; link; link = link->next) {
		switch (link->type) {
#if SPUDGPU_EXT_MESH_SHADING
		case SPUD_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS:
			if (seen_mesh)
				return SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_mesh = true;
			break;
#endif
#if SPUDGPU_EXT_BINDLESS
		case SPUD_STRUCT_TYPE_GPU_BINDLESS_LIMITS:
			if (seen_bindless)
				return SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_bindless = true;
			break;
#endif
		// Declared on every backend, as its feature bit is.
		case SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS:
			if (seen_ray_query)
				return SPUDRESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_ray_query = true;
			break;
		default:
			return SPUDRESULT_STRUCT_UNKNOWN_IN_CHAIN;
		}
	}
	(void)seen_mesh;
	(void)seen_bindless;

	// Every field but the base is SpudGPU's to write; the base, chain
	// included, stays the caller's.
	const SPUD_STRUCT_BASE_OUT base = out_properties->base;
	*out_properties                 = facts->properties;
	out_properties->base            = base;
	out_properties->shader_formats  = 1u << spudgpu___internal_shader_format();

	for (SPUD_STRUCT_BASE_OUT *link = out_properties->base.next; link; link = link->next) {
#if SPUDGPU_EXT_MESH_SHADING
		if (link->type == SPUD_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS) {
			SPUDGPU_MESH_SHADING_LIMITS *mesh   = (SPUDGPU_MESH_SHADING_LIMITS *)link;
			mesh->max_mesh_output_vertices       = facts->mesh_max_output_vertices;
			mesh->max_mesh_output_primitives     = facts->mesh_max_output_primitives;
			mesh->max_mesh_workgroup_invocations = facts->mesh_max_workgroup_invocations;
		}
#endif
#if SPUDGPU_EXT_BINDLESS
		if (link->type == SPUD_STRUCT_TYPE_GPU_BINDLESS_LIMITS) {
			SPUDGPU_BINDLESS_LIMITS *bindless = (SPUDGPU_BINDLESS_LIMITS *)link;
			bindless->max_sampled_images      = facts->bindless_max_sampled_images;
			bindless->max_storage_images      = facts->bindless_max_storage_images;
			bindless->max_storage_buffers     = facts->bindless_max_storage_buffers;
		}
#endif
		if (link->type == SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS) {
			SPUDGPU_RAY_QUERY_LIMITS *ray_query  = (SPUDGPU_RAY_QUERY_LIMITS *)link;
			ray_query->max_geometries           = facts->ray_query_max_geometries;
			ray_query->max_instances            = facts->ray_query_max_instances;
			ray_query->max_primitives           = facts->ray_query_max_primitives;
			ray_query->scratch_offset_alignment = facts->ray_query_scratch_offset_alignment;
			ray_query->placement_alignment      = facts->ray_query_placement_alignment;
		}
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu___internal_check_swap_chain_desc(
    SPUDGPU_FEATURES features,
    const spudgpu_swap_chain_desc *desc,
    const spudgpu_swap_chain *out_swap_chain) {
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_swap_chain)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->surface)
		return SPUDRESULT_GPU_INVALID_SURFACE;
	if (!desc->queue)
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!(features & SPUDGPU_FEATURE_PRESENT))
		return SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED;
	switch (desc->present_scaling) {
	case SPUDGPU_PRESENT_SCALING_ONE_TO_ONE:
	case SPUDGPU_PRESENT_SCALING_STRETCH:
	case SPUDGPU_PRESENT_SCALING_ASPECT_FIT:
		return SPUD_SUCCESS;
	default:
		return SPUDRESULT_GPU_INVALID_PRESENT_SCALING;
	}
}

SPUDRESULT spudgpu___internal_check_surface_present_scalings(
    SPUDGPU_FEATURES features,
    const void *surface,
    const uint32_t *out_scalings) {
	if (!surface)
		return SPUDRESULT_GPU_INVALID_SURFACE;
	// The surface's struct is the backend's; its head is first in every one.
	if (SPUD_STALE_HEAD((const spud_object_head *)surface, "spudgpu_get_surface_present_scalings", "surface"))
		return SPUDRESULT_GPU_INVALID_SURFACE;
	if (!out_scalings)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!(features & SPUDGPU_FEATURE_PRESENT))
		return SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED;
	return SPUD_SUCCESS;
}

uint32_t spudgpu___internal_queue_count(
    const spudgpu_device_desc *desc,
    SPUDGPU_COMMAND_LIST_TYPE type) {
	switch (type) {
	case SPUDGPU_COMMAND_LIST_TYPE_DIRECT:
		return desc->direct_queue_count;
	case SPUDGPU_COMMAND_LIST_TYPE_COMPUTE:
		return desc->compute_queue_count;
	case SPUDGPU_COMMAND_LIST_TYPE_COPY:
		return desc->copy_queue_count;
	default:
		return 0;
	}
}

SPUDRESULT spudgpu___internal_check_device_desc(
    const spudgpu___adapter_facts *facts,
    const spudgpu_device_desc *desc,
    spudgpu_device *out_device) {
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_result = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_DEVICE_DESC);
	if (base_result != SPUD_SUCCESS)
		return base_result;
	if (!out_device)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_device = NULL;

	const SPUDGPU_ADAPTER_PROPERTIES *properties = &facts->properties;
	if (desc->required_features & ~properties->supported_features)
		return SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED;
	if (desc->direct_queue_count == 0 && desc->compute_queue_count == 0 && desc->copy_queue_count == 0)
		return SPUDRESULT_ZERO_SIZE;
	for (uint32_t i = 0; i < properties->queue_pool_count; ++i) {
		const SPUDGPU_QUEUE_POOL *pool = &properties->queue_pools[i];
		// 64 bits: three counts near UINT32_MAX don't wrap into a small sum.
		uint64_t asked = 0;
		if (pool->kinds & SPUDGPU_QUEUE_KIND_DIRECT)
			asked += desc->direct_queue_count;
		if (pool->kinds & SPUDGPU_QUEUE_KIND_COMPUTE)
			asked += desc->compute_queue_count;
		if (pool->kinds & SPUDGPU_QUEUE_KIND_COPY)
			asked += desc->copy_queue_count;
		if (asked > pool->max_queues)
			return SPUDRESULT_GPU_TOO_MANY_QUEUES;
	}
	if (desc->max_descriptors > properties->max_descriptors)
		return SPUDRESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE;
	if (desc->max_samplers > properties->max_samplers)
		return SPUDRESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE;
	if (desc->required_features & SPUDGPU_FEATURE_BINDLESS) {
		const uint64_t table = (uint64_t)facts->bindless_max_sampled_images + facts->bindless_max_storage_images +
		                       facts->bindless_max_storage_buffers;
		if (table > desc->max_descriptors)
			return SPUDRESULT_GPU_OUT_OF_DESCRIPTORS;
	}
	return SPUD_SUCCESS;
}

void spudgpu___internal_pool_descriptor_counts(
    const spudgpu_descriptor_pool_desc *desc,
    uint64_t *out_descriptors,
    uint64_t *out_samplers) {
	uint64_t descriptors = 0;
	uint64_t samplers    = 0;
	for (uint32_t i = 0; i < desc->pool_size_count && i < SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES; ++i) {
		const spudgpu_descriptor_pool_size *size = &desc->pool_sizes[i];
		if (size->descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER) {
			samplers += size->count;
		} else {
			descriptors += size->count;
			// The image's descriptor and the sampler's, which D3D12 keeps in
			// two heaps and every backend counts the same way.
			if (size->descriptor_type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				samplers += size->count;
		}
	}
	*out_descriptors = descriptors;
	*out_samplers    = samplers;
}

bool spudgpu___internal_budget_init(
    spudgpu___descriptor_budget *budget,
    uint32_t max_descriptors,
    uint32_t max_samplers) {
	if (!spud_mutex_init(&budget->lock))
		return false;
	budget->max_descriptors  = max_descriptors;
	budget->max_samplers     = max_samplers;
	budget->used_descriptors = 0;
	budget->used_samplers    = 0;
	return true;
}

void spudgpu___internal_budget_destroy(spudgpu___descriptor_budget *budget) {
	spud_mutex_destroy(&budget->lock);
}

SPUDRESULT spudgpu___internal_budget_take(
    spudgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers) {
	SPUDRESULT result = SPUD_SUCCESS;
	spud_mutex_lock(&budget->lock);
	if (descriptors > budget->max_descriptors - budget->used_descriptors ||
	    samplers > budget->max_samplers - budget->used_samplers) {
		result = SPUDRESULT_GPU_OUT_OF_DESCRIPTORS;
	} else {
		budget->used_descriptors += descriptors;
		budget->used_samplers += samplers;
	}
	spud_mutex_unlock(&budget->lock);
	return result;
}

void spudgpu___internal_budget_give_back(
    spudgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers) {
	spud_mutex_lock(&budget->lock);
	assert(budget->used_descriptors >= descriptors);
	assert(budget->used_samplers >= samplers);
	budget->used_descriptors -= descriptors;
	budget->used_samplers -= samplers;
	spud_mutex_unlock(&budget->lock);
}

// ---------------------------------------------------------------------------
// Descriptor types
// ---------------------------------------------------------------------------

SPUDRESULT spudgpu___internal_check_descriptor_type(SPUDGPU_DESCRIPTOR_TYPE type, SPUDGPU_FEATURES features) {
	switch (type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case SPUDGPU_DESCRIPTOR_TYPE_SAMPLER:
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return SPUD_SUCCESS;
	case SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
		return (features & SPUDGPU_FEATURE_RAY_QUERY) ? SPUD_SUCCESS : SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	default:
		return SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
	}
}

// ---------------------------------------------------------------------------
// Acceleration structures
// ---------------------------------------------------------------------------

#if SPUDGPU_EXT_RAY_QUERY

// Instances are written by the CPU in little-endian words, as every target
// SpudGPU builds for is.
#define SPUDGPU___RQ_INDEX_LIMIT (1u << 24)
#define SPUDGPU___RQ_KNOWN_BUILD_FLAGS                                                                                         \
	(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD |           \
	 SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION)
#define SPUDGPU___RQ_KNOWN_GEOMETRY_FLAGS \
	(SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE | SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES)
#define SPUDGPU___RQ_KNOWN_INSTANCE_FLAGS                                                                                      \
	(SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING |                                                        \
	 SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FRONT_COUNTERCLOCKWISE | SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE |    \
	 SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE)

spudgpu_acceleration_structure_instance_layout spudgpu___internal_instance_layout(void) {
	spudgpu_acceleration_structure_instance_layout layout;
#if SPUDGPU_COMPILE_METAL_API
	// MTLIndirectAccelerationStructureInstanceDescriptor: 72 bytes, its
	// 64-bit resource ID last.
	layout.kind      = SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72;
	layout.size      = 72;
	layout.alignment = 8;
#else
	// VkAccelerationStructureInstanceKHR and D3D12_RAYTRACING_INSTANCE_DESC:
	// the same 64 bytes, an array of them 16-byte aligned on both.
	layout.kind      = SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64;
	layout.size      = 64;
	layout.alignment = 16;
#endif
	return layout;
}

// Whether [count] elements of [element] bytes, [stride] apart from
// [offset], fit in [size] bytes, with no arithmetic that can wrap.
static bool spudgpu___rq_fits(uint64_t size, uint64_t offset, uint64_t count, uint64_t stride, uint64_t element) {
	if (offset > size)
		return false;
	uint64_t room = size - offset;
	if (count == 0)
		return true;
	if (element > room)
		return false;
	room -= element;
	return stride == 0 || count - 1 <= room / stride;
}

// [buffer]'s desc through the public call, which refuses a destroyed one
// in Debug: SPUDRESULT_GPU_INVALID_BUFFER for NULL or destroyed.
static SPUDRESULT spudgpu___rq_buffer_desc(spudgpu_buffer buffer, spudgpu_buffer_desc *out_desc) {
	if (!buffer)
		return SPUDRESULT_GPU_INVALID_BUFFER;
	return spudgpu_get_buffer_desc(buffer, out_desc);
}

// [structure] is NULL, or destroyed (Debug).
static bool spudgpu___rq_invalid(spudgpu_acceleration_structure structure, const char *function, const char *param) {
	if (!structure)
		return true;
	return SPUD_STALE(structure, function, param);
}

SPUDRESULT spudgpu___internal_check_acceleration_structure_desc(const spudgpu_acceleration_structure_desc *desc) {
	if (desc->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && desc->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && desc->max_bottom_levels != 0)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (desc->size == 0)
		return SPUDRESULT_ZERO_SIZE;
	return SPUD_SUCCESS;
}

bool spudgpu___internal_acceleration_structure_state_init(
    spudgpu___acceleration_structure_state *state,
    const SPUD_ALLOCATOR *allocator,
    const spudgpu_acceleration_structure_desc *desc) {
	memset(state, 0, sizeof(*state));
	state->size              = desc->size;
	state->level             = desc->level;
	state->max_bottom_levels = desc->max_bottom_levels;
	state->allow_compaction  = desc->allow_compaction;
	state->eq_allocator      = spud_eq_allocator(allocator);
	eq_array_init(&state->geometries, &state->eq_allocator);
	if (desc->max_bottom_levels > 0) {
		state->bottom_levels = SPUD_NEW_ARRAY(allocator, spudgpu_acceleration_structure, desc->max_bottom_levels);
		if (!state->bottom_levels)
			return false;
	}
	return true;
}

void spudgpu___internal_acceleration_structure_state_free(
    spudgpu___acceleration_structure_state *state,
    const SPUD_ALLOCATOR *allocator) {
	if (state->bottom_levels)
		SPUD_DELETE_ARRAY(allocator, spudgpu_acceleration_structure, state->bottom_levels, state->max_bottom_levels);
	state->bottom_levels = NULL;
	eq_array_free(&state->geometries);
}

// A bottom level's geometries, in the header's order.
static SPUDRESULT spudgpu___rq_check_bottom(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_inputs *inputs) {
	if (inputs->instances || inputs->instance_offset || inputs->instance_count || inputs->bottom_levels ||
	    inputs->bottom_level_count)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!inputs->geometries)
		return SPUDRESULT_NULL_DESC;
	if (inputs->geometry_count == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (inputs->geometry_count > facts->ray_query_max_geometries)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	const SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND first_kind = inputs->geometries[0].kind;
	uint64_t primitives                                          = 0;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const spudgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
		if (g->kind != SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES && g->kind != SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		if (g->kind != first_kind)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		if (g->flags & ~(SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS)SPUDGPU___RQ_KNOWN_GEOMETRY_FLAGS)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;

		spudgpu_buffer_desc data;
		const SPUDRESULT data_sr = spudgpu___rq_buffer_desc(g->data, &data);
		if (data_sr != SPUD_SUCCESS)
			return data_sr;
		if (!(data.usage & SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
			return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
		const bool triangles   = g->kind == SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
		const uint64_t align   = triangles ? 4 : 8;
		const uint64_t element = triangles ? 12 : 24;
		if (g->data_offset % align != 0 || g->data_stride % align != 0 || g->data_stride < element || g->data_stride > UINT32_MAX)
			return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
		if (g->data_count == 0)
			return SPUDRESULT_ZERO_SIZE;
		if (!spudgpu___rq_fits(data.size, g->data_offset, g->data_count, g->data_stride, element))
			return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE;

		const bool known_format = g->index_format == SPUDGPU_FORMAT_UNKNOWN ||
		                          (triangles && (g->index_format == SPUDGPU_FORMAT_R16_UINT || g->index_format == SPUDGPU_FORMAT_R32_UINT));
		if (!known_format)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		const bool indexed = g->index_format != SPUDGPU_FORMAT_UNKNOWN;
		if (!indexed && g->indices)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		uint64_t count = 0;
		if (indexed) {
			spudgpu_buffer_desc index_desc;
			const SPUDRESULT index_sr = spudgpu___rq_buffer_desc(g->indices, &index_desc);
			if (index_sr != SPUD_SUCCESS)
				return index_sr;
			if (!(index_desc.usage & SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
				return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
			const uint64_t index_size = g->index_format == SPUDGPU_FORMAT_R16_UINT ? 2 : 4;
			if (g->index_offset % index_size != 0)
				return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
			if (g->index_count == 0)
				return SPUDRESULT_ZERO_SIZE;
			if (g->index_count % 3 != 0)
				return SPUDRESULT_DESC_INVALID_PARAMETERS;
			if (!spudgpu___rq_fits(index_desc.size, g->index_offset, g->index_count, index_size, index_size))
				return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
			count = g->index_count / 3;
		} else {
			if (g->index_offset != 0 || g->index_count != 0)
				return SPUDRESULT_DESC_INVALID_PARAMETERS;
			if (triangles && g->data_count % 3 != 0)
				return SPUDRESULT_DESC_INVALID_PARAMETERS;
			count = triangles ? g->data_count / 3 : g->data_count;
		}
		primitives += count;
		if (primitives > facts->ray_query_max_primitives)
			return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	}
	return SPUD_SUCCESS;
}

// A top level's instances and bottom levels, in the header's order.
static SPUDRESULT spudgpu___rq_check_top(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_inputs *inputs,
    const char *function) {
	if (inputs->geometries || inputs->geometry_count)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	spudgpu_buffer_desc instances;
	const SPUDRESULT instances_sr = spudgpu___rq_buffer_desc(inputs->instances, &instances);
	if (instances_sr != SPUD_SUCCESS)
		return instances_sr;
	if (!(instances.usage & SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
		return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
	const spudgpu_acceleration_structure_instance_layout layout = spudgpu___internal_instance_layout();
	if (inputs->instance_offset % layout.alignment != 0)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	if (inputs->instance_count > facts->ray_query_max_instances)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	if (!spudgpu___rq_fits(instances.size, inputs->instance_offset, inputs->instance_count, layout.size, layout.size))
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
	if (inputs->bottom_level_count > 0 && !inputs->bottom_levels)
		return SPUDRESULT_NULL_DESC;
	for (uint32_t i = 0; i < inputs->bottom_level_count; ++i) {
		const spudgpu_acceleration_structure bottom = inputs->bottom_levels[i];
		if (spudgpu___rq_invalid(bottom, function, "inputs->bottom_levels[i]"))
			return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		if (spudgpu___as_state(bottom)->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
			return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	}
	return SPUD_SUCCESS;
}

// The inputs' checks after their base, for [function]'s stale message.
static SPUDRESULT spudgpu___rq_check_inputs(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_inputs *inputs,
    const char *function) {
	if (inputs->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && inputs->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (inputs->flags & ~(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS)SPUDGPU___RQ_KNOWN_BUILD_FLAGS)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if ((inputs->flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE) &&
	    (inputs->flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD))
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return spudgpu___rq_check_bottom(facts, inputs);
	return spudgpu___rq_check_top(facts, inputs, function);
}

SPUDRESULT spudgpu___internal_check_acceleration_structure_inputs(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_inputs *inputs) {
	return spudgpu___rq_check_inputs(facts, inputs, "spudgpu_get_acceleration_structure_build_sizes");
}

SPUDRESULT spudgpu___internal_check_acceleration_structure_list(SPUDGPU_COMMAND_LIST_TYPE type, bool rendering) {
	// Direct and compute queues build on every API; a copy queue builds on
	// none of the three's portable subset (Metal has no queue kinds, D3D12
	// and Vulkan no builds there), and a bundle records no build.
	if (type != SPUDGPU_COMMAND_LIST_TYPE_DIRECT && type != SPUDGPU_COMMAND_LIST_TYPE_COMPUTE)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE;
	if (rendering)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING;
	return SPUD_SUCCESS;
}

// Whether [state]'s last build has exactly [inputs]' layout: what an update
// must keep, compared value by value.
static bool spudgpu___rq_same_layout(const spudgpu___acceleration_structure_state *state, const spudgpu_acceleration_structure_inputs *inputs) {
	if (state->build_flags != inputs->flags)
		return false;
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return state->instance_count == inputs->instance_count;
	if (state->geometries.count != inputs->geometry_count)
		return false;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const spudgpu___geometry_layout *kept           = &state->geometries.data[i];
		const spudgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
		if (kept->kind != g->kind || kept->flags != g->flags || kept->data_count != g->data_count || kept->index_format != g->index_format ||
		    kept->index_count != g->index_count)
			return false;
	}
	return true;
}

// Whether a structure's layout is kept: a bottom level whose build allows
// updates (only an update compares it).
static bool spudgpu___rq_keeps_layout(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	return level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);
}

SPUDRESULT spudgpu___internal_reserve_build_layout(
    spudgpu_acceleration_structure destination,
    const spudgpu_acceleration_structure_inputs *inputs) {
	if (!spudgpu___rq_keeps_layout(inputs->level, inputs->flags))
		return SPUD_SUCCESS;
	spudgpu___acceleration_structure_state *state = spudgpu___as_state(destination);
	return eq_array_reserve(&state->geometries, inputs->geometry_count) == EQ_SUCCESS ? SPUD_SUCCESS : SPUDRESULT_OUT_OF_MEMORY;
}

SPUDRESULT spudgpu___internal_check_build_head(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_build *build) {
	const SPUDRESULT base_sr = spud_struct_check(&build->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (build->mode != SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD && build->mode != SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (!build->inputs)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT inputs_base_sr = spud_struct_check(&build->inputs->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != SPUD_SUCCESS)
		return inputs_base_sr;
	return spudgpu___rq_check_inputs(facts, build->inputs, "spudgpu_cmd_build_acceleration_structures");
}

SPUDRESULT spudgpu___internal_check_build_tail(
    const spudgpu___adapter_facts *facts,
    const spudgpu_acceleration_structure_build *build,
    const spudgpu_acceleration_structure_sizes *sizes) {
	static const char function[]                       = "spudgpu_cmd_build_acceleration_structures";
	const spudgpu_acceleration_structure_inputs *inputs = build->inputs;
	if (build->mode == SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD) {
		if (build->source)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
	} else {
		if (spudgpu___rq_invalid(build->source, function, "builds[i].source"))
			return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		const spudgpu___acceleration_structure_state *source = spudgpu___as_state(build->source);
		if (source->level != inputs->level)
			return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		if (!source->built || !(source->build_flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ||
		    !spudgpu___rq_same_layout(source, inputs))
			return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED;
	}
	if (spudgpu___rq_invalid(build->destination, function, "builds[i].destination"))
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const spudgpu___acceleration_structure_state *destination = spudgpu___as_state(build->destination);
	if (destination->level != inputs->level)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (destination->size < sizes->storage_size)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL;
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP && inputs->bottom_level_count > destination->max_bottom_levels)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;

	spudgpu_buffer_desc scratch;
	const SPUDRESULT scratch_sr = spudgpu___rq_buffer_desc(build->scratch, &scratch);
	if (scratch_sr != SPUD_SUCCESS)
		return scratch_sr;
	if (!(scratch.usage & SPUDGPU_BUFFER_USAGE_STORAGE))
		return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
	uint64_t address = 0;
	const SPUDRESULT address_sr = spudgpu_get_buffer_gpu_address(build->scratch, &address);
	if (address_sr != SPUD_SUCCESS)
		return address_sr;
	// The alignment is a power of two, so the sum's low bits are right
	// even where it wraps.
	if (((address + build->scratch_offset) & (uint64_t)(facts->ray_query_scratch_offset_alignment - 1u)) != 0)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	const uint64_t needed = build->mode == SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD ? sizes->build_scratch_size : sizes->update_scratch_size;
	if (!spudgpu___rq_fits(scratch.size, build->scratch_offset, 1, 0, needed))
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL;
	return SPUD_SUCCESS;
}

void spudgpu___internal_record_build(const spudgpu_acceleration_structure_build *build) {
	const spudgpu_acceleration_structure_inputs *inputs = build->inputs;
	spudgpu___acceleration_structure_state *destination  = spudgpu___as_state(build->destination);
	destination->built                                  = true;
	destination->build_flags                            = inputs->flags;
	destination->instance_count = inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? inputs->instance_count : 0;
	// Room was made by spudgpu___internal_reserve_build_layout: no resize
	// below allocates.
	eq_array_clear(&destination->geometries);
	if (spudgpu___rq_keeps_layout(inputs->level, inputs->flags)) {
		(void)eq_array_resize(&destination->geometries, inputs->geometry_count);
		for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
			const spudgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
			spudgpu___geometry_layout *kept                  = &destination->geometries.data[i];
			kept->kind                                       = g->kind;
			kept->flags                                      = g->flags;
			kept->data_count                                 = g->data_count;
			kept->index_format                               = g->index_format;
			kept->index_count                                = g->index_count;
		}
	}
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		// Checked against max_bottom_levels: the list made with the
		// structure holds it.
		if (inputs->bottom_level_count > 0)
			memcpy(destination->bottom_levels, inputs->bottom_levels, sizeof(spudgpu_acceleration_structure) * inputs->bottom_level_count);
		destination->bottom_level_count = inputs->bottom_level_count;
	}
}

SPUDRESULT spudgpu___internal_check_copy(
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	static const char function[] = "spudgpu_cmd_copy_acceleration_structure";
	if (spudgpu___rq_invalid(source, function, "source"))
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const spudgpu___acceleration_structure_state *from = spudgpu___as_state(source);
	if (!from->built)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (spudgpu___rq_invalid(destination, function, "destination"))
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const spudgpu___acceleration_structure_state *to = spudgpu___as_state(destination);
	if (destination == source || to->level != from->level)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (mode != SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE && mode != SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT)
		return SPUDRESULT_DESC_INVALID_PARAMETERS;
	if (mode == SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT && !(from->build_flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION))
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED;
	if (mode == SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE && to->size < from->size)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL;
	if (from->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP && from->bottom_level_count > to->max_bottom_levels)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	// Room for the source's layout, the last thing that can fail.
	spudgpu___acceleration_structure_state *grown = spudgpu___as_state(destination);
	if (eq_array_reserve(&grown->geometries, from->geometries.count) != EQ_SUCCESS)
		return SPUDRESULT_OUT_OF_MEMORY;
	return SPUD_SUCCESS;
}

void spudgpu___internal_record_copy(spudgpu_acceleration_structure source, spudgpu_acceleration_structure destination) {
	const spudgpu___acceleration_structure_state *from = spudgpu___as_state(source);
	spudgpu___acceleration_structure_state *to         = spudgpu___as_state(destination);
	to->built                                          = true;
	to->build_flags                                    = from->build_flags;
	to->instance_count                                 = from->instance_count;
	// Room was made by spudgpu___internal_check_copy.
	eq_array_clear(&to->geometries);
	if (from->geometries.count > 0)
		(void)eq_array_append(&to->geometries, from->geometries.data, from->geometries.count);
	if (from->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		if (from->bottom_level_count > 0)
			memcpy(to->bottom_levels, from->bottom_levels, sizeof(spudgpu_acceleration_structure) * from->bottom_level_count);
		to->bottom_level_count = from->bottom_level_count;
	}
}

SPUDRESULT spudgpu___internal_check_compacted_sizes(
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset) {
	if (!structures)
		return SPUDRESULT_NULL_DESC;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	for (uint32_t i = 0; i < count; ++i) {
		if (spudgpu___rq_invalid(structures[i], "spudgpu_cmd_write_acceleration_structure_compacted_sizes", "structures[i]"))
			return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		const spudgpu___acceleration_structure_state *state = spudgpu___as_state(structures[i]);
		if (!state->allow_compaction || !state->built || !(state->build_flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION))
			return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED;
	}
	spudgpu_buffer_desc desc;
	const SPUDRESULT buffer_sr = spudgpu___rq_buffer_desc(buffer, &desc);
	if (buffer_sr != SPUD_SUCCESS)
		return buffer_sr;
	const SPUDGPU_BUFFER_USAGE needed = SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	if ((desc.usage & needed) != needed)
		return SPUDRESULT_GPU_INVALID_BUFFER_USAGE;
	if (offset % 8 != 0)
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	if (!spudgpu___rq_fits(desc.size, offset, count, 8, 8))
		return SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
	return SPUD_SUCCESS;
}

bool spudgpu___internal_check_acceleration_structure_barriers(
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (count == 0 || !barriers)
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		const spudgpu_acceleration_structure_barrier *b = &barriers[i];
		if (spud_struct_check(&b->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER) != SPUD_SUCCESS)
			return false;
		if (spudgpu___rq_invalid(b->structure, "spudgpu_cmd_acceleration_structure_barrier", "barriers[i].structure"))
			return false;
		if (b->access_before > SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE || b->access_after > SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE)
			return false;
	}
	return true;
}

SPUDRESULT spudgpu___internal_write_instances(
    spudgpu_acceleration_structure_instance_layout layout,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!instances)
		return SPUDRESULT_NULL_DESC;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	if (!out_bytes)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	for (uint32_t i = 0; i < count; ++i) {
		const spudgpu_acceleration_structure_instance *instance = &instances[i];
		if (instance->user_index >= SPUDGPU___RQ_INDEX_LIMIT || instance->table_offset >= SPUDGPU___RQ_INDEX_LIMIT)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		if (instance->flags & ~(SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS)SPUDGPU___RQ_KNOWN_INSTANCE_FLAGS)
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
		if ((instance->flags & SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE) &&
		    (instance->flags & SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE))
			return SPUDRESULT_DESC_INVALID_PARAMETERS;
	}

	// One pass over the whole array. The flags' bits are the same in all
	// three APIs (Vulkan's VkGeometryInstanceFlagBitsKHR, D3D12's
	// D3D12_RAYTRACING_INSTANCE_FLAGS, Metal's
	// MTLAccelerationStructureInstanceOptions), so they are written as they
	// are. Bytes are copied, not stored through a cast, so out_bytes may be
	// any memory.
	uint8_t *out = (uint8_t *)out_bytes;
	for (uint32_t i = 0; i < count; ++i, out += layout.size) {
		const spudgpu_acceleration_structure_instance *instance = &instances[i];
		switch (layout.kind) {
		case SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64: {
			// VkAccelerationStructureInstanceKHR / D3D12_RAYTRACING_INSTANCE_DESC:
			// the 3x4 rows as they are, then two words whose low 24 bits are
			// the index (or table offset) and high 8 the mask (or flags).
			const uint32_t index_and_mask  = instance->user_index | ((uint32_t)instance->mask << 24);
			const uint32_t offset_and_flags = instance->table_offset | ((uint32_t)instance->flags << 24);
			memcpy(out, instance->transform, 48);
			memcpy(out + 48, &index_and_mask, 4);
			memcpy(out + 52, &offset_and_flags, 4);
			memcpy(out + 56, &instance->bottom_level_reference, 8);
			break;
		}
		case SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72: {
			// MTLIndirectAccelerationStructureInstanceDescriptor: a packed
			// 4x3, four columns of three floats, then options, mask, the
			// intersection function table offset, the user ID and the
			// resource ID.
			float columns[12];
			for (uint32_t c = 0; c < 4; ++c)
				for (uint32_t r = 0; r < 3; ++r)
					columns[c * 3 + r] = instance->transform[r * 4 + c];
			const uint32_t words[4] = {instance->flags, instance->mask, instance->table_offset, instance->user_index};
			memcpy(out, columns, 48);
			memcpy(out + 48, words, 16);
			memcpy(out + 64, &instance->bottom_level_reference, 8);
			break;
		}
		default:
			break;
		}
	}
	return SPUD_SUCCESS;
}

bool spudgpu___internal_check_acceleration_structure_writes(
    SPUDGPU_FEATURES features,
    const spudgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	for (uint32_t i = 0; i < write_count; ++i) {
		const spudgpu_write_descriptor_set *write = &writes[i];
		if (write->descriptor_type != SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE)
			continue;
		if (!(features & SPUDGPU_FEATURE_RAY_QUERY))
			return false;
		if (!write->acceleration_structures)
			return false;
		for (uint32_t k = 0; k < write->descriptor_count; ++k) {
			const spudgpu_acceleration_structure structure = write->acceleration_structures[k];
			if (spudgpu___rq_invalid(structure, "spudgpu_update_descriptor_sets", "writes[i].acceleration_structures[k]"))
				return false;
			if (spudgpu___as_state(structure)->level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
				return false;
		}
	}
	return true;
}

#endif // SPUDGPU_EXT_RAY_QUERY

// ---------------------------------------------------------------------------
// Live objects (debug builds)
// ---------------------------------------------------------------------------

#if _DEBUG
static const char *spudgpu___object_kind_name(SPUDGPU___OBJECT_KIND kind) {
	switch (kind) {
	case SPUDGPU___OBJECT_BUFFER:
		return "buffer";
	case SPUDGPU___OBJECT_BUFFER_VIEW:
		return "buffer view";
	case SPUDGPU___OBJECT_IMAGE:
		return "image";
	case SPUDGPU___OBJECT_IMAGE_VIEW:
		return "image view";
	case SPUDGPU___OBJECT_SAMPLER:
		return "sampler";
	case SPUDGPU___OBJECT_HEAP:
		return "heap";
	case SPUDGPU___OBJECT_SHADER_MODULE:
		return "shader module";
	case SPUDGPU___OBJECT_SHADER_PIPELINE:
		return "shader pipeline";
	case SPUDGPU___OBJECT_COMPUTE_PIPELINE:
		return "compute pipeline";
	case SPUDGPU___OBJECT_DESCRIPTOR_SET_LAYOUT:
		return "descriptor set layout";
	case SPUDGPU___OBJECT_DESCRIPTOR_POOL:
		return "descriptor pool";
	case SPUDGPU___OBJECT_COMMAND_ALLOCATOR:
		return "command allocator";
	case SPUDGPU___OBJECT_COMMAND_LIST:
		return "command list";
	case SPUDGPU___OBJECT_FENCE:
		return "fence";
	case SPUDGPU___OBJECT_SEMAPHORE:
		return "semaphore";
	case SPUDGPU___OBJECT_SWAP_CHAIN:
		return "swap chain";
	case SPUDGPU___OBJECT_TIMESTAMP_SET:
		return "timestamp set";
	case SPUDGPU___OBJECT_ACCELERATION_STRUCTURE:
		return "acceleration structure";
	default:
		return "object";
	}
}

bool spudgpu___internal_live_init(spudgpu___live_objects *live) {
	memset(live->counts, 0, sizeof(live->counts));
	return spud_mutex_init(&live->lock);
}

void spudgpu___internal_live_destroy(spudgpu___live_objects *live) {
	spud_mutex_destroy(&live->lock);
}

void spudgpu___internal_live_add(spudgpu___live_objects *live, SPUDGPU___OBJECT_KIND kind) {
	spud_mutex_lock(&live->lock);
	++live->counts[kind];
	spud_mutex_unlock(&live->lock);
}

void spudgpu___internal_live_remove(spudgpu___live_objects *live, SPUDGPU___OBJECT_KIND kind) {
	spud_mutex_lock(&live->lock);
	assert(live->counts[kind] > 0 && "spudgpu: an object was destroyed that its device never counted");
	if (live->counts[kind] > 0)
		--live->counts[kind];
	spud_mutex_unlock(&live->lock);
}

void spudgpu___internal_live_check_none(spudgpu___live_objects *live, const char *device_name) {
	spud_mutex_lock(&live->lock);
	uint64_t total = 0;
	for (int kind = 0; kind < SPUDGPU___OBJECT_KIND_COUNT; ++kind)
		total += live->counts[kind];
	if (total > 0) {
		printf("spudgpu: device \"%s\" destroyed with", device_name ? device_name : "");
		bool first = true;
		for (int kind = 0; kind < SPUDGPU___OBJECT_KIND_COUNT; ++kind) {
			if (live->counts[kind] == 0)
				continue;
			printf("%s %llu %s%s", first ? "" : ",", (unsigned long long)live->counts[kind],
			    spudgpu___object_kind_name((SPUDGPU___OBJECT_KIND)kind), live->counts[kind] == 1 ? "" : "s");
			first = false;
		}
		printf(" alive\n");
	}
	spud_mutex_unlock(&live->lock);
	assert(total == 0 && "spudgpu: a device was destroyed with objects made from it still alive");
}
#endif

#if __cplusplus
}
#endif
