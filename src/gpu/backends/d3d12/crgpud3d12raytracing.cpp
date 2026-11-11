#if CRGPU_COMPILE_D3D12_API

// CRGPU_EXT_RAY_QUERY on D3D12: DXR 1.1's acceleration structures and
// inline ray queries (raytracing tier 1.1). What every backend checks the
// same way is in src/gpu/crgpushared.c; this file translates.
//
// A structure is a buffer with ALLOW_UNORDERED_ACCESS in the
// RAYTRACING_ACCELERATION_STRUCTURE state for its whole life, committed or
// placed, named by its GPU virtual address. A build is one
// BuildRaytracingAccelerationStructure; its geometry descs come from the
// command list's _scratch_geometries, which keeps its capacity, so a steady
// stream of builds allocates nothing.

#include "crgpud3d12.hpp"
#include "../../../crstructbase.h"
#include <cstdio>
#include <cstring>

// The layout crgpu___internal_instance_layout() states for D3D12.
static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == 64,
              "CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 is D3D12's instance desc");
static_assert(D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT == 16, "the layout's alignment");

static D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS crgpu_d3d12___rq_build_flags(CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS d3d = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION)
		d3d |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
	return d3d;
}

static D3D12_RAYTRACING_GEOMETRY_FLAGS crgpu_d3d12___rq_geometry_flags(CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_FLAGS flags) {
	D3D12_RAYTRACING_GEOMETRY_FLAGS d3d = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE)
		d3d |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	if (flags & CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES)
		d3d |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
	return d3d;
}

// How many geometry descs [inputs] are: a bottom level's geometries, none
// for a top level.
static uint32_t crgpu_d3d12___rq_geometry_slots(const crgpu_acceleration_structure_inputs *inputs) {
	return inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP ? 0u : inputs->geometry_count;
}

// D3D12's inputs for [inputs], which have passed their checks, over
// [geometries] (crgpu_d3d12___rq_geometry_slots of them), which this
// fills. Every offset is folded into its address.
static D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS crgpu_d3d12___rq_inputs(
    const crgpu_acceleration_structure_inputs *inputs,
    D3D12_RAYTRACING_GEOMETRY_DESC *geometries) {
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = {};
	d3d.Flags                                                = crgpu_d3d12___rq_build_flags(inputs->flags);
	d3d.DescsLayout                                          = D3D12_ELEMENTS_LAYOUT_ARRAY;
	if (inputs->level == CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP) {
		d3d.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		d3d.NumDescs      = inputs->instance_count;
		d3d.InstanceDescs = inputs->instances->_d3d_gpu_address + inputs->instance_offset;
		return d3d;
	}
	d3d.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	d3d.NumDescs       = inputs->geometry_count;
	d3d.pGeometryDescs = geometries;
	for (uint32_t i = 0; i < inputs->geometry_count; ++i) {
		const crgpu_acceleration_structure_geometry *source = &inputs->geometries[i];
		D3D12_RAYTRACING_GEOMETRY_DESC *g                     = &geometries[i];
		memset(g, 0, sizeof(*g));
		g->Flags                          = crgpu_d3d12___rq_geometry_flags(source->flags);
		const D3D12_GPU_VIRTUAL_ADDRESS data = source->data->_d3d_gpu_address + source->data_offset;
		if (source->kind == CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES) {
			g->Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			g->Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
			g->Triangles.VertexCount                = source->data_count;
			g->Triangles.VertexBuffer.StartAddress  = data;
			g->Triangles.VertexBuffer.StrideInBytes = source->data_stride;
			if (source->index_format != CRGPU_FORMAT_UNKNOWN) {
				g->Triangles.IndexFormat = source->index_format == CRGPU_FORMAT_R16_UINT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
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
// (crgpu.h).
static void crgpu_d3d12___rq_sizes(
    crgpu_device_d3d12 *device,
    const crgpu_acceleration_structure_inputs *inputs,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *d3d,
    crgpu_acceleration_structure_sizes *out_sizes) {
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
	device->_d3d_device->GetRaytracingAccelerationStructurePrebuildInfo(d3d, &info);
	out_sizes->storage_size       = info.ResultDataMaxSizeInBytes;
	out_sizes->build_scratch_size = info.ScratchDataSizeInBytes;
	out_sizes->update_scratch_size =
	    (inputs->flags & CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE) ? info.UpdateScratchDataSizeInBytes : 0;
}

// Frees a structure's handle and what it holds, made or null.
static void crgpu_d3d12___rq_release(crgpu_acceleration_structure_d3d12 *structure) {
	const CR_ALLOCATOR allocator = structure->head.allocator;
	crgpu___internal_acceleration_structure_state_free(&structure->_state, &allocator);
	cr_string_free(&allocator, structure->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, structure);
	structure->~crgpu_acceleration_structure_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_acceleration_structure_d3d12, structure);
}

// crgpu_acceleration_structure_create and the placed one after their
// argument checks: committed when [heap] is null, placed at [offset] of it
// otherwise (the offset already a multiple of placement_alignment and the
// rounded range inside the heap).
static CR_RESULT crgpu_d3d12___rq_create(
    crgpu_device_d3d12 *device,
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	const CR_ALLOCATOR *allocator = heap ? &heap->head.allocator : &device->head.allocator;
	crgpu_acceleration_structure_d3d12 *structure = CR_NEW(allocator, crgpu_acceleration_structure_d3d12);
	if (!structure)
		return CR_RESULT_OUT_OF_MEMORY;
	structure                 = new (structure) crgpu_acceleration_structure_d3d12();
	structure->head.allocator = *allocator;
	CR_BIRTH_IN(structure, &device->_quarantine);
	structure->_device = device;
	structure->_heap   = heap;
	if (!crgpu___internal_acceleration_structure_state_init(&structure->_state, &structure->head.allocator, desc)) {
		crgpu_d3d12___rq_release(structure);
		return CR_RESULT_OUT_OF_MEMORY;
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
			crgpu_d3d12___rq_release(structure);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
		if (offset % info.Alignment != 0) {
			crgpu_d3d12___rq_release(structure);
			return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
		}
		if (info.SizeInBytes > covered) {
			crgpu_d3d12___rq_release(structure);
			return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
		}
		hr = device->_d3d_device->CreatePlacedResource(
		    heap->_d3d_heap.Get(), offset, &resourceDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
		    IID_PPV_ARGS(&structure->_d3d_resource));
	} else {
		const D3D12_HEAP_PROPERTIES heapProperties = crgpu_d3d12_get_heap_properties_from_memory_kind(CRGPU_MEMORY_KIND_DEVICE);
		hr = device->_d3d_device->CreateCommittedResource(
		    &heapProperties, crgpu_d3d12_get_heap_flags(), &resourceDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		    nullptr, IID_PPV_ARGS(&structure->_d3d_resource));
	}
	if (FAILED(hr)) {
		crgpu_d3d12___rq_release(structure);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	structure->_d3d_gpu_address = structure->_d3d_resource->GetGPUVirtualAddress();

	if (cr_debug_name_set(structure, desc->debug_name) != CR_SUCCESS) {
		crgpu_d3d12___rq_release(structure);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_ACCELERATION_STRUCTURE);
#if _DEBUG
	if (heap)
		heap->_placed_count++;
#endif
	*out_structure = structure;
	return CR_SUCCESS;
}

extern "C" {

CR_RESULT crgpu_device_get_acceleration_structure_build_sizes(
    crgpu_device device,
    const crgpu_acceleration_structure_inputs *inputs,
    crgpu_acceleration_structure_sizes *out_sizes) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_get_acceleration_structure_build_sizes", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!inputs)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT inputs_base_sr = cr_struct_check(&inputs->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS);
	if (inputs_base_sr != CR_SUCCESS)
		return inputs_base_sr;
	if (!out_sizes)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT out_base_sr = cr_struct_out_check(&out_sizes->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES);
	if (out_base_sr != CR_SUCCESS)
		return out_base_sr;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT inputs_sr = crgpu___internal_check_acceleration_structure_inputs(&device->_adapter->_facts, inputs);
	if (inputs_sr != CR_SUCCESS)
		return inputs_sr;

	// The geometry descs D3D12's query reads, for this call only.
	const uint32_t slots                    = crgpu_d3d12___rq_geometry_slots(inputs);
	D3D12_RAYTRACING_GEOMETRY_DESC *geometries = nullptr;
	if (slots > 0) {
		geometries = CR_NEW_ARRAY(&device->head.allocator, D3D12_RAYTRACING_GEOMETRY_DESC, slots);
		if (!geometries)
			return CR_RESULT_OUT_OF_MEMORY;
	}
	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = crgpu_d3d12___rq_inputs(inputs, geometries);
	crgpu_acceleration_structure_sizes sizes                     = *out_sizes;
	crgpu_d3d12___rq_sizes(device, inputs, &d3d, &sizes);
	*out_sizes = sizes;
	if (geometries)
		CR_DELETE_ARRAY(&device->head.allocator, D3D12_RAYTRACING_GEOMETRY_DESC, geometries, slots);
	return CR_SUCCESS;
}

CR_RESULT crgpu_acceleration_structure_create(
    crgpu_device device,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_acceleration_structure_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_structure)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT desc_sr = crgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	return crgpu_d3d12___rq_create(device, nullptr, 0, desc, out_structure);
}

CR_RESULT crgpu_acceleration_structure_create_placed(
    crgpu_heap heap,
    uint64_t offset,
    const crgpu_acceleration_structure_desc *desc,
    crgpu_acceleration_structure *out_structure) {
	if (!heap)
		return CR_RESULT_GPU_INVALID_HEAP;
	if (CR_STALE(heap, "crgpu_acceleration_structure_create_placed", "heap"))
		return CR_RESULT_GPU_INVALID_HEAP;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_structure)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_device_d3d12 *device = heap->_device;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT desc_sr = crgpu___internal_check_acceleration_structure_desc(desc);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	if (heap->_desc.resource_class != CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES)
		return CR_RESULT_GPU_HEAP_MISMATCH;
	const uint64_t alignment = device->_adapter->_facts.ray_query_placement_alignment;
	if (offset % alignment != 0)
		return CR_RESULT_GPU_PLACEMENT_MISALIGNED;
	// The size rounded up to the alignment, from the offset, inside the
	// heap, with nothing that can wrap.
	if (offset > heap->_desc.size)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	const uint64_t room = heap->_desc.size - offset;
	if (desc->size > room || (desc->size + (alignment - desc->size % alignment) % alignment) > room)
		return CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE;
	return crgpu_d3d12___rq_create(device, heap, offset, desc, out_structure);
}

void crgpu_acceleration_structure_destroy(crgpu_acceleration_structure structure) {
	if (!structure)
		return;
	if (CR_STALE(structure, "crgpu_acceleration_structure_destroy", "structure"))
		return;
#if _DEBUG
	if (structure->_heap)
		structure->_heap->_placed_count--;
#endif
	CRGPU___LIVE_REMOVE(&structure->_device->_live, CRGPU___OBJECT_ACCELERATION_STRUCTURE);
	CR_KILL(structure);
	crgpu_d3d12___rq_release(structure);
}

CR_RESULT crgpu_acceleration_structure_get_reference(
    crgpu_acceleration_structure bottom_level,
    uint64_t *out_reference) {
	if (!bottom_level)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (CR_STALE(bottom_level, "crgpu_acceleration_structure_get_reference", "bottom_level"))
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (bottom_level->_state.level != CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM)
		return CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE;
	if (!out_reference)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_reference = bottom_level->_d3d_gpu_address;
	return CR_SUCCESS;
}

CR_RESULT crgpu_device_get_acceleration_structure_instance_layout(
    crgpu_device device,
    crgpu_acceleration_structure_instance_layout *out_layout) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_get_acceleration_structure_instance_layout", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!out_layout)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	*out_layout = crgpu___internal_instance_layout();
	return CR_SUCCESS;
}

CR_RESULT crgpu_device_write_acceleration_structure_instances(
    crgpu_device device,
    const crgpu_acceleration_structure_instance *instances,
    uint32_t count,
    void *out_bytes) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_device_write_acceleration_structure_instances", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	return crgpu___internal_write_instances(crgpu___internal_instance_layout(), instances, count, out_bytes);
}

CR_RESULT crgpu_command_list_build_acceleration_structures(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure_build *builds,
    uint32_t count) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_build_acceleration_structures", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	crgpu_device_d3d12 *device = cmd->_allocator->_device;
	if (!(device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr =
	    crgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	if (!builds)
		return CR_RESULT_NULL_DESC;
	if (count == 0)
		return CR_RESULT_ZERO_SIZE;
	const crgpu___adapter_facts *facts = &device->_adapter->_facts;

	// Every build checked, in order, before anything is recorded, its
	// sizes asked with its geometry descs in the list's array, which grows
	// to the largest build of the call here, and the destination's layout
	// room made after its last check, so the recording below can't fail.
	eq::array<D3D12_RAYTRACING_GEOMETRY_DESC> &geometries = cmd->_scratch_geometries;
	for (uint32_t b = 0; b < count; ++b) {
		const crgpu_acceleration_structure_build *build = &builds[b];
		const CR_RESULT head_sr                         = crgpu___internal_check_build_head(facts, build);
		if (head_sr != CR_SUCCESS)
			return head_sr;
		geometries.clear();
		if (geometries.resize(crgpu_d3d12___rq_geometry_slots(build->inputs)) != EQ_SUCCESS)
			return CR_RESULT_OUT_OF_MEMORY;
		const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d = crgpu_d3d12___rq_inputs(build->inputs, geometries.data);
		crgpu_acceleration_structure_sizes sizes                     = {};
		crgpu_d3d12___rq_sizes(device, build->inputs, &d3d, &sizes);
		const CR_RESULT tail_sr = crgpu___internal_check_build_tail(facts, build, &sizes);
		if (tail_sr != CR_SUCCESS)
			return tail_sr;
		const CR_RESULT layout_sr = crgpu___internal_reserve_build_layout(build->destination, build->inputs);
		if (layout_sr != CR_SUCCESS)
			return layout_sr;
	}

	for (uint32_t b = 0; b < count; ++b) {
		const crgpu_acceleration_structure_build *build = &builds[b];
		geometries.clear();
		(void)geometries.resize(crgpu_d3d12___rq_geometry_slots(build->inputs));
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
		desc.Inputs                                             = crgpu_d3d12___rq_inputs(build->inputs, geometries.data);
		desc.DestAccelerationStructureData                      = build->destination->_d3d_gpu_address;
		desc.ScratchAccelerationStructureData                   = build->scratch->_d3d_gpu_address + build->scratch_offset;
		if (build->mode == CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE) {
			desc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
			desc.SourceAccelerationStructureData = build->source->_d3d_gpu_address;
		}
		cmd->_d3d_cmd_list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
		crgpu___internal_record_build(build);
	}
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_list_copy_acceleration_structure(
    crgpu_command_list cmd,
    crgpu_acceleration_structure source,
    crgpu_acceleration_structure destination,
    CRGPU_ACCELERATION_STRUCTURE_COPY_MODE mode) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_copy_acceleration_structure", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (!(cmd->_allocator->_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr =
	    crgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	const CR_RESULT sr = crgpu___internal_check_copy(source, destination, mode);
	if (sr != CR_SUCCESS)
		return sr;
	cmd->_d3d_cmd_list->CopyRaytracingAccelerationStructure(
	    destination->_d3d_gpu_address, source->_d3d_gpu_address,
	    mode == CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT
	                                                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
	crgpu___internal_record_copy(source, destination);
	return CR_SUCCESS;
}

CR_RESULT crgpu_command_list_write_acceleration_structure_compacted_sizes(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure *structures,
    uint32_t count,
    crgpu_buffer buffer,
    uint64_t offset) {
	if (!cmd)
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (CR_STALE(cmd, "crgpu_command_list_write_acceleration_structure_compacted_sizes", "cmd"))
		return CR_RESULT_GPU_INVALID_COMMAND_LIST;
	if (!(cmd->_allocator->_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED;
	const CR_RESULT list_sr =
	    crgpu___internal_check_acceleration_structure_list(cmd->_allocator->_type, cmd->_rendering_active);
	if (list_sr != CR_SUCCESS)
		return list_sr;
	const CR_RESULT sr = crgpu___internal_check_compacted_sizes(structures, count, buffer, offset);
	if (sr != CR_SUCCESS)
		return sr;
	eq::array<D3D12_GPU_VIRTUAL_ADDRESS> &addresses = cmd->_scratch_addresses;
	addresses.clear();
	if (addresses.resize(count) != EQ_SUCCESS)
		return CR_RESULT_OUT_OF_MEMORY;
	for (uint32_t i = 0; i < count; ++i)
		addresses.data[i] = structures[i]->_d3d_gpu_address;
	// One UINT64 a structure, in order, from the offset.
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info = {};
	info.DestBuffer = buffer->_d3d_gpu_address + offset;
	info.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
	cmd->_d3d_cmd_list->EmitRaytracingAccelerationStructurePostbuildInfo(&info, count, addresses.data);
	return CR_SUCCESS;
}

void crgpu_command_list_acceleration_structure_barrier(
    crgpu_command_list cmd,
    const crgpu_acceleration_structure_barrier *barriers,
    uint32_t count) {
	if (!cmd)
		return;
	if (CR_STALE(cmd, "crgpu_command_list_acceleration_structure_barrier", "cmd"))
		return;
	if (!(cmd->_allocator->_device->_features & CRGPU_FEATURE_RAY_QUERY))
		return;
	if (!crgpu___internal_check_acceleration_structure_barriers(barriers, count))
		return;
	// A UAV barrier on each structure used before in this submission: D3D12
	// orders a structure's accesses by UAV barriers, whatever the pair. One
	// out of NONE needs none: D3D12 orders submissions on a queue.
	eq::array<D3D12_RESOURCE_BARRIER> &d3d = cmd->_scratch_barriers;
	d3d.clear();
	if (d3d.reserve(count) != EQ_SUCCESS) {
		printf("crgpu: crgpu_command_list_acceleration_structure_barrier - out of memory building barriers; nothing recorded\n");
		return;
	}
	for (uint32_t i = 0; i < count; ++i)
		if (barriers[i].access_before != CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE)
			(void)d3d.push(CD3DX12_RESOURCE_BARRIER::UAV(barriers[i].structure->_d3d_resource.Get()));
	if (!d3d.empty())
		cmd->_d3d_cmd_list->ResourceBarrier(static_cast<UINT>(d3d.size()), d3d.data);
}

} // extern "C"

#endif // CRGPU_COMPILE_D3D12_API
