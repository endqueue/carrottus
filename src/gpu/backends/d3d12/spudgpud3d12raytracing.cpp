#if SPUDGPU_COMPILE_D3D12_API

// SPUDGPU_EXT_RAY_QUERY on D3D12: DXR 1.1's acceleration structures and
// inline ray queries (raytracing tier 1.1). What every backend checks the
// same way is in src/gpu/spudgpushared.c; this file translates.
//
// A structure is a buffer with ALLOW_UNORDERED_ACCESS in the
// RAYTRACING_ACCELERATION_STRUCTURE state for its whole life, committed or
// placed, named by its GPU virtual address. A build is one
// BuildRaytracingAccelerationStructure; its geometry descs come from the
// command list's _scratch_geometries, which keeps its capacity, so a steady
// stream of builds allocates nothing.

#include "spudgpud3d12.hpp"
#include "../../../spudstructbase.h"
#include <cstdio>
#include <cstring>

// The layout spudgpu___internal_instance_layout() states for D3D12.
static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == 64,
              "SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 is D3D12's instance desc");
static_assert(D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT == 16, "the layout's alignment");

static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS spudgpu_d3d12___rq_build_flags(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS d3d = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
	return d3d;
}

static D3D12_RAYTRACING_GEOMETRY_FLAGS spudgpu_d3d12___rq_geometry_flags(SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags) {
	D3D12_RAYTRACING_GEOMETRY_FLAGS d3d = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE)
		d3d |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	if (flags & SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES)
		d3d |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
	return d3d;
}

// How many geometry descs [inputs] are: a bottom level's geometries, none
// for a top level.
static uint32_t spudgpu_d3d12___rq_geometry_slots(const spudgpu_acceleration_structure_inputs *inputs) {
	return inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? 0u : inputs->geometry_count;
}

// D3D12's inputs for [inputs], which have passed their checks, over
// [geometries] (spudgpu_d3d12___rq_geometry_slots of them), which this
// fills. Every offset is folded into its address.
static D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS spudgpu_d3d12___rq_inputs(
    const spudgpu_acceleration_structure_inputs *inputs,
    D3D12_RAYTRACING_GEOMETRY_DESC *geometries) {
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = {};
	d3d.Flags                                                = spudgpu_d3d12___rq_build_flags(inputs->flags);
	d3d.DescsLayout                                          = D3D12_ELEMENTS_LAYOUT_ARRAY;
	if (inputs->level == SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		d3d.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		d3d.NumDescs      = inputs->instance_count;
		d3d.InstanceDescs = inputs->instances->_d3d_gpu_address + inputs->instance_offset;
		return d3d;
	}
	d3d.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	d3d.NumDescs       = inputs->geometry_count;
	d3d.pGeometryDescs = geometries;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const spudgpu_acceleration_structure_geometry *source = &inputs->geometries[i];
		D3D12_RAYTRACING_GEOMETRY_DESC *g                     = &geometries[i];
		memset(g, 0, sizeof(*g));
		g->Flags                          = spudgpu_d3d12___rq_geometry_flags(source->flags);
		const D3D12_GPU_VIRTUAL_ADDRESS data = source->data->_d3d_gpu_address + source->data_offset;
		if (source->kind == SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES) {
			g->Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			g->Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
			g->Triangles.VertexCount                = source->data_count;
			g->Triangles.VertexBuffer.StartAddress  = data;
			g->Triangles.VertexBuffer.StrideInBytes = source->data_stride;
			if (source->index_format != SPUDGPU_FORMAT_UNKNOWN) {
				g->Triangles.IndexFormat = source->index_format == SPUDGPU_FORMAT_R16_UINT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
				g->Triangles.IndexCount  = source->index_count;
				g->Triangles.IndexBuffer = source->indices->_d3d_gpu_address + source->index_offset;
			} else {
				g->Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
			}
		} else {
			g->Type                          = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
			g->AABBs.AABBCount               = source->data_count;
			g->AABBs.AABBs.StartAddress      = data;
			g->AABBs.AABBs.StrideInBytes     = source->data_stride;
		}
	}
	return d3d;
}

// [inputs]' sizes. An update needs no scratch without ALLOW_UPDATE
// (spudgpu.h).
static void spudgpu_d3d12___rq_sizes(
    spudgpu_device_d3d12 *device,
    const spudgpu_acceleration_structure_inputs *inputs,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *d3d,
    spudgpu_acceleration_structure_sizes *out_sizes) {
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	device->_d3d_device->GetRaytracingAccelerationStructurePrebuildInfo(d3d, &info);
	out_sizes->storage_size       = info.ResultDataMaxSizeInBytes;
	out_sizes->build_scratch_size = info.ScratchDataSizeInBytes;
	out_sizes->update_scratch_size =
	    (inputs->flags & SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ? info.UpdateScratchDataSizeInBytes : 0;
}

// Frees a structure's handle and what it holds, made or null.
static void spudgpu_d3d12___rq_release(spudgpu_acceleration_structure_d3d12 *structure) {
	const SPUD_ALLOCATOR allocator = structure->head.allocator;
	spudgpu___internal_acceleration_structure_state_free(&structure->_state, &allocator);
	spud_string_free(&allocator, structure->head.debug_name);
	SPUD_QUARANTINE_KEEP(quarantine, structure);
	structure->~spudgpu_acceleration_structure_d3d12();
	SPUD_RETIRE(quarantine, &allocator, spudgpu_acceleration_structure_d3d12, structure);
}

// spudgpu_create_acceleration_structure and the placed one after their
// argument checks: committed when [heap] is null, placed at [offset] of it
// otherwise (the offset already a multiple of placement_alignment and the
// rounded range inside the heap).
static SPUDRESULT spudgpu_d3d12___rq_create(
    spudgpu_device_d3d12 *device,
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	const SPUD_ALLOCATOR *allocator = heap ? &heap->head.allocator : &device->head.allocator;
	spudgpu_acceleration_structure_d3d12 *structure = SPUD_NEW(allocator, spudgpu_acceleration_structure_d3d12);
	if (!structure)
		return SPUDRESULT_OUT_OF_MEMORY;
	structure                 = new (structure) spudgpu_acceleration_structure_d3d12();
	structure->head.allocator = *allocator;
	SPUD_BIRTH_IN(structure, &device->_quarantine);
	structure->_device = device;
	structure->_heap   = heap;
	if (!spudgpu___internal_acceleration_structure_state_init(&structure->_state, &structure->head.allocator, desc)) {
		spudgpu_d3d12___rq_release(structure);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	const CD3DX12_RESOURCE_DESC resourceDesc =
	    CD3DX12_RESOURCE_DESC::Buffer(desc->size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	HRESULT hr = S_OK;
	if (heap) {
		// D3D12's own alignment and size, where it asks more than the
		// reported rule (it doesn't for a buffer: 64 KiB, the rule): the
		// bytes it covers stay inside the range the caller placed it over,
		// its size rounded up to placement_alignment.
		const uint64_t alignment = device->_adapter->_facts.ray_query_placement_alignment;
		const uint64_t covered   = desc->size + (alignment - desc->size % alignment) % alignment;
		const D3D12_RESOURCE_ALLOCATION_INFO info = device->_d3d_device->GetResourceAllocationInfo(0, 1, &resourceDesc);
		if (info.SizeInBytes == UINT64_MAX) {
			spudgpu_d3d12___rq_release(structure);
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}
		if (offset % info.Alignment != 0) {
			spudgpu_d3d12___rq_release(structure);
			return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
		}
		if (info.SizeInBytes > covered) {
			spudgpu_d3d12___rq_release(structure);
			return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
		}
		hr = device->_d3d_device->CreatePlacedResource(
		    heap->_d3d_heap.Get(), offset, &resourceDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
		    IID_PPV_ARGS(&structure->_d3d_resource));
	} else {
		const D3D12_HEAP_PROPERTIES heapProperties = spudgpu_d3d12_get_heap_properties_from_memory_kind(SPUDGPU_MEMORY_KIND_DEVICE);
		hr = device->_d3d_device->CreateCommittedResource(
		    &heapProperties, spudgpu_d3d12_get_heap_flags(), &resourceDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		    nullptr, IID_PPV_ARGS(&structure->_d3d_resource));
	}
	if (FAILED(hr)) {
		spudgpu_d3d12___rq_release(structure);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	structure->_d3d_gpu_address = structure->_d3d_resource->GetGPUVirtualAddress();

	if (spud_debug_name_set(structure, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_d3d12___rq_release(structure);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	SPUDGPU___LIVE_ADD(&device->_live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
#if _DEBUG
	if (heap)
		heap->_placed_count++;
#endif
	*out_structure = structure;
	return SPUD_SUCCESS;
}

extern "C" {

SPUDRESULT spudgpu_get_acceleration_structure_build_sizes(
    spudgpu_device device,
    const spudgpu_acceleration_structure_inputs *inputs,
    spudgpu_acceleration_structure_sizes *out_sizes) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_build_sizes", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!inputs)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT inputs_base_sr = spud_struct_check(&inputs->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != SPUD_SUCCESS)
		return inputs_base_sr;
	if (!out_sizes)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT out_base_sr = spud_struct_out_check(&out_sizes->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES);
	if (out_base_sr != SPUD_SUCCESS)
		return out_base_sr;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT inputs_sr = spudgpu___internal_check_acceleration_structure_inputs(&device->_adapter->_facts, inputs);
	if (inputs_sr != SPUD_SUCCESS)
		return inputs_sr;

	// The geometry descs D3D12's query reads, for this call only.
	const uint32_t slots                    = spudgpu_d3d12___rq_geometry_slots(inputs);
	D3D12_RAYTRACING_GEOMETRY_DESC *geometries = nullptr;
	if (slots > 0) {
		geometries = SPUD_NEW_ARRAY(&device->head.allocator, D3D12_RAYTRACING_GEOMETRY_DESC, slots);
		if (!geometries)
			return SPUDRESULT_OUT_OF_MEMORY;
	}
	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = spudgpu_d3d12___rq_inputs(inputs, geometries);
	spudgpu_acceleration_structure_sizes sizes                     = *out_sizes;
	spudgpu_d3d12___rq_sizes(device, inputs, &d3d, &sizes);
	*out_sizes = sizes;
	if (geometries)
		SPUD_DELETE_ARRAY(&device->head.allocator, D3D12_RAYTRACING_GEOMETRY_DESC, geometries, slots);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_acceleration_structure(
    spudgpu_device device,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_acceleration_structure", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_structure)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	return spudgpu_d3d12___rq_create(device, nullptr, 0, desc, out_structure);
}

SPUDRESULT spudgpu_create_placed_acceleration_structure(
    spudgpu_heap heap,
    uint64_t offset,
    const spudgpu_acceleration_structure_desc *desc,
    spudgpu_acceleration_structure *out_structure) {
	if (!heap)
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (SPUD_STALE(heap, "spudgpu_create_placed_acceleration_structure", "heap"))
		return SPUDRESULT_GPU_INVALID_HEAP;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_structure)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_device_d3d12 *device = heap->_device;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT desc_sr = spudgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	if (heap->_desc.resource_class != SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return SPUDRESULT_GPU_HEAP_MISMATCH;
	const uint64_t alignment = device->_adapter->_facts.ray_query_placement_alignment;
	if (offset % alignment != 0)
		return SPUDRESULT_GPU_PLACEMENT_MISALIGNED;
	// The size rounded up to the alignment, from the offset, inside the
	// heap, with nothing that can wrap.
	if (offset > heap->_desc.size)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	const uint64_t room = heap->_desc.size - offset;
	if (desc->size > room || (desc->size + (alignment - desc->size % alignment) % alignment) > room)
		return SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return spudgpu_d3d12___rq_create(device, heap, offset, desc, out_structure);
}

void spudgpu_destroy_acceleration_structure(spudgpu_acceleration_structure structure) {
	if (!structure)
		return;
	if (SPUD_STALE(structure, "spudgpu_destroy_acceleration_structure", "structure"))
		return;
#if _DEBUG
	if (structure->_heap)
		structure->_heap->_placed_count--;
#endif
	SPUDGPU___LIVE_REMOVE(&structure->_device->_live, SPUDGPU___OBJECT_ACCELERATION_STRUCTURE);
	SPUD_KILL(structure);
	spudgpu_d3d12___rq_release(structure);
}

SPUDRESULT spudgpu_get_acceleration_structure_reference(
    spudgpu_acceleration_structure bottom_level,
    uint64_t *out_reference) {
	if (!bottom_level)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (SPUD_STALE(bottom_level, "spudgpu_get_acceleration_structure_reference", "bottom_level"))
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (bottom_level->_state.level != SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (!out_reference)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_reference = bottom_level->_d3d_gpu_address;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_acceleration_structure_instance_layout(
    spudgpu_device device,
    spudgpu_acceleration_structure_instance_layout *out_layout) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_acceleration_structure_instance_layout", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!out_layout)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	*out_layout = spudgpu___internal_instance_layout();
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_write_acceleration_structure_instances(
    spudgpu_device device,
    const spudgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_write_acceleration_structure_instances", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	return spudgpu___internal_write_instances(spudgpu___internal_instance_layout(), instances, count, out_bytes);
}

SPUDRESULT spudgpu_cmd_build_acceleration_structures(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_build *builds,
    uint32_t count) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_build_acceleration_structures", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	spudgpu_device_d3d12 *device = cmd->_allocator->_device;
	if (!(device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr =
	    spudgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	if (!builds)
		return SPUDRESULT_NULL_DESC;
	if (count == 0)
		return SPUDRESULT_ZERO_SIZE;
	const spudgpu___adapter_facts *facts = &device->_adapter->_facts;

	// Every build checked, in order, before anything is recorded, its
	// sizes asked with its geometry descs in the list's array, which grows
	// to the largest build of the call here, and the destination's layout
	// room made after its last check, so the recording below can't fail.
	eq::array<D3D12_RAYTRACING_GEOMETRY_DESC> &geometries = cmd->_scratch_geometries;
	for (uint32_t b = 0; b < count; ++b) {
		const spudgpu_acceleration_structure_build *build = &builds[b];
		const SPUDRESULT head_sr                         = spudgpu___internal_check_build_head(facts, build);
		if (head_sr != SPUD_SUCCESS)
			return head_sr;
		geometries.clear();
		if (geometries.resize(spudgpu_d3d12___rq_geometry_slots(build->inputs)) != EQ_SUCCESS)
			return SPUDRESULT_OUT_OF_MEMORY;
		const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = spudgpu_d3d12___rq_inputs(build->inputs, geometries.data);
		spudgpu_acceleration_structure_sizes sizes                     = {};
		spudgpu_d3d12___rq_sizes(device, build->inputs, &d3d, &sizes);
		const SPUDRESULT tail_sr = spudgpu___internal_check_build_tail(facts, build, &sizes);
		if (tail_sr != SPUD_SUCCESS)
			return tail_sr;
		const SPUDRESULT layout_sr = spudgpu___internal_reserve_build_layout(build->destination, build->inputs);
		if (layout_sr != SPUD_SUCCESS)
			return layout_sr;
	}

	for (uint32_t b = 0; b < count; ++b) {
		const spudgpu_acceleration_structure_build *build = &builds[b];
		geometries.clear();
		(void)geometries.resize(spudgpu_d3d12___rq_geometry_slots(build->inputs));
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
		desc.Inputs                                             = spudgpu_d3d12___rq_inputs(build->inputs, geometries.data);
		desc.DestAccelerationStructureData                      = build->destination->_d3d_gpu_address;
		desc.ScratchAccelerationStructureData                   = build->scratch->_d3d_gpu_address + build->scratch_offset;
		if (build->mode == SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
			desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
			desc.SourceAccelerationStructureData = build->source->_d3d_gpu_address;
		}
		cmd->_d3d_cmd_list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
		spudgpu___internal_record_build(build);
	}
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_copy_acceleration_structure(
    spudgpu_command_list cmd,
    spudgpu_acceleration_structure source,
    spudgpu_acceleration_structure destination,
    SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_copy_acceleration_structure", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (!(cmd->_allocator->_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr =
	    spudgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	const SPUDRESULT sr = spudgpu___internal_check_copy(source, destination, mode);
	if (sr != SPUD_SUCCESS)
		return sr;
	cmd->_d3d_cmd_list->CopyRaytracingAccelerationStructure(
	    destination->_d3d_gpu_address, source->_d3d_gpu_address,
	    mode == SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT
	                                                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
	spudgpu___internal_record_copy(source, destination);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_cmd_write_acceleration_structure_compacted_sizes(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure *structures,
    uint32_t count,
    spudgpu_buffer buffer,
    uint64_t offset) {
	if (!cmd)
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (SPUD_STALE(cmd, "spudgpu_cmd_write_acceleration_structure_compacted_sizes", "cmd"))
		return SPUDRESULT_GPU_INVALID_COMMAND_LIST;
	if (!(cmd->_allocator->_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const SPUDRESULT list_sr =
	    spudgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != SPUD_SUCCESS)
		return list_sr;
	const SPUDRESULT sr = spudgpu___internal_check_compacted_sizes(structures, count, buffer, offset);
	if (sr != SPUD_SUCCESS)
		return sr;
	eq::array<D3D12_GPU_VIRTUAL_ADDRESS> &addresses = cmd->_scratch_addresses;
	addresses.clear();
	if (addresses.resize(count) != EQ_SUCCESS)
		return SPUDRESULT_OUT_OF_MEMORY;
	for (uint32_t i = 0; i < count; ++i)
		addresses.data[i] = structures[i]->_d3d_gpu_address;
	// One UINT64 a structure, in order, from the offset.
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info = {};
	info.DestBuffer = buffer->_d3d_gpu_address + offset;
	info.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
	cmd->_d3d_cmd_list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, count, addresses.data);
	return SPUD_SUCCESS;
}

void spudgpu_cmd_acceleration_structure_barrier(
    spudgpu_command_list cmd,
    const spudgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (!cmd)
		return;
	if (SPUD_STALE(cmd, "spudgpu_cmd_acceleration_structure_barrier", "cmd"))
		return;
	if (!(cmd->_allocator->_device->_features & SPUDGPU_FEATURE_RAY_QUERY))
		return;
	if (!spudgpu___internal_check_acceleration_structure_barriers(barriers, count))
		return;
	// A UAV barrier on each structure used before in this submission: D3D12
	// orders a structure's accesses by UAV barriers, whatever the pair. One
	// out of NONE needs none: D3D12 orders submissions on a queue.
	eq::array<D3D12_RESOURCE_BARRIER> &d3d = cmd->_scratch_barriers;
	d3d.clear();
	if (d3d.reserve(count) != EQ_SUCCESS) {
		printf("spudgpu: spudgpu_cmd_acceleration_structure_barrier - out of memory building barriers; nothing recorded\n");
		return;
	}
	for (uint32_t i = 0; i < count; ++i)
		if (barriers[i].access_before != SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE)
			(void)d3d.push(CD3DX12_RESOURCE_BARRIER::UAV(barriers[i].structure->_d3d_resource.Get()));
	if (!d3d.empty())
		cmd->_d3d_cmd_list->ResourceBarrier(static_cast<UINT>(d3d.size()), d3d.data);
}

} // extern "C"

#endif // SPUDGPU_COMPILE_D3D12_API
