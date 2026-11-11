/*
 * crgpu_device_test — Carrottus GPU's adapters and devices.
 *
 * Lists the adapters, reads the first one's properties (chained structs
 * included), and creates and destroys devices on it: the argument checks of
 * every call, a device's queues by kind, features asked for and not, the
 * descriptor budget, one device per adapter, and a device made again after
 * one is destroyed. Everything goes through a checking allocator, which must
 * end with nothing left.
 *
 * Needs a GPU but no window, nobody watching and nothing drawn, so it is a
 * ctest; with no adapter it exits 77, which ctest reports as skipped.
 *
 * Exits 0 when every check passes, 1 when one fails.
 */

#include <crgpu.h>

#include "crtest_allocator.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define SKIPPED 77

static CRGPU_ADAPTER_PROPERTIES properties_of(crgpu_adapter adapter) {
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	return properties;
}

static crgpu_device_desc device_desc(void) {
	crgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "crgpu_device_test";
	return desc;
}

static void test_enumerate(crgpu_instance instance) {
	const crgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	CHECK(crgpu_adapter_enumerate(NULL, &adapters, &count) == CR_RESULT_GPU_INVALID_INSTANCE);
	CHECK(crgpu_adapter_enumerate(instance, NULL, &count) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_adapter_enumerate(instance, &adapters, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// The list is read once: every call hands out the same array.
	const crgpu_adapter *again = NULL;
	uint32_t again_count         = 0;
	CHECK(crgpu_adapter_enumerate(instance, &adapters, &count) == CR_SUCCESS);
	CHECK(crgpu_adapter_enumerate(instance, &again, &again_count) == CR_SUCCESS);
	CHECK(adapters == again);
	CHECK(count == again_count);
}

static void test_adapter_properties(crgpu_adapter adapter) {
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(NULL, &properties) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_adapter_get_properties(adapter, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	properties.base.type = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_WRONG_TYPE);
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;

	// A struct the call doesn't take there, anywhere in the chain, refuses
	// the whole call and writes nothing.
	CR_STRUCT_BASE_OUT stranger = {CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS, NULL};
	properties.base.next          = &stranger;
	properties.description[0]     = '\x7f';
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
	CHECK(properties.description[0] == '\x7f');
	properties.base.next = NULL;

#if CRGPU_EXT_MESH_SHADING
	CRGPU_MESH_SHADING_LIMITS mesh_a, mesh_b;
	memset(&mesh_a, 0, sizeof(mesh_a));
	memset(&mesh_b, 0, sizeof(mesh_b));
	mesh_a.base.type     = CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS;
	mesh_b.base.type     = CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS;
	mesh_a.base.next     = &mesh_b.base;
	properties.base.next = &mesh_a.base;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_RESULT_STRUCT_DUPLICATE_IN_CHAIN);

	// One limits struct: filled in, and the chain left as the caller built it.
	mesh_a.base.next = NULL;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	CHECK(properties.base.type == CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES);
	CHECK(properties.base.next == &mesh_a.base);
	CHECK(mesh_a.base.type == CR_STRUCT_TYPE_GPU_MESH_SHADING_LIMITS);
	CHECK(mesh_a.base.next == NULL);
	if (properties.supported_features & CRGPU_FEATURE_MESH_SHADING) {
		CHECK(mesh_a.max_mesh_output_vertices > 0);
		CHECK(mesh_a.max_mesh_workgroup_invocations > 0);
	} else {
		CHECK(mesh_a.max_mesh_output_vertices == 0);
	}
	properties.base.next = NULL;
#endif

	properties = properties_of(adapter);
	printf("adapter: %s, vendor 0x%04x, type %u, features 0x%llx, %u queue pools\n", properties.description,
	    (unsigned)properties.vendor_id, (unsigned)properties.adapter_type,
	    (unsigned long long)properties.supported_features, (unsigned)properties.queue_pool_count);
	CHECK(properties.description[0] != '\0');
	CHECK(properties.adapter_type <= CRGPU_ADAPTER_TYPE_SOFTWARE);

	// Some byte of the id says which GPU.
	bool id_nonzero = false;
	for (size_t i = 0; i < sizeof(properties.adapter_id); ++i)
		id_nonzero = id_nonzero || properties.adapter_id[i] != 0;
	CHECK(id_nonzero);

	// Every kind is in exactly one pool, and no pool is empty.
	CHECK(properties.queue_pool_count >= 1 && properties.queue_pool_count <= 3);
	CRGPU_QUEUE_KINDS seen = 0;
	for (uint32_t i = 0; i < properties.queue_pool_count && i < 3; ++i) {
		CHECK(properties.queue_pools[i].kinds != 0);
		CHECK((seen & properties.queue_pools[i].kinds) == 0);
		CHECK(properties.queue_pools[i].max_queues > 0);
		seen |= properties.queue_pools[i].kinds;
	}
	CHECK(seen == (CRGPU_QUEUE_KIND_DIRECT | CRGPU_QUEUE_KIND_COMPUTE | CRGPU_QUEUE_KIND_COPY));

	CHECK(properties.uniform_buffer_offset_alignment > 0);
	CHECK((properties.uniform_buffer_offset_alignment & (properties.uniform_buffer_offset_alignment - 1)) == 0);
	CHECK(properties.storage_buffer_offset_alignment > 0);
	CHECK(properties.max_draw_indirect_count >= 1);
	CHECK(properties.max_descriptors > 0);
	CHECK(properties.max_samplers > 0);
#if !CRGPU_EXT_BINDLESS
	CHECK(!(properties.supported_features & CRGPU_FEATURE_BINDLESS));
#endif
	// A device-mappable kind needs memory that is the system's.
	if (properties.supported_features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY)
		CHECK(properties.unified_memory);

	bool supported = true;
	CHECK(crgpu_adapter_supports_surface(NULL, NULL, &supported) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_adapter_supports_surface(adapter, NULL, &supported) == CR_RESULT_GPU_INVALID_SURFACE);
}

static void test_device_desc_checks(crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	crgpu_device device                       = (crgpu_device)(uintptr_t)1;
	crgpu_device_desc desc                    = device_desc();

	CHECK(crgpu_device_create(NULL, &desc, &device) == CR_RESULT_GPU_INVALID_ADAPTER);
	CHECK(crgpu_device_create(adapter, NULL, &device) == CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_STRUCT_WRONG_TYPE);
	desc = device_desc();
	CHECK(crgpu_device_create(adapter, &desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// Refused from here: the output is NULL.
	device                 = (crgpu_device)(uintptr_t)1;
	desc.required_features = UINT64_C(1) << 63;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
	CHECK(device == NULL);
	desc = device_desc();

	desc.direct_queue_count = 0;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_ZERO_SIZE);
	desc = device_desc();

	for (uint32_t i = 0; i < properties.queue_pool_count; ++i) {
		const CRGPU_QUEUE_POOL *pool = &properties.queue_pools[i];
		if (pool->max_queues == UINT32_MAX)
			continue;
		crgpu_device_desc too_many = device_desc();
		too_many.direct_queue_count  = 0;
		if (pool->kinds & CRGPU_QUEUE_KIND_DIRECT)
			too_many.direct_queue_count = pool->max_queues + 1;
		else if (pool->kinds & CRGPU_QUEUE_KIND_COMPUTE)
			too_many.compute_queue_count = pool->max_queues + 1;
		else
			too_many.copy_queue_count = pool->max_queues + 1;
		CHECK(crgpu_device_create(adapter, &too_many, &device) == CR_RESULT_GPU_TOO_MANY_QUEUES);
	}

	if (properties.max_descriptors < UINT32_MAX) {
		desc.max_descriptors = properties.max_descriptors + 1;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE);
		desc = device_desc();
	}
	if (properties.max_samplers < UINT32_MAX) {
		desc.max_samplers = properties.max_samplers + 1;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_DESCRIPTOR_BUDGET_TOO_LARGE);
		desc = device_desc();
	}

#if CRGPU_EXT_BINDLESS
	if (properties.supported_features & CRGPU_FEATURE_BINDLESS) {
		// The table comes out of the budget, which 16 doesn't hold.
		desc.required_features = CRGPU_FEATURE_BINDLESS;
		CHECK(crgpu_device_create(adapter, &desc, &device) == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
		desc = device_desc();
	}
#endif
}

static void test_queues(crgpu_device device) {
	crgpu_command_queue queue = NULL;
	crgpu_command_queue again = NULL;
	CHECK(crgpu_command_queue_get(NULL, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_BUNDLE, 0, &queue) ==
	      CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	CHECK(crgpu_command_queue_get(device, 99, 0, &queue) == CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);

	// Two direct queues, one compute, no copy queue.
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
	CHECK(queue != NULL);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &again) == CR_SUCCESS);
	CHECK(queue == again);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 1, &again) == CR_SUCCESS);
	CHECK(again != NULL && again != queue);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 2, &again) == CR_RESULT_INDEX_OUT_OF_RANGE);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_COMPUTE, 0, &again) == CR_SUCCESS);
	CHECK(again != NULL);
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_COPY, 0, &again) == CR_RESULT_INDEX_OUT_OF_RANGE);

	// An allocator of a kind the device has no queue of is refused; a bundle
	// allocator needs a direct queue.
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_COPY;
	crgpu_command_allocator allocator = NULL;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_RESULT_GPU_INVALID_COMMAND_LIST_TYPE);
	allocator_desc.type = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_SUCCESS);
	crgpu_command_list list = NULL;
	CHECK(crgpu_command_list_create(allocator, &list) == CR_SUCCESS);
	crgpu_command_list_destroy(list);
	crgpu_command_allocator_destroy(allocator);
}

static crgpu_descriptor_pool make_pool(crgpu_device device, CRGPU_DESCRIPTOR_TYPE type, uint32_t count, CR_RESULT *out_result) {
	crgpu_descriptor_pool_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                  = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	desc.max_sets                   = 1;
	desc.pool_size_count            = 1;
	desc.pool_sizes[0].descriptor_type = type;
	desc.pool_sizes[0].count        = count;
	crgpu_descriptor_pool pool    = NULL;
	*out_result                     = crgpu_descriptor_pool_create(device, &desc, &pool);
	return pool;
}

static void test_budget(crgpu_device device) {
	// The device's budget is 16 descriptors and 4 samplers.
	CR_RESULT result;
	crgpu_descriptor_pool first = make_pool(device, CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12, &result);
	CHECK(result == CR_SUCCESS);
	make_pool(device, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5, &result);
	CHECK(result == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
	crgpu_descriptor_pool second = make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, &result);
	CHECK(result == CR_SUCCESS);
	make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLER, 5, &result);
	CHECK(result == CR_RESULT_GPU_OUT_OF_DESCRIPTORS);
	crgpu_descriptor_pool samplers = make_pool(device, CRGPU_DESCRIPTOR_TYPE_SAMPLER, 4, &result);
	CHECK(result == CR_SUCCESS);

	// What a destroyed pool held is given back.
	crgpu_descriptor_pool_destroy(first);
	crgpu_descriptor_pool third = make_pool(device, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12, &result);
	CHECK(result == CR_SUCCESS);

	crgpu_descriptor_pool_destroy(third);
	crgpu_descriptor_pool_destroy(samplers);
	crgpu_descriptor_pool_destroy(second);
}

static void test_features(crgpu_adapter adapter, crgpu_device device) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	CHECK(crgpu_device_get_adapter(device) == adapter);
	CHECK(crgpu_device_get_adapter(NULL) == NULL);
	CHECK(crgpu_device_get_features(NULL) == 0);

	// The device has exactly what it was created with: here, nothing. A
	// feature the adapter has is still refused.
	CHECK(crgpu_device_get_features(device) == 0);
	if (properties.supported_features & CRGPU_FEATURE_PASS_TIMESTAMPS) {
		crgpu_command_queue queue = NULL;
		CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
		crgpu_timestamp_set set = NULL;
		CHECK(crgpu_timestamp_set_create(device, queue, 2, &set) == CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED);
	}
}

static void test_timestamps_when_asked(crgpu_adapter adapter) {
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	if (!(properties.supported_features & CRGPU_FEATURE_PASS_TIMESTAMPS))
		return;
	crgpu_device_desc desc = device_desc();
	desc.required_features   = CRGPU_FEATURE_PASS_TIMESTAMPS;
	crgpu_device device    = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	if (!device)
		return;
	CHECK(crgpu_device_get_features(device) == CRGPU_FEATURE_PASS_TIMESTAMPS);
	crgpu_command_queue queue = NULL;
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
	crgpu_timestamp_set set = NULL;
	CHECK(crgpu_timestamp_set_create(device, queue, 2, &set) == CR_SUCCESS);
	crgpu_timestamp_set_destroy(set);
	crgpu_device_destroy(device);
}

int main(void) {
	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);

	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "crgpu_device_test";
	instance_desc.engine_name      = "crgpu_device_test";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS) {
		fprintf(stderr, "crgpu_device_test: no instance\n");
		return 1;
	}

	test_enumerate(instance);
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	if (adapter_count == 0) {
		printf("crgpu_device_test: no adapter, skipped\n");
		crgpu_instance_destroy(instance);
		return SKIPPED;
	}
	const crgpu_adapter adapter = adapters[0];

	test_adapter_properties(adapter);
	test_device_desc_checks(adapter);

	// A device of two direct queues and one compute queue, no copy queue,
	// no features, a budget of 16 descriptors and 4 samplers.
	const CRGPU_ADAPTER_PROPERTIES properties = properties_of(adapter);
	crgpu_device_desc desc                    = device_desc();
	desc.direct_queue_count                     = 2;
	desc.compute_queue_count                    = 1;
	// Where compute shares the direct queues' pool, the pool must hold three.
	bool fits = true;
	for (uint32_t i = 0; i < properties.queue_pool_count; ++i) {
		const CRGPU_QUEUE_POOL *pool = &properties.queue_pools[i];
		uint64_t asked                 = 0;
		if (pool->kinds & CRGPU_QUEUE_KIND_DIRECT)
			asked += 2;
		if (pool->kinds & CRGPU_QUEUE_KIND_COMPUTE)
			asked += 1;
		fits = fits && asked <= pool->max_queues;
	}
	if (!fits) {
		printf("crgpu_device_test: the adapter's pools can't hold 2 direct and 1 compute queue; skipped the queue tests\n");
		desc.direct_queue_count  = 1;
		desc.compute_queue_count = 0;
	}

	crgpu_device device = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	if (device) {
		// One device per adapter at a time.
		crgpu_device second = (crgpu_device)(uintptr_t)1;
		CHECK(crgpu_device_create(adapter, &desc, &second) == CR_RESULT_GPU_ADAPTER_IN_USE);
		CHECK(second == NULL);

		if (fits)
			test_queues(device);
		test_budget(device);
		test_features(adapter, device);
		crgpu_device_destroy(device);
	}
	crgpu_device_destroy(NULL);

	// The adapter is free again: a device is made on it as before, which is
	// also the way back from a lost device.
	test_timestamps_when_asked(adapter);
	device = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	crgpu_device_destroy(device);

	CHECK(crgpu_instance_destroy(instance) == CR_SUCCESS);
	CHECK(crtest_allocator_clean(&tracker));

	if (failures)
		fprintf(stderr, "crgpu_device_test: %d checks failed\n", failures);
	else
		printf("crgpu_device_test: all checks passed\n");
	return failures ? 1 : 0;
}
