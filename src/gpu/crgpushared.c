#include "crgpu.h"
#include "crgpushared.h"
#include "../crallocator.h"
#include "../crstructbase.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

CR_RESULT crgpu___internal_check_instance_desc(
    const crgpu_instance_desc *desc,
    crgpu_instance *out_instance) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_INSTANCE_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_instance)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_instance = NULL;
	return cr_allocator_check(desc->allocator);
}

bool crgpu___internal_blend_attachments_equal(
    const crgpu_blend_attachment_desc *a,
    const crgpu_blend_attachment_desc *b) {
	if (a->blend_enable != b->blend_enable)
		return false;
	if (!a->blend_enable)
		return true;
	return a->src_color_blend_factor == b->src_color_blend_factor && a->dst_color_blend_factor == b->dst_color_blend_factor &&
	       a->color_blend_op == b->color_blend_op && a->src_alpha_blend_factor == b->src_alpha_blend_factor &&
	       a->dst_alpha_blend_factor == b->dst_alpha_blend_factor && a->alpha_blend_op == b->alpha_blend_op;
}

CRGPU_SHADER_FORMAT crgpu___internal_shader_format(void) {
#if CRGPU_COMPILE_VULKAN_API
	return CRGPU_SHADER_FORMAT_SPIRV;
#elif CRGPU_COMPILE_D3D12_API
	return CRGPU_SHADER_FORMAT_DXIL;
#else
	return CRGPU_SHADER_FORMAT_METALLIB;
#endif
}

CR_RESULT crgpu___internal_check_shader_module_desc(
    const crgpu_shader_module_desc *desc,
    CRGPU_FEATURES features) {
	const CRGPU_SHADER_STAGE stage = desc->stage;
	// One known stage: a single bit of the enum.
	if (stage == CRGPU_SHADER_STAGE_NONE || (stage & (stage - 1)) != 0 || stage > CRGPU_SHADER_STAGE_TASK)
		return CR_RESULT_GPU_INVALID_SHADER_STAGE;
	if (desc->format != crgpu___internal_shader_format())
		return CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED;
	if (!desc->code || desc->code_size == 0)
		return CR_RESULT_GPU_NULL_SHADER_CODE;
	if ((uintptr_t)desc->code % 4 != 0)
		return CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT;
	if (desc->format != CRGPU_SHADER_FORMAT_METALLIB && desc->code_size % 4 != 0)
		return CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT;
	if (!desc->entry_point || !desc->entry_point[0])
		return CR_RESULT_GPU_NULL_ENTRY_POINT;
	const bool has_workgroup =
	    stage == CRGPU_SHADER_STAGE_COMPUTE || stage == CRGPU_SHADER_STAGE_MESH || stage == CRGPU_SHADER_STAGE_TASK;
	for (uint32_t i = 0; i < 3; ++i) {
		if (has_workgroup && desc->workgroup_size[i] == 0)
			return CR_RESULT_GPU_INVALID_WORKGROUP_SIZE;
		if (!has_workgroup && desc->workgroup_size[i] != 0)
			return CR_RESULT_GPU_INVALID_WORKGROUP_SIZE;
	}
	// A mesh or task module needs the feature even as a module: its SPIR-V
	// declares MeshShadingEXT, which Vulkan refuses without it, and the rule
	// holds on every backend.
	if ((stage == CRGPU_SHADER_STAGE_MESH || stage == CRGPU_SHADER_STAGE_TASK) &&
	    !(features & CRGPU_FEATURE_MESH_SHADING))
		return CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	// Geometry and tessellation SPIR-V declares capabilities Vulkan refuses
	// without the features; the rule holds on every backend, and on Metal,
	// whose GPUs never have the bits.
	if (stage == CRGPU_SHADER_STAGE_GEOMETRY && !(features & CRGPU_FEATURE_GEOMETRY_SHADER))
		return CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED;
	if ((stage == CRGPU_SHADER_STAGE_TESSELLATION_CONTROL || stage == CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION) &&
	    !(features & CRGPU_FEATURE_TESSELLATION))
		return CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED;
	return CR_SUCCESS;
}

CR_RESULT crgpu___internal_check_pipeline_outputs(
    const crgpu_graphics_pipeline_desc *desc,
    CRGPU_FEATURES features) {
	if (desc->geometry_module && !(features & CRGPU_FEATURE_GEOMETRY_SHADER))
		return CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED;
	if ((desc->tess_control_module || desc->tess_eval_module) && !(features & CRGPU_FEATURE_TESSELLATION))
		return CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED;
	if (desc->color_attachment_count > CRGPU_MAX_COLOR_ATTACHMENTS)
		return CR_RESULT_GPU_TOO_MANY_COLOR_ATTACHMENTS;
	if (desc->depth_bias_clamp != 0.0f && !(features & CRGPU_FEATURE_DEPTH_BIAS_CLAMP))
		return CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED;
	if (!(features & CRGPU_FEATURE_INDEPENDENT_BLEND))
		for (uint32_t i = 1; i < desc->color_attachment_count; ++i)
			if (!crgpu___internal_blend_attachments_equal(&desc->blend_attachments[0], &desc->blend_attachments[i]))
				return CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED;
	return CR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Adapters and devices
// ---------------------------------------------------------------------------

CR_RESULT crgpu___internal_get_adapter_properties(
    const crgpu___adapter_facts *facts,
    CRGPU_ADAPTER_PROPERTIES *out_properties) {
	if (!out_properties)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (out_properties->base.type != CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES)
		return CR_RESULT_STRUCT_WRONG_TYPE;

	// The whole chain is checked before anything is written: a call that
	// fails leaves every struct of it as it was.
	bool seen_mesh      = false;
	bool seen_bindless  = false;
	bool seen_ray_query = false;
	for (const CR_STRUCT_BASE_OUT *link = out_properties->base.next; link; link = link->next) {
		switch (link->type) {
#if CRGPU_EXT_MESH_SHADING
		case CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS:
			if (seen_mesh)
				return CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_mesh = true;
			break;
#endif
#if CRGPU_EXT_BINDLESS
		case CR_STRUCT_TYPE_GPU_BINDLESS_LIMITS:
			if (seen_bindless)
				return CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_bindless = true;
			break;
#endif
		// Declared on every backend, as its feature bit is.
		case CR_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS:
			if (seen_ray_query)
				return CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN;
			seen_ray_query = true;
			break;
		default:
			return CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
		}
	}
	(void)seen_mesh;
	(void)seen_bindless;

	// Every field but the base is Carrottus GPU's to write; the base, chain
	// included, stays the caller's.
	const CR_STRUCT_BASE_OUT base = out_properties->base;
	*out_properties                 = facts->properties;
	out_properties->base            = base;
	out_properties->shader_formats  = 1u << crgpu___internal_shader_format();

	for (CR_STRUCT_BASE_OUT *link = out_properties->base.next; link; link = link->next) {
#if CRGPU_EXT_MESH_SHADING
		if (link->type == CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS) {
			CRGPU_MESH_SHADING_LIMITS *mesh   = (CRGPU_MESH_SHADING_LIMITS *)link;
			mesh->max_mesh_output_vertices       = facts->mesh_max_output_vertices;
			mesh->max_mesh_output_primitives     = facts->mesh_max_output_primitives;
			mesh->max_mesh_workgroup_invocations = facts->mesh_max_workgroup_invocations;
		}
#endif
#if CRGPU_EXT_BINDLESS
		if (link->type == CR_STRUCT_TYPE_GPU_BINDLESS_LIMITS) {
			CRGPU_BINDLESS_LIMITS *bindless = (CRGPU_BINDLESS_LIMITS *)link;
			bindless->max_sampled_images      = facts->bindless_max_sampled_images;
			bindless->max_storage_images      = facts->bindless_max_storage_images;
			bindless->max_storage_buffers     = facts->bindless_max_storage_buffers;
		}
#endif
		if (link->type == CR_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS) {
			CRGPU_RAY_QUERY_LIMITS *ray_query  = (CRGPU_RAY_QUERY_LIMITS *)link;
			ray_query->max_geometries           = facts->ray_query_max_geometries;
			ray_query->max_instances            = facts->ray_query_max_instances;
			ray_query->max_primitives           = facts->ray_query_max_primitives;
			ray_query->scratch_offset_alignment = facts->ray_query_scratch_offset_alignment;
			ray_query->placement_alignment      = facts->ray_query_placement_alignment;
		}
	}
	return CR_SUCCESS;
}

CR_RESULT crgpu___internal_check_swap_chain_desc(
    CRGPU_FEATURES features,
    const crgpu_swap_chain_desc *desc,
    const crgpu_swap_chain *out_swap_chain) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_swap_chain)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->surface)
		return CR_RESULT_GPU_INVALID_SURFACE;
	if (!desc->queue)
		return CR_RESULT_GPU_INVALID_COMMAND_QUEUE;
	if (!(features & CRGPU_FEATURE_PRESENT))
		return CR_RESULT_GPU_FEATURE_NOT_SUPPORTED;
	switch (desc->present_scaling) {
	case CRGPU_PRESENT_SCALING_ONE_TO_ONE:
	case CRGPU_PRESENT_SCALING_STRETCH:
	case CRGPU_PRESENT_SCALING_ASPECT_FIT:
		return CR_SUCCESS;
	default:
		return CR_RESULT_GPU_INVALID_PRESENT_SCALING;
	}
}

CR_RESULT crgpu___internal_check_surface_present_scalings(
    CRGPU_FEATURES features,
    const void *surface,
    const uint32_t *out_scalings) {
	if (!surface)
		return CR_RESULT_GPU_INVALID_SURFACE;
	// The surface's struct is the backend's; its head is first in every one.
	if (CR_STALE_HEAD((const cr_object_head *)surface, "crgpu_surface_get_present_scalings", "surface"))
		return CR_RESULT_GPU_INVALID_SURFACE;
	if (!out_scalings)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!(features & CRGPU_FEATURE_PRESENT))
		return CR_RESULT_GPU_FEATURE_NOT_SUPPORTED;
	return CR_SUCCESS;
}

uint32_t crgpu___internal_queue_count(
    const crgpu_device_desc *desc,
    CRGPU_COMMAND_LIST_TYPE type) {
	switch (type) {
	case CRGPU_COMMAND_LIST_TYPE_DIRECT:
		return desc->direct_queue_count;
	case CRGPU_COMMAND_LIST_TYPE_COMPUTE:
		return desc->compute_queue_count;
	case CRGPU_COMMAND_LIST_TYPE_COPY:
		return desc->copy_queue_count;
	default:
		return 0;
	}
}

CR_RESULT crgpu___internal_check_device_desc(
    const crgpu___adapter_facts *facts,
    const crgpu_device_desc *desc,
    crgpu_device *out_device) {
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_result = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_DEVICE_DESC);
	if (base_result != CR_SUCCESS)
		return base_result;
	if (!out_device)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_device = NULL;

	const CRGPU_ADAPTER_PROPERTIES *properties = &facts->properties;
	if (desc->required_features & ~properties->supported_features)
		return CR_RESULT_GPU_FEATURE_NOT_SUPPORTED;
	if (desc->direct_queue_count == 0 && desc->compute_queue_count == 0 && desc->copy_queue_count == 0)
		return CR_RESULT_ZERO_SIZE;
	for (uint32_t i = 0; i < properties->queue_pool_count; ++i) {
		const CRGPU_QUEUE_POOL *pool = &properties->queue_pools[i];
		// 64 bits: three counts near UINT32_MAX don't wrap into a small sum.
		uint64_t asked = 0;
		if (pool->kinds & CRGPU_QUEUE_KIND_DIRECT)
			asked += desc->direct_queue_count;
		if (pool->kinds & CRGPU_QUEUE_KIND_COMPUTE)
			asked += desc->compute_queue_count;
		if (pool->kinds & CRGPU_QUEUE_KIND_COPY)
			asked += desc->copy_queue_count;
		if (asked > pool->max_queues)
			return CR_RESULT_GPU_TOO_MANY_QUEUES;
	}
	if (desc->max_descriptors > properties->max_descriptors)
		return CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE;
	if (desc->max_samplers > properties->max_samplers)
		return CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE;
	if (desc->required_features & CRGPU_FEATURE_BINDLESS) {
		const uint64_t table = (uint64_t)facts->bindless_max_sampled_images + facts->bindless_max_storage_images +
		                       facts->bindless_max_storage_buffers;
		if (table > desc->max_descriptors)
			return CR_RESULT_GPU_OUT_OF_DESCRIPTORS;
	}
	return CR_SUCCESS;
}

void crgpu___internal_pool_descriptor_counts(
    const crgpu_descriptor_pool_desc *desc,
    uint64_t *out_descriptors,
    uint64_t *out_samplers) {
	uint64_t descriptors = 0;
	uint64_t samplers    = 0;
	for (uint32_t i = 0; i < desc->pool_size_count && i < CRGPU_MAX_DESCRIPTOR_POOL_SIZES; ++i) {
		const crgpu_descriptor_pool_size *size = &desc->pool_sizes[i];
		if (size->descriptor_type == CRGPU_DESCRIPTOR_TYPE_SAMPLER) {
			samplers += size->count;
		} else {
			descriptors += size->count;
			// The image's descriptor and the sampler's, which D3D12 keeps in
			// two heaps and every backend counts the same way.
			if (size->descriptor_type == CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				samplers += size->count;
		}
	}
	*out_descriptors = descriptors;
	*out_samplers    = samplers;
}

bool crgpu___internal_budget_init(
    crgpu___descriptor_budget *budget,
    uint32_t max_descriptors,
    uint32_t max_samplers) {
	if (!cr_mutex_init(&budget->lock))
		return false;
	budget->max_descriptors  = max_descriptors;
	budget->max_samplers     = max_samplers;
	budget->used_descriptors = 0;
	budget->used_samplers    = 0;
	return true;
}

void crgpu___internal_budget_destroy(crgpu___descriptor_budget *budget) {
	cr_mutex_destroy(&budget->lock);
}

CR_RESULT crgpu___internal_budget_take(
    crgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers) {
	CR_RESULT result = CR_SUCCESS;
	cr_mutex_lock(&budget->lock);
	if (descriptors > budget->max_descriptors - budget->used_descriptors ||
	    samplers > budget->max_samplers - budget->used_samplers) {
		result = CR_RESULT_GPU_OUT_OF_DESCRIPTORS;
	} else {
		budget->used_descriptors += descriptors;
		budget->used_samplers += samplers;
	}
	cr_mutex_unlock(&budget->lock);
	return result;
}

void crgpu___internal_budget_give_back(
    crgpu___descriptor_budget *budget,
    uint64_t descriptors,
    uint64_t samplers) {
	cr_mutex_lock(&budget->lock);
	assert(budget->used_descriptors >= descriptors);
	assert(budget->used_samplers >= samplers);
	budget->used_descriptors -= descriptors;
	budget->used_samplers -= samplers;
	cr_mutex_unlock(&budget->lock);
}

// ---------------------------------------------------------------------------
// Descriptor types
// ---------------------------------------------------------------------------

CR_RESULT crgpu___internal_check_descriptor_type(CRGPU_DESCRIPTOR_TYPE type, CRGPU_FEATURES features) {
	switch (type) {
	case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case CRGPU_DESCRIPTOR_TYPE_SAMPLER:
	case CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return CR_SUCCESS;
	case CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
		return (features & CRGPU_FEATURE_RAY_QUERY) ? CR_SUCCESS : CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	default:
		return CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE;
	}
}

// ---------------------------------------------------------------------------
// Acceleration structures
// ---------------------------------------------------------------------------

#if CRGPU_EXT_RAY_QUERY

// Instances are written by the CPU in little-endian words, as every target
// Carrottus GPU builds for is.
#define CRGPU___RQ_INDEX_LIMIT (1u << 24)
#define CRGPU___RQ_KNOWN_BUILD_FLAGS                                                                                         \
	(CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD |           \
	 CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE | CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION)
#define CRGPU___RQ_KNOWN_GEOMETRY_FLAGS \
	(CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE | CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES)
#define CRGPU___RQ_KNOWN_INSTANCE_FLAGS                                                                                      \
	(CRGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING |                                                        \
	 CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FRONT_COUNTERCLOCKWISE | CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE |    \
	 CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE)

crgpu_acceleration_structure_instance_layout crgpu___internal_instance_layout(void) {
	crgpu_acceleration_structure_instance_layout layout;
#if CRGPU_COMPILE_METAL_API
	// MTLIndirectAccelerationStructureInstanceDescriptor: 72 bytes, its
	// 64-bit resource ID last.
	layout.kind      = CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72;
	layout.size      = 72;
	layout.alignment = 8;
#else
	// VkAccelerationStructureInstanceKHR and D3D12_RAYTRACING_INSTANCE_DESC:
	// the same 64 bytes, an array of them 16-byte aligned on both.
	layout.kind      = CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64;
	layout.size      = 64;
	layout.alignment = 16;
#endif
	return layout;
}

// Whether [count] elements of [element] bytes, [stride] apart from
// [offset], fit in [size] bytes, with no arithmetic that can wrap.
static bool crgpu___rq_fits(uint64_t size, uint64_t offset, uint64_t count, uint64_t stride, uint64_t element) {
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
// in Debug: CR_RESULT_GPU_INVALID_BUFFER for NULL or destroyed.
static CR_RESULT crgpu___rq_buffer_desc(crgpu_buffer buffer, crgpu_buffer_desc *out_desc) {
	if (!buffer)
		return CR_RESULT_GPU_INVALID_BUFFER;
	return crgpu_buffer_get_desc(buffer, out_desc);
}

// [structure] is NULL, or destroyed (Debug).
static bool crgpu___rq_invalid(crgpu_acceleration_structure structure, const char *function, const char *param) {
	if (!structure)
		return true;
	return CR_STALE(structure, function, param);
}

CR_RESULT crgpu___internal_check_acceleration_structure_desc(const crgpu_acceleration_structure_desc *desc) {
	if (desc->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && desc->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && desc->max_bottom_levels != 0)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (desc->size == 0)
		return CR_RESULT_ZERO_SIZE;
	return CR_SUCCESS;
}

bool crgpu___internal_acceleration_structure_state_init(
    crgpu___acceleration_structure_state *state,
    const CR_ALLOCATOR *allocator,
    const crgpu_acceleration_structure_desc *desc) {
	memset(state, 0, sizeof(*state));
	state->size              = desc->size;
	state->level             = desc->level;
	state->max_bottom_levels = desc->max_bottom_levels;
	state->allow_compaction  = desc->allow_compaction;
	state->eq_allocator      = cr_eq_allocator(allocator);
	eq_array_init(&state->geometries, &state->eq_allocator);
	if (desc->max_bottom_levels > 0) {
		state->bottom_levels = CR_NEW_ARRAY(allocator, crgpu_acceleration_structure, desc->max_bottom_levels);
		if (!state->bottom_levels)
			return false;
	}
	return true;
}

void crgpu___internal_acceleration_structure_state_free(
    crgpu___acceleration_structure_state *state,
    const CR_ALLOCATOR *allocator) {
	if (state->bottom_levels)
		CR_DELETE_ARRAY(allocator, crgpu_acceleration_structure, state->bottom_levels, state->max_bottom_levels);
	state->bottom_levels = NULL;
	eq_array_free(&state->geometries);
}

// A bottom level's geometries, in the header's order.
static CR_RESULT crgpu___rq_check_bottom(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_inputs *inputs) {
	if (inputs->instances || inputs->instance_offset || inputs->instance_count || inputs->bottom_levels ||
	    inputs->bottom_level_count)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!inputs->geometries)
		return CR_RESULT_NULL_DESC;
	if (inputs->geometry_count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (inputs->geometry_count > facts->ray_query_max_geometries)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	const CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_KIND first_kind = inputs->geometries[0].kind;
	uint64_t primitives                                          = 0;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const crgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
		if (g->kind != CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES && g->kind != CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		if (g->kind != first_kind)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		if (g->flags & ~(CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS)CRGPU___RQ_KNOWN_GEOMETRY_FLAGS)
			return CR_RESULT_DESC_INVALID_PARAMETERS;

		crgpu_buffer_desc data;
		const CR_RESULT data_sr = crgpu___rq_buffer_desc(g->data, &data);
		if (data_sr != CR_SUCCESS)
			return data_sr;
		if (!(data.usage & CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
			return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
		const bool triangles   = g->kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
		const uint64_t align   = triangles ? 4 : 8;
		const uint64_t element = triangles ? 12 : 24;
		if (g->data_offset % align != 0 || g->data_stride % align != 0 || g->data_stride < element || g->data_stride > UINT32_MAX)
			return CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
		if (g->data_count == 0)
			return CR_RESULT_ZERO_SIZE;
		if (!crgpu___rq_fits(data.size, g->data_offset, g->data_count, g->data_stride, element))
			return CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE;

		const bool known_format = g->index_format == CRGPU_FORMAT_UNKNOWN ||
		                          (triangles && (g->index_format == CRGPU_FORMAT_R16_UINT || g->index_format == CRGPU_FORMAT_R32_UINT));
		if (!known_format)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		const bool indexed = g->index_format != CRGPU_FORMAT_UNKNOWN;
		if (!indexed && g->indices)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		uint64_t count = 0;
		if (indexed) {
			crgpu_buffer_desc index_desc;
			const CR_RESULT index_sr = crgpu___rq_buffer_desc(g->indices, &index_desc);
			if (index_sr != CR_SUCCESS)
				return index_sr;
			if (!(index_desc.usage & CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
				return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
			const uint64_t index_size = g->index_format == CRGPU_FORMAT_R16_UINT ? 2 : 4;
			if (g->index_offset % index_size != 0)
				return CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
			if (g->index_count == 0)
				return CR_RESULT_ZERO_SIZE;
			if (g->index_count % 3 != 0)
				return CR_RESULT_DESC_INVALID_PARAMETERS;
			if (!crgpu___rq_fits(index_desc.size, g->index_offset, g->index_count, index_size, index_size))
				return CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
			count = g->index_count / 3;
		} else {
			if (g->index_offset != 0 || g->index_count != 0)
				return CR_RESULT_DESC_INVALID_PARAMETERS;
			if (triangles && g->data_count % 3 != 0)
				return CR_RESULT_DESC_INVALID_PARAMETERS;
			count = triangles ? g->data_count / 3 : g->data_count;
		}
		primitives += count;
		if (primitives > facts->ray_query_max_primitives)
			return CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	}
	return CR_SUCCESS;
}

// A top level's instances and bottom levels, in the header's order.
static CR_RESULT crgpu___rq_check_top(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_inputs *inputs,
    const char *function) {
	if (inputs->geometries || inputs->geometry_count)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	crgpu_buffer_desc instances;
	const CR_RESULT instances_sr = crgpu___rq_buffer_desc(inputs->instances, &instances);
	if (instances_sr != CR_SUCCESS)
		return instances_sr;
	if (!(instances.usage & CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT))
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	const crgpu_acceleration_structure_instance_layout layout = crgpu___internal_instance_layout();
	if (inputs->instance_offset % layout.alignment != 0)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	if (inputs->instance_count > facts->ray_query_max_instances)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	if (!crgpu___rq_fits(instances.size, inputs->instance_offset, inputs->instance_count, layout.size, layout.size))
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
	if (inputs->bottom_level_count > 0 && !inputs->bottom_levels)
		return CR_RESULT_NULL_DESC;
	for (uint32_t i = 0; i < inputs->bottom_level_count; ++i) {
		const crgpu_acceleration_structure bottom = inputs->bottom_levels[i];
		if (crgpu___rq_invalid(bottom, function, "inputs->bottom_levels[i]"))
			return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		if (crgpu___as_state(bottom)->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
			return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	}
	return CR_SUCCESS;
}

// The inputs' checks after their base, for [function]'s stale message.
static CR_RESULT crgpu___rq_check_inputs(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_inputs *inputs,
    const char *function) {
	if (inputs->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && inputs->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (inputs->flags & ~(CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS)CRGPU___RQ_KNOWN_BUILD_FLAGS)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if ((inputs->flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE) &&
	    (inputs->flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return crgpu___rq_check_bottom(facts, inputs);
	return crgpu___rq_check_top(facts, inputs, function);
}

CR_RESULT crgpu___internal_check_acceleration_structure_inputs(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_inputs *inputs) {
	return crgpu___rq_check_inputs(facts, inputs, "crgpu_device_get_acceleration_structure_build_sizes");
}

CR_RESULT crgpu___internal_check_acceleration_structure_list(CRGPU_COMMAND_LIST_TYPE type, bool rendering) {
	// Direct and compute queues build on every API; a copy queue builds on
	// none of the three's portable subset (Metal has no queue kinds, D3D12
	// and Vulkan no builds there), and a bundle records no build.
	if (type != CRGPU_COMMAND_LIST_TYPE_DIRECT && type != CRGPU_COMMAND_LIST_TYPE_COMPUTE)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE;
	if (rendering)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING;
	return CR_SUCCESS;
}

// Whether [state]'s last build has exactly [inputs]' layout: what an update
// must keep, compared value by value.
static bool crgpu___rq_same_layout(const crgpu___acceleration_structure_state *state, const crgpu_acceleration_structure_inputs *inputs) {
	if (state->build_flags != inputs->flags)
		return false;
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
		return state->instance_count == inputs->instance_count;
	if (state->geometries.count != inputs->geometry_count)
		return false;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const crgpu___geometry_layout *kept           = &state->geometries.data[i];
		const crgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
		if (kept->kind != g->kind || kept->flags != g->flags || kept->data_count != g->data_count || kept->index_format != g->index_format ||
		    kept->index_count != g->index_count)
			return false;
	}
	return true;
}

// Whether a structure's layout is kept: a bottom level whose build allows
// updates (only an update compares it).
static bool crgpu___rq_keeps_layout(CRGPU_ACCELERATION_STRUCTURE_LEVEL level, CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	return level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM && (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);
}

CR_RESULT crgpu___internal_reserve_build_layout(
    crgpu_acceleration_structure destination,
    const crgpu_acceleration_structure_inputs *inputs) {
	if (!crgpu___rq_keeps_layout(inputs->level, inputs->flags))
		return CR_SUCCESS;
	crgpu___acceleration_structure_state *state = crgpu___as_state(destination);
	return eq_array_reserve(&state->geometries, inputs->geometry_count) == EQ_SUCCESS ? CR_SUCCESS : CR_RESULT_OUT_OF_MEMORY;
}

CR_RESULT crgpu___internal_check_build_head(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_build *build) {
	const CR_RESULT base_sr = cr_struct_check(&build->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (build->mode != CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD && build->mode != CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!build->inputs)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT inputs_base_sr = cr_struct_check(&build->inputs->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != CR_SUCCESS)
		return inputs_base_sr;
	return crgpu___rq_check_inputs(facts, build->inputs, "crgpu_command_list_build_acceleration_structures");
}

CR_RESULT crgpu___internal_check_build_tail(
    const crgpu___adapter_facts *facts,
    const crgpu_acceleration_structure_build *build,
    const crgpu_acceleration_structure_sizes *sizes) {
	static const char function[]                       = "crgpu_command_list_build_acceleration_structures";
	const crgpu_acceleration_structure_inputs *inputs = build->inputs;
	if (build->mode == CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD) {
		if (build->source)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	} else {
		if (crgpu___rq_invalid(build->source, function, "builds[i].source"))
			return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		const crgpu___acceleration_structure_state *source = crgpu___as_state(build->source);
		if (source->level != inputs->level)
			return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		if (!source->built || !(source->build_flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ||
		    !crgpu___rq_same_layout(source, inputs))
			return CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED;
	}
	if (crgpu___rq_invalid(build->destination, function, "builds[i].destination"))
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const crgpu___acceleration_structure_state *destination = crgpu___as_state(build->destination);
	if (destination->level != inputs->level)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (destination->size < sizes->storage_size)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL;
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP && inputs->bottom_level_count > destination->max_bottom_levels)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;

	crgpu_buffer_desc scratch;
	const CR_RESULT scratch_sr = crgpu___rq_buffer_desc(build->scratch, &scratch);
	if (scratch_sr != CR_SUCCESS)
		return scratch_sr;
	if (!(scratch.usage & CRGPU_BUFFER_USAGE_STORAGE))
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	uint64_t address = 0;
	const CR_RESULT address_sr = crgpu_buffer_get_gpu_address(build->scratch, &address);
	if (address_sr != CR_SUCCESS)
		return address_sr;
	// The alignment is a power of two, so the sum's low bits are right
	// even where it wraps.
	if (((address + build->scratch_offset) & (uint64_t)(facts->ray_query_scratch_offset_alignment - 1u)) != 0)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	const uint64_t needed = build->mode == CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD ? sizes->build_scratch_size : sizes->update_scratch_size;
	if (!crgpu___rq_fits(scratch.size, build->scratch_offset, 1, 0, needed))
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL;
	return CR_SUCCESS;
}

void crgpu___internal_record_build(const crgpu_acceleration_structure_build *build) {
	const crgpu_acceleration_structure_inputs *inputs = build->inputs;
	crgpu___acceleration_structure_state *destination  = crgpu___as_state(build->destination);
	destination->built                                  = true;
	destination->build_flags                            = inputs->flags;
	destination->instance_count = inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? inputs->instance_count : 0;
	// Room was made by crgpu___internal_reserve_build_layout: no resize
	// below allocates.
	eq_array_clear(&destination->geometries);
	if (crgpu___rq_keeps_layout(inputs->level, inputs->flags)) {
		(void)eq_array_resize(&destination->geometries, inputs->geometry_count);
		for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
			const crgpu_acceleration_structure_geometry *g = &inputs->geometries[i];
			crgpu___geometry_layout *kept                  = &destination->geometries.data[i];
			kept->kind                                       = g->kind;
			kept->flags                                      = g->flags;
			kept->data_count                                 = g->data_count;
			kept->index_format                               = g->index_format;
			kept->index_count                                = g->index_count;
		}
	}
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		// Checked against max_bottom_levels: the list made with the
		// structure holds it.
		if (inputs->bottom_level_count > 0)
			memcpy(destination->bottom_levels, inputs->bottom_levels, sizeof(crgpu_acceleration_structure) * inputs->bottom_level_count);
		destination->bottom_level_count = inputs->bottom_level_count;
	}
}

CR_RESULT crgpu___internal_check_copy(
    crgpu_acceleration_structure source,
    crgpu_acceleration_structure destination,
    CRGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	static const char function[] = "crgpu_command_list_copy_acceleration_structure";
	if (crgpu___rq_invalid(source, function, "source"))
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const crgpu___acceleration_structure_state *from = crgpu___as_state(source);
	if (!from->built)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (crgpu___rq_invalid(destination, function, "destination"))
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	const crgpu___acceleration_structure_state *to = crgpu___as_state(destination);
	if (destination == source || to->level != from->level)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (mode != CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE && mode != CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT)
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (mode == CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT && !(from->build_flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION))
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED;
	if (mode == CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE && to->size < from->size)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL;
	if (from->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP && from->bottom_level_count > to->max_bottom_levels)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT;
	// Room for the source's layout, the last thing that can fail.
	crgpu___acceleration_structure_state *grown = crgpu___as_state(destination);
	if (eq_array_reserve(&grown->geometries, from->geometries.count) != EQ_SUCCESS)
		return CR_RESULT_OUT_OF_MEMORY;
	return CR_SUCCESS;
}

void crgpu___internal_record_copy(crgpu_acceleration_structure source, crgpu_acceleration_structure destination) {
	const crgpu___acceleration_structure_state *from = crgpu___as_state(source);
	crgpu___acceleration_structure_state *to         = crgpu___as_state(destination);
	to->built                                          = true;
	to->build_flags                                    = from->build_flags;
	to->instance_count                                 = from->instance_count;
	// Room was made by crgpu___internal_check_copy.
	eq_array_clear(&to->geometries);
	if (from->geometries.count > 0)
		(void)eq_array_append(&to->geometries, from->geometries.data, from->geometries.count);
	if (from->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		if (from->bottom_level_count > 0)
			memcpy(to->bottom_levels, from->bottom_levels, sizeof(crgpu_acceleration_structure) * from->bottom_level_count);
		to->bottom_level_count = from->bottom_level_count;
	}
}

CR_RESULT crgpu___internal_check_compacted_sizes(
    const crgpu_acceleration_structure *structures,
    uint32_t count,
    crgpu_buffer buffer,
    uint64_t offset) {
	if (!structures)
		return CR_RESULT_NULL_DESC;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	for (uint32_t i = 0; i < count; ++i) {
		if (crgpu___rq_invalid(structures[i], "crgpu_command_list_write_acceleration_structure_compacted_sizes", "structures[i]"))
			return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
		const crgpu___acceleration_structure_state *state = crgpu___as_state(structures[i]);
		if (!state->allow_compaction || !state->built || !(state->build_flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION))
			return CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED;
	}
	crgpu_buffer_desc desc;
	const CR_RESULT buffer_sr = crgpu___rq_buffer_desc(buffer, &desc);
	if (buffer_sr != CR_SUCCESS)
		return buffer_sr;
	const CRGPU_BUFFER_USAGE needed = CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST;
	if ((desc.usage & needed) != needed)
		return CR_RESULT_GPU_INVALID_BUFFER_USAGE;
	if (offset % 8 != 0)
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT;
	if (!crgpu___rq_fits(desc.size, offset, count, 8, 8))
		return CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE;
	return CR_SUCCESS;
}

bool crgpu___internal_check_acceleration_structure_barriers(
    const crgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (count == 0 || !barriers)
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		const crgpu_acceleration_structure_barrier *b = &barriers[i];
		if (cr_struct_check(&b->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER) != CR_SUCCESS)
			return false;
		if (crgpu___rq_invalid(b->structure, "crgpu_command_list_acceleration_structure_barrier", "barriers[i].structure"))
			return false;
		if (b->access_before > CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE || b->access_after > CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE)
			return false;
	}
	return true;
}

CR_RESULT crgpu___internal_write_instances(
    crgpu_acceleration_structure_instance_layout layout,
    const crgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!instances)
		return CR_RESULT_NULL_DESC;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	if (!out_bytes)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	for (uint32_t i = 0; i < count; ++i) {
		const crgpu_acceleration_structure_instance *instance = &instances[i];
		if (instance->user_index >= CRGPU___RQ_INDEX_LIMIT || instance->table_offset >= CRGPU___RQ_INDEX_LIMIT)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		if (instance->flags & ~(CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FLAGS)CRGPU___RQ_KNOWN_INSTANCE_FLAGS)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		if ((instance->flags & CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE) &&
		    (instance->flags & CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE))
			return CR_RESULT_DESC_INVALID_PARAMETERS;
	}

	// One pass over the whole array. The flags' bits are the same in all
	// three APIs (Vulkan's VkGeometryInstanceFlagBitsKHR, D3D12's
	// D3D12_RAYTRACING_INSTANCE_FLAGS, Metal's
	// MTLAccelerationStructureInstanceOptions), so they are written as they
	// are. Bytes are copied, not stored through a cast, so out_bytes may be
	// any memory.
	uint8_t *out = (uint8_t *)out_bytes;
	for (uint32_t i = 0; i < count; ++i, out += layout.size) {
		const crgpu_acceleration_structure_instance *instance = &instances[i];
		switch (layout.kind) {
		case CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64: {
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
		case CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72: {
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
	return CR_SUCCESS;
}

bool crgpu___internal_check_acceleration_structure_writes(
    CRGPU_FEATURES features,
    const crgpu_write_descriptor_set *writes,
    uint32_t write_count) {
	for (uint32_t i = 0; i < write_count; ++i) {
		const crgpu_write_descriptor_set *write = &writes[i];
		if (write->descriptor_type != CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE)
			continue;
		if (!(features & CRGPU_FEATURE_RAY_QUERY))
			return false;
		if (!write->acceleration_structures)
			return false;
		for (uint32_t k = 0; k < write->descriptor_count; ++k) {
			const crgpu_acceleration_structure structure = write->acceleration_structures[k];
			if (crgpu___rq_invalid(structure, "crgpu_device_update_descriptor_sets", "writes[i].acceleration_structures[k]"))
				return false;
			if (crgpu___as_state(structure)->level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP)
				return false;
		}
	}
	return true;
}

#endif // CRGPU_EXT_RAY_QUERY

// ---------------------------------------------------------------------------
// Live objects (debug builds)
// ---------------------------------------------------------------------------

#if _DEBUG
static const char *crgpu___object_kind_name(CRGPU___OBJECT_KIND kind) {
	switch (kind) {
	case CRGPU___OBJECT_BUFFER:
		return "buffer";
	case CRGPU___OBJECT_BUFFER_VIEW:
		return "buffer view";
	case CRGPU___OBJECT_IMAGE:
		return "image";
	case CRGPU___OBJECT_IMAGE_VIEW:
		return "image view";
	case CRGPU___OBJECT_SAMPLER:
		return "sampler";
	case CRGPU___OBJECT_HEAP:
		return "heap";
	case CRGPU___OBJECT_SHADER_MODULE:
		return "shader module";
	case CRGPU___OBJECT_SHADER_PIPELINE:
		return "shader pipeline";
	case CRGPU___OBJECT_COMPUTE_PIPELINE:
		return "compute pipeline";
	case CRGPU___OBJECT_DESCRIPTOR_SET_LAYOUT:
		return "descriptor set layout";
	case CRGPU___OBJECT_DESCRIPTOR_POOL:
		return "descriptor pool";
	case CRGPU___OBJECT_COMMAND_ALLOCATOR:
		return "command allocator";
	case CRGPU___OBJECT_COMMAND_LIST:
		return "command list";
	case CRGPU___OBJECT_FENCE:
		return "fence";
	case CRGPU___OBJECT_SEMAPHORE:
		return "semaphore";
	case CRGPU___OBJECT_SWAP_CHAIN:
		return "swap chain";
	case CRGPU___OBJECT_TIMESTAMP_SET:
		return "timestamp set";
	case CRGPU___OBJECT_ACCELERATION_STRUCTURE:
		return "acceleration structure";
	default:
		return "object";
	}
}

bool crgpu___internal_live_init(crgpu___live_objects *live) {
	memset(live->counts, 0, sizeof(live->counts));
	return cr_mutex_init(&live->lock);
}

void crgpu___internal_live_destroy(crgpu___live_objects *live) {
	cr_mutex_destroy(&live->lock);
}

void crgpu___internal_live_add(crgpu___live_objects *live, CRGPU___OBJECT_KIND kind) {
	cr_mutex_lock(&live->lock);
	++live->counts[kind];
	cr_mutex_unlock(&live->lock);
}

void crgpu___internal_live_remove(crgpu___live_objects *live, CRGPU___OBJECT_KIND kind) {
	cr_mutex_lock(&live->lock);
	assert(live->counts[kind] > 0 && "crgpu: an object was destroyed that its device never counted");
	if (live->counts[kind] > 0)
		--live->counts[kind];
	cr_mutex_unlock(&live->lock);
}

void crgpu___internal_live_check_none(crgpu___live_objects *live, const char *device_name) {
	cr_mutex_lock(&live->lock);
	uint64_t total = 0;
	for (int kind = 0; kind < CRGPU___OBJECT_KIND_COUNT; ++kind)
		total += live->counts[kind];
	if (total > 0) {
		printf("crgpu: device \"%s\" destroyed with", device_name ? device_name : "");
		bool first = true;
		for (int kind = 0; kind < CRGPU___OBJECT_KIND_COUNT; ++kind) {
			if (live->counts[kind] == 0)
				continue;
			printf("%s %llu %s%s", first ? "" : ",", (unsigned long long)live->counts[kind],
			    crgpu___object_kind_name((CRGPU___OBJECT_KIND)kind), live->counts[kind] == 1 ? "" : "s");
			first = false;
		}
		printf(" alive\n");
	}
	cr_mutex_unlock(&live->lock);
	assert(total == 0 && "crgpu: a device was destroyed with objects made from it still alive");
}
#endif

#if __cplusplus
}
#endif
