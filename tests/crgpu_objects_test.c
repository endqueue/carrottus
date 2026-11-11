/*
 * crgpu_objects_test — Carrottus GPU calls nothing else in the workspace makes.
 *
 * On a device of the adapter asked for (0 by default), with nothing drawn:
 * a format looked up by name; a fence signalled from the CPU and waited on; a semaphore made
 * and destroyed; a buffer's desc and GPU address read back; a buffer view's
 * desc; a heap's desc; and a descriptor pool reset and allocated from again.
 * Each with its argument checks. Every block of the instance goes through a
 * counting allocator, checked at the end.
 *
 * Then, on a second device of the same adapter made with
 * CRGPU_FEATURE_PRESENT where it has it: the present scaling checks of
 * crgpu_surface_get_present_scalings and crgpu_swap_chain_create that
 * come before a surface is read, given a surface handle that is only a
 * head (not_a_surface). On Metal, where a surface is a CAMetalLayer and needs no
 * window, also the query's answer, a swap chain of each scaling, the gravity
 * each sets on the layer (a flipped layer's included) and the scaling read
 * back from the swap chain's desc.
 *
 * Then, on a third device with the optional features the adapter has of
 * those a frame can use, a frame drawn into an image and read back: every
 * draw call, the descriptor, push constant, copy, blit, barrier, clear,
 * placed-resource and timestamp calls, a mesh shader and the depth bounds
 * where the device has them, each call's work checked by the pixels it left
 * (record_frame, below, says which cell is whose). Then a compute shader
 * doubling a buffer, and its pipeline's desc read back. The shaders are
 * shaders built from tests/shaders/ (tests/crgpu_test_shaders.h).
 *
 * And, on the device made with CRGPU_FEATURE_PRESENT, three frames
 * presented through a swap chain on a real surface that nobody sees: a
 * hidden window on Windows, a CAMetalLayer with no window on Metal; on
 * Vulkan on Windows also a surface made by
 * crgpu_surface_create_from_callback with a callback that calls
 * vkCreateWin32SurfaceKHR. Linux has no surface without a display here, and
 * skips the frames.
 *
 * And, on every adapter, every ray query call (CRGPU_EXT_RAY_QUERY) refused
 * on the first device, made without CRGPU_FEATURE_RAY_QUERY; on an adapter
 * with it, a device made with it: bottom levels of a triangle and of an
 * AABB, a top level of both, rq_trace.slang's grid of rays read back against
 * the hits worked out here, a refit, compaction, a clone, instances written
 * by the CPU call and by a compute shader, rq_shadow.slang drawn, a placed
 * structure, and each call's refusals in order (test_ray_queries).
 *
 * Not here, because they can't run on every backend: the storage-buffer and
 * storage-image bindless calls and crgpu_command_list_bind_compute_bindless_resources
 * (CRGPU_EXT_BINDLESS, 0 on Metal).
 *
 *   crgpu_objects_test [--adapter N]
 *
 * --adapter picks the adapter, by its index in crgpu_adapter_enumerate();
 * 0 by default. Exits 0 when every check passes, 1 when one fails, 77
 * (skipped) with no adapter.
 */

#include <crgpu.h>

#include "../src/crallocator.h"
#include "crgpu_test_shaders.h"
#include "crtest_allocator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if CRGPU_COMPILE_METAL_API
#include "crgpu_test_metal_layer.h"
#endif

#define SKIPPED 77

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

static void test_format_from_string(void) {
	CHECK(crgpu_format_from_string(NULL) == CRGPU_FORMAT_UNKNOWN);
	CHECK(crgpu_format_from_string("") == CRGPU_FORMAT_UNKNOWN);
	CHECK(crgpu_format_from_string("not a format") == CRGPU_FORMAT_UNKNOWN);
	/* A name is the enumerator's without its CRGPU_FORMAT_ prefix. */
	CHECK(crgpu_format_from_string("R8G8B8A8_UNORM") == CRGPU_FORMAT_R8G8B8A8_UNORM);
	CHECK(crgpu_format_from_string("B8G8R8A8_UNORM") == CRGPU_FORMAT_B8G8R8A8_UNORM);
	CHECK(crgpu_format_from_string("D32_FLOAT") == CRGPU_FORMAT_D32_FLOAT);
	CHECK(crgpu_format_from_string("CRGPU_FORMAT_R8G8B8A8_UNORM") == CRGPU_FORMAT_UNKNOWN);
	CHECK(crgpu_format_from_string("r8g8b8a8_unorm") == CRGPU_FORMAT_UNKNOWN);
}

static void test_fence_signal(crgpu_device device) {
	crgpu_fence fence = NULL;
	CHECK(crgpu_fence_create(device, CRGPU_FENCE_FLAG_NONE, 3, &fence) == CR_SUCCESS);
	if (!fence)
		return;
	CHECK(crgpu_fence_get_value(fence) == 3);
	CHECK(crgpu_fence_signal(NULL, fence, 4) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_fence_signal(device, NULL, 4) == CR_RESULT_GPU_INVALID_FENCE);
	CHECK(crgpu_fence_signal(device, fence, 7) == CR_SUCCESS);
	CHECK(crgpu_fence_get_value(fence) == 7);
	/* A wait for the signalled value returns at once; a higher one times out. */
	uint64_t value = 7;
	CHECK(crgpu_fence_wait(device, &fence, &value, 1, true, 0) == CR_SUCCESS);
	value = 8;
	CHECK(crgpu_fence_wait(device, &fence, &value, 1, true, 0) == CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT);
	CHECK(crgpu_fence_signal(device, fence, 8) == CR_SUCCESS);
	CHECK(crgpu_fence_wait(device, &fence, &value, 1, true, 1000000000ull) == CR_SUCCESS);
	crgpu_fence_destroy(fence);
}

static void test_semaphore(crgpu_device device) {
	crgpu_semaphore semaphore = NULL;
	CHECK(crgpu_semaphore_create(NULL, &semaphore) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_semaphore_create(device, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_semaphore_create(device, &semaphore) == CR_SUCCESS);
	CHECK(semaphore != NULL);
	crgpu_semaphore_destroy(semaphore);
	crgpu_semaphore_destroy(NULL);
}

static void test_buffer_and_view(crgpu_device device) {
	char name[32];
	strcpy(name, "objects buffer");
	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage       = CRGPU_BUFFER_USAGE_UNIFORM | CRGPU_BUFFER_USAGE_TRANSFER_SRC;
	desc.memory_kind = CRGPU_MEMORY_KIND_UPLOAD;
	desc.size        = 4096;
	desc.debug_name  = name;
	crgpu_buffer buffer = NULL;
	CHECK(crgpu_buffer_create(device, &desc, &buffer) == CR_SUCCESS);
	if (!buffer)
		return;
	strcpy(name, "changed");

	crgpu_buffer_desc read;
	memset(&read, 0xA5, sizeof(read));
	CHECK(crgpu_buffer_get_desc(NULL, &read) == CR_RESULT_GPU_INVALID_BUFFER);
	CHECK(crgpu_buffer_get_desc(buffer, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_buffer_get_desc(buffer, &read) == CR_SUCCESS);
	CHECK(read.base.type == CR_STRUCT_TYPE_GPU_BUFFER_DESC);
	CHECK(read.usage == desc.usage);
	CHECK(read.memory_kind == desc.memory_kind);
	CHECK(read.size == desc.size);
	/* The buffer's own copy of its name, not the caller's string. */
	CHECK(read.debug_name != NULL && read.debug_name != name && strcmp(read.debug_name, "objects buffer") == 0);

	uint64_t address = 0;
	CHECK(crgpu_buffer_get_gpu_address(NULL, &address) == CR_RESULT_GPU_INVALID_BUFFER);
	CHECK(crgpu_buffer_get_gpu_address(buffer, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_buffer_get_gpu_address(buffer, &address) == CR_SUCCESS);
	CHECK(address != 0);
	uint64_t again = 0;
	CHECK(crgpu_buffer_get_gpu_address(buffer, &again) == CR_SUCCESS);
	CHECK(again == address);

	crgpu_buffer_view_desc view_desc;
	memset(&view_desc, 0, sizeof(view_desc));
	view_desc.base.type                 = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	view_desc.parent_buffer             = buffer;
	view_desc.offset_from_parent_buffer = 256;
	view_desc.stride                    = 16;
	view_desc.size                      = 512;
	crgpu_buffer_view view            = NULL;
	CHECK(crgpu_buffer_view_create(buffer, &view_desc, &view) == CR_SUCCESS);
	if (view) {
		crgpu_buffer_view_desc view_read;
		memset(&view_read, 0xA5, sizeof(view_read));
		CHECK(crgpu_buffer_view_get_desc(NULL, &view_read) == CR_RESULT_GPU_INVALID_BUFFER_VIEW);
		CHECK(crgpu_buffer_view_get_desc(view, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_buffer_view_get_desc(view, &view_read) == CR_SUCCESS);
		CHECK(view_read.parent_buffer == buffer);
		CHECK(view_read.offset_from_parent_buffer == 256);
		CHECK(view_read.stride == 16);
		CHECK(view_read.size == 512);
		crgpu_buffer_view_destroy(view);
	}
	crgpu_buffer_destroy(buffer);
}

/* Card X27 (src/crstale.h): in a Debug build a destroyed buffer's memory
 * stays its device's until the device is destroyed, so the destroyed-handle
 * check reads CR_DEAD however many buffers of the same size are made after
 * it; the device frees every one when it goes, and its own block stays its
 * instance's. Debug only: Release has no check, and frees at once. */
static void test_quarantine(crgpu_adapter adapter, const crtest_allocator *tracker) {
#if defined(_DEBUG) && _DEBUG
	enum { ROUNDS = 256 };
	const size_t before = tracker->live_blocks;
	crgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.direct_queue_count = 1;
	crgpu_device device          = NULL;
	CHECK(crgpu_device_create(adapter, &device_desc, &device) == CR_SUCCESS);
	if (!device)
		return;

	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = CRGPU_BUFFER_USAGE_UNIFORM;
	desc.memory_kind      = CRGPU_MEMORY_KIND_UPLOAD;
	desc.size             = 256;
	crgpu_buffer dead   = NULL;
	CHECK(crgpu_buffer_create(device, &desc, &dead) == CR_SUCCESS);
	if (!dead) {
		crgpu_device_destroy(device);
		return;
	}
	crgpu_buffer_destroy(dead);
	const size_t held = tracker->live_blocks;
	for (int i = 0; i < ROUNDS; ++i) {
		crgpu_buffer buffer = NULL;
		CHECK(crgpu_buffer_create(device, &desc, &buffer) == CR_SUCCESS);
		CHECK(buffer != dead);
		crgpu_buffer_destroy(buffer);
	}
	crgpu_buffer_desc read;
	memset(&read, 0, sizeof(read));
	CHECK(crgpu_buffer_get_desc(dead, &read) == CR_RESULT_GPU_INVALID_BUFFER);
	/* Every destroyed buffer's block is still the device's. */
	CHECK(tracker->live_blocks >= held + ROUNDS);
	crgpu_device_destroy(device);
	/* All of them freed with it; the device's own block is the instance's. */
	CHECK(tracker->live_blocks == before + 1);
#else
	(void)adapter;
	(void)tracker;
#endif
}

static void test_heap_desc(crgpu_device device) {
	crgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = CR_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = CRGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	desc.size           = CRGPU_HEAP_SIZE_GRANULARITY * 4;
	desc.debug_name     = "objects heap";
	crgpu_heap heap   = NULL;
	CHECK(crgpu_heap_create(device, &desc, &heap) == CR_SUCCESS);
	if (!heap)
		return;
	crgpu_heap_desc read;
	memset(&read, 0xA5, sizeof(read));
	CHECK(crgpu_heap_get_desc(NULL, &read) == CR_RESULT_GPU_INVALID_HEAP);
	CHECK(crgpu_heap_get_desc(heap, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_heap_get_desc(heap, &read) == CR_SUCCESS);
	CHECK(read.memory_kind == desc.memory_kind);
	CHECK(read.resource_class == desc.resource_class);
	CHECK(read.size == desc.size);
	CHECK(read.debug_name != NULL && strcmp(read.debug_name, "objects heap") == 0);
	crgpu_heap_destroy(heap);
}

static void test_descriptor_pool_reset(crgpu_device device) {
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_VERTEX | CRGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 1;
	crgpu_descriptor_set_layout layout    = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &layout) == CR_SUCCESS);
	if (!layout)
		return;

	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 2;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_layouts[1] = layout;
		set_desc.set_count      = 2;
		/* The pool's whole capacity, three times over: a reset gives it all
		 * back each time. */
		for (int round = 0; round < 3; ++round) {
			crgpu_descriptor_set sets[2] = {NULL, NULL};
			CHECK(crgpu_descriptor_set_create(device, &set_desc, sets) == CR_SUCCESS);
			CHECK(sets[0] != NULL && sets[1] != NULL && sets[0] != sets[1]);
			crgpu_descriptor_pool_reset(pool);
		}
		crgpu_descriptor_pool_reset(NULL);
		crgpu_descriptor_pool_destroy(pool);
	}
	crgpu_descriptor_set_layout_destroy(layout);
}

// A swap chain desc with every field but the scaling set to what any
// backend takes; the queue is the device's first direct one.
static crgpu_swap_chain_desc swap_chain_desc_for(crgpu_device device, crgpu_surface surface, CRGPU_PRESENT_SCALING scaling) {
	crgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &desc.queue);
	desc.width           = 64;
	desc.height          = 64;
	desc.buffer_count    = 1;
	desc.format          = CRGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode    = CRGPU_PRESENT_MODE_FIFO;
	desc.present_scaling = scaling;
	desc.fullscreen_mode = CRGPU_FULLSCREEN_MODE_WINDOWED;
	return desc;
}

// A surface Carrottus GPU never made, for the checks that come before a surface
// is read: a handle's head and nothing else, marked live so that a Debug
// build's check for a destroyed handle (src/crstale.h) lets it through to
// them.
static crgpu_surface not_a_surface(void) {
	static cr_object_head head;
	CR_BIRTH(&head);
	return (crgpu_surface)(void *)&head;
}

// A device made without CRGPU_FEATURE_PRESENT: both calls refuse it, after
// their argument checks.
static void test_present_scaling_without_present(crgpu_device device) {
	crgpu_surface surface = not_a_surface();
	uint32_t scalings       = 0;
	CHECK(crgpu_surface_get_present_scalings(device, surface, &scalings) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
	crgpu_swap_chain_desc desc = swap_chain_desc_for(device, surface, CRGPU_PRESENT_SCALING_NONE);
	crgpu_swap_chain swap_chain = NULL;
	// The feature is checked before the scaling.
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_FEATURE_NOT_SUPPORTED);
	CHECK(swap_chain == NULL);
}

// [device] has CRGPU_FEATURE_PRESENT. Every check here comes before the
// surface is read, so the surface handle is a pointer to nothing Carrottus GPU
// knows but a live head (not_a_surface).
static void test_present_scaling_checks(crgpu_device device) {
	crgpu_surface surface = not_a_surface();
	uint32_t scalings       = 0xdeadu;
	CHECK(crgpu_surface_get_present_scalings(NULL, surface, &scalings) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_surface_get_present_scalings(device, NULL, &scalings) == CR_RESULT_GPU_INVALID_SURFACE);
	CHECK(crgpu_surface_get_present_scalings(device, surface, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_surface_get_present_scalings(device, NULL, NULL) == CR_RESULT_GPU_INVALID_SURFACE);
	CHECK(scalings == 0xdeadu);

	crgpu_swap_chain swap_chain = NULL;
	crgpu_swap_chain_desc desc  = swap_chain_desc_for(device, surface, CRGPU_PRESENT_SCALING_NONE);
	CHECK(desc.queue != NULL);
	CHECK(crgpu_swap_chain_create(NULL, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_DEVICE);
	// A zeroed desc has not chosen.
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_PRESENT_SCALING);
	// Exactly one value, and only a value the enum has.
	desc.present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE | CRGPU_PRESENT_SCALING_STRETCH;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_PRESENT_SCALING);
	desc.present_scaling = CRGPU_PRESENT_SCALING_ASPECT_FIT << 1;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_PRESENT_SCALING);
	desc.present_scaling = 0xffffffffu;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_PRESENT_SCALING);
	// The checks before it come first: the output, the surface, the queue.
	desc.present_scaling = CRGPU_PRESENT_SCALING_NONE;
	CHECK(crgpu_swap_chain_create(device, &desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	desc.surface = NULL;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_SURFACE);
	desc.surface = surface;
	desc.queue   = NULL;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_RESULT_GPU_INVALID_COMMAND_QUEUE);
	CHECK(swap_chain == NULL);

	CHECK(strcmp(cr_result_str(CR_RESULT_GPU_INVALID_PRESENT_SCALING), "CR_RESULT_GPU_INVALID_PRESENT_SCALING") == 0);
	CHECK(strcmp(cr_result_str(CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED), "CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED") == 0);
}

#if CRGPU_COMPILE_METAL_API
// A swap chain of [scaling] on a layer, which sets the layer's gravity to
// [gravity] and reads its scaling back.
static void check_metal_swap_chain(crgpu_device device, crgpu_surface surface, void *layer, CRGPU_PRESENT_SCALING scaling, const char *gravity) {
	crgpu_test_metal_layer_set_gravity_other(layer);
	crgpu_swap_chain_desc desc  = swap_chain_desc_for(device, surface, scaling);
	crgpu_swap_chain swap_chain = NULL;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_SUCCESS);
	CHECK(swap_chain != NULL);
	CHECK(strcmp(crgpu_test_metal_layer_gravity(layer), gravity) == 0);
	crgpu_swap_chain_desc read;
	memset(&read, 0, sizeof(read));
	CHECK(crgpu_swap_chain_get_desc(swap_chain, &read) == CR_SUCCESS);
	CHECK(read.present_scaling == scaling);
	crgpu_swap_chain_destroy(swap_chain);
}

static void test_present_scaling_metal(crgpu_instance instance, crgpu_device device) {
	for (int flipped = 0; flipped < 2; ++flipped) {
		void *layer             = crgpu_test_metal_layer_create(flipped != 0);
		crgpu_surface surface = NULL;
		CHECK(crgpu_surface_create(instance, layer, NULL, &surface) == CR_SUCCESS);
		if (surface) {
			uint32_t scalings = 0;
			CHECK(crgpu_surface_get_present_scalings(device, surface, &scalings) == CR_SUCCESS);
			CHECK(scalings == (CRGPU_PRESENT_SCALING_ONE_TO_ONE | CRGPU_PRESENT_SCALING_STRETCH | CRGPU_PRESENT_SCALING_ASPECT_FIT));
			// Top left on screen: a flipped layer's contents put the y axis
			// the other way up.
			check_metal_swap_chain(device, surface, layer, CRGPU_PRESENT_SCALING_ONE_TO_ONE, flipped ? "bottomLeft" : "topLeft");
			check_metal_swap_chain(device, surface, layer, CRGPU_PRESENT_SCALING_STRETCH, "resize");
			check_metal_swap_chain(device, surface, layer, CRGPU_PRESENT_SCALING_ASPECT_FIT, "resizeAspect");
			crgpu_surface_destroy(surface);
		}
		crgpu_test_metal_layer_destroy(layer);
	}
}
#endif

// ---- A frame drawn and read back ------------------------------------------
//
// One pass into a 64 x 64 image seen as an 8 x 8 grid of cells, each draw
// call covering cells of its own in a colour of its own, then the image
// copied to a second one by a blit and both read back, so that what each
// call drew is checked by its pixels. The frame takes the Carrottus GPU calls
// nothing else in this test runs: shader modules and pipelines, a uniform
// buffer, a texture and a sampler through a descriptor set, push constants,
// vertex, index and indirect buffers, every draw call, a colour and a depth
// clear before the pass, the depth bounds and a mesh shader where the device
// has them, timestamps where it has them, buffer and image copies and
// barriers, a placed buffer and a placed image, debug labels, a plain submit
// and a wait for the queue. A compute shader doubles a buffer after it.

#define DRAW_GRID 8
#define DRAW_CELL 8
#define DRAW_SIZE (DRAW_GRID * DRAW_CELL)

typedef struct draw_push {
	float color[4];
	// x: depth; y: how much of the texture to add; zw: the mesh shader's
	// first cell.
	float params[4];
} draw_push;

typedef struct cell {
	float x, y;
} cell;

// The format the test's adapter takes (its shader_formats), set in main()
// before any module is made.
static CRGPU_SHADER_FORMAT test_shader_format = CRGPU_SHADER_FORMAT_NONE;

static crgpu_shader_module make_module(crgpu_device device, const crgpu_test_shader *shader, const char *name) {
	const crgpu_shader_module_desc desc = crgpu_test_module_desc(shader, test_shader_format, name);
	crgpu_shader_module module          = NULL;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_SUCCESS);
	return module;
}

// Each refusal of crgpu_shader_module_create() after the base checks, in
// the header's order, and that a refused call leaves out_module alone; then
// the module made right. The same on every backend.
static void test_shader_module_checks(crgpu_device device) {
	const crgpu_test_shader *vert = &crgpu_test_objects_draw_vert;
	const crgpu_test_shader *comp = &crgpu_test_objects_double_comp;
	crgpu_shader_module_desc desc;
	crgpu_shader_module module = NULL;
	crgpu_shader_module marker = (crgpu_shader_module)(uintptr_t)1;

	desc       = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.stage = CRGPU_SHADER_STAGE_VERTEX | CRGPU_SHADER_STAGE_FRAGMENT;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_SHADER_STAGE);
	desc        = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.format = CRGPU_SHADER_FORMAT_NONE;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED);
	for (CRGPU_SHADER_FORMAT other = CRGPU_SHADER_FORMAT_SPIRV; other <= CRGPU_SHADER_FORMAT_METALLIB; ++other) {
		if (other == test_shader_format)
			continue;
		desc        = crgpu_test_module_desc(vert, test_shader_format, "checks");
		desc.format = other;
		CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED);
	}
	desc      = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code = NULL;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_NULL_SHADER_CODE);
	desc           = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code_size = 0;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_NULL_SHADER_CODE);
	desc      = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code = (const uint8_t *)desc.code + 1;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT);
	if (test_shader_format != CRGPU_SHADER_FORMAT_METALLIB) {
		desc           = crgpu_test_module_desc(vert, test_shader_format, "checks");
		desc.code_size = desc.code_size - 2;
		CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT);
	}
	desc             = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = NULL;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_NULL_ENTRY_POINT);
	desc.entry_point = "";
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_NULL_ENTRY_POINT);
	desc                   = crgpu_test_module_desc(comp, test_shader_format, "checks");
	desc.workgroup_size[1] = 0;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_WORKGROUP_SIZE);
	desc                   = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.workgroup_size[0] = 1;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_WORKGROUP_SIZE);

	// Bytes that aren't the format: its first word broken.
	uint32_t broken[64];
	memset(broken, 0, sizeof(broken));
	broken[0]      = 0x12345678u;
	desc           = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code      = broken;
	desc.code_size = sizeof(broken);
	module         = marker;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_INVALID_SHADER_CODE);
	CHECK(module == marker);

	// An entry point the module lacks, and the right name for another stage.
	desc             = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = "no_such_entry";
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND);
	desc       = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.stage = CRGPU_SHADER_STAGE_FRAGMENT;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND);
	CHECK(module == marker);

	// The caller's entry point string is copied: changed after the call, the
	// module still makes a pipeline (test_drawing) and is destroyed cleanly.
	char entry[64];
	snprintf(entry, sizeof(entry), "%s", vert->entry_point);
	desc             = crgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = entry;
	module           = NULL;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_SUCCESS);
	memset(entry, 'x', sizeof(entry) - 1);
	CHECK(module != NULL);
	crgpu_shader_module_destroy(module);
}

// Geometry and tessellation behind their EXTs, on [device], created with
// no feature: a module of either stage and a pipeline naming one are
// refused, the same on every backend. [supported] is the adapter's: D3D12
// has both bits on every adapter, Metal neither (its EXTs are 0); Vulkan
// says what the GPU has.
static void test_geometry_tessellation_refusals(crgpu_device device, CRGPU_FEATURES supported) {
	printf("crgpu_objects_test: geometry shaders %s, tessellation %s\n",
	    (supported & CRGPU_FEATURE_GEOMETRY_SHADER) ? "yes" : "no",
	    (supported & CRGPU_FEATURE_TESSELLATION) ? "yes" : "no");
	if (test_shader_format == CRGPU_SHADER_FORMAT_DXIL) {
		CHECK((supported & CRGPU_FEATURE_GEOMETRY_SHADER) != 0);
		CHECK((supported & CRGPU_FEATURE_TESSELLATION) != 0);
	}
	if (test_shader_format == CRGPU_SHADER_FORMAT_METALLIB) {
		CHECK((supported & CRGPU_FEATURE_GEOMETRY_SHADER) == 0);
		CHECK((supported & CRGPU_FEATURE_TESSELLATION) == 0);
	}
	CHECK((crgpu_device_get_features(device) & (CRGPU_FEATURE_GEOMETRY_SHADER | CRGPU_FEATURE_TESSELLATION)) == 0);

	// A module of each stage: refused after the checks every module has, so
	// the vertex shader's code stands in.
	crgpu_shader_module_desc desc = crgpu_test_module_desc(&crgpu_test_objects_draw_vert, test_shader_format, "stage ext");
	crgpu_shader_module module    = NULL;
	desc.stage                      = CRGPU_SHADER_STAGE_GEOMETRY;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED);
	desc.stage = CRGPU_SHADER_STAGE_TESSELLATION_CONTROL;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
	desc.stage = CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION;
	CHECK(crgpu_shader_module_create(device, &desc, &module) == CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
	CHECK(module == NULL);

	// A pipeline naming one: refused after the mesh check, before anything
	// reads the module, so the vertex module stands in for each stage.
	crgpu_shader_module vert = make_module(device, &crgpu_test_objects_draw_vert, "stage ext vertex");
	crgpu_shader_module frag = make_module(device, &crgpu_test_objects_draw_frag, "stage ext fragment");
	if (vert && frag) {
		crgpu_graphics_pipeline_desc pd;
		memset(&pd, 0, sizeof(pd));
		pd.base.type                     = CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC;
		pd.vertex_module                 = vert;
		pd.fragment_module               = frag;
		pd.color_attachment_count        = 1;
		pd.color_attachment_formats[0]   = CRGPU_FORMAT_R8G8B8A8_UNORM;
		crgpu_graphics_pipeline pipeline = NULL;
		pd.geometry_module               = vert;
		CHECK(crgpu_graphics_pipeline_create(device, &pd, &pipeline) == CR_RESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED);
		pd.geometry_module     = NULL;
		pd.tess_control_module = vert;
		CHECK(crgpu_graphics_pipeline_create(device, &pd, &pipeline) == CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
		pd.tess_control_module = NULL;
		pd.tess_eval_module    = vert;
		CHECK(crgpu_graphics_pipeline_create(device, &pd, &pipeline) == CR_RESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
		CHECK(pipeline == NULL);
	}
	crgpu_shader_module_destroy(frag);
	crgpu_shader_module_destroy(vert);
}

static crgpu_buffer make_buffer(crgpu_device device, CRGPU_MEMORY_KIND kind, CRGPU_BUFFER_USAGE usage, uint64_t size, const void *contents) {
	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = usage;
	desc.memory_kind      = kind;
	desc.size             = size;
	crgpu_buffer buffer = NULL;
	CHECK(crgpu_buffer_create(device, &desc, &buffer) == CR_SUCCESS);
	if (buffer && contents) {
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(buffer, 0, size, &mapped) == CR_SUCCESS);
		if (mapped) {
			memcpy(mapped, contents, (size_t)size);
			CHECK(crgpu_buffer_flush(buffer, 0, size) == CR_SUCCESS);
			crgpu_buffer_unmap(buffer);
		}
	}
	return buffer;
}

static crgpu_buffer_view make_view(crgpu_buffer buffer, uint64_t offset, uint64_t stride, uint64_t size) {
	crgpu_buffer_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                 = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	desc.parent_buffer             = buffer;
	desc.offset_from_parent_buffer = offset;
	desc.stride                    = stride;
	desc.size                      = size;
	crgpu_buffer_view view       = NULL;
	if (buffer)
		CHECK(crgpu_buffer_view_create(buffer, &desc, &view) == CR_SUCCESS);
	return view;
}

static crgpu_image make_image(crgpu_device device, crgpu_heap heap, CRGPU_IMAGE_USAGE usage, CRGPU_FORMAT format, uint32_t width, uint32_t height,
                                const char *name) {
	crgpu_image_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	desc.usage              = usage;
	desc.type               = CRGPU_IMAGE_TYPE_2D;
	desc.format             = format;
	desc.clear_value.format = format;
	// The values the frame clears the attachments to, which D3D12 clears
	// fastest when told at creation.
	if (format == CRGPU_FORMAT_D32_FLOAT) {
		desc.clear_value.depth_stencil.depth = 0.25f;
	} else {
		desc.clear_value.color[0] = 0.4f;
		desc.clear_value.color[1] = 0.4f;
		desc.clear_value.color[2] = 0.4f;
		desc.clear_value.color[3] = 1.0f;
	}
	desc.width          = width;
	desc.height         = height;
	desc.depth          = 1;
	desc.array_layers   = 1;
	desc.mip_levels     = 1;
	desc.debug_name     = name;
	crgpu_image image = NULL;
	if (heap) {
		crgpu_placement_requirements needs;
		memset(&needs, 0, sizeof(needs));
		needs.base.type = CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS;
		CHECK(crgpu_heap_get_image_placement_requirements(heap, &desc, &needs) == CR_SUCCESS);
		CHECK(needs.size >= (uint64_t)width * height * 4 && needs.alignment > 0);
		CHECK(crgpu_image_create_placed(heap, 0, &desc, &image) == CR_SUCCESS);
	} else {
		CHECK(crgpu_image_create(device, &desc, &image) == CR_SUCCESS);
	}
	return image;
}

static crgpu_image_view make_image_view(crgpu_image image, uint64_t aspect) {
	crgpu_image_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	desc.parent_image                        = image;
	desc.type                                = CRGPU_IMAGE_VIEW_TYPE_2D;
	desc.subresource_range.aspect_mask       = aspect;
	desc.subresource_range.mip_level_count   = 1;
	desc.subresource_range.array_layer_count = 1;
	crgpu_image_view view                  = NULL;
	if (image)
		CHECK(crgpu_image_view_create(image, &desc, &view) == CR_SUCCESS);
	return view;
}

static crgpu_heap make_heap(crgpu_device device, CRGPU_HEAP_RESOURCE_CLASS resource_class, const char *name) {
	crgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = CR_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = CRGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = resource_class;
	desc.size           = CRGPU_HEAP_SIZE_GRANULARITY * 16;
	desc.debug_name     = name;
	crgpu_heap heap   = NULL;
	CHECK(crgpu_heap_create(device, &desc, &heap) == CR_SUCCESS);
	return heap;
}

// A graphics pipeline over the test's shaders: [mesh] in place of the
// vertex shader where given; depth as asked.
static crgpu_graphics_pipeline make_pipeline(crgpu_device device, crgpu_shader_module vertex, crgpu_shader_module mesh, crgpu_shader_module fragment,
                                             crgpu_descriptor_set_layout layout, bool depth_test, CRGPU_COMPARE_OP compare, bool depth_bounds,
                                             const char *name) {
	crgpu_graphics_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type       = CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC;
	desc.debug_name      = name;
	desc.fragment_module = fragment;
	if (mesh) {
		desc.mesh_module = mesh;
	} else {
		desc.vertex_module                   = vertex;
		desc.vertex_attributes[0].location   = 0;
		desc.vertex_attributes[0].binding    = 0;
		desc.vertex_attributes[0].format     = CRGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attributes[1].location   = 1;
		desc.vertex_attributes[1].binding    = 1;
		desc.vertex_attributes[1].format     = CRGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attribute_count          = 2;
		desc.vertex_bindings[0].binding      = 0;
		desc.vertex_bindings[0].stride       = sizeof(cell);
		desc.vertex_bindings[1].binding      = 1;
		desc.vertex_bindings[1].stride       = sizeof(cell);
		desc.vertex_bindings[1].per_instance = true;
		desc.vertex_binding_count            = 2;
	}
	desc.primitive_topology          = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                   = CRGPU_CULL_MODE_NONE;
	desc.depth_test_enable           = depth_test;
	desc.depth_write_enable          = depth_test && !depth_bounds;
	desc.depth_compare_op            = compare;
	desc.depth_bounds_test_enable    = depth_bounds;
	desc.color_attachment_formats[0] = CRGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count      = 1;
	desc.depth_format                = CRGPU_FORMAT_D32_FLOAT;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layout_count = 1;
	desc.push_constant_ranges[0].stage_flags = (mesh ? CRGPU_SHADER_STAGE_MESH : CRGPU_SHADER_STAGE_VERTEX) | CRGPU_SHADER_STAGE_FRAGMENT;
	desc.push_constant_ranges[0].size        = sizeof(draw_push);
	desc.push_constant_range_count           = 1;
	crgpu_graphics_pipeline pipeline         = NULL;
	CHECK(crgpu_graphics_pipeline_create(device, &desc, &pipeline) == CR_SUCCESS);
	return pipeline;
}

static void push_draw(crgpu_command_list cmd, crgpu_graphics_pipeline pipeline, float r, float g, float b, float a, float depth, float texture) {
	draw_push push = {{r, g, b, a}, {depth, texture, 0.0f, 0.0f}};
	crgpu_command_list_push_graphics_constants(cmd, pipeline, 0, sizeof(push), &push);
}

// Each byte of a pixel, to within 2.
static bool pixel_is(const uint8_t *pixels, uint32_t row_pitch, uint32_t x, uint32_t y, int r, int g, int b, int a) {
	const uint8_t *p  = pixels + (size_t)y * row_pitch + (size_t)x * 4;
	const int want[4] = {r, g, b, a};
	for (int i = 0; i < 4; ++i) {
		const int d = (int)p[i] - want[i];
		if (d < -2 || d > 2) {
			fprintf(stderr, "  pixel (%u, %u) is (%u, %u, %u, %u), expected (%d, %d, %d, %d)\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
			return false;
		}
	}
	return true;
}

// The middle of cell (cx, cy) is (r, g, b, a).
static bool cell_is(const uint8_t *pixels, uint32_t row_pitch, uint32_t cx, uint32_t cy, int r, int g, int b, int a) {
	return pixel_is(pixels, row_pitch, cx * DRAW_CELL + DRAW_CELL / 2, cy * DRAW_CELL + DRAW_CELL / 2, r, g, b, a);
}

// Records with [record], submits with crgpu_command_queue_submit_lists() and
// waits for the queue to be idle.
typedef void (*record_fn)(crgpu_command_list cmd, void *context);
static void run_once(crgpu_command_queue queue, crgpu_command_allocator allocator, crgpu_command_list cmd, record_fn record, void *context) {
	CHECK(crgpu_command_allocator_reset(allocator) == CR_SUCCESS);
	crgpu_command_list_begin(cmd);
	record(cmd, context);
	crgpu_command_list_end(cmd);
	CHECK(crgpu_command_queue_submit_lists(queue, &cmd, 1) == CR_SUCCESS);
	crgpu_command_queue_wait_idle(queue);
}

typedef struct draw_frame {
	CRGPU_FEATURES features;
	crgpu_graphics_pipeline plain, depth_tested, depth_bounded, meshed;
	crgpu_descriptor_set set;
	crgpu_image target, copy, depth, texture;
	crgpu_image_view target_view, depth_view;
	crgpu_buffer vertex_upload, vertices, texel_upload, indirect, readback_target, readback_copy;
	crgpu_buffer_view corners, indices, instances[8];
	uint64_t texel_row_pitch, row_pitch;
	crgpu_timestamp_set timestamps;
} draw_frame;

// The vertices to the device buffer, the texels to the texture: before the
// frame, each copy between its barriers.
static void record_uploads(crgpu_command_list cmd, void *context) {
	draw_frame *f = (draw_frame *)context;
	crgpu_command_list_begin_debug_label(cmd, "uploads");
	crgpu_buffer_barrier to_copy;
	memset(&to_copy, 0, sizeof(to_copy));
	to_copy.base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	to_copy.buffer       = f->vertices;
	to_copy.state_before = CRGPU_RESOURCE_STATE_COMMON;
	to_copy.state_after  = CRGPU_RESOURCE_STATE_COPY_DEST;
	crgpu_command_list_pipeline_barrier(cmd, &to_copy, 1, NULL, 0);
	// In two copies, so that the offsets count: the first six corners, then
	// the four after them.
	crgpu_command_list_copy_buffer(cmd, f->vertex_upload, f->vertices, 0, 0, 6 * sizeof(cell));
	crgpu_command_list_copy_buffer(cmd, f->vertex_upload, f->vertices, 6 * sizeof(cell), 6 * sizeof(cell), 4 * sizeof(cell));
	crgpu_buffer_barrier to_vertices = to_copy;
	to_vertices.state_before           = CRGPU_RESOURCE_STATE_COPY_DEST;
	to_vertices.state_after            = CRGPU_RESOURCE_STATE_VERTEX_BUFFER;
	crgpu_command_list_pipeline_barrier(cmd, &to_vertices, 1, NULL, 0);

	crgpu_command_list_image_barrier(cmd, f->texture, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);
	crgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(f->texel_row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = 2;
	copy.height            = 2;
	copy.depth             = 1;
	crgpu_command_list_copy_buffer_to_image(cmd, f->texel_upload, f->texture, &copy);
	crgpu_command_list_image_barrier(cmd, f->texture, CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	crgpu_command_list_end_debug_label(cmd);
}

static void record_frame(crgpu_command_list cmd, void *context) {
	draw_frame *f = (draw_frame *)context;
	if (f->timestamps)
		crgpu_command_list_reset_timestamps(cmd, f->timestamps, 0, 2);
	crgpu_command_list_begin_debug_label(cmd, "frame");
	crgpu_command_list_image_barrier_view(cmd, f->target_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	crgpu_command_list_image_barrier_view(cmd, f->depth_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	// Both attachments cleared before the pass, which loads them: what no
	// draw covers is grey only if the colour clear ran, and the depth tests
	// below pass and fail against the depth clear's 0.25.
	crgpu_command_list_clear_color_attachment(cmd, f->target_view, 0.4f, 0.4f, 0.4f, 1.0f, DRAW_SIZE, DRAW_SIZE);
	crgpu_command_list_clear_depth_attachment(cmd, f->depth_view, true, false, 0.25f, 0, DRAW_SIZE, DRAW_SIZE);

	crgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                         = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view   = f->target_view;
	pass.color_attachments[0].load_op      = CRGPU_LOAD_OP_LOAD;
	pass.color_attachments[0].store_op     = CRGPU_STORE_OP_STORE;
	pass.color_attachment_count            = 1;
	pass.depth_attachment.image_view       = f->depth_view;
	pass.depth_attachment.depth_load_op    = CRGPU_LOAD_OP_LOAD;
	pass.depth_attachment.depth_store_op   = CRGPU_STORE_OP_STORE;
	pass.depth_attachment.stencil_load_op  = CRGPU_LOAD_OP_DONT_CARE;
	pass.depth_attachment.stencil_store_op = CRGPU_STORE_OP_DONT_CARE;
	pass.width                             = DRAW_SIZE;
	pass.height                            = DRAW_SIZE;
	if (f->timestamps) {
		pass.timestamp_set         = f->timestamps;
		pass.timestamp_begin_index = 0;
		pass.timestamp_end_index   = 1;
	}
	crgpu_command_list_begin_rendering(cmd, &pass);

	CRGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)DRAW_SIZE, (float)DRAW_SIZE, 0.0f, 1.0f};
	crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
	CRGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)DRAW_SIZE, (float)DRAW_SIZE};
	crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);

	crgpu_command_list_bind_graphics_pipeline(cmd, f->plain);
	crgpu_command_list_bind_graphics_descriptor_sets(cmd, f->plain, 0, &f->set, 1);
	crgpu_command_list_set_index_buffer(cmd, f->indices);
	crgpu_buffer_view views[2] = {f->corners, f->instances[0]};

	// Row 0, each call in its own colour.
	// (0, 0): a plain draw of six corners.
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw(cmd, 6, 0);
	// (1, 0) and (2, 0): two instances from the second.
	views[1] = f->instances[1];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw_instanced(cmd, 6, 2, 0, 1);
	// (3, 0): indexed, the four corners after the first six.
	views[1] = f->instances[2];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw_indexed(cmd, 6, 0, 6);
	// (4, 0) and (5, 0): indexed and instanced, from the second six indices
	// and the second instance.
	views[1] = f->instances[3];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw_indexed_instanced(cmd, 6, 2, 6, 6, 1);
	// (6, 0), then (6, 0) and (7, 0): two indirect draws, 32 bytes apart;
	// in one call where the device draws several.
	views[1] = f->instances[4];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f);
	const uint32_t draws = (f->features & CRGPU_FEATURE_MULTI_DRAW_INDIRECT) ? 2 : 1;
	crgpu_command_list_draw_indirect(cmd, f->indirect, 0, draws, 32);
	if (draws == 1)
		crgpu_command_list_draw_indirect(cmd, f->indirect, 32, 1, 32);
	// Row 1. (0, 1), then (0, 1) and (1, 1): two indexed indirect draws,
	// 20 bytes apart.
	views[1] = f->instances[5];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.6f, 0.2f, 0.0f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw_indexed_indirect(cmd, f->indirect, 64, draws, 20);
	if (draws == 1)
		crgpu_command_list_draw_indexed_indirect(cmd, f->indirect, 84, 1, 20);
	// (2, 1): the texture alone, one texel a quarter of the cell.
	views[1] = f->instances[6];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f);
	crgpu_command_list_draw(cmd, 6, 0);

	// Depth, less than the cleared 0.25 to pass: (3, 1) at 0.2 passes and
	// writes 0.2, (4, 1) at 0.5 fails, (5, 1) at 0.1 passes.
	crgpu_command_list_bind_graphics_pipeline(cmd, f->depth_tested);
	crgpu_command_list_bind_graphics_descriptor_sets(cmd, f->depth_tested, 0, &f->set, 1);
	views[1] = f->instances[7];
	crgpu_command_list_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->depth_tested, 0.2f, 0.2f, 0.8f, 1.0f, 0.2f, 0.0f);
	crgpu_command_list_draw_instanced(cmd, 6, 1, 0, 0);
	push_draw(cmd, f->depth_tested, 0.2f, 0.2f, 0.8f, 1.0f, 0.5f, 0.0f);
	crgpu_command_list_draw_instanced(cmd, 6, 1, 0, 1);
	push_draw(cmd, f->depth_tested, 0.8f, 0.2f, 0.2f, 1.0f, 0.1f, 0.0f);
	crgpu_command_list_draw_instanced(cmd, 6, 1, 0, 2);

	// Depth bounds of 0.15 to 0.22 over (3, 1), whose depth is 0.2, and
	// (6, 1), whose depth is 0.25: only the first is drawn.
	if (f->depth_bounded) {
		crgpu_command_list_bind_graphics_pipeline(cmd, f->depth_bounded);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, f->depth_bounded, 0, &f->set, 1);
		crgpu_command_list_set_depth_bounds(cmd, 0.15f, 0.22f);
		push_draw(cmd, f->depth_bounded, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f);
		crgpu_command_list_draw_instanced(cmd, 6, 2, 0, 3);
	}
	// Row 2. (0, 2) and (1, 2): a mesh shader's two workgroups.
	if (f->meshed) {
		crgpu_command_list_bind_graphics_pipeline(cmd, f->meshed);
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, f->meshed, 0, &f->set, 1);
		draw_push push = {{1.0f, 0.6f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 2.0f}};
		crgpu_command_list_push_graphics_constants(cmd, f->meshed, 0, sizeof(push), &push);
		crgpu_command_list_dispatch_mesh(cmd, 2, 1, 1);
	}
	crgpu_command_list_end_rendering(cmd);
	if (f->timestamps)
		crgpu_command_list_resolve_timestamps(cmd, f->timestamps, 0, 2);

	// The target to the copy by a blit, then both to buffers.
	crgpu_command_list_image_barrier_view(cmd, f->target_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	crgpu_image_view_desc_subresource_range whole;
	memset(&whole, 0, sizeof(whole));
	whole.aspect_mask       = CRGPU_IMAGE_ASPECT_COLOR;
	whole.mip_level_count   = 1;
	whole.array_layer_count = 1;
	crgpu_command_list_image_barrier_subresource(cmd, f->copy, &whole, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);
	crgpu_image_blit_desc blit;
	memset(&blit, 0, sizeof(blit));
	blit.src_array_layer_count = 1;
	blit.src_x1                = DRAW_SIZE;
	blit.src_y1                = DRAW_SIZE;
	blit.src_z1                = 1;
	blit.dst_array_layer_count = 1;
	blit.dst_x1                = DRAW_SIZE;
	blit.dst_y1                = DRAW_SIZE;
	blit.dst_z1                = 1;
	blit.filter                = CRGPU_FILTER_NEAREST;
	crgpu_command_list_blit_image(cmd, f->target, f->copy, &blit);
	crgpu_command_list_image_barrier_subresource(cmd, f->copy, &whole, CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	crgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(f->row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = DRAW_SIZE;
	copy.height            = DRAW_SIZE;
	copy.depth             = 1;
	crgpu_command_list_copy_image_to_buffer(cmd, f->target, f->readback_target, &copy);
	crgpu_command_list_copy_image_to_buffer(cmd, f->copy, f->readback_copy, &copy);
	crgpu_command_list_end_debug_label(cmd);
}

static void check_frame(const draw_frame *f, const uint8_t *pixels, const char *which) {
	const uint32_t pitch = (uint32_t)f->row_pitch;
	const int tint       = 51; // the uniform buffer's blue, added to every draw
	printf("crgpu_objects_test: checking the %s\n", which);
	CHECK(cell_is(pixels, pitch, 0, 0, 255, 0, tint, 255));   // draw
	CHECK(cell_is(pixels, pitch, 1, 0, 0, 255, tint, 255));   // draw_instanced, first instance 1
	CHECK(cell_is(pixels, pitch, 2, 0, 0, 255, tint, 255));
	CHECK(cell_is(pixels, pitch, 3, 0, 255, 255, tint, 255)); // draw_indexed, base vertex 6
	CHECK(cell_is(pixels, pitch, 4, 0, 255, 0, 255, 255));    // draw_indexed_instanced
	CHECK(cell_is(pixels, pitch, 5, 0, 255, 0, 255, 255));
	CHECK(cell_is(pixels, pitch, 6, 0, 0, 255, 255, 255));    // draw_indirect, both draws
	CHECK(cell_is(pixels, pitch, 7, 0, 0, 255, 255, 255));
	CHECK(cell_is(pixels, pitch, 0, 1, 153, 51, tint, 255));  // draw_indexed_indirect, both draws
	CHECK(cell_is(pixels, pitch, 1, 1, 153, 51, tint, 255));
	// The texture: red, green above; blue, white below; nearest texels.
	CHECK(pixel_is(pixels, pitch, 2 * DRAW_CELL + 2, 1 * DRAW_CELL + 2, 255, 0, tint, 255));
	CHECK(pixel_is(pixels, pitch, 2 * DRAW_CELL + 6, 1 * DRAW_CELL + 2, 0, 255, tint, 255));
	CHECK(pixel_is(pixels, pitch, 2 * DRAW_CELL + 2, 1 * DRAW_CELL + 6, 0, 0, 255, 255));
	CHECK(pixel_is(pixels, pitch, 2 * DRAW_CELL + 6, 1 * DRAW_CELL + 6, 255, 255, 255, 255));
	// Depth: (3, 1) drawn, then drawn over within the bounds where they are
	// tested; (4, 1) not drawn; (5, 1) drawn; (6, 1) outside the bounds.
	if (f->depth_bounded)
		CHECK(cell_is(pixels, pitch, 3, 1, 255, 255, 255, 255));
	else
		CHECK(cell_is(pixels, pitch, 3, 1, 51, 51, 255, 255));
	CHECK(cell_is(pixels, pitch, 4, 1, 102, 102, 102, 255));
	CHECK(cell_is(pixels, pitch, 5, 1, 204, 51, 102, 255));
	CHECK(cell_is(pixels, pitch, 6, 1, 102, 102, 102, 255));
	if (f->meshed) {
		CHECK(cell_is(pixels, pitch, 0, 2, 255, 153, tint, 255));
		CHECK(cell_is(pixels, pitch, 1, 2, 255, 153, tint, 255));
	}
	// What nothing drew: the grey of the clear before the pass.
	CHECK(cell_is(pixels, pitch, 7, 7, 102, 102, 102, 255));
	CHECK(cell_is(pixels, pitch, 2, 2, 102, 102, 102, 255));
}

static void read_back(const draw_frame *f, crgpu_buffer buffer, uint64_t size, const char *which) {
	void *mapped = NULL;
	CHECK(crgpu_buffer_map(buffer, 0, size, &mapped) == CR_SUCCESS);
	if (!mapped)
		return;
	CHECK(crgpu_buffer_invalidate(buffer, 0, size) == CR_SUCCESS);
	check_frame(f, (const uint8_t *)mapped, which);
	crgpu_buffer_unmap(buffer);
}

static void test_draw_frame(crgpu_device device, crgpu_command_queue queue, crgpu_command_allocator allocator, crgpu_command_list cmd) {
	draw_frame f;
	memset(&f, 0, sizeof(f));
	f.features = crgpu_device_get_features(device);

	// Shaders, a set of a tint, a texture and a sampler, and the pipelines.
	crgpu_shader_module vertex =
	    make_module(device, &crgpu_test_objects_draw_vert, "draw vertex");
	crgpu_shader_module fragment =
	    make_module(device, &crgpu_test_objects_draw_frag, "draw fragment");
	crgpu_shader_module mesh = NULL;
	if (f.features & CRGPU_FEATURE_MESH_SHADING)
		mesh = make_module(device, &crgpu_test_objects_draw_mesh, "draw mesh");
	else
		printf("crgpu_objects_test: the device has no mesh shading, crgpu_command_list_dispatch_mesh not run\n");
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.bindings[1].binding         = 1;
	layout_desc.bindings[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	layout_desc.bindings[1].count           = 1;
	layout_desc.bindings[1].stage_flags     = CRGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.bindings[2].binding         = 2;
	layout_desc.bindings[2].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
	layout_desc.bindings[2].count           = 1;
	layout_desc.bindings[2].stage_flags     = CRGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 3;
	crgpu_descriptor_set_layout layout    = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &layout) == CR_SUCCESS);
	if (vertex && fragment && layout) {
		f.plain        = make_pipeline(device, vertex, NULL, fragment, layout, false, CRGPU_COMPARE_OP_ALWAYS, false, "draw plain");
		f.depth_tested = make_pipeline(device, vertex, NULL, fragment, layout, true, CRGPU_COMPARE_OP_LESS, false, "draw depth");
		if (f.features & CRGPU_FEATURE_DEPTH_BOUNDS_TEST)
			f.depth_bounded = make_pipeline(device, vertex, NULL, fragment, layout, true, CRGPU_COMPARE_OP_ALWAYS, true, "draw bounds");
		else
			printf("crgpu_objects_test: the device has no depth bounds test, crgpu_command_list_set_depth_bounds not run\n");
		if (mesh)
			f.meshed = make_pipeline(device, NULL, mesh, fragment, layout, false, CRGPU_COMPARE_OP_ALWAYS, false, "draw meshed");
	}

	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_sizes[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_desc.pool_sizes[1].count           = 1;
	pool_desc.pool_sizes[2].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
	pool_desc.pool_sizes[2].count           = 1;
	pool_desc.pool_size_count               = 3;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(device, &set_desc, &f.set) == CR_SUCCESS);
	}

	// The corners: a unit quad as six (two triangles), then as four for the
	// index buffer's two triangles; uploaded to a buffer placed in a heap.
	static const cell corners[10] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}, {0, 0}, {1, 0}, {0, 1}, {1, 1}};
	f.vertex_upload               = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, sizeof(corners), corners);
	crgpu_heap buffer_heap      = make_heap(device, CRGPU_HEAP_RESOURCE_CLASS_BUFFERS, "draw buffers");
	if (buffer_heap) {
		crgpu_buffer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
		desc.usage       = CRGPU_BUFFER_USAGE_VERTEX | CRGPU_BUFFER_USAGE_TRANSFER_DST;
		desc.memory_kind = CRGPU_MEMORY_KIND_DEVICE;
		desc.size        = sizeof(corners);
		crgpu_placement_requirements needs;
		memset(&needs, 0, sizeof(needs));
		needs.base.type = CR_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS;
		CHECK(crgpu_heap_get_buffer_placement_requirements(buffer_heap, &desc, &needs) == CR_SUCCESS);
		CHECK(needs.size >= sizeof(corners) && needs.alignment > 0);
		// Past the heap's start, at the alignment asked for.
		CHECK(crgpu_buffer_create_placed(buffer_heap, needs.alignment, &desc, &f.vertices) == CR_SUCCESS);
	}
	f.corners = make_view(f.vertices, 0, sizeof(cell), sizeof(corners));
	// The instances: one list per draw call, a cell each. (-1, -1) is off
	// the target: what a draw that ignored its first instance would draw.
	static const cell instances[8][3] = {
	    {{0, 0}},
	    {{-1, -1}, {1, 0}, {2, 0}},
	    {{3, 0}},
	    {{-1, -1}, {4, 0}, {5, 0}},
	    {{6, 0}, {7, 0}},
	    {{0, 1}, {1, 1}},
	    {{2, 1}},
	    {{3, 1}, {4, 1}, {5, 1}},
	};
	// After the last list: the depth bounds' two cells.
	static const cell bounded[3] = {{3, 1}, {6, 1}};
	cell instance_data[8 * 3 + 3];
	memcpy(instance_data, instances, sizeof(instances));
	memcpy(instance_data + 8 * 3, bounded, sizeof(bounded));
	crgpu_buffer instance_buffer = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_VERTEX, sizeof(instance_data), instance_data);
	for (int i = 0; i < 8; ++i)
		f.instances[i] = make_view(instance_buffer, (uint64_t)i * 3 * sizeof(cell), sizeof(cell), (i == 7 ? 6 : 3) * sizeof(cell));
	static const uint16_t indices[12] = {0, 1, 2, 2, 1, 3, 0, 1, 2, 2, 1, 3};
	crgpu_buffer index_buffer       = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_INDEX, sizeof(indices), indices);
	f.indices                         = make_view(index_buffer, 0, sizeof(uint16_t), sizeof(indices));
	// Indirect arguments: two draws 32 bytes apart at 0, two indexed draws
	// 20 bytes apart at 64.
	uint8_t indirect[104];
	memset(indirect, 0, sizeof(indirect));
	const crgpu_draw_indirect_args draws[2]                 = {{6, 1, 0, 0}, {6, 2, 0, 0}};
	const crgpu_draw_indexed_indirect_args indexed_draws[2] = {{6, 1, 0, 6, 0}, {6, 2, 6, 6, 0}};
	memcpy(indirect, &draws[0], sizeof(draws[0]));
	memcpy(indirect + 32, &draws[1], sizeof(draws[1]));
	memcpy(indirect + 64, &indexed_draws[0], sizeof(indexed_draws[0]));
	memcpy(indirect + 84, &indexed_draws[1], sizeof(indexed_draws[1]));
	f.indirect = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_INDIRECT, sizeof(indirect), indirect);
	// The tint, written through a mapping of part of the buffer.
	const float tint[4]    = {0.0f, 0.0f, 0.2f, 0.0f};
	crgpu_buffer uniform = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_UNIFORM, 256, NULL);
	if (uniform) {
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(uniform, 0, 256, &mapped) == CR_SUCCESS);
		if (mapped) {
			memcpy(mapped, tint, sizeof(tint));
			CHECK(crgpu_buffer_flush(uniform, 0, sizeof(tint)) == CR_SUCCESS);
			crgpu_buffer_unmap(uniform);
		}
	}

	// The images: the target placed in a heap, a copy, depth, and a 2 x 2
	// texture with its sampler.
	crgpu_heap image_heap = make_heap(device, CRGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES, "draw attachments");
	if (image_heap)
		f.target = make_image(device, image_heap, CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_TRANSFER_SRC, CRGPU_FORMAT_R8G8B8A8_UNORM, DRAW_SIZE,
		                      DRAW_SIZE, "draw target");
	f.copy = make_image(device, NULL, CRGPU_IMAGE_USAGE_TRANSFER_DST | CRGPU_IMAGE_USAGE_TRANSFER_SRC, CRGPU_FORMAT_R8G8B8A8_UNORM, DRAW_SIZE, DRAW_SIZE,
	                    "draw copy");
	f.depth   = make_image(device, NULL, CRGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT, CRGPU_FORMAT_D32_FLOAT, DRAW_SIZE, DRAW_SIZE, "draw depth");
	f.texture = make_image(device, NULL, CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_TRANSFER_DST, CRGPU_FORMAT_R8G8B8A8_UNORM, 2, 2, "draw texture");
	f.target_view                   = make_image_view(f.target, CRGPU_IMAGE_ASPECT_COLOR);
	f.depth_view                    = make_image_view(f.depth, CRGPU_IMAGE_ASPECT_DEPTH);
	crgpu_image_view texture_view = make_image_view(f.texture, CRGPU_IMAGE_ASPECT_COLOR);
	if (f.target && f.target_view) {
		crgpu_image_desc read;
		memset(&read, 0xA5, sizeof(read));
		CHECK(crgpu_image_get_desc(NULL, &read) == CR_RESULT_GPU_INVALID_IMAGE);
		CHECK(crgpu_image_get_desc(f.target, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_image_get_desc(f.target, &read) == CR_SUCCESS);
		CHECK(read.width == DRAW_SIZE && read.height == DRAW_SIZE && read.format == CRGPU_FORMAT_R8G8B8A8_UNORM && read.mip_levels == 1);
		CHECK(read.debug_name != NULL && strcmp(read.debug_name, "draw target") == 0);
		crgpu_image_view_desc view_read;
		memset(&view_read, 0xA5, sizeof(view_read));
		CHECK(crgpu_image_view_get_desc(NULL, &view_read) == CR_RESULT_GPU_INVALID_IMAGE_VIEW);
		CHECK(crgpu_image_view_get_desc(f.target_view, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_image_view_get_desc(f.target_view, &view_read) == CR_SUCCESS);
		CHECK(view_read.parent_image == f.target && view_read.type == CRGPU_IMAGE_VIEW_TYPE_2D);
		CHECK(view_read.subresource_range.aspect_mask == CRGPU_IMAGE_ASPECT_COLOR && view_read.subresource_range.mip_level_count == 1);
	}
	crgpu_sampler_desc sampler_desc;
	memset(&sampler_desc, 0, sizeof(sampler_desc));
	sampler_desc.base.type      = CR_STRUCT_TYPE_GPU_SAMPLER_DESC;
	sampler_desc.mag_filter     = CRGPU_FILTER_NEAREST;
	sampler_desc.min_filter     = CRGPU_FILTER_NEAREST;
	sampler_desc.mipmap_filter  = CRGPU_FILTER_NEAREST;
	sampler_desc.address_mode_u = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_v = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_w = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.max_lod        = 0.0f;
	sampler_desc.max_anisotropy = 1.0f;
	sampler_desc.debug_name     = "draw sampler";
	crgpu_sampler sampler     = NULL;
	CHECK(crgpu_sampler_create(device, &sampler_desc, &sampler) == CR_SUCCESS);

	// The texels, each row at the pitch a copy wants.
	uint64_t texel_total = 0;
	crgpu_image_get_buffer_copy_size(f.texture, 0, &f.texel_row_pitch, &texel_total);
	CHECK(f.texel_row_pitch >= 8 && texel_total >= f.texel_row_pitch + 8);
	uint64_t alignment = 0;
	CHECK(crgpu_image_get_buffer_copy_offset_alignment(NULL, &alignment) == CR_RESULT_GPU_INVALID_IMAGE);
	if (f.texture) {
		CHECK(crgpu_image_get_buffer_copy_offset_alignment(f.texture, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_image_get_buffer_copy_offset_alignment(f.texture, &alignment) == CR_SUCCESS);
		CHECK(alignment >= 4 && alignment % 4 == 0);
	}
	uint8_t texels[2048];
	memset(texels, 0, sizeof(texels));
	if (texel_total <= sizeof(texels)) {
		const uint8_t top[8]    = {255, 0, 0, 255, 0, 255, 0, 255};
		const uint8_t bottom[8] = {0, 0, 255, 255, 255, 255, 255, 255};
		memcpy(texels, top, 8);
		memcpy(texels + f.texel_row_pitch, bottom, 8);
		f.texel_upload = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, texel_total, texels);
	}
	uint64_t total = 0;
	crgpu_image_get_buffer_copy_size(f.target, 0, &f.row_pitch, &total);
	CHECK(f.row_pitch >= DRAW_SIZE * 4 && total >= f.row_pitch * (DRAW_SIZE - 1) + DRAW_SIZE * 4);
	f.readback_target = make_buffer(device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);
	f.readback_copy   = make_buffer(device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);

	if (f.set && uniform && texture_view && sampler) {
		crgpu_descriptor_buffer_info uniform_info = {uniform, 0, 256};
		crgpu_descriptor_image_info texture_info  = {texture_view, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY};
		crgpu_write_descriptor_set writes[3];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 3; ++i) {
			writes[i].base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = f.set;
			writes[i].dst_binding      = (uint32_t)i;
			writes[i].descriptor_count = 1;
		}
		writes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].buffer_info     = &uniform_info;
		writes[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[1].image_info      = &texture_info;
		writes[2].descriptor_type = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
		writes[2].sampler         = sampler;
		crgpu_device_update_descriptor_sets(device, writes, 3);
	}

	if (f.features & CRGPU_FEATURE_PASS_TIMESTAMPS)
		CHECK(crgpu_timestamp_set_create(device, queue, 2, &f.timestamps) == CR_SUCCESS);
	else
		printf("crgpu_objects_test: the device has no pass timestamps, they are not run\n");

	const bool ready = f.plain && f.depth_tested && f.set && f.vertex_upload && f.vertices && f.corners && f.indices && f.indirect && f.texel_upload &&
	                   f.target_view && f.depth_view && f.copy && f.readback_target && f.readback_copy && instance_buffer && f.instances[7];
	CHECK(ready);
	if (ready) {
		run_once(queue, allocator, cmd, record_uploads, &f);
		run_once(queue, allocator, cmd, record_frame, &f);
		read_back(&f, f.readback_target, total, "target");
		read_back(&f, f.readback_copy, total, "blitted copy");
		if (f.timestamps) {
			uint64_t ns[2] = {0, 0};
			CHECK(crgpu_timestamp_set_get(f.timestamps, 0, 2, ns) == CR_SUCCESS);
			CHECK(ns[1] >= ns[0]);
			printf("crgpu_objects_test: the pass took %llu ns on the GPU\n", (unsigned long long)(ns[1] - ns[0]));
		}
	}

	crgpu_timestamp_set_destroy(f.timestamps);
	crgpu_sampler_destroy(sampler);
	crgpu_image_view_destroy(texture_view);
	crgpu_image_view_destroy(f.depth_view);
	crgpu_image_view_destroy(f.target_view);
	crgpu_image_destroy(f.texture);
	crgpu_image_destroy(f.depth);
	crgpu_image_destroy(f.copy);
	crgpu_image_destroy(f.target);
	crgpu_heap_destroy(image_heap);
	crgpu_buffer_destroy(f.readback_copy);
	crgpu_buffer_destroy(f.readback_target);
	crgpu_buffer_destroy(f.texel_upload);
	crgpu_buffer_destroy(uniform);
	crgpu_buffer_destroy(f.indirect);
	crgpu_buffer_view_destroy(f.indices);
	crgpu_buffer_destroy(index_buffer);
	for (int i = 0; i < 8; ++i)
		crgpu_buffer_view_destroy(f.instances[i]);
	crgpu_buffer_destroy(instance_buffer);
	crgpu_buffer_view_destroy(f.corners);
	crgpu_buffer_destroy(f.vertices);
	crgpu_heap_destroy(buffer_heap);
	crgpu_buffer_destroy(f.vertex_upload);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_graphics_pipeline_destroy(f.meshed);
	crgpu_graphics_pipeline_destroy(f.depth_bounded);
	crgpu_graphics_pipeline_destroy(f.depth_tested);
	crgpu_graphics_pipeline_destroy(f.plain);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(mesh);
	crgpu_shader_module_destroy(fragment);
	crgpu_shader_module_destroy(vertex);
}

// ---- Compute -------------------------------------------------------------------

typedef struct compute_run {
	crgpu_compute_pipeline pipeline;
	crgpu_descriptor_set set;
	crgpu_buffer upload, source, destination, readback;
} compute_run;

#define COMPUTE_COUNT 100u
#define COMPUTE_ADD 7u

static void record_compute(crgpu_command_list cmd, void *context) {
	compute_run *c       = (compute_run *)context;
	const uint64_t bytes = COMPUTE_COUNT * sizeof(uint32_t);
	crgpu_buffer_barrier barriers[2];
	memset(barriers, 0, sizeof(barriers));
	barriers[0].base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	barriers[0].buffer       = c->source;
	barriers[0].state_before = CRGPU_RESOURCE_STATE_COMMON;
	barriers[0].state_after  = CRGPU_RESOURCE_STATE_COPY_DEST;
	crgpu_command_list_pipeline_barrier(cmd, barriers, 1, NULL, 0);
	crgpu_command_list_copy_buffer(cmd, c->upload, c->source, 0, 0, bytes);
	barriers[0].state_before = CRGPU_RESOURCE_STATE_COPY_DEST;
	barriers[0].state_after  = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	barriers[1]              = barriers[0];
	barriers[1].buffer       = c->destination;
	barriers[1].state_before = CRGPU_RESOURCE_STATE_COMMON;
	crgpu_command_list_pipeline_barrier(cmd, barriers, 2, NULL, 0);
	crgpu_command_list_bind_compute_pipeline(cmd, c->pipeline);
	crgpu_command_list_bind_compute_descriptor_sets(cmd, c->pipeline, 0, &c->set, 1);
	const uint32_t push[2] = {COMPUTE_ADD, COMPUTE_COUNT};
	crgpu_command_list_push_compute_constants(cmd, c->pipeline, 0, sizeof(push), push);
	crgpu_command_list_dispatch(cmd, (COMPUTE_COUNT + 63) / 64, 1, 1);
	barriers[0].buffer       = c->destination;
	barriers[0].state_before = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	barriers[0].state_after  = CRGPU_RESOURCE_STATE_COPY_SOURCE;
	crgpu_command_list_pipeline_barrier(cmd, barriers, 1, NULL, 0);
	crgpu_command_list_copy_buffer(cmd, c->destination, c->readback, 0, 0, bytes);
}

static void test_compute(crgpu_device device, crgpu_command_queue queue, crgpu_command_allocator allocator, crgpu_command_list cmd) {
	compute_run c;
	memset(&c, 0, sizeof(c));
	crgpu_shader_module module =
	    make_module(device, &crgpu_test_objects_double_comp, "double");
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < 2; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count            = 2;
	crgpu_descriptor_set_layout layout = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &layout) == CR_SUCCESS);

	crgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module                      = module;
	desc.descriptor_set_layouts[0]           = layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_COMPUTE;
	desc.push_constant_ranges[0].size        = 2 * sizeof(uint32_t);
	desc.push_constant_range_count           = 1;
	desc.debug_name                          = "double";
	if (module && layout)
		CHECK(crgpu_compute_pipeline_create(device, &desc, &c.pipeline) == CR_SUCCESS);
	if (c.pipeline) {
		crgpu_compute_pipeline_desc read;
		memset(&read, 0xA5, sizeof(read));
		CHECK(crgpu_compute_pipeline_get_desc(NULL, &read) == CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE);
		CHECK(crgpu_compute_pipeline_get_desc(c.pipeline, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_compute_pipeline_get_desc(c.pipeline, &read) == CR_SUCCESS);
		CHECK(read.base.type == CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
		CHECK(read.compute_module == module);
		CHECK(read.descriptor_set_layout_count == 1 && read.descriptor_set_layouts[0] == (void *)layout);
		CHECK(read.push_constant_range_count == 1 && read.push_constant_ranges[0].size == 8 &&
		      read.push_constant_ranges[0].stage_flags == CRGPU_SHADER_STAGE_COMPUTE);
		CHECK(read.debug_name != NULL && strcmp(read.debug_name, "double") == 0);
	}

	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(device, &set_desc, &c.set) == CR_SUCCESS);
	}
	uint32_t values[COMPUTE_COUNT];
	for (uint32_t i = 0; i < COMPUTE_COUNT; ++i)
		values[i] = i * 3u + 1u;
	const uint64_t bytes = sizeof(values);
	c.upload             = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, bytes, values);
	c.source             = make_buffer(device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST, bytes, NULL);
	c.destination        = make_buffer(device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_SRC, bytes, NULL);
	c.readback           = make_buffer(device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, bytes, NULL);
	if (c.pipeline && c.set && c.upload && c.source && c.destination && c.readback) {
		crgpu_descriptor_buffer_info infos[2] = {{c.source, 0, bytes}, {c.destination, 0, bytes}};
		crgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 2; ++i) {
			writes[i].base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = c.set;
			writes[i].dst_binding      = (uint32_t)i;
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].buffer_info      = &infos[i];
		}
		crgpu_device_update_descriptor_sets(device, writes, 2);
		// Submitted with crgpu_command_queue_submit(), signalling a fence to 5,
		// and waited for through the fence rather than the queue.
		crgpu_fence fence = NULL;
		CHECK(crgpu_fence_create(device, CRGPU_FENCE_FLAG_NONE, 0, &fence) == CR_SUCCESS);
		CHECK(crgpu_command_allocator_reset(allocator) == CR_SUCCESS);
		crgpu_command_list_begin(cmd);
		record_compute(cmd, &c);
		crgpu_command_list_end(cmd);
		crgpu_submit_desc submit;
		memset(&submit, 0, sizeof(submit));
		submit.base.type          = CR_STRUCT_TYPE_GPU_SUBMIT_DESC;
		submit.cmd_lists          = &cmd;
		submit.cmd_list_count     = 1;
		submit.signal_fence       = fence;
		submit.signal_fence_value = 5;
		CHECK(crgpu_command_queue_submit(NULL, &submit) == CR_RESULT_GPU_INVALID_COMMAND_QUEUE);
		CHECK(crgpu_command_queue_submit(queue, &submit) == CR_SUCCESS);
		if (fence) {
			uint64_t value = 5;
			CHECK(crgpu_fence_wait(device, &fence, &value, 1, true, 10000000000ull) == CR_SUCCESS);
			CHECK(crgpu_fence_get_value(fence) == 5);
		}
		crgpu_fence_destroy(fence);
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(c.readback, 0, bytes, &mapped) == CR_SUCCESS);
		if (mapped) {
			CHECK(crgpu_buffer_invalidate(c.readback, 0, bytes) == CR_SUCCESS);
			const uint32_t *out = (const uint32_t *)mapped;
			uint32_t wrong      = 0;
			for (uint32_t i = 0; i < COMPUTE_COUNT; ++i)
				wrong += out[i] != values[i] * 2u + COMPUTE_ADD;
			CHECK(wrong == 0);
			printf("crgpu_objects_test: the compute shader doubled %u values, %u wrong\n", COMPUTE_COUNT, wrong);
			crgpu_buffer_unmap(c.readback);
		}
	}
	crgpu_buffer_destroy(c.readback);
	crgpu_buffer_destroy(c.destination);
	crgpu_buffer_destroy(c.source);
	crgpu_buffer_destroy(c.upload);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_compute_pipeline_destroy(c.pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
}

// ---- The first use of an attachment image (card S38) ------------------------

// A committed image with color attachment and storage usage, first cleared
// over the whole image (crgpu_image_create(), "the first use of an
// attachment image"), then written by a compute shader through a storage
// view and read back: the left half the shader's values, the right half the
// clear. Under D3D12's debug layer, a not-zeroed render target written first
// by compute is message 1422; cleared first, nothing.
#define FIRST_USE_SIZE 8u

typedef struct first_use_run {
	crgpu_compute_pipeline pipeline;
	crgpu_descriptor_set set;
	crgpu_image image;
	crgpu_image_view view;
	crgpu_buffer readback;
	uint64_t row_pitch;
} first_use_run;

static void record_first_use(crgpu_command_list cmd, void *context) {
	first_use_run *r = (first_use_run *)context;
	crgpu_command_list_image_barrier_view(cmd, r->view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	// The image's own clear value (make_image()), so D3D12 has no mismatch to warn of.
	crgpu_command_list_clear_color_attachment(cmd, r->view, 0.4f, 0.4f, 0.4f, 1.0f, FIRST_USE_SIZE, FIRST_USE_SIZE);
	crgpu_command_list_image_barrier_view(cmd, r->view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_GENERAL);
	crgpu_command_list_bind_compute_pipeline(cmd, r->pipeline);
	crgpu_command_list_bind_compute_descriptor_sets(cmd, r->pipeline, 0, &r->set, 1);
	crgpu_command_list_dispatch(cmd, 1, 1, 1);
	crgpu_command_list_image_barrier_view(cmd, r->view, CRGPU_IMAGE_LAYOUT_GENERAL, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	crgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(r->row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = FIRST_USE_SIZE;
	copy.height            = FIRST_USE_SIZE;
	copy.depth             = 1;
	crgpu_command_list_copy_image_to_buffer(cmd, r->image, r->readback, &copy);
}

static void test_attachment_first_use(crgpu_device device, crgpu_command_queue queue, crgpu_command_allocator allocator,
                                      crgpu_command_list cmd) {
	first_use_run r;
	memset(&r, 0, sizeof(r));
	crgpu_shader_module module = make_module(device, &crgpu_test_objects_first_use_comp, "first use");
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	layout_desc.binding_count               = 1;
	crgpu_descriptor_set_layout layout    = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &layout) == CR_SUCCESS);
	crgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                   = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module              = module;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layout_count = 1;
	desc.debug_name                  = "first use";
	if (module && layout)
		CHECK(crgpu_compute_pipeline_create(device, &desc, &r.pipeline) == CR_SUCCESS);

	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(device, &set_desc, &r.set) == CR_SUCCESS);
	}
	r.image = make_image(device, NULL,
	                     CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_STORAGE | CRGPU_IMAGE_USAGE_TRANSFER_SRC,
	                     CRGPU_FORMAT_R32_FLOAT, FIRST_USE_SIZE, FIRST_USE_SIZE, "first use");
	r.view  = make_image_view(r.image, CRGPU_IMAGE_ASPECT_COLOR);
	uint64_t total = 0;
	if (r.image) {
		crgpu_image_get_buffer_copy_size(r.image, 0, &r.row_pitch, &total);
		CHECK(r.row_pitch >= FIRST_USE_SIZE * 4 && total >= r.row_pitch * (FIRST_USE_SIZE - 1) + FIRST_USE_SIZE * 4);
		r.readback = make_buffer(device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);
	}
	if (r.pipeline && r.set && r.view && r.readback) {
		crgpu_descriptor_image_info info = {r.view, CRGPU_IMAGE_LAYOUT_GENERAL};
		crgpu_write_descriptor_set write;
		memset(&write, 0, sizeof(write));
		write.base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		write.dst_set          = r.set;
		write.dst_binding      = 0;
		write.descriptor_count = 1;
		write.descriptor_type  = CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		write.image_info       = &info;
		crgpu_device_update_descriptor_sets(device, &write, 1);
		run_once(queue, allocator, cmd, record_first_use, &r);
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(r.readback, 0, total, &mapped) == CR_SUCCESS);
		if (mapped) {
			CHECK(crgpu_buffer_invalidate(r.readback, 0, total) == CR_SUCCESS);
			uint32_t wrong = 0;
			for (uint32_t y = 0; y < FIRST_USE_SIZE; ++y)
				for (uint32_t x = 0; x < FIRST_USE_SIZE; ++x) {
					float texel;
					memcpy(&texel, (const uint8_t *)mapped + y * r.row_pitch + x * 4, sizeof(texel));
					const float expected = x < FIRST_USE_SIZE / 2 ? (float)(x + y * FIRST_USE_SIZE + 1) : 0.4f;
					wrong += texel != expected;
				}
			CHECK(wrong == 0);
			printf("crgpu_objects_test: an attachment image cleared, then written by compute: %u texels wrong\n", wrong);
			crgpu_buffer_unmap(r.readback);
		}
	}
	crgpu_buffer_destroy(r.readback);
	crgpu_image_view_destroy(r.view);
	crgpu_image_destroy(r.image);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_compute_pipeline_destroy(r.pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
}

// ---- Every binding of a set (card S27) ----------------------------------------

// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8;
// set 1, for the compute modules, a storage buffer at 0. The whole module
// declares set 0 whole and reads 5 and 8; the others are what Metal
// refuses (crgpu_shader_module_desc, "Every binding of a set").
#define SET_BINDINGS_SIZE 4u

typedef struct set_bindings_run {
	crgpu_graphics_pipeline pipeline;
	crgpu_descriptor_set set;
	crgpu_image texture, target;
	crgpu_image_view target_view;
	crgpu_buffer texel_upload, readback;
	uint64_t texel_row_pitch, row_pitch;
} set_bindings_run;

static void record_set_bindings(crgpu_command_list cmd, void *context) {
	set_bindings_run *r = (set_bindings_run *)context;
	crgpu_command_list_image_barrier(cmd, r->texture, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);
	crgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(r->texel_row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = 1;
	copy.height            = 1;
	copy.depth             = 1;
	crgpu_command_list_copy_buffer_to_image(cmd, r->texel_upload, r->texture, &copy);
	crgpu_command_list_image_barrier(cmd, r->texture, CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	crgpu_command_list_image_barrier_view(cmd, r->target_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	crgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                       = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = r->target_view;
	pass.color_attachments[0].load_op    = CRGPU_LOAD_OP_CLEAR;
	pass.color_attachments[0].store_op   = CRGPU_STORE_OP_STORE;
	pass.color_attachment_count          = 1;
	pass.width                           = SET_BINDINGS_SIZE;
	pass.height                          = SET_BINDINGS_SIZE;
	crgpu_command_list_begin_rendering(cmd, &pass);
	CRGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)SET_BINDINGS_SIZE, (float)SET_BINDINGS_SIZE, 0.0f, 1.0f};
	crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
	CRGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)SET_BINDINGS_SIZE, (float)SET_BINDINGS_SIZE};
	crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);
	crgpu_command_list_bind_graphics_pipeline(cmd, r->pipeline);
	crgpu_command_list_bind_graphics_descriptor_sets(cmd, r->pipeline, 0, &r->set, 1);
	crgpu_command_list_draw(cmd, 3, 0);
	crgpu_command_list_end_rendering(cmd);

	crgpu_command_list_image_barrier_view(cmd, r->target_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(r->row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = SET_BINDINGS_SIZE;
	copy.height            = SET_BINDINGS_SIZE;
	copy.depth             = 1;
	crgpu_command_list_copy_image_to_buffer(cmd, r->target, r->readback, &copy);
}

// A graphics pipeline of [fragment] over the vertex module with the set's
// layout, no vertex input and no depth. Returns what the create returned.
static CR_RESULT set_bindings_pipeline(crgpu_device device, crgpu_shader_module vertex, crgpu_shader_module fragment,
                                        crgpu_descriptor_set_layout layout, crgpu_graphics_pipeline *out_pipeline) {
	crgpu_graphics_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                   = CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC;
	desc.vertex_module               = vertex;
	desc.fragment_module             = fragment;
	desc.primitive_topology          = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                   = CRGPU_CULL_MODE_NONE;
	desc.color_attachment_formats[0] = CRGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count      = 1;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layout_count = 1;
	desc.debug_name                  = "set bindings";
	*out_pipeline                    = NULL;
	return crgpu_graphics_pipeline_create(device, &desc, out_pipeline);
}

// A compute pipeline of [module] with both layouts. Returns what the create
// returned.
static CR_RESULT set_bindings_compute(crgpu_device device, crgpu_shader_module module, crgpu_descriptor_set_layout layout,
                                       crgpu_descriptor_set_layout result_layout, crgpu_compute_pipeline *out_pipeline) {
	crgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                   = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module              = module;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layouts[1]   = result_layout;
	desc.descriptor_set_layout_count = 2;
	desc.debug_name                  = "set bindings compute";
	*out_pipeline                    = NULL;
	return crgpu_compute_pipeline_create(device, &desc, out_pipeline);
}

static void test_set_bindings(crgpu_device device, crgpu_command_queue queue, crgpu_command_allocator allocator, crgpu_command_list cmd) {
	// Metal sees a module's bindings and refuses one that isn't its set's
	// layout. Vulkan and D3D12 don't check (card S27): there a module that
	// breaks the rule is the caller's error, which their runtimes may refuse
	// or accept, and their validation reports - so only the modules that keep
	// the rule are made there (D3D12's runtime refused two that don't on
	// 2026-11-02, the ThinkPad).
	const bool checked        = test_shader_format == CRGPU_SHADER_FORMAT_METALLIB;
	const CR_RESULT refused  = CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH;
	set_bindings_run r;
	memset(&r, 0, sizeof(r));

	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	const uint32_t bindings[3]              = {0, 5, 8};
	const CRGPU_DESCRIPTOR_TYPE types[3] = {CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	                                          CRGPU_DESCRIPTOR_TYPE_SAMPLER};
	for (uint32_t i = 0; i < 3; ++i) {
		layout_desc.bindings[i].binding         = bindings[i];
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = CRGPU_SHADER_STAGE_FRAGMENT | CRGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count            = 3;
	crgpu_descriptor_set_layout layout = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &layout) == CR_SUCCESS);
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	layout_desc.binding_count               = 1;
	crgpu_descriptor_set_layout result_layout = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &result_layout) == CR_SUCCESS);

	crgpu_shader_module vertex = make_module(device, &crgpu_test_bindings_vert, "bindings vertex");
	const crgpu_test_shader *fragments[5] = {&crgpu_test_bindings_whole_frag, &crgpu_test_bindings_part_frag, &crgpu_test_bindings_extra_frag,
	                                           &crgpu_test_bindings_type_frag, &crgpu_test_bindings_count_frag};
	const crgpu_test_shader *computes[5]  = {&crgpu_test_bindings_whole_comp, &crgpu_test_bindings_none_comp, &crgpu_test_bindings_part_comp,
                                              &crgpu_test_bindings_extra_comp, &crgpu_test_bindings_type_comp};
	crgpu_shader_module fragment_modules[5] = {NULL, NULL, NULL, NULL, NULL};
	if (vertex && layout && result_layout) {
		for (int i = 0; i < 5; ++i) {
			if (i != 0 && !checked)
				continue;
			fragment_modules[i]              = make_module(device, fragments[i], fragments[i]->entry_point);
			crgpu_graphics_pipeline pipeline = NULL;
			if (fragment_modules[i]) {
				const CR_RESULT sr = set_bindings_pipeline(device, vertex, fragment_modules[i], layout, &pipeline);
				// The whole set made; part of it, a binding past it, one of
				// another type and one of another count refused (on Metal,
				// the one backend that makes them), leaving no pipeline.
				CHECK(sr == (i == 0 ? CR_SUCCESS : refused));
				CHECK((sr == CR_SUCCESS) == (pipeline != NULL));
				if (i == 0)
					r.pipeline = pipeline;
				else
					crgpu_graphics_pipeline_destroy(pipeline);
			}
		}
		for (int i = 0; i < 5; ++i) {
			if (i >= 2 && !checked)
				continue;
			crgpu_shader_module module      = make_module(device, computes[i], computes[i]->entry_point);
			crgpu_compute_pipeline pipeline = NULL;
			if (module) {
				const CR_RESULT sr = set_bindings_compute(device, module, layout, result_layout, &pipeline);
				// The whole set, and none of set 0, made; the rest refused
				// (made on Metal only).
				CHECK(sr == (i < 2 ? CR_SUCCESS : refused));
				CHECK((sr == CR_SUCCESS) == (pipeline != NULL));
			}
			crgpu_compute_pipeline_destroy(pipeline);
			crgpu_shader_module_destroy(module);
		}
	}

	// The whole module's draw reads the image's texel, not another member
	// of the argument buffer (the case that read wrong on Metal before).
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets  = 1;
	for (uint32_t i = 0; i < 3; ++i) {
		pool_desc.pool_sizes[i].descriptor_type = types[i];
		pool_desc.pool_sizes[i].count           = 1;
	}
	pool_desc.pool_size_count    = 3;
	crgpu_descriptor_pool pool = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(device, &set_desc, &r.set) == CR_SUCCESS);
	}
	r.texture = make_image(device, NULL, CRGPU_IMAGE_USAGE_SAMPLED | CRGPU_IMAGE_USAGE_TRANSFER_DST, CRGPU_FORMAT_R8G8B8A8_UNORM, 1, 1,
	                       "set bindings texture");
	r.target  = make_image(device, NULL, CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_TRANSFER_SRC, CRGPU_FORMAT_R8G8B8A8_UNORM,
	                       SET_BINDINGS_SIZE, SET_BINDINGS_SIZE, "set bindings target");
	crgpu_image_view texture_view = make_image_view(r.texture, CRGPU_IMAGE_ASPECT_COLOR);
	r.target_view                   = make_image_view(r.target, CRGPU_IMAGE_ASPECT_COLOR);
	// The uniform buffer the module doesn't read, filled with what reading
	// it in the image's place would show.
	const float unread[4]  = {1.0f, 0.0f, 1.0f, 1.0f};
	crgpu_buffer uniform = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_UNIFORM, 256, NULL);
	if (uniform) {
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(uniform, 0, 256, &mapped) == CR_SUCCESS);
		if (mapped) {
			memcpy(mapped, unread, sizeof(unread));
			CHECK(crgpu_buffer_flush(uniform, 0, sizeof(unread)) == CR_SUCCESS);
			crgpu_buffer_unmap(uniform);
		}
	}
	crgpu_sampler_desc sampler_desc;
	memset(&sampler_desc, 0, sizeof(sampler_desc));
	sampler_desc.base.type      = CR_STRUCT_TYPE_GPU_SAMPLER_DESC;
	sampler_desc.mag_filter     = CRGPU_FILTER_NEAREST;
	sampler_desc.min_filter     = CRGPU_FILTER_NEAREST;
	sampler_desc.mipmap_filter  = CRGPU_FILTER_NEAREST;
	sampler_desc.address_mode_u = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_v = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_w = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.max_anisotropy = 1.0f;
	sampler_desc.debug_name     = "set bindings sampler";
	crgpu_sampler sampler     = NULL;
	CHECK(crgpu_sampler_create(device, &sampler_desc, &sampler) == CR_SUCCESS);
	uint64_t texel_total = 0;
	crgpu_image_get_buffer_copy_size(r.texture, 0, &r.texel_row_pitch, &texel_total);
	uint8_t texels[512];
	memset(texels, 0, sizeof(texels));
	const uint8_t texel[4] = {64, 128, 192, 255};
	memcpy(texels, texel, sizeof(texel));
	if (texel_total >= sizeof(texel) && texel_total <= sizeof(texels))
		r.texel_upload = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, texel_total, texels);
	uint64_t total = 0;
	crgpu_image_get_buffer_copy_size(r.target, 0, &r.row_pitch, &total);
	r.readback = make_buffer(device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);

	if (r.set && uniform && texture_view && sampler) {
		crgpu_descriptor_buffer_info uniform_info = {uniform, 0, 256};
		crgpu_descriptor_image_info texture_info  = {texture_view, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY};
		crgpu_write_descriptor_set writes[3];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 3; ++i) {
			writes[i].base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = r.set;
			writes[i].dst_binding      = bindings[i];
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = types[i];
		}
		writes[0].buffer_info = &uniform_info;
		writes[1].image_info  = &texture_info;
		writes[2].sampler     = sampler;
		crgpu_device_update_descriptor_sets(device, writes, 3);
	}
	const bool ready = r.pipeline && r.set && r.texel_upload && r.target_view && r.readback;
	CHECK(ready);
	if (ready) {
		run_once(queue, allocator, cmd, record_set_bindings, &r);
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(r.readback, 0, total, &mapped) == CR_SUCCESS);
		if (mapped) {
			CHECK(crgpu_buffer_invalidate(r.readback, 0, total) == CR_SUCCESS);
			const uint8_t *pixels = (const uint8_t *)mapped;
			CHECK(pixel_is(pixels, (uint32_t)r.row_pitch, 0, 0, 64, 128, 192, 255));
			CHECK(pixel_is(pixels, (uint32_t)r.row_pitch, SET_BINDINGS_SIZE - 1, SET_BINDINGS_SIZE - 1, 64, 128, 192, 255));
			printf("crgpu_objects_test: the set's padded module read its image's texel\n");
			crgpu_buffer_unmap(r.readback);
		}
	}

	crgpu_buffer_destroy(r.readback);
	crgpu_buffer_destroy(r.texel_upload);
	crgpu_sampler_destroy(sampler);
	crgpu_buffer_destroy(uniform);
	crgpu_image_view_destroy(r.target_view);
	crgpu_image_view_destroy(texture_view);
	crgpu_image_destroy(r.target);
	crgpu_image_destroy(r.texture);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_graphics_pipeline_destroy(r.pipeline);
	for (int i = 0; i < 5; ++i)
		crgpu_shader_module_destroy(fragment_modules[i]);
	crgpu_shader_module_destroy(vertex);
	crgpu_descriptor_set_layout_destroy(result_layout);
	crgpu_descriptor_set_layout_destroy(layout);
}

// A device with the features the frame can use that [adapter] has, and the
// frame and the compute shader on it.
static void test_drawing(crgpu_adapter adapter, CRGPU_FEATURES supported) {
	crgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = supported & (CRGPU_FEATURE_PASS_TIMESTAMPS | CRGPU_FEATURE_DEPTH_BOUNDS_TEST | CRGPU_FEATURE_MESH_SHADING |
                                          CRGPU_FEATURE_MULTI_DRAW_INDIRECT);
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "crgpu_objects_test drawing";
	crgpu_device device   = NULL;
	CHECK(crgpu_device_create(adapter, &desc, &device) == CR_SUCCESS);
	if (!device)
		return;
	CHECK(crgpu_device_get_features(device) == desc.required_features);
	crgpu_command_queue queue = NULL;
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == CR_SUCCESS);
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	crgpu_command_allocator allocator = NULL;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_SUCCESS);
	crgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(crgpu_command_list_create(allocator, &cmd) == CR_SUCCESS);
	if (queue && cmd) {
		test_draw_frame(device, queue, allocator, cmd);
		test_compute(device, queue, allocator, cmd);
		test_attachment_first_use(device, queue, allocator, cmd);
		test_set_bindings(device, queue, allocator, cmd);
	}
	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(allocator);
	crgpu_device_destroy(device);
}

// ---- Ray queries (CRGPU_EXT_RAY_QUERY) --------------------------------------

#if CRGPU_EXT_RAY_QUERY


static crgpu_acceleration_structure_geometry rq_triangles(crgpu_buffer vertices) {
	crgpu_acceleration_structure_geometry g;
	memset(&g, 0, sizeof(g));
	g.kind        = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	g.flags       = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
	g.data        = vertices;
	g.data_stride = 12;
	g.data_count  = 3;
	return g;
}

static crgpu_acceleration_structure_geometry rq_boxes(crgpu_buffer boxes) {
	crgpu_acceleration_structure_geometry g;
	memset(&g, 0, sizeof(g));
	g.kind        = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS;
	g.data        = boxes;
	g.data_stride = 24;
	g.data_count  = 1;
	return g;
}

static crgpu_acceleration_structure_inputs rq_bottom_inputs(const crgpu_acceleration_structure_geometry *geometry,
                                                              CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	crgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level          = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs.flags          = flags;
	inputs.geometries     = geometry;
	inputs.geometry_count = 1;
	return inputs;
}

static crgpu_acceleration_structure_inputs rq_top_inputs(crgpu_buffer instances, uint64_t offset, uint32_t count,
                                                           const crgpu_acceleration_structure *bottoms, uint32_t bottom_count,
                                                           CRGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	crgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type          = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level              = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	inputs.flags              = flags;
	inputs.instances          = instances;
	inputs.instance_offset    = offset;
	inputs.instance_count     = count;
	inputs.bottom_levels      = bottoms;
	inputs.bottom_level_count = bottom_count;
	return inputs;
}

static crgpu_acceleration_structure_desc rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL level, uint64_t size, uint32_t max_bottom_levels,
                                                   bool allow_compaction, const char *name) {
	crgpu_acceleration_structure_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type         = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	desc.level             = level;
	desc.size              = size;
	desc.max_bottom_levels = max_bottom_levels;
	desc.allow_compaction  = allow_compaction;
	desc.debug_name        = name;
	return desc;
}

static crgpu_acceleration_structure_sizes rq_sizes_out(void) {
	crgpu_acceleration_structure_sizes sizes;
	memset(&sizes, 0, sizeof(sizes));
	sizes.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	return sizes;
}

static crgpu_acceleration_structure_instance rq_instance(float x, float y, float z, uint32_t user_index, uint8_t mask, uint64_t reference) {
	crgpu_acceleration_structure_instance instance;
	memset(&instance, 0, sizeof(instance));
	instance.transform[0]           = 1.0f;
	instance.transform[3]           = x;
	instance.transform[5]           = 1.0f;
	instance.transform[7]           = y;
	instance.transform[10]          = 1.0f;
	instance.transform[11]          = z;
	instance.user_index             = user_index;
	instance.mask                   = mask;
	instance.bottom_level_reference = reference;
	return instance;
}

/* On a device created without CRGPU_FEATURE_RAY_QUERY: every call of the
 * extension refused with its result, a heap of acceleration structures and
 * a descriptor of one refused, and a buffer of build inputs made. On every
 * adapter, so the Iris Xe's refusals and a ray tracing GPU's are the same
 * test. */
static void test_ray_query_without_feature(crgpu_device device) {
	CHECK(!(crgpu_device_get_features(device) & CRGPU_FEATURE_RAY_QUERY));
	const float vertices[9]        = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	crgpu_buffer input           = make_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT,
	                                             sizeof(vertices), vertices);
	crgpu_acceleration_structure_geometry geometry = rq_triangles(input);
	crgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, 0);
	crgpu_acceleration_structure_sizes sizes       = rq_sizes_out();
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(device, &inputs, &sizes) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	crgpu_acceleration_structure_desc desc = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, 4096, 0, false, "refused");
	crgpu_acceleration_structure structure = NULL;
	CHECK(crgpu_acceleration_structure_create(device, &desc, &structure) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(structure == NULL);

	crgpu_heap_desc heap_desc;
	memset(&heap_desc, 0, sizeof(heap_desc));
	heap_desc.base.type      = CR_STRUCT_TYPE_GPU_HEAP_DESC;
	heap_desc.memory_kind    = CRGPU_MEMORY_KIND_DEVICE;
	heap_desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES;
	heap_desc.size           = CRGPU_HEAP_SIZE_GRANULARITY;
	crgpu_heap heap        = NULL;
	CHECK(crgpu_heap_create(device, &heap_desc, &heap) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(heap == NULL);
	// A heap of buffers: the feature is checked before the heap's class.
	heap_desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	CHECK(crgpu_heap_create(device, &heap_desc, &heap) == CR_SUCCESS);
	if (heap)
		CHECK(crgpu_acceleration_structure_create_placed(heap, 0, &desc, &structure) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	crgpu_heap_destroy(heap);

	crgpu_acceleration_structure_instance_layout layout;
	CHECK(crgpu_device_get_acceleration_structure_instance_layout(device, &layout) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	const crgpu_acceleration_structure_instance instance = rq_instance(0, 0, 0, 1, 0xFF, 0);
	uint8_t bytes[72];
	CHECK(crgpu_device_write_acceleration_structure_instances(device, &instance, 1, bytes) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(crgpu_acceleration_structure_get_reference(NULL, NULL) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	crgpu_acceleration_structure_destroy(NULL);

	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	crgpu_command_allocator allocator = NULL;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_SUCCESS);
	crgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(crgpu_command_list_create(allocator, &cmd) == CR_SUCCESS);
	if (cmd) {
		crgpu_command_list_begin(cmd);
		crgpu_acceleration_structure_build build;
		memset(&build, 0, sizeof(build));
		build.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode      = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs    = &inputs;
		CHECK(crgpu_command_list_build_acceleration_structures(NULL, &build, 1) == CR_RESULT_GPU_INVALID_COMMAND_LIST);
		CHECK(crgpu_command_list_build_acceleration_structures(cmd, &build, 1) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		CHECK(crgpu_command_list_copy_acceleration_structure(cmd, NULL, NULL, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
		      CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(cmd, NULL, 0, NULL, 0) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		crgpu_command_list_acceleration_structure_barrier(cmd, NULL, 0);
		crgpu_command_list_end(cmd);
	}
	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(allocator);

	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	layout_desc.binding_count               = 1;
	crgpu_descriptor_set_layout set_layout = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &set_layout) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_RESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	// A type that is none, the same refusal on every backend.
	layout_desc.bindings[0].descriptor_type = 99;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &set_layout) == CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE);
	pool_desc.pool_sizes[0].descriptor_type = 99;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_RESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE);
	crgpu_buffer_destroy(input);
	printf("crgpu_objects_test: ray queries: every call refused on a device without CRGPU_FEATURE_RAY_QUERY\n");
}

// The grid rq_trace.slang traces: RQ_W x RQ_H rays along +z from z = -1,
// RQ_STEP apart, the first at (RQ_X0 + RQ_STEP / 2, RQ_Y0 + RQ_STEP / 2),
// off every edge of the scene by more than RQ_EDGE.
#define RQ_W 16u
#define RQ_H 8u
#define RQ_STEP 0.25f
#define RQ_X0 0.03f
#define RQ_Y0 0.01f
#define RQ_EDGE 0.01f
#define RQ_TRIANGLE_ID 7u
#define RQ_BOX_ID 11u

/* rq_trace.slang's RqHit, as it stores it. */
typedef struct rq_hit {
	float t;
	uint32_t instance_index;
	uint32_t instance_id;
	uint32_t geometry_index;
	uint32_t primitive_index;
	uint32_t kind;
	float barycentrics[2];
} rq_hit;

/* rq_trace.slang's RqParams. */
typedef struct rq_trace_params {
	float origin[4];
	float step_and_tmax[4];
	uint32_t width;
	uint32_t height;
	uint32_t cull_mask;
	uint32_t ray_flags;
} rq_trace_params;

/* The hit rq_trace makes at pixel (x, y) of a scene of instance 0 (the
 * triangle (0,0,0) (1,0,0) (0,1,0), moved by [triangle]) and instance 1 (the
 * sphere in the box [0,1]^3, moved by [box]), seen through [mask]. *skip is
 * set where the ray passes within RQ_EDGE of an edge. */
static rq_hit rq_expected(uint32_t x, uint32_t y, const float triangle[3], const float box[3], uint32_t mask, bool *skip) {
	const float px = RQ_X0 + ((float)x + 0.5f) * RQ_STEP;
	const float py = RQ_Y0 + ((float)y + 0.5f) * RQ_STEP;
	rq_hit hit;
	memset(&hit, 0, sizeof(hit));
	hit.t = -1.0f;
	*skip = false;
	if (mask & 0x01u) {
		const float u = px - triangle[0], v = py - triangle[1];
		const float edge = fminf(fminf(fabsf(u), fabsf(v)), fabsf(u + v - 1.0f));
		if (edge < RQ_EDGE)
			*skip = true;
		if (u > 0.0f && v > 0.0f && u + v < 1.0f) {
			hit.t               = triangle[2] + 1.0f;
			hit.instance_index  = 0;
			hit.instance_id     = RQ_TRIANGLE_ID;
			hit.kind            = 1;
			hit.barycentrics[0] = u;
			hit.barycentrics[1] = v;
		}
	}
	if (mask & 0x02u) {
		const float dx = px - (box[0] + 0.5f), dy = py - (box[1] + 0.5f);
		const float d2 = dx * dx + dy * dy;
		if (fabsf(sqrtf(d2) - 0.5f) < RQ_EDGE)
			*skip = true;
		if (d2 < 0.25f) {
			const float t = box[2] + 0.5f - sqrtf(0.25f - d2) + 1.0f;
			if (hit.kind == 0 || t < hit.t) {
				memset(&hit, 0, sizeof(hit));
				hit.t              = t;
				hit.instance_index = 1;
				hit.instance_id    = RQ_BOX_ID;
				hit.kind           = 2;
			}
		}
	}
	return hit;
}

/* Every ray's hit against rq_expected; returns how many differ. */
static uint32_t rq_check_hits(const rq_hit *hits, const float triangle[3], const float box[3], uint32_t mask, uint32_t *out_hit_count) {
	uint32_t wrong = 0, hit_count = 0;
	for (uint32_t y = 0; y < RQ_H; ++y)
		for (uint32_t x = 0; x < RQ_W; ++x) {
			bool skip         = false;
			const rq_hit want = rq_expected(x, y, triangle, box, mask, &skip);
			const rq_hit *got = &hits[y * RQ_W + x];
			if (got->kind != 0)
				++hit_count;
			if (skip)
				continue;
			bool same = got->kind == want.kind;
			if (same && want.kind != 0) {
				same = fabsf(got->t - want.t) < 1e-3f && got->instance_index == want.instance_index && got->instance_id == want.instance_id &&
				       got->geometry_index == 0 && got->primitive_index == 0;
				if (want.kind == 1)
					same = same && fabsf(got->barycentrics[0] - want.barycentrics[0]) < 1e-3f &&
					       fabsf(got->barycentrics[1] - want.barycentrics[1]) < 1e-3f;
			}
			if (!same) {
				if (wrong < 4)
					fprintf(stderr, "  ray (%u, %u): kind %u t %f instance %u id %u, expected kind %u t %f instance %u id %u\n", x, y, got->kind,
					        got->t, got->instance_index, got->instance_id, want.kind, want.t, want.instance_index, want.instance_id);
				++wrong;
			}
		}
	*out_hit_count = hit_count;
	return wrong;
}

/* What the ray query tests share on one device. */
typedef struct rq_run {
	crgpu_device device;
	crgpu_command_queue queue;
	crgpu_command_allocator allocator;
	crgpu_command_list cmd;
	CRGPU_RAY_QUERY_LIMITS limits;
	crgpu_acceleration_structure_instance_layout layout;
	// UPLOAD, build inputs: the triangle's vertices, the box, the
	// instances (written by the CPU call).
	crgpu_buffer vertices, boxes, instances;
	// DEVICE, storage: every build's scratch, each at its own offset.
	crgpu_buffer scratch;
	uint64_t scratch_bottom[2], scratch_top, scratch_update;
	// The hits, and their copy for the CPU.
	crgpu_buffer hits, hits_readback;
	crgpu_acceleration_structure triangle, box, top;
	crgpu_acceleration_structure_sizes triangle_sizes, box_sizes, top_sizes;
	crgpu_shader_module trace_module;
	crgpu_descriptor_set_layout set_layout;
	crgpu_descriptor_pool pool;
	crgpu_descriptor_set set;
	crgpu_compute_pipeline trace;
} rq_run;

static uint64_t rq_align(uint64_t value, uint64_t alignment) {
	return (value + alignment - 1) / alignment * alignment;
}

static crgpu_buffer_barrier rq_buffer_barrier(crgpu_buffer buffer, CRGPU_RESOURCE_STATE before, CRGPU_RESOURCE_STATE after) {
	crgpu_buffer_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	b.buffer       = buffer;
	b.state_before = before;
	b.state_after  = after;
	return b;
}

static void rq_barrier(crgpu_command_list cmd, crgpu_acceleration_structure structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS before,
                       CRGPU_ACCELERATION_STRUCTURE_ACCESS after) {
	crgpu_acceleration_structure_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
	b.structure     = structure;
	b.access_before = before;
	b.access_after  = after;
	crgpu_command_list_acceleration_structure_barrier(cmd, &b, 1);
}

static crgpu_acceleration_structure_build rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE mode, const crgpu_acceleration_structure_inputs *inputs,
                                                     crgpu_acceleration_structure source, crgpu_acceleration_structure destination,
                                                     crgpu_buffer scratch, uint64_t scratch_offset) {
	crgpu_acceleration_structure_build build;
	memset(&build, 0, sizeof(build));
	build.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
	build.mode           = mode;
	build.inputs         = inputs;
	build.source         = source;
	build.destination    = destination;
	build.scratch        = scratch;
	build.scratch_offset = scratch_offset;
	return build;
}

/* Binds rq_trace, its set and [mask]'s push constants, and dispatches it:
 * the hits written into the set's buffer, in UNORDERED_ACCESS. */
static void rq_dispatch_trace(rq_run *r, crgpu_command_list cmd, uint32_t mask) {
	crgpu_command_list_bind_compute_pipeline(cmd, r->trace);
	crgpu_command_list_bind_compute_descriptor_sets(cmd, r->trace, 0, &r->set, 1);
	rq_trace_params params;
	memset(&params, 0, sizeof(params));
	params.origin[0]        = RQ_X0;
	params.origin[1]        = RQ_Y0;
	params.origin[2]        = -1.0f;
	params.origin[3]        = 0.0f;
	params.step_and_tmax[0] = RQ_STEP;
	params.step_and_tmax[1] = RQ_STEP;
	params.step_and_tmax[2] = 100.0f;
	params.width            = RQ_W;
	params.height           = RQ_H;
	params.cull_mask        = mask;
	params.ray_flags        = 0;
	crgpu_command_list_push_compute_constants(cmd, r->trace, 0, sizeof(params), &params);
	crgpu_command_list_dispatch(cmd, (RQ_W + 7) / 8, (RQ_H + 7) / 8, 1);
}

/* Records rq_trace over [r]'s set (its top level, the hits) with [mask],
 * and the hits' copy for the CPU. The top level has had its barrier to
 * TRACE. Binds everything it uses, as it must after a build
 * (crgpu_command_list_build_acceleration_structures()). */
static void rq_record_trace(rq_run *r, crgpu_command_list cmd, uint32_t mask) {
	crgpu_buffer_barrier b = rq_buffer_barrier(r->hits, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	crgpu_command_list_pipeline_barrier(cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, cmd, mask);
	b = rq_buffer_barrier(r->hits, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS, CRGPU_RESOURCE_STATE_COPY_SOURCE);
	crgpu_command_list_pipeline_barrier(cmd, &b, 1, NULL, 0);
	crgpu_command_list_copy_buffer(cmd, r->hits, r->hits_readback, 0, 0, sizeof(rq_hit) * RQ_W * RQ_H);
}

static void rq_submit(rq_run *r) {
	crgpu_command_list_end(r->cmd);
	CHECK(crgpu_command_queue_submit_lists(r->queue, &r->cmd, 1) == CR_SUCCESS);
	crgpu_command_queue_wait_idle(r->queue);
}

static void rq_begin(rq_run *r) {
	CHECK(crgpu_command_allocator_reset(r->allocator) == CR_SUCCESS);
	crgpu_command_list_begin(r->cmd);
}

/* The hits read back, checked against the scene with the triangle at
 * [triangle] and the box at [box], through [mask]. */
static void rq_check(rq_run *r, const float triangle[3], const float box[3], uint32_t mask, const char *what) {
	void *mapped        = NULL;
	const uint64_t size = sizeof(rq_hit) * RQ_W * RQ_H;
	CHECK(crgpu_buffer_map(r->hits_readback, 0, size, &mapped) == CR_SUCCESS);
	if (!mapped)
		return;
	CHECK(crgpu_buffer_invalidate(r->hits_readback, 0, size) == CR_SUCCESS);
	uint32_t hit_count   = 0;
	const uint32_t wrong = rq_check_hits((const rq_hit *)mapped, triangle, box, mask, &hit_count);
	crgpu_buffer_unmap(r->hits_readback);
	CHECK(wrong == 0);
	CHECK(hit_count > 0 || mask == 0);
	printf("crgpu_objects_test: ray queries: %s: %u rays, %u hits, %u wrong\n", what, RQ_W * RQ_H, hit_count, wrong);
}

/* Writes [count] instances through the CPU call into [r]'s mapped
 * instance buffer at [offset]. */
static void rq_write_instances(rq_run *r, const crgpu_acceleration_structure_instance *instances, uint32_t count, uint64_t offset) {
	void *mapped        = NULL;
	const uint64_t size = (uint64_t)r->layout.size * count;
	CHECK(crgpu_buffer_map(r->instances, offset, size, &mapped) == CR_SUCCESS);
	if (!mapped)
		return;
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, instances, count, mapped) == CR_SUCCESS);
	CHECK(crgpu_buffer_flush(r->instances, offset, size) == CR_SUCCESS);
	crgpu_buffer_unmap(r->instances);
}

/* The write call: its refusals in order, and the bytes it writes in the
 * device's layout, checked against the layout crgpu.h states. */
static void rq_test_write_instances(rq_run *r) {
	crgpu_acceleration_structure_instance instances[2];
	instances[0]              = rq_instance(1.5f, -2.0f, 3.25f, 0x123456u, 0xA5, UINT64_C(0x1122334455667788));
	instances[0].transform[1] = 0.5f;
	instances[0].transform[4] = -0.5f;
	instances[0].table_offset = 0x654321u;
	instances[0].flags        = CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE | CRGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING;
	instances[1]              = rq_instance(0, 0, 0, 0, 0xFF, 0);
	uint8_t bytes[2 * 72];
	memset(bytes, 0xCD, sizeof(bytes));
	CHECK(crgpu_device_write_acceleration_structure_instances(NULL, instances, 2, bytes) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, NULL, 2, bytes) == CR_RESULT_NULL_DESC);
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, instances, 0, bytes) == CR_RESULT_ZERO_SIZE);
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, instances, 2, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	crgpu_acceleration_structure_instance bad[2] = {instances[0], instances[1]};
	bad[1].user_index                              = 1u << 24;
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, bad, 2, bytes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	bad[1]              = instances[1];
	bad[1].table_offset = 1u << 24;
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, bad, 2, bytes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	bad[1]       = instances[1];
	bad[1].flags = 1u << 4;
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, bad, 2, bytes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	bad[1].flags = CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE | CRGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE;
	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, bad, 2, bytes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	// Nothing written by a refusal, the first instance's bytes included.
	bool untouched = true;
	for (size_t i = 0; i < sizeof(bytes); ++i)
		untouched = untouched && bytes[i] == 0xCD;
	CHECK(untouched);

	CHECK(crgpu_device_write_acceleration_structure_instances(r->device, instances, 2, bytes) == CR_SUCCESS);
	const crgpu_acceleration_structure_instance *in = &instances[0];
	uint32_t words[18];
	memcpy(words, bytes, sizeof(words));
	uint64_t reference = 0;
	bool right         = true;
	if (r->layout.kind == CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64) {
		right = right && memcmp(bytes, in->transform, 48) == 0;
		right = right && words[12] == (0x123456u | (0xA5u << 24));
		right = right && words[13] == (0x654321u | ((uint32_t)in->flags << 24));
		memcpy(&reference, bytes + 56, 8);
	} else {
		for (uint32_t c = 0; c < 4; ++c)
			for (uint32_t row = 0; row < 3; ++row) {
				float f = 0.0f;
				memcpy(&f, bytes + (c * 3 + row) * 4, 4);
				right = right && f == in->transform[row * 4 + c];
			}
		right = right && words[12] == in->flags && words[13] == 0xA5u && words[14] == 0x654321u && words[15] == 0x123456u;
		memcpy(&reference, bytes + 64, 8);
	}
	right = right && reference == UINT64_C(0x1122334455667788);
	CHECK(right);
	printf("crgpu_objects_test: ray queries: instances written in layout %u (%u bytes, aligned %u): %s\n", r->layout.kind, r->layout.size,
	       r->layout.alignment, right ? "as the layout states" : "WRONG");
}

/* The size query's refusals, in its order. */
static void rq_test_size_refusals(rq_run *r) {
	crgpu_acceleration_structure_geometry geometry = rq_triangles(r->vertices);
	crgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, 0);
	crgpu_acceleration_structure_sizes sizes       = rq_sizes_out();
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(NULL, &inputs, &sizes) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, NULL, &sizes) == CR_RESULT_NULL_DESC);
	inputs.base.type = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_STRUCT_WRONG_TYPE);
	inputs.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	sizes.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_STRUCT_WRONG_TYPE);
	sizes = rq_sizes_out();
	// The inputs, field by field.
	inputs.level = CRGPU_ACCELERATION_STRUCTURE_LEVEL_NONE;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.level = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs.flags = CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.flags = 1u << 7;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.flags          = 0;
	inputs.instance_count = 1;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.instance_count = 0;
	inputs.geometries     = NULL;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_NULL_DESC);
	inputs.geometries     = &geometry;
	inputs.geometry_count = 0;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_ZERO_SIZE);
	inputs.geometry_count = 1;
	geometry.kind         = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_NONE;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	geometry       = rq_triangles(r->vertices);
	geometry.flags = 1u << 5;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	geometry      = rq_triangles(NULL);
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_BUFFER);
	geometry      = rq_triangles(r->scratch);
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_BUFFER_USAGE);
	geometry             = rq_triangles(r->vertices);
	geometry.data_offset = 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_offset = 0;
	geometry.data_stride = 8;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_stride = 12;
	geometry.data_count  = 0;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_ZERO_SIZE);
	geometry.data_count = 6;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	geometry.data_count   = 3;
	geometry.index_format = CRGPU_FORMAT_R8_UINT;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	geometry.index_format = CRGPU_FORMAT_UNKNOWN;
	geometry.indices      = r->vertices;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	geometry.index_format = CRGPU_FORMAT_R32_UINT;
	geometry.indices      = NULL;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_BUFFER);
	geometry.indices      = r->vertices;
	geometry.index_offset = 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.index_offset = 0;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_ZERO_SIZE);
	geometry.index_count = 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	geometry.index_count = 12;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	geometry            = rq_triangles(r->vertices);
	geometry.data_count = 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	// AABBs: their own alignment and stride.
	geometry             = rq_boxes(r->boxes);
	geometry.data_stride = 16;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_stride  = 24;
	geometry.index_format = CRGPU_FORMAT_R16_UINT;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	// Two geometries of two kinds.
	crgpu_acceleration_structure_geometry mixed[2] = {rq_triangles(r->vertices), rq_boxes(r->boxes)};
	inputs.geometries                                = mixed;
	inputs.geometry_count                            = 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.geometry_count = (uint32_t)(r->limits.max_geometries > UINT32_MAX ? UINT32_MAX : r->limits.max_geometries + 1);
	if (r->limits.max_geometries < UINT32_MAX)
		CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);

	// A top level.
	inputs = rq_top_inputs(r->instances, 0, 2, NULL, 0, 0);
	inputs.geometry_count = 1;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_DESC_INVALID_PARAMETERS);
	inputs.geometry_count = 0;
	inputs.instances      = NULL;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_BUFFER);
	inputs.instances = r->scratch;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_BUFFER_USAGE);
	inputs.instances       = r->instances;
	inputs.instance_offset = r->layout.alignment / 2;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	inputs.instance_offset = 0;
	if (r->limits.max_instances < UINT32_MAX) {
		inputs.instance_count = (uint32_t)r->limits.max_instances + 1;
		CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);
	}
	inputs.instance_count = 100;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	inputs.instance_count     = 2;
	inputs.bottom_level_count = 1;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_NULL_DESC);
	crgpu_acceleration_structure none = NULL;
	inputs.bottom_levels                = &none;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	// Nothing was written by a refusal.
	CHECK(sizes.storage_size == 0 && sizes.build_scratch_size == 0);
}

/* The create calls' refusals, in order. */
static void rq_test_create_refusals(rq_run *r) {
	crgpu_acceleration_structure_desc desc = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, 4096, 0, false, NULL);
	crgpu_acceleration_structure structure = NULL;
	CHECK(crgpu_acceleration_structure_create(NULL, &desc, &structure) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_acceleration_structure_create(r->device, NULL, &structure) == CR_RESULT_NULL_DESC);
	desc.base.type = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, &structure) == CR_RESULT_STRUCT_WRONG_TYPE);
	desc.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	desc.level = CRGPU_ACCELERATION_STRUCTURE_LEVEL_NONE;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, &structure) == CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.level             = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	desc.max_bottom_levels = 1;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, &structure) == CR_RESULT_DESC_INVALID_PARAMETERS);
	desc.max_bottom_levels = 0;
	desc.size              = 0;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, &structure) == CR_RESULT_ZERO_SIZE);
	CHECK(structure == NULL);

	// Placed: in a heap of acceleration structures, at the device's
	// alignment, inside the heap.
	crgpu_heap_desc heap_desc;
	memset(&heap_desc, 0, sizeof(heap_desc));
	heap_desc.base.type      = CR_STRUCT_TYPE_GPU_HEAP_DESC;
	heap_desc.memory_kind    = CRGPU_MEMORY_KIND_UPLOAD;
	heap_desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES;
	heap_desc.size           = CRGPU_HEAP_SIZE_GRANULARITY * 4;
	heap_desc.debug_name     = "rq heap";
	crgpu_heap heap        = NULL;
	CHECK(crgpu_heap_create(r->device, &heap_desc, &heap) == CR_RESULT_GPU_INVALID_MEMORY_KIND);
	heap_desc.memory_kind = CRGPU_MEMORY_KIND_DEVICE;
	CHECK(crgpu_heap_create(r->device, &heap_desc, &heap) == CR_SUCCESS);
	heap_desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	crgpu_heap buffers     = NULL;
	CHECK(crgpu_heap_create(r->device, &heap_desc, &buffers) == CR_SUCCESS);
	desc                         = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r->triangle_sizes.storage_size, 0, false, "placed");
	const uint64_t alignment     = r->limits.placement_alignment;
	if (heap && buffers) {
		CHECK(crgpu_acceleration_structure_create_placed(NULL, 0, &desc, &structure) == CR_RESULT_GPU_INVALID_HEAP);
		CHECK(crgpu_acceleration_structure_create_placed(heap, 0, NULL, &structure) == CR_RESULT_NULL_DESC);
		CHECK(crgpu_acceleration_structure_create_placed(heap, 0, &desc, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
		CHECK(crgpu_acceleration_structure_create_placed(buffers, 0, &desc, &structure) == CR_RESULT_GPU_HEAP_MISMATCH);
		CHECK(crgpu_acceleration_structure_create_placed(heap, alignment / 2, &desc, &structure) == CR_RESULT_GPU_PLACEMENT_MISALIGNED);
		CHECK(crgpu_acceleration_structure_create_placed(heap, heap_desc.size, &desc, &structure) == CR_RESULT_GPU_PLACEMENT_OUT_OF_RANGE);
		CHECK(structure == NULL);
		CHECK(crgpu_acceleration_structure_create_placed(heap, alignment, &desc, &structure) == CR_SUCCESS);
		if (structure) {
			// A build into the placed structure, submitted.
			crgpu_acceleration_structure_geometry geometry = rq_triangles(r->vertices);
			crgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE |
			                                                                                   CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION);
			crgpu_acceleration_structure_build build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, structure,
			                                                      r->scratch, r->scratch_bottom[0]);
			rq_begin(r);
			crgpu_buffer_barrier b = rq_buffer_barrier(r->scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
			crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
			rq_barrier(r->cmd, structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
			CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
			rq_submit(r);
			crgpu_acceleration_structure_destroy(structure);
		}
		printf("crgpu_objects_test: ray queries: a bottom level placed at %llu of a heap of acceleration structures, built\n",
		       (unsigned long long)alignment);
	}
	crgpu_heap_destroy(buffers);
	crgpu_heap_destroy(heap);
}

/* The build call's refusals, in order: recorded into an open list, which is
 * then ended unsubmitted (nothing was recorded). */
static void rq_test_build_refusals(rq_run *r, const crgpu_acceleration_structure_inputs *triangle_inputs,
                                   const crgpu_acceleration_structure_inputs *top_inputs) {
	rq_begin(r);
	crgpu_acceleration_structure_build build =
	    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, triangle_inputs, NULL, r->triangle, r->scratch, r->scratch_bottom[0]);
	CHECK(crgpu_command_list_build_acceleration_structures(NULL, &build, 1) == CR_RESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, NULL, 1) == CR_RESULT_NULL_DESC);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 0) == CR_RESULT_ZERO_SIZE);
	crgpu_acceleration_structure_build bad = build;
	bad.base.type                            = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_STRUCT_WRONG_TYPE);
	bad      = build;
	bad.mode = 0;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_DESC_INVALID_PARAMETERS);
	bad        = build;
	bad.inputs = NULL;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_NULL_DESC);
	bad        = build;
	bad.source = r->box;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_DESC_INVALID_PARAMETERS);
	bad      = build;
	bad.mode = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.source = r->top;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.source = r->triangle;
	// Never built yet.
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	bad             = build;
	bad.destination = NULL;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.destination = r->top;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	// A bottom level too small for the triangle: the box's, if it is.
	if (r->box_sizes.storage_size < r->triangle_sizes.storage_size) {
		bad.destination = r->box;
		CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL);
	}
	bad         = build;
	bad.scratch = NULL;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_BUFFER);
	bad.scratch = r->vertices;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_INVALID_BUFFER_USAGE);
	bad.scratch = r->scratch;
	if (r->limits.scratch_offset_alignment > 1) {
		bad.scratch_offset = r->limits.scratch_offset_alignment / 2;
		CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	}
	crgpu_buffer_desc scratch_desc;
	CHECK(crgpu_buffer_get_desc(r->scratch, &scratch_desc) == CR_SUCCESS);
	bad.scratch_offset = rq_align(scratch_desc.size, r->limits.scratch_offset_alignment);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL);
	// A top level naming more bottom levels than its destination holds.
	crgpu_acceleration_structure three[3]       = {r->triangle, r->box, r->triangle};
	crgpu_acceleration_structure_inputs too_many = *top_inputs;
	too_many.bottom_levels                         = three;
	too_many.bottom_level_count                    = 3;
	bad = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &too_many, NULL, r->top, r->scratch, r->scratch_top);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &bad, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);
	// The second of two builds refused: neither is recorded, the first's
	// destination keeps having no build.
	crgpu_acceleration_structure_build two[2] = {build, build};
	two[1].mode                                 = 0;
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, two, 2) == CR_RESULT_DESC_INVALID_PARAMETERS);
	crgpu_command_list_end(r->cmd);
	// Copies and size writes of structures never built.
	rq_begin(r);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, r->box, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
	      CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, r->scratch, 0) ==
	      CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	crgpu_command_list_end(r->cmd);
}

/* A compute pipeline of [module] over a set layout of [binding_count]
 * bindings 0..n-1 of [types], with [push_bytes] of push constants. */
static crgpu_compute_pipeline rq_compute_pipeline(crgpu_device device, crgpu_shader_module module, const CRGPU_DESCRIPTOR_TYPE *types,
                                                    uint32_t binding_count, uint32_t push_bytes, crgpu_descriptor_set_layout *out_layout) {
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < binding_count; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count = binding_count;
	*out_layout               = NULL;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, out_layout) == CR_SUCCESS);
	crgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module                      = module;
	desc.descriptor_set_layouts[0]           = *out_layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_COMPUTE;
	desc.push_constant_ranges[0].size        = push_bytes;
	desc.push_constant_range_count           = 1;
	crgpu_compute_pipeline pipeline        = NULL;
	if (module && *out_layout)
		CHECK(crgpu_compute_pipeline_create(device, &desc, &pipeline) == CR_SUCCESS);
	return pipeline;
}

/* Points [r]'s set at [top] and the hits. */
static void rq_write_set(rq_run *r, crgpu_acceleration_structure top) {
	crgpu_write_descriptor_set writes[2];
	memset(writes, 0, sizeof(writes));
	writes[0].base.type               = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
	writes[0].dst_set                 = r->set;
	writes[0].dst_binding             = 0;
	writes[0].descriptor_count        = 1;
	writes[0].descriptor_type         = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	writes[0].acceleration_structures = &top;
	const crgpu_descriptor_buffer_info hits = {r->hits, 0, sizeof(rq_hit) * RQ_W * RQ_H};
	writes[1].base.type                       = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
	writes[1].dst_set                         = r->set;
	writes[1].dst_binding                     = 1;
	writes[1].descriptor_count                = 1;
	writes[1].descriptor_type                 = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].buffer_info                     = &hits;
	crgpu_device_update_descriptor_sets(r->device, writes, 2);
}

/* The compacted triangle: its size written and read back, a structure of
 * that size made, the copy, and the top level rebuilt over it; the same
 * hits. Returns the compacted structure, which [r]'s top level now names. */
static crgpu_acceleration_structure rq_test_compaction(rq_run *r, crgpu_acceleration_structure_inputs *top_inputs,
                                                         crgpu_acceleration_structure *bottoms, uint64_t box_reference,
                                                         const float triangle_at[3], const float box_at[3]) {
	crgpu_buffer sizes = make_buffer(r->device, CRGPU_MEMORY_KIND_DEVICE,
	                                   CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST | CRGPU_BUFFER_USAGE_TRANSFER_SRC, 64, NULL);
	crgpu_buffer readback = make_buffer(r->device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, 64, NULL);
	if (!sizes || !readback) {
		crgpu_buffer_destroy(sizes);
		crgpu_buffer_destroy(readback);
		return NULL;
	}
	rq_begin(r);
	// Refusals first, in order, then the write.
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(NULL, &r->triangle, 1, sizes, 0) == CR_RESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, NULL, 1, sizes, 0) == CR_RESULT_NULL_DESC);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 0, sizes, 0) == CR_RESULT_ZERO_SIZE);
	crgpu_acceleration_structure none = NULL;
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &none, 1, sizes, 0) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->box, 1, sizes, 0) ==
	      CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, NULL, 0) == CR_RESULT_GPU_INVALID_BUFFER);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, readback, 0) == CR_RESULT_GPU_INVALID_BUFFER_USAGE);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 4) ==
	      CR_RESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 64) ==
	      CR_RESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	rq_barrier(r->cmd, r->triangle, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	crgpu_buffer_barrier b = rq_buffer_barrier(sizes, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 8) == CR_SUCCESS);
	b = rq_buffer_barrier(sizes, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST, CRGPU_RESOURCE_STATE_COPY_SOURCE);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	crgpu_command_list_copy_buffer(r->cmd, sizes, readback, 8, 0, 8);
	rq_submit(r);
	uint64_t compacted = 0;
	void *mapped       = NULL;
	CHECK(crgpu_buffer_map(readback, 0, 8, &mapped) == CR_SUCCESS);
	if (mapped) {
		CHECK(crgpu_buffer_invalidate(readback, 0, 8) == CR_SUCCESS);
		memcpy(&compacted, mapped, 8);
		crgpu_buffer_unmap(readback);
	}
	crgpu_buffer_destroy(readback);
	crgpu_buffer_destroy(sizes);
	CHECK(compacted > 0 && compacted <= r->triangle_sizes.storage_size);
	printf("crgpu_objects_test: ray queries: the triangle's bottom level is %llu bytes, %llu compacted\n",
	       (unsigned long long)r->triangle_sizes.storage_size, (unsigned long long)compacted);
	if (compacted == 0)
		return NULL;

	crgpu_acceleration_structure_desc desc = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, compacted, 0, false, "compacted triangle");
	crgpu_acceleration_structure compact    = NULL;
	CHECK(crgpu_acceleration_structure_create(r->device, &desc, &compact) == CR_SUCCESS);
	if (!compact)
		return NULL;
	uint64_t reference = 0;
	CHECK(crgpu_acceleration_structure_get_reference(compact, &reference) == CR_SUCCESS);
	const crgpu_acceleration_structure_instance instances[2] = {
	    rq_instance(triangle_at[0], triangle_at[1], triangle_at[2], RQ_TRIANGLE_ID, 0x01, reference),
	    rq_instance(box_at[0], box_at[1], box_at[2], RQ_BOX_ID, 0x02, box_reference)};
	rq_write_instances(r, instances, 2, 0);
	bottoms[0] = compact;

	rq_begin(r);
	CHECK(crgpu_command_list_copy_acceleration_structure(NULL, r->triangle, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      CR_RESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, NULL, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, r->triangle, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, r->top, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, compact, 0) == CR_RESULT_DESC_INVALID_PARAMETERS);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->box, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	if (compacted < r->triangle_sizes.storage_size)
		CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
		      CR_RESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL);
	rq_barrier(r->cmd, r->triangle, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, r->triangle, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) == CR_SUCCESS);
	rq_barrier(r->cmd, compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, r->box, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	b = rq_buffer_barrier(r->scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	crgpu_acceleration_structure_build build =
	    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, top_inputs, NULL, r->top, r->scratch, r->scratch_top);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
	rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	rq_record_trace(r, r->cmd, 0xFF);
	rq_submit(r);
	rq_check(r, triangle_at, box_at, 0xFF, "over the compacted triangle");
	return compact;
}

/* The caller's GPU writer for the device's layout kind: the instances
 * written by a compute shader, compared byte for byte with the CPU call's,
 * then a top level built from them and traced. */
static void rq_test_gpu_writer(rq_run *r, const crgpu_acceleration_structure_inputs *top_inputs_cpu, const uint64_t references[2],
                               const float triangle_at[3], const float box_at[3]) {
	const crgpu_test_shader *shader = r->layout.kind == CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72
	                                        ? &crgpu_test_rq_instances_col72
	                                        : &crgpu_test_rq_instances_row64;
	crgpu_shader_module module = make_module(r->device, shader, "rq instance writer");
	const CRGPU_DESCRIPTOR_TYPE types[2]  = {CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	crgpu_descriptor_set_layout layout = NULL;
	crgpu_compute_pipeline pipeline    = rq_compute_pipeline(r->device, module, types, 2, 8, &layout);

	// The writer's input: 20 words an instance.
	uint32_t words[2][20];
	memset(words, 0, sizeof(words));
	const crgpu_acceleration_structure_instance instances[2] = {
	    rq_instance(triangle_at[0], triangle_at[1], triangle_at[2], RQ_TRIANGLE_ID, 0x01, references[0]),
	    rq_instance(box_at[0], box_at[1], box_at[2], RQ_BOX_ID, 0x02, references[1])};
	for (uint32_t i = 0; i < 2; ++i) {
		memcpy(words[i], instances[i].transform, 48);
		words[i][12] = instances[i].user_index;
		words[i][13] = instances[i].mask;
		words[i][14] = instances[i].table_offset;
		words[i][15] = instances[i].flags;
		words[i][16] = (uint32_t)instances[i].bottom_level_reference;
		words[i][17] = (uint32_t)(instances[i].bottom_level_reference >> 32);
	}
	const uint64_t out_bytes = (uint64_t)r->layout.size * 2;
	crgpu_buffer upload    = make_buffer(r->device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, sizeof(words), words);
	crgpu_buffer input     = make_buffer(r->device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST,
	                                       sizeof(words), NULL);
	crgpu_buffer output = make_buffer(r->device, CRGPU_MEMORY_KIND_DEVICE,
	                                    CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | CRGPU_BUFFER_USAGE_TRANSFER_SRC,
	                                    out_bytes, NULL);
	crgpu_buffer readback = make_buffer(r->device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, out_bytes, NULL);
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(r->device, &pool_desc, &pool) == CR_SUCCESS);
	crgpu_descriptor_set set = NULL;
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(r->device, &set_desc, &set) == CR_SUCCESS);
	}
	if (pipeline && set && upload && input && output && readback) {
		const crgpu_descriptor_buffer_info infos[2] = {{input, 0, sizeof(words)}, {output, 0, out_bytes}};
		crgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		for (uint32_t i = 0; i < 2; ++i) {
			writes[i].base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = set;
			writes[i].dst_binding      = i;
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].buffer_info      = &infos[i];
		}
		crgpu_device_update_descriptor_sets(r->device, writes, 2);

		rq_begin(r);
		crgpu_buffer_barrier b[2] = {rq_buffer_barrier(input, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_COPY_DEST),
		                               rq_buffer_barrier(output, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS)};
		crgpu_command_list_pipeline_barrier(r->cmd, b, 2, NULL, 0);
		crgpu_command_list_copy_buffer(r->cmd, upload, input, 0, 0, sizeof(words));
		b[0] = rq_buffer_barrier(input, CRGPU_RESOURCE_STATE_COPY_DEST, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
		crgpu_command_list_pipeline_barrier(r->cmd, b, 1, NULL, 0);
		crgpu_command_list_bind_compute_pipeline(r->cmd, pipeline);
		crgpu_command_list_bind_compute_descriptor_sets(r->cmd, pipeline, 0, &set, 1);
		const uint32_t push[2] = {2, 0};
		crgpu_command_list_push_compute_constants(r->cmd, pipeline, 0, sizeof(push), push);
		crgpu_command_list_dispatch(r->cmd, 1, 1, 1);
		b[0] = rq_buffer_barrier(output, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS, CRGPU_RESOURCE_STATE_COPY_SOURCE);
		crgpu_command_list_pipeline_barrier(r->cmd, b, 1, NULL, 0);
		crgpu_command_list_copy_buffer(r->cmd, output, readback, 0, 0, out_bytes);
		// The top level, built from what the GPU wrote.
		b[0] = rq_buffer_barrier(output, CRGPU_RESOURCE_STATE_COPY_SOURCE, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
		b[1] = rq_buffer_barrier(r->scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		crgpu_command_list_pipeline_barrier(r->cmd, b, 2, NULL, 0);
		crgpu_acceleration_structure_inputs inputs = *top_inputs_cpu;
		inputs.instances                             = output;
		inputs.instance_offset                       = 0;
		for (uint32_t i = 0; i < inputs.bottom_level_count; ++i)
			rq_barrier(r->cmd, inputs.bottom_levels[i], CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		crgpu_acceleration_structure_build build =
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, r->top, r->scratch, r->scratch_top);
		CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
		rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		rq_record_trace(r, r->cmd, 0xFF);
		rq_submit(r);

		uint8_t cpu_bytes[2 * 72];
		CHECK(crgpu_device_write_acceleration_structure_instances(r->device, instances, 2, cpu_bytes) == CR_SUCCESS);
		void *mapped = NULL;
		CHECK(crgpu_buffer_map(readback, 0, out_bytes, &mapped) == CR_SUCCESS);
		if (mapped) {
			CHECK(crgpu_buffer_invalidate(readback, 0, out_bytes) == CR_SUCCESS);
			const bool same = memcmp(mapped, cpu_bytes, (size_t)out_bytes) == 0;
			CHECK(same);
			printf("crgpu_objects_test: ray queries: %s wrote the same %llu bytes as the CPU call: %s\n", shader->entry_point,
			       (unsigned long long)out_bytes, same ? "yes" : "NO");
			crgpu_buffer_unmap(readback);
		}
		rq_check(r, triangle_at, box_at, 0xFF, "a top level of GPU-written instances");
	}
	crgpu_descriptor_pool_destroy(pool);
	crgpu_buffer_destroy(readback);
	crgpu_buffer_destroy(output);
	crgpu_buffer_destroy(input);
	crgpu_buffer_destroy(upload);
	crgpu_compute_pipeline_destroy(pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
}

#define RQ_SHADOW_W 32u
#define RQ_SHADOW_H 16u

/* rq_shadow.slang drawn over a ground plane under the scene, its rays going
 * up: shadowed (0) under the triangle, lit (1) elsewhere. The box's sphere
 * casts none: the shader commits no procedural candidate (its loop is
 * empty, and an AABB is the shader's to decide even when forced opaque). */
static void rq_test_shadow(rq_run *r, const float triangle_at[3]) {
	crgpu_shader_module vert = make_module(r->device, &crgpu_test_rq_shadow_vert, "rq shadow vertex");
	crgpu_shader_module frag = make_module(r->device, &crgpu_test_rq_shadow, "rq shadow fragment");
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 1;
	crgpu_descriptor_set_layout layout    = NULL;
	CHECK(crgpu_descriptor_set_layout_create(r->device, &layout_desc, &layout) == CR_SUCCESS);
	crgpu_graphics_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC;
	desc.debug_name                          = "rq shadow";
	desc.vertex_module                       = vert;
	desc.fragment_module                     = frag;
	desc.primitive_topology                  = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                           = CRGPU_CULL_MODE_NONE;
	desc.color_attachment_formats[0]         = CRGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count              = 1;
	desc.depth_format                        = CRGPU_FORMAT_UNKNOWN;
	desc.descriptor_set_layouts[0]           = layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_VERTEX | CRGPU_SHADER_STAGE_FRAGMENT;
	desc.push_constant_ranges[0].size        = 48;
	desc.push_constant_range_count           = 1;
	crgpu_graphics_pipeline pipeline         = NULL;
	if (vert && frag && layout)
		CHECK(crgpu_graphics_pipeline_create(r->device, &desc, &pipeline) == CR_SUCCESS);
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool            = NULL;
	CHECK(crgpu_descriptor_pool_create(r->device, &pool_desc, &pool) == CR_SUCCESS);
	crgpu_descriptor_set set = NULL;
	if (pool && layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(r->device, &set_desc, &set) == CR_SUCCESS);
	}
	crgpu_image_desc image_desc;
	memset(&image_desc, 0, sizeof(image_desc));
	image_desc.base.type          = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage              = CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type               = CRGPU_IMAGE_TYPE_2D;
	image_desc.format             = CRGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.format = CRGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.color[1] = 0.5f;
	image_desc.clear_value.color[3] = 1.0f;
	image_desc.width              = RQ_SHADOW_W;
	image_desc.height             = RQ_SHADOW_H;
	image_desc.depth              = 1;
	image_desc.array_layers       = 1;
	image_desc.mip_levels         = 1;
	image_desc.debug_name         = "rq shadow target";
	crgpu_image image           = NULL;
	CHECK(crgpu_image_create(r->device, &image_desc, &image) == CR_SUCCESS);
	crgpu_image_view view = make_image_view(image, CRGPU_IMAGE_ASPECT_COLOR);
	uint64_t row_pitch = 0, total = 0;
	if (image)
		crgpu_image_get_buffer_copy_size(image, 0, &row_pitch, &total);
	crgpu_buffer readback = total ? make_buffer(r->device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL) : NULL;

	if (pipeline && set && view && readback) {
		crgpu_write_descriptor_set write;
		memset(&write, 0, sizeof(write));
		write.base.type               = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		write.dst_set                 = set;
		write.descriptor_count        = 1;
		write.descriptor_type         = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		write.acceleration_structures = &r->top;
		crgpu_device_update_descriptor_sets(r->device, &write, 1);

		rq_begin(r);
		rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		crgpu_command_list_image_barrier_view(r->cmd, view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		crgpu_rendering_begin_desc pass;
		memset(&pass, 0, sizeof(pass));
		pass.base.type                           = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
		pass.color_attachments[0].image_view     = view;
		pass.color_attachments[0].load_op        = CRGPU_LOAD_OP_CLEAR;
		pass.color_attachments[0].store_op       = CRGPU_STORE_OP_STORE;
		pass.color_attachments[0].clear_color[1] = 0.5f;
		pass.color_attachments[0].clear_color[3] = 1.0f;
		pass.color_attachment_count              = 1;
		pass.width                               = RQ_SHADOW_W;
		pass.height                              = RQ_SHADOW_H;
		crgpu_command_list_begin_rendering(r->cmd, &pass);
		CRGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)RQ_SHADOW_W, (float)RQ_SHADOW_H, 0.0f, 1.0f};
		crgpu_command_list_set_viewports(r->cmd, 0, 1, &viewport);
		CRGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)RQ_SHADOW_W, (float)RQ_SHADOW_H};
		crgpu_command_list_set_scissor_rects(r->cmd, 0, 1, &scissor);
		crgpu_command_list_bind_graphics_pipeline(r->cmd, pipeline);
		crgpu_command_list_bind_graphics_descriptor_sets(r->cmd, pipeline, 0, &set, 1);
		// rq_shadow's 32 bytes (toward +z, t from 0.001 to 100, every
		// instance), then the plane: 4 x 2 at z = -0.5.
		const float push[12] = {0.0f, 0.0f, 1.0f, 0.001f, 100.0f, 0.0f, 0.0f, 0.0f, 4.0f, 2.0f, -0.5f, 0.0f};
		uint32_t mask_word   = 0xFFu;
		float pushed[12];
		memcpy(pushed, push, sizeof(push));
		memcpy(&pushed[5], &mask_word, 4);
		crgpu_command_list_push_graphics_constants(r->cmd, pipeline, 0, sizeof(pushed), pushed);
		crgpu_command_list_draw(r->cmd, 6, 0);
		// Inside the pass, every build, copy and size write is refused
		// (card S26), before its other arguments are read, and the pass
		// goes on.
		CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, NULL, 0) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, NULL, NULL, 0) ==
		      CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(r->cmd, NULL, 0, NULL, 0) ==
		      CR_RESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		crgpu_command_list_end_rendering(r->cmd);
		crgpu_command_list_image_barrier_view(r->cmd, view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC);
		crgpu_image_buffer_copy_desc copy;
		memset(&copy, 0, sizeof(copy));
		copy.buffer_row_length = (uint32_t)(row_pitch / 4);
		copy.array_layer_count = 1;
		copy.width             = RQ_SHADOW_W;
		copy.height            = RQ_SHADOW_H;
		copy.depth             = 1;
		crgpu_command_list_copy_image_to_buffer(r->cmd, image, readback, &copy);
		rq_submit(r);

		void *mapped = NULL;
		CHECK(crgpu_buffer_map(readback, 0, total, &mapped) == CR_SUCCESS);
		if (mapped) {
			CHECK(crgpu_buffer_invalidate(readback, 0, total) == CR_SUCCESS);
			const uint8_t *pixels = (const uint8_t *)mapped;
			uint32_t wrong = 0, shadowed = 0;
			for (uint32_t y = 0; y < RQ_SHADOW_H; ++y)
				for (uint32_t x = 0; x < RQ_SHADOW_W; ++x) {
					const float wx = ((float)x + 0.5f) / (float)RQ_SHADOW_W * 4.0f;
					const float wy = ((float)y + 0.5f) / (float)RQ_SHADOW_H * 2.0f;
					const float u = wx - triangle_at[0], v = wy - triangle_at[1];
					if (fminf(fminf(fabsf(u), fabsf(v)), fabsf(u + v - 1.0f)) < 0.05f)
						continue;
					const bool under = u > 0.0f && v > 0.0f && u + v < 1.0f;
					shadowed += under;
					if (!pixel_is(pixels, (uint32_t)row_pitch, x, y, under ? 0 : 255, under ? 0 : 255, under ? 0 : 255, 255))
						++wrong;
				}
			CHECK(wrong == 0);
			CHECK(shadowed > 0);
			printf("crgpu_objects_test: ray queries: rq_shadow over %u x %u pixels, %u in the triangle's shadow, %u wrong\n", RQ_SHADOW_W,
			       RQ_SHADOW_H, shadowed, wrong);
			crgpu_buffer_unmap(readback);
		}
	}
	crgpu_buffer_destroy(readback);
	crgpu_image_view_destroy(view);
	crgpu_image_destroy(image);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_graphics_pipeline_destroy(pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(frag);
	crgpu_shader_module_destroy(vert);
}

/* On a device created with CRGPU_FEATURE_RAY_QUERY: a bottom level of one
 * triangle and one of an AABB, a top level of both, rq_trace.slang's grid
 * read back against hits worked out here (t, instance index and id,
 * geometry, primitive, barycentrics, kind) through three cull masks; the
 * triangle's instance moved and the top level refitted, the grid's hits
 * moved; compaction; a clone; the instances written by the CPU call and by
 * a compute shader, the same bytes; rq_shadow.slang; a placed structure;
 * and every refusal of every call on the way. */
/* A list of a copy queue refuses every build, copy and size write (card
 * S26), before its other arguments are read. */
static void rq_test_list_refusals(rq_run *r) {
	crgpu_command_allocator_desc d;
	memset(&d, 0, sizeof(d));
	d.base.type                         = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	d.type                              = CRGPU_COMMAND_LIST_TYPE_COPY;
	crgpu_command_allocator allocator = NULL;
	crgpu_command_list cmd            = NULL;
	CHECK(crgpu_command_allocator_create(r->device, &d, &allocator) == CR_SUCCESS);
	if (allocator)
		CHECK(crgpu_command_list_create(allocator, &cmd) == CR_SUCCESS);
	if (cmd) {
		crgpu_command_list_begin(cmd);
		const crgpu_acceleration_structure_build build =
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, NULL, NULL, r->triangle, r->scratch, 0);
		CHECK(crgpu_command_list_build_acceleration_structures(cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(crgpu_command_list_build_acceleration_structures(cmd, NULL, 0) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(crgpu_command_list_copy_acceleration_structure(cmd, NULL, NULL, 0) ==
		      CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(cmd, NULL, 0, NULL, 0) ==
		      CR_RESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		crgpu_command_list_end(cmd);
	}
	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(allocator);
	printf("crgpu_objects_test: ray queries: a copy list's build, copy and size write refused\n");
}

/* An update is taken only of exactly its source's last layout (fix 6 of the
 * review: the values themselves, compared one by one). A bottom level built
 * with ALLOW_UPDATE is updated in place; updates with another geometry
 * count, another geometry's flags or other build flags are refused; a
 * clone takes its source's layout and is updated in turn; the top level
 * refuses an update of another instance count. */
static void rq_test_layout_updates(rq_run *r, const crgpu_acceleration_structure_inputs *top_inputs) {
	crgpu_acceleration_structure_geometry geometries[2] = {rq_triangles(r->vertices), rq_triangles(r->vertices)};
	crgpu_acceleration_structure_inputs inputs = rq_bottom_inputs(geometries, CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);
	inputs.geometry_count                        = 2;
	crgpu_acceleration_structure_sizes sizes = rq_sizes_out();
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == CR_SUCCESS);
	CHECK(sizes.storage_size > 0);
	const uint64_t scratch_size =
	    sizes.build_scratch_size > sizes.update_scratch_size ? sizes.build_scratch_size : sizes.update_scratch_size;
	crgpu_buffer scratch = make_buffer(r->device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, scratch_size, NULL);
	crgpu_acceleration_structure_desc d = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, sizes.storage_size, 0, false, "updated");
	crgpu_acceleration_structure updated = NULL, clone = NULL;
	CHECK(crgpu_acceleration_structure_create(r->device, &d, &updated) == CR_SUCCESS);
	d.debug_name = "updated clone";
	CHECK(crgpu_acceleration_structure_create(r->device, &d, &clone) == CR_SUCCESS);
	if (!scratch || !updated || !clone) {
		crgpu_acceleration_structure_destroy(clone);
		crgpu_acceleration_structure_destroy(updated);
		crgpu_buffer_destroy(scratch);
		return;
	}

	rq_begin(r);
	crgpu_buffer_barrier b = rq_buffer_barrier(scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_barrier(r->cmd, updated, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	crgpu_acceleration_structure_build build =
	    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, updated, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
	rq_barrier(r->cmd, updated, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	b = rq_buffer_barrier(scratch, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);

	// Refused: one geometry fewer, a geometry's flags, the build's flags.
	crgpu_acceleration_structure_inputs other = inputs;
	other.geometry_count                        = 1;
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	crgpu_acceleration_structure_geometry other_geometries[2] = {geometries[0], geometries[1]};
	other_geometries[1].flags = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES;
	other            = inputs;
	other.geometries = other_geometries;
	build            = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	other = inputs;
	other.flags |= CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	// The same layout, in place.
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &inputs, updated, updated, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);

	// A clone takes the layout, and is updated as its source would be.
	rq_barrier(r->cmd, updated, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, clone, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	CHECK(crgpu_command_list_copy_acceleration_structure(r->cmd, updated, clone, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) == CR_SUCCESS);
	rq_barrier(r->cmd, clone, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	other                = inputs;
	other.geometry_count = 1;
	build                = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, clone, clone, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &inputs, clone, clone, scratch, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);

	// The top level, built over two instances: an update over one refused.
	crgpu_acceleration_structure_inputs top_other = *top_inputs;
	top_other.instance_count                        = 1;
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &top_other, r->top, r->top, r->scratch, r->scratch_update);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	rq_submit(r);
	crgpu_acceleration_structure_destroy(clone);
	crgpu_acceleration_structure_destroy(updated);
	crgpu_buffer_destroy(scratch);
	printf("crgpu_objects_test: ray queries: updates taken of exactly the last layout, by a structure and its clone\n");
}

/* One storage buffer written by a dispatch, then the scratch of three
 * builds in turn at one range, then written by a dispatch again, a barrier
 * between each use (card S25, and every pair of states that are both
 * D3D12's UNORDERED_ACCESS): the dispatch's writes before the first build's
 * (UNORDERED_ACCESS to SCRATCH), each build's before the next's (SCRATCH to
 * SCRATCH), the last build's before the dispatch (SCRATCH to
 * UNORDERED_ACCESS). The top level built there is traced into the same
 * buffer and checked. Vulkan's synchronization validation reports any
 * missing order. */
static void rq_test_shared_scratch(rq_run *r, const crgpu_acceleration_structure_inputs *triangle_inputs,
                                   const crgpu_acceleration_structure_inputs *box_inputs,
                                   const crgpu_acceleration_structure_inputs *top_inputs, const float triangle_at[3],
                                   const float box_at[3]) {
	uint64_t size = sizeof(rq_hit) * RQ_W * RQ_H;
	if (r->triangle_sizes.build_scratch_size > size)
		size = r->triangle_sizes.build_scratch_size;
	if (r->box_sizes.build_scratch_size > size)
		size = r->box_sizes.build_scratch_size;
	if (r->top_sizes.build_scratch_size > size)
		size = r->top_sizes.build_scratch_size;
	crgpu_buffer shared =
	    make_buffer(r->device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_SRC, size, NULL);
	if (!shared)
		return;
	crgpu_buffer hits = r->hits;
	r->hits             = shared;
	rq_write_set(r, r->top);

	rq_begin(r);
	crgpu_acceleration_structure bottoms[2] = {r->triangle, r->box};
	crgpu_acceleration_structure_barrier ab[2];
	memset(ab, 0, sizeof(ab));
	for (int i = 0; i < 2; ++i) {
		ab[i].base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
		ab[i].structure     = bottoms[i];
		ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
		ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	}
	crgpu_command_list_acceleration_structure_barrier(r->cmd, ab, 2);
	rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	crgpu_buffer_barrier b = rq_buffer_barrier(shared, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, r->cmd, 0xFF);

	// The traced structures rebuilt, as they were, each build's scratch at
	// the buffer's start.
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
		ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
	}
	crgpu_command_list_acceleration_structure_barrier(r->cmd, ab, 2);
	rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	b = rq_buffer_barrier(shared, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	crgpu_acceleration_structure_build build =
	    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, triangle_inputs, NULL, r->triangle, shared, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
	b = rq_buffer_barrier(shared, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, box_inputs, NULL, r->box, shared, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
		ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
	}
	crgpu_command_list_acceleration_structure_barrier(r->cmd, ab, 2);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	build = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, top_inputs, NULL, r->top, shared, 0);
	CHECK(crgpu_command_list_build_acceleration_structures(r->cmd, &build, 1) == CR_SUCCESS);
	rq_barrier(r->cmd, r->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	}
	crgpu_command_list_acceleration_structure_barrier(r->cmd, ab, 2);

	// The hits traced into the buffer the builds used, after the last
	// build's writes. Everything the dispatch uses bound again.
	b = rq_buffer_barrier(shared, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, r->cmd, 0xFF);
	b = rq_buffer_barrier(shared, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS, CRGPU_RESOURCE_STATE_COPY_SOURCE);
	crgpu_command_list_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	crgpu_command_list_copy_buffer(r->cmd, shared, r->hits_readback, 0, 0, sizeof(rq_hit) * RQ_W * RQ_H);
	rq_submit(r);
	rq_check(r, triangle_at, box_at, 0xFF, "three builds sharing one scratch range with a storage buffer's dispatches");

	r->hits = hits;
	rq_write_set(r, r->top);
	crgpu_buffer_destroy(shared);
}

static void test_ray_queries(crgpu_adapter adapter, CRGPU_FEATURES supported) {
	if (!(supported & CRGPU_FEATURE_RAY_QUERY)) {
		printf("crgpu_objects_test: ray queries: the adapter has no CRGPU_FEATURE_RAY_QUERY, the device tests skipped (its refusals ran)\n");
		return;
	}
	rq_run r;
	memset(&r, 0, sizeof(r));
	memset(&r.limits, 0, sizeof(r.limits));
	r.limits.base.type = CR_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS;
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	properties.base.next = &r.limits.base;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	CHECK(r.limits.max_geometries > 0 && r.limits.max_instances > 0 && r.limits.max_primitives > 0);
	CHECK(r.limits.scratch_offset_alignment > 0 && (r.limits.scratch_offset_alignment & (r.limits.scratch_offset_alignment - 1)) == 0);
	CHECK(r.limits.placement_alignment > 0 && (r.limits.placement_alignment & (r.limits.placement_alignment - 1)) == 0);
	printf("crgpu_objects_test: ray queries: limits: %llu geometries, %llu instances, %llu primitives, scratch at %u, placed at %u\n",
	       (unsigned long long)r.limits.max_geometries, (unsigned long long)r.limits.max_instances, (unsigned long long)r.limits.max_primitives,
	       r.limits.scratch_offset_alignment, r.limits.placement_alignment);

	crgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = CRGPU_FEATURE_RAY_QUERY;
	desc.direct_queue_count = 1;
	// A copy queue, whose lists every build, copy and size write refuses
	// (rq_test_list_refusals).
	desc.copy_queue_count   = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "crgpu_objects_test ray queries";
	CHECK(crgpu_device_create(adapter, &desc, &r.device) == CR_SUCCESS);
	if (!r.device)
		return;
	CHECK(crgpu_command_queue_get(r.device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &r.queue) == CR_SUCCESS);
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(crgpu_command_allocator_create(r.device, &allocator_desc, &r.allocator) == CR_SUCCESS);
	if (r.allocator)
		CHECK(crgpu_command_list_create(r.allocator, &r.cmd) == CR_SUCCESS);

	const CR_RESULT layout_sr = crgpu_device_get_acceleration_structure_instance_layout(r.device, &r.layout);
	if (!r.cmd) {
		crgpu_command_list_destroy(r.cmd);
		crgpu_command_allocator_destroy(r.allocator);
		crgpu_device_destroy(r.device);
		return;
	}
	CHECK(layout_sr == CR_SUCCESS);
	CHECK(crgpu_device_get_acceleration_structure_instance_layout(NULL, &r.layout) == CR_RESULT_GPU_INVALID_DEVICE);
	CHECK(crgpu_device_get_acceleration_structure_instance_layout(r.device, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
#if CRGPU_COMPILE_METAL_API
	CHECK(r.layout.kind == CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 && r.layout.size == 72);
#else
	CHECK(r.layout.kind == CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 && r.layout.size == 64);
#endif
	rq_test_write_instances(&r);

	// The scene's buffers. The vertices of the triangle (0,0,0) (1,0,0)
	// (0,1,0), and the box [0,1]^3: the sphere rq_trace intersects in it.
	const float vertices[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float box[6]      = {0, 0, 0, 1, 1, 1};
	const CRGPU_BUFFER_USAGE input_usage = CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT;
	r.vertices      = make_buffer(r.device, CRGPU_MEMORY_KIND_UPLOAD, input_usage, sizeof(vertices), vertices);
	r.boxes         = make_buffer(r.device, CRGPU_MEMORY_KIND_UPLOAD, input_usage, sizeof(box), box);
	r.instances     = make_buffer(r.device, CRGPU_MEMORY_KIND_UPLOAD, input_usage, (uint64_t)r.layout.size * 4, NULL);
	r.hits          = make_buffer(r.device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_SRC,
	                              sizeof(rq_hit) * RQ_W * RQ_H, NULL);
	r.hits_readback = make_buffer(r.device, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, sizeof(rq_hit) * RQ_W * RQ_H, NULL);

	crgpu_acceleration_structure_geometry triangle_geometry = rq_triangles(r.vertices);
	crgpu_acceleration_structure_geometry box_geometry      = rq_boxes(r.boxes);
	crgpu_acceleration_structure_inputs triangle_inputs     = rq_bottom_inputs(
	    &triangle_geometry, CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION);
	crgpu_acceleration_structure_inputs box_inputs = rq_bottom_inputs(&box_geometry, CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD);
	crgpu_acceleration_structure bottoms[2]        = {NULL, NULL};
	crgpu_acceleration_structure_inputs top_inputs =
	    rq_top_inputs(r.instances, 0, 2, bottoms, 2, CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);

	r.triangle_sizes = rq_sizes_out();
	r.box_sizes      = rq_sizes_out();
	r.top_sizes      = rq_sizes_out();
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r.device, &triangle_inputs, &r.triangle_sizes) == CR_SUCCESS);
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r.device, &box_inputs, &r.box_sizes) == CR_SUCCESS);
	CHECK(r.triangle_sizes.storage_size > 0 && r.triangle_sizes.build_scratch_size > 0 && r.triangle_sizes.update_scratch_size == 0);
	CHECK(r.box_sizes.storage_size > 0);

	// Every build's scratch at its own offset of one buffer.
	const uint64_t a  = r.limits.scratch_offset_alignment;
	r.scratch_bottom[0] = 0;
	r.scratch_bottom[1] = rq_align(r.triangle_sizes.build_scratch_size, a);
	r.scratch_top       = r.scratch_bottom[1] + rq_align(r.box_sizes.build_scratch_size, a);
	// The top level's sizes need its instance buffer, so the scratch's
	// rest is sized once it exists (below).
	r.scratch = make_buffer(r.device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, r.scratch_top + 1024 * 1024, NULL);
	if (!r.vertices || !r.boxes || !r.instances || !r.hits || !r.hits_readback || !r.scratch)
		goto done;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r.device, &top_inputs, &r.top_sizes) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	rq_test_size_refusals(&r);

	{
		crgpu_acceleration_structure_desc d =
		    rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r.triangle_sizes.storage_size, 0, true, "triangle");
		CHECK(crgpu_acceleration_structure_create(r.device, &d, &r.triangle) == CR_SUCCESS);
		d = rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r.box_sizes.storage_size, 0, false, "box");
		CHECK(crgpu_acceleration_structure_create(r.device, &d, &r.box) == CR_SUCCESS);
	}
	if (!r.triangle || !r.box)
		goto done;
	bottoms[0] = r.triangle;
	bottoms[1] = r.box;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(r.device, &top_inputs, &r.top_sizes) == CR_SUCCESS);
	// An update may need no scratch at all (WARP asks for none).
	CHECK(r.top_sizes.storage_size > 0);
	r.scratch_update = r.scratch_top + rq_align(r.top_sizes.build_scratch_size, a);
	if (r.scratch_update + r.top_sizes.update_scratch_size > r.scratch_top + 1024 * 1024) {
		printf("crgpu_objects_test: ray queries: the top level's scratch is larger than the test's buffer\n");
		CHECK(false);
		goto done;
	}
	{
		crgpu_acceleration_structure_desc d =
		    rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, r.top_sizes.storage_size, 2, false, "top");
		CHECK(crgpu_acceleration_structure_create(r.device, &d, &r.top) == CR_SUCCESS);
	}
	if (!r.top)
		goto done;
	rq_test_create_refusals(&r);

	uint64_t references[2] = {0, 0};
	CHECK(crgpu_acceleration_structure_get_reference(NULL, &references[0]) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_acceleration_structure_get_reference(r.top, &references[0]) == CR_RESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(crgpu_acceleration_structure_get_reference(r.triangle, NULL) == CR_RESULT_NULL_OUTPUT_PARAMETER);
	CHECK(crgpu_acceleration_structure_get_reference(r.triangle, &references[0]) == CR_SUCCESS);
	CHECK(crgpu_acceleration_structure_get_reference(r.box, &references[1]) == CR_SUCCESS);
	CHECK(references[0] != 0 && references[1] != 0 && references[0] != references[1]);

	{
		const float triangle_at[3] = {0.0f, 0.0f, 2.0f};
		const float box_at[3]      = {2.0f, 0.0f, 0.0f};
		const crgpu_acceleration_structure_instance instances[2] = {
		    rq_instance(triangle_at[0], triangle_at[1], triangle_at[2], RQ_TRIANGLE_ID, 0x01, references[0]),
		    rq_instance(box_at[0], box_at[1], box_at[2], RQ_BOX_ID, 0x02, references[1])};
		rq_write_instances(&r, instances, 2, 0);
		rq_test_build_refusals(&r, &triangle_inputs, &top_inputs);

		// The trace pipeline and its set.
		r.trace_module = make_module(r.device, &crgpu_test_rq_trace, "rq_trace");
		const CRGPU_DESCRIPTOR_TYPE types[2] = {CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
		r.trace = rq_compute_pipeline(r.device, r.trace_module, types, 2, sizeof(rq_trace_params), &r.set_layout);
		crgpu_descriptor_pool_desc pool_desc;
		memset(&pool_desc, 0, sizeof(pool_desc));
		pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
		pool_desc.max_sets                      = 1;
		pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		pool_desc.pool_sizes[0].count           = 1;
		pool_desc.pool_sizes[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		pool_desc.pool_sizes[1].count           = 1;
		pool_desc.pool_size_count               = 2;
		CHECK(crgpu_descriptor_pool_create(r.device, &pool_desc, &r.pool) == CR_SUCCESS);
		if (r.pool && r.set_layout) {
			crgpu_descriptor_set_desc set_desc;
			memset(&set_desc, 0, sizeof(set_desc));
			set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
			set_desc.pool           = r.pool;
			set_desc.set_layouts[0] = r.set_layout;
			set_desc.set_count      = 1;
			CHECK(crgpu_descriptor_set_create(r.device, &set_desc, &r.set) == CR_SUCCESS);
		}
		if (!r.trace || !r.set)
			goto done;
		rq_write_set(&r, r.top);

		// The two bottom levels in one call, then the top level over them,
		// then the grid.
		rq_begin(&r);
		crgpu_buffer_barrier b = rq_buffer_barrier(r.scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		crgpu_command_list_pipeline_barrier(r.cmd, &b, 1, NULL, 0);
		crgpu_acceleration_structure_build builds[2] = {
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &triangle_inputs, NULL, r.triangle, r.scratch, r.scratch_bottom[0]),
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &box_inputs, NULL, r.box, r.scratch, r.scratch_bottom[1])};
		CHECK(crgpu_command_list_build_acceleration_structures(r.cmd, builds, 2) == CR_SUCCESS);
		crgpu_acceleration_structure_barrier ab[2];
		memset(ab, 0, sizeof(ab));
		for (int i = 0; i < 2; ++i) {
			ab[i].base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
			ab[i].structure     = bottoms[i];
			ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
			ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		}
		crgpu_command_list_acceleration_structure_barrier(r.cmd, ab, 2);
		rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		crgpu_acceleration_structure_build top_build =
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &top_inputs, NULL, r.top, r.scratch, r.scratch_top);
		CHECK(crgpu_command_list_build_acceleration_structures(r.cmd, &top_build, 1) == CR_SUCCESS);
		rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		rq_record_trace(&r, r.cmd, 0xFF);
		rq_submit(&r);
		rq_check(&r, triangle_at, box_at, 0xFF, "rq_trace's grid, both instances");
		rq_test_list_refusals(&r);
		rq_test_layout_updates(&r, &top_inputs);
		rq_test_shared_scratch(&r, &triangle_inputs, &box_inputs, &top_inputs, triangle_at, box_at);

		// Each instance alone, through the cull mask.
		const uint32_t masks[2] = {0x01, 0x02};
		for (int m = 0; m < 2; ++m) {
			rq_begin(&r);
			rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			rq_record_trace(&r, r.cmd, masks[m]);
			rq_submit(&r);
			rq_check(&r, triangle_at, box_at, masks[m], m == 0 ? "cull mask 0x01, the triangle alone" : "cull mask 0x02, the sphere alone");
		}

		// The triangle moved up and further, and the top level refitted in
		// place: an update from itself.
		const float moved_at[3] = {0.0f, 1.0f, 3.0f};
		const crgpu_acceleration_structure_instance moved[2] = {
		    rq_instance(moved_at[0], moved_at[1], moved_at[2], RQ_TRIANGLE_ID, 0x01, references[0]), instances[1]};
		rq_write_instances(&r, moved, 2, 0);
		rq_begin(&r);
		// Updates that are refused: of a bottom level built without
		// ALLOW_UPDATE, and of the top level with other flags.
		crgpu_acceleration_structure_build refused =
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &box_inputs, r.box, r.box, r.scratch, r.scratch_bottom[1]);
		CHECK(crgpu_command_list_build_acceleration_structures(r.cmd, &refused, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
		crgpu_acceleration_structure_inputs other_flags = top_inputs;
		other_flags.flags |= CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
		refused = rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other_flags, r.top, r.top, r.scratch, r.scratch_update);
		CHECK(crgpu_command_list_build_acceleration_structures(r.cmd, &refused, 1) == CR_RESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
		b = rq_buffer_barrier(r.scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		crgpu_command_list_pipeline_barrier(r.cmd, &b, 1, NULL, 0);
		for (int i = 0; i < 2; ++i) {
			ab[i].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
			ab[i].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		}
		crgpu_command_list_acceleration_structure_barrier(r.cmd, ab, 2);
		// In place: the top level is read and written by its update.
		rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		crgpu_acceleration_structure_build refit =
		    rq_build(CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &top_inputs, r.top, r.top, r.scratch, r.scratch_update);
		CHECK(crgpu_command_list_build_acceleration_structures(r.cmd, &refit, 1) == CR_SUCCESS);
		rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		rq_record_trace(&r, r.cmd, 0xFF);
		rq_submit(&r);
		rq_check(&r, moved_at, box_at, 0xFF, "the triangle moved, the top level refitted");

		// A clone of the top level, traced through.
		crgpu_acceleration_structure_desc clone_desc =
		    rq_desc(CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, r.top_sizes.storage_size, 2, false, "top clone");
		crgpu_acceleration_structure clone = NULL;
		CHECK(crgpu_acceleration_structure_create(r.device, &clone_desc, &clone) == CR_SUCCESS);
		if (clone) {
			rq_begin(&r);
			rq_barrier(r.cmd, r.top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
			rq_barrier(r.cmd, clone, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
			CHECK(crgpu_command_list_copy_acceleration_structure(r.cmd, r.top, clone, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) == CR_SUCCESS);
			rq_barrier(r.cmd, clone, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			crgpu_command_list_end(r.cmd);
			CHECK(crgpu_command_queue_submit_lists(r.queue, &r.cmd, 1) == CR_SUCCESS);
			crgpu_command_queue_wait_idle(r.queue);
			rq_write_set(&r, clone);
			rq_begin(&r);
			rq_barrier(r.cmd, clone, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			rq_record_trace(&r, r.cmd, 0xFF);
			rq_submit(&r);
			rq_check(&r, moved_at, box_at, 0xFF, "a clone of the refitted top level");
			rq_write_set(&r, r.top);
			crgpu_acceleration_structure_destroy(clone);
		}

		// Compaction, the top level rebuilt over the compacted triangle.
		crgpu_acceleration_structure compact = rq_test_compaction(&r, &top_inputs, bottoms, references[1], triangle_at, box_at);
		// The GPU writer, over the compacted triangle when there is one.
		uint64_t gpu_references[2] = {references[0], references[1]};
		if (compact)
			CHECK(crgpu_acceleration_structure_get_reference(compact, &gpu_references[0]) == CR_SUCCESS);
		rq_test_gpu_writer(&r, &top_inputs, gpu_references, triangle_at, box_at);
		rq_test_shadow(&r, triangle_at);
		// The top level names the compacted triangle: it goes first.
		crgpu_acceleration_structure_destroy(r.top);
		r.top = NULL;
		crgpu_acceleration_structure_destroy(compact);
	}

done:
	crgpu_descriptor_pool_destroy(r.pool);
	crgpu_compute_pipeline_destroy(r.trace);
	crgpu_descriptor_set_layout_destroy(r.set_layout);
	crgpu_shader_module_destroy(r.trace_module);
	crgpu_acceleration_structure_destroy(r.top);
	crgpu_acceleration_structure_destroy(r.box);
	crgpu_acceleration_structure_destroy(r.triangle);
	crgpu_buffer_destroy(r.scratch);
	crgpu_buffer_destroy(r.hits_readback);
	crgpu_buffer_destroy(r.hits);
	crgpu_buffer_destroy(r.instances);
	crgpu_buffer_destroy(r.boxes);
	crgpu_buffer_destroy(r.vertices);
	crgpu_command_list_destroy(r.cmd);
	crgpu_command_allocator_destroy(r.allocator);
	crgpu_device_destroy(r.device);
}

#endif // CRGPU_EXT_RAY_QUERY

// ---- A swap chain's frames -----------------------------------------------------

#if defined(_WIN32)
static LRESULT CALLBACK hidden_window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
	return DefWindowProcW(window, message, w, l);
}

// A window that is never shown: a surface needs one, nobody needs to see it.
static HWND hidden_window_create(void) {
	WNDCLASSEXW window_class;
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize        = sizeof(window_class);
	window_class.lpfnWndProc   = hidden_window_proc;
	window_class.hInstance     = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"crgpu_objects_test";
	RegisterClassExW(&window_class);
	RECT rect = {0, 0, 64, 64};
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	return CreateWindowExW(0, L"crgpu_objects_test", L"crgpu_objects_test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
	                       rect.bottom - rect.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void hidden_window_destroy(HWND window) {
	DestroyWindow(window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"crgpu_objects_test", GetModuleHandleW(NULL));
}
#endif

static bool callback_refuses(void *vk_instance, void *user_data, void *out_surface) {
	(void)vk_instance;
	(void)out_surface;
	++*(int *)user_data;
	return false;
}

#if CRGPU_COMPILE_VULKAN_API && defined(_WIN32)
// What the callback needs of Vulkan, declared here so that the test needs no
// Vulkan headers: vkCreateWin32SurfaceKHR, found through the loader.
typedef struct test_win32_surface_info {
	uint32_t type; // VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR
	const void *next;
	uint32_t flags;
	HINSTANCE instance;
	HWND window;
} test_win32_surface_info;
typedef void (*test_vk_void_fn)(void);
typedef test_vk_void_fn (*test_get_instance_proc_fn)(void *instance, const char *name);
typedef int32_t (*test_create_win32_surface_fn)(void *instance, const test_win32_surface_info *info, const void *allocator, uint64_t *surface);

typedef struct callback_window {
	HWND window;
	int calls;
	void *instance_seen;
} callback_window;

static bool callback_makes_win32_surface(void *vk_instance, void *user_data, void *out_surface) {
	callback_window *w = (callback_window *)user_data;
	++w->calls;
	w->instance_seen = vk_instance;
	HMODULE loader   = GetModuleHandleW(L"vulkan-1.dll");
	if (!loader)
		return false;
	test_get_instance_proc_fn get = (test_get_instance_proc_fn)(void *)GetProcAddress(loader, "vkGetInstanceProcAddr");
	if (!get)
		return false;
	test_create_win32_surface_fn create = (test_create_win32_surface_fn)(void *)get(vk_instance, "vkCreateWin32SurfaceKHR");
	if (!create)
		return false;
	test_win32_surface_info info = {1000009000u, NULL, 0, GetModuleHandleW(NULL), w->window};
	return create(vk_instance, &info, NULL, (uint64_t *)out_surface) == 0;
}
#endif

// Three frames into a swap chain on [surface]: acquired, cleared, submitted
// with crgpu_command_queue_submit_lists_synced() and presented.
static void present_frames(crgpu_device device, crgpu_surface surface, const char *what) {
	crgpu_swap_chain_desc desc = swap_chain_desc_for(device, surface, CRGPU_PRESENT_SCALING_ONE_TO_ONE);
#if !CRGPU_COMPILE_METAL_API
	desc.buffer_count = 2;
#endif
	crgpu_swap_chain swap_chain = NULL;
	CHECK(crgpu_swap_chain_create(device, &desc, &swap_chain) == CR_SUCCESS);
	if (!swap_chain)
		return;
	crgpu_swap_chain_desc created;
	memset(&created, 0, sizeof(created));
	created.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	CHECK(crgpu_swap_chain_get_desc(swap_chain, &created) == CR_SUCCESS);

	crgpu_semaphore available = crgpu_swap_chain_get_image_available_semaphore(swap_chain);
	crgpu_semaphore finished  = crgpu_swap_chain_get_render_finished_semaphore(swap_chain);
	CHECK(crgpu_swap_chain_get_image_available_semaphore(NULL) == NULL);
	CHECK(crgpu_swap_chain_get_render_finished_semaphore(NULL) == NULL);
#if CRGPU_COMPILE_D3D12_API
	// Not implemented by D3D12, which synchronizes a present by a fence of
	// its own: NULL, as crgpu.h says.
	CHECK(available == NULL && finished == NULL);
#elif CRGPU_COMPILE_VULKAN_API
	CHECK(available != NULL && finished != NULL && available != finished);
	CHECK(crgpu_swap_chain_get_image_available_semaphore(swap_chain) == available);
	CHECK(crgpu_swap_chain_get_render_finished_semaphore(swap_chain) == finished);
#else
	(void)available;
	(void)finished;
#endif

	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	crgpu_command_allocator allocator = NULL;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &allocator) == CR_SUCCESS);
	crgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(crgpu_command_list_create(allocator, &cmd) == CR_SUCCESS);
	CHECK(crgpu_swap_chain_get_image_view(NULL, 0) == NULL);
	CHECK(crgpu_swap_chain_get_image_view(swap_chain, created.buffer_count) == NULL);
	int presented = 0;
	for (int frame = 0; cmd && frame < 3; ++frame) {
		const uint32_t index = crgpu_swap_chain_acquire_next_image(swap_chain);
		CHECK(index < created.buffer_count);
		crgpu_image_view view = crgpu_swap_chain_get_image_view(swap_chain, index);
		CHECK(view != NULL);
		if (!view)
			break;
		CHECK(crgpu_command_allocator_reset(allocator) == CR_SUCCESS);
		crgpu_command_list_begin(cmd);
		crgpu_command_list_image_barrier_view(cmd, view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		crgpu_rendering_begin_desc pass;
		memset(&pass, 0, sizeof(pass));
		pass.base.type                       = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
		pass.color_attachments[0].image_view = view;
		pass.color_attachments[0].load_op    = CRGPU_LOAD_OP_CLEAR;
		pass.color_attachments[0].store_op   = CRGPU_STORE_OP_STORE;
		pass.color_attachment_count          = 1;
		pass.width                           = created.width;
		pass.height                          = created.height;
		crgpu_command_list_begin_rendering(cmd, &pass);
		crgpu_command_list_end_rendering(cmd);
		crgpu_command_list_image_barrier_view(cmd, view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);
		crgpu_command_list_end(cmd);
		CHECK(crgpu_command_queue_submit_lists_synced(NULL, &cmd, 1, swap_chain) == CR_RESULT_GPU_INVALID_COMMAND_QUEUE);
		CHECK(crgpu_command_queue_submit_lists_synced(desc.queue, &cmd, 1, NULL) == CR_RESULT_GPU_INVALID_SWAP_CHAIN);
		CHECK(crgpu_command_queue_submit_lists_synced(desc.queue, &cmd, 1, swap_chain) == CR_SUCCESS);
		crgpu_swap_chain_present(swap_chain);
		++presented;
	}
	CHECK(presented == 3);
	printf("crgpu_objects_test: %d frames presented to %s\n", presented, what);
	crgpu_command_queue_wait_idle(desc.queue);
	crgpu_command_list_destroy(cmd);
	crgpu_command_allocator_destroy(allocator);
	crgpu_swap_chain_destroy(swap_chain);
}

// A swap chain's frames on a real surface, where this test can make one
// without showing anything; and crgpu_surface_create_from_callback().
static void test_swap_chain_frames(crgpu_instance instance, crgpu_device device) {
	int refusals = 0;
	CHECK(crgpu_surface_create_from_callback(NULL, &refusals, callback_refuses) == NULL);
	CHECK(crgpu_surface_create_from_callback(instance, &refusals, NULL) == NULL);
	CHECK(crgpu_surface_create_from_callback(instance, &refusals, callback_refuses) == NULL);
#if CRGPU_COMPILE_VULKAN_API
	CHECK(refusals == 1); // called once, and its refusal is the call's
#else
	CHECK(refusals == 0); // not implemented: the callback is never called
#endif
#if defined(_WIN32)
	HWND window = hidden_window_create();
	CHECK(window != NULL);
	if (!window)
		return;
	crgpu_surface surface = NULL;
	CHECK(crgpu_surface_create(instance, window, NULL, &surface) == CR_SUCCESS);
	if (surface) {
		present_frames(device, surface, "a hidden window");
		crgpu_surface_destroy(surface);
	}
#if CRGPU_COMPILE_VULKAN_API
	callback_window w             = {window, 0, NULL};
	crgpu_surface from_callback = crgpu_surface_create_from_callback(instance, &w, callback_makes_win32_surface);
	CHECK(from_callback != NULL);
	CHECK(w.calls == 1 && w.instance_seen != NULL);
	if (from_callback) {
		present_frames(device, from_callback, "a surface the callback made");
		crgpu_surface_destroy(from_callback);
	}
#endif
	hidden_window_destroy(window);
#elif CRGPU_COMPILE_METAL_API
	void *layer             = crgpu_test_metal_layer_create(false);
	crgpu_surface surface = NULL;
	CHECK(crgpu_surface_create(instance, layer, NULL, &surface) == CR_SUCCESS);
	if (surface) {
		present_frames(device, surface, "a CAMetalLayer");
		crgpu_surface_destroy(surface);
	}
	crgpu_test_metal_layer_destroy(layer);
#else
	(void)device;
	printf("crgpu_objects_test: no surface without a display here, no swap chain frames\n");
#endif
}

static void test_queries(crgpu_instance instance) {
	CHECK(crgpu_instance_get_native_gpu_api(NULL) == CRGPU_NATIVE_API_NONE);
#if CRGPU_COMPILE_VULKAN_API
	CHECK(crgpu_instance_get_native_gpu_api(instance) == CRGPU_NATIVE_API_VULKAN);
#elif CRGPU_COMPILE_D3D12_API
	CHECK(crgpu_instance_get_native_gpu_api(instance) == CRGPU_NATIVE_API_D3D12);
#elif CRGPU_COMPILE_METAL_API
	CHECK(crgpu_instance_get_native_gpu_api(instance) == CRGPU_NATIVE_API_METAL);
#endif
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_UNKNOWN) == 0);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_R8G8B8A8_UNORM) == 32);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_B8G8R8A8_UNORM) == 32);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_R32G32_FLOAT) == 64);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_R32G32B32A32_FLOAT) == 128);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_R16_UINT) == 16);
	CHECK(crgpu_format_get_bit_count(CRGPU_FORMAT_D32_FLOAT) == 32);
}

int main(int argc, char **argv) {
	uint32_t adapter_index = 0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
			adapter_index = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: crgpu_objects_test [--adapter N]\n");
			return 2;
		}
	}
	test_format_from_string();

	crtest_allocator tracker;
	CR_ALLOCATOR allocator = crtest_allocator_make(&tracker);
	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "crgpu_objects_test";
	instance_desc.engine_name      = "crgpu_objects_test";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS) {
		fprintf(stderr, "crgpu_objects_test: no instance\n");
		return 1;
	}
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	if (adapter_count == 0) {
		printf("crgpu_objects_test: no adapter, skipped\n");
		crgpu_instance_destroy(instance);
		return SKIPPED;
	}
	if (adapter_index >= adapter_count) {
		fprintf(stderr, "crgpu_objects_test: no adapter %u (%u listed)\n", adapter_index, adapter_count);
		crgpu_instance_destroy(instance);
		return 2;
	}
	const crgpu_adapter adapter = adapters[adapter_index];
	test_queries(instance);

	crgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "crgpu_objects_test";
	crgpu_device device          = NULL;
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(adapter, &properties) == CR_SUCCESS);
	printf("crgpu_objects_test: adapter %u: %s\n", adapter_index, properties.description);
	// Exactly one format, the backend's own.
	test_shader_format = crgpu_test_shader_format(properties.shader_formats);
	CHECK(test_shader_format != CRGPU_SHADER_FORMAT_NONE);
	CHECK(properties.shader_formats == (1u << test_shader_format));

	CHECK(crgpu_device_create(adapter, &device_desc, &device) == CR_SUCCESS);
	if (device) {
		test_shader_module_checks(device);
		test_geometry_tessellation_refusals(device, properties.supported_features);
		test_fence_signal(device);
		test_semaphore(device);
		test_buffer_and_view(device);
		test_heap_desc(device);
		test_descriptor_pool_reset(device);
		test_present_scaling_without_present(device);
#if CRGPU_EXT_RAY_QUERY
		test_ray_query_without_feature(device);
#endif
		crgpu_device_destroy(device);
	}

	test_quarantine(adapter, &tracker);
	test_drawing(adapter, properties.supported_features);
#if CRGPU_EXT_RAY_QUERY
	test_ray_queries(adapter, properties.supported_features);
#endif
	if (properties.supported_features & CRGPU_FEATURE_PRESENT) {
		device_desc.required_features = CRGPU_FEATURE_PRESENT;
		device_desc.debug_name        = "crgpu_objects_test present";
		crgpu_device present_device = NULL;
		CHECK(crgpu_device_create(adapter, &device_desc, &present_device) == CR_SUCCESS);
		if (present_device) {
			test_present_scaling_checks(present_device);
#if CRGPU_COMPILE_METAL_API
			test_present_scaling_metal(instance, present_device);
#endif
			test_swap_chain_frames(instance, present_device);
			crgpu_device_destroy(present_device);
		}
	} else {
		printf("crgpu_objects_test: the adapter can't present, present scaling checks and swap chain frames skipped\n");
	}
	crgpu_instance_destroy(instance);
	CHECK(crtest_allocator_clean(&tracker));

	if (failures) {
		fprintf(stderr, "crgpu_objects_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("crgpu_objects_test: all passed\n");
	return 0;
}
