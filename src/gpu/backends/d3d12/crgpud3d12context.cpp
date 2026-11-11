
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include <cstring>
#include "../../../crstructbase.h"
#include "../../crgpushared.h"

#if _DEBUG
static const char *crgpu_d3d12_severity_name(D3D12_MESSAGE_SEVERITY severity) {
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
static void CALLBACK crgpu_d3d12_debug_message(
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id,
    LPCSTR description,
    void *context) {
	(void)category;
	if (severity == D3D12_MESSAGE_SEVERITY_INFO || severity == D3D12_MESSAGE_SEVERITY_MESSAGE)
		return;
	const crgpu_device_d3d12 *device = (const crgpu_device_d3d12 *)context;
	const char *name                   = device->head.debug_name ? device->head.debug_name : "";
	printf("D3D12 %s: [ %u ] %s%s%s\n",
	    crgpu_d3d12_severity_name(severity),
	    (unsigned)id,
	    description,
	    name[0] ? " | device " : "",
	    name);
}

// Registers crgpu_d3d12_debug_message on [device]. Nothing to do when the
// debug layer is off (no info queue) or the runtime predates
// ID3D12InfoQueue1; the device works the same either way, so neither is a
// failure.
static void crgpu_d3d12_register_debug_callback(crgpu_device_d3d12 *device) {
	Microsoft::WRL::ComPtr<ID3D12InfoQueue1> info_queue;
	if (FAILED(device->_d3d_device.As(&info_queue)))
		return;
	DWORD cookie = 0;
	if (FAILED(info_queue->RegisterMessageCallback(crgpu_d3d12_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, device, &cookie)))
		return;
	device->_debug_callback_cookie = cookie;
}

// Unregisters it, before the device it was given as context goes.
static void crgpu_d3d12_unregister_debug_callback(crgpu_device_d3d12 *device) {
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
// and Carrottus GPU's descriptor sets have no such limit. Everything past it is a
// feature an adapter has or lacks.
static const D3D_FEATURE_LEVEL crgpu_d3d12___minimum_feature_level = D3D_FEATURE_LEVEL_12_0;

// Fills in what [adapter] is and can do. D3D12 answers the features only
// through a device (CheckFeatureSupport is a device method), so this creates
// a probe device on the adapter and releases it before returning: D3D12
// hands back the one live device per adapter, and a probe still held would
// become the caller's device later. Returns false when no device of the
// minimum feature level can be made on the adapter, which is then not
// listed.
static bool crgpu_d3d12___read_adapter_facts(crgpu_adapter_d3d12 *adapter) {
	crgpu___adapter_facts *facts         = &adapter->_facts;
	CRGPU_ADAPTER_PROPERTIES *properties = &facts->properties;
	properties->base                       = cr_struct_base_out(CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);

	DXGI_ADAPTER_DESC3 desc = {};
	if (FAILED(adapter->_dxgi_adapter->GetDesc3(&desc)))
		return false;

	Microsoft::WRL::ComPtr<ID3D12Device> probe;
	if (FAILED(D3D12CreateDevice(adapter->_dxgi_adapter.Get(), crgpu_d3d12___minimum_feature_level, IID_PPV_ARGS(&probe))))
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
		properties->adapter_type = CRGPU_ADAPTER_TYPE_SOFTWARE;
	else if (uma)
		properties->adapter_type = CRGPU_ADAPTER_TYPE_INTEGRATED;
	else
		properties->adapter_type = CRGPU_ADAPTER_TYPE_DISCRETE;

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
	properties->queue_pools[0].kinds = CRGPU_QUEUE_KIND_DIRECT;
	properties->queue_pools[1].kinds = CRGPU_QUEUE_KIND_COMPUTE;
	properties->queue_pools[2].kinds = CRGPU_QUEUE_KIND_COPY;
	for (uint32_t i = 0; i < 3; ++i) {
		properties->queue_pools[i].max_queues = UINT32_MAX;
		properties->queue_pools[i].engine     = CRGPU_QUEUE_POOL_ENGINE_UNKNOWN;
	}

	CRGPU_FEATURES features = 0;
	// A layered pass needs the vertex shader to write
	// SV_RenderTargetArrayIndex, which is this capability.
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	const bool have_options = SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
	if (have_options && options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation)
		features |= CRGPU_FEATURE_LAYERED_RENDERING;
	// DepthBiasClamp, IndependentBlendEnable, any ExecuteIndirect count and
	// StartInstanceLocation, TEXTURECUBEARRAY views, a direct queue's
	// timestamps, heaps of every kind, presenting, and geometry and
	// tessellation shaders: on every device of the minimum level.
	features |= CRGPU_FEATURE_DEPTH_BIAS_CLAMP | CRGPU_FEATURE_INDEPENDENT_BLEND | CRGPU_FEATURE_MULTI_DRAW_INDIRECT |
	            CRGPU_FEATURE_CUBE_ARRAY_VIEWS | CRGPU_FEATURE_PASS_TIMESTAMPS | CRGPU_FEATURE_MAPPABLE_HEAPS |
	            CRGPU_FEATURE_PRESENT | CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE |
	            CRGPU_FEATURE_GEOMETRY_SHADER | CRGPU_FEATURE_TESSELLATION;
	// CRGPU_MEMORY_KIND_DEVICE_MAPPABLE is a custom heap in pool L0 that
	// the CPU can map, which holds a resource in any state only on a UMA
	// adapter.
	if (uma)
		features |= CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
	// Bindless binds CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS storage images and
	// as many storage buffers in one table, more UAVs than tier 2 allows a
	// stage (64).
	if (have_options && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3) {
		features |= CRGPU_FEATURE_BINDLESS;
		facts->bindless_max_sampled_images  = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
		facts->bindless_max_storage_images  = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
		facts->bindless_max_storage_buffers = CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))) &&
	    options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
		features |= CRGPU_FEATURE_MESH_SHADING;
		// D3D12 has no query for these: Shader Model 6.5 guarantees them on
		// every mesh shader tier.
		facts->mesh_max_output_vertices       = 256;
		facts->mesh_max_output_primitives     = 256;
		facts->mesh_max_workgroup_invocations = 128;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2))) &&
	    options2.DepthBoundsTestSupported)
		features |= CRGPU_FEATURE_DEPTH_BOUNDS_TEST;
	// Ray queries are DXR 1.1's inline tracing: raytracing tier 1.1. The
	// limits are D3D12's constants, which it has no query for.
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	if (SUCCEEDED(probe->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
	    options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) {
		features |= CRGPU_FEATURE_RAY_QUERY;
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

static void crgpu_d3d12___destroy_adapters(crgpu_instance_d3d12 *instance) {
	for (uint32_t i = 0; i < instance->_adapter_count; ++i) {
		crgpu_adapter_d3d12 *adapter = instance->_adapters[i];
		if (!adapter)
			continue;
		CR_KILL(adapter);
		const CR_ALLOCATOR allocator = adapter->head.allocator;
		cr_string_free(&allocator, adapter->head.debug_name);
		adapter->~crgpu_adapter_d3d12();
		CR_DELETE(&allocator, crgpu_adapter_d3d12, adapter);
	}
	CR_DELETE_ARRAY(&instance->head.allocator, crgpu_adapter, instance->_adapters, instance->_adapter_count);
	instance->_adapters      = nullptr;
	instance->_adapter_count = 0;
}

// Lists the adapters DXGI gives, in its order, that a device of the minimum
// level can be made on: one probe device at a time, each released before
// the next. Software adapters (WARP) are listed too, marked.
static CR_RESULT crgpu_d3d12___list_adapters(crgpu_instance_d3d12 *instance) {
	const EQ_ALLOCATOR eq_allocator = cr_eq_allocator(&instance->head.allocator);
	eq::array<crgpu_adapter_d3d12 *> listed(&eq_allocator);
	Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgi_adapter1;
	for (UINT index = 0; instance->_dxgi_factory->EnumAdapters1(index, &dxgi_adapter1) != DXGI_ERROR_NOT_FOUND; ++index) {
		Microsoft::WRL::ComPtr<IDXGIAdapter4> dxgi_adapter;
		if (!dxgi_adapter1 || FAILED(dxgi_adapter1.As(&dxgi_adapter)))
			continue;
		crgpu_adapter_d3d12 *adapter = CR_NEW(&instance->head.allocator, crgpu_adapter_d3d12);
		if (!adapter) {
			for (crgpu_adapter_d3d12 *made : listed) {
				made->~crgpu_adapter_d3d12();
				CR_DELETE(&instance->head.allocator, crgpu_adapter_d3d12, made);
			}
			return CR_RESULT_OUT_OF_MEMORY;
		}
		adapter                 = new (adapter) crgpu_adapter_d3d12();
		adapter->head.allocator = instance->head.allocator;
		CR_BIRTH(adapter);
		adapter->_instance      = instance;
		adapter->_dxgi_adapter  = dxgi_adapter;
		if (!crgpu_d3d12___read_adapter_facts(adapter)) {
			adapter->~crgpu_adapter_d3d12();
			CR_DELETE(&instance->head.allocator, crgpu_adapter_d3d12, adapter);
			continue;
		}
		if (listed.push(adapter) != EQ_SUCCESS) {
			adapter->~crgpu_adapter_d3d12();
			CR_DELETE(&instance->head.allocator, crgpu_adapter_d3d12, adapter);
			for (crgpu_adapter_d3d12 *made : listed) {
				made->~crgpu_adapter_d3d12();
				CR_DELETE(&instance->head.allocator, crgpu_adapter_d3d12, made);
			}
			return CR_RESULT_OUT_OF_MEMORY;
		}
	}
	if (listed.empty())
		return CR_SUCCESS;
	instance->_adapters = CR_NEW_ARRAY(&instance->head.allocator, crgpu_adapter, listed.size());
	if (!instance->_adapters) {
		for (crgpu_adapter_d3d12 *made : listed) {
			made->~crgpu_adapter_d3d12();
			CR_DELETE(&instance->head.allocator, crgpu_adapter_d3d12, made);
		}
		return CR_RESULT_OUT_OF_MEMORY;
	}
	for (size_t i = 0; i < listed.size(); ++i)
		instance->_adapters[i] = listed.data[i];
	instance->_adapter_count = (uint32_t)listed.size();
	return CR_SUCCESS;
}

extern "C" {

CR_RESULT crgpu_instance_create(
    const crgpu_instance_desc *desc,
    crgpu_instance *out_instance) {
	// NULL desc, its base, NULL out_instance (and *out_instance = NULL from
	// here on), its allocator.
	const CR_RESULT check_sr = crgpu___internal_check_instance_desc(desc, out_instance);
	if (check_sr != CR_SUCCESS)
		return check_sr;

	crgpu_instance_d3d12 *pResult = CR_NEW(desc->allocator, crgpu_instance_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                      = new (pResult) crgpu_instance_d3d12();
	pResult->head.allocator      = *desc->allocator;
	CR_BIRTH(pResult);
	CR_QUARANTINE_INIT(&pResult->_quarantine, &pResult->head.allocator);
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
		pResult->~crgpu_instance_d3d12();
		CR_DELETE(desc->allocator, crgpu_instance_d3d12, pResult);
		return CR_RESULT_GPU_DEVICE_ENUMERATION_FAILURE;
	}

	const CR_RESULT list_sr = crgpu_d3d12___list_adapters(pResult);
	if (list_sr != CR_SUCCESS) {
		pResult->~crgpu_instance_d3d12();
		CR_DELETE(desc->allocator, crgpu_instance_d3d12, pResult);
		return list_sr;
	}

	*out_instance = pResult;
	return CR_SUCCESS;
}

CR_RESULT crgpu_instance_destroy(crgpu_instance instance) {
	if (!instance)
		return CR_SUCCESS;
	if (CR_STALE(instance, "crgpu_instance_destroy", "instance"))
		return CR_SUCCESS;
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	crgpu_d3d12___destroy_adapters(instance);
	CR_QUARANTINE_RELEASE(&instance->_quarantine);
	cr_string_free(&allocator, instance->head.debug_name);
	instance->~crgpu_instance_d3d12();
	CR_DELETE(&allocator, crgpu_instance_d3d12, instance);
	return CR_SUCCESS;
}

CR_RESULT crgpu_adapter_enumerate(
    crgpu_instance instance,
    const crgpu_adapter **out_adapters,
    uint32_t *out_adapters_count) {
	if (!instance)
		return CR_RESULT_GPU_INVALID_INSTANCE;
	if (CR_STALE(instance, "crgpu_adapter_enumerate", "instance"))
		return CR_RESULT_GPU_INVALID_INSTANCE;
	if (!out_adapters)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!out_adapters_count)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_adapters       = instance->_adapters;
	*out_adapters_count = instance->_adapter_count;
	return CR_SUCCESS;
}

CR_RESULT crgpu_adapter_get_properties(
    crgpu_adapter adapter,
    CRGPU_ADAPTER_PROPERTIES *out_properties) {
	if (!adapter)
		return CR_RESULT_GPU_INVALID_ADAPTER;
	if (CR_STALE(adapter, "crgpu_adapter_get_properties", "adapter"))
		return CR_RESULT_GPU_INVALID_ADAPTER;
	return crgpu___internal_get_adapter_properties(&adapter->_facts, out_properties);
}

CR_RESULT crgpu_adapter_supports_surface(
    crgpu_adapter adapter,
    crgpu_surface surface,
    bool *out_supported) {
	if (!adapter)
		return CR_RESULT_GPU_INVALID_ADAPTER;
	if (CR_STALE(adapter, "crgpu_adapter_supports_surface", "adapter"))
		return CR_RESULT_GPU_INVALID_ADAPTER;
	if (!surface)
		return CR_RESULT_GPU_INVALID_SURFACE;
	if (CR_STALE(surface, "crgpu_adapter_supports_surface", "surface"))
		return CR_RESULT_GPU_INVALID_SURFACE;
	if (!out_supported)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	// A DXGI swap chain presents a window from any adapter.
	*out_supported = (adapter->_facts.properties.supported_features & CRGPU_FEATURE_PRESENT) != 0;
	return CR_SUCCESS;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

static void crgpu_d3d12___destroy_queues(crgpu_device_d3d12 *device, crgpu_command_queue *queues, uint32_t count) {
	if (!queues)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		crgpu_command_queue_d3d12 *queue = queues[i];
		if (!queue)
			continue;
		CR_KILL(queue);
		const CR_ALLOCATOR allocator = queue->head.allocator;
		cr_string_free(&allocator, queue->head.debug_name);
		queue->~crgpu_command_queue_d3d12();
		CR_DELETE(&allocator, crgpu_command_queue_d3d12, queue);
	}
	CR_DELETE_ARRAY(&device->head.allocator, crgpu_command_queue, queues, count);
}

// Makes [count] queues of [type] into a new array at [*out_queues]: all or
// none. Carrottus GPU sets no priority (crgpu.h): NORMAL, D3D12's own none.
static CR_RESULT crgpu_d3d12___create_queues(
    crgpu_device_d3d12 *device,
    D3D12_COMMAND_LIST_TYPE type,
    uint32_t count,
    crgpu_command_queue **out_queues) {
	*out_queues = nullptr;
	if (count == 0)
		return CR_SUCCESS;
	crgpu_command_queue *queues = CR_NEW_ARRAY_ZEROED(&device->head.allocator, crgpu_command_queue, count);
	if (!queues)
		return CR_RESULT_OUT_OF_MEMORY;
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type                     = type;
	desc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask                 = 1;
	desc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	for (uint32_t i = 0; i < count; ++i) {
		crgpu_command_queue_d3d12 *queue = CR_NEW(&device->head.allocator, crgpu_command_queue_d3d12);
		if (!queue) {
			crgpu_d3d12___destroy_queues(device, queues, count);
			return CR_RESULT_OUT_OF_MEMORY;
		}
		queue                 = new (queue) crgpu_command_queue_d3d12();
		queue->head.allocator = device->head.allocator;
		CR_BIRTH(queue);
		queue->_device        = device;
		queue->_scratch_lists = eq::array<ID3D12CommandList *>(&device->_eq_allocator);
		queues[i]             = queue;
		if (FAILED(device->_d3d_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue->_d3d_cmd_queue)))) {
			crgpu_d3d12___destroy_queues(device, queues, count);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
	}
	*out_queues = queues;
	return CR_SUCCESS;
}

// Releases [device] and everything it owns, and gives its adapter back.
// Every member is made or empty, so a device that failed part way is
// released too.
static void crgpu_d3d12___free_device(crgpu_device_d3d12 *device) {
#if _DEBUG
	// First: the callback's context is this device.
	crgpu_d3d12_unregister_debug_callback(device);
#endif
	const CR_ALLOCATOR allocator = device->head.allocator;
	crgpu_d3d12___destroy_queues(device, device->_cmd_queues_direct, device->_cmd_queue_count_direct);
	crgpu_d3d12___destroy_queues(device, device->_cmd_queues_compute, device->_cmd_queue_count_compute);
	crgpu_d3d12___destroy_queues(device, device->_cmd_queues_copy, device->_cmd_queue_count_copy);
	if (crgpu_bindless_state_d3d12 *bindless = device->_bindless) {
		if (bindless->layout) {
			CR_KILL(bindless->layout);
			cr_string_free(&allocator, bindless->layout->head.debug_name);
		}
		CR_DELETE(&allocator, crgpu_descriptor_set_layout_d3d12, bindless->layout);
		bindless->~crgpu_bindless_state_d3d12();
		CR_DELETE(&allocator, crgpu_bindless_state_d3d12, bindless);
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
	CR_QUARANTINE_RELEASE(&device->_quarantine);
	crgpu_adapter_d3d12 *adapter = device->_adapter;
	cr_string_free(&allocator, device->head.debug_name);
	CR_QUARANTINE_KEEP(quarantine, device);
	// The native device goes with the destructor, before the adapter is
	// free for another: D3D12 hands back a device still alive.
	device->~crgpu_device_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_device_d3d12, device);
	adapter->_in_use.store(false);
}

extern "C" {

CR_RESULT crgpu_device_create(
    crgpu_adapter adapter,
    const crgpu_device_desc *desc,
    crgpu_device *out_device) {
	if (!adapter)
		return CR_RESULT_GPU_INVALID_ADAPTER;
	if (CR_STALE(adapter, "crgpu_device_create", "adapter"))
		return CR_RESULT_GPU_INVALID_ADAPTER;
	const CR_RESULT check_sr = crgpu___internal_check_device_desc(&adapter->_facts, desc, out_device);
	if (check_sr != CR_SUCCESS)
		return check_sr;
	// Last: one device per adapter at a time.
	if (adapter->_in_use.exchange(true))
		return CR_RESULT_GPU_ADAPTER_IN_USE;

	crgpu_device_d3d12 *device = CR_NEW(&adapter->head.allocator, crgpu_device_d3d12);
	if (!device) {
		adapter->_in_use.store(false);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	device                 = new (device) crgpu_device_d3d12();
	device->head.allocator = adapter->head.allocator;
	CR_BIRTH_IN(device, &adapter->_instance->_quarantine);
	CR_QUARANTINE_INIT(&device->_quarantine, &device->head.allocator);
	device->_eq_allocator  = cr_eq_allocator(&device->head.allocator);
	eq_array_init(&device->_csv_heap.free_ranges, &device->_eq_allocator);
	eq_array_init(&device->_sampler_heap.free_ranges, &device->_eq_allocator);
	eq_array_init(&device->_indirect_signatures, &device->_eq_allocator);
	device->_adapter = adapter;
	device->_features      = desc->required_features;
#if _DEBUG
	if (!crgpu___internal_live_init(&device->_live)) {
		crgpu_d3d12___free_device(device);
		return CR_RESULT_GENERAL_FAILURE;
	}
#endif

	CR_RESULT sr = CR_SUCCESS;
	if (FAILED(D3D12CreateDevice(adapter->_dxgi_adapter.Get(), crgpu_d3d12___minimum_feature_level, IID_PPV_ARGS(&device->_d3d_device))))
		sr = CR_RESULT_API_SPECIFIC_FAILURE;
#if _DEBUG
	if (sr == CR_SUCCESS)
		crgpu_d3d12_register_debug_callback(device);
#endif
	if (sr == CR_SUCCESS)
		sr = crgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_DIRECT, desc->direct_queue_count, &device->_cmd_queues_direct);
	if (sr == CR_SUCCESS) {
		device->_cmd_queue_count_direct = desc->direct_queue_count;
		sr = crgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, desc->compute_queue_count, &device->_cmd_queues_compute);
	}
	if (sr == CR_SUCCESS) {
		device->_cmd_queue_count_compute = desc->compute_queue_count;
		sr = crgpu_d3d12___create_queues(device, D3D12_COMMAND_LIST_TYPE_COPY, desc->copy_queue_count, &device->_cmd_queues_copy);
	}
	if (sr == CR_SUCCESS) {
		device->_cmd_queue_count_copy = desc->copy_queue_count;
		sr = crgpu_d3d12___create_shader_heaps(device, desc->max_descriptors, desc->max_samplers);
	}
	if (sr == CR_SUCCESS && (desc->required_features & CRGPU_FEATURE_BINDLESS))
		sr = crgpu_d3d12___create_bindless_state(device);
	if (sr == CR_SUCCESS) {
		device->_tight_draw_signature =
		    crgpu_d3d12___create_draw_indirect_signature(device->_d3d_device.Get(), false, sizeof(crgpu_draw_indirect_args));
		device->_tight_draw_indexed_signature = crgpu_d3d12___create_draw_indirect_signature(
		    device->_d3d_device.Get(), true, sizeof(crgpu_draw_indexed_indirect_args));
		if (!device->_tight_draw_signature || !device->_tight_draw_indexed_signature)
			sr = CR_RESULT_API_SPECIFIC_FAILURE;
	}
	if (sr == CR_SUCCESS && cr_debug_name_set(device, desc->debug_name) != CR_SUCCESS)
		sr = CR_RESULT_OUT_OF_MEMORY;
	if (sr != CR_SUCCESS) {
#if _DEBUG
		crgpu___internal_live_destroy(&device->_live);
#endif
		crgpu_d3d12___free_device(device);
		return sr;
	}

	*out_device = device;
	return CR_SUCCESS;
}

void crgpu_device_destroy(crgpu_device device) {
	if (!device)
		return;
	if (CR_STALE(device, "crgpu_device_destroy", "device"))
		return;
#if _DEBUG
	crgpu___internal_live_check_none(&device->_live, device->head.debug_name);
	crgpu___internal_live_destroy(&device->_live);
#endif
	CR_KILL(device);
	crgpu_d3d12___free_device(device);
}

crgpu_adapter crgpu_device_get_adapter(crgpu_device device) {
	if (!device) return nullptr;
	if (CR_STALE(device, "crgpu_device_get_adapter", "device")) return nullptr;
	return device->_adapter;
}

CRGPU_FEATURES crgpu_device_get_features(crgpu_device device) {
	if (!device) return 0;
	if (CR_STALE(device, "crgpu_device_get_features", "device")) return 0;
	return device->_features;
}

CRGPU_NATIVE_API crgpu_instance_get_native_gpu_api(crgpu_instance instance) {
	if (!instance) return CRGPU_NATIVE_API_NONE;
	if (CR_STALE(instance, "crgpu_instance_get_native_gpu_api", "instance")) return CRGPU_NATIVE_API_NONE;
	return CRGPU_NATIVE_API_D3D12;
}

CR_RESULT crgpu_surface_create(
    crgpu_instance instance,
    void *window_handle,
    void *display_handle,
    crgpu_surface *out_surface) {
	if (!instance)
		return CR_RESULT_GPU_INVALID_INSTANCE;
	if (CR_STALE(instance, "crgpu_surface_create", "instance"))
		return CR_RESULT_GPU_INVALID_INSTANCE;
	if (!window_handle)
		return CR_RESULT_GPU_INVALID_WINDOW_HANDLE;
	if (!out_surface)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	crgpu_surface_d3d12 *pResult =
	    CR_NEW_ZEROED(&instance->head.allocator, crgpu_surface_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(pResult, &instance->_quarantine);
	pResult->_hwnd          = (HWND)window_handle;
	pResult->_instance      = instance;
	*out_surface            = pResult;
	return CR_SUCCESS;
}
void crgpu_surface_destroy(crgpu_surface surface) {
	if (!surface)
		return;
	if (CR_STALE(surface, "crgpu_surface_destroy", "surface"))
		return;
	CR_KILL(surface);
	const CR_ALLOCATOR allocator = surface->head.allocator;
	cr_string_free(&allocator, surface->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(surface), &allocator, crgpu_surface_d3d12, surface);
}
}

#endif
