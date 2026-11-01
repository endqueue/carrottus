
#if SPUDGPU_COMPILE_METAL_API

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "spudgpumetal.h"
#include "../../spudgpushared.h"
#include "../../../spudstructbase.h"

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

// Fills in what [adapter] is and can do, from its MTLDevice. Called once,
// when the instance is made; nothing changes it after.
static void spudgpumetal___internal_read_adapter_facts(spudgpu_adapter_metal *adapter) {
	id<MTLDevice> mtl                       = adapter->_device_mtl;
	spudgpu___adapter_facts *facts          = &adapter->_facts;
	SPUDGPU_ADAPTER_PROPERTIES *properties  = &facts->properties;
	properties->base                        = spud_struct_base_out(SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);

	const char *name = [mtl.name UTF8String];
	size_t name_len  = strnlen(name, sizeof(properties->description) - 1);
	memcpy(properties->description, name, name_len);
	properties->description[name_len] = '\0';

	// Metal has no PCI-style vendor or device ID: every GPU it lists on the
	// platforms SpudGPU builds for is an Apple GPU.
	properties->vendor_id    = SPUDGPU_VENDOR_APPLE;
	properties->device_id    = 0;
	properties->subsystem_id = 0;
	properties->revision     = 0;

	// registryID names the GPU across processes; it is an IORegistry entry
	// ID, so a restart or a replug may give the GPU another.
	const uint64_t registry_id = mtl.registryID;
	memset(properties->adapter_id, 0, sizeof(properties->adapter_id));
	memcpy(properties->adapter_id, &registry_id, sizeof(registry_id));
	memset(properties->luid, 0, sizeof(properties->luid));
	properties->luid_valid = false;

	// isRemovable would tell an eGPU apart, but it is macOS only and
	// deprecated (no eGPU on Apple Silicon): a GPU whose memory is the
	// system's is integrated, any other discrete.
	const bool unified       = mtl.hasUnifiedMemory;
	properties->adapter_type = unified ? SPUDGPU_ADAPTER_TYPE_INTEGRATED : SPUDGPU_ADAPTER_TYPE_DISCRETE;

	// Apple Silicon GPUs share the system's memory, so "dedicated video
	// memory" doesn't apply the way it does to a discrete Vulkan or D3D12
	// card: the working-set budget is reported under whichever bucket matches
	// this GPU's memory.
	const uint64_t working_set           = mtl.recommendedMaxWorkingSetSize;
	properties->dedicated_video_memory  = unified ? 0 : working_set;
	properties->dedicated_system_memory = 0;
	properties->shared_system_memory    = unified ? working_set : 0;
	properties->unified_memory          = unified;

	// Metal has no query for a buffer offset's alignment. Its feature set
	// tables ask at most 256 bytes of a constant buffer's offset, so 256 is
	// valid on every GPU; a storage buffer is held to the same.
	properties->uniform_buffer_offset_alignment = 256;
	properties->storage_buffer_offset_alignment = 256;
	// spudgpu_cmd_draw_indirect loops one indirect draw per entry, so any
	// count is recorded.
	properties->max_draw_indirect_count = UINT32_MAX;
	// Argument buffers have no device-wide count to keep under.
	properties->max_descriptors = UINT32_MAX;
	properties->max_samplers    = UINT32_MAX;

	// Any MTLCommandQueue encodes any work: one pool per kind, as many as
	// asked.
	properties->queue_pool_count           = 3;
	properties->queue_pools[0].kinds       = SPUDGPU_QUEUE_KIND_DIRECT;
	properties->queue_pools[1].kinds       = SPUDGPU_QUEUE_KIND_COMPUTE;
	properties->queue_pools[2].kinds       = SPUDGPU_QUEUE_KIND_COPY;
	for (uint32_t i = 0; i < 3; i++) {
		properties->queue_pools[i].max_queues = UINT32_MAX;
		properties->queue_pools[i].engine     = SPUDGPU_QUEUE_POOL_ENGINE_UNKNOWN;
	}

	SPUDGPU_FEATURES features = 0;
	// MTLRenderPassDescriptor.renderTargetArrayLength: Apple5 and later and
	// every Mac family, by Metal's feature set tables, so every Metal 3 GPU
	// this backend lists (Apple7 and later, Mac2).
	features |= SPUDGPU_FEATURE_LAYERED_RENDERING;
	// setDepthBias:slopeScale:clamp: takes a clamp, each colour attachment of
	// a render pipeline descriptor blends on its own, an indirect draw loops
	// any count and reads its arguments' baseInstance (Apple3 and Mac2 and
	// later), and MTLTextureTypeCubeArray exists, on every GPU.
	features |= SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP | SPUDGPU_FEATURE_INDEPENDENT_BLEND |
	            SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT | SPUDGPU_FEATURE_CUBE_ARRAY_VIEWS |
	            SPUDGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
	// A pass's timestamps are samples of the timestamp counter set at its
	// stage boundaries.
	id<MTLCounterSet> timestamp_counter_set = spudgpumetal___internal_timestamp_counter_set(mtl);
	if (timestamp_counter_set != nil && [mtl supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
		features |= SPUDGPU_FEATURE_PASS_TIMESTAMPS;
	[timestamp_counter_set release];
	// SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE is Shared storage, which is the
	// GPU's own memory only where that memory is the system's.
	if (unified)
		features |= SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
	// A heap of MTLStorageModeShared, which every mappable kind is, can be
	// made on an Apple GPU and not on any other. Each MTLGPUFamilyApple
	// family includes the ones before it, so Apple1 is every Apple GPU.
	if ([mtl supportsFamily:MTLGPUFamilyApple1])
		features |= SPUDGPU_FEATURE_MAPPABLE_HEAPS;
	// Metal 3 mesh shading, on every GPU this backend lists.
	features |= SPUDGPU_FEATURE_MESH_SHADING;
	// setDepthTestMinBound:maxBound: is MTLGPUFamilyApple10 only.
	if ([mtl supportsFamily:MTLGPUFamilyApple10])
		features |= SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST;
	// Every GPU presents to a CAMetalLayer.
	features |= SPUDGPU_FEATURE_PRESENT;
	// Acceleration structures and intersection queries with indirect
	// instances, in compute (supportsRaytracing) and fragment
	// (supportsRaytracingFromRender) functions, on an Apple7 or later GPU.
	// Indirect instance descriptors (MTLIndirectAccelerationStructureInstanceDescriptor)
	// need macOS 14 / iOS 17: on an older OS the bit is never set, whatever
	// the build's deployment target, and no ray query call is reached.
	// Metal has no query for the limits: the narrower of Apple's documented
	// ones.
	bool ray_query = false;
	if (@available(macOS 14.0, iOS 17.0, *))
		ray_query = mtl.supportsRaytracing && mtl.supportsRaytracingFromRender && [mtl supportsFamily:MTLGPUFamilyApple7];
	if (ray_query) {
		features |= SPUDGPU_FEATURE_RAY_QUERY;
		facts->ray_query_max_geometries           = (1u << 24) - 1u;
		facts->ray_query_max_instances            = (1u << 24) - 1u;
		facts->ray_query_max_primitives           = (1u << 28) - 1u;
		facts->ray_query_scratch_offset_alignment = 256;
		// The alignment Metal gives a structure in a heap, asked for the
		// smallest one (1,024 bytes for every size on an Apple M5 Pro, macOS
		// 27, 2026-11-01). Checked again against the real one for its size
		// when a structure is placed.
		facts->ray_query_placement_alignment = (uint64_t)[mtl heapAccelerationStructureSizeAndAlignWithSize:1].align;
	}
	// No SPUDGPU_FEATURE_BINDLESS: SPUDGPU_EXT_BINDLESS is 0 on Metal.
	properties->supported_features = features;

	// Metal has no query for a mesh shader's output limits: its own limit is
	// a pipeline's maxTotalThreadsPerMeshThreadgroup. These are the meshlet
	// limits the mesh shading samples are written to, not a queried ceiling.
	facts->mesh_max_output_vertices       = 256;
	facts->mesh_max_output_primitives     = 256;
	facts->mesh_max_workgroup_invocations = 1024;
}

static void spudgpumetal___internal_destroy_adapters(spudgpu_instance_metal *instance) {
	for (uint32_t i = 0; i < instance->_adapter_count; i++) {
		spudgpu_adapter_metal *adapter = (spudgpu_adapter_metal *)instance->_adapters[i];
		if (!adapter)
			continue;
		SPUD_KILL(adapter);
		[adapter->_device_mtl release];
		const SPUD_ALLOCATOR allocator = adapter->head.allocator;
		spud_string_free(&allocator, adapter->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_adapter_metal, adapter);
	}
	SPUD_DELETE_ARRAY(&instance->head.allocator, spudgpu_adapter, instance->_adapters, instance->_adapter_count);
	instance->_adapters      = NULL;
	instance->_adapter_count = 0;
}

// Lists the GPUs MTLCopyAllDevices gives that are Metal 3, in its order.
static SPUDRESULT spudgpumetal___internal_list_adapters(spudgpu_instance_metal *instance) {
	NSArray<id<MTLDevice> > *mtl_devices = MTLCopyAllDevices();
	const uint32_t gpu_count             = (uint32_t)mtl_devices.count;
	uint32_t listed                      = 0;
	for (uint32_t i = 0; i < gpu_count; i++)
		if ([mtl_devices[i] supportsFamily:MTLGPUFamilyMetal3])
			listed++;
	if (listed == 0) {
		[mtl_devices release];
		return SPUD_SUCCESS;
	}

	instance->_adapters = SPUD_NEW_ARRAY_ZEROED(&instance->head.allocator, spudgpu_adapter, listed);
	if (!instance->_adapters) {
		[mtl_devices release];
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	instance->_adapter_count = listed;

	uint32_t next = 0;
	for (uint32_t i = 0; i < gpu_count; i++) {
		if (![mtl_devices[i] supportsFamily:MTLGPUFamilyMetal3])
			continue;
		spudgpu_adapter_metal *adapter = SPUD_NEW_ZEROED(&instance->head.allocator, spudgpu_adapter_metal);
		if (!adapter) {
			spudgpumetal___internal_destroy_adapters(instance);
			[mtl_devices release];
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		adapter->head.allocator = instance->head.allocator;
		SPUD_BIRTH(adapter);
		adapter->_device_mtl    = [mtl_devices[i] retain];
		atomic_init(&adapter->_in_use, false);
		spudgpumetal___internal_read_adapter_facts(adapter);
		instance->_adapters[next++] = (spudgpu_adapter)adapter;
	}
	[mtl_devices release];
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_instance(
    const spudgpu_instance_desc *desc,
    spudgpu_instance *out_instance) {
	const SPUDRESULT desc_sr = spudgpu___internal_check_instance_desc(desc, out_instance);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	// Metal has no application/engine name or instance-level version info to
	// hand anywhere (unlike VkApplicationInfo) - desc's are accepted for API
	// symmetry with the Vulkan/D3D12 backends and otherwise unused.

	spudgpu_instance_metal *instance = SPUD_NEW_ZEROED(desc->allocator, spudgpu_instance_metal);
	if (!instance)
		return SPUDRESULT_OUT_OF_MEMORY;
	instance->head.allocator = *desc->allocator;
	SPUD_BIRTH(instance);

	const SPUDRESULT list_sr = spudgpumetal___internal_list_adapters(instance);
	if (list_sr != SPUD_SUCCESS) {
		SPUD_DELETE(desc->allocator, spudgpu_instance_metal, instance);
		return list_sr;
	}

	*out_instance = instance;
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_destroy_instance(spudgpu_instance instance) {
	if (!instance)
		return SPUD_SUCCESS;
	if (SPUD_STALE(instance, "spudgpu_destroy_instance", "instance"))
		return SPUD_SUCCESS;
	spudgpumetal___internal_destroy_adapters(instance);
	SPUD_KILL(instance);
	const SPUD_ALLOCATOR allocator = instance->head.allocator;
	spud_string_free(&allocator, instance->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_instance_metal, instance);
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
	// Any GPU presents to any CAMetalLayer.
	*out_supported = (adapter->_facts.properties.supported_features & SPUDGPU_FEATURE_PRESENT) != 0;
	return SPUD_SUCCESS;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

static void spudgpumetal___internal_destroy_queues(spudgpu_device_metal *device, spudgpu_command_queue *queues, uint32_t count) {
	if (!queues)
		return;
	for (uint32_t i = 0; i < count; i++) {
		spudgpu_command_queue_metal *queue = (spudgpu_command_queue_metal *)queues[i];
		if (!queue)
			continue;
		SPUD_KILL(queue);
		[queue->_command_queue_mtl release];
		const SPUD_ALLOCATOR allocator = queue->head.allocator;
		spud_string_free(&allocator, queue->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_command_queue_metal, queue);
	}
	SPUD_DELETE_ARRAY(&device->head.allocator, spudgpu_command_queue, queues, count);
}

// Makes [count] queues into a new array at [*out_queues]: all or none.
static SPUDRESULT spudgpumetal___internal_create_queues(
    spudgpu_device_metal *device,
    uint32_t count,
    spudgpu_command_queue **out_queues) {
	*out_queues = NULL;
	if (count == 0)
		return SPUD_SUCCESS;
	spudgpu_command_queue *queues = SPUD_NEW_ARRAY_ZEROED(&device->head.allocator, spudgpu_command_queue, count);
	if (!queues)
		return SPUDRESULT_OUT_OF_MEMORY;
	for (uint32_t i = 0; i < count; i++) {
		spudgpu_command_queue_metal *queue = SPUD_NEW_ZEROED(&device->head.allocator, spudgpu_command_queue_metal);
		if (!queue) {
			spudgpumetal___internal_destroy_queues(device, queues, count);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
		queue->head.allocator     = device->head.allocator;
		SPUD_BIRTH(queue);
		queue->_parent_device     = device;
		queue->_command_queue_mtl = [device->_device_mtl newCommandQueue];
		queues[i]                 = (spudgpu_command_queue)queue;
		if (!queue->_command_queue_mtl) {
			spudgpumetal___internal_destroy_queues(device, queues, count);
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}
	}
	*out_queues = queues;
	return SPUD_SUCCESS;
}

// Frees [device] and everything it owns, and gives its adapter back. Every
// member is made or NULL/0, so a device that failed part way is freed too.
static void spudgpumetal___internal_free_device(spudgpu_device_metal *device, bool budget_made) {
	spudgpumetal___internal_destroy_queues(device, device->_cmd_queues_direct, device->_cmd_queue_count_direct);
	spudgpumetal___internal_destroy_queues(device, device->_cmd_queues_compute, device->_cmd_queue_count_compute);
	spudgpumetal___internal_destroy_queues(device, device->_cmd_queues_copy, device->_cmd_queue_count_copy);
	if (budget_made)
		spudgpu___internal_budget_destroy(&device->_budget);
	[device->_device_mtl release];
	atomic_store(&device->_adapter->_in_use, false);
	const SPUD_ALLOCATOR allocator = device->head.allocator;
	spud_string_free(&allocator, device->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_device_metal, device);
}

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
	if (atomic_exchange(&adapter->_in_use, true))
		return SPUDRESULT_GPU_ADAPTER_IN_USE;

	spudgpu_device_metal *device = SPUD_NEW_ZEROED(&adapter->head.allocator, spudgpu_device_metal);
	if (!device) {
		atomic_store(&adapter->_in_use, false);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	device->head.allocator = adapter->head.allocator;
	SPUD_BIRTH(device);
	device->_adapter       = adapter;
	device->_device_mtl    = [adapter->_device_mtl retain];
	device->_features      = desc->required_features;

	if (!spudgpu___internal_budget_init(&device->_budget, desc->max_descriptors, desc->max_samplers)) {
		spudgpumetal___internal_free_device(device, false);
		return SPUDRESULT_GENERAL_FAILURE;
	}
#if _DEBUG
	if (!spudgpu___internal_live_init(&device->_live)) {
		spudgpumetal___internal_free_device(device, true);
		return SPUDRESULT_GENERAL_FAILURE;
	}
#endif

	SPUDRESULT sr = spudgpumetal___internal_create_queues(device, desc->direct_queue_count, &device->_cmd_queues_direct);
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_direct = desc->direct_queue_count;
		sr = spudgpumetal___internal_create_queues(device, desc->compute_queue_count, &device->_cmd_queues_compute);
	}
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_compute = desc->compute_queue_count;
		sr = spudgpumetal___internal_create_queues(device, desc->copy_queue_count, &device->_cmd_queues_copy);
	}
	if (sr == SPUD_SUCCESS) {
		device->_cmd_queue_count_copy = desc->copy_queue_count;
		if (spud_debug_name_set(device, desc->debug_name) != SPUD_SUCCESS)
			sr = SPUDRESULT_OUT_OF_MEMORY;
	}
	if (sr != SPUD_SUCCESS) {
#if _DEBUG
		spudgpu___internal_live_destroy(&device->_live);
#endif
		spudgpumetal___internal_free_device(device, true);
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
	spudgpumetal___internal_free_device(device, true);
}

spudgpu_adapter spudgpu_get_device_adapter(spudgpu_device device) {
	if (!device) return NULL;
	if (SPUD_STALE(device, "spudgpu_get_device_adapter", "device")) return NULL;
	return (spudgpu_adapter)device->_adapter;
}

SPUDGPU_FEATURES spudgpu_get_device_features(spudgpu_device device) {
	if (!device) return 0;
	if (SPUD_STALE(device, "spudgpu_get_device_features", "device")) return 0;
	return device->_features;
}

SPUDGPU_NATIVE_API spudgpu_get_native_gpu_api(spudgpu_instance instance) {
	if (!instance) return SPUDGPU_NATIVE_API_NONE;
	if (SPUD_STALE(instance, "spudgpu_get_native_gpu_api", "instance")) return SPUDGPU_NATIVE_API_NONE;
	return SPUDGPU_NATIVE_API_METAL;
}

#endif // SPUDGPU_COMPILE_METAL_API
