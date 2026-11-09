
#if SPUDGPU_COMPILE_D3D12_API

#include "spudgpud3d12.hpp"
#include <cstring>
#include "../../../spudstructbase.h"
#include "../../spudgpushared.h"

#if _DEBUG
static const char *spudgpu_d3d12_severity_name(D3D12_MESSAGE_SEVERITY severity) {
	switch (severity) {
	case D3D12_MESSAGE_SEVERITY_CORRUPTION:
		return "CORRUPTION";
	case D3D12_MESSAGE_SEVERITY_ERROR:
		return "ERROR";
	case D3D12_MESSAGE_SEVERITY_WARNING:
		return "WARNING";
	case D3D12_MESSAGE_SEVERITY_INFO:
		return "INFO";
	default:
		return "MESSAGE";
	}
}

// Prints the debug layer's corruptions, errors and warnings to stdout, as
// Vulkan's validation layer does with no messenger registered, so a Debug run
// shows them without a debugger attached. Info and plain messages are left
// to the debugger. Called on whichever thread made the D3D12 call that
// raised the message.
static void CALLBACK spudgpu_d3d12_debug_message(
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id,
    LPCSTR description,
    void *context) {
	(void)category;
	if (severity == D3D12_MESSAGE_SEVERITY_INFO || severity == D3D12_MESSAGE_SEVERITY_MESSAGE)
		return;
	const spudgpu_device_d3d12 *device = (const spudgpu_device_d3d12 *)context;
	const char *name                   = device->head.debug_name ? device->head.debug_name : "";
	printf("D3D12 %s: [ %u ] %s%s%s\n",
	    spudgpu_d3d12_severity_name(severity),
	    (unsigned)id,
	    description,
	    name[0] ? " | device " : "",
	    name);
}

// Registers spudgpu_d3d12_debug_message on [device]. Nothing to do when the
// debug layer is off (no info queue) or the runtime predates
// ID3D12InfoQueue1; the device works the same either way, so neither is a
// failure.
static void spudgpu_d3d12_register_debug_callback(spudgpu_device_d3d12 *device) {
	Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info_queue;
	if (FAILED(device->_d3d_device.As(&info_queue)))
		return;
	DWORD cookie = 0;
	if (FAILED(info_queue->RegisterMessageCallback(spudgpu_d3d12_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, device, &cookie)))
		return;
	device->_debug_callback_cookie = cookie;
}

// Unregisters it, before the device it was given as context goes.
static void spudgpu_d3d12_unregister_debug_callback(spudgpu_device_d3d12 *device) {
	if (device->_debug_callback_cookie == 0)
		return;
	Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info_queue;
	if (SUCCEEDED(device->_d3d_device.As(&info_queue)))
		info_queue->UnregisterMessageCallback(device->_debug_callback_cookie);
	device->_debug_callback_cookie = 0;
}
#endif

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

// What the backend needs of every device: 12_0 guarantees resource binding
// tier 2, without which a descriptor table may hold only 128 SRVs per stage
// and SpudGPU's descriptor sets have no such limit. Everything past it is a
// feature an adapter has or lacks.
static const D3D_FEATURE_LEVEL spudgpu_d3d12___minimum_feature_level = D3D_FEATURE_LEVEL_12_0;

// Fills in what [adapter] is and can do. D3D12 answers the features only
// through a device (CheckFeatureSupport is a device method), so this creates
// a probe device on the adapter and releases it before returning: D3D12
// hands back the one live device per adapter, and a probe still held would
// become the caller's device later. Returns false when no device of the
// minimum feature level can be made on the adapter, which is then not
// listed.
static bool spudgpu_d3d12___read_adapter_facts(spudgpu_adapter_d3d12 *adapter) {
	spudgpu___adapter_facts *facts         = &adapter->_facts;
	SPUDGPU_ADAPTER_PROPERTIES *properties = &facts->properties;
	properties->base                       = spud_struct_base_out(SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);

	DXGI_ADAPTER_DESC3 desc = {};
	if (FAILED(adapter->_dxgi_adapter->GetDesc3(&desc)))
		return false;

	Microsoft::WRL::ComPtr<ID3D12Device> probe;
	if (FAILED(D3D12CreateDevice(adapter->_dxgi_adapter.Get(), spudgpu_d3d12___minimum_feature_level, IID_PPV_ARGS(&probe))))
		return false;

	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, properties->description, (int)sizeof(properties->description), NULL, NULL);
	properties->description[sizeof(properties->description) - 1] = '\0';
	properties->vendor_id                                         = desc.VendorId;
	properties->device_id                                         = desc.DeviceId;
	properties->subsystem_id                                      = desc.SubSysId;
	properties->revision                                          = desc.Revision;
	properties->dedicated_video_memory                            = desc.DedicatedVideoMemory;
	properties->dedicated_system_memory                           = desc.DedicatedSystemMemory;
	properties->shared_system_memory                              = desc.SharedSystemMemory;

	// The LUID names the GPU until a restart or a driver restart, and is
	// what interop names an adapter by.
	static_assert(sizeof(desc.AdapterLuid) == 8, "a LUID is 8 bytes");
	memset(properties->adapter_id, 0, sizeof(properties->adapter_id));
	memcpy(properties->adapter_id, &desc.AdapterLuid, sizeof(desc.AdapterLuid));
	memcpy(properties->luid, &desc.AdapterLuid, sizeof(desc.AdapterLuid));
	properties->luid_valid = true;

	D3D12_FEATURE_DATA_ARCHITECTURE1 architecture = {};
	architecture.NodeIndex                        = 0;
	const bool uma = SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture))) &&
	                 architecture.UMA;
	properties->unified_memory = uma;
	if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)
		properties->adapter_type = SPUDGPU_ADAPTER_TYPE_SOFTWARE;
	else if (uma)
		properties->adapter_type = SPUDGPU_ADAPTER_TYPE_INTEGRATED;
	else
		properties->adapter_type = SPUDGPU_ADAPTER_TYPE_DISCRETE;

	// A CBV's BufferLocation, and a raw buffer view's first byte, on every
	// device.
	properties->uniform_buffer_offset_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	properties->storage_buffer_offset_alignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
	// ExecuteIndirect takes any MaxCommandCount.
	properties->max_draw_indirect_count = UINT32_MAX;
	// The largest shader-visible heaps every device allows (tier 3 may allow
	// a larger CBV/SRV/UAV heap; one limit is kept for every tier).
	properties->max_descriptors = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1;
	properties->max_samplers    = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

	// A device makes as many queues of each type as asked, and D3D12 says
	// nothing about where they run.
	properties->queue_pool_count     = 3;
	properties->queue_pools[0].kinds = SPUDGPU_QUEUE_KIND_DIRECT;
	properties->queue_pools[1].kinds = SPUDGPU_QUEUE_KIND_COMPUTE;
	properties->queue_pools[2].kinds = SPUDGPU_QUEUE_KIND_COPY;
	for (uint32_t i = 0; i < 3; ++i) {
		properties->queue_pools[i].max_queues = UINT32_MAX;
		properties->queue_pools[i].engine     = SPUDGPU_QUEUE_POOL_ENGINE_UNKNOWN;
	}

	SPUDGPU_FEATURES features = 0;
	// A layered pass needs the vertex shader to write
	// SV_RenderTargetArrayIndex, which is this capability.
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	const bool have_options = SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
	if (have_options && options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation)
		features |= SPUDGPU_FEATURE_LAYERED_RENDERING;
	// DepthBiasClamp, IndependentBlendEnable, any ExecuteIndirect count and
	// StartInstanceLocation, TEXTURECUBEARRAY views, a direct queue's
	// timestamps, heaps of every kind, presenting, and geometry and
	// tessellation shaders: on every device of the minimum level.
	features |= SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP | SPUDGPU_FEATURE_INDEPENDENT_BLEND | SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT |
	            SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS | SPUDGPU_FEATURE_PASS_TIMESTAMPS | SPUDGPU_FEATURE_MAPPABLE_HEAPS |
	            SPUDGPU_FEATURE_PRESENT | SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
	            SPUDGPU_FEATURE_GEOMETRY_SHADER | SPUDGPU_FEATURE_TESSELLATION;
	// SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE is a custom heap in pool L0 that
	// the CPU can map, which holds a resource in any state only on a UMA
	// adapter.
	if (uma)
		features |= SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
	// Bindless binds SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS storage images and
	// as many storage buffers in one table, more UAVs than tier 2 allows a
	// stage (64).
	if (have_options && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3) {
		features |= SPUDGPU_FEATURE_BINDLESS;
		facts->bindless_max_sampled_images  = SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
		facts->bindless_max_storage_images  = SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
		facts->bindless_max_storage_buffers = SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))) &&
	    options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
		features |= SPUDGPU_FEATURE_MESH_SHADING;
		// D3D12 has no query for these: Shader Model 6.5 guarantees them on
		// every mesh shader tier.
		facts->mesh_max_output_vertices       = 256;
		facts->mesh_max_output_primitives     = 256;
		facts->mesh_max_workgroup_invocations = 128;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2))) &&
	    options2.DepthBoundsTestSupported)
		features |= SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST;
	// Ray queries are DXR 1.1's inline tracing: raytracing tier 1.1. The
	// limits are D3D12's constants, which it has no query for.
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
	    options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) {
		features |= SPUDGPU_FEATURE_RAY_QUERY;
		facts->ray_query_max_geometries           = D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE;
		facts->ray_query_max_instances            = D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE;
		facts->ray_query_max_primitives           = D3D12_RAYTRACING_MAX_PRIMITIVES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE;
		facts->ray_query_scratch_offset_alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		// A placed structure is a placed buffer, which D3D12 places at
		// 64 KiB and covers in multiples of it.
		facts->ray_query_placement_alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	}
	properties->supported_features = features;
	// The probe is released here, as the ComPtr goes.
	return true;
}

static void spudgpu_d3d12___destroy_adapters(spudgpu_instance_d3d12 *instance) {
	for (uint32_t i = 0; i < instance->_adapter_count; ++i) {
		spudgpu_adapter_d3d12 *adapter = instance->_adapters[i];
		if (!adapter)
			continue;
		SPUD_KILL(adapter);
		const SPUD_ALLOCATOR allocator = adapter->head.allocator;
		spud_string_free(&allocator, adapter->head.debug_name);
		adapter->~spudgpu_adapter_d3d12();
		SPUD_DELETE(&allocator, spudgpu_adapter_d3d12, adapter);
	}
	SPUD_DELETE_ARRAY(&instance->head.allocator, spudgpu_adapter, instance->_adapters, instance->_adapter_count);
	instance->_adapters      = nullptr;
	instance->_adapter_count = 0;
}

// Lists the adapters DXGI gives, in its order, that a device of the minimum
// level can be made on: one probe device at a time, each released before
// the next. Software adapters (WARP) are listed too, marked.
static SPUDRESULT spudgpu_d3d12___list_adapters(spudgpu_instance_d3d12 *instance) {
	const EQ_ALLOCATOR eq_allocator = spud_eq_allocator(&instance->head.allocator);
	eq::array<spudgpu_adapter_d3d12 *> listed(&eq_allocator);
	Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgi_adapter1;
	for (UINT index = 0; instance->_dxgi_factory->EnumAdapters1(index, &dxgi_adapter1) != DXGI_ERROR_NOT_FOUND; ++index) {
		Microsoft::WRL::ComPtr<IDXGIAdapter4> dxgi_adapter;
		if (!dxgi_adapter1 || FAILED(dxgi_adapter1.As(&dxgi_adapter)))
			continue;
		spudgpu_adapter_d3d12 *adapter = SPUD_NEW(&instance->head.allocator, spudgpu_adapter_d3d12);
		if (!adapter) {
			for (spudgpu_adapter_d3d12 *made : listed) {
				made->~spudgpu_adapter_d3d12();
				SPUD_DELETE(&instance->head.allocator, spudgpu_adapter_d3d12, made);
			}
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		adapter                 = new (adapter) spudgpu_adapter_d3d12();
		adapter->head.allocator = instance->head.allocator;
		SPUD_BIRTH(adapter);
		adapter->_instance      = instance;
		adapter->_dxgi_adapter  = dxgi_adapter;
		if (!spudgpu_d3d12___read_adapter_facts(adapter)) {
			adapter->~spudgpu_adapter_d3d12();
			SPUD_DELETE(&instance->head.allocator, spudgpu_adapter_d3d12, adapter);
			continue;
		}
		if (listed.push(adapter) != EQ_SUCCESS) {
			adapter->~spudgpu_adapter_d3d12();
			SPUD_DELETE(&instance->head.allocator, spudgpu_adapter_d3d12, adapter);
			for (spudgpu_adapter_d3d12 *made : listed) {
				made->~spudgpu_adapter_d3d12();
				SPUD_DELETE(&instance->head.allocator, spudgpu_adapter_d3d12, made);
			}
			return SPUDRESULT_OUT_OF_MEMORY;
		}
	}
	if (listed.empty())
		return SPUD_SUCCESS;
	instance->_adapters = SPUD_NEW_ARRAY(&instance->head.allocator, spudgpu_adapter, listed.size());
	if (!instance->_adapters) {
		for (spudgpu_adapter_d3d12 *made : listed) {
			made->~spudgpu_adapter_d3d12();
			SPUD_DELETE(&instance->head.allocator, spudgpu_adapter_d3d12, made);
		}
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	for (size_t i = 0; i < listed.size(); ++i)
		instance->_adapters[i] = listed.data[i];
	instance->_adapter_count = (uint32_t)listed.size();
	return SPUD_SUCCESS;
}

extern "C" {

SPUDRESULT spudgpu_create_instance(
    const spudgpu_instance_desc *desc,
    spudgpu_instance *out_instance) {
	// NULL desc, its base, NULL out_instance (and *out_instance = NULL from
	// here on), its allocator.
	const SPUDRESULT check_sr = spudgpu___internal_check_instance_desc(desc, out_instance);
	if (check_sr != SPUD_SUCCESS)
		return check_sr;

	spudgpu_instance_d3d12 *pResult = SPUD_NEW(desc->allocator, spudgpu_instance_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult                      = new (pResult) spudgpu_instance_d3d12();
	pResult->head.allocator      = *desc->allocator;
	SPUD_BIRTH(pResult);
	SPUD_QUARANTINE_INIT(&pResult->_quarantine, &pResult->head.allocator);
	pResult->application_name    = desc->application_name;
	pResult->application_version = desc->application_version;
	pResult->engine_name         = desc->engine_name;
	pResult->engine_version      = desc->engine_version;
	pResult->_adapters           = nullptr;
	pResult->_adapter_count      = 0;

#ifdef _DEBUG
	// Before any device of the process, the adapters' probe devices included:
	// switching the layer on removes every device that already exists.
	Microsoft::WRL::ComPtr<ID3D12Debug1> d3dDebugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3dDebugController)))) {
		d3dDebugController->EnableDebugLayer();
		printf("Enabled D3D12 Debug Interface Layer\r\n");
	}
#endif

	UINT dxgiFlags = 0;
#ifdef _DEBUG
	//dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
	HRESULT hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&pResult->_dxgi_factory));
	if (FAILED(hr)) {
		pResult->~spudgpu_instance_d3d12();
		SPUD_DELETE(desc->allocator, spudgpu_instance_d3d12, pResult);
		return SPUDRESULT_GPU_DEVICE_ENUMERATION_FAILURE;
	}

	const SPUDRESULT list_sr = spudgpu_d3d12___list_adapters(pResult);
	if (list_sr != SPUD_SUCCESS) {
		pResult->~spudgpu_instance_d3d12();
		SPUD_DELETE(desc->allocator, spudgpu_instance_d3d12, pResult);
		return list_sr;
	}

	*out_instance = pResult;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_destroy_instance(spudgpu_instance instance) {
	if (!instance)
		return SPUD_SUCCESS;
	if (SPUD_STALE(instance, "spudgpu_destroy_instance", "instance"))
		return SPUD_SUCCESS;
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	spudgpu_d3d12___destroy_adapters(instance);
	SPUD_QUARANTINE_RELEASE(&instance->_quarantine);
	spud_string_free(&allocator, instance->head.debug_name);
	instance->~spudgpu_instance_d3d12();
	SPUD_DELETE(&allocator, spudgpu_instance_d3d12, instance);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_enumerate_adapters(
    spudgpu_instance instance,
    const spudgpu_adapter **out_adapters,
    uint32_t *out_adapters_count) {
	if (!instance)
		return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudgpu_enumerate_adapters", "instance"))
		return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (!out_adapters)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!out_adapters_count)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_adapters       = instance->_adapters;
	*out_adapters_count = instance->_adapter_count;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_get_adapter_properties(
    spudgpu_adapter adapter,
    SPUDGPU_ADAPTER_PROPERTIES *out_properties) {
	if (!adapter)
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	if (SPUD_STALE(adapter, "spudgpu_get_adapter_properties", "adapter"))
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	return spudgpu___internal_get_adapter_properties(&adapter->_facts, out_properties);
}

SPUDRESULT spudgpu_adapter_supports_surface(
    spudgpu_adapter adapter,
    spudgpu_surface surface,
    bool *out_supported) {
	if (!adapter)
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	if (SPUD_STALE(adapter, "spudgpu_adapter_supports_surface", "adapter"))
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	if (!surface)
		return SPUDRESULT_GPU_INVALID_SURFACE;
	if (SPUD_STALE(surface, "spudgpu_adapter_supports_surface", "surface"))
		return SPUDRESULT_GPU_INVALID_SURFACE;
	if (!out_supported)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	// A DXGI swap chain presents a window from any adapter.
	*out_supported = (adapter->_facts.properties.supported_features & SPUDGPU_FEATURE_PRESENT) != 0;
	return SPUD_SUCCESS;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

static void spudgpu_d3d12___destroy_queues(spudgpu_device_d3d12 *device, spudgpu_command_queue *queues, uint32_t count) {
	if (!queues)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		spudgpu_command_queue_d3d12 *queue = queues[i];
		if (!queue)
			continue;
		SPUD_KILL(queue);
		const SPUD_ALLOCATOR allocator = queue->head.allocator;
		spud_string_free(&allocator, queue->head.debug_name);
		queue->~spudgpu_command_queue_d3d12();
		SPUD_DELETE(&allocator, spudgpu_command_queue_d3d12, queue);
	}
	SPUD_DELETE_ARRAY(&device->head.allocator, spudgpu_command_queue, queues, count);
}

// Makes [count] queues of [type] into a new array at [*out_queues]: all or
// none. SpudGPU sets no priority (spudgpu.h): NORMAL, D3D12's own none.
static SPUDRESULT spudgpu_d3d12___create_queues(
    spudgpu_device_d3d12 *device,
    D3D12_COMMAND_LIST_TYPE type,
    uint32_t count,
    spudgpu_command_queue **out_queues) {
	*out_queues = nullptr;
	if (count == 0)
		return SPUD_SUCCESS;
	spudgpu_command_queue *queues = SPUD_NEW_ARRAY_ZEROED(&device->head.allocator, spudgpu_command_queue, count);
	if (!queues)
		return SPUDRESULT_OUT_OF_MEMORY;
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type                     = type;
	desc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask                 = 1;
	desc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	for (uint32_t i = 0; i < count; ++i) {
		spudgpu_command_queue_d3d12 *queue = SPUD_NEW(&device->head.allocator, spudgpu_command_queue_d3d12);
		if (!queue) {
			spudgpu_d3d12___destroy_queues(device, queues, count);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		queue                 = new (queue) spudgpu_command_queue_d3d12();
		queue->head.allocator = device->head.allocator;
		SPUD_BIRTH(queue);
		queue->_device        = device;
		queue->_scratch_lists = eq::array<ID3D12CommandList *>(&device->_eq_allocator);
		queues[i]             = queue;
		if (FAILED(device->_d3d_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue->_d3d_cmd_queue)))) {
			spudgpu_d3d12___destroy_queues(device, queues, count);
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}
	}
	*out_queues = queues;
	return SPUD_SUCCESS;
}

// Releases [device] and everything it owns, and gives its adapter back.
// Every member is made or empty, so a device that failed part way is
// released too.
static void spudgpu_d3d12___free_device(spudgpu_device_d3d12 *device) {
#if _DEBUG
	// First: the callback's context is this device.
	spudgpu_d3d12_unregister_debug_callback(device);
#endif
	const SPUD_ALLOCATOR allocator = device->head.allocator;
	spudgpu_d3d12___destroy_queues(device, device->_cmd_queues_direct, device->_cmd_queue_count_direct);
	spudgpu_d3d12___destroy_queues(device, device->_cmd_queues_compute, device->_cmd_queue_count_compute);
	spudgpu_d3d12___destroy_queues(device, device->_cmd_queues_copy, device->_cmd_queue_count_copy);
	if (spudgpu_bindless_state_d3d12 *bindless = device->_bindless) {
		if (bindless->layout) {
			SPUD_KILL(bindless->layout);
			spud_string_free(&allocator, bindless->layout->head.debug_name);
		}
		SPUD_DELETE(&allocator, spudgpu_descriptor_set_layout_d3d12, bindless->layout);
		bindless->~spudgpu_bindless_state_d3d12();
		SPUD_DELETE(&allocator, spudgpu_bindless_state_d3d12, bindless);
		device->_bindless = nullptr;
	}
	// Freed here rather than in their structs' destructors, which have no
	// allocator.
	eq_array_free(&device->_csv_heap.free_ranges);
	eq_array_free(&device->_sampler_heap.free_ranges);
	if (device->_tight_draw_signature)
		device->_tight_draw_signature->Release();
	if (device->_tight_draw_indexed_signature)
		device->_tight_draw_indexed_signature->Release();
	for (size_t i = 0; i < device->_indirect_signatures.count; ++i)
		device->_indirect_signatures.data[i].signature->Release();
	eq_array_free(&device->_indirect_signatures);
	SPUD_QUARANTINE_RELEASE(&device->_quarantine);
	spudgpu_adapter_d3d12 *adapter = device->_adapter;
	spud_string_free(&allocator, device->head.debug_name);
	SPUD_QUARANTINE_KEEP(quarantine, device);
	// The native device goes with the destructor, before the adapter is
	// free for another: D3D12 hands back a device still alive.
	device->~spudgpu_device_d3d12();
	SPUD_RETIRE(quarantine, &allocator, spudgpu_device_d3d12, device);
	adapter->_in_use.store(false);
}

extern "C" {

SPUDRESULT spudgpu_create_device(
    spudgpu_adapter adapter,
    const spudgpu_device_desc *desc,
    spudgpu_device *out_device) {
	if (!adapter)
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	if (SPUD_STALE(adapter, "spudgpu_create_device", "adapter"))
		return SPUDRESULT_GPU_INVALID_ADAPTER;
	const SPUDRESULT check_sr = spudgpu___internal_check_device_desc(&adapter->_facts, desc, out_device);
	if (check_sr != SPUD_SUCCESS)
		return check_sr;
	// Last: one device per adapter at a time.
	if (adapter->_in_use.exchange(true))
		return SPUDRESULT_GPU_ADAPTER_IN_USE;

	spudgpu_device_d3d12 *device = SPUD_NEW(&adapter->head.allocator, spudgpu_device_d3d12);
	if (!device) {
		adapter->_in_use.store(false);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	device                 = new (device) spudgpu_device_d3d12();
	device->head.allocator = adapter->head.allocator;
	SPUD_BIRTH_IN(device, &adapter->_instance->_quarantine);
	SPUD_QUARANTINE_INIT(&device->_quarantine, &device->head.allocator);
	device->_eq_allocator  = spud_eq_allocator(&device->head.allocator);
	eq_array_init(&device->_csv_heap.free_ranges, &device->_eq_allocator);
	eq_array_init(&device->_sampler_heap.free_ranges, &device->_eq_allocator);
	eq_array_init(&device->_indirect_signatures, &device->_eq_allocator);
	device->_adapter = adapter;
	device->_features      = desc->required_features;
#if _DEBUG
	if (!spudgpu___internal_live_init(&device->_live)) {
		spudgpu_d3d12___free_device(device);
		return SPUDRESULT_GENERAL_FAILURE;
	}
#endif

	SPUDRESULT sr = SPUD_SUCCESS;
	if (FAILED(D3D12CreateDevice(adapter->_dxgi_adapter.Get(), spudgpu_d3d12___minimum_feature_level, IID_PPV_ARGS(&device->_d3d_device))))
		sr = SPUDRESULT_API_SPECIFIC_FAILURE;
#if _DEBUG
	if (sr == SPUD_SUCCESS)
		spudgpu_d3d12_register_debug_callback(device);
#endif
	if (sr == SPUD_SUCCESS)
		sr = spudgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_DIRECT, desc->direct_queue_count, &device->_cmd_queues_direct);
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_direct = desc->direct_queue_count;
		sr = spudgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, desc->compute_queue_count, &device->_cmd_queues_compute);
	}
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_compute = desc->compute_queue_count;
		sr = spudgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_COPY, desc->copy_queue_count, &device->_cmd_queues_copy);
	}
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_copy = desc->copy_queue_count;
		sr = spudgpu_d3d12___create_shader_heaps(device, desc->max_descriptors, desc->max_samplers);
	}
	if (sr == SPUD_SUCCESS && (desc->required_features & SPUDGPU_FEATURE_BINDLESS))
		sr = spudgpu_d3d12___create_bindless_state(device);
	if (sr == SPUD_SUCCESS) {
		device->_tight_draw_signature =
		    spudgpu_d3d12___create_draw_indirect_signature(device->_d3d_device.Get(), false, sizeof(spudgpu_draw_indirect_args));
		device->_tight_draw_indexed_signature = spudgpu_d3d12___create_draw_indirect_signature(
		    device->_d3d_device.Get(), true, sizeof(spudgpu_draw_indexed_indirect_args));
		if (!device->_tight_draw_signature || !device->_tight_draw_indexed_signature)
			sr = SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	if (sr == SPUD_SUCCESS && spud_debug_name_set(device, desc->debug_name) != SPUD_SUCCESS)
		sr = SPUDRESULT_OUT_OF_MEMORY;
	if (sr != SPUD_SUCCESS) {
#if _DEBUG
		spudgpu___internal_live_destroy(&device->_live);
#endif
		spudgpu_d3d12___free_device(device);
		return sr;
	}

	*out_device = device;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_device(spudgpu_device device) {
	if (!device)
		return;
	if (SPUD_STALE(device, "spudgpu_destroy_device", "device"))
		return;
#if _DEBUG
	spudgpu___internal_live_check_none(&device->_live, device->head.debug_name);
	spudgpu___internal_live_destroy(&device->_live);
#endif
	SPUD_KILL(device);
	spudgpu_d3d12___free_device(device);
}

spudgpu_adapter spudgpu_get_device_adapter(spudgpu_device device) {
	if (!device) return nullptr;
	if (SPUD_STALE(device, "spudgpu_get_device_adapter", "device")) return nullptr;
	return device->_adapter;
}

SPUDGPU_FEATURES spudgpu_get_device_features(spudgpu_device device) {
	if (!device) return 0;
	if (SPUD_STALE(device, "spudgpu_get_device_features", "device")) return 0;
	return device->_features;
}

SPUDGPU_NATIVE_API spudgpu_get_native_gpu_api(spudgpu_instance instance) {
	if (!instance) return SPUDGPU_NATIVE_API_NONE;
	if (SPUD_STALE(instance, "spudgpu_get_native_gpu_api", "instance")) return SPUDGPU_NATIVE_API_NONE;
	return SPUDGPU_NATIVE_API_D3D12;
}

SPUDRESULT spudgpu_create_surface(
    spudgpu_instance instance,
    void *window_handle,
    void *display_handle,
    spudgpu_surface *out_surface) {
	if (!instance)
		return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudgpu_create_surface", "instance"))
		return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (!window_handle)
		return SPUDRESULT_GPU_INVALID_WINDOW_HANDLE;
	if (!out_surface)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	spudgpu_surface_d3d12 *pResult =
	    SPUD_NEW_ZEROED(&instance->head.allocator, spudgpu_surface_d3d12);
	if (!pResult)
		return SPUDRESULT_OUT_OF_MEMORY;
	pResult->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(pResult, &instance->_quarantine);
	pResult->_hwnd          = (HWND)window_handle;
	pResult->_instance      = instance;
	*out_surface            = pResult;
	return SPUD_SUCCESS;
}
void spudgpu_destroy_surface(spudgpu_surface surface) {
	if (!surface)
		return;
	if (SPUD_STALE(surface, "spudgpu_destroy_surface", "surface"))
		return;
	SPUD_KILL(surface);
	const SPUD_ALLOCATOR allocator = surface->head.allocator;
	spud_string_free(&allocator, surface->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(surface), &allocator, spudgpu_surface_d3d12, surface);
}
}

#endif
