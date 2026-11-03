/*
 * spud_noalloc_tests — no allocation per frame (ROADMAP.md in the workspace,
 * section 3.1, "Data-oriented checks").
 *
 * Each module's hot path, warmed up, then counted for 100 iterations with
 * every heap allocation the counting thread makes, through tests/alloctrace.h,
 * which says what each platform's hook sees and how each allocation is put
 * down to the code that asked for it. Only ours (this executable's, where
 * SpudLib is linked) must be none; what a driver, the graphics runtime or the
 * system asks for is reported apart and not counted. Where no hook is
 * available the test says so and passes nothing off as checked.
 *
 * The hot paths:
 * - SpudGPU recording and submitting a frame offscreen on a real device:
 *   reset the allocator, begin, a barrier, a pass that clears an image, a
 *   barrier out, end, spudgpu_queue_submit() signalling a fence, and a wait
 *   for the frame before. On every platform with an adapter.
 * - SpudGPU's submit and present loop: the same frame drawn into a swap
 *   chain's image, acquired, submitted with
 *   spudgpu_submit_command_lists_synced() and presented. Windows presents to
 *   a hidden window, macOS to a CAMetalLayer with no window; Linux has no
 *   surface here without a display, so it is skipped there.
 * - SpudAudio's stream thread (the platform's own on Core Audio and
 *   RemoteIO): an output stream on the general default
 *   device writing silence, counted on the callback's own thread from its
 *   Nth call to its (N+100)th, everything SpudAudio does on that thread
 *   between the calls included. Skipped with no output device.
 * - SpudNet with a loopback connection open and idle: a receive with
 *   SPUDNET_NO_WAIT on each end and a wait set's poll, per round.
 * - SpudInput's poll, on the platform's native API, gamepads or none.
 * - SpudGPU's ray queries (SPUDGPU_EXT_RAY_QUERY), on an adapter with them:
 *   a bottom level and a top level built once, then per frame the instance
 *   written by spudgpu_write_acceleration_structure_instances() into a
 *   mapped buffer, the top level refitted in place and rq_trace.slang
 *   tracing a 64 x 64 grid. Creating each structure's bytes from the
 *   caller's allocator are printed.
 *
 * Each scenario also times its iterations, uncounted, and prints the best
 * of a few runs: what a change to its path costs or saves.
 *
 *   spud_noalloc_tests [--adapter N]
 *
 * --adapter picks the GPU, by its index in spudgpu_enumerate_adapters(); 0 by
 * default. Exits 0 when every check passes, 1 when one fails.
 */

#include <spudaudio.h>
#include <spudcore.h>
#include <spudgpu.h>
#include <spudinput.h>
#include <spudnet.h>
#include <spudperf.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloctrace.h"
#include "spudgpu_test_shaders.h"

#if SPUDGPU_COMPILE_METAL_API
#include "spudgpu_test_metal_layer.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

#define ITERATIONS 100
#define WARM_UP 8
/* The timed runs: how many, and iterations in each. */
#define TIMED_RUNS 5
#define TIMED_ITERATIONS 200

static void sleep_ms(uint32_t ms) {
#if defined(_WIN32)
	Sleep(ms);
#else
	struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
#endif
}

/* Ours must be none; what other modules asked for is reported, not counted. */
static void report(const char *what) {
	char line[256];
	snprintf(line, sizeof(line), "spud_noalloc_tests: %s, %d iterations", what, ITERATIONS);
	alloc_trace_report(line, 6);
	if (alloc_trace_ours() != 0)
		++failures;
}

static void print_time(const char *what, uint64_t best_ns, uint32_t iterations) {
	printf("spud_noalloc_tests: time: %s: %.2f us an iteration (best of %d runs of %u)\n", what,
	       (double)best_ns / (double)iterations / 1000.0, TIMED_RUNS, iterations);
}

/* The hook counts at all: one malloc and one calloc of this thread, counted
 * as two of ours. A zero everywhere else means nothing if this is zero too. */
static void test_hook_counts(void) {
	alloc_trace_start();
	void *volatile a = malloc(24);
	void *volatile b = calloc(3, 8);
	alloc_trace_stop();
	free(a);
	free(b);
	printf("spud_noalloc_tests: the hook counted %lu of 2 known allocations of ours\n", alloc_trace_ours());
	CHECK(alloc_trace_ours() == 2);
}

// ---- SpudGPU ---------------------------------------------------------------

#define TARGET_SIZE 256

typedef struct gpu_frame {
	spudgpu_device device;
	spudgpu_command_queue queue;
	spudgpu_command_allocator allocator;
	spudgpu_command_list cmd;
} gpu_frame;

/* The commands of one frame: the target into a colour attachment, a pass
 * that clears it, and the target out to [after]. */
static void record_frame(spudgpu_command_list cmd, spudgpu_image_view view, uint32_t width, uint32_t height, SPUDGPU_IMAGE_LAYOUT before,
                         SPUDGPU_IMAGE_LAYOUT after, uint64_t frame) {
	spudgpu_begin_command_list(cmd);
	spudgpu_cmd_image_barrier_view(cmd, view, before, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	spudgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                       = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = view;
	pass.color_attachments[0].load_op    = SPUDGPU_LOAD_OP_CLEAR;
	pass.color_attachments[0].store_op   = SPUDGPU_STORE_OP_STORE;
	pass.color_attachments[0].clear_color[0] = (float)(frame % 16) / 16.0f;
	pass.color_attachments[0].clear_color[3] = 1.0f;
	pass.color_attachment_count          = 1;
	pass.width                           = width;
	pass.height                          = height;
	spudgpu_cmd_begin_rendering(cmd, &pass);
	spudgpu_cmd_end_rendering(cmd);
	spudgpu_cmd_image_barrier_view(cmd, view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, after);
	spudgpu_end_command_list(cmd);
}

/* Offscreen: one frame, submitted with a fence signalled to [frame]; the
 * frame before waited for first, so the one allocator can be reset. */
typedef struct offscreen {
	gpu_frame f;
	spudgpu_image image;
	spudgpu_image_view view;
	spudgpu_fence fence;
	uint64_t frame;
	/* Of the timed runs: the time in spudgpu_queue_submit() alone. */
	uint64_t submit_ns;
} offscreen;

static bool offscreen_frame(offscreen *o) {
	if (o->frame > 0) {
		uint64_t value = o->frame;
		if (spudgpu_wait_for_fences(o->f.device, &o->fence, &value, 1, true, UINT64_MAX) != SPUD_SUCCESS)
			return false;
	}
	if (spudgpu_reset_command_allocator(o->f.allocator) != SPUD_SUCCESS)
		return false;
	record_frame(o->f.cmd, o->view, TARGET_SIZE, TARGET_SIZE,
	             o->frame == 0 ? SPUDGPU_IMAGE_LAYOUT_UNDEFINED : SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY,
	             SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, o->frame);
	++o->frame;
	spudgpu_submit_desc submit;
	memset(&submit, 0, sizeof(submit));
	submit.base.type          = SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = &o->f.cmd;
	submit.cmd_list_count     = 1;
	submit.signal_fence       = o->fence;
	submit.signal_fence_value = o->frame;
	const uint64_t start       = spudperf_get_monotonic_time_ns();
	const SPUDRESULT submitted = spudgpu_queue_submit(o->f.queue, &submit);
	o->submit_ns += spudperf_get_monotonic_time_ns() - start;
	return submitted == SPUD_SUCCESS;
}

static void test_gpu_offscreen(gpu_frame f) {
	offscreen o;
	memset(&o, 0, sizeof(o));
	o.f = f;
	spudgpu_image_desc image_desc;
	memset(&image_desc, 0, sizeof(image_desc));
	image_desc.base.type    = SPUD_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage        = SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_SAMPLED;
	image_desc.type         = SPUDGPU_IMAGE_TYPE_2D;
	image_desc.format       = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.format = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.width        = TARGET_SIZE;
	image_desc.height       = TARGET_SIZE;
	image_desc.depth        = 1;
	image_desc.array_layers = 1;
	image_desc.mip_levels   = 1;
	image_desc.debug_name   = "noalloc target";
	CHECK(spudgpu_create_image(f.device, &image_desc, &o.image) == SPUD_SUCCESS);
	spudgpu_image_view_desc view_desc;
	memset(&view_desc, 0, sizeof(view_desc));
	view_desc.base.type                           = SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	view_desc.parent_image                        = o.image;
	view_desc.type                                = SPUDGPU_IMAGE_VIEW_TYPE_2D;
	view_desc.subresource_range.aspect_mask       = SPUDGPU_IMAGE_ASPECT_COLOR;
	view_desc.subresource_range.mip_level_count   = 1;
	view_desc.subresource_range.array_layer_count = 1;
	if (o.image)
		CHECK(spudgpu_create_image_view(o.image, &view_desc, &o.view) == SPUD_SUCCESS);
	CHECK(spudgpu_create_fence(f.device, SPUDGPU_FENCE_FLAG_NONE, 0, &o.fence) == SPUD_SUCCESS);
	if (o.view && o.fence) {
		bool ok = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = offscreen_frame(&o);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = offscreen_frame(&o);
		alloc_trace_stop();
		CHECK(ok);
		report("SpudGPU recording and submitting a frame offscreen, a fence waited on");

		uint64_t best = UINT64_MAX, best_submit = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			o.submit_ns          = 0;
			const uint64_t start = spudperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = offscreen_frame(&o);
			const uint64_t took = spudperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
			if (o.submit_ns < best_submit)
				best_submit = o.submit_ns;
		}
		if (ok) {
			print_time("SpudGPU offscreen frame (recording, submit, fence wait)", best, TIMED_ITERATIONS);
			print_time("SpudGPU offscreen frame, spudgpu_queue_submit() alone", best_submit, TIMED_ITERATIONS);
		}
		uint64_t value = o.frame;
		spudgpu_wait_for_fences(f.device, &o.fence, &value, 1, true, UINT64_MAX);
	}
	spudgpu_destroy_fence(o.fence);
	spudgpu_destroy_image_view(o.view);
	spudgpu_destroy_image(o.image);
}

#if defined(_WIN32) || SPUDGPU_COMPILE_METAL_API
#define NOALLOC_PRESENT 1

typedef struct presenting {
	gpu_frame f;
	spudgpu_swap_chain swap_chain;
	uint32_t width, height;
	uint64_t frame;
	/* Of the timed runs: recording and the synced submit only, no acquire
	 * and no present, which wait; and the synced submit alone. */
	uint64_t recording_and_submit_ns;
	uint64_t submit_ns;
} presenting;

static bool present_frame(presenting *p) {
	const uint32_t index    = spudgpu_swap_chain_acquire_next_image(p->swap_chain);
	spudgpu_image_view view = spudgpu_get_swap_chain_image_view(p->swap_chain, index);
	if (!view)
		return false;
	const uint64_t start = spudperf_get_monotonic_time_ns();
	if (spudgpu_reset_command_allocator(p->f.allocator) != SPUD_SUCCESS)
		return false;
	record_frame(p->f.cmd, view, p->width, p->height, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC, p->frame++);
	const uint64_t submit_start = spudperf_get_monotonic_time_ns();
	const SPUDRESULT submitted  = spudgpu_submit_command_lists_synced(p->f.queue, &p->f.cmd, 1, p->swap_chain);
	const uint64_t end          = spudperf_get_monotonic_time_ns();
	p->recording_and_submit_ns += end - start;
	p->submit_ns += end - submit_start;
	if (submitted != SPUD_SUCCESS)
		return false;
	spudgpu_swap_chain_present(p->swap_chain);
	return true;
}

static void test_gpu_present(spudgpu_instance instance, gpu_frame f, void *window) {
	presenting p;
	memset(&p, 0, sizeof(p));
	p.f                     = f;
	spudgpu_surface surface = NULL;
	CHECK(spudgpu_create_surface(instance, window, NULL, &surface) == SPUD_SUCCESS);
	if (!surface)
		return;
	spudgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	desc.queue     = f.queue;
	desc.width     = TARGET_SIZE;
	desc.height    = TARGET_SIZE;
#if SPUDGPU_COMPILE_METAL_API
	desc.buffer_count = 1;
#else
	desc.buffer_count = 2;
#endif
	desc.format          = SPUDGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode    = SPUDGPU_PRESENT_MODE_FIFO;
	desc.present_scaling = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE;
	desc.fullscreen_mode = SPUDGPU_FULLSCREEN_MODE_WINDOWED;
	CHECK(spudgpu_create_swap_chain(f.device, &desc, &p.swap_chain) == SPUD_SUCCESS);
	if (p.swap_chain) {
		spudgpu_swap_chain_desc created;
		memset(&created, 0, sizeof(created));
		created.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
		CHECK(spudgpu_get_swap_chain_desc(p.swap_chain, &created) == SPUD_SUCCESS);
		p.width  = created.width;
		p.height = created.height;
		bool ok  = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = present_frame(&p);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = present_frame(&p);
		alloc_trace_stop();
		CHECK(ok);
		report("SpudGPU's frame into a swap chain: acquire, record, synced submit, present");

		uint64_t best = UINT64_MAX, best_recording = UINT64_MAX, best_submit = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			p.recording_and_submit_ns = 0;
			p.submit_ns               = 0;
			const uint64_t start      = spudperf_get_monotonic_time_ns();
			for (int i = 0; i < ITERATIONS && ok; ++i)
				ok = present_frame(&p);
			const uint64_t took = spudperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
			if (p.recording_and_submit_ns < best_recording)
				best_recording = p.recording_and_submit_ns;
			if (p.submit_ns < best_submit)
				best_submit = p.submit_ns;
		}
		if (ok) {
			print_time("SpudGPU present frame, whole (FIFO: paced by the display)", best, ITERATIONS);
			print_time("SpudGPU present frame, reset + recording + synced submit only", best_recording, ITERATIONS);
			print_time("SpudGPU present frame, spudgpu_submit_command_lists_synced() alone", best_submit, ITERATIONS);
		}
		spudgpu_queue_wait_idle(f.queue);
		spudgpu_destroy_swap_chain(p.swap_chain);
	}
	spudgpu_destroy_surface(surface);
}
#else
#define NOALLOC_PRESENT 0
#endif

#if defined(_WIN32)
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
	return DefWindowProcW(window, message, w, l);
}

/* A window that is never shown: a surface needs one, nobody needs to see it. */
static HWND hidden_window_create(void) {
	WNDCLASSEXW window_class;
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize        = sizeof(window_class);
	window_class.lpfnWndProc   = window_proc;
	window_class.hInstance     = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"spud_noalloc_tests";
	RegisterClassExW(&window_class);
	RECT rect = {0, 0, TARGET_SIZE, TARGET_SIZE};
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	return CreateWindowExW(0, L"spud_noalloc_tests", L"spud_noalloc_tests", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                       rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void hidden_window_destroy(HWND window) {
	DestroyWindow(window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"spud_noalloc_tests", GetModuleHandleW(NULL));
}
#endif

#if SPUDGPU_EXT_RAY_QUERY
static void test_gpu_ray_query(uint32_t adapter_index);
#endif

static void test_gpu(uint32_t adapter_index) {
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	spudgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "spud_noalloc_tests";
	instance_desc.engine_name      = "spud_noalloc_tests";
	spudgpu_instance instance      = NULL;
	if (spudgpu_create_instance(&instance_desc, &instance) != SPUD_SUCCESS) {
		printf("spud_noalloc_tests: SpudGPU: no instance, skipped\n");
		return;
	}
	const spudgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	spudgpu_enumerate_adapters(instance, &adapters, &adapter_count);
	if (adapter_index >= adapter_count) {
		printf("spud_noalloc_tests: SpudGPU: no adapter %u (%u listed), skipped\n", adapter_index, adapter_count);
		spudgpu_destroy_instance(instance);
		return;
	}
	SPUDGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(spudgpu_get_adapter_properties(adapters[adapter_index], &properties) == SPUD_SUCCESS);
	printf("spud_noalloc_tests: SpudGPU on adapter %u: %s\n", adapter_index, properties.description);

	const bool can_present = NOALLOC_PRESENT && (properties.supported_features & SPUDGPU_FEATURE_PRESENT) != 0;
	spudgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.required_features  = can_present ? SPUDGPU_FEATURE_PRESENT : 0;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "spud_noalloc_tests";
	gpu_frame f;
	memset(&f, 0, sizeof(f));
	CHECK(spudgpu_create_device(adapters[adapter_index], &device_desc, &f.device) == SPUD_SUCCESS);
	if (f.device) {
		CHECK(spudgpu_get_command_queue(f.device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &f.queue) == SPUD_SUCCESS);
		spudgpu_command_allocator_desc allocator_desc;
		memset(&allocator_desc, 0, sizeof(allocator_desc));
		allocator_desc.base.type = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
		allocator_desc.type      = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
		CHECK(spudgpu_create_command_allocator(f.device, &allocator_desc, &f.allocator) == SPUD_SUCCESS);
		if (f.allocator)
			CHECK(spudgpu_create_command_list(f.allocator, &f.cmd) == SPUD_SUCCESS);
		if (f.queue && f.cmd) {
			test_gpu_offscreen(f);
#if NOALLOC_PRESENT
			if (!can_present) {
				printf("spud_noalloc_tests: SpudGPU: the adapter can't present, the present loop skipped\n");
			} else {
#if defined(_WIN32)
				HWND window = hidden_window_create();
				CHECK(window != NULL);
				if (window) {
					test_gpu_present(instance, f, window);
					hidden_window_destroy(window);
				}
#else
				void *layer = spudgpu_test_metal_layer_create(false);
				test_gpu_present(instance, f, layer);
				spudgpu_test_metal_layer_destroy(layer);
#endif
			}
#else
			printf("spud_noalloc_tests: SpudGPU: no surface without a display here, the present loop skipped\n");
#endif
			spudgpu_queue_wait_idle(f.queue);
		}
		spudgpu_destroy_command_list(f.cmd);
		spudgpu_destroy_command_allocator(f.allocator);
		spudgpu_destroy_device(f.device);
	}
	spudgpu_destroy_instance(instance);
#if SPUDGPU_EXT_RAY_QUERY
	test_gpu_ray_query(adapter_index);
#endif
}

#if SPUDGPU_EXT_RAY_QUERY
// ---- SpudGPU ray queries ---------------------------------------------------

/* What SpudLib takes from its allocator, counted: the bytes a structure's
 * creation allocates. */
typedef struct counting {
	SPUD_ALLOCATOR system;
	size_t blocks;
	size_t bytes;
} counting;

static void *counting_allocate(void *user_data, size_t size, size_t alignment) {
	counting *c = (counting *)user_data;
	++c->blocks;
	c->bytes += size;
	return c->system.allocate(c->system.user_data, size, alignment);
}

static void counting_free(void *user_data, void *memory, size_t size, size_t alignment) {
	counting *c = (counting *)user_data;
	c->system.free(c->system.user_data, memory, size, alignment);
}

#define RQ_GRID 64u

/* rq_trace.slang's RqParams. */
typedef struct rq_params {
	float origin[4];
	float step_and_tmax[4];
	uint32_t width, height, cull_mask, ray_flags;
} rq_params;

typedef struct rq_frame {
	gpu_frame f;
	spudgpu_fence fence;
	uint64_t frame;
	spudgpu_buffer instances, scratch, hits;
	uint64_t top_scratch_offset;
	void *mapped_instances;
	uint32_t instance_size;
	spudgpu_acceleration_structure bottom, top;
	uint64_t bottom_reference;
	spudgpu_acceleration_structure_inputs top_inputs;
	spudgpu_compute_pipeline trace;
	spudgpu_descriptor_set set;
} rq_frame;

/* One frame: the instance moved and written by the CPU call into the mapped
 * instance buffer, the top level refitted in place, and the grid traced. */
static bool rq_frame_run(rq_frame *r) {
	if (r->frame > 0) {
		uint64_t value = r->frame;
		if (spudgpu_wait_for_fences(r->f.device, &r->fence, &value, 1, true, UINT64_MAX) != SPUD_SUCCESS)
			return false;
	}
	spudgpu_acceleration_structure_instance instance;
	memset(&instance, 0, sizeof(instance));
	instance.transform[0]           = 1.0f;
	instance.transform[3]           = (float)(r->frame % 8) * 0.125f;
	instance.transform[5]           = 1.0f;
	instance.transform[10]          = 1.0f;
	instance.transform[11]          = 2.0f;
	instance.user_index             = 7;
	instance.mask                   = 0xFF;
	instance.bottom_level_reference = r->bottom_reference;
	if (spudgpu_write_acceleration_structure_instances(r->f.device, &instance, 1, r->mapped_instances) != SPUD_SUCCESS)
		return false;
	if (spudgpu_flush_buffer(r->instances, 0, r->instance_size) != SPUD_SUCCESS)
		return false;
	if (spudgpu_reset_command_allocator(r->f.allocator) != SPUD_SUCCESS)
		return false;
	spudgpu_command_list cmd = r->f.cmd;
	spudgpu_begin_command_list(cmd);
	spudgpu_buffer_barrier b[2];
	memset(b, 0, sizeof(b));
	b[0].base.type    = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	b[0].buffer       = r->scratch;
	b[0].state_before = SPUDGPU_RESOURCE_STATE_COMMON;
	b[0].state_after  = SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH;
	b[1]              = b[0];
	b[1].buffer       = r->hits;
	b[1].state_after  = SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	spudgpu_cmd_pipeline_barrier(cmd, b, 2, NULL, 0);
	spudgpu_acceleration_structure_barrier ab[2];
	memset(ab, 0, sizeof(ab));
	ab[0].base.type     = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
	ab[0].structure     = r->bottom;
	ab[0].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
	ab[0].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
	ab[1]               = ab[0];
	ab[1].structure     = r->top;
	spudgpu_cmd_acceleration_structure_barrier(cmd, ab, 2);
	spudgpu_acceleration_structure_build build;
	memset(&build, 0, sizeof(build));
	build.base.type   = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
	build.mode        = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE;
	build.inputs      = &r->top_inputs;
	build.source      = r->top;
	build.destination = r->top;
	build.scratch     = r->scratch;
	build.scratch_offset = r->top_scratch_offset;
	if (spudgpu_cmd_build_acceleration_structures(cmd, &build, 1) != SPUD_SUCCESS) {
		spudgpu_end_command_list(cmd);
		return false;
	}
	ab[1].access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
	ab[1].access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	spudgpu_cmd_acceleration_structure_barrier(cmd, &ab[1], 1);
	spudgpu_cmd_bind_compute_pipeline(cmd, r->trace);
	spudgpu_cmd_bind_descriptor_sets_compute(cmd, r->trace, 0, &r->set, 1);
	const rq_params params = {{0.0f, 0.0f, -1.0f, 0.0f}, {1.5f / RQ_GRID, 1.5f / RQ_GRID, 100.0f, 0.0f}, RQ_GRID, RQ_GRID, 0xFF, 0};
	spudgpu_cmd_push_constants_compute(cmd, r->trace, 0, sizeof(params), &params);
	spudgpu_cmd_dispatch(cmd, RQ_GRID / 8, RQ_GRID / 8, 1);
	spudgpu_end_command_list(cmd);
	++r->frame;
	spudgpu_submit_desc submit;
	memset(&submit, 0, sizeof(submit));
	submit.base.type          = SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = &cmd;
	submit.cmd_list_count     = 1;
	submit.signal_fence       = r->fence;
	submit.signal_fence_value = r->frame;
	return spudgpu_queue_submit(r->f.queue, &submit) == SPUD_SUCCESS;
}

static spudgpu_buffer rq_buffer(spudgpu_device device, SPUDGPU_MEMORY_KIND kind, SPUDGPU_BUFFER_USAGE usage, uint64_t size) {
	spudgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = usage;
	desc.memory_kind      = kind;
	desc.size             = size;
	spudgpu_buffer buffer = NULL;
	CHECK(spudgpu_create_buffer(device, &desc, &buffer) == SPUD_SUCCESS);
	return buffer;
}

/* SpudGPU's ray query frame: a bottom level of one triangle and a top
 * level of one instance built once, then, per frame, the instance written
 * by the CPU call, the top level refitted and a 64 x 64 grid traced. */
static void test_gpu_ray_query(uint32_t adapter_index) {
	counting count;
	memset(&count, 0, sizeof(count));
	count.system             = spud_allocator_system();
	SPUD_ALLOCATOR allocator = {&count, counting_allocate, counting_free};
	spudgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "spud_noalloc_tests";
	instance_desc.engine_name      = "spud_noalloc_tests";
	spudgpu_instance instance      = NULL;
	if (spudgpu_create_instance(&instance_desc, &instance) != SPUD_SUCCESS)
		return;
	const spudgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	spudgpu_enumerate_adapters(instance, &adapters, &adapter_count);
	SPUDGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	if (adapter_index >= adapter_count || spudgpu_get_adapter_properties(adapters[adapter_index], &properties) != SPUD_SUCCESS ||
	    !(properties.supported_features & SPUDGPU_FEATURE_RAY_QUERY)) {
		printf("spud_noalloc_tests: SpudGPU: the adapter has no SPUDGPU_FEATURE_RAY_QUERY, the ray query frame skipped\n");
		spudgpu_destroy_instance(instance);
		return;
	}
	rq_frame r;
	memset(&r, 0, sizeof(r));
	spudgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.required_features  = SPUDGPU_FEATURE_RAY_QUERY;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "spud_noalloc_tests ray queries";
	CHECK(spudgpu_create_device(adapters[adapter_index], &device_desc, &r.f.device) == SPUD_SUCCESS);
	spudgpu_device device = r.f.device;
	if (!device) {
		spudgpu_destroy_instance(instance);
		return;
	}
	CHECK(spudgpu_get_command_queue(device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &r.f.queue) == SPUD_SUCCESS);
	spudgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(spudgpu_create_command_allocator(device, &allocator_desc, &r.f.allocator) == SPUD_SUCCESS);
	if (r.f.allocator)
		CHECK(spudgpu_create_command_list(r.f.allocator, &r.f.cmd) == SPUD_SUCCESS);
	CHECK(spudgpu_create_fence(device, SPUDGPU_FENCE_FLAG_NONE, 0, &r.fence) == SPUD_SUCCESS);
	spudgpu_acceleration_structure_instance_layout layout;
	memset(&layout, 0, sizeof(layout));
	CHECK(spudgpu_get_acceleration_structure_instance_layout(device, &layout) == SPUD_SUCCESS);
	r.instance_size = layout.size;

	const float vertices[9]          = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	spudgpu_buffer vertex_buffer     = rq_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, sizeof(vertices));
	r.instances                      = rq_buffer(device, SPUDGPU_MEMORY_KIND_UPLOAD, SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, layout.size);
	r.hits                           = rq_buffer(device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE, 32ull * RQ_GRID * RQ_GRID);
	spudgpu_shader_module module     = NULL;
	spudgpu_descriptor_set_layout set_layout = NULL;
	spudgpu_descriptor_pool pool     = NULL;
	if (vertex_buffer) {
		void *mapped = NULL;
		if (spudgpu_map_buffer(vertex_buffer, 0, sizeof(vertices), &mapped) == SPUD_SUCCESS) {
			memcpy(mapped, vertices, sizeof(vertices));
			spudgpu_flush_buffer(vertex_buffer, 0, sizeof(vertices));
			spudgpu_unmap_buffer(vertex_buffer);
		}
	}
	if (r.instances)
		CHECK(spudgpu_map_buffer(r.instances, 0, layout.size, &r.mapped_instances) == SPUD_SUCCESS);

	spudgpu_acceleration_structure_geometry geometry;
	memset(&geometry, 0, sizeof(geometry));
	geometry.kind        = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	geometry.flags       = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
	geometry.data        = vertex_buffer;
	geometry.data_stride = 12;
	geometry.data_count  = 3;
	spudgpu_acceleration_structure_inputs bottom_inputs;
	memset(&bottom_inputs, 0, sizeof(bottom_inputs));
	bottom_inputs.base.type      = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	bottom_inputs.level          = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	bottom_inputs.flags          = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE;
	bottom_inputs.geometries     = &geometry;
	bottom_inputs.geometry_count = 1;
	spudgpu_acceleration_structure_sizes bottom_sizes, top_sizes;
	memset(&bottom_sizes, 0, sizeof(bottom_sizes));
	bottom_sizes.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	top_sizes              = bottom_sizes;
	CHECK(spudgpu_get_acceleration_structure_build_sizes(device, &bottom_inputs, &bottom_sizes) == SPUD_SUCCESS);

	// What creating a structure takes from the caller's allocator.
	spudgpu_acceleration_structure_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	desc.level              = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	desc.size               = bottom_sizes.storage_size;
	size_t blocks           = count.blocks;
	size_t bytes            = count.bytes;
	CHECK(spudgpu_create_acceleration_structure(device, &desc, &r.bottom) == SPUD_SUCCESS);
	printf("spud_noalloc_tests: SpudGPU: creating a bottom level took %zu block(s), %zu bytes, from the caller's allocator\n",
	       count.blocks - blocks, count.bytes - bytes);
	CHECK(spudgpu_get_acceleration_structure_reference(r.bottom, &r.bottom_reference) == SPUD_SUCCESS);

	r.top_inputs.base.type          = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	r.top_inputs.level              = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	r.top_inputs.flags              = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	r.top_inputs.instances          = r.instances;
	r.top_inputs.instance_count     = 1;
	r.top_inputs.bottom_levels      = &r.bottom;
	r.top_inputs.bottom_level_count = 1;
	if (r.bottom)
		CHECK(spudgpu_get_acceleration_structure_build_sizes(device, &r.top_inputs, &top_sizes) == SPUD_SUCCESS);
	desc.level             = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	desc.size              = top_sizes.storage_size ? top_sizes.storage_size : 1;
	desc.max_bottom_levels = 1;
	blocks                 = count.blocks;
	bytes                  = count.bytes;
	CHECK(spudgpu_create_acceleration_structure(device, &desc, &r.top) == SPUD_SUCCESS);
	printf("spud_noalloc_tests: SpudGPU: creating a top level of 1 bottom level took %zu block(s), %zu bytes\n", count.blocks - blocks,
	       count.bytes - bytes);
	// The bottom level's scratch, then the top level's at 64 KiB past it
	// (a multiple of every device's scratch alignment), for its build and
	// every update: no scratch range is used by two builds in one list.
	r.top_scratch_offset = (bottom_sizes.build_scratch_size + 65535u) / 65536u * 65536u;
	uint64_t top_scratch = top_sizes.build_scratch_size;
	if (top_sizes.update_scratch_size > top_scratch)
		top_scratch = top_sizes.update_scratch_size;
	r.scratch = rq_buffer(device, SPUDGPU_MEMORY_KIND_DEVICE, SPUDGPU_BUFFER_USAGE_STORAGE, r.top_scratch_offset + top_scratch + 256);

	// The pipeline and its set.
	SPUDGPU_SHADER_FORMAT format = SPUDGPU_SHADER_FORMAT_NONE;
	for (SPUDGPU_SHADER_FORMAT f = SPUDGPU_SHADER_FORMAT_SPIRV; f <= SPUDGPU_SHADER_FORMAT_METALLIB; ++f)
		if (properties.shader_formats & (1u << f))
			format = f;
	spudgpu_shader_module_desc module_desc = spudgpu_test_module_desc(&spudgpu_test_rq_trace, format, "rq_trace");
	if (module_desc.code)
		CHECK(spudgpu_create_shader_module(device, &module_desc, &module) == SPUD_SUCCESS);
	spudgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = SPUDGPU_SHADER_STAGE_COMPUTE;
	layout_desc.bindings[1]                 = layout_desc.bindings[0];
	layout_desc.bindings[1].binding         = 1;
	layout_desc.bindings[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	layout_desc.binding_count               = 2;
	CHECK(spudgpu_create_descriptor_set_layout(device, &layout_desc, &set_layout) == SPUD_SUCCESS);
	spudgpu_compute_pipeline_desc pipeline_desc;
	memset(&pipeline_desc, 0, sizeof(pipeline_desc));
	pipeline_desc.base.type                           = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	pipeline_desc.compute_module                      = module;
	pipeline_desc.descriptor_set_layouts[0]           = set_layout;
	pipeline_desc.descriptor_set_layout_count         = 1;
	pipeline_desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	pipeline_desc.push_constant_ranges[0].size        = sizeof(rq_params);
	pipeline_desc.push_constant_range_count           = 1;
	if (module && set_layout)
		CHECK(spudgpu_create_compute_pipeline(device, &pipeline_desc, &r.trace) == SPUD_SUCCESS);
	spudgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_sizes[1].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[1].count           = 1;
	pool_desc.pool_size_count               = 2;
	CHECK(spudgpu_create_descriptor_pool(device, &pool_desc, &pool) == SPUD_SUCCESS);
	if (pool && set_layout) {
		spudgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = set_layout;
		set_desc.set_count      = 1;
		CHECK(spudgpu_create_descriptor_sets(device, &set_desc, &r.set) == SPUD_SUCCESS);
	}

	const bool ready = r.f.queue && r.f.cmd && r.fence && vertex_buffer && r.mapped_instances && r.hits && r.scratch && r.bottom && r.top &&
	                   r.trace && r.set;
	CHECK(ready);
	if (ready) {
		spudgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		writes[0].base.type               = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		writes[0].dst_set                 = r.set;
		writes[0].descriptor_count        = 1;
		writes[0].descriptor_type         = SPUDGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		writes[0].acceleration_structures = &r.top;
		const spudgpu_descriptor_buffer_info hits = {r.hits, 0, 32ull * RQ_GRID * RQ_GRID};
		writes[1].base.type                       = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		writes[1].dst_set                         = r.set;
		writes[1].dst_binding                     = 1;
		writes[1].descriptor_count                = 1;
		writes[1].descriptor_type                 = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].buffer_info                     = &hits;
		spudgpu_update_descriptor_sets(device, writes, 2);

		// The one build of both levels, before the loop.
		spudgpu_acceleration_structure_instance instance;
		memset(&instance, 0, sizeof(instance));
		instance.transform[0]           = 1.0f;
		instance.transform[5]           = 1.0f;
		instance.transform[10]          = 1.0f;
		instance.mask                   = 0xFF;
		instance.bottom_level_reference = r.bottom_reference;
		CHECK(spudgpu_write_acceleration_structure_instances(device, &instance, 1, r.mapped_instances) == SPUD_SUCCESS);
		spudgpu_flush_buffer(r.instances, 0, layout.size);
		spudgpu_begin_command_list(r.f.cmd);
		spudgpu_buffer_barrier b;
		memset(&b, 0, sizeof(b));
		b.base.type    = SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER;
		b.buffer       = r.scratch;
		b.state_before = SPUDGPU_RESOURCE_STATE_COMMON;
		b.state_after  = SPUDGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH;
		spudgpu_cmd_pipeline_barrier(r.f.cmd, &b, 1, NULL, 0);
		spudgpu_acceleration_structure_build build;
		memset(&build, 0, sizeof(build));
		build.base.type   = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode        = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs      = &bottom_inputs;
		build.destination = r.bottom;
		build.scratch     = r.scratch;
		CHECK(spudgpu_cmd_build_acceleration_structures(r.f.cmd, &build, 1) == SPUD_SUCCESS);
		spudgpu_acceleration_structure_barrier ab;
		memset(&ab, 0, sizeof(ab));
		ab.base.type     = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
		ab.structure     = r.bottom;
		ab.access_before = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
		ab.access_after  = SPUDGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		spudgpu_cmd_acceleration_structure_barrier(r.f.cmd, &ab, 1);
		build.inputs         = &r.top_inputs;
		build.destination    = r.top;
		build.scratch_offset = r.top_scratch_offset;
		CHECK(spudgpu_cmd_build_acceleration_structures(r.f.cmd, &build, 1) == SPUD_SUCCESS);
		spudgpu_end_command_list(r.f.cmd);
		CHECK(spudgpu_submit_command_lists(r.f.queue, &r.f.cmd, 1) == SPUD_SUCCESS);
		spudgpu_queue_wait_idle(r.f.queue);

		bool ok = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = rq_frame_run(&r);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = rq_frame_run(&r);
		alloc_trace_stop();
		CHECK(ok);
		report("SpudGPU ray queries: the instance written, the top level refitted, a 64 x 64 grid traced");
		uint64_t best = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			const uint64_t start = spudperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = rq_frame_run(&r);
			const uint64_t took = spudperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
		}
		if (ok)
			print_time("SpudGPU ray query frame (write, refit, trace, fence wait)", best, TIMED_ITERATIONS);
		uint64_t value = r.frame;
		spudgpu_wait_for_fences(device, &r.fence, &value, 1, true, UINT64_MAX);
	}
	if (r.mapped_instances)
		spudgpu_unmap_buffer(r.instances);
	spudgpu_destroy_descriptor_pool(pool);
	spudgpu_destroy_compute_pipeline(r.trace);
	spudgpu_destroy_descriptor_set_layout(set_layout);
	spudgpu_destroy_shader_module(module);
	spudgpu_destroy_acceleration_structure(r.top);
	spudgpu_destroy_acceleration_structure(r.bottom);
	spudgpu_destroy_buffer(r.scratch);
	spudgpu_destroy_buffer(r.hits);
	spudgpu_destroy_buffer(r.instances);
	spudgpu_destroy_buffer(vertex_buffer);
	spudgpu_destroy_fence(r.fence);
	spudgpu_destroy_command_list(r.f.cmd);
	spudgpu_destroy_command_allocator(r.f.allocator);
	spudgpu_destroy_device(device);
	spudgpu_destroy_instance(instance);
}
#endif

// ---- SpudAudio -------------------------------------------------------------

/* The first counted call, and how many are counted. */
#define AUDIO_FIRST_COUNTED 20

typedef struct silence {
	uint32_t channels;
	uint32_t sample_bytes;
	bool interleaved;
	/* Written only by the callback's thread; read by main after the stream
	 * has stopped. */
	volatile uint32_t calls;
	volatile bool counted;
	/* Of the callbacks after the counted ones: the time from one call to
	 * the next is the device's period, so what is timed is the callback's
	 * own share: the time SpudAudio's thread spends from the start of one
	 * call to the start of the next, less the time it waits, is not
	 * visible here. The callback's own time is. */
} silence;

static void silence_callback(spudaudio_stream stream, void *frames, uint32_t frame_count, const SPUDAUDIO_CALLBACK_INFO *info, void *user_data) {
	(void)stream;
	(void)info;
	silence *s           = (silence *)user_data;
	const uint32_t calls = s->calls;
	if (calls == AUDIO_FIRST_COUNTED)
		alloc_trace_start();
	if (calls == AUDIO_FIRST_COUNTED + ITERATIONS) {
		alloc_trace_end();
		s->counted = true;
	}
	if (s->interleaved) {
		memset(frames, 0, (size_t)frame_count * s->channels * s->sample_bytes);
	} else {
		for (uint32_t c = 0; c < s->channels; ++c)
			memset(((void **)frames)[c], 0, (size_t)frame_count * s->sample_bytes);
	}
	s->calls = calls + 1;
}

static void test_audio(void) {
#if SPUDAUDIO_COMPILE_COREAUDIO
	const SPUDAUDIO_NATIVE_API api = SPUDAUDIO_NATIVE_API_COREAUDIO;
#elif SPUDAUDIO_COMPILE_WASAPI
	const SPUDAUDIO_NATIVE_API api = SPUDAUDIO_NATIVE_API_WASAPI;
#elif SPUDAUDIO_COMPILE_ALSA
	const SPUDAUDIO_NATIVE_API api = SPUDAUDIO_NATIVE_API_ALSA;
#elif SPUDAUDIO_COMPILE_REMOTEIO
	const SPUDAUDIO_NATIVE_API api = SPUDAUDIO_NATIVE_API_REMOTEIO;
#else
	const SPUDAUDIO_NATIVE_API api = SPUDAUDIO_NATIVE_API_NONE;
#endif
	if (api == SPUDAUDIO_NATIVE_API_NONE) {
		printf("spud_noalloc_tests: SpudAudio: no native API in this build, skipped\n");
		return;
	}
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	SPUDAUDIO_INSTANCE_DESC instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type     = SPUD_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	instance_desc.allocator     = &allocator;
	instance_desc.native_api    = api;
	spudaudio_instance instance = NULL;
	if (spudaudio_create_instance(&instance_desc, &instance) != SPUD_SUCCESS) {
		printf("spud_noalloc_tests: SpudAudio: no instance, skipped\n");
		return;
	}
	spudaudio_device *devices = NULL;
	uint32_t device_count     = 0;
	spudaudio_enumerate_devices(instance, SPUDAUDIO_DIRECTION_OUTPUT, &devices, &device_count);
	SPUDAUDIO_DEVICE_PROPERTIES props;
	spudaudio_device device = NULL;
	for (uint32_t i = 0; i < device_count && !device; ++i) {
		memset(&props, 0, sizeof(props));
		props.base.type = SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		if (spudaudio_get_device_properties(devices[i], &props) == SPUD_SUCCESS && (props.default_roles & SPUDAUDIO_DEFAULT_ROLE_GENERAL))
			device = devices[i];
	}
	if (!device && device_count > 0) {
		device = devices[0];
		memset(&props, 0, sizeof(props));
		props.base.type = SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		spudaudio_get_device_properties(device, &props);
	}
	if (!device) {
		printf("spud_noalloc_tests: SpudAudio: no output device, skipped\n");
		spudaudio_destroy_instance(instance);
		return;
	}

	SPUDAUDIO_STREAM_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_AUDIO_STREAM_DESC;
	desc.device    = device;
	if (props.native_format.sample_format != SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN) {
		desc.format = props.native_format;
	} else {
		desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
		desc.format.sample_rate   = 48000;
		desc.format.channel_count = 2;
		desc.format.interleaved   = true;
	}
	desc.flags = SPUDAUDIO_STREAM_FLAG_NONE;
	/* Core Audio's and RemoteIO's callbacks run on the platform's own
	 * thread, the only mechanism they have; the others have a thread of
	 * SpudAudio's at the OS's default. */
#if SPUDAUDIO_COMPILE_COREAUDIO || SPUDAUDIO_COMPILE_REMOTEIO
	desc.thread.priority = SPUDAUDIO_THREAD_PRIORITY_PLATFORM;
#else
	desc.thread.priority = SPUDAUDIO_THREAD_PRIORITY_NORMAL;
#endif
	desc.callback        = silence_callback;
	SPUDAUDIO_TIMING_CAPS caps;
	memset(&caps, 0, sizeof(caps));
	caps.base.type = SPUD_STRUCT_TYPE_AUDIO_TIMING_CAPS;
	if (spudaudio_get_timing_caps(device, &desc.format, desc.flags, &caps) == SPUD_SUCCESS) {
		desc.period_frames = caps.default_period_frames ? caps.default_period_frames : caps.min_period_frames;
		if (caps.max_buffer_frames) {
			desc.buffer_frames = desc.period_frames * 2;
			if (desc.buffer_frames < caps.min_buffer_frames)
				desc.buffer_frames = caps.min_buffer_frames;
			if (desc.buffer_frames > caps.max_buffer_frames)
				desc.buffer_frames = caps.max_buffer_frames;
		}
	} else {
		desc.period_frames = desc.format.sample_rate / 100;
		desc.buffer_frames = api == SPUDAUDIO_NATIVE_API_ALSA ? desc.period_frames * 2 : 0;
	}
	/* Each rejected field takes the probe's suggestion, a few rounds. */
	SPUDAUDIO_STREAM_CONFIG config;
	bool accepted = false;
	for (int round = 0; round < 8 && !accepted; ++round) {
		memset(&config, 0, sizeof(config));
		config.base.type   = SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		const SPUDRESULT r = spudaudio_probe_stream(&desc, &config);
		if (r == SPUD_SUCCESS)
			accepted = true;
		else if (r == SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED && config.format.sample_format != SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN)
			desc.format = config.format;
		else if (r == SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE && config.period_frames)
			desc.period_frames = config.period_frames;
		else if (r == SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE)
			desc.buffer_frames = config.buffer_frames;
		else
			break;
	}
	if (!accepted) {
		printf("spud_noalloc_tests: SpudAudio: the device takes no stream this test asks for, skipped\n");
		spudaudio_destroy_instance(instance);
		return;
	}
	silence s;
	memset(&s, 0, sizeof(s));
	desc.user_data          = &s;
	spudaudio_stream stream = NULL;
	CHECK(spudaudio_create_stream(&desc, &stream) == SPUD_SUCCESS);
	if (stream) {
		memset(&config, 0, sizeof(config));
		config.base.type = SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		CHECK(spudaudio_stream_get_config(stream, &config) == SPUD_SUCCESS);
		s.channels     = config.format.channel_count;
		s.sample_bytes = spudaudio_sample_format_byte_size(config.format.sample_format);
		s.interleaved  = config.format.interleaved;
		const SPUDRESULT started = spudaudio_stream_start(stream);
		if (started != SPUD_SUCCESS)
			printf("spud_noalloc_tests: SpudAudio: the stream didn't start: %s\n", spudresult_str(started));
		CHECK(started == SPUD_SUCCESS);
		/* The counted calls are a little over a second at a 10 ms period;
		 * ten seconds is plenty for any period a device takes. */
		for (int waited = 0; waited < 10000 && !s.counted; waited += 10)
			sleep_ms(10);
		CHECK(spudaudio_stream_stop(stream) == SPUD_SUCCESS);
		CHECK(s.counted);
		if (s.counted) {
			alloc_trace_attribute();
			char what[160];
			snprintf(what, sizeof(what), "SpudAudio's stream callback writing silence, %u-frame period, callbacks %d to %d",
			         config.period_frames, AUDIO_FIRST_COUNTED, AUDIO_FIRST_COUNTED + ITERATIONS);
			report(what);
		}
		spudaudio_destroy_stream(stream);
	}
	CHECK(spudaudio_destroy_instance(instance) == SPUD_SUCCESS);
}

// ---- SpudNet ---------------------------------------------------------------

typedef struct idle_connection {
	spudnet_tcp_socket client, server;
	spudnet_wait_set set;
} idle_connection;

static bool net_round(const idle_connection *c) {
	uint8_t buffer[64];
	uint64_t received = 0;
	spudnet_wait_event events[4];
	uint32_t count = 0;
	return spudnet_tcp_socket_recv(c->client, buffer, sizeof(buffer), SPUDNET_NO_WAIT, &received) == SPUDRESULT_SPUDNET_TIMED_OUT &&
	       spudnet_tcp_socket_recv(c->server, buffer, sizeof(buffer), SPUDNET_NO_WAIT, &received) == SPUDRESULT_SPUDNET_TIMED_OUT &&
	       spudnet_wait_set_wait(c->set, SPUDNET_NO_WAIT, events, 4, &count) == SPUDRESULT_SPUDNET_TIMED_OUT;
}

static void test_net(void) {
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	spudnet_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type   = SPUD_STRUCT_TYPE_NET_INSTANCE_DESC;
	instance_desc.allocator   = &allocator;
	spudnet_instance instance = NULL;
	if (spudnet_instance_create(&instance_desc, &instance, NULL) != SPUD_SUCCESS) {
		printf("spud_noalloc_tests: SpudNet: no instance, skipped\n");
		return;
	}
	spudnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type         = SPUD_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address           = "127.0.0.1";
	listen_desc.backlog           = 1;
	spudnet_tcp_listener listener = NULL;
	if (spudnet_tcp_listener_create(instance, &listen_desc, &listener, NULL) != SPUD_SUCCESS) {
		printf("spud_noalloc_tests: SpudNet: no loopback listener, skipped\n");
		spudnet_instance_destroy(instance);
		return;
	}
	uint16_t port = 0;
	CHECK(spudnet_tcp_listener_get_port(listener, &port) == SPUD_SUCCESS);
	idle_connection c;
	memset(&c, 0, sizeof(c));
	CHECK(spudnet_tcp_socket_create(instance, &c.client) == SPUD_SUCCESS);
	spudnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type  = SPUD_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = port;
	connect_desc.timeout_ms = 5000;
	if (c.client)
		CHECK(spudnet_tcp_socket_connect(c.client, &connect_desc) == SPUD_SUCCESS);
	CHECK(spudnet_tcp_listener_accept(listener, 5000, &c.server) == SPUD_SUCCESS);
	CHECK(spudnet_wait_set_create(instance, &c.set, NULL) == SPUD_SUCCESS);
	static int client_tag, server_tag;
	if (c.client && c.server && c.set) {
		CHECK(spudnet_wait_set_add_tcp_socket(c.set, c.client, SPUDNET_WAIT_FOR_INCOMING, &client_tag) == SPUD_SUCCESS);
		CHECK(spudnet_wait_set_add_tcp_socket(c.set, c.server, SPUDNET_WAIT_FOR_INCOMING, &server_tag) == SPUD_SUCCESS);
		bool ok = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = net_round(&c);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = net_round(&c);
		alloc_trace_stop();
		CHECK(ok);
		report("SpudNet's receives with SPUDNET_NO_WAIT on an idle loopback connection's two ends and a wait set's poll");
		uint64_t best = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			const uint64_t start = spudperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = net_round(&c);
			const uint64_t took = spudperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
		}
		if (ok)
			print_time("SpudNet idle round (two receives, a wait set poll)", best, TIMED_ITERATIONS);
	}
	spudnet_wait_set_destroy(c.set);
	spudnet_tcp_socket_destroy(c.server);
	spudnet_tcp_socket_destroy(c.client);
	spudnet_tcp_listener_destroy(listener);
	spudnet_instance_destroy(instance);
}

// ---- SpudInput -------------------------------------------------------------

static void test_input(void) {
#if SPUDINPUT_COMPILE_GAMECONTROLLER
	const SPUDINPUT_NATIVE_API api = SPUDINPUT_NATIVE_API_GAMECONTROLLER;
#elif SPUDINPUT_COMPILE_GAMEINPUT
	const SPUDINPUT_NATIVE_API api = SPUDINPUT_NATIVE_API_GAMEINPUT;
#elif SPUDINPUT_COMPILE_XINPUT
	const SPUDINPUT_NATIVE_API api = SPUDINPUT_NATIVE_API_XINPUT;
#elif SPUDINPUT_COMPILE_EVDEV
	const SPUDINPUT_NATIVE_API api = SPUDINPUT_NATIVE_API_EVDEV;
#else
	const SPUDINPUT_NATIVE_API api = SPUDINPUT_NATIVE_API_NONE;
#endif
	if (api == SPUDINPUT_NATIVE_API_NONE) {
		printf("spud_noalloc_tests: SpudInput: no native API in this build, skipped\n");
		return;
	}
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	SPUDINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type  = SPUD_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.allocator  = &allocator;
	desc.native_api = api;
#if SPUDINPUT_COMPILE_XINPUT || SPUDINPUT_COMPILE_EVDEV
	desc.read_in_background = true; /* these always read */
#else
	desc.read_in_background = false;
#endif
	desc.motion_sample_capacity = 0;
	spudinput_instance instance = NULL;
	const SPUDRESULT created    = spudinput_instance_create(&desc, &instance);
	if (created != SPUD_SUCCESS) {
		printf("spud_noalloc_tests: SpudInput: no instance (%s), skipped\n", spudresult_str(created));
		return;
	}
	bool ok = true;
	for (int i = 0; i < WARM_UP && ok; ++i)
		ok = spudinput_instance_poll(instance) == SPUD_SUCCESS;
	CHECK(ok);
	const spudinput_gamepad_id *ids = NULL;
	uint32_t count                  = 0;
	spudinput_instance_get_gamepads(instance, &ids, &count);
	alloc_trace_start();
	for (int i = 0; i < ITERATIONS && ok; ++i)
		ok = spudinput_instance_poll(instance) == SPUD_SUCCESS;
	alloc_trace_stop();
	CHECK(ok);
	char what[128];
	snprintf(what, sizeof(what), "SpudInput's poll, %u gamepad(s) connected", count);
	report(what);
	uint64_t best = UINT64_MAX;
	for (int run = 0; run < TIMED_RUNS && ok; ++run) {
		const uint64_t start = spudperf_get_monotonic_time_ns();
		for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
			ok = spudinput_instance_poll(instance) == SPUD_SUCCESS;
		const uint64_t took = spudperf_get_monotonic_time_ns() - start;
		if (took < best)
			best = took;
	}
	if (ok)
		print_time("SpudInput poll", best, TIMED_ITERATIONS);
	spudinput_instance_destroy(instance);
}

int main(int argc, char **argv) {
	uint32_t adapter = 0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
			adapter = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: spud_noalloc_tests [--adapter N]\n");
			return 2;
		}
	}
	if (!alloc_trace_available()) {
		printf("spud_noalloc_tests: no allocation hook on this platform or build (a sanitizer's heap): nothing measured\n");
		return 0;
	}
	test_hook_counts();
	test_gpu(adapter);
	test_audio();
	test_net();
	test_input();
	if (failures) {
		printf("spud_noalloc_tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("spud_noalloc_tests: no allocation of ours in any hot path\n");
	return 0;
}
