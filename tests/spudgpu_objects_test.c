/*
 * spudgpu_objects_test — SpudGPU calls nothing else in the workspace makes.
 *
 * On a device of the adapter asked for (0 by default), with nothing drawn:
 * a format looked up by name; a fence signalled from the CPU and waited on; a semaphore made
 * and destroyed; a buffer's desc and GPU address read back; a buffer view's
 * desc; a heap's desc; and a descriptor pool reset and allocated from again.
 * Each with its argument checks. Every block of the instance goes through a
 * counting allocator, checked at the end.
 *
 * Then, on a second device of the same adapter made with
 * SPUDGPU_FEATURE_PRESENT where it has it: the present scaling checks of
 * spudgpu_get_surface_present_scalings and spudgpu_create_swap_chain that
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
 * shaders built from tests/shaders/ (tests/spudgpu_test_shaders.h).
 *
 * And, on the device made with SPUDGPU_FEATURE_PRESENT, three frames
 * presented through a swap chain on a real surface that nobody sees: a
 * hidden window on Windows, a CAMetalLayer with no window on Metal; on
 * Vulkan on Windows also a surface made by
 * spudgpu_create_surface_from_callback with a callback that calls
 * vkCreateWin32SurfaceKHR. Linux has no surface without a display here, and
 * skips the frames.
 *
 * And, on every adapter, every ray query call (SPUDGPU_EXT_RAY_QUERY) refused
 * on the first device, made without SPUDGPU_FEATURE_RAY_QUERY; on an adapter
 * with it, a device made with it: bottom levels of a triangle and of an
 * AABB, a top level of both, rq_trace.slang's grid of rays read back against
 * the hits worked out here, a refit, compaction, a clone, instances written
 * by the CPU call and by a compute shader, rq_shadow.slang drawn, a placed
 * structure, and each call's refusals in order (test_ray_queries).
 *
 * Not here, because they can't run on every backend: the storage-buffer and
 * storage-image bindless calls and spudgpu_cmd_bind_bindless_resources_compute
 * (SPUDGPU_EXT_BINDLESS, 0 on Metal).
 *
 *   spudgpu_objects_test [--adapter N]
 *
 * --adapter picks the adapter, by its index in spudgpu_enumerate_adapters();
 * 0 by default. Exits 0 when every check passes, 1 when one fails, 77
 * (skipped) with no adapter.
 */

#include <spudgpu.h>

#include "../src/spudallocator.h"
#include "spudgpu_test_shaders.h"
#include "spudtest_allocator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if SPUDGPU_COMPILE_METAL_API
#include "spudgpu_test_metal_layer.h"
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
	CHECK(spudgpu_format_from_string(NULL) == SPUDGPU_FORMAT_UNKNOWN);
	CHECK(spudgpu_format_from_string("") == SPUDGPU_FORMAT_UNKNOWN);
	CHECK(spudgpu_format_from_string("not a format") == SPUDGPU_FORMAT_UNKNOWN);
	/* A name is the enumerator's without its SPUDGPU_FORMAT_ prefix. */
	CHECK(spudgpu_format_from_string("R8G8B8A8_UNORM") == SPUDGPU_FORMAT_R8G8B8A8_UNORM);
	CHECK(spudgpu_format_from_string("B8G8R8A8_UNORM") == SPUDGPU_FORMAT_B8G8R8A8_UNORM);
	CHECK(spudgpu_format_from_string("D32_FLOAT") == SPUDGPU_FORMAT_D32_FLOAT);
	CHECK(spudgpu_format_from_string("SPUDGPU_FORMAT_R8G8B8A8_UNORM") == SPUDGPU_FORMAT_UNKNOWN);
	CHECK(spudgpu_format_from_string("r8g8b8a8_unorm") == SPUDGPU_FORMAT_UNKNOWN);
}

static void test_fence_signal(spudgpu_device device) {
	spudgpu_fence fence = NULL;
	CHECK(spudgpu_create_fence(device, SPUDGPU_FENCE_FLAG_NONE, 3, &fence) == SPUD_SUCCESS);
	if (!fence)
		return;
	CHECK(spudgpu_get_fence_value(fence) == 3);
	CHECK(spudgpu_signal_fence(NULL, fence, 4) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_signal_fence(device, NULL, 4) == SPUDRESULT_GPU_INVALID_FENCE);
	CHECK(spudgpu_signal_fence(device, fence, 7) == SPUD_SUCCESS);
	CHECK(spudgpu_get_fence_value(fence) == 7);
	/* A wait for the signalled value returns at once; a higher one times out. */
	uint64_t value = 7;
	CHECK(spudgpu_wait_for_fences(device, &fence, &value, 1, true, 0) == SPUD_SUCCESS);
	value = 8;
	CHECK(spudgpu_wait_for_fences(device, &fence, &value, 1, true, 0) == SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT);
	CHECK(spudgpu_signal_fence(device, fence, 8) == SPUD_SUCCESS);
	CHECK(spudgpu_wait_for_fences(device, &fence, &value, 1, true, 1000000000ull) == SPUD_SUCCESS);
	spudgpu_destroy_fence(fence);
}

static void test_semaphore(spudgpu_device device) {
	spudgpu_semaphore semaphore = NULL;
	CHECK(spudgpu_create_semaphore(NULL, &semaphore) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_create_semaphore(device, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_create_semaphore(device, &semaphore) == SPUD_SUCCESS);
	CHECK(semaphore != NULL);
	spudgpu_destroy_semaphore(semaphore);
	spudgpu_destroy_semaphore(NULL);
}

static void test_buffer_and_view(spudgpu_device device) {
	char name[32];
	strcpy(name, "objects buffer");
	spudgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage       = SPUDGPU_BUFFER_USAGE_UNIFORM | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	desc.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	desc.size        = 4096;
	desc.debug_name  = name;
	spudgpu_buffer buffer = NULL;
	CHECK(spudgpu_create_buffer(device, &desc, &buffer) == SPUD_SUCCESS);
	if (!buffer)
		return;
	strcpy(name, "changed");

	spudgpu_buffer_desc read;
	memset(&read, 0xA5, sizeof(read));
	CHECK(spudgpu_get_buffer_desc(NULL, &read) == SPUDRESULT_GPU_INVALID_BUFFER);
	CHECK(spudgpu_get_buffer_desc(buffer, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_get_buffer_desc(buffer, &read) == SPUD_SUCCESS);
	CHECK(read.base.type == SPUD_STRUCT_TYPE_GPU_BUFFER_DESC);
	CHECK(read.usage == desc.usage);
	CHECK(read.memory_kind == desc.memory_kind);
	CHECK(read.size == desc.size);
	/* The buffer's own copy of its name, not the caller's string. */
	CHECK(read.debug_name != NULL && read.debug_name != name && strcmp(read.debug_name, "objects buffer") == 0);

	uint64_t address = 0;
	CHECK(spudgpu_get_buffer_gpu_address(NULL, &address) == SPUDRESULT_GPU_INVALID_BUFFER);
	CHECK(spudgpu_get_buffer_gpu_address(buffer, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_get_buffer_gpu_address(buffer, &address) == SPUD_SUCCESS);
	CHECK(address != 0);
	uint64_t again = 0;
	CHECK(spudgpu_get_buffer_gpu_address(buffer, &again) == SPUD_SUCCESS);
	CHECK(again == address);

	spudgpu_buffer_view_desc view_desc;
	memset(&view_desc, 0, sizeof(view_desc));
	view_desc.base.type                 = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	view_desc.parent_buffer             = buffer;
	view_desc.offset_from_parent_buffer = 256;
	view_desc.stride                    = 16;
	view_desc.size                      = 512;
	spudgpu_buffer_view view            = NULL;
	CHECK(spudgpu_create_buffer_view(buffer, &view_desc, &view) == SPUD_SUCCESS);
	if (view) {
		spudgpu_buffer_view_desc view_read;
		memset(&view_read, 0xA5, sizeof(view_read));
		CHECK(spudgpu_get_buffer_view_desc(NULL, &view_read) == SPUDRESULT_GPU_INVALID_BUFFER_VIEW);
		CHECK(spudgpu_get_buffer_view_desc(view, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_get_buffer_view_desc(view, &view_read) == SPUD_SUCCESS);
		CHECK(view_read.parent_buffer == buffer);
		CHECK(view_read.offset_from_parent_buffer == 256);
		CHECK(view_read.stride == 16);
		CHECK(view_read.size == 512);
		spudgpu_destroy_buffer_view(view);
	}
	spudgpu_destroy_buffer(buffer);
}

static void test_heap_desc(spudgpu_device device) {
	spudgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = SPUD_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = SPUDGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	desc.size           = SPUDGPU_HEAP_SIZE_GRANULARITY * 4;
	desc.debug_name     = "objects heap";
	spudgpu_heap heap   = NULL;
	CHECK(spudgpu_create_heap(device, &desc, &heap) == SPUD_SUCCESS);
	if (!heap)
		return;
	spudgpu_heap_desc read;
	memset(&read, 0xA5, sizeof(read));
	CHECK(spudgpu_get_heap_desc(NULL, &read) == SPUDRESULT_GPU_INVALID_HEAP);
	CHECK(spudgpu_get_heap_desc(heap, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_get_heap_desc(heap, &read) == SPUD_SUCCESS);
	CHECK(read.memory_kind == desc.memory_kind);
	CHECK(read.resource_class == desc.resource_class);
	CHECK(read.size == desc.size);
	CHECK(read.debug_name != NULL && strcmp(read.debug_name, "objects heap") == 0);
	spudgpu_destroy_heap(heap);
}

static void test_descriptor_pool_reset(spudgpu_device device) {
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 1;
	spudgpu_descriptor_set_layout layout    = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &layout) == SPUD_SUCCESS);
	if (!layout)
		return;

	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 2;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUD_SUCCESS);
	if (pool) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_layouts[1] = layout;
		set_desc.set_count      = 2;
		/* The pool's whole capacity, three times over: a reset gives it all
		 * back each time. */
		for (int round = 0; round < 3; ++round) {
			spudgpu_descriptor_set sets[2] = {NULL, NULL};
			CHECK(spudgpu_create_descriptor_sets(device, &set_desc, sets) == SPUD_SUCCESS);
			CHECK(sets[0] != NULL && sets[1] != NULL && sets[0] != sets[1]);
			spudgpu_reset_descriptor_pool(pool);
		}
		spudgpu_reset_descriptor_pool(NULL);
		spudgpu_destroy_descriptor_pool(pool);
	}
	spudgpu_destroy_descriptor_set_layout(layout);
}

// A swap chain desc with every field but the scaling set to what any
// backend takes; the queue is the device's first direct one.
static spudgpu_swap_chain_desc swap_chain_desc_for(spudgpu_device device, spudgpu_surface surface, SPUDGPU_PRESENT_SCALING scaling) {
	spudgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	spudgpu_get_command_queue(device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &desc.queue);
	desc.width           = 64;
	desc.height          = 64;
	desc.buffer_count    = 1;
	desc.format          = SPUDGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode    = SPUDGPU_PRESENT_MODE_FIFO;
	desc.present_scaling = scaling;
	desc.fullscreen_mode = SPUDGPU_FULLSCREEN_MODE_WINDOWED;
	return desc;
}

// A surface SpudGPU never made, for the checks that come before a surface
// is read: a handle's head and nothing else, marked live so that a Debug
// build's check for a destroyed handle (src/spudstale.h) lets it through to
// them.
static spudgpu_surface not_a_surface(void) {
	static spud_object_head head;
	SPUD_BIRTH(&head);
	return (spudgpu_surface)(void *)&head;
}

// A device made without SPUDGPU_FEATURE_PRESENT: both calls refuse it, after
// their argument checks.
static void test_present_scaling_without_present(spudgpu_device device) {
	spudgpu_surface surface = not_a_surface();
	uint32_t scalings       = 0;
	CHECK(spudgpu_get_surface_present_scalings(device, surface, &scalings) == SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED);
	spudgpu_swap_chain_desc desc = swap_chain_desc_for(device, surface, SPUDGPU_PRESENT_SCALING_NONE);
	spudgpu_swap_chain swap_chain = NULL;
	// The feature is checked before the scaling.
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED);
	CHECK(swap_chain == NULL);
}

// [device] has SPUDGPU_FEATURE_PRESENT. Every check here comes before the
// surface is read, so the surface handle is a pointer to nothing SpudGPU
// knows but a live head (not_a_surface).
static void test_present_scaling_checks(spudgpu_device device) {
	spudgpu_surface surface = not_a_surface();
	uint32_t scalings       = 0xdeadu;
	CHECK(spudgpu_get_surface_present_scalings(NULL, surface, &scalings) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_get_surface_present_scalings(device, NULL, &scalings) == SPUDRESULT_GPU_INVALID_SURFACE);
	CHECK(spudgpu_get_surface_present_scalings(device, surface, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_get_surface_present_scalings(device, NULL, NULL) == SPUDRESULT_GPU_INVALID_SURFACE);
	CHECK(scalings == 0xdeadu);

	spudgpu_swap_chain swap_chain = NULL;
	spudgpu_swap_chain_desc desc  = swap_chain_desc_for(device, surface, SPUDGPU_PRESENT_SCALING_NONE);
	CHECK(desc.queue != NULL);
	CHECK(spudgpu_create_swap_chain(NULL, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_DEVICE);
	// A zeroed desc has not chosen.
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_PRESENT_SCALING);
	// Exactly one value, and only a value the enum has.
	desc.present_scaling = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE | SPUDGPU_PRESENT_SCALING_STRETCH;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_PRESENT_SCALING);
	desc.present_scaling = SPUDGPU_PRESENT_SCALING_ASPECT_FIT << 1;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_PRESENT_SCALING);
	desc.present_scaling = 0xffffffffu;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_PRESENT_SCALING);
	// The checks before it come first: the output, the surface, the queue.
	desc.present_scaling = SPUDGPU_PRESENT_SCALING_NONE;
	CHECK(spudgpu_create_swap_chain(device, &desc, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	desc.surface = NULL;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_SURFACE);
	desc.surface = surface;
	desc.queue   = NULL;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUDRESULT_GPU_INVALID_COMMAND_QUEUE);
	CHECK(swap_chain == NULL);

	CHECK(strcmp(spudresult_str(SPUDRESULT_GPU_INVALID_PRESENT_SCALING), "SPUDRESULT_GPU_INVALID_PRESENT_SCALING") == 0);
	CHECK(strcmp(spudresult_str(SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED), "SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED") == 0);
}

#if SPUDGPU_COMPILE_METAL_API
// A swap chain of [scaling] on a layer, which sets the layer's gravity to
// [gravity] and reads its scaling back.
static void check_metal_swap_chain(spudgpu_device device, spudgpu_surface surface, void *layer, SPUDGPU_PRESENT_SCALING scaling, const char *gravity) {
	spudgpu_test_metal_layer_set_gravity_other(layer);
	spudgpu_swap_chain_desc desc  = swap_chain_desc_for(device, surface, scaling);
	spudgpu_swap_chain swap_chain = NULL;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUD_SUCCESS);
	CHECK(swap_chain != NULL);
	CHECK(strcmp(spudgpu_test_metal_layer_gravity(layer), gravity) == 0);
	spudgpu_swap_chain_desc read;
	memset(&read, 0, sizeof(read));
	CHECK(spudgpu_get_swap_chain_desc(swap_chain, &read) == SPUD_SUCCESS);
	CHECK(read.present_scaling == scaling);
	spudgpu_destroy_swap_chain(swap_chain);
}

static void test_present_scaling_metal(spudgpu_instance instance, spudgpu_device device) {
	for (int flipped = 0; flipped < 2; ++flipped) {
		void *layer             = spudgpu_test_metal_layer_create(flipped != 0);
		spudgpu_surface surface = NULL;
		CHECK(spudgpu_create_surface(instance, layer, NULL, &surface) == SPUD_SUCCESS);
		if (surface) {
			uint32_t scalings = 0;
			CHECK(spudgpu_get_surface_present_scalings(device, surface, &scalings) == SPUD_SUCCESS);
			CHECK(scalings == (SPUDGPU_PRESENT_SCALING_ONE_TO_ONE | SPUDGPU_PRESENT_SCALING_STRETCH | SPUDGPU_PRESENT_SCALING_ASPECT_FIT));
			// Top left on screen: a flipped layer's contents put the y axis
			// the other way up.
			check_metal_swap_chain(device, surface, layer, SPUDGPU_PRESENT_SCALING_ONE_TO_ONE, flipped ? "bottomLeft" : "topLeft");
			check_metal_swap_chain(device, surface, layer, SPUDGPU_PRESENT_SCALING_STRETCH, "resize");
			check_metal_swap_chain(device, surface, layer, SPUDGPU_PRESENT_SCALING_ASPECT_FIT, "resizeAspect");
			spudgpu_destroy_surface(surface);
		}
		spudgpu_test_metal_layer_destroy(layer);
	}
}
#endif

// ---- A frame drawn and read back ------------------------------------------
//
// One pass into a 64 x 64 image seen as an 8 x 8 grid of cells, each draw
// call covering cells of its own in a colour of its own, then the image
// copied to a second one by a blit and both read back, so that what each
// call drew is checked by its pixels. The frame takes the SpudGPU calls
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
static SPUDGPU_SHADER_FORMAT test_shader_format = SPUDGPU_SHADER_FORMAT_NONE;

static spudgpu_shader_module make_module(spudgpu_device device, const spudgpu_test_shader *shader, const char *name) {
	const spudgpu_shader_module_desc desc = spudgpu_test_module_desc(shader, test_shader_format, name);
	spudgpu_shader_module module          = NULL;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUD_SUCCESS);
	return module;
}

// Each refusal of spudgpu_create_shader_module() after the base checks, in
// the header's order, and that a refused call leaves out_module alone; then
// the module made right. The same on every backend.
static void test_shader_module_checks(spudgpu_device device) {
	const spudgpu_test_shader *vert = &spudgpu_test_objects_draw_vert;
	const spudgpu_test_shader *comp = &spudgpu_test_objects_double_comp;
	spudgpu_shader_module_desc desc;
	spudgpu_shader_module module = NULL;
	spudgpu_shader_module marker = (spudgpu_shader_module)(uintptr_t)1;

	desc       = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.stage = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_SHADER_STAGE);
	desc        = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.format = SPUDGPU_SHADER_FORMAT_NONE;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED);
	for (SPUDGPU_SHADER_FORMAT other = SPUDGPU_SHADER_FORMAT_SPIRV; other <= SPUDGPU_SHADER_FORMAT_METALLIB; ++other) {
		if (other == test_shader_format)
			continue;
		desc        = spudgpu_test_module_desc(vert, test_shader_format, "checks");
		desc.format = other;
		CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_SHADER_FORMAT_NOT_SUPPORTED);
	}
	desc      = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code = NULL;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_NULL_SHADER_CODE);
	desc           = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code_size = 0;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_NULL_SHADER_CODE);
	desc      = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code = (const uint8_t *)desc.code + 1;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT);
	if (test_shader_format != SPUDGPU_SHADER_FORMAT_METALLIB) {
		desc           = spudgpu_test_module_desc(vert, test_shader_format, "checks");
		desc.code_size = desc.code_size - 2;
		CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_SHADER_CODE_ALIGNMENT);
	}
	desc             = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = NULL;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_NULL_ENTRY_POINT);
	desc.entry_point = "";
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_NULL_ENTRY_POINT);
	desc                   = spudgpu_test_module_desc(comp, test_shader_format, "checks");
	desc.workgroup_size[1] = 0;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE);
	desc                   = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.workgroup_size[0] = 1;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_WORKGROUP_SIZE);

	// Bytes that aren't the format: its first word broken.
	uint32_t broken[64];
	memset(broken, 0, sizeof(broken));
	broken[0]      = 0x12345678u;
	desc           = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.code      = broken;
	desc.code_size = sizeof(broken);
	module         = marker;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_INVALID_SHADER_CODE);
	CHECK(module == marker);

	// An entry point the module lacks, and the right name for another stage.
	desc             = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = "no_such_entry";
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND);
	desc       = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.stage = SPUDGPU_SHADER_STAGE_FRAGMENT;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND);
	CHECK(module == marker);

	// The caller's entry point string is copied: changed after the call, the
	// module still makes a pipeline (test_drawing) and is destroyed cleanly.
	char entry[64];
	snprintf(entry, sizeof(entry), "%s", vert->entry_point);
	desc             = spudgpu_test_module_desc(vert, test_shader_format, "checks");
	desc.entry_point = entry;
	module           = NULL;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUD_SUCCESS);
	memset(entry, 'x', sizeof(entry) - 1);
	CHECK(module != NULL);
	spudgpu_destroy_shader_module(module);
}

// Geometry and tessellation behind their EXTs, on [device], created with
// no feature: a module of either stage and a pipeline naming one are
// refused, the same on every backend. [supported] is the adapter's: D3D12
// has both bits on every adapter, Metal neither (its EXTs are 0); Vulkan
// says what the GPU has.
static void test_geometry_tessellation_refusals(spudgpu_device device, SPUDGPU_FEATURES supported) {
	printf("spudgpu_objects_test: geometry shaders %s, tessellation %s\n",
	    (supported & SPUDGPU_FEATURE_GEOMETRY_SHADER) ? "yes" : "no",
	    (supported & SPUDGPU_FEATURE_TESSELLATION) ? "yes" : "no");
	if (test_shader_format == SPUDGPU_SHADER_FORMAT_DXIL) {
		CHECK((supported & SPUDGPU_FEATURE_GEOMETRY_SHADER) != 0);
		CHECK((supported & SPUDGPU_FEATURE_TESSELLATION) != 0);
	}
	if (test_shader_format == SPUDGPU_SHADER_FORMAT_METALLIB) {
		CHECK((supported & SPUDGPU_FEATURE_GEOMETRY_SHADER) == 0);
		CHECK((supported & SPUDGPU_FEATURE_TESSELLATION) == 0);
	}
	CHECK((spudgpu_get_device_features(device) & (SPUDGPU_FEATURE_GEOMETRY_SHADER | SPUDGPU_FEATURE_TESSELLATION)) == 0);

	// A module of each stage: refused after the checks every module has, so
	// the vertex shader's code stands in.
	spudgpu_shader_module_desc desc = spudgpu_test_module_desc(&spudgpu_test_objects_draw_vert, test_shader_format, "stage ext");
	spudgpu_shader_module module    = NULL;
	desc.stage                      = SPUDGPU_SHADER_STAGE_GEOMETRY;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED);
	desc.stage = SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
	desc.stage = SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION;
	CHECK(spudgpu_create_shader_module(device, &desc, &module) == SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
	CHECK(module == NULL);

	// A pipeline naming one: refused after the mesh check, before anything
	// reads the module, so the vertex module stands in for each stage.
	spudgpu_shader_module vert = make_module(device, &spudgpu_test_objects_draw_vert, "stage ext vertex");
	spudgpu_shader_module frag = make_module(device, &spudgpu_test_objects_draw_frag, "stage ext fragment");
	if (vert && frag) {
		spudgpu_shader_pipeline_desc pd;
		memset(&pd, 0, sizeof(pd));
		pd.base.type                     = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
		pd.vertex_module                 = vert;
		pd.fragment_module               = frag;
		pd.color_attachment_count        = 1;
		pd.color_attachment_formats[0]   = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
		spudgpu_shader_pipeline pipeline = NULL;
		pd.geometry_module               = vert;
		CHECK(spudgpu_create_shader_pipeline(device, &pd, &pipeline) == SPUDRESULT_GPU_EXT_GEOMETRY_SHADER_NOT_SUPPORTED);
		pd.geometry_module     = NULL;
		pd.tess_control_module = vert;
		CHECK(spudgpu_create_shader_pipeline(device, &pd, &pipeline) == SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
		pd.tess_control_module = NULL;
		pd.tess_eval_module    = vert;
		CHECK(spudgpu_create_shader_pipeline(device, &pd, &pipeline) == SPUDRESULT_GPU_EXT_TESSELLATION_NOT_SUPPORTED);
		CHECK(pipeline == NULL);
	}
	spudgpu_destroy_shader_module(frag);
	spudgpu_destroy_shader_module(vert);
}

static spudgpu_buffer make_buffer(spudgpu_device device, SPUDGPU_MEMORY_KIND kind, SPUDGPU_BUFFER_USAGE usage, uint64_t size, const void *contents) {
	spudgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = usage;
	desc.memory_kind      = kind;
	desc.size             = size;
	spudgpu_buffer buffer = NULL;
	CHECK(spudgpu_create_buffer(device, &desc, &buffer) == SPUD_SUCCESS);
	if (buffer && contents) {
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(buffer, 0, size, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			memcpy(mapped, contents, (size_t)size);
			CHECK(spudgpu_flush_buffer(buffer, 0, size) == SPUD_SUCCESS);
			spudgpu_unmap_buffer(buffer);
		}
	}
	return buffer;
}

static spudgpu_buffer_view make_view(spudgpu_buffer buffer, uint64_t offset, uint64_t stride, uint64_t size) {
	spudgpu_buffer_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                 = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	desc.parent_buffer             = buffer;
	desc.offset_from_parent_buffer = offset;
	desc.stride                    = stride;
	desc.size                      = size;
	spudgpu_buffer_view view       = NULL;
	if (buffer)
		CHECK(spudgpu_create_buffer_view(buffer, &desc, &view) == SPUD_SUCCESS);
	return view;
}

static spudgpu_image make_image(spudgpu_device device, spudgpu_heap heap, SPUDGPU_IMAGE_USAGE usage, SPUDGPU_FORMAT format, uint32_t width, uint32_t height,
                                const char *name) {
	spudgpu_image_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_IMAGE_DESC;
	desc.usage              = usage;
	desc.type               = SPUDGPU_IMAGE_TYPE_2D;
	desc.format             = format;
	desc.clear_value.format = format;
	// The values the frame clears the attachments to, which D3D12 clears
	// fastest when told at creation.
	if (format == SPUDGPU_FORMAT_D32_FLOAT) {
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
	spudgpu_image image = NULL;
	if (heap) {
		spudgpu_placement_requirements needs;
		memset(&needs, 0, sizeof(needs));
		needs.base.type = SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS;
		CHECK(spudgpu_get_image_placement_requirements(heap, &desc, &needs) == SPUD_SUCCESS);
		CHECK(needs.size >= (uint64_t)width * height * 4 && needs.alignment > 0);
		CHECK(spudgpu_create_placed_image(heap, 0, &desc, &image) == SPUD_SUCCESS);
	} else {
		CHECK(spudgpu_create_image(device, &desc, &image) == SPUD_SUCCESS);
	}
	return image;
}

static spudgpu_image_view make_image_view(spudgpu_image image, uint64_t aspect) {
	spudgpu_image_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	desc.parent_image                        = image;
	desc.type                                = SPUDGPU_IMAGE_VIEW_TYPE_2D;
	desc.subresource_range.aspect_mask       = aspect;
	desc.subresource_range.mip_level_count   = 1;
	desc.subresource_range.array_layer_count = 1;
	spudgpu_image_view view                  = NULL;
	if (image)
		CHECK(spudgpu_create_image_view(image, &desc, &view) == SPUD_SUCCESS);
	return view;
}

static spudgpu_heap make_heap(spudgpu_device device, SPUDGPU_HEAP_RESOURCE_CLASS resource_class, const char *name) {
	spudgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = SPUD_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = SPUDGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = resource_class;
	desc.size           = SPUDGPU_HEAP_SIZE_GRANULARITY * 16;
	desc.debug_name     = name;
	spudgpu_heap heap   = NULL;
	CHECK(spudgpu_create_heap(device, &desc, &heap) == SPUD_SUCCESS);
	return heap;
}

// A graphics pipeline over the test's shaders: [mesh] in place of the
// vertex shader where given; depth as asked.
static spudgpu_shader_pipeline make_pipeline(spudgpu_device device, spudgpu_shader_module vertex, spudgpu_shader_module mesh, spudgpu_shader_module fragment,
                                             spudgpu_descriptor_set_layout layout, bool depth_test, SPUDGPU_COMPARE_OP compare, bool depth_bounds,
                                             const char *name) {
	spudgpu_shader_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type       = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
	desc.debug_name      = name;
	desc.fragment_module = fragment;
	if (mesh) {
		desc.mesh_module = mesh;
	} else {
		desc.vertex_module                   = vertex;
		desc.vertex_attributes[0].location   = 0;
		desc.vertex_attributes[0].binding    = 0;
		desc.vertex_attributes[0].format     = SPUDGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attributes[1].location   = 1;
		desc.vertex_attributes[1].binding    = 1;
		desc.vertex_attributes[1].format     = SPUDGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attribute_count          = 2;
		desc.vertex_bindings[0].binding      = 0;
		desc.vertex_bindings[0].stride       = sizeof(cell);
		desc.vertex_bindings[1].binding      = 1;
		desc.vertex_bindings[1].stride       = sizeof(cell);
		desc.vertex_bindings[1].per_instance = true;
		desc.vertex_binding_count            = 2;
	}
	desc.primitive_topology          = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                   = SPUDGPU_CULL_MODE_NONE;
	desc.depth_test_enable           = depth_test;
	desc.depth_write_enable          = depth_test && !depth_bounds;
	desc.depth_compare_op            = compare;
	desc.depth_bounds_test_enable    = depth_bounds;
	desc.color_attachment_formats[0] = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count      = 1;
	desc.depth_format                = SPUDGPU_FORMAT_D32_FLOAT;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layout_count = 1;
	desc.push_constant_ranges[0].stage_flags = (mesh ? SPUDGPU_SHADER_STAGE_MESH : SPUDGPU_SHADER_STAGE_VERTEX) | SPUDGPU_SHADER_STAGE_FRAGMENT;
	desc.push_constant_ranges[0].size        = sizeof(draw_push);
	desc.push_constant_range_count           = 1;
	spudgpu_shader_pipeline pipeline         = NULL;
	CHECK(spudgpu_create_shader_pipeline(device, &desc, &pipeline) == SPUD_SUCCESS);
	return pipeline;
}

static void push_draw(spudgpu_command_list cmd, spudgpu_shader_pipeline pipeline, float r, float g, float b, float a, float depth, float texture) {
	draw_push push = {{r, g, b, a}, {depth, texture, 0.0f, 0.0f}};
	spudgpu_cmd_push_constants(cmd, pipeline, 0, sizeof(push), &push);
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

// Records with [record], submits with spudgpu_submit_command_lists() and
// waits for the queue to be idle.
typedef void (*record_fn)(spudgpu_command_list cmd, void *context);
static void run_once(spudgpu_command_queue queue, spudgpu_command_allocator allocator, spudgpu_command_list cmd, record_fn record, void *context) {
	CHECK(spudgpu_reset_command_allocator(allocator) == SPUD_SUCCESS);
	spudgpu_begin_command_list(cmd);
	record(cmd, context);
	spudgpu_end_command_list(cmd);
	CHECK(spudgpu_submit_command_lists(queue, &cmd, 1) == SPUD_SUCCESS);
	spudgpu_queue_wait_idle(queue);
}

typedef struct draw_frame {
	SPUDGPU_FEATURES features;
	spudgpu_shader_pipeline plain, depth_tested, depth_bounded, meshed;
	spudgpu_descriptor_set set;
	spudgpu_image target, copy, depth, texture;
	spudgpu_image_view target_view, depth_view;
	spudgpu_buffer vertex_upload, vertices, texel_upload, indirect, readback_target, readback_copy;
	spudgpu_buffer_view corners, indices, instances[8];
	uint64_t texel_row_pitch, row_pitch;
	spudgpu_timestamp_set timestamps;
} draw_frame;

// The vertices to the device buffer, the texels to the texture: before the
// frame, each copy between its barriers.
static void record_uploads(spudgpu_command_list cmd, void *context) {
	draw_frame *f = (draw_frame *)context;
	spudgpu_cmd_begin_debug_label(cmd, "uploads");
	spudgpu_buffer_barrier to_copy;
	memset(&to_copy, 0, sizeof(to_copy));
	to_copy.base.type    = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	to_copy.buffer       = f->vertices;
	to_copy.state_before = SPUDGPU_RESOURCE_STATE_COMMON;
	to_copy.state_after  = SPUDGPU_RESOURCE_STATE_COPY_DEST;
	spudgpu_cmd_pipeline_barrier(cmd, &to_copy, 1, NULL, 0);
	// In two copies, so that the offsets count: the first six corners, then
	// the four after them.
	spudgpu_cmd_copy_buffer(cmd, f->vertex_upload, f->vertices, 0, 0, 6 * sizeof(cell));
	spudgpu_cmd_copy_buffer(cmd, f->vertex_upload, f->vertices, 6 * sizeof(cell), 6 * sizeof(cell), 4 * sizeof(cell));
	spudgpu_buffer_barrier to_vertices = to_copy;
	to_vertices.state_before           = SPUDGPU_RESOURCE_STATE_COPY_DEST;
	to_vertices.state_after            = SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER;
	spudgpu_cmd_pipeline_barrier(cmd, &to_vertices, 1, NULL, 0);

	spudgpu_cmd_image_barrier(cmd, f->texture, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST);
	spudgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(f->texel_row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = 2;
	copy.height            = 2;
	copy.depth             = 1;
	spudgpu_cmd_copy_buffer_to_image(cmd, f->texel_upload, f->texture, &copy);
	spudgpu_cmd_image_barrier(cmd, f->texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	spudgpu_cmd_end_debug_label(cmd);
}

static void record_frame(spudgpu_command_list cmd, void *context) {
	draw_frame *f = (draw_frame *)context;
	if (f->timestamps)
		spudgpu_cmd_reset_timestamps(cmd, f->timestamps, 0, 2);
	spudgpu_cmd_begin_debug_label(cmd, "frame");
	spudgpu_cmd_image_barrier_view(cmd, f->target_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	spudgpu_cmd_image_barrier_view(cmd, f->depth_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	// Both attachments cleared before the pass, which loads them: what no
	// draw covers is grey only if the colour clear ran, and the depth tests
	// below pass and fail against the depth clear's 0.25.
	spudgpu_cmd_clear_color_attachment(cmd, f->target_view, 0.4f, 0.4f, 0.4f, 1.0f, DRAW_SIZE, DRAW_SIZE);
	spudgpu_cmd_clear_depth_attachment(cmd, f->depth_view, true, false, 0.25f, 0, DRAW_SIZE, DRAW_SIZE);

	spudgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                         = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view   = f->target_view;
	pass.color_attachments[0].load_op      = SPUDGPU_LOAD_OP_LOAD;
	pass.color_attachments[0].store_op     = SPUDGPU_STORE_OP_STORE;
	pass.color_attachment_count            = 1;
	pass.depth_attachment.image_view       = f->depth_view;
	pass.depth_attachment.depth_load_op    = SPUDGPU_LOAD_OP_LOAD;
	pass.depth_attachment.depth_store_op   = SPUDGPU_STORE_OP_STORE;
	pass.depth_attachment.stencil_load_op  = SPUDGPU_LOAD_OP_DONT_CARE;
	pass.depth_attachment.stencil_store_op = SPUDGPU_STORE_OP_DONT_CARE;
	pass.width                             = DRAW_SIZE;
	pass.height                            = DRAW_SIZE;
	if (f->timestamps) {
		pass.timestamp_set         = f->timestamps;
		pass.timestamp_begin_index = 0;
		pass.timestamp_end_index   = 1;
	}
	spudgpu_cmd_begin_rendering(cmd, &pass);

	SPUDGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)DRAW_SIZE, (float)DRAW_SIZE, 0.0f, 1.0f};
	spudgpu_cmd_set_viewports(cmd, 0, 1, &viewport);
	SPUDGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)DRAW_SIZE, (float)DRAW_SIZE};
	spudgpu_cmd_set_scissor_rects(cmd, 0, 1, &scissor);

	spudgpu_cmd_bind_pipeline(cmd, f->plain);
	spudgpu_cmd_bind_descriptor_sets(cmd, f->plain, 0, &f->set, 1);
	spudgpu_cmd_set_index_buffer(cmd, f->indices);
	spudgpu_buffer_view views[2] = {f->corners, f->instances[0]};

	// Row 0, each call in its own colour.
	// (0, 0): a plain draw of six corners.
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw(cmd, 6, 0);
	// (1, 0) and (2, 0): two instances from the second.
	views[1] = f->instances[1];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw_instanced(cmd, 6, 2, 0, 1);
	// (3, 0): indexed, the four corners after the first six.
	views[1] = f->instances[2];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw_indexed(cmd, 6, 0, 6);
	// (4, 0) and (5, 0): indexed and instanced, from the second six indices
	// and the second instance.
	views[1] = f->instances[3];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 1.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw_indexed_instanced(cmd, 6, 2, 6, 6, 1);
	// (6, 0), then (6, 0) and (7, 0): two indirect draws, 32 bytes apart;
	// in one call where the device draws several.
	views[1] = f->instances[4];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f);
	const uint32_t draws = (f->features & SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT) ? 2 : 1;
	spudgpu_cmd_draw_indirect(cmd, f->indirect, 0, draws, 32);
	if (draws == 1)
		spudgpu_cmd_draw_indirect(cmd, f->indirect, 32, 1, 32);
	// Row 1. (0, 1), then (0, 1) and (1, 1): two indexed indirect draws,
	// 20 bytes apart.
	views[1] = f->instances[5];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.6f, 0.2f, 0.0f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw_indexed_indirect(cmd, f->indirect, 64, draws, 20);
	if (draws == 1)
		spudgpu_cmd_draw_indexed_indirect(cmd, f->indirect, 84, 1, 20);
	// (2, 1): the texture alone, one texel a quarter of the cell.
	views[1] = f->instances[6];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->plain, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f);
	spudgpu_cmd_draw(cmd, 6, 0);

	// Depth, less than the cleared 0.25 to pass: (3, 1) at 0.2 passes and
	// writes 0.2, (4, 1) at 0.5 fails, (5, 1) at 0.1 passes.
	spudgpu_cmd_bind_pipeline(cmd, f->depth_tested);
	spudgpu_cmd_bind_descriptor_sets(cmd, f->depth_tested, 0, &f->set, 1);
	views[1] = f->instances[7];
	spudgpu_cmd_set_vertex_buffers(cmd, 0, 2, views);
	push_draw(cmd, f->depth_tested, 0.2f, 0.2f, 0.8f, 1.0f, 0.2f, 0.0f);
	spudgpu_cmd_draw_instanced(cmd, 6, 1, 0, 0);
	push_draw(cmd, f->depth_tested, 0.2f, 0.2f, 0.8f, 1.0f, 0.5f, 0.0f);
	spudgpu_cmd_draw_instanced(cmd, 6, 1, 0, 1);
	push_draw(cmd, f->depth_tested, 0.8f, 0.2f, 0.2f, 1.0f, 0.1f, 0.0f);
	spudgpu_cmd_draw_instanced(cmd, 6, 1, 0, 2);

	// Depth bounds of 0.15 to 0.22 over (3, 1), whose depth is 0.2, and
	// (6, 1), whose depth is 0.25: only the first is drawn.
	if (f->depth_bounded) {
		spudgpu_cmd_bind_pipeline(cmd, f->depth_bounded);
		spudgpu_cmd_bind_descriptor_sets(cmd, f->depth_bounded, 0, &f->set, 1);
		spudgpu_cmd_set_depth_bounds(cmd, 0.15f, 0.22f);
		push_draw(cmd, f->depth_bounded, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f);
		spudgpu_cmd_draw_instanced(cmd, 6, 2, 0, 3);
	}
	// Row 2. (0, 2) and (1, 2): a mesh shader's two workgroups.
	if (f->meshed) {
		spudgpu_cmd_bind_pipeline(cmd, f->meshed);
		spudgpu_cmd_bind_descriptor_sets(cmd, f->meshed, 0, &f->set, 1);
		draw_push push = {{1.0f, 0.6f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 2.0f}};
		spudgpu_cmd_push_constants(cmd, f->meshed, 0, sizeof(push), &push);
		spudgpu_cmd_dispatch_mesh(cmd, 2, 1, 1);
	}
	spudgpu_cmd_end_rendering(cmd);
	if (f->timestamps)
		spudgpu_cmd_resolve_timestamps(cmd, f->timestamps, 0, 2);

	// The target to the copy by a blit, then both to buffers.
	spudgpu_cmd_image_barrier_view(cmd, f->target_view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	spudgpu_image_view_desc_subresource_range whole;
	memset(&whole, 0, sizeof(whole));
	whole.aspect_mask       = SPUDGPU_IMAGE_ASPECT_COLOR;
	whole.mip_level_count   = 1;
	whole.array_layer_count = 1;
	spudgpu_cmd_image_barrier_subresource(cmd, f->copy, &whole, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST);
	spudgpu_image_blit_desc blit;
	memset(&blit, 0, sizeof(blit));
	blit.src_array_layer_count = 1;
	blit.src_x1                = DRAW_SIZE;
	blit.src_y1                = DRAW_SIZE;
	blit.src_z1                = 1;
	blit.dst_array_layer_count = 1;
	blit.dst_x1                = DRAW_SIZE;
	blit.dst_y1                = DRAW_SIZE;
	blit.dst_z1                = 1;
	blit.filter                = SPUDGPU_FILTER_NEAREST;
	spudgpu_cmd_blit_image(cmd, f->target, f->copy, &blit);
	spudgpu_cmd_image_barrier_subresource(cmd, f->copy, &whole, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	spudgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(f->row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = DRAW_SIZE;
	copy.height            = DRAW_SIZE;
	copy.depth             = 1;
	spudgpu_cmd_copy_image_to_buffer(cmd, f->target, f->readback_target, &copy);
	spudgpu_cmd_copy_image_to_buffer(cmd, f->copy, f->readback_copy, &copy);
	spudgpu_cmd_end_debug_label(cmd);
}

static void check_frame(const draw_frame *f, const uint8_t *pixels, const char *which) {
	const uint32_t pitch = (uint32_t)f->row_pitch;
	const int tint       = 51; // the uniform buffer's blue, added to every draw
	printf("spudgpu_objects_test: checking the %s\n", which);
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

static void read_back(const draw_frame *f, spudgpu_buffer buffer, uint64_t size, const char *which) {
	void *mapped = NULL;
	CHECK(spudgpu_map_buffer(buffer, 0, size, &mapped) == SPUD_SUCCESS);
	if (!mapped)
		return;
	CHECK(spudgpu_invalidate_buffer(buffer, 0, size) == SPUD_SUCCESS);
	check_frame(f, (const uint8_t *)mapped, which);
	spudgpu_unmap_buffer(buffer);
}

static void test_draw_frame(spudgpu_device device, spudgpu_command_queue queue, spudgpu_command_allocator allocator, spudgpu_command_list cmd) {
	draw_frame f;
	memset(&f, 0, sizeof(f));
	f.features = spudgpu_get_device_features(device);

	// Shaders, a set of a tint, a texture and a sampler, and the pipelines.
	spudgpu_shader_module vertex =
	    make_module(device, &spudgpu_test_objects_draw_vert, "draw vertex");
	spudgpu_shader_module fragment =
	    make_module(device, &spudgpu_test_objects_draw_frag, "draw fragment");
	spudgpu_shader_module mesh = NULL;
	if (f.features & SPUDGPU_FEATURE_MESH_SHADING)
		mesh = make_module(device, &spudgpu_test_objects_draw_mesh, "draw mesh");
	else
		printf("spudgpu_objects_test: the device has no mesh shading, spudgpu_cmd_dispatch_mesh not run\n");
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.bindings[1].binding         = 1;
	layout_desc.bindings[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	layout_desc.bindings[1].count           = 1;
	layout_desc.bindings[1].stage_flags     = SPUDGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.bindings[2].binding         = 2;
	layout_desc.bindings[2].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
	layout_desc.bindings[2].count           = 1;
	layout_desc.bindings[2].stage_flags     = SPUDGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 3;
	spudgpu_descriptor_set_layout layout    = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &layout) == SPUD_SUCCESS);
	if (vertex && fragment && layout) {
		f.plain        = make_pipeline(device, vertex, NULL, fragment, layout, false, SPUDGPU_COMPARE_OP_ALWAYS, false, "draw plain");
		f.depth_tested = make_pipeline(device, vertex, NULL, fragment, layout, true, SPUDGPU_COMPARE_OP_LESS, false, "draw depth");
		if (f.features & SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST)
			f.depth_bounded = make_pipeline(device, vertex, NULL, fragment, layout, true, SPUDGPU_COMPARE_OP_ALWAYS, true, "draw bounds");
		else
			printf("spudgpu_objects_test: the device has no depth bounds test, spudgpu_cmd_set_depth_bounds not run\n");
		if (mesh)
			f.meshed = make_pipeline(device, NULL, mesh, fragment, layout, false, SPUDGPU_COMPARE_OP_ALWAYS, false, "draw meshed");
	}

	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_sizes[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_desc.pool_sizes[1].count           = 1;
	pool_desc.pool_sizes[2].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
	pool_desc.pool_sizes[2].count           = 1;
	pool_desc.pool_size_count               = 3;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUD_SUCCESS);
	if (pool && layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(device, &set_desc, &f.set) == SPUD_SUCCESS);
	}

	// The corners: a unit quad as six (two triangles), then as four for the
	// index buffer's two triangles; uploaded to a buffer placed in a heap.
	static const cell corners[10] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}, {0, 0}, {1, 0}, {0, 1}, {1, 1}};
	f.vertex_upload               = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, sizeof(corners), corners);
	spudgpu_heap buffer_heap      = make_heap(device, SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS, "draw buffers");
	if (buffer_heap) {
		spudgpu_buffer_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
		desc.usage       = SPUDGPU_BUFFER_USAGE_VERTEX | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
		desc.memory_kind = SPUDGPU_MEMORY_KIND_DEVICE;
		desc.size        = sizeof(corners);
		spudgpu_placement_requirements needs;
		memset(&needs, 0, sizeof(needs));
		needs.base.type = SPUD_STRUCT_TYPE_GPU_PLACEMENT_REQUIREMENTS;
		CHECK(spudgpu_get_buffer_placement_requirements(buffer_heap, &desc, &needs) == SPUD_SUCCESS);
		CHECK(needs.size >= sizeof(corners) && needs.alignment > 0);
		// Past the heap's start, at the alignment asked for.
		CHECK(spudgpu_create_placed_buffer(buffer_heap, needs.alignment, &desc, &f.vertices) == SPUD_SUCCESS);
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
	spudgpu_buffer instance_buffer = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_VERTEX, sizeof(instance_data), instance_data);
	for (int i = 0; i < 8; ++i)
		f.instances[i] = make_view(instance_buffer, (uint64_t)i * 3 * sizeof(cell), sizeof(cell), (i == 7 ? 6 : 3) * sizeof(cell));
	static const uint16_t indices[12] = {0, 1, 2, 2, 1, 3, 0, 1, 2, 2, 1, 3};
	spudgpu_buffer index_buffer       = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_INDEX, sizeof(indices), indices);
	f.indices                         = make_view(index_buffer, 0, sizeof(uint16_t), sizeof(indices));
	// Indirect arguments: two draws 32 bytes apart at 0, two indexed draws
	// 20 bytes apart at 64.
	uint8_t indirect[104];
	memset(indirect, 0, sizeof(indirect));
	const spudgpu_draw_indirect_args draws[2]                 = {{6, 1, 0, 0}, {6, 2, 0, 0}};
	const spudgpu_draw_indexed_indirect_args indexed_draws[2] = {{6, 1, 0, 6, 0}, {6, 2, 6, 6, 0}};
	memcpy(indirect, &draws[0], sizeof(draws[0]));
	memcpy(indirect + 32, &draws[1], sizeof(draws[1]));
	memcpy(indirect + 64, &indexed_draws[0], sizeof(indexed_draws[0]));
	memcpy(indirect + 84, &indexed_draws[1], sizeof(indexed_draws[1]));
	f.indirect = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_INDIRECT, sizeof(indirect), indirect);
	// The tint, written through a mapping of part of the buffer.
	const float tint[4]    = {0.0f, 0.0f, 0.2f, 0.0f};
	spudgpu_buffer uniform = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_UNIFORM, 256, NULL);
	if (uniform) {
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(uniform, 0, 256, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			memcpy(mapped, tint, sizeof(tint));
			CHECK(spudgpu_flush_buffer(uniform, 0, sizeof(tint)) == SPUD_SUCCESS);
			spudgpu_unmap_buffer(uniform);
		}
	}

	// The images: the target placed in a heap, a copy, depth, and a 2 x 2
	// texture with its sampler.
	spudgpu_heap image_heap = make_heap(device, SPUDGPU_HEAP_RESOURCE_CLASS_ATTACHMENT_IMAGES, "draw attachments");
	if (image_heap)
		f.target = make_image(device, image_heap, SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC, SPUDGPU_FORMAT_R8G8B8A8_UNORM, DRAW_SIZE,
		                      DRAW_SIZE, "draw target");
	f.copy = make_image(device, NULL, SPUDGPU_IMAGE_USAGE_TRANSFER_DST | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC, SPUDGPU_FORMAT_R8G8B8A8_UNORM, DRAW_SIZE, DRAW_SIZE,
	                    "draw copy");
	f.depth   = make_image(device, NULL, SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT, SPUDGPU_FORMAT_D32_FLOAT, DRAW_SIZE, DRAW_SIZE, "draw depth");
	f.texture = make_image(device, NULL, SPUDGPU_IMAGE_USAGE_SAMPLED | SPUDGPU_IMAGE_USAGE_TRANSFER_DST, SPUDGPU_FORMAT_R8G8B8A8_UNORM, 2, 2, "draw texture");
	f.target_view                   = make_image_view(f.target, SPUDGPU_IMAGE_ASPECT_COLOR);
	f.depth_view                    = make_image_view(f.depth, SPUDGPU_IMAGE_ASPECT_DEPTH);
	spudgpu_image_view texture_view = make_image_view(f.texture, SPUDGPU_IMAGE_ASPECT_COLOR);
	if (f.target && f.target_view) {
		spudgpu_image_desc read;
		memset(&read, 0xA5, sizeof(read));
		CHECK(spudgpu_get_image_desc(NULL, &read) == SPUDRESULT_GPU_INVALID_IMAGE);
		CHECK(spudgpu_get_image_desc(f.target, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_get_image_desc(f.target, &read) == SPUD_SUCCESS);
		CHECK(read.width == DRAW_SIZE && read.height == DRAW_SIZE && read.format == SPUDGPU_FORMAT_R8G8B8A8_UNORM && read.mip_levels == 1);
		CHECK(read.debug_name != NULL && strcmp(read.debug_name, "draw target") == 0);
		spudgpu_image_view_desc view_read;
		memset(&view_read, 0xA5, sizeof(view_read));
		CHECK(spudgpu_get_image_view_desc(NULL, &view_read) == SPUDRESULT_GPU_INVALID_IMAGE_VIEW);
		CHECK(spudgpu_get_image_view_desc(f.target_view, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_get_image_view_desc(f.target_view, &view_read) == SPUD_SUCCESS);
		CHECK(view_read.parent_image == f.target && view_read.type == SPUDGPU_IMAGE_VIEW_TYPE_2D);
		CHECK(view_read.subresource_range.aspect_mask == SPUDGPU_IMAGE_ASPECT_COLOR && view_read.subresource_range.mip_level_count == 1);
	}
	spudgpu_sampler_desc sampler_desc;
	memset(&sampler_desc, 0, sizeof(sampler_desc));
	sampler_desc.base.type      = SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC;
	sampler_desc.mag_filter     = SPUDGPU_FILTER_NEAREST;
	sampler_desc.min_filter     = SPUDGPU_FILTER_NEAREST;
	sampler_desc.mipmap_filter  = SPUDGPU_FILTER_NEAREST;
	sampler_desc.address_mode_u = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_v = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_w = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.max_lod        = 0.0f;
	sampler_desc.max_anisotropy = 1.0f;
	sampler_desc.debug_name     = "draw sampler";
	spudgpu_sampler sampler     = NULL;
	CHECK(spudgpu_create_sampler(device, &sampler_desc, &sampler) == SPUD_SUCCESS);

	// The texels, each row at the pitch a copy wants.
	uint64_t texel_total = 0;
	spudgpu_get_image_buffer_copy_size(f.texture, 0, &f.texel_row_pitch, &texel_total);
	CHECK(f.texel_row_pitch >= 8 && texel_total >= f.texel_row_pitch + 8);
	uint64_t alignment = 0;
	CHECK(spudgpu_get_image_buffer_copy_offset_alignment(NULL, &alignment) == SPUDRESULT_GPU_INVALID_IMAGE);
	if (f.texture) {
		CHECK(spudgpu_get_image_buffer_copy_offset_alignment(f.texture, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_get_image_buffer_copy_offset_alignment(f.texture, &alignment) == SPUD_SUCCESS);
		CHECK(alignment >= 4 && alignment % 4 == 0);
	}
	uint8_t texels[2048];
	memset(texels, 0, sizeof(texels));
	if (texel_total <= sizeof(texels)) {
		const uint8_t top[8]    = {255, 0, 0, 255, 0, 255, 0, 255};
		const uint8_t bottom[8] = {0, 0, 255, 255, 255, 255, 255, 255};
		memcpy(texels, top, 8);
		memcpy(texels + f.texel_row_pitch, bottom, 8);
		f.texel_upload = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, texel_total, texels);
	}
	uint64_t total = 0;
	spudgpu_get_image_buffer_copy_size(f.target, 0, &f.row_pitch, &total);
	CHECK(f.row_pitch >= DRAW_SIZE * 4 && total >= f.row_pitch * (DRAW_SIZE - 1) + DRAW_SIZE * 4);
	f.readback_target = make_buffer(device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);
	f.readback_copy   = make_buffer(device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);

	if (f.set && uniform && texture_view && sampler) {
		spudgpu_descriptor_buffer_info uniform_info = {uniform, 0, 256};
		spudgpu_descriptor_image_info texture_info  = {texture_view, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY};
		spudgpu_write_descriptor_set writes[3];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 3; ++i) {
			writes[i].base.type        = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = f.set;
			writes[i].dst_binding      = (uint32_t)i;
			writes[i].descriptor_count = 1;
		}
		writes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].buffer_info     = &uniform_info;
		writes[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[1].image_info      = &texture_info;
		writes[2].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
		writes[2].sampler         = sampler;
		spudgpu_update_descriptor_sets(device, writes, 3);
	}

	if (f.features & SPUDGPU_FEATURE_PASS_TIMESTAMPS)
		CHECK(spudgpu_create_timestamp_set(device, queue, 2, &f.timestamps) == SPUD_SUCCESS);
	else
		printf("spudgpu_objects_test: the device has no pass timestamps, they are not run\n");

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
			CHECK(spudgpu_get_timestamps(f.timestamps, 0, 2, ns) == SPUD_SUCCESS);
			CHECK(ns[1] >= ns[0]);
			printf("spudgpu_objects_test: the pass took %llu ns on the GPU\n", (unsigned long long)(ns[1] - ns[0]));
		}
	}

	spudgpu_destroy_timestamp_set(f.timestamps);
	spudgpu_destroy_sampler(sampler);
	spudgpu_destroy_image_view(texture_view);
	spudgpu_destroy_image_view(f.depth_view);
	spudgpu_destroy_image_view(f.target_view);
	spudgpu_destroy_image(f.texture);
	spudgpu_destroy_image(f.depth);
	spudgpu_destroy_image(f.copy);
	spudgpu_destroy_image(f.target);
	spudgpu_destroy_heap(image_heap);
	spudgpu_destroy_buffer(f.readback_copy);
	spudgpu_destroy_buffer(f.readback_target);
	spudgpu_destroy_buffer(f.texel_upload);
	spudgpu_destroy_buffer(uniform);
	spudgpu_destroy_buffer(f.indirect);
	spudgpu_destroy_buffer_view(f.indices);
	spudgpu_destroy_buffer(index_buffer);
	for (int i = 0; i < 8; ++i)
		spudgpu_destroy_buffer_view(f.instances[i]);
	spudgpu_destroy_buffer(instance_buffer);
	spudgpu_destroy_buffer_view(f.corners);
	spudgpu_destroy_buffer(f.vertices);
	spudgpu_destroy_heap(buffer_heap);
	spudgpu_destroy_buffer(f.vertex_upload);
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_shader_pipeline(f.meshed);
	spudgpu_destroy_shader_pipeline(f.depth_bounded);
	spudgpu_destroy_shader_pipeline(f.depth_tested);
	spudgpu_destroy_shader_pipeline(f.plain);
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(mesh);
	spudgpu_destroy_shader_module(fragment);
	spudgpu_destroy_shader_module(vertex);
}

// ---- Compute -------------------------------------------------------------------

typedef struct compute_run {
	spudgpu_compute_pipeline pipeline;
	spudgpu_descriptor_set set;
	spudgpu_buffer upload, source, destination, readback;
} compute_run;

#define COMPUTE_COUNT 100u
#define COMPUTE_ADD 7u

static void record_compute(spudgpu_command_list cmd, void *context) {
	compute_run *c       = (compute_run *)context;
	const uint64_t bytes = COMPUTE_COUNT * sizeof(uint32_t);
	spudgpu_buffer_barrier barriers[2];
	memset(barriers, 0, sizeof(barriers));
	barriers[0].base.type    = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	barriers[0].buffer       = c->source;
	barriers[0].state_before = SPUDGPU_RESOURCE_STATE_COMMON;
	barriers[0].state_after  = SPUDGPU_RESOURCE_STATE_COPY_DEST;
	spudgpu_cmd_pipeline_barrier(cmd, barriers, 1, NULL, 0);
	spudgpu_cmd_copy_buffer(cmd, c->upload, c->source, 0, 0, bytes);
	barriers[0].state_before = SPUDGPU_RESOURCE_STATE_COPY_DEST;
	barriers[0].state_after  = SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	barriers[1]              = barriers[0];
	barriers[1].buffer       = c->destination;
	barriers[1].state_before = SPUDGPU_RESOURCE_STATE_COMMON;
	spudgpu_cmd_pipeline_barrier(cmd, barriers, 2, NULL, 0);
	spudgpu_cmd_bind_compute_pipeline(cmd, c->pipeline);
	spudgpu_cmd_bind_descriptor_sets_compute(cmd, c->pipeline, 0, &c->set, 1);
	const uint32_t push[2] = {COMPUTE_ADD, COMPUTE_COUNT};
	spudgpu_cmd_push_constants_compute(cmd, c->pipeline, 0, sizeof(push), push);
	spudgpu_cmd_dispatch(cmd, (COMPUTE_COUNT + 63) / 64, 1, 1);
	barriers[0].buffer       = c->destination;
	barriers[0].state_before = SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	barriers[0].state_after  = SPUDGPU_RESOURCE_STATE_COPY_SOURCE;
	spudgpu_cmd_pipeline_barrier(cmd, barriers, 1, NULL, 0);
	spudgpu_cmd_copy_buffer(cmd, c->destination, c->readback, 0, 0, bytes);
}

static void test_compute(spudgpu_device device, spudgpu_command_queue queue, spudgpu_command_allocator allocator, spudgpu_command_list cmd) {
	compute_run c;
	memset(&c, 0, sizeof(c));
	spudgpu_shader_module module =
	    make_module(device, &spudgpu_test_objects_double_comp, "double");
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < 2; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = SPUDGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count            = 2;
	spudgpu_descriptor_set_layout layout = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &layout) == SPUD_SUCCESS);

	spudgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module                      = module;
	desc.descriptor_set_layouts[0]           = layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc.push_constant_ranges[0].size        = 2 * sizeof(uint32_t);
	desc.push_constant_range_count           = 1;
	desc.debug_name                          = "double";
	if (module && layout)
		CHECK(spudgpu_create_compute_pipeline(device, &desc, &c.pipeline) == SPUD_SUCCESS);
	if (c.pipeline) {
		spudgpu_compute_pipeline_desc read;
		memset(&read, 0xA5, sizeof(read));
		CHECK(spudgpu_get_compute_pipeline_desc(NULL, &read) == SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE);
		CHECK(spudgpu_get_compute_pipeline_desc(c.pipeline, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_get_compute_pipeline_desc(c.pipeline, &read) == SPUD_SUCCESS);
		CHECK(read.base.type == SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
		CHECK(read.compute_module == module);
		CHECK(read.descriptor_set_layout_count == 1 && read.descriptor_set_layouts[0] == (void *)layout);
		CHECK(read.push_constant_range_count == 1 && read.push_constant_ranges[0].size == 8 &&
		      read.push_constant_ranges[0].stage_flags == SPUDGPU_SHADER_STAGE_COMPUTE);
		CHECK(read.debug_name != NULL && strcmp(read.debug_name, "double") == 0);
	}

	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUD_SUCCESS);
	if (pool && layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(device, &set_desc, &c.set) == SPUD_SUCCESS);
	}
	uint32_t values[COMPUTE_COUNT];
	for (uint32_t i = 0; i < COMPUTE_COUNT; ++i)
		values[i] = i * 3u + 1u;
	const uint64_t bytes = sizeof(values);
	c.upload             = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, bytes, values);
	c.source             = make_buffer(device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST, bytes, NULL);
	c.destination        = make_buffer(device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, bytes, NULL);
	c.readback           = make_buffer(device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, bytes, NULL);
	if (c.pipeline && c.set && c.upload && c.source && c.destination && c.readback) {
		spudgpu_descriptor_buffer_info infos[2] = {{c.source, 0, bytes}, {c.destination, 0, bytes}};
		spudgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 2; ++i) {
			writes[i].base.type        = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = c.set;
			writes[i].dst_binding      = (uint32_t)i;
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].buffer_info      = &infos[i];
		}
		spudgpu_update_descriptor_sets(device, writes, 2);
		// Submitted with spudgpu_queue_submit(), signalling a fence to 5,
		// and waited for through the fence rather than the queue.
		spudgpu_fence fence = NULL;
		CHECK(spudgpu_create_fence(device, SPUDGPU_FENCE_FLAG_NONE, 0, &fence) == SPUD_SUCCESS);
		CHECK(spudgpu_reset_command_allocator(allocator) == SPUD_SUCCESS);
		spudgpu_begin_command_list(cmd);
		record_compute(cmd, &c);
		spudgpu_end_command_list(cmd);
		spudgpu_submit_desc submit;
		memset(&submit, 0, sizeof(submit));
		submit.base.type          = SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC;
		submit.cmd_lists          = &cmd;
		submit.cmd_list_count     = 1;
		submit.signal_fence       = fence;
		submit.signal_fence_value = 5;
		CHECK(spudgpu_queue_submit(NULL, &submit) == SPUDRESULT_GPU_INVALID_COMMAND_QUEUE);
		CHECK(spudgpu_queue_submit(queue, &submit) == SPUD_SUCCESS);
		if (fence) {
			uint64_t value = 5;
			CHECK(spudgpu_wait_for_fences(device, &fence, &value, 1, true, 10000000000ull) == SPUD_SUCCESS);
			CHECK(spudgpu_get_fence_value(fence) == 5);
		}
		spudgpu_destroy_fence(fence);
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(c.readback, 0, bytes, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			CHECK(spudgpu_invalidate_buffer(c.readback, 0, bytes) == SPUD_SUCCESS);
			const uint32_t *out = (const uint32_t *)mapped;
			uint32_t wrong      = 0;
			for (uint32_t i = 0; i < COMPUTE_COUNT; ++i)
				wrong += out[i] != values[i] * 2u + COMPUTE_ADD;
			CHECK(wrong == 0);
			printf("spudgpu_objects_test: the compute shader doubled %u values, %u wrong\n", COMPUTE_COUNT, wrong);
			spudgpu_unmap_buffer(c.readback);
		}
	}
	spudgpu_destroy_buffer(c.readback);
	spudgpu_destroy_buffer(c.destination);
	spudgpu_destroy_buffer(c.source);
	spudgpu_destroy_buffer(c.upload);
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_compute_pipeline(c.pipeline);
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(module);
}

// ---- Every binding of a set (card S27) ----------------------------------------

// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8;
// set 1, for the compute modules, a storage buffer at 0. The whole module
// declares set 0 whole and reads 5 and 8; the others are what Metal
// refuses (spudgpu_shader_module_desc, "Every binding of a set").
#define SET_BINDINGS_SIZE 4u

typedef struct set_bindings_run {
	spudgpu_shader_pipeline pipeline;
	spudgpu_descriptor_set set;
	spudgpu_image texture, target;
	spudgpu_image_view target_view;
	spudgpu_buffer texel_upload, readback;
	uint64_t texel_row_pitch, row_pitch;
} set_bindings_run;

static void record_set_bindings(spudgpu_command_list cmd, void *context) {
	set_bindings_run *r = (set_bindings_run *)context;
	spudgpu_cmd_image_barrier(cmd, r->texture, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST);
	spudgpu_image_buffer_copy_desc copy;
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(r->texel_row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = 1;
	copy.height            = 1;
	copy.depth             = 1;
	spudgpu_cmd_copy_buffer_to_image(cmd, r->texel_upload, r->texture, &copy);
	spudgpu_cmd_image_barrier(cmd, r->texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	spudgpu_cmd_image_barrier_view(cmd, r->target_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	spudgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                       = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = r->target_view;
	pass.color_attachments[0].load_op    = SPUDGPU_LOAD_OP_CLEAR;
	pass.color_attachments[0].store_op   = SPUDGPU_STORE_OP_STORE;
	pass.color_attachment_count          = 1;
	pass.width                           = SET_BINDINGS_SIZE;
	pass.height                          = SET_BINDINGS_SIZE;
	spudgpu_cmd_begin_rendering(cmd, &pass);
	SPUDGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)SET_BINDINGS_SIZE, (float)SET_BINDINGS_SIZE, 0.0f, 1.0f};
	spudgpu_cmd_set_viewports(cmd, 0, 1, &viewport);
	SPUDGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)SET_BINDINGS_SIZE, (float)SET_BINDINGS_SIZE};
	spudgpu_cmd_set_scissor_rects(cmd, 0, 1, &scissor);
	spudgpu_cmd_bind_pipeline(cmd, r->pipeline);
	spudgpu_cmd_bind_descriptor_sets(cmd, r->pipeline, 0, &r->set, 1);
	spudgpu_cmd_draw(cmd, 3, 0);
	spudgpu_cmd_end_rendering(cmd);

	spudgpu_cmd_image_barrier_view(cmd, r->target_view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	memset(&copy, 0, sizeof(copy));
	copy.buffer_row_length = (uint32_t)(r->row_pitch / 4);
	copy.array_layer_count = 1;
	copy.width             = SET_BINDINGS_SIZE;
	copy.height            = SET_BINDINGS_SIZE;
	copy.depth             = 1;
	spudgpu_cmd_copy_image_to_buffer(cmd, r->target, r->readback, &copy);
}

// A graphics pipeline of [fragment] over the vertex module with the set's
// layout, no vertex input and no depth. Returns what the create returned.
static SPUDRESULT set_bindings_pipeline(spudgpu_device device, spudgpu_shader_module vertex, spudgpu_shader_module fragment,
                                        spudgpu_descriptor_set_layout layout, spudgpu_shader_pipeline *out_pipeline) {
	spudgpu_shader_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                   = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
	desc.vertex_module               = vertex;
	desc.fragment_module             = fragment;
	desc.primitive_topology          = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                   = SPUDGPU_CULL_MODE_NONE;
	desc.color_attachment_formats[0] = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count      = 1;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layout_count = 1;
	desc.debug_name                  = "set bindings";
	*out_pipeline                    = NULL;
	return spudgpu_create_shader_pipeline(device, &desc, out_pipeline);
}

// A compute pipeline of [module] with both layouts. Returns what the create
// returned.
static SPUDRESULT set_bindings_compute(spudgpu_device device, spudgpu_shader_module module, spudgpu_descriptor_set_layout layout,
                                       spudgpu_descriptor_set_layout result_layout, spudgpu_compute_pipeline *out_pipeline) {
	spudgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                   = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module              = module;
	desc.descriptor_set_layouts[0]   = layout;
	desc.descriptor_set_layouts[1]   = result_layout;
	desc.descriptor_set_layout_count = 2;
	desc.debug_name                  = "set bindings compute";
	*out_pipeline                    = NULL;
	return spudgpu_create_compute_pipeline(device, &desc, out_pipeline);
}

static void test_set_bindings(spudgpu_device device, spudgpu_command_queue queue, spudgpu_command_allocator allocator, spudgpu_command_list cmd) {
	// Metal sees a module's bindings and refuses one that isn't its set's
	// layout; Vulkan and D3D12 create every pipeline here.
	const bool checked        = test_shader_format == SPUDGPU_SHADER_FORMAT_METALLIB;
	const SPUDRESULT refused  = checked ? SPUDRESULT_GPU_SHADER_LAYOUT_MISMATCH : SPUD_SUCCESS;
	set_bindings_run r;
	memset(&r, 0, sizeof(r));

	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	const uint32_t bindings[3]              = {0, 5, 8};
	const SPUDGPU_DESCRIPTOR_TYPE types[3] = {SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	                                          SPUDGPU_DESCRIPTOR_TYPE_SAMPLER};
	for (uint32_t i = 0; i < 3; ++i) {
		layout_desc.bindings[i].binding         = bindings[i];
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = SPUDGPU_SHADER_STAGE_FRAGMENT | SPUDGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count            = 3;
	spudgpu_descriptor_set_layout layout = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &layout) == SPUD_SUCCESS);
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_COMPUTE;
	layout_desc.binding_count               = 1;
	spudgpu_descriptor_set_layout result_layout = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &result_layout) == SPUD_SUCCESS);

	spudgpu_shader_module vertex = make_module(device, &spudgpu_test_bindings_vert, "bindings vertex");
	const spudgpu_test_shader *fragments[5] = {&spudgpu_test_bindings_whole_frag, &spudgpu_test_bindings_part_frag, &spudgpu_test_bindings_extra_frag,
	                                           &spudgpu_test_bindings_type_frag, &spudgpu_test_bindings_count_frag};
	const spudgpu_test_shader *computes[5]  = {&spudgpu_test_bindings_whole_comp, &spudgpu_test_bindings_none_comp, &spudgpu_test_bindings_part_comp,
                                              &spudgpu_test_bindings_extra_comp, &spudgpu_test_bindings_type_comp};
	spudgpu_shader_module fragment_modules[5] = {NULL, NULL, NULL, NULL, NULL};
	if (vertex && layout && result_layout) {
		for (int i = 0; i < 5; ++i) {
			fragment_modules[i]              = make_module(device, fragments[i], fragments[i]->entry_point);
			spudgpu_shader_pipeline pipeline = NULL;
			if (fragment_modules[i]) {
				const SPUDRESULT sr = set_bindings_pipeline(device, vertex, fragment_modules[i], layout, &pipeline);
				// The whole set made; part of it, a binding past it, one of
				// another type and one of another count refused on Metal,
				// leaving no pipeline.
				CHECK(sr == (i == 0 ? SPUD_SUCCESS : refused));
				CHECK((sr == SPUD_SUCCESS) == (pipeline != NULL));
				if (i == 0)
					r.pipeline = pipeline;
				else
					spudgpu_destroy_shader_pipeline(pipeline);
			}
		}
		for (int i = 0; i < 5; ++i) {
			spudgpu_shader_module module      = make_module(device, computes[i], computes[i]->entry_point);
			spudgpu_compute_pipeline pipeline = NULL;
			if (module) {
				const SPUDRESULT sr = set_bindings_compute(device, module, layout, result_layout, &pipeline);
				// The whole set, and none of set 0, made; the rest refused
				// on Metal.
				CHECK(sr == (i < 2 ? SPUD_SUCCESS : refused));
				CHECK((sr == SPUD_SUCCESS) == (pipeline != NULL));
			}
			spudgpu_destroy_compute_pipeline(pipeline);
			spudgpu_destroy_shader_module(module);
		}
	}

	// The whole module's draw reads the image's texel, not another member
	// of the argument buffer (the case that read wrong on Metal before).
	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets  = 1;
	for (uint32_t i = 0; i < 3; ++i) {
		pool_desc.pool_sizes[i].descriptor_type = types[i];
		pool_desc.pool_sizes[i].count           = 1;
	}
	pool_desc.pool_size_count    = 3;
	spudgpu_descriptor_pool pool = NULL;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUD_SUCCESS);
	if (pool && layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(device, &set_desc, &r.set) == SPUD_SUCCESS);
	}
	r.texture = make_image(device, NULL, SPUDGPU_IMAGE_USAGE_SAMPLED | SPUDGPU_IMAGE_USAGE_TRANSFER_DST, SPUDGPU_FORMAT_R8G8B8A8_UNORM, 1, 1,
	                       "set bindings texture");
	r.target  = make_image(device, NULL, SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC, SPUDGPU_FORMAT_R8G8B8A8_UNORM,
	                       SET_BINDINGS_SIZE, SET_BINDINGS_SIZE, "set bindings target");
	spudgpu_image_view texture_view = make_image_view(r.texture, SPUDGPU_IMAGE_ASPECT_COLOR);
	r.target_view                   = make_image_view(r.target, SPUDGPU_IMAGE_ASPECT_COLOR);
	// The uniform buffer the module doesn't read, filled with what reading
	// it in the image's place would show.
	const float unread[4]  = {1.0f, 0.0f, 1.0f, 1.0f};
	spudgpu_buffer uniform = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_UNIFORM, 256, NULL);
	if (uniform) {
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(uniform, 0, 256, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			memcpy(mapped, unread, sizeof(unread));
			CHECK(spudgpu_flush_buffer(uniform, 0, sizeof(unread)) == SPUD_SUCCESS);
			spudgpu_unmap_buffer(uniform);
		}
	}
	spudgpu_sampler_desc sampler_desc;
	memset(&sampler_desc, 0, sizeof(sampler_desc));
	sampler_desc.base.type      = SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC;
	sampler_desc.mag_filter     = SPUDGPU_FILTER_NEAREST;
	sampler_desc.min_filter     = SPUDGPU_FILTER_NEAREST;
	sampler_desc.mipmap_filter  = SPUDGPU_FILTER_NEAREST;
	sampler_desc.address_mode_u = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_v = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.address_mode_w = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_desc.max_anisotropy = 1.0f;
	sampler_desc.debug_name     = "set bindings sampler";
	spudgpu_sampler sampler     = NULL;
	CHECK(spudgpu_create_sampler(device, &sampler_desc, &sampler) == SPUD_SUCCESS);
	uint64_t texel_total = 0;
	spudgpu_get_image_buffer_copy_size(r.texture, 0, &r.texel_row_pitch, &texel_total);
	uint8_t texels[512];
	memset(texels, 0, sizeof(texels));
	const uint8_t texel[4] = {64, 128, 192, 255};
	memcpy(texels, texel, sizeof(texel));
	if (texel_total >= sizeof(texel) && texel_total <= sizeof(texels))
		r.texel_upload = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, texel_total, texels);
	uint64_t total = 0;
	spudgpu_get_image_buffer_copy_size(r.target, 0, &r.row_pitch, &total);
	r.readback = make_buffer(device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL);

	if (r.set && uniform && texture_view && sampler) {
		spudgpu_descriptor_buffer_info uniform_info = {uniform, 0, 256};
		spudgpu_descriptor_image_info texture_info  = {texture_view, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY};
		spudgpu_write_descriptor_set writes[3];
		memset(writes, 0, sizeof(writes));
		for (int i = 0; i < 3; ++i) {
			writes[i].base.type        = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = r.set;
			writes[i].dst_binding      = bindings[i];
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = types[i];
		}
		writes[0].buffer_info = &uniform_info;
		writes[1].image_info  = &texture_info;
		writes[2].sampler     = sampler;
		spudgpu_update_descriptor_sets(device, writes, 3);
	}
	const bool ready = r.pipeline && r.set && r.texel_upload && r.target_view && r.readback;
	CHECK(ready);
	if (ready) {
		run_once(queue, allocator, cmd, record_set_bindings, &r);
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(r.readback, 0, total, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			CHECK(spudgpu_invalidate_buffer(r.readback, 0, total) == SPUD_SUCCESS);
			const uint8_t *pixels = (const uint8_t *)mapped;
			CHECK(pixel_is(pixels, (uint32_t)r.row_pitch, 0, 0, 64, 128, 192, 255));
			CHECK(pixel_is(pixels, (uint32_t)r.row_pitch, SET_BINDINGS_SIZE - 1, SET_BINDINGS_SIZE - 1, 64, 128, 192, 255));
			printf("spudgpu_objects_test: the set's padded module read its image's texel\n");
			spudgpu_unmap_buffer(r.readback);
		}
	}

	spudgpu_destroy_buffer(r.readback);
	spudgpu_destroy_buffer(r.texel_upload);
	spudgpu_destroy_sampler(sampler);
	spudgpu_destroy_buffer(uniform);
	spudgpu_destroy_image_view(r.target_view);
	spudgpu_destroy_image_view(texture_view);
	spudgpu_destroy_image(r.target);
	spudgpu_destroy_image(r.texture);
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_shader_pipeline(r.pipeline);
	for (int i = 0; i < 5; ++i)
		spudgpu_destroy_shader_module(fragment_modules[i]);
	spudgpu_destroy_shader_module(vertex);
	spudgpu_destroy_descriptor_set_layout(result_layout);
	spudgpu_destroy_descriptor_set_layout(layout);
}

// A device with the features the frame can use that [adapter] has, and the
// frame and the compute shader on it.
static void test_drawing(spudgpu_adapter adapter, SPUDGPU_FEATURES supported) {
	spudgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = supported & (SPUDGPU_FEATURE_PASS_TIMESTAMPS | SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST | SPUDGPU_FEATURE_MESH_SHADING |
                                          SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT);
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "spudgpu_objects_test drawing";
	spudgpu_device device   = NULL;
	CHECK(spudgpu_create_device(adapter, &desc, &device) == SPUD_SUCCESS);
	if (!device)
		return;
	CHECK(spudgpu_get_device_features(device) == desc.required_features);
	spudgpu_command_queue queue = NULL;
	CHECK(spudgpu_get_command_queue(device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue) == SPUD_SUCCESS);
	spudgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	spudgpu_command_allocator allocator = NULL;
	CHECK(spudgpu_create_command_allocator(device, &allocator_desc, &allocator) == SPUD_SUCCESS);
	spudgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(spudgpu_create_command_list(allocator, &cmd) == SPUD_SUCCESS);
	if (queue && cmd) {
		test_draw_frame(device, queue, allocator, cmd);
		test_compute(device, queue, allocator, cmd);
		test_set_bindings(device, queue, allocator, cmd);
	}
	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
	spudgpu_destroy_device(device);
}

// ---- Ray queries (SPUDGPU_EXT_RAY_QUERY) --------------------------------------

#if SPUDGPU_EXT_RAY_QUERY


static spudgpu_acceleration_structure_geometry rq_triangles(spudgpu_buffer vertices) {
	spudgpu_acceleration_structure_geometry g;
	memset(&g, 0, sizeof(g));
	g.kind        = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	g.flags       = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
	g.data        = vertices;
	g.data_stride = 12;
	g.data_count  = 3;
	return g;
}

static spudgpu_acceleration_structure_geometry rq_boxes(spudgpu_buffer boxes) {
	spudgpu_acceleration_structure_geometry g;
	memset(&g, 0, sizeof(g));
	g.kind        = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_AABBS;
	g.data        = boxes;
	g.data_stride = 24;
	g.data_count  = 1;
	return g;
}

static spudgpu_acceleration_structure_inputs rq_bottom_inputs(const spudgpu_acceleration_structure_geometry *geometry,
                                                              SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	spudgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type      = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level          = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs.flags          = flags;
	inputs.geometries     = geometry;
	inputs.geometry_count = 1;
	return inputs;
}

static spudgpu_acceleration_structure_inputs rq_top_inputs(spudgpu_buffer instances, uint64_t offset, uint32_t count,
                                                           const spudgpu_acceleration_structure *bottoms, uint32_t bottom_count,
                                                           SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
	spudgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type          = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level              = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	inputs.flags              = flags;
	inputs.instances          = instances;
	inputs.instance_offset    = offset;
	inputs.instance_count     = count;
	inputs.bottom_levels      = bottoms;
	inputs.bottom_level_count = bottom_count;
	return inputs;
}

static spudgpu_acceleration_structure_desc rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL level, uint64_t size, uint32_t max_bottom_levels,
                                                   bool allow_compaction, const char *name) {
	spudgpu_acceleration_structure_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type         = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	desc.level             = level;
	desc.size              = size;
	desc.max_bottom_levels = max_bottom_levels;
	desc.allow_compaction  = allow_compaction;
	desc.debug_name        = name;
	return desc;
}

static spudgpu_acceleration_structure_sizes rq_sizes_out(void) {
	spudgpu_acceleration_structure_sizes sizes;
	memset(&sizes, 0, sizeof(sizes));
	sizes.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	return sizes;
}

static spudgpu_acceleration_structure_instance rq_instance(float x, float y, float z, uint32_t user_index, uint8_t mask, uint64_t reference) {
	spudgpu_acceleration_structure_instance instance;
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

/* On a device created without SPUDGPU_FEATURE_RAY_QUERY: every call of the
 * extension refused with its result, a heap of acceleration structures and
 * a descriptor of one refused, and a buffer of build inputs made. On every
 * adapter, so the Iris Xe's refusals and a ray tracing GPU's are the same
 * test. */
static void test_ray_query_without_feature(spudgpu_device device) {
	CHECK(!(spudgpu_get_device_features(device) & SPUDGPU_FEATURE_RAY_QUERY));
	const float vertices[9]        = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	spudgpu_buffer input           = make_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT,
	                                             sizeof(vertices), vertices);
	spudgpu_acceleration_structure_geometry geometry = rq_triangles(input);
	spudgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, 0);
	spudgpu_acceleration_structure_sizes sizes       = rq_sizes_out();
	CHECK(spudgpu_get_acceleration_structure_build_sizes(device, &inputs, &sizes) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	spudgpu_acceleration_structure_desc desc = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, 4096, 0, false, "refused");
	spudgpu_acceleration_structure structure = NULL;
	CHECK(spudgpu_create_acceleration_structure(device, &desc, &structure) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(structure == NULL);

	spudgpu_heap_desc heap_desc;
	memset(&heap_desc, 0, sizeof(heap_desc));
	heap_desc.base.type      = SPUD_STRUCT_TYPE_GPU_HEAP_DESC;
	heap_desc.memory_kind    = SPUDGPU_MEMORY_KIND_DEVICE;
	heap_desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES;
	heap_desc.size           = SPUDGPU_HEAP_SIZE_GRANULARITY;
	spudgpu_heap heap        = NULL;
	CHECK(spudgpu_create_heap(device, &heap_desc, &heap) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(heap == NULL);
	// A heap of buffers: the feature is checked before the heap's class.
	heap_desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	CHECK(spudgpu_create_heap(device, &heap_desc, &heap) == SPUD_SUCCESS);
	if (heap)
		CHECK(spudgpu_create_placed_acceleration_structure(heap, 0, &desc, &structure) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	spudgpu_destroy_heap(heap);

	spudgpu_acceleration_structure_instance_layout layout;
	CHECK(spudgpu_get_acceleration_structure_instance_layout(device, &layout) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	const spudgpu_acceleration_structure_instance instance = rq_instance(0, 0, 0, 1, 0xFF, 0);
	uint8_t bytes[72];
	CHECK(spudgpu_write_acceleration_structure_instances(device, &instance, 1, bytes) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	CHECK(spudgpu_get_acceleration_structure_reference(NULL, NULL) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	spudgpu_destroy_acceleration_structure(NULL);

	spudgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	spudgpu_command_allocator allocator = NULL;
	CHECK(spudgpu_create_command_allocator(device, &allocator_desc, &allocator) == SPUD_SUCCESS);
	spudgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(spudgpu_create_command_list(allocator, &cmd) == SPUD_SUCCESS);
	if (cmd) {
		spudgpu_begin_command_list(cmd);
		spudgpu_acceleration_structure_build build;
		memset(&build, 0, sizeof(build));
		build.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode      = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs    = &inputs;
		CHECK(spudgpu_cmd_build_acceleration_structures(NULL, &build, 1) == SPUDRESULT_GPU_INVALID_COMMAND_LIST);
		CHECK(spudgpu_cmd_build_acceleration_structures(cmd, &build, 1) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		CHECK(spudgpu_cmd_copy_acceleration_structure(cmd, NULL, NULL, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
		      SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(cmd, NULL, 0, NULL, 0) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
		spudgpu_cmd_acceleration_structure_barrier(cmd, NULL, 0);
		spudgpu_end_command_list(cmd);
	}
	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);

	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_COMPUTE;
	layout_desc.binding_count               = 1;
	spudgpu_descriptor_set_layout set_layout = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &set_layout) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUDRESULT_GPU_EXT_RAY_QUERY_NOT_SUPPORTED);
	// A type that is none, the same refusal on every backend.
	layout_desc.bindings[0].descriptor_type = 99;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &set_layout) == SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE);
	pool_desc.pool_sizes[0].descriptor_type = 99;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUDRESULT_GPU_CANNOT_RESOLVE_API_SPECIFIC_DESCRIPTOR_TYPE);
	spudgpu_destroy_buffer(input);
	printf("spudgpu_objects_test: ray queries: every call refused on a device without SPUDGPU_FEATURE_RAY_QUERY\n");
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
	spudgpu_device device;
	spudgpu_command_queue queue;
	spudgpu_command_allocator allocator;
	spudgpu_command_list cmd;
	SPUDGPU_RAY_QUERY_LIMITS limits;
	spudgpu_acceleration_structure_instance_layout layout;
	// UPLOAD, build inputs: the triangle's vertices, the box, the
	// instances (written by the CPU call).
	spudgpu_buffer vertices, boxes, instances;
	// DEVICE, storage: every build's scratch, each at its own offset.
	spudgpu_buffer scratch;
	uint64_t scratch_bottom[2], scratch_top, scratch_update;
	// The hits, and their copy for the CPU.
	spudgpu_buffer hits, hits_readback;
	spudgpu_acceleration_structure triangle, box, top;
	spudgpu_acceleration_structure_sizes triangle_sizes, box_sizes, top_sizes;
	spudgpu_shader_module trace_module;
	spudgpu_descriptor_set_layout set_layout;
	spudgpu_descriptor_pool pool;
	spudgpu_descriptor_set set;
	spudgpu_compute_pipeline trace;
} rq_run;

static uint64_t rq_align(uint64_t value, uint64_t alignment) {
	return (value + alignment - 1) / alignment * alignment;
}

static spudgpu_buffer_barrier rq_buffer_barrier(spudgpu_buffer buffer, SPUDGPU_RESOURCE_STATE before, SPUDGPU_RESOURCE_STATE after) {
	spudgpu_buffer_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type    = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	b.buffer       = buffer;
	b.state_before = before;
	b.state_after  = after;
	return b;
}

static void rq_barrier(spudgpu_command_list cmd, spudgpu_acceleration_structure structure, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS before,
                       SPUDGPU_ACCELERATION_STRUCTURE_ACCESS after) {
	spudgpu_acceleration_structure_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type     = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
	b.structure     = structure;
	b.access_before = before;
	b.access_after  = after;
	spudgpu_cmd_acceleration_structure_barrier(cmd, &b, 1);
}

static spudgpu_acceleration_structure_build rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE mode, const spudgpu_acceleration_structure_inputs *inputs,
                                                     spudgpu_acceleration_structure source, spudgpu_acceleration_structure destination,
                                                     spudgpu_buffer scratch, uint64_t scratch_offset) {
	spudgpu_acceleration_structure_build build;
	memset(&build, 0, sizeof(build));
	build.base.type      = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
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
static void rq_dispatch_trace(rq_run *r, spudgpu_command_list cmd, uint32_t mask) {
	spudgpu_cmd_bind_compute_pipeline(cmd, r->trace);
	spudgpu_cmd_bind_descriptor_sets_compute(cmd, r->trace, 0, &r->set, 1);
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
	spudgpu_cmd_push_constants_compute(cmd, r->trace, 0, sizeof(params), &params);
	spudgpu_cmd_dispatch(cmd, (RQ_W + 7) / 8, (RQ_H + 7) / 8, 1);
}

/* Records rq_trace over [r]'s set (its top level, the hits) with [mask],
 * and the hits' copy for the CPU. The top level has had its barrier to
 * TRACE. Binds everything it uses, as it must after a build
 * (spudgpu_cmd_build_acceleration_structures()). */
static void rq_record_trace(rq_run *r, spudgpu_command_list cmd, uint32_t mask) {
	spudgpu_buffer_barrier b = rq_buffer_barrier(r->hits, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	spudgpu_cmd_pipeline_barrier(cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, cmd, mask);
	b = rq_buffer_barrier(r->hits, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_COPY_SOURCE);
	spudgpu_cmd_pipeline_barrier(cmd, &b, 1, NULL, 0);
	spudgpu_cmd_copy_buffer(cmd, r->hits, r->hits_readback, 0, 0, sizeof(rq_hit) * RQ_W * RQ_H);
}

static void rq_submit(rq_run *r) {
	spudgpu_end_command_list(r->cmd);
	CHECK(spudgpu_submit_command_lists(r->queue, &r->cmd, 1) == SPUD_SUCCESS);
	spudgpu_queue_wait_idle(r->queue);
}

static void rq_begin(rq_run *r) {
	CHECK(spudgpu_reset_command_allocator(r->allocator) == SPUD_SUCCESS);
	spudgpu_begin_command_list(r->cmd);
}

/* The hits read back, checked against the scene with the triangle at
 * [triangle] and the box at [box], through [mask]. */
static void rq_check(rq_run *r, const float triangle[3], const float box[3], uint32_t mask, const char *what) {
	void *mapped        = NULL;
	const uint64_t size = sizeof(rq_hit) * RQ_W * RQ_H;
	CHECK(spudgpu_map_buffer(r->hits_readback, 0, size, &mapped) == SPUD_SUCCESS);
	if (!mapped)
		return;
	CHECK(spudgpu_invalidate_buffer(r->hits_readback, 0, size) == SPUD_SUCCESS);
	uint32_t hit_count   = 0;
	const uint32_t wrong = rq_check_hits((const rq_hit *)mapped, triangle, box, mask, &hit_count);
	spudgpu_unmap_buffer(r->hits_readback);
	CHECK(wrong == 0);
	CHECK(hit_count > 0 || mask == 0);
	printf("spudgpu_objects_test: ray queries: %s: %u rays, %u hits, %u wrong\n", what, RQ_W * RQ_H, hit_count, wrong);
}

/* Writes [count] instances through the CPU call into [r]'s mapped
 * instance buffer at [offset]. */
static void rq_write_instances(rq_run *r, const spudgpu_acceleration_structure_instance *instances, uint32_t count, uint64_t offset) {
	void *mapped        = NULL;
	const uint64_t size = (uint64_t)r->layout.size * count;
	CHECK(spudgpu_map_buffer(r->instances, offset, size, &mapped) == SPUD_SUCCESS);
	if (!mapped)
		return;
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, instances, count, mapped) == SPUD_SUCCESS);
	CHECK(spudgpu_flush_buffer(r->instances, offset, size) == SPUD_SUCCESS);
	spudgpu_unmap_buffer(r->instances);
}

/* The write call: its refusals in order, and the bytes it writes in the
 * device's layout, checked against the layout spudgpu.h states. */
static void rq_test_write_instances(rq_run *r) {
	spudgpu_acceleration_structure_instance instances[2];
	instances[0]              = rq_instance(1.5f, -2.0f, 3.25f, 0x123456u, 0xA5, UINT64_C(0x1122334455667788));
	instances[0].transform[1] = 0.5f;
	instances[0].transform[4] = -0.5f;
	instances[0].table_offset = 0x654321u;
	instances[0].flags        = SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE | SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_DISABLE_TRIANGLE_CULLING;
	instances[1]              = rq_instance(0, 0, 0, 0, 0xFF, 0);
	uint8_t bytes[2 * 72];
	memset(bytes, 0xCD, sizeof(bytes));
	CHECK(spudgpu_write_acceleration_structure_instances(NULL, instances, 2, bytes) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, NULL, 2, bytes) == SPUDRESULT_NULL_DESC);
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, instances, 0, bytes) == SPUDRESULT_ZERO_SIZE);
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, instances, 2, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	spudgpu_acceleration_structure_instance bad[2] = {instances[0], instances[1]};
	bad[1].user_index                              = 1u << 24;
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, bad, 2, bytes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	bad[1]              = instances[1];
	bad[1].table_offset = 1u << 24;
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, bad, 2, bytes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	bad[1]       = instances[1];
	bad[1].flags = 1u << 4;
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, bad, 2, bytes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	bad[1].flags = SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE | SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_FORCE_NON_OPAQUE;
	CHECK(spudgpu_write_acceleration_structure_instances(r->device, bad, 2, bytes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	// Nothing written by a refusal, the first instance's bytes included.
	bool untouched = true;
	for (size_t i = 0; i < sizeof(bytes); ++i)
		untouched = untouched && bytes[i] == 0xCD;
	CHECK(untouched);

	CHECK(spudgpu_write_acceleration_structure_instances(r->device, instances, 2, bytes) == SPUD_SUCCESS);
	const spudgpu_acceleration_structure_instance *in = &instances[0];
	uint32_t words[18];
	memcpy(words, bytes, sizeof(words));
	uint64_t reference = 0;
	bool right         = true;
	if (r->layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64) {
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
	printf("spudgpu_objects_test: ray queries: instances written in layout %u (%u bytes, aligned %u): %s\n", r->layout.kind, r->layout.size,
	       r->layout.alignment, right ? "as the layout states" : "WRONG");
}

/* The size query's refusals, in its order. */
static void rq_test_size_refusals(rq_run *r) {
	spudgpu_acceleration_structure_geometry geometry = rq_triangles(r->vertices);
	spudgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, 0);
	spudgpu_acceleration_structure_sizes sizes       = rq_sizes_out();
	CHECK(spudgpu_get_acceleration_structure_build_sizes(NULL, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, NULL, &sizes) == SPUDRESULT_NULL_DESC);
	inputs.base.type = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_STRUCT_WRONG_TYPE);
	inputs.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	sizes.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_STRUCT_WRONG_TYPE);
	sizes = rq_sizes_out();
	// The inputs, field by field.
	inputs.level = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_NONE;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.level = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs.flags = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.flags = 1u << 7;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.flags          = 0;
	inputs.instance_count = 1;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.instance_count = 0;
	inputs.geometries     = NULL;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_NULL_DESC);
	inputs.geometries     = &geometry;
	inputs.geometry_count = 0;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_ZERO_SIZE);
	inputs.geometry_count = 1;
	geometry.kind         = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NONE;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	geometry       = rq_triangles(r->vertices);
	geometry.flags = 1u << 5;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	geometry      = rq_triangles(NULL);
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_BUFFER);
	geometry      = rq_triangles(r->scratch);
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_BUFFER_USAGE);
	geometry             = rq_triangles(r->vertices);
	geometry.data_offset = 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_offset = 0;
	geometry.data_stride = 8;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_stride = 12;
	geometry.data_count  = 0;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_ZERO_SIZE);
	geometry.data_count = 6;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	geometry.data_count   = 3;
	geometry.index_format = SPUDGPU_FORMAT_R8_UINT;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	geometry.index_format = SPUDGPU_FORMAT_UNKNOWN;
	geometry.indices      = r->vertices;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	geometry.index_format = SPUDGPU_FORMAT_R32_UINT;
	geometry.indices      = NULL;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_BUFFER);
	geometry.indices      = r->vertices;
	geometry.index_offset = 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.index_offset = 0;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_ZERO_SIZE);
	geometry.index_count = 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	geometry.index_count = 12;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	geometry            = rq_triangles(r->vertices);
	geometry.data_count = 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	// AABBs: their own alignment and stride.
	geometry             = rq_boxes(r->boxes);
	geometry.data_stride = 16;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	geometry.data_stride  = 24;
	geometry.index_format = SPUDGPU_FORMAT_R16_UINT;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	// Two geometries of two kinds.
	spudgpu_acceleration_structure_geometry mixed[2] = {rq_triangles(r->vertices), rq_boxes(r->boxes)};
	inputs.geometries                                = mixed;
	inputs.geometry_count                            = 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.geometry_count = (uint32_t)(r->limits.max_geometries > UINT32_MAX ? UINT32_MAX : r->limits.max_geometries + 1);
	if (r->limits.max_geometries < UINT32_MAX)
		CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);

	// A top level.
	inputs = rq_top_inputs(r->instances, 0, 2, NULL, 0, 0);
	inputs.geometry_count = 1;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	inputs.geometry_count = 0;
	inputs.instances      = NULL;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_BUFFER);
	inputs.instances = r->scratch;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_BUFFER_USAGE);
	inputs.instances       = r->instances;
	inputs.instance_offset = r->layout.alignment / 2;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	inputs.instance_offset = 0;
	if (r->limits.max_instances < UINT32_MAX) {
		inputs.instance_count = (uint32_t)r->limits.max_instances + 1;
		CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);
	}
	inputs.instance_count = 100;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	inputs.instance_count     = 2;
	inputs.bottom_level_count = 1;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_NULL_DESC);
	spudgpu_acceleration_structure none = NULL;
	inputs.bottom_levels                = &none;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	// Nothing was written by a refusal.
	CHECK(sizes.storage_size == 0 && sizes.build_scratch_size == 0);
}

/* The create calls' refusals, in order. */
static void rq_test_create_refusals(rq_run *r) {
	spudgpu_acceleration_structure_desc desc = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, 4096, 0, false, NULL);
	spudgpu_acceleration_structure structure = NULL;
	CHECK(spudgpu_create_acceleration_structure(NULL, &desc, &structure) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_create_acceleration_structure(r->device, NULL, &structure) == SPUDRESULT_NULL_DESC);
	desc.base.type = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, &structure) == SPUDRESULT_STRUCT_WRONG_TYPE);
	desc.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	desc.level = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_NONE;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, &structure) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.level             = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	desc.max_bottom_levels = 1;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, &structure) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	desc.max_bottom_levels = 0;
	desc.size              = 0;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, &structure) == SPUDRESULT_ZERO_SIZE);
	CHECK(structure == NULL);

	// Placed: in a heap of acceleration structures, at the device's
	// alignment, inside the heap.
	spudgpu_heap_desc heap_desc;
	memset(&heap_desc, 0, sizeof(heap_desc));
	heap_desc.base.type      = SPUD_STRUCT_TYPE_GPU_HEAP_DESC;
	heap_desc.memory_kind    = SPUDGPU_MEMORY_KIND_UPLOAD;
	heap_desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_ACCELERATION_STRUCTURES;
	heap_desc.size           = SPUDGPU_HEAP_SIZE_GRANULARITY * 4;
	heap_desc.debug_name     = "rq heap";
	spudgpu_heap heap        = NULL;
	CHECK(spudgpu_create_heap(r->device, &heap_desc, &heap) == SPUDRESULT_GPU_INVALID_MEMORY_KIND);
	heap_desc.memory_kind = SPUDGPU_MEMORY_KIND_DEVICE;
	CHECK(spudgpu_create_heap(r->device, &heap_desc, &heap) == SPUD_SUCCESS);
	heap_desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	spudgpu_heap buffers     = NULL;
	CHECK(spudgpu_create_heap(r->device, &heap_desc, &buffers) == SPUD_SUCCESS);
	desc                         = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r->triangle_sizes.storage_size, 0, false, "placed");
	const uint64_t alignment     = r->limits.placement_alignment;
	if (heap && buffers) {
		CHECK(spudgpu_create_placed_acceleration_structure(NULL, 0, &desc, &structure) == SPUDRESULT_GPU_INVALID_HEAP);
		CHECK(spudgpu_create_placed_acceleration_structure(heap, 0, NULL, &structure) == SPUDRESULT_NULL_DESC);
		CHECK(spudgpu_create_placed_acceleration_structure(heap, 0, &desc, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
		CHECK(spudgpu_create_placed_acceleration_structure(buffers, 0, &desc, &structure) == SPUDRESULT_GPU_HEAP_MISMATCH);
		CHECK(spudgpu_create_placed_acceleration_structure(heap, alignment / 2, &desc, &structure) == SPUDRESULT_GPU_PLACEMENT_MISALIGNED);
		CHECK(spudgpu_create_placed_acceleration_structure(heap, heap_desc.size, &desc, &structure) == SPUDRESULT_GPU_PLACEMENT_OUT_OF_RANGE);
		CHECK(structure == NULL);
		CHECK(spudgpu_create_placed_acceleration_structure(heap, alignment, &desc, &structure) == SPUD_SUCCESS);
		if (structure) {
			// A build into the placed structure, submitted.
			spudgpu_acceleration_structure_geometry geometry = rq_triangles(r->vertices);
			spudgpu_acceleration_structure_inputs inputs     = rq_bottom_inputs(&geometry, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE |
			                                                                                   SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION);
			spudgpu_acceleration_structure_build build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, structure,
			                                                      r->scratch, r->scratch_bottom[0]);
			rq_begin(r);
			spudgpu_buffer_barrier b = rq_buffer_barrier(r->scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
			spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
			rq_barrier(r->cmd, structure, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
			CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
			rq_submit(r);
			spudgpu_destroy_acceleration_structure(structure);
		}
		printf("spudgpu_objects_test: ray queries: a bottom level placed at %llu of a heap of acceleration structures, built\n",
		       (unsigned long long)alignment);
	}
	spudgpu_destroy_heap(buffers);
	spudgpu_destroy_heap(heap);
}

/* The build call's refusals, in order: recorded into an open list, which is
 * then ended unsubmitted (nothing was recorded). */
static void rq_test_build_refusals(rq_run *r, const spudgpu_acceleration_structure_inputs *triangle_inputs,
                                   const spudgpu_acceleration_structure_inputs *top_inputs) {
	rq_begin(r);
	spudgpu_acceleration_structure_build build =
	    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, triangle_inputs, NULL, r->triangle, r->scratch, r->scratch_bottom[0]);
	CHECK(spudgpu_cmd_build_acceleration_structures(NULL, &build, 1) == SPUDRESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, NULL, 1) == SPUDRESULT_NULL_DESC);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 0) == SPUDRESULT_ZERO_SIZE);
	spudgpu_acceleration_structure_build bad = build;
	bad.base.type                            = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_STRUCT_WRONG_TYPE);
	bad      = build;
	bad.mode = 0;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	bad        = build;
	bad.inputs = NULL;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_NULL_DESC);
	bad        = build;
	bad.source = r->box;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	bad      = build;
	bad.mode = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.source = r->top;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.source = r->triangle;
	// Never built yet.
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	bad             = build;
	bad.destination = NULL;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	bad.destination = r->top;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	// A bottom level too small for the triangle: the box's, if it is.
	if (r->box_sizes.storage_size < r->triangle_sizes.storage_size) {
		bad.destination = r->box;
		CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL);
	}
	bad         = build;
	bad.scratch = NULL;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_BUFFER);
	bad.scratch = r->vertices;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_INVALID_BUFFER_USAGE);
	bad.scratch = r->scratch;
	if (r->limits.scratch_offset_alignment > 1) {
		bad.scratch_offset = r->limits.scratch_offset_alignment / 2;
		CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	}
	spudgpu_buffer_desc scratch_desc;
	CHECK(spudgpu_get_buffer_desc(r->scratch, &scratch_desc) == SPUD_SUCCESS);
	bad.scratch_offset = rq_align(scratch_desc.size, r->limits.scratch_offset_alignment);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_SCRATCH_TOO_SMALL);
	// A top level naming more bottom levels than its destination holds.
	spudgpu_acceleration_structure three[3]       = {r->triangle, r->box, r->triangle};
	spudgpu_acceleration_structure_inputs too_many = *top_inputs;
	too_many.bottom_levels                         = three;
	too_many.bottom_level_count                    = 3;
	bad = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &too_many, NULL, r->top, r->scratch, r->scratch_top);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &bad, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_LIMIT);
	// The second of two builds refused: neither is recorded, the first's
	// destination keeps having no build.
	spudgpu_acceleration_structure_build two[2] = {build, build};
	two[1].mode                                 = 0;
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, two, 2) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	spudgpu_end_command_list(r->cmd);
	// Copies and size writes of structures never built.
	rq_begin(r);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, r->box, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
	      SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, r->scratch, 0) ==
	      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	spudgpu_end_command_list(r->cmd);
}

/* A compute pipeline of [module] over a set layout of [binding_count]
 * bindings 0..n-1 of [types], with [push_bytes] of push constants. */
static spudgpu_compute_pipeline rq_compute_pipeline(spudgpu_device device, spudgpu_shader_module module, const SPUDGPU_DESCRIPTOR_TYPE *types,
                                                    uint32_t binding_count, uint32_t push_bytes, spudgpu_descriptor_set_layout *out_layout) {
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < binding_count; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = SPUDGPU_SHADER_STAGE_COMPUTE;
	}
	layout_desc.binding_count = binding_count;
	*out_layout               = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, out_layout) == SPUD_SUCCESS);
	spudgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module                      = module;
	desc.descriptor_set_layouts[0]           = *out_layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc.push_constant_ranges[0].size        = push_bytes;
	desc.push_constant_range_count           = 1;
	spudgpu_compute_pipeline pipeline        = NULL;
	if (module && *out_layout)
		CHECK(spudgpu_create_compute_pipeline(device, &desc, &pipeline) == SPUD_SUCCESS);
	return pipeline;
}

/* Points [r]'s set at [top] and the hits. */
static void rq_write_set(rq_run *r, spudgpu_acceleration_structure top) {
	spudgpu_write_descriptor_set writes[2];
	memset(writes, 0, sizeof(writes));
	writes[0].base.type               = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
	writes[0].dst_set                 = r->set;
	writes[0].dst_binding             = 0;
	writes[0].descriptor_count        = 1;
	writes[0].descriptor_type         = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	writes[0].acceleration_structures = &top;
	const spudgpu_descriptor_buffer_info hits = {r->hits, 0, sizeof(rq_hit) * RQ_W * RQ_H};
	writes[1].base.type                       = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
	writes[1].dst_set                         = r->set;
	writes[1].dst_binding                     = 1;
	writes[1].descriptor_count                = 1;
	writes[1].descriptor_type                 = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].buffer_info                     = &hits;
	spudgpu_update_descriptor_sets(r->device, writes, 2);
}

/* The compacted triangle: its size written and read back, a structure of
 * that size made, the copy, and the top level rebuilt over it; the same
 * hits. Returns the compacted structure, which [r]'s top level now names. */
static spudgpu_acceleration_structure rq_test_compaction(rq_run *r, spudgpu_acceleration_structure_inputs *top_inputs,
                                                         spudgpu_acceleration_structure *bottoms, uint64_t box_reference,
                                                         const float triangle_at[3], const float box_at[3]) {
	spudgpu_buffer sizes = make_buffer(r->device, SPUDGPU_MEMORY_KIND_DEVICE,
	                                   SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, 64, NULL);
	spudgpu_buffer readback = make_buffer(r->device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, 64, NULL);
	if (!sizes || !readback) {
		spudgpu_destroy_buffer(sizes);
		spudgpu_destroy_buffer(readback);
		return NULL;
	}
	rq_begin(r);
	// Refusals first, in order, then the write.
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(NULL, &r->triangle, 1, sizes, 0) == SPUDRESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, NULL, 1, sizes, 0) == SPUDRESULT_NULL_DESC);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 0, sizes, 0) == SPUDRESULT_ZERO_SIZE);
	spudgpu_acceleration_structure none = NULL;
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &none, 1, sizes, 0) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->box, 1, sizes, 0) ==
	      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, NULL, 0) == SPUDRESULT_GPU_INVALID_BUFFER);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, readback, 0) == SPUDRESULT_GPU_INVALID_BUFFER_USAGE);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 4) ==
	      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_ALIGNMENT);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 64) ==
	      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_RANGE);
	rq_barrier(r->cmd, r->triangle, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	spudgpu_buffer_barrier b = rq_buffer_barrier(sizes, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, &r->triangle, 1, sizes, 8) == SPUD_SUCCESS);
	b = rq_buffer_barrier(sizes, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST, SPUDGPU_RESOURCE_STATE_COPY_SOURCE);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	spudgpu_cmd_copy_buffer(r->cmd, sizes, readback, 8, 0, 8);
	rq_submit(r);
	uint64_t compacted = 0;
	void *mapped       = NULL;
	CHECK(spudgpu_map_buffer(readback, 0, 8, &mapped) == SPUD_SUCCESS);
	if (mapped) {
		CHECK(spudgpu_invalidate_buffer(readback, 0, 8) == SPUD_SUCCESS);
		memcpy(&compacted, mapped, 8);
		spudgpu_unmap_buffer(readback);
	}
	spudgpu_destroy_buffer(readback);
	spudgpu_destroy_buffer(sizes);
	CHECK(compacted > 0 && compacted <= r->triangle_sizes.storage_size);
	printf("spudgpu_objects_test: ray queries: the triangle's bottom level is %llu bytes, %llu compacted\n",
	       (unsigned long long)r->triangle_sizes.storage_size, (unsigned long long)compacted);
	if (compacted == 0)
		return NULL;

	spudgpu_acceleration_structure_desc desc = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, compacted, 0, false, "compacted triangle");
	spudgpu_acceleration_structure compact    = NULL;
	CHECK(spudgpu_create_acceleration_structure(r->device, &desc, &compact) == SPUD_SUCCESS);
	if (!compact)
		return NULL;
	uint64_t reference = 0;
	CHECK(spudgpu_get_acceleration_structure_reference(compact, &reference) == SPUD_SUCCESS);
	const spudgpu_acceleration_structure_instance instances[2] = {
	    rq_instance(triangle_at[0], triangle_at[1], triangle_at[2], RQ_TRIANGLE_ID, 0x01, reference),
	    rq_instance(box_at[0], box_at[1], box_at[2], RQ_BOX_ID, 0x02, box_reference)};
	rq_write_instances(r, instances, 2, 0);
	bottoms[0] = compact;

	rq_begin(r);
	CHECK(spudgpu_cmd_copy_acceleration_structure(NULL, r->triangle, compact, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      SPUDRESULT_GPU_INVALID_COMMAND_LIST);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, NULL, compact, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, r->triangle, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, r->top, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, compact, 0) == SPUDRESULT_DESC_INVALID_PARAMETERS);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->box, compact, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) ==
	      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMPACTION_REFUSED);
	if (compacted < r->triangle_sizes.storage_size)
		CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, compact, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ==
		      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_TOO_SMALL);
	rq_barrier(r->cmd, r->triangle, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, compact, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, r->triangle, compact, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) == SPUD_SUCCESS);
	rq_barrier(r->cmd, compact, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, r->box, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	b = rq_buffer_barrier(r->scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	spudgpu_acceleration_structure_build build =
	    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, top_inputs, NULL, r->top, r->scratch, r->scratch_top);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
	rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	rq_record_trace(r, r->cmd, 0xFF);
	rq_submit(r);
	rq_check(r, triangle_at, box_at, 0xFF, "over the compacted triangle");
	return compact;
}

/* The caller's GPU writer for the device's layout kind: the instances
 * written by a compute shader, compared byte for byte with the CPU call's,
 * then a top level built from them and traced. */
static void rq_test_gpu_writer(rq_run *r, const spudgpu_acceleration_structure_inputs *top_inputs_cpu, const uint64_t references[2],
                               const float triangle_at[3], const float box_at[3]) {
	const spudgpu_test_shader *shader = r->layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72
	                                        ? &spudgpu_test_rq_instances_col72
	                                        : &spudgpu_test_rq_instances_row64;
	spudgpu_shader_module module = make_module(r->device, shader, "rq instance writer");
	const SPUDGPU_DESCRIPTOR_TYPE types[2]  = {SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	spudgpu_descriptor_set_layout layout = NULL;
	spudgpu_compute_pipeline pipeline    = rq_compute_pipeline(r->device, module, types, 2, 8, &layout);

	// The writer's input: 20 words an instance.
	uint32_t words[2][20];
	memset(words, 0, sizeof(words));
	const spudgpu_acceleration_structure_instance instances[2] = {
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
	spudgpu_buffer upload    = make_buffer(r->device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, sizeof(words), words);
	spudgpu_buffer input     = make_buffer(r->device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_DST,
	                                       sizeof(words), NULL);
	spudgpu_buffer output = make_buffer(r->device, SPUDGPU_MEMORY_KIND_DEVICE,
	                                    SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC,
	                                    out_bytes, NULL);
	spudgpu_buffer readback = make_buffer(r->device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, out_bytes, NULL);
	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[0].count           = 2;
	pool_desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(r->device, &pool_desc, &pool) == SPUD_SUCCESS);
	spudgpu_descriptor_set set = NULL;
	if (pool && layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(r->device, &set_desc, &set) == SPUD_SUCCESS);
	}
	if (pipeline && set && upload && input && output && readback) {
		const spudgpu_descriptor_buffer_info infos[2] = {{input, 0, sizeof(words)}, {output, 0, out_bytes}};
		spudgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		for (uint32_t i = 0; i < 2; ++i) {
			writes[i].base.type        = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			writes[i].dst_set          = set;
			writes[i].dst_binding      = i;
			writes[i].descriptor_count = 1;
			writes[i].descriptor_type  = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].buffer_info      = &infos[i];
		}
		spudgpu_update_descriptor_sets(r->device, writes, 2);

		rq_begin(r);
		spudgpu_buffer_barrier b[2] = {rq_buffer_barrier(input, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST),
		                               rq_buffer_barrier(output, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS)};
		spudgpu_cmd_pipeline_barrier(r->cmd, b, 2, NULL, 0);
		spudgpu_cmd_copy_buffer(r->cmd, upload, input, 0, 0, sizeof(words));
		b[0] = rq_buffer_barrier(input, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
		spudgpu_cmd_pipeline_barrier(r->cmd, b, 1, NULL, 0);
		spudgpu_cmd_bind_compute_pipeline(r->cmd, pipeline);
		spudgpu_cmd_bind_descriptor_sets_compute(r->cmd, pipeline, 0, &set, 1);
		const uint32_t push[2] = {2, 0};
		spudgpu_cmd_push_constants_compute(r->cmd, pipeline, 0, sizeof(push), push);
		spudgpu_cmd_dispatch(r->cmd, 1, 1, 1);
		b[0] = rq_buffer_barrier(output, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_COPY_SOURCE);
		spudgpu_cmd_pipeline_barrier(r->cmd, b, 1, NULL, 0);
		spudgpu_cmd_copy_buffer(r->cmd, output, readback, 0, 0, out_bytes);
		// The top level, built from what the GPU wrote.
		b[0] = rq_buffer_barrier(output, SPUDGPU_RESOURCE_STATE_COPY_SOURCE, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
		b[1] = rq_buffer_barrier(r->scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		spudgpu_cmd_pipeline_barrier(r->cmd, b, 2, NULL, 0);
		spudgpu_acceleration_structure_inputs inputs = *top_inputs_cpu;
		inputs.instances                             = output;
		inputs.instance_offset                       = 0;
		for (uint32_t i = 0; i < inputs.bottom_level_count; ++i)
			rq_barrier(r->cmd, inputs.bottom_levels[i], SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		spudgpu_acceleration_structure_build build =
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, r->top, r->scratch, r->scratch_top);
		CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
		rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		rq_record_trace(r, r->cmd, 0xFF);
		rq_submit(r);

		uint8_t cpu_bytes[2 * 72];
		CHECK(spudgpu_write_acceleration_structure_instances(r->device, instances, 2, cpu_bytes) == SPUD_SUCCESS);
		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(readback, 0, out_bytes, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			CHECK(spudgpu_invalidate_buffer(readback, 0, out_bytes) == SPUD_SUCCESS);
			const bool same = memcmp(mapped, cpu_bytes, (size_t)out_bytes) == 0;
			CHECK(same);
			printf("spudgpu_objects_test: ray queries: %s wrote the same %llu bytes as the CPU call: %s\n", shader->entry_point,
			       (unsigned long long)out_bytes, same ? "yes" : "NO");
			spudgpu_unmap_buffer(readback);
		}
		rq_check(r, triangle_at, box_at, 0xFF, "a top level of GPU-written instances");
	}
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_buffer(readback);
	spudgpu_destroy_buffer(output);
	spudgpu_destroy_buffer(input);
	spudgpu_destroy_buffer(upload);
	spudgpu_destroy_compute_pipeline(pipeline);
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(module);
}

#define RQ_SHADOW_W 32u
#define RQ_SHADOW_H 16u

/* rq_shadow.slang drawn over a ground plane under the scene, its rays going
 * up: shadowed (0) under the triangle, lit (1) elsewhere. The box's sphere
 * casts none: the shader commits no procedural candidate (its loop is
 * empty, and an AABB is the shader's to decide even when forced opaque). */
static void rq_test_shadow(rq_run *r, const float triangle_at[3]) {
	spudgpu_shader_module vert = make_module(r->device, &spudgpu_test_rq_shadow_vert, "rq shadow vertex");
	spudgpu_shader_module frag = make_module(r->device, &spudgpu_test_rq_shadow, "rq shadow fragment");
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_FRAGMENT;
	layout_desc.binding_count               = 1;
	spudgpu_descriptor_set_layout layout    = NULL;
	CHECK(spudgpu_create_descriptor_set_layout(r->device, &layout_desc, &layout) == SPUD_SUCCESS);
	spudgpu_shader_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
	desc.debug_name                          = "rq shadow";
	desc.vertex_module                       = vert;
	desc.fragment_module                     = frag;
	desc.primitive_topology                  = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.cull_mode                           = SPUDGPU_CULL_MODE_NONE;
	desc.color_attachment_formats[0]         = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	desc.color_attachment_count              = 1;
	desc.depth_format                        = SPUDGPU_FORMAT_UNKNOWN;
	desc.descriptor_set_layouts[0]           = layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
	desc.push_constant_ranges[0].size        = 48;
	desc.push_constant_range_count           = 1;
	spudgpu_shader_pipeline pipeline         = NULL;
	if (vert && frag && layout)
		CHECK(spudgpu_create_shader_pipeline(r->device, &desc, &pipeline) == SPUD_SUCCESS);
	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool            = NULL;
	CHECK(spudgpu_create_descriptor_pool(r->device, &pool_desc, &pool) == SPUD_SUCCESS);
	spudgpu_descriptor_set set = NULL;
	if (pool && layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(r->device, &set_desc, &set) == SPUD_SUCCESS);
	}
	spudgpu_image_desc image_desc;
	memset(&image_desc, 0, sizeof(image_desc));
	image_desc.base.type          = SPUD_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage              = SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type               = SPUDGPU_IMAGE_TYPE_2D;
	image_desc.format             = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.format = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.color[1] = 0.5f;
	image_desc.clear_value.color[3] = 1.0f;
	image_desc.width              = RQ_SHADOW_W;
	image_desc.height             = RQ_SHADOW_H;
	image_desc.depth              = 1;
	image_desc.array_layers       = 1;
	image_desc.mip_levels         = 1;
	image_desc.debug_name         = "rq shadow target";
	spudgpu_image image           = NULL;
	CHECK(spudgpu_create_image(r->device, &image_desc, &image) == SPUD_SUCCESS);
	spudgpu_image_view view = make_image_view(image, SPUDGPU_IMAGE_ASPECT_COLOR);
	uint64_t row_pitch = 0, total = 0;
	if (image)
		spudgpu_get_image_buffer_copy_size(image, 0, &row_pitch, &total);
	spudgpu_buffer readback = total ? make_buffer(r->device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, total, NULL) : NULL;

	if (pipeline && set && view && readback) {
		spudgpu_write_descriptor_set write;
		memset(&write, 0, sizeof(write));
		write.base.type               = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		write.dst_set                 = set;
		write.descriptor_count        = 1;
		write.descriptor_type         = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		write.acceleration_structures = &r->top;
		spudgpu_update_descriptor_sets(r->device, &write, 1);

		rq_begin(r);
		rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		spudgpu_cmd_image_barrier_view(r->cmd, view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		spudgpu_rendering_begin_desc pass;
		memset(&pass, 0, sizeof(pass));
		pass.base.type                           = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
		pass.color_attachments[0].image_view     = view;
		pass.color_attachments[0].load_op        = SPUDGPU_LOAD_OP_CLEAR;
		pass.color_attachments[0].store_op       = SPUDGPU_STORE_OP_STORE;
		pass.color_attachments[0].clear_color[1] = 0.5f;
		pass.color_attachments[0].clear_color[3] = 1.0f;
		pass.color_attachment_count              = 1;
		pass.width                               = RQ_SHADOW_W;
		pass.height                              = RQ_SHADOW_H;
		spudgpu_cmd_begin_rendering(r->cmd, &pass);
		SPUDGPU_VIEWPORT viewport = {0.0f, 0.0f, (float)RQ_SHADOW_W, (float)RQ_SHADOW_H, 0.0f, 1.0f};
		spudgpu_cmd_set_viewports(r->cmd, 0, 1, &viewport);
		SPUDGPU_SCISSOR_RECT scissor = {0.0f, 0.0f, (float)RQ_SHADOW_W, (float)RQ_SHADOW_H};
		spudgpu_cmd_set_scissor_rects(r->cmd, 0, 1, &scissor);
		spudgpu_cmd_bind_pipeline(r->cmd, pipeline);
		spudgpu_cmd_bind_descriptor_sets(r->cmd, pipeline, 0, &set, 1);
		// rq_shadow's 32 bytes (toward +z, t from 0.001 to 100, every
		// instance), then the plane: 4 x 2 at z = -0.5.
		const float push[12] = {0.0f, 0.0f, 1.0f, 0.001f, 100.0f, 0.0f, 0.0f, 0.0f, 4.0f, 2.0f, -0.5f, 0.0f};
		uint32_t mask_word   = 0xFFu;
		float pushed[12];
		memcpy(pushed, push, sizeof(push));
		memcpy(&pushed[5], &mask_word, 4);
		spudgpu_cmd_push_constants(r->cmd, pipeline, 0, sizeof(pushed), pushed);
		spudgpu_cmd_draw(r->cmd, 6, 0);
		// Inside the pass, every build, copy and size write is refused
		// (card S26), before its other arguments are read, and the pass
		// goes on.
		CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, NULL, 0) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, NULL, NULL, 0) ==
		      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(r->cmd, NULL, 0, NULL, 0) ==
		      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_INSIDE_RENDERING);
		spudgpu_cmd_end_rendering(r->cmd);
		spudgpu_cmd_image_barrier_view(r->cmd, view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC);
		spudgpu_image_buffer_copy_desc copy;
		memset(&copy, 0, sizeof(copy));
		copy.buffer_row_length = (uint32_t)(row_pitch / 4);
		copy.array_layer_count = 1;
		copy.width             = RQ_SHADOW_W;
		copy.height            = RQ_SHADOW_H;
		copy.depth             = 1;
		spudgpu_cmd_copy_image_to_buffer(r->cmd, image, readback, &copy);
		rq_submit(r);

		void *mapped = NULL;
		CHECK(spudgpu_map_buffer(readback, 0, total, &mapped) == SPUD_SUCCESS);
		if (mapped) {
			CHECK(spudgpu_invalidate_buffer(readback, 0, total) == SPUD_SUCCESS);
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
			printf("spudgpu_objects_test: ray queries: rq_shadow over %u x %u pixels, %u in the triangle's shadow, %u wrong\n", RQ_SHADOW_W,
			       RQ_SHADOW_H, shadowed, wrong);
			spudgpu_unmap_buffer(readback);
		}
	}
	spudgpu_destroy_buffer(readback);
	spudgpu_destroy_image_view(view);
	spudgpu_destroy_image(image);
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_shader_pipeline(pipeline);
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(frag);
	spudgpu_destroy_shader_module(vert);
}

/* On a device created with SPUDGPU_FEATURE_RAY_QUERY: a bottom level of one
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
	spudgpu_command_allocator_desc d;
	memset(&d, 0, sizeof(d));
	d.base.type                         = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	d.type                              = SPUDGPU_COMMAND_LIST_TYPE_COPY;
	spudgpu_command_allocator allocator = NULL;
	spudgpu_command_list cmd            = NULL;
	CHECK(spudgpu_create_command_allocator(r->device, &d, &allocator) == SPUD_SUCCESS);
	if (allocator)
		CHECK(spudgpu_create_command_list(allocator, &cmd) == SPUD_SUCCESS);
	if (cmd) {
		spudgpu_begin_command_list(cmd);
		const spudgpu_acceleration_structure_build build =
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, NULL, NULL, r->triangle, r->scratch, 0);
		CHECK(spudgpu_cmd_build_acceleration_structures(cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(spudgpu_cmd_build_acceleration_structures(cmd, NULL, 0) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(spudgpu_cmd_copy_acceleration_structure(cmd, NULL, NULL, 0) ==
		      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		CHECK(spudgpu_cmd_write_acceleration_structure_compacted_sizes(cmd, NULL, 0, NULL, 0) ==
		      SPUDRESULT_GPU_ACCELERATION_STRUCTURE_COMMAND_LIST_TYPE);
		spudgpu_end_command_list(cmd);
	}
	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
	printf("spudgpu_objects_test: ray queries: a copy list's build, copy and size write refused\n");
}

/* An update is taken only of exactly its source's last layout (fix 6 of the
 * review: the values themselves, compared one by one). A bottom level built
 * with ALLOW_UPDATE is updated in place; updates with another geometry
 * count, another geometry's flags or other build flags are refused; a
 * clone takes its source's layout and is updated in turn; the top level
 * refuses an update of another instance count. */
static void rq_test_layout_updates(rq_run *r, const spudgpu_acceleration_structure_inputs *top_inputs) {
	spudgpu_acceleration_structure_geometry geometries[2] = {rq_triangles(r->vertices), rq_triangles(r->vertices)};
	spudgpu_acceleration_structure_inputs inputs = rq_bottom_inputs(geometries, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);
	inputs.geometry_count                        = 2;
	spudgpu_acceleration_structure_sizes sizes = rq_sizes_out();
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r->device, &inputs, &sizes) == SPUD_SUCCESS);
	CHECK(sizes.storage_size > 0);
	const uint64_t scratch_size =
	    sizes.build_scratch_size > sizes.update_scratch_size ? sizes.build_scratch_size : sizes.update_scratch_size;
	spudgpu_buffer scratch = make_buffer(r->device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE, scratch_size, NULL);
	spudgpu_acceleration_structure_desc d = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, sizes.storage_size, 0, false, "updated");
	spudgpu_acceleration_structure updated = NULL, clone = NULL;
	CHECK(spudgpu_create_acceleration_structure(r->device, &d, &updated) == SPUD_SUCCESS);
	d.debug_name = "updated clone";
	CHECK(spudgpu_create_acceleration_structure(r->device, &d, &clone) == SPUD_SUCCESS);
	if (!scratch || !updated || !clone) {
		spudgpu_destroy_acceleration_structure(clone);
		spudgpu_destroy_acceleration_structure(updated);
		spudgpu_destroy_buffer(scratch);
		return;
	}

	rq_begin(r);
	spudgpu_buffer_barrier b = rq_buffer_barrier(scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_barrier(r->cmd, updated, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	spudgpu_acceleration_structure_build build =
	    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &inputs, NULL, updated, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
	rq_barrier(r->cmd, updated, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	b = rq_buffer_barrier(scratch, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);

	// Refused: one geometry fewer, a geometry's flags, the build's flags.
	spudgpu_acceleration_structure_inputs other = inputs;
	other.geometry_count                        = 1;
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	spudgpu_acceleration_structure_geometry other_geometries[2] = {geometries[0], geometries[1]};
	other_geometries[1].flags = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_CANDIDATES;
	other            = inputs;
	other.geometries = other_geometries;
	build            = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	other = inputs;
	other.flags |= SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, updated, updated, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	// The same layout, in place.
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &inputs, updated, updated, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);

	// A clone takes the layout, and is updated as its source would be.
	rq_barrier(r->cmd, updated, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	rq_barrier(r->cmd, clone, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	CHECK(spudgpu_cmd_copy_acceleration_structure(r->cmd, updated, clone, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) == SPUD_SUCCESS);
	rq_barrier(r->cmd, clone, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	other                = inputs;
	other.geometry_count = 1;
	build                = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other, clone, clone, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &inputs, clone, clone, scratch, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);

	// The top level, built over two instances: an update over one refused.
	spudgpu_acceleration_structure_inputs top_other = *top_inputs;
	top_other.instance_count                        = 1;
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &top_other, r->top, r->top, r->scratch, r->scratch_update);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
	rq_submit(r);
	spudgpu_destroy_acceleration_structure(clone);
	spudgpu_destroy_acceleration_structure(updated);
	spudgpu_destroy_buffer(scratch);
	printf("spudgpu_objects_test: ray queries: updates taken of exactly the last layout, by a structure and its clone\n");
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
static void rq_test_shared_scratch(rq_run *r, const spudgpu_acceleration_structure_inputs *triangle_inputs,
                                   const spudgpu_acceleration_structure_inputs *box_inputs,
                                   const spudgpu_acceleration_structure_inputs *top_inputs, const float triangle_at[3],
                                   const float box_at[3]) {
	uint64_t size = sizeof(rq_hit) * RQ_W * RQ_H;
	if (r->triangle_sizes.build_scratch_size > size)
		size = r->triangle_sizes.build_scratch_size;
	if (r->box_sizes.build_scratch_size > size)
		size = r->box_sizes.build_scratch_size;
	if (r->top_sizes.build_scratch_size > size)
		size = r->top_sizes.build_scratch_size;
	spudgpu_buffer shared =
	    make_buffer(r->device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC, size, NULL);
	if (!shared)
		return;
	spudgpu_buffer hits = r->hits;
	r->hits             = shared;
	rq_write_set(r, r->top);

	rq_begin(r);
	spudgpu_acceleration_structure bottoms[2] = {r->triangle, r->box};
	spudgpu_acceleration_structure_barrier ab[2];
	memset(ab, 0, sizeof(ab));
	for (int i = 0; i < 2; ++i) {
		ab[i].base.type     = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
		ab[i].structure     = bottoms[i];
		ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
		ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	}
	spudgpu_cmd_acceleration_structure_barrier(r->cmd, ab, 2);
	rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	spudgpu_buffer_barrier b = rq_buffer_barrier(shared, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, r->cmd, 0xFF);

	// The traced structures rebuilt, as they were, each build's scratch at
	// the buffer's start.
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
		ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
	}
	spudgpu_cmd_acceleration_structure_barrier(r->cmd, ab, 2);
	rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	b = rq_buffer_barrier(shared, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	spudgpu_acceleration_structure_build build =
	    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, triangle_inputs, NULL, r->triangle, shared, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
	b = rq_buffer_barrier(shared, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, box_inputs, NULL, r->box, shared, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
		ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
	}
	spudgpu_cmd_acceleration_structure_barrier(r->cmd, ab, 2);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	build = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, top_inputs, NULL, r->top, shared, 0);
	CHECK(spudgpu_cmd_build_acceleration_structures(r->cmd, &build, 1) == SPUD_SUCCESS);
	rq_barrier(r->cmd, r->top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	for (int i = 0; i < 2; ++i) {
		ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	}
	spudgpu_cmd_acceleration_structure_barrier(r->cmd, ab, 2);

	// The hits traced into the buffer the builds used, after the last
	// build's writes. Everything the dispatch uses bound again.
	b = rq_buffer_barrier(shared, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	rq_dispatch_trace(r, r->cmd, 0xFF);
	b = rq_buffer_barrier(shared, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_COPY_SOURCE);
	spudgpu_cmd_pipeline_barrier(r->cmd, &b, 1, NULL, 0);
	spudgpu_cmd_copy_buffer(r->cmd, shared, r->hits_readback, 0, 0, sizeof(rq_hit) * RQ_W * RQ_H);
	rq_submit(r);
	rq_check(r, triangle_at, box_at, 0xFF, "three builds sharing one scratch range with a storage buffer's dispatches");

	r->hits = hits;
	rq_write_set(r, r->top);
	spudgpu_destroy_buffer(shared);
}

static void test_ray_queries(spudgpu_adapter adapter, SPUDGPU_FEATURES supported) {
	if (!(supported & SPUDGPU_FEATURE_RAY_QUERY)) {
		printf("spudgpu_objects_test: ray queries: the adapter has no SPUDGPU_FEATURE_RAY_QUERY, the device tests skipped (its refusals ran)\n");
		return;
	}
	rq_run r;
	memset(&r, 0, sizeof(r));
	memset(&r.limits, 0, sizeof(r.limits));
	r.limits.base.type = SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS;
	SPUDGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	properties.base.next = &r.limits.base;
	CHECK(spudgpu_get_adapter_properties(adapter, &properties) == SPUD_SUCCESS);
	CHECK(r.limits.max_geometries > 0 && r.limits.max_instances > 0 && r.limits.max_primitives > 0);
	CHECK(r.limits.scratch_offset_alignment > 0 && (r.limits.scratch_offset_alignment & (r.limits.scratch_offset_alignment - 1)) == 0);
	CHECK(r.limits.placement_alignment > 0 && (r.limits.placement_alignment & (r.limits.placement_alignment - 1)) == 0);
	printf("spudgpu_objects_test: ray queries: limits: %llu geometries, %llu instances, %llu primitives, scratch at %u, placed at %u\n",
	       (unsigned long long)r.limits.max_geometries, (unsigned long long)r.limits.max_instances, (unsigned long long)r.limits.max_primitives,
	       r.limits.scratch_offset_alignment, r.limits.placement_alignment);

	spudgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = SPUDGPU_FEATURE_RAY_QUERY;
	desc.direct_queue_count = 1;
	// A copy queue, whose lists every build, copy and size write refuses
	// (rq_test_list_refusals).
	desc.copy_queue_count   = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	desc.debug_name         = "spudgpu_objects_test ray queries";
	CHECK(spudgpu_create_device(adapter, &desc, &r.device) == SPUD_SUCCESS);
	if (!r.device)
		return;
	CHECK(spudgpu_get_command_queue(r.device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &r.queue) == SPUD_SUCCESS);
	spudgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(spudgpu_create_command_allocator(r.device, &allocator_desc, &r.allocator) == SPUD_SUCCESS);
	if (r.allocator)
		CHECK(spudgpu_create_command_list(r.allocator, &r.cmd) == SPUD_SUCCESS);

	const SPUDRESULT layout_sr = spudgpu_get_acceleration_structure_instance_layout(r.device, &r.layout);
	if (!r.cmd) {
		spudgpu_destroy_command_list(r.cmd);
		spudgpu_destroy_command_allocator(r.allocator);
		spudgpu_destroy_device(r.device);
		return;
	}
	CHECK(layout_sr == SPUD_SUCCESS);
	CHECK(spudgpu_get_acceleration_structure_instance_layout(NULL, &r.layout) == SPUDRESULT_GPU_INVALID_DEVICE);
	CHECK(spudgpu_get_acceleration_structure_instance_layout(r.device, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
#if SPUDGPU_COMPILE_METAL_API
	CHECK(r.layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 && r.layout.size == 72);
#else
	CHECK(r.layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64 && r.layout.size == 64);
#endif
	rq_test_write_instances(&r);

	// The scene's buffers. The vertices of the triangle (0,0,0) (1,0,0)
	// (0,1,0), and the box [0,1]^3: the sphere rq_trace intersects in it.
	const float vertices[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const float box[6]      = {0, 0, 0, 1, 1, 1};
	const SPUDGPU_BUFFER_USAGE input_usage = SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT;
	r.vertices      = make_buffer(r.device, SPUDGPU_MEMORY_KIND_UPLOAD, input_usage, sizeof(vertices), vertices);
	r.boxes         = make_buffer(r.device, SPUDGPU_MEMORY_KIND_UPLOAD, input_usage, sizeof(box), box);
	r.instances     = make_buffer(r.device, SPUDGPU_MEMORY_KIND_UPLOAD, input_usage, (uint64_t)r.layout.size * 4, NULL);
	r.hits          = make_buffer(r.device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC,
	                              sizeof(rq_hit) * RQ_W * RQ_H, NULL);
	r.hits_readback = make_buffer(r.device, SPUDGPU_MEMORY_KIND_READBACK, SPUDGPU_BUFFER_USAGE_TRANSFER_DST, sizeof(rq_hit) * RQ_W * RQ_H, NULL);

	spudgpu_acceleration_structure_geometry triangle_geometry = rq_triangles(r.vertices);
	spudgpu_acceleration_structure_geometry box_geometry      = rq_boxes(r.boxes);
	spudgpu_acceleration_structure_inputs triangle_inputs     = rq_bottom_inputs(
	    &triangle_geometry, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION);
	spudgpu_acceleration_structure_inputs box_inputs = rq_bottom_inputs(&box_geometry, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD);
	spudgpu_acceleration_structure bottoms[2]        = {NULL, NULL};
	spudgpu_acceleration_structure_inputs top_inputs =
	    rq_top_inputs(r.instances, 0, 2, bottoms, 2, SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE);

	r.triangle_sizes = rq_sizes_out();
	r.box_sizes      = rq_sizes_out();
	r.top_sizes      = rq_sizes_out();
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r.device, &triangle_inputs, &r.triangle_sizes) == SPUD_SUCCESS);
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r.device, &box_inputs, &r.box_sizes) == SPUD_SUCCESS);
	CHECK(r.triangle_sizes.storage_size > 0 && r.triangle_sizes.build_scratch_size > 0 && r.triangle_sizes.update_scratch_size == 0);
	CHECK(r.box_sizes.storage_size > 0);

	// Every build's scratch at its own offset of one buffer.
	const uint64_t a  = r.limits.scratch_offset_alignment;
	r.scratch_bottom[0] = 0;
	r.scratch_bottom[1] = rq_align(r.triangle_sizes.build_scratch_size, a);
	r.scratch_top       = r.scratch_bottom[1] + rq_align(r.box_sizes.build_scratch_size, a);
	// The top level's sizes need its instance buffer, so the scratch's
	// rest is sized once it exists (below).
	r.scratch = make_buffer(r.device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE, r.scratch_top + 1024 * 1024, NULL);
	if (!r.vertices || !r.boxes || !r.instances || !r.hits || !r.hits_readback || !r.scratch)
		goto done;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r.device, &top_inputs, &r.top_sizes) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	rq_test_size_refusals(&r);

	{
		spudgpu_acceleration_structure_desc d =
		    rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r.triangle_sizes.storage_size, 0, true, "triangle");
		CHECK(spudgpu_create_acceleration_structure(r.device, &d, &r.triangle) == SPUD_SUCCESS);
		d = rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, r.box_sizes.storage_size, 0, false, "box");
		CHECK(spudgpu_create_acceleration_structure(r.device, &d, &r.box) == SPUD_SUCCESS);
	}
	if (!r.triangle || !r.box)
		goto done;
	bottoms[0] = r.triangle;
	bottoms[1] = r.box;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(r.device, &top_inputs, &r.top_sizes) == SPUD_SUCCESS);
	// An update may need no scratch at all (WARP asks for none).
	CHECK(r.top_sizes.storage_size > 0);
	r.scratch_update = r.scratch_top + rq_align(r.top_sizes.build_scratch_size, a);
	if (r.scratch_update + r.top_sizes.update_scratch_size > r.scratch_top + 1024 * 1024) {
		printf("spudgpu_objects_test: ray queries: the top level's scratch is larger than the test's buffer\n");
		CHECK(false);
		goto done;
	}
	{
		spudgpu_acceleration_structure_desc d =
		    rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, r.top_sizes.storage_size, 2, false, "top");
		CHECK(spudgpu_create_acceleration_structure(r.device, &d, &r.top) == SPUD_SUCCESS);
	}
	if (!r.top)
		goto done;
	rq_test_create_refusals(&r);

	uint64_t references[2] = {0, 0};
	CHECK(spudgpu_get_acceleration_structure_reference(NULL, &references[0]) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_get_acceleration_structure_reference(r.top, &references[0]) == SPUDRESULT_GPU_INVALID_ACCELERATION_STRUCTURE);
	CHECK(spudgpu_get_acceleration_structure_reference(r.triangle, NULL) == SPUDRESULT_NULL_OUTPUT_PARAMETER);
	CHECK(spudgpu_get_acceleration_structure_reference(r.triangle, &references[0]) == SPUD_SUCCESS);
	CHECK(spudgpu_get_acceleration_structure_reference(r.box, &references[1]) == SPUD_SUCCESS);
	CHECK(references[0] != 0 && references[1] != 0 && references[0] != references[1]);

	{
		const float triangle_at[3] = {0.0f, 0.0f, 2.0f};
		const float box_at[3]      = {2.0f, 0.0f, 0.0f};
		const spudgpu_acceleration_structure_instance instances[2] = {
		    rq_instance(triangle_at[0], triangle_at[1], triangle_at[2], RQ_TRIANGLE_ID, 0x01, references[0]),
		    rq_instance(box_at[0], box_at[1], box_at[2], RQ_BOX_ID, 0x02, references[1])};
		rq_write_instances(&r, instances, 2, 0);
		rq_test_build_refusals(&r, &triangle_inputs, &top_inputs);

		// The trace pipeline and its set.
		r.trace_module = make_module(r.device, &spudgpu_test_rq_trace, "rq_trace");
		const SPUDGPU_DESCRIPTOR_TYPE types[2] = {SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
		r.trace = rq_compute_pipeline(r.device, r.trace_module, types, 2, sizeof(rq_trace_params), &r.set_layout);
		spudgpu_descriptor_pool_desc pool_desc;
		memset(&pool_desc, 0, sizeof(pool_desc));
		pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
		pool_desc.max_sets                      = 1;
		pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		pool_desc.pool_sizes[0].count           = 1;
		pool_desc.pool_sizes[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		pool_desc.pool_sizes[1].count           = 1;
		pool_desc.pool_size_count               = 2;
		CHECK(spudgpu_create_descriptor_pool(r.device, &pool_desc, &r.pool) == SPUD_SUCCESS);
		if (r.pool && r.set_layout) {
			spudgpu_descriptor_set_desc set_desc;
			memset(&set_desc, 0, sizeof(set_desc));
			set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
			set_desc.pool           = r.pool;
			set_desc.set_layouts[0] = r.set_layout;
			set_desc.set_count      = 1;
			CHECK(spudgpu_create_descriptor_sets(r.device, &set_desc, &r.set) == SPUD_SUCCESS);
		}
		if (!r.trace || !r.set)
			goto done;
		rq_write_set(&r, r.top);

		// The two bottom levels in one call, then the top level over them,
		// then the grid.
		rq_begin(&r);
		spudgpu_buffer_barrier b = rq_buffer_barrier(r.scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		spudgpu_cmd_pipeline_barrier(r.cmd, &b, 1, NULL, 0);
		spudgpu_acceleration_structure_build builds[2] = {
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &triangle_inputs, NULL, r.triangle, r.scratch, r.scratch_bottom[0]),
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &box_inputs, NULL, r.box, r.scratch, r.scratch_bottom[1])};
		CHECK(spudgpu_cmd_build_acceleration_structures(r.cmd, builds, 2) == SPUD_SUCCESS);
		spudgpu_acceleration_structure_barrier ab[2];
		memset(ab, 0, sizeof(ab));
		for (int i = 0; i < 2; ++i) {
			ab[i].base.type     = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
			ab[i].structure     = bottoms[i];
			ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
			ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		}
		spudgpu_cmd_acceleration_structure_barrier(r.cmd, ab, 2);
		rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		spudgpu_acceleration_structure_build top_build =
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD, &top_inputs, NULL, r.top, r.scratch, r.scratch_top);
		CHECK(spudgpu_cmd_build_acceleration_structures(r.cmd, &top_build, 1) == SPUD_SUCCESS);
		rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
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
			rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			rq_record_trace(&r, r.cmd, masks[m]);
			rq_submit(&r);
			rq_check(&r, triangle_at, box_at, masks[m], m == 0 ? "cull mask 0x01, the triangle alone" : "cull mask 0x02, the sphere alone");
		}

		// The triangle moved up and further, and the top level refitted in
		// place: an update from itself.
		const float moved_at[3] = {0.0f, 1.0f, 3.0f};
		const spudgpu_acceleration_structure_instance moved[2] = {
		    rq_instance(moved_at[0], moved_at[1], moved_at[2], RQ_TRIANGLE_ID, 0x01, references[0]), instances[1]};
		rq_write_instances(&r, moved, 2, 0);
		rq_begin(&r);
		// Updates that are refused: of a bottom level built without
		// ALLOW_UPDATE, and of the top level with other flags.
		spudgpu_acceleration_structure_build refused =
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &box_inputs, r.box, r.box, r.scratch, r.scratch_bottom[1]);
		CHECK(spudgpu_cmd_build_acceleration_structures(r.cmd, &refused, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
		spudgpu_acceleration_structure_inputs other_flags = top_inputs;
		other_flags.flags |= SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
		refused = rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &other_flags, r.top, r.top, r.scratch, r.scratch_update);
		CHECK(spudgpu_cmd_build_acceleration_structures(r.cmd, &refused, 1) == SPUDRESULT_GPU_ACCELERATION_STRUCTURE_UPDATE_REFUSED);
		b = rq_buffer_barrier(r.scratch, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
		spudgpu_cmd_pipeline_barrier(r.cmd, &b, 1, NULL, 0);
		for (int i = 0; i < 2; ++i) {
			ab[i].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
			ab[i].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		}
		spudgpu_cmd_acceleration_structure_barrier(r.cmd, ab, 2);
		// In place: the top level is read and written by its update.
		rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		spudgpu_acceleration_structure_build refit =
		    rq_build(SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE, &top_inputs, r.top, r.top, r.scratch, r.scratch_update);
		CHECK(spudgpu_cmd_build_acceleration_structures(r.cmd, &refit, 1) == SPUD_SUCCESS);
		rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		rq_record_trace(&r, r.cmd, 0xFF);
		rq_submit(&r);
		rq_check(&r, moved_at, box_at, 0xFF, "the triangle moved, the top level refitted");

		// A clone of the top level, traced through.
		spudgpu_acceleration_structure_desc clone_desc =
		    rq_desc(SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, r.top_sizes.storage_size, 2, false, "top clone");
		spudgpu_acceleration_structure clone = NULL;
		CHECK(spudgpu_create_acceleration_structure(r.device, &clone_desc, &clone) == SPUD_SUCCESS);
		if (clone) {
			rq_begin(&r);
			rq_barrier(r.cmd, r.top, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
			rq_barrier(r.cmd, clone, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
			CHECK(spudgpu_cmd_copy_acceleration_structure(r.cmd, r.top, clone, SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) == SPUD_SUCCESS);
			rq_barrier(r.cmd, clone, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			spudgpu_end_command_list(r.cmd);
			CHECK(spudgpu_submit_command_lists(r.queue, &r.cmd, 1) == SPUD_SUCCESS);
			spudgpu_queue_wait_idle(r.queue);
			rq_write_set(&r, clone);
			rq_begin(&r);
			rq_barrier(r.cmd, clone, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
			rq_record_trace(&r, r.cmd, 0xFF);
			rq_submit(&r);
			rq_check(&r, moved_at, box_at, 0xFF, "a clone of the refitted top level");
			rq_write_set(&r, r.top);
			spudgpu_destroy_acceleration_structure(clone);
		}

		// Compaction, the top level rebuilt over the compacted triangle.
		spudgpu_acceleration_structure compact = rq_test_compaction(&r, &top_inputs, bottoms, references[1], triangle_at, box_at);
		// The GPU writer, over the compacted triangle when there is one.
		uint64_t gpu_references[2] = {references[0], references[1]};
		if (compact)
			CHECK(spudgpu_get_acceleration_structure_reference(compact, &gpu_references[0]) == SPUD_SUCCESS);
		rq_test_gpu_writer(&r, &top_inputs, gpu_references, triangle_at, box_at);
		rq_test_shadow(&r, triangle_at);
		// The top level names the compacted triangle: it goes first.
		spudgpu_destroy_acceleration_structure(r.top);
		r.top = NULL;
		spudgpu_destroy_acceleration_structure(compact);
	}

done:
	spudgpu_destroy_descriptor_pool(r.pool);
	spudgpu_destroy_compute_pipeline(r.trace);
	spudgpu_destroy_descriptor_set_layout(r.set_layout);
	spudgpu_destroy_shader_module(r.trace_module);
	spudgpu_destroy_acceleration_structure(r.top);
	spudgpu_destroy_acceleration_structure(r.box);
	spudgpu_destroy_acceleration_structure(r.triangle);
	spudgpu_destroy_buffer(r.scratch);
	spudgpu_destroy_buffer(r.hits_readback);
	spudgpu_destroy_buffer(r.hits);
	spudgpu_destroy_buffer(r.instances);
	spudgpu_destroy_buffer(r.boxes);
	spudgpu_destroy_buffer(r.vertices);
	spudgpu_destroy_command_list(r.cmd);
	spudgpu_destroy_command_allocator(r.allocator);
	spudgpu_destroy_device(r.device);
}

#endif // SPUDGPU_EXT_RAY_QUERY

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
	window_class.lpszClassName = L"spudgpu_objects_test";
	RegisterClassExW(&window_class);
	RECT rect = {0, 0, 64, 64};
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	return CreateWindowExW(0, L"spudgpu_objects_test", L"spudgpu_objects_test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
	                       rect.bottom - rect.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void hidden_window_destroy(HWND window) {
	DestroyWindow(window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"spudgpu_objects_test", GetModuleHandleW(NULL));
}
#endif

static bool callback_refuses(void *vk_instance, void *user_data, void *out_surface) {
	(void)vk_instance;
	(void)out_surface;
	++*(int *)user_data;
	return false;
}

#if SPUDGPU_COMPILE_VULKAN_API && defined(_WIN32)
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
// with spudgpu_submit_command_lists_synced() and presented.
static void present_frames(spudgpu_device device, spudgpu_surface surface, const char *what) {
	spudgpu_swap_chain_desc desc = swap_chain_desc_for(device, surface, SPUDGPU_PRESENT_SCALING_ONE_TO_ONE);
#if !SPUDGPU_COMPILE_METAL_API
	desc.buffer_count = 2;
#endif
	spudgpu_swap_chain swap_chain = NULL;
	CHECK(spudgpu_create_swap_chain(device, &desc, &swap_chain) == SPUD_SUCCESS);
	if (!swap_chain)
		return;
	spudgpu_swap_chain_desc created;
	memset(&created, 0, sizeof(created));
	created.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	CHECK(spudgpu_get_swap_chain_desc(swap_chain, &created) == SPUD_SUCCESS);

	spudgpu_semaphore available = spudgpu_swap_chain_get_image_available_semaphore(swap_chain);
	spudgpu_semaphore finished  = spudgpu_swap_chain_get_render_finished_semaphore(swap_chain);
	CHECK(spudgpu_swap_chain_get_image_available_semaphore(NULL) == NULL);
	CHECK(spudgpu_swap_chain_get_render_finished_semaphore(NULL) == NULL);
#if SPUDGPU_COMPILE_D3D12_API
	// Not implemented by D3D12, which synchronizes a present by a fence of
	// its own: NULL, as spudgpu.h says.
	CHECK(available == NULL && finished == NULL);
#elif SPUDGPU_COMPILE_VULKAN_API
	CHECK(available != NULL && finished != NULL && available != finished);
	CHECK(spudgpu_swap_chain_get_image_available_semaphore(swap_chain) == available);
	CHECK(spudgpu_swap_chain_get_render_finished_semaphore(swap_chain) == finished);
#else
	(void)available;
	(void)finished;
#endif

	spudgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type            = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type                 = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	spudgpu_command_allocator allocator = NULL;
	CHECK(spudgpu_create_command_allocator(device, &allocator_desc, &allocator) == SPUD_SUCCESS);
	spudgpu_command_list cmd = NULL;
	if (allocator)
		CHECK(spudgpu_create_command_list(allocator, &cmd) == SPUD_SUCCESS);
	CHECK(spudgpu_get_swap_chain_image_view(NULL, 0) == NULL);
	CHECK(spudgpu_get_swap_chain_image_view(swap_chain, created.buffer_count) == NULL);
	int presented = 0;
	for (int frame = 0; cmd && frame < 3; ++frame) {
		const uint32_t index = spudgpu_swap_chain_acquire_next_image(swap_chain);
		CHECK(index < created.buffer_count);
		spudgpu_image_view view = spudgpu_get_swap_chain_image_view(swap_chain, index);
		CHECK(view != NULL);
		if (!view)
			break;
		CHECK(spudgpu_reset_command_allocator(allocator) == SPUD_SUCCESS);
		spudgpu_begin_command_list(cmd);
		spudgpu_cmd_image_barrier_view(cmd, view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		spudgpu_rendering_begin_desc pass;
		memset(&pass, 0, sizeof(pass));
		pass.base.type                       = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
		pass.color_attachments[0].image_view = view;
		pass.color_attachments[0].load_op    = SPUDGPU_LOAD_OP_CLEAR;
		pass.color_attachments[0].store_op   = SPUDGPU_STORE_OP_STORE;
		pass.color_attachment_count          = 1;
		pass.width                           = created.width;
		pass.height                          = created.height;
		spudgpu_cmd_begin_rendering(cmd, &pass);
		spudgpu_cmd_end_rendering(cmd);
		spudgpu_cmd_image_barrier_view(cmd, view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC);
		spudgpu_end_command_list(cmd);
		CHECK(spudgpu_submit_command_lists_synced(NULL, &cmd, 1, swap_chain) == SPUDRESULT_GPU_INVALID_COMMAND_QUEUE);
		CHECK(spudgpu_submit_command_lists_synced(desc.queue, &cmd, 1, NULL) == SPUDRESULT_GPU_INVALID_SWAP_CHAIN);
		CHECK(spudgpu_submit_command_lists_synced(desc.queue, &cmd, 1, swap_chain) == SPUD_SUCCESS);
		spudgpu_swap_chain_present(swap_chain);
		++presented;
	}
	CHECK(presented == 3);
	printf("spudgpu_objects_test: %d frames presented to %s\n", presented, what);
	spudgpu_queue_wait_idle(desc.queue);
	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
	spudgpu_destroy_swap_chain(swap_chain);
}

// A swap chain's frames on a real surface, where this test can make one
// without showing anything; and spudgpu_create_surface_from_callback().
static void test_swap_chain_frames(spudgpu_instance instance, spudgpu_device device) {
	int refusals = 0;
	CHECK(spudgpu_create_surface_from_callback(NULL, &refusals, callback_refuses) == NULL);
	CHECK(spudgpu_create_surface_from_callback(instance, &refusals, NULL) == NULL);
	CHECK(spudgpu_create_surface_from_callback(instance, &refusals, callback_refuses) == NULL);
#if SPUDGPU_COMPILE_VULKAN_API
	CHECK(refusals == 1); // called once, and its refusal is the call's
#else
	CHECK(refusals == 0); // not implemented: the callback is never called
#endif
#if defined(_WIN32)
	HWND window = hidden_window_create();
	CHECK(window != NULL);
	if (!window)
		return;
	spudgpu_surface surface = NULL;
	CHECK(spudgpu_create_surface(instance, window, NULL, &surface) == SPUD_SUCCESS);
	if (surface) {
		present_frames(device, surface, "a hidden window");
		spudgpu_destroy_surface(surface);
	}
#if SPUDGPU_COMPILE_VULKAN_API
	callback_window w             = {window, 0, NULL};
	spudgpu_surface from_callback = spudgpu_create_surface_from_callback(instance, &w, callback_makes_win32_surface);
	CHECK(from_callback != NULL);
	CHECK(w.calls == 1 && w.instance_seen != NULL);
	if (from_callback) {
		present_frames(device, from_callback, "a surface the callback made");
		spudgpu_destroy_surface(from_callback);
	}
#endif
	hidden_window_destroy(window);
#elif SPUDGPU_COMPILE_METAL_API
	void *layer             = spudgpu_test_metal_layer_create(false);
	spudgpu_surface surface = NULL;
	CHECK(spudgpu_create_surface(instance, layer, NULL, &surface) == SPUD_SUCCESS);
	if (surface) {
		present_frames(device, surface, "a CAMetalLayer");
		spudgpu_destroy_surface(surface);
	}
	spudgpu_test_metal_layer_destroy(layer);
#else
	(void)device;
	printf("spudgpu_objects_test: no surface without a display here, no swap chain frames\n");
#endif
}

static void test_queries(spudgpu_instance instance) {
	CHECK(spudgpu_get_native_gpu_api(NULL) == SPUDGPU_NATIVE_API_NONE);
#if SPUDGPU_COMPILE_VULKAN_API
	CHECK(spudgpu_get_native_gpu_api(instance) == SPUDGPU_NATIVE_API_VULKAN);
#elif SPUDGPU_COMPILE_D3D12_API
	CHECK(spudgpu_get_native_gpu_api(instance) == SPUDGPU_NATIVE_API_D3D12);
#elif SPUDGPU_COMPILE_METAL_API
	CHECK(spudgpu_get_native_gpu_api(instance) == SPUDGPU_NATIVE_API_METAL);
#endif
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_UNKNOWN) == 0);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_R8G8B8A8_UNORM) == 32);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_B8G8R8A8_UNORM) == 32);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_R32G32_FLOAT) == 64);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_R32G32B32A32_FLOAT) == 128);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_R16_UINT) == 16);
	CHECK(spudgpu_format_bit_count(SPUDGPU_FORMAT_D32_FLOAT) == 32);
}

int main(int argc, char **argv) {
	uint32_t adapter_index = 0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
			adapter_index = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: spudgpu_objects_test [--adapter N]\n");
			return 2;
		}
	}
	test_format_from_string();

	spudtest_allocator tracker;
	SPUD_ALLOCATOR allocator = spudtest_allocator_make(&tracker);
	spudgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "spudgpu_objects_test";
	instance_desc.engine_name      = "spudgpu_objects_test";
	spudgpu_instance instance      = NULL;
	if (spudgpu_create_instance(&instance_desc, &instance) != SPUD_SUCCESS) {
		fprintf(stderr, "spudgpu_objects_test: no instance\n");
		return 1;
	}
	const spudgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	spudgpu_enumerate_adapters(instance, &adapters, &adapter_count);
	if (adapter_count == 0) {
		printf("spudgpu_objects_test: no adapter, skipped\n");
		spudgpu_destroy_instance(instance);
		return SKIPPED;
	}
	if (adapter_index >= adapter_count) {
		fprintf(stderr, "spudgpu_objects_test: no adapter %u (%u listed)\n", adapter_index, adapter_count);
		spudgpu_destroy_instance(instance);
		return 2;
	}
	const spudgpu_adapter adapter = adapters[adapter_index];
	test_queries(instance);

	spudgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "spudgpu_objects_test";
	spudgpu_device device          = NULL;
	SPUDGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(spudgpu_get_adapter_properties(adapter, &properties) == SPUD_SUCCESS);
	printf("spudgpu_objects_test: adapter %u: %s\n", adapter_index, properties.description);
	// Exactly one format, the backend's own.
	test_shader_format = spudgpu_test_shader_format(properties.shader_formats);
	CHECK(test_shader_format != SPUDGPU_SHADER_FORMAT_NONE);
	CHECK(properties.shader_formats == (1u << test_shader_format));

	CHECK(spudgpu_create_device(adapter, &device_desc, &device) == SPUD_SUCCESS);
	if (device) {
		test_shader_module_checks(device);
		test_geometry_tessellation_refusals(device, properties.supported_features);
		test_fence_signal(device);
		test_semaphore(device);
		test_buffer_and_view(device);
		test_heap_desc(device);
		test_descriptor_pool_reset(device);
		test_present_scaling_without_present(device);
#if SPUDGPU_EXT_RAY_QUERY
		test_ray_query_without_feature(device);
#endif
		spudgpu_destroy_device(device);
	}

	test_drawing(adapter, properties.supported_features);
#if SPUDGPU_EXT_RAY_QUERY
	test_ray_queries(adapter, properties.supported_features);
#endif
	if (properties.supported_features & SPUDGPU_FEATURE_PRESENT) {
		device_desc.required_features = SPUDGPU_FEATURE_PRESENT;
		device_desc.debug_name        = "spudgpu_objects_test present";
		spudgpu_device present_device = NULL;
		CHECK(spudgpu_create_device(adapter, &device_desc, &present_device) == SPUD_SUCCESS);
		if (present_device) {
			test_present_scaling_checks(present_device);
#if SPUDGPU_COMPILE_METAL_API
			test_present_scaling_metal(instance, present_device);
#endif
			test_swap_chain_frames(instance, present_device);
			spudgpu_destroy_device(present_device);
		}
	} else {
		printf("spudgpu_objects_test: the adapter can't present, present scaling checks and swap chain frames skipped\n");
	}
	spudgpu_destroy_instance(instance);
	CHECK(spudtest_allocator_clean(&tracker));

	if (failures) {
		fprintf(stderr, "spudgpu_objects_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("spudgpu_objects_test: all passed\n");
	return 0;
}
