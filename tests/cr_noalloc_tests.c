/*
 * cr_noalloc_tests — no allocation per frame (ROADMAP.md in the workspace,
 * section 3.1, "Data-oriented checks").
 *
 * Each module's hot path, warmed up, then counted for 100 iterations with
 * every heap allocation the counting thread makes, through tests/alloctrace.h,
 * which says what each platform's hook sees and how each allocation is put
 * down to the code that asked for it. Only ours (this executable's, where
 * Carrottus is linked) must be none; what a driver, the graphics runtime or the
 * system asks for is reported apart and not counted. Where no hook is
 * available the test says so and passes nothing off as checked.
 *
 * The hot paths:
 * - Carrottus GPU recording and submitting a frame offscreen on a real device:
 *   reset the allocator, begin, a barrier, a pass that clears an image, a
 *   barrier out, end, crgpu_command_queue_submit() signalling a fence, and a wait
 *   for the frame before. On every platform with an adapter.
 * - Carrottus GPU's submit and present loop: the same frame drawn into a swap
 *   chain's image, acquired, submitted with
 *   crgpu_command_queue_submit_lists_synced() and presented. Windows presents to
 *   a hidden window, macOS to a CAMetalLayer with no window; Linux has no
 *   surface here without a display, so it is skipped there.
 * - Carrottus Audio's stream thread (the platform's own on Core Audio and
 *   RemoteIO): an output stream on the general default
 *   device writing silence, counted on the callback's own thread from its
 *   Nth call to its (N+100)th, everything Carrottus Audio does on that thread
 *   between the calls included. Skipped with no output device.
 * - Carrottus Net with a loopback connection open and idle: a receive with
 *   CRNET_NO_WAIT on each end and a wait set's poll, per round. And a
 *   WebSocket, opened against a server this test writes over that
 *   loopback: one message sent a round with CRNET_NO_WAIT and flushed with
 *   it (the message buffer grown once, then kept), the server's end
 *   drained. Where the stack copies a message itself (NSURLSession's
 *   NSData), that copy is the platform's and is reported apart.
 * - Carrottus Input's poll, on the platform's native API, gamepads or none.
 * - Carrottus GPU's ray queries (CRGPU_EXT_RAY_QUERY), on an adapter with them:
 *   a bottom level and a top level built once, then per frame the instance
 *   written by crgpu_device_write_acceleration_structure_instances() into a
 *   mapped buffer, the top level refitted in place and rq_trace.slang
 *   tracing a 64 x 64 grid. Creating each structure's bytes from the
 *   caller's allocator are printed.
 *
 * Each scenario also times its iterations, uncounted, and prints the best
 * of a few runs: what a change to its path costs or saves.
 *
 *   cr_noalloc_tests [--adapter N]
 *
 * --adapter picks the GPU, by its index in crgpu_adapter_enumerate(); 0 by
 * default. Exits 0 when every check passes, 1 when one fails.
 */

#include <craudio.h>
#include <crcore.h>
#include <crgpu.h>
#include <crinput.h>
#include <crnet.h>
#include <crperf.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloctrace.h"
#include "crgpu_test_shaders.h"

#if CRGPU_COMPILE_METAL_API
#include "crgpu_test_metal_layer.h"
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
	snprintf(line, sizeof(line), "cr_noalloc_tests: %s, %d iterations", what, ITERATIONS);
	alloc_trace_report(line, 6);
	if (alloc_trace_ours() != 0)
		++failures;
}

static void print_time(const char *what, uint64_t best_ns, uint32_t iterations) {
	printf("cr_noalloc_tests: time: %s: %.2f us an iteration (best of %d runs of %u)\n", what,
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
	printf("cr_noalloc_tests: the hook counted %lu of 2 known allocations of ours\n", alloc_trace_ours());
	CHECK(alloc_trace_ours() == 2);
}

// ---- Carrottus GPU ---------------------------------------------------------------

#define TARGET_SIZE 256

typedef struct gpu_frame {
	crgpu_device device;
	crgpu_command_queue queue;
	crgpu_command_allocator allocator;
	crgpu_command_list cmd;
} gpu_frame;

/* The commands of one frame: the target into a colour attachment, a pass
 * that clears it, and the target out to [after]. */
static void record_frame(crgpu_command_list cmd, crgpu_image_view view, uint32_t width, uint32_t height, CRGPU_IMAGE_LAYOUT before,
                         CRGPU_IMAGE_LAYOUT after, uint64_t frame) {
	crgpu_command_list_begin(cmd);
	crgpu_command_list_image_barrier_view(cmd, view, before, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	crgpu_rendering_begin_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.base.type                       = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = view;
	pass.color_attachments[0].load_op    = CRGPU_LOAD_OP_CLEAR;
	pass.color_attachments[0].store_op   = CRGPU_STORE_OP_STORE;
	pass.color_attachments[0].clear_color[0] = (float)(frame % 16) / 16.0f;
	pass.color_attachments[0].clear_color[3] = 1.0f;
	pass.color_attachment_count          = 1;
	pass.width                           = width;
	pass.height                          = height;
	crgpu_command_list_begin_rendering(cmd, &pass);
	crgpu_command_list_end_rendering(cmd);
	crgpu_command_list_image_barrier_view(cmd, view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, after);
	crgpu_command_list_end(cmd);
}

/* Offscreen: one frame, submitted with a fence signalled to [frame]; the
 * frame before waited for first, so the one allocator can be reset. */
typedef struct offscreen {
	gpu_frame f;
	crgpu_image image;
	crgpu_image_view view;
	crgpu_fence fence;
	uint64_t frame;
	/* Of the timed runs: the time in crgpu_command_queue_submit() alone. */
	uint64_t submit_ns;
} offscreen;

static bool offscreen_frame(offscreen *o) {
	if (o->frame > 0) {
		uint64_t value = o->frame;
		if (crgpu_fence_wait(o->f.device, &o->fence, &value, 1, true, UINT64_MAX) != CR_SUCCESS)
			return false;
	}
	if (crgpu_command_allocator_reset(o->f.allocator) != CR_SUCCESS)
		return false;
	record_frame(o->f.cmd, o->view, TARGET_SIZE, TARGET_SIZE,
	             o->frame == 0 ? CRGPU_IMAGE_LAYOUT_UNDEFINED : CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY,
	             CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, o->frame);
	++o->frame;
	crgpu_submit_desc submit;
	memset(&submit, 0, sizeof(submit));
	submit.base.type          = CR_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = &o->f.cmd;
	submit.cmd_list_count     = 1;
	submit.signal_fence       = o->fence;
	submit.signal_fence_value = o->frame;
	const uint64_t start       = crperf_get_monotonic_time_ns();
	const CR_RESULT submitted = crgpu_command_queue_submit(o->f.queue, &submit);
	o->submit_ns += crperf_get_monotonic_time_ns() - start;
	return submitted == CR_SUCCESS;
}

static void test_gpu_offscreen(gpu_frame f) {
	offscreen o;
	memset(&o, 0, sizeof(o));
	o.f = f;
	crgpu_image_desc image_desc;
	memset(&image_desc, 0, sizeof(image_desc));
	image_desc.base.type    = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	image_desc.usage        = CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | CRGPU_IMAGE_USAGE_SAMPLED;
	image_desc.type         = CRGPU_IMAGE_TYPE_2D;
	image_desc.format       = CRGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.clear_value.format = CRGPU_FORMAT_R8G8B8A8_UNORM;
	image_desc.width        = TARGET_SIZE;
	image_desc.height       = TARGET_SIZE;
	image_desc.depth        = 1;
	image_desc.array_layers = 1;
	image_desc.mip_levels   = 1;
	image_desc.debug_name   = "noalloc target";
	CHECK(crgpu_image_create(f.device, &image_desc, &o.image) == CR_SUCCESS);
	crgpu_image_view_desc view_desc;
	memset(&view_desc, 0, sizeof(view_desc));
	view_desc.base.type                           = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	view_desc.parent_image                        = o.image;
	view_desc.type                                = CRGPU_IMAGE_VIEW_TYPE_2D;
	view_desc.subresource_range.aspect_mask       = CRGPU_IMAGE_ASPECT_COLOR;
	view_desc.subresource_range.mip_level_count   = 1;
	view_desc.subresource_range.array_layer_count = 1;
	if (o.image)
		CHECK(crgpu_image_view_create(o.image, &view_desc, &o.view) == CR_SUCCESS);
	CHECK(crgpu_fence_create(f.device, CRGPU_FENCE_FLAG_NONE, 0, &o.fence) == CR_SUCCESS);
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
		report("Carrottus GPU recording and submitting a frame offscreen, a fence waited on");

		uint64_t best = UINT64_MAX, best_submit = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			o.submit_ns          = 0;
			const uint64_t start = crperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = offscreen_frame(&o);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
			if (o.submit_ns < best_submit)
				best_submit = o.submit_ns;
		}
		if (ok) {
			print_time("Carrottus GPU offscreen frame (recording, submit, fence wait)", best, TIMED_ITERATIONS);
			print_time("Carrottus GPU offscreen frame, crgpu_command_queue_submit() alone", best_submit, TIMED_ITERATIONS);
		}
		uint64_t value = o.frame;
		crgpu_fence_wait(f.device, &o.fence, &value, 1, true, UINT64_MAX);
	}
	crgpu_fence_destroy(o.fence);
	crgpu_image_view_destroy(o.view);
	crgpu_image_destroy(o.image);
}

#if defined(_WIN32) || CRGPU_COMPILE_METAL_API
#define NOALLOC_PRESENT 1

typedef struct presenting {
	gpu_frame f;
	crgpu_swap_chain swap_chain;
	uint32_t width, height;
	uint64_t frame;
	/* Of the timed runs: recording and the synced submit only, no acquire
	 * and no present, which wait; and the synced submit alone. */
	uint64_t recording_and_submit_ns;
	uint64_t submit_ns;
} presenting;

static bool present_frame(presenting *p) {
	const uint32_t index    = crgpu_swap_chain_acquire_next_image(p->swap_chain);
	crgpu_image_view view = crgpu_swap_chain_get_image_view(p->swap_chain, index);
	if (!view)
		return false;
	const uint64_t start = crperf_get_monotonic_time_ns();
	if (crgpu_command_allocator_reset(p->f.allocator) != CR_SUCCESS)
		return false;
	record_frame(p->f.cmd, view, p->width, p->height, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_PRESENT_SRC, p->frame++);
	const uint64_t submit_start = crperf_get_monotonic_time_ns();
	const CR_RESULT submitted  = crgpu_command_queue_submit_lists_synced(p->f.queue, &p->f.cmd, 1, p->swap_chain);
	const uint64_t end          = crperf_get_monotonic_time_ns();
	p->recording_and_submit_ns += end - start;
	p->submit_ns += end - submit_start;
	if (submitted != CR_SUCCESS)
		return false;
	crgpu_swap_chain_present(p->swap_chain);
	return true;
}

static void test_gpu_present(crgpu_instance instance, gpu_frame f, void *window) {
	presenting p;
	memset(&p, 0, sizeof(p));
	p.f                     = f;
	crgpu_surface surface = NULL;
	CHECK(crgpu_surface_create(instance, window, NULL, &surface) == CR_SUCCESS);
	if (!surface)
		return;
	crgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	desc.queue     = f.queue;
	desc.width     = TARGET_SIZE;
	desc.height    = TARGET_SIZE;
#if CRGPU_COMPILE_METAL_API
	desc.buffer_count = 1;
#else
	desc.buffer_count = 2;
#endif
	desc.format          = CRGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode    = CRGPU_PRESENT_MODE_FIFO;
	desc.present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE;
	desc.fullscreen_mode = CRGPU_FULLSCREEN_MODE_WINDOWED;
	CHECK(crgpu_swap_chain_create(f.device, &desc, &p.swap_chain) == CR_SUCCESS);
	if (p.swap_chain) {
		crgpu_swap_chain_desc created;
		memset(&created, 0, sizeof(created));
		created.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
		CHECK(crgpu_swap_chain_get_desc(p.swap_chain, &created) == CR_SUCCESS);
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
		report("Carrottus GPU's frame into a swap chain: acquire, record, synced submit, present");

		uint64_t best = UINT64_MAX, best_recording = UINT64_MAX, best_submit = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			p.recording_and_submit_ns = 0;
			p.submit_ns               = 0;
			const uint64_t start      = crperf_get_monotonic_time_ns();
			for (int i = 0; i < ITERATIONS && ok; ++i)
				ok = present_frame(&p);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
			if (p.recording_and_submit_ns < best_recording)
				best_recording = p.recording_and_submit_ns;
			if (p.submit_ns < best_submit)
				best_submit = p.submit_ns;
		}
		if (ok) {
			print_time("Carrottus GPU present frame, whole (FIFO: paced by the display)", best, ITERATIONS);
			print_time("Carrottus GPU present frame, reset + recording + synced submit only", best_recording, ITERATIONS);
			print_time("Carrottus GPU present frame, crgpu_command_queue_submit_lists_synced() alone", best_submit, ITERATIONS);
		}
		crgpu_command_queue_wait_idle(f.queue);
		crgpu_swap_chain_destroy(p.swap_chain);
	}
	crgpu_surface_destroy(surface);
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
	window_class.lpszClassName = L"cr_noalloc_tests";
	RegisterClassExW(&window_class);
	RECT rect = {0, 0, TARGET_SIZE, TARGET_SIZE};
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	return CreateWindowExW(0, L"cr_noalloc_tests", L"cr_noalloc_tests", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                       rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void hidden_window_destroy(HWND window) {
	DestroyWindow(window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"cr_noalloc_tests", GetModuleHandleW(NULL));
}
#endif

#if CRGPU_EXT_RAY_QUERY
static void test_gpu_ray_query(uint32_t adapter_index);
#endif

static void test_gpu(uint32_t adapter_index) {
	const CR_ALLOCATOR allocator = cr_allocator_system();
	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "cr_noalloc_tests";
	instance_desc.engine_name      = "cr_noalloc_tests";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus GPU: no instance, skipped\n");
		return;
	}
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	if (adapter_index >= adapter_count) {
		printf("cr_noalloc_tests: Carrottus GPU: no adapter %u (%u listed), skipped\n", adapter_index, adapter_count);
		crgpu_instance_destroy(instance);
		return;
	}
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	CHECK(crgpu_adapter_get_properties(adapters[adapter_index], &properties) == CR_SUCCESS);
	printf("cr_noalloc_tests: Carrottus GPU on adapter %u: %s\n", adapter_index, properties.description);

	const bool can_present = NOALLOC_PRESENT && (properties.supported_features & CRGPU_FEATURE_PRESENT) != 0;
	crgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.required_features  = can_present ? CRGPU_FEATURE_PRESENT : 0;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "cr_noalloc_tests";
	gpu_frame f;
	memset(&f, 0, sizeof(f));
	CHECK(crgpu_device_create(adapters[adapter_index], &device_desc, &f.device) == CR_SUCCESS);
	if (f.device) {
		CHECK(crgpu_command_queue_get(f.device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &f.queue) == CR_SUCCESS);
		crgpu_command_allocator_desc allocator_desc;
		memset(&allocator_desc, 0, sizeof(allocator_desc));
		allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
		allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_DIRECT;
		CHECK(crgpu_command_allocator_create(f.device, &allocator_desc, &f.allocator) == CR_SUCCESS);
		if (f.allocator)
			CHECK(crgpu_command_list_create(f.allocator, &f.cmd) == CR_SUCCESS);
		if (f.queue && f.cmd) {
			test_gpu_offscreen(f);
#if NOALLOC_PRESENT
			if (!can_present) {
				printf("cr_noalloc_tests: Carrottus GPU: the adapter can't present, the present loop skipped\n");
			} else {
#if defined(_WIN32)
				HWND window = hidden_window_create();
				CHECK(window != NULL);
				if (window) {
					test_gpu_present(instance, f, window);
					hidden_window_destroy(window);
				}
#else
				void *layer = crgpu_test_metal_layer_create(false);
				test_gpu_present(instance, f, layer);
				crgpu_test_metal_layer_destroy(layer);
#endif
			}
#else
			printf("cr_noalloc_tests: Carrottus GPU: no surface without a display here, the present loop skipped\n");
#endif
			crgpu_command_queue_wait_idle(f.queue);
		}
		crgpu_command_list_destroy(f.cmd);
		crgpu_command_allocator_destroy(f.allocator);
		crgpu_device_destroy(f.device);
	}
	crgpu_instance_destroy(instance);
#if CRGPU_EXT_RAY_QUERY
	test_gpu_ray_query(adapter_index);
#endif
}

#if CRGPU_EXT_RAY_QUERY
// ---- Carrottus GPU ray queries ---------------------------------------------------

/* What Carrottus takes from its allocator, counted: the bytes a structure's
 * creation allocates. */
typedef struct counting {
	CR_ALLOCATOR system;
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
	crgpu_fence fence;
	uint64_t frame;
	crgpu_buffer instances, scratch, hits;
	uint64_t top_scratch_offset;
	void *mapped_instances;
	uint32_t instance_size;
	crgpu_acceleration_structure bottom, top;
	uint64_t bottom_reference;
	crgpu_acceleration_structure_inputs top_inputs;
	crgpu_compute_pipeline trace;
	crgpu_descriptor_set set;
} rq_frame;

/* One frame: the instance moved and written by the CPU call into the mapped
 * instance buffer, the top level refitted in place, and the grid traced. */
static bool rq_frame_run(rq_frame *r) {
	if (r->frame > 0) {
		uint64_t value = r->frame;
		if (crgpu_fence_wait(r->f.device, &r->fence, &value, 1, true, UINT64_MAX) != CR_SUCCESS)
			return false;
	}
	crgpu_acceleration_structure_instance instance;
	memset(&instance, 0, sizeof(instance));
	instance.transform[0]           = 1.0f;
	instance.transform[3]           = (float)(r->frame % 8) * 0.125f;
	instance.transform[5]           = 1.0f;
	instance.transform[10]          = 1.0f;
	instance.transform[11]          = 2.0f;
	instance.user_index             = 7;
	instance.mask                   = 0xFF;
	instance.bottom_level_reference = r->bottom_reference;
	if (crgpu_device_write_acceleration_structure_instances(r->f.device, &instance, 1, r->mapped_instances) != CR_SUCCESS)
		return false;
	if (crgpu_buffer_flush(r->instances, 0, r->instance_size) != CR_SUCCESS)
		return false;
	if (crgpu_command_allocator_reset(r->f.allocator) != CR_SUCCESS)
		return false;
	crgpu_command_list cmd = r->f.cmd;
	crgpu_command_list_begin(cmd);
	crgpu_buffer_barrier b[2];
	memset(b, 0, sizeof(b));
	b[0].base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	b[0].buffer       = r->scratch;
	b[0].state_before = CRGPU_RESOURCE_STATE_COMMON;
	b[0].state_after  = CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH;
	b[1]              = b[0];
	b[1].buffer       = r->hits;
	b[1].state_after  = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	crgpu_command_list_pipeline_barrier(cmd, b, 2, NULL, 0);
	crgpu_acceleration_structure_barrier ab[2];
	memset(ab, 0, sizeof(ab));
	ab[0].base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
	ab[0].structure     = r->bottom;
	ab[0].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
	ab[0].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
	ab[1]               = ab[0];
	ab[1].structure     = r->top;
	crgpu_command_list_acceleration_structure_barrier(cmd, ab, 2);
	crgpu_acceleration_structure_build build;
	memset(&build, 0, sizeof(build));
	build.base.type   = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
	build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE;
	build.inputs      = &r->top_inputs;
	build.source      = r->top;
	build.destination = r->top;
	build.scratch     = r->scratch;
	build.scratch_offset = r->top_scratch_offset;
	if (crgpu_command_list_build_acceleration_structures(cmd, &build, 1) != CR_SUCCESS) {
		crgpu_command_list_end(cmd);
		return false;
	}
	ab[1].access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
	ab[1].access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE;
	crgpu_command_list_acceleration_structure_barrier(cmd, &ab[1], 1);
	crgpu_command_list_bind_compute_pipeline(cmd, r->trace);
	crgpu_command_list_bind_compute_descriptor_sets(cmd, r->trace, 0, &r->set, 1);
	const rq_params params = {{0.0f, 0.0f, -1.0f, 0.0f}, {1.5f / RQ_GRID, 1.5f / RQ_GRID, 100.0f, 0.0f}, RQ_GRID, RQ_GRID, 0xFF, 0};
	crgpu_command_list_push_compute_constants(cmd, r->trace, 0, sizeof(params), &params);
	crgpu_command_list_dispatch(cmd, RQ_GRID / 8, RQ_GRID / 8, 1);
	crgpu_command_list_end(cmd);
	++r->frame;
	crgpu_submit_desc submit;
	memset(&submit, 0, sizeof(submit));
	submit.base.type          = CR_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = &cmd;
	submit.cmd_list_count     = 1;
	submit.signal_fence       = r->fence;
	submit.signal_fence_value = r->frame;
	return crgpu_command_queue_submit(r->f.queue, &submit) == CR_SUCCESS;
}

static crgpu_buffer rq_buffer(crgpu_device device, CRGPU_MEMORY_KIND kind, CRGPU_BUFFER_USAGE usage, uint64_t size) {
	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = usage;
	desc.memory_kind      = kind;
	desc.size             = size;
	crgpu_buffer buffer = NULL;
	CHECK(crgpu_buffer_create(device, &desc, &buffer) == CR_SUCCESS);
	return buffer;
}

/* Carrottus GPU's ray query frame: a bottom level of one triangle and a top
 * level of one instance built once, then, per frame, the instance written
 * by the CPU call, the top level refitted and a 64 x 64 grid traced. */
static void test_gpu_ray_query(uint32_t adapter_index) {
	counting count;
	memset(&count, 0, sizeof(count));
	count.system             = cr_allocator_system();
	CR_ALLOCATOR allocator = {&count, counting_allocate, counting_free};
	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "cr_noalloc_tests";
	instance_desc.engine_name      = "cr_noalloc_tests";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS)
		return;
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	if (adapter_index >= adapter_count || crgpu_adapter_get_properties(adapters[adapter_index], &properties) != CR_SUCCESS ||
	    !(properties.supported_features & CRGPU_FEATURE_RAY_QUERY)) {
		printf("cr_noalloc_tests: Carrottus GPU: the adapter has no CRGPU_FEATURE_RAY_QUERY, the ray query frame skipped\n");
		crgpu_instance_destroy(instance);
		return;
	}
	rq_frame r;
	memset(&r, 0, sizeof(r));
	crgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.required_features  = CRGPU_FEATURE_RAY_QUERY;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 16;
	device_desc.max_samplers       = 4;
	device_desc.debug_name         = "cr_noalloc_tests ray queries";
	CHECK(crgpu_device_create(adapters[adapter_index], &device_desc, &r.f.device) == CR_SUCCESS);
	crgpu_device device = r.f.device;
	if (!device) {
		crgpu_instance_destroy(instance);
		return;
	}
	CHECK(crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &r.f.queue) == CR_SUCCESS);
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(crgpu_command_allocator_create(device, &allocator_desc, &r.f.allocator) == CR_SUCCESS);
	if (r.f.allocator)
		CHECK(crgpu_command_list_create(r.f.allocator, &r.f.cmd) == CR_SUCCESS);
	CHECK(crgpu_fence_create(device, CRGPU_FENCE_FLAG_NONE, 0, &r.fence) == CR_SUCCESS);
	crgpu_acceleration_structure_instance_layout layout;
	memset(&layout, 0, sizeof(layout));
	CHECK(crgpu_device_get_acceleration_structure_instance_layout(device, &layout) == CR_SUCCESS);
	r.instance_size = layout.size;

	const float vertices[9]          = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	crgpu_buffer vertex_buffer     = rq_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, sizeof(vertices));
	r.instances                      = rq_buffer(device, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, layout.size);
	r.hits                           = rq_buffer(device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, 32ull * RQ_GRID * RQ_GRID);
	crgpu_shader_module module     = NULL;
	crgpu_descriptor_set_layout set_layout = NULL;
	crgpu_descriptor_pool pool     = NULL;
	if (vertex_buffer) {
		void *mapped = NULL;
		if (crgpu_buffer_map(vertex_buffer, 0, sizeof(vertices), &mapped) == CR_SUCCESS) {
			memcpy(mapped, vertices, sizeof(vertices));
			crgpu_buffer_flush(vertex_buffer, 0, sizeof(vertices));
			crgpu_buffer_unmap(vertex_buffer);
		}
	}
	if (r.instances)
		CHECK(crgpu_buffer_map(r.instances, 0, layout.size, &r.mapped_instances) == CR_SUCCESS);

	crgpu_acceleration_structure_geometry geometry;
	memset(&geometry, 0, sizeof(geometry));
	geometry.kind        = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	geometry.flags       = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
	geometry.data        = vertex_buffer;
	geometry.data_stride = 12;
	geometry.data_count  = 3;
	crgpu_acceleration_structure_inputs bottom_inputs;
	memset(&bottom_inputs, 0, sizeof(bottom_inputs));
	bottom_inputs.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	bottom_inputs.level          = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	bottom_inputs.flags          = CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE;
	bottom_inputs.geometries     = &geometry;
	bottom_inputs.geometry_count = 1;
	crgpu_acceleration_structure_sizes bottom_sizes, top_sizes;
	memset(&bottom_sizes, 0, sizeof(bottom_sizes));
	bottom_sizes.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	top_sizes              = bottom_sizes;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(device, &bottom_inputs, &bottom_sizes) == CR_SUCCESS);

	// What creating a structure takes from the caller's allocator.
	crgpu_acceleration_structure_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	desc.level              = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	desc.size               = bottom_sizes.storage_size;
	size_t blocks           = count.blocks;
	size_t bytes            = count.bytes;
	CHECK(crgpu_acceleration_structure_create(device, &desc, &r.bottom) == CR_SUCCESS);
	printf("cr_noalloc_tests: Carrottus GPU: creating a bottom level took %zu block(s), %zu bytes, from the caller's allocator\n",
	       count.blocks - blocks, count.bytes - bytes);
	CHECK(crgpu_acceleration_structure_get_reference(r.bottom, &r.bottom_reference) == CR_SUCCESS);

	r.top_inputs.base.type          = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	r.top_inputs.level              = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	r.top_inputs.flags              = CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE | CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	r.top_inputs.instances          = r.instances;
	r.top_inputs.instance_count     = 1;
	r.top_inputs.bottom_levels      = &r.bottom;
	r.top_inputs.bottom_level_count = 1;
	if (r.bottom)
		CHECK(crgpu_device_get_acceleration_structure_build_sizes(device, &r.top_inputs, &top_sizes) == CR_SUCCESS);
	desc.level             = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	desc.size              = top_sizes.storage_size ? top_sizes.storage_size : 1;
	desc.max_bottom_levels = 1;
	blocks                 = count.blocks;
	bytes                  = count.bytes;
	CHECK(crgpu_acceleration_structure_create(device, &desc, &r.top) == CR_SUCCESS);
	printf("cr_noalloc_tests: Carrottus GPU: creating a top level of 1 bottom level took %zu block(s), %zu bytes\n", count.blocks - blocks,
	       count.bytes - bytes);
	// The bottom level's scratch, then the top level's at 64 KiB past it
	// (a multiple of every device's scratch alignment), for its build and
	// every update: no scratch range is used by two builds in one list.
	r.top_scratch_offset = (bottom_sizes.build_scratch_size + 65535u) / 65536u * 65536u;
	uint64_t top_scratch = top_sizes.build_scratch_size;
	if (top_sizes.update_scratch_size > top_scratch)
		top_scratch = top_sizes.update_scratch_size;
	r.scratch = rq_buffer(device, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, r.top_scratch_offset + top_scratch + 256);

	// The pipeline and its set.
	CRGPU_SHADER_FORMAT format = CRGPU_SHADER_FORMAT_NONE;
	for (CRGPU_SHADER_FORMAT f = CRGPU_SHADER_FORMAT_SPIRV; f <= CRGPU_SHADER_FORMAT_METALLIB; ++f)
		if (properties.shader_formats & (1u << f))
			format = f;
	crgpu_shader_module_desc module_desc = crgpu_test_module_desc(&crgpu_test_rq_trace, format, "rq_trace");
	if (module_desc.code)
		CHECK(crgpu_shader_module_create(device, &module_desc, &module) == CR_SUCCESS);
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type                   = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	layout_desc.bindings[0].binding         = 0;
	layout_desc.bindings[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	layout_desc.bindings[0].count           = 1;
	layout_desc.bindings[0].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
	layout_desc.bindings[1]                 = layout_desc.bindings[0];
	layout_desc.bindings[1].binding         = 1;
	layout_desc.bindings[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	layout_desc.binding_count               = 2;
	CHECK(crgpu_descriptor_set_layout_create(device, &layout_desc, &set_layout) == CR_SUCCESS);
	crgpu_compute_pipeline_desc pipeline_desc;
	memset(&pipeline_desc, 0, sizeof(pipeline_desc));
	pipeline_desc.base.type                           = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	pipeline_desc.compute_module                      = module;
	pipeline_desc.descriptor_set_layouts[0]           = set_layout;
	pipeline_desc.descriptor_set_layout_count         = 1;
	pipeline_desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_COMPUTE;
	pipeline_desc.push_constant_ranges[0].size        = sizeof(rq_params);
	pipeline_desc.push_constant_range_count           = 1;
	if (module && set_layout)
		CHECK(crgpu_compute_pipeline_create(device, &pipeline_desc, &r.trace) == CR_SUCCESS);
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets                      = 1;
	pool_desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
	pool_desc.pool_sizes[0].count           = 1;
	pool_desc.pool_sizes[1].descriptor_type = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_desc.pool_sizes[1].count           = 1;
	pool_desc.pool_size_count               = 2;
	CHECK(crgpu_descriptor_pool_create(device, &pool_desc, &pool) == CR_SUCCESS);
	if (pool && set_layout) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = pool;
		set_desc.set_layouts[0] = set_layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(device, &set_desc, &r.set) == CR_SUCCESS);
	}

	const bool ready = r.f.queue && r.f.cmd && r.fence && vertex_buffer && r.mapped_instances && r.hits && r.scratch && r.bottom && r.top &&
	                   r.trace && r.set;
	CHECK(ready);
	if (ready) {
		crgpu_write_descriptor_set writes[2];
		memset(writes, 0, sizeof(writes));
		writes[0].base.type               = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		writes[0].dst_set                 = r.set;
		writes[0].descriptor_count        = 1;
		writes[0].descriptor_type         = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		writes[0].acceleration_structures = &r.top;
		const crgpu_descriptor_buffer_info hits = {r.hits, 0, 32ull * RQ_GRID * RQ_GRID};
		writes[1].base.type                       = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		writes[1].dst_set                         = r.set;
		writes[1].dst_binding                     = 1;
		writes[1].descriptor_count                = 1;
		writes[1].descriptor_type                 = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].buffer_info                     = &hits;
		crgpu_device_update_descriptor_sets(device, writes, 2);

		// The one build of both levels, before the loop.
		crgpu_acceleration_structure_instance instance;
		memset(&instance, 0, sizeof(instance));
		instance.transform[0]           = 1.0f;
		instance.transform[5]           = 1.0f;
		instance.transform[10]          = 1.0f;
		instance.mask                   = 0xFF;
		instance.bottom_level_reference = r.bottom_reference;
		CHECK(crgpu_device_write_acceleration_structure_instances(device, &instance, 1, r.mapped_instances) == CR_SUCCESS);
		crgpu_buffer_flush(r.instances, 0, layout.size);
		crgpu_command_list_begin(r.f.cmd);
		crgpu_buffer_barrier b;
		memset(&b, 0, sizeof(b));
		b.base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
		b.buffer       = r.scratch;
		b.state_before = CRGPU_RESOURCE_STATE_COMMON;
		b.state_after  = CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH;
		crgpu_command_list_pipeline_barrier(r.f.cmd, &b, 1, NULL, 0);
		crgpu_acceleration_structure_build build;
		memset(&build, 0, sizeof(build));
		build.base.type   = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs      = &bottom_inputs;
		build.destination = r.bottom;
		build.scratch     = r.scratch;
		CHECK(crgpu_command_list_build_acceleration_structures(r.f.cmd, &build, 1) == CR_SUCCESS);
		crgpu_acceleration_structure_barrier ab;
		memset(&ab, 0, sizeof(ab));
		ab.base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
		ab.structure     = r.bottom;
		ab.access_before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE;
		ab.access_after  = CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ;
		crgpu_command_list_acceleration_structure_barrier(r.f.cmd, &ab, 1);
		build.inputs         = &r.top_inputs;
		build.destination    = r.top;
		build.scratch_offset = r.top_scratch_offset;
		CHECK(crgpu_command_list_build_acceleration_structures(r.f.cmd, &build, 1) == CR_SUCCESS);
		crgpu_command_list_end(r.f.cmd);
		CHECK(crgpu_command_queue_submit_lists(r.f.queue, &r.f.cmd, 1) == CR_SUCCESS);
		crgpu_command_queue_wait_idle(r.f.queue);

		bool ok = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = rq_frame_run(&r);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = rq_frame_run(&r);
		alloc_trace_stop();
		CHECK(ok);
		report("Carrottus GPU ray queries: the instance written, the top level refitted, a 64 x 64 grid traced");
		uint64_t best = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			const uint64_t start = crperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = rq_frame_run(&r);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
		}
		if (ok)
			print_time("Carrottus GPU ray query frame (write, refit, trace, fence wait)", best, TIMED_ITERATIONS);
		uint64_t value = r.frame;
		crgpu_fence_wait(device, &r.fence, &value, 1, true, UINT64_MAX);
	}
	if (r.mapped_instances)
		crgpu_buffer_unmap(r.instances);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_compute_pipeline_destroy(r.trace);
	crgpu_descriptor_set_layout_destroy(set_layout);
	crgpu_shader_module_destroy(module);
	crgpu_acceleration_structure_destroy(r.top);
	crgpu_acceleration_structure_destroy(r.bottom);
	crgpu_buffer_destroy(r.scratch);
	crgpu_buffer_destroy(r.hits);
	crgpu_buffer_destroy(r.instances);
	crgpu_buffer_destroy(vertex_buffer);
	crgpu_fence_destroy(r.fence);
	crgpu_command_list_destroy(r.f.cmd);
	crgpu_command_allocator_destroy(r.f.allocator);
	crgpu_device_destroy(device);
	crgpu_instance_destroy(instance);
}
#endif

// ---- Carrottus Audio -------------------------------------------------------------

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
	 * own share: the time Carrottus Audio's thread spends from the start of one
	 * call to the start of the next, less the time it waits, is not
	 * visible here. The callback's own time is. */
} silence;

static void silence_callback(craudio_stream stream, void *frames, uint32_t frame_count, const CRAUDIO_CALLBACK_INFO *info, void *user_data) {
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
#if CRAUDIO_COMPILE_COREAUDIO
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_COREAUDIO;
#elif CRAUDIO_COMPILE_WASAPI
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_WASAPI;
#elif CRAUDIO_COMPILE_ALSA
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_ALSA;
#elif CRAUDIO_COMPILE_REMOTEIO
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_REMOTEIO;
#else
	const CRAUDIO_NATIVE_API api = CRAUDIO_NATIVE_API_NONE;
#endif
	if (api == CRAUDIO_NATIVE_API_NONE) {
		printf("cr_noalloc_tests: Carrottus Audio: no native API in this build, skipped\n");
		return;
	}
	const CR_ALLOCATOR allocator = cr_allocator_system();
	CRAUDIO_INSTANCE_DESC instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type     = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	instance_desc.allocator     = &allocator;
	instance_desc.native_api    = api;
	craudio_instance instance = NULL;
	if (craudio_instance_create(&instance_desc, &instance) != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus Audio: no instance, skipped\n");
		return;
	}
	craudio_device *devices = NULL;
	uint32_t device_count     = 0;
	craudio_device_enumerate(instance, CRAUDIO_DIRECTION_OUTPUT, &devices, &device_count);
	CRAUDIO_DEVICE_PROPERTIES props;
	craudio_device device = NULL;
	for (uint32_t i = 0; i < device_count && !device; ++i) {
		memset(&props, 0, sizeof(props));
		props.base.type = CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		if (craudio_device_get_properties(devices[i], &props) == CR_SUCCESS && (props.default_roles & CRAUDIO_DEFAULT_ROLE_GENERAL))
			device = devices[i];
	}
	if (!device && device_count > 0) {
		device = devices[0];
		memset(&props, 0, sizeof(props));
		props.base.type = CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		craudio_device_get_properties(device, &props);
	}
	if (!device) {
		printf("cr_noalloc_tests: Carrottus Audio: no output device, skipped\n");
		craudio_instance_destroy(instance);
		return;
	}

	CRAUDIO_STREAM_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_DESC;
	desc.device    = device;
	if (props.native_format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN) {
		desc.format = props.native_format;
	} else {
		desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
		desc.format.sample_rate   = 48000;
		desc.format.channel_count = 2;
		desc.format.interleaved   = true;
	}
	desc.flags = CRAUDIO_STREAM_FLAG_NONE;
	/* Core Audio's and RemoteIO's callbacks run on the platform's own
	 * thread, the only mechanism they have; the others have a thread of
	 * Carrottus Audio's at the OS's default. */
#if CRAUDIO_COMPILE_COREAUDIO || CRAUDIO_COMPILE_REMOTEIO
	desc.thread.priority = CRAUDIO_THREAD_PRIORITY_PLATFORM;
#else
	desc.thread.priority = CRAUDIO_THREAD_PRIORITY_NORMAL;
#endif
	desc.callback        = silence_callback;
	CRAUDIO_TIMING_CAPS caps;
	memset(&caps, 0, sizeof(caps));
	caps.base.type = CR_STRUCT_TYPE_AUDIO_TIMING_CAPS;
	if (craudio_device_get_timing_caps(device, &desc.format, desc.flags, &caps) == CR_SUCCESS) {
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
		desc.buffer_frames = api == CRAUDIO_NATIVE_API_ALSA ? desc.period_frames * 2 : 0;
	}
	/* Each rejected field takes the probe's suggestion, a few rounds. */
	CRAUDIO_STREAM_CONFIG config;
	bool accepted = false;
	for (int round = 0; round < 8 && !accepted; ++round) {
		memset(&config, 0, sizeof(config));
		config.base.type   = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		const CR_RESULT r = craudio_stream_probe(&desc, &config);
		if (r == CR_SUCCESS)
			accepted = true;
		else if (r == CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED && config.format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN)
			desc.format = config.format;
		else if (r == CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE && config.period_frames)
			desc.period_frames = config.period_frames;
		else if (r == CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE)
			desc.buffer_frames = config.buffer_frames;
		else
			break;
	}
	if (!accepted) {
		printf("cr_noalloc_tests: Carrottus Audio: the device takes no stream this test asks for, skipped\n");
		craudio_instance_destroy(instance);
		return;
	}
	silence s;
	memset(&s, 0, sizeof(s));
	desc.user_data          = &s;
	craudio_stream stream = NULL;
	CHECK(craudio_stream_create(&desc, &stream) == CR_SUCCESS);
	if (stream) {
		memset(&config, 0, sizeof(config));
		config.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
		CHECK(craudio_stream_get_config(stream, &config) == CR_SUCCESS);
		s.channels     = config.format.channel_count;
		s.sample_bytes = craudio_sample_format_get_byte_size(config.format.sample_format);
		s.interleaved  = config.format.interleaved;
		const CR_RESULT started = craudio_stream_start(stream);
		if (started != CR_SUCCESS)
			printf("cr_noalloc_tests: Carrottus Audio: the stream didn't start: %s\n", cr_result_str(started));
		CHECK(started == CR_SUCCESS);
		/* The counted calls are a little over a second at a 10 ms period;
		 * ten seconds is plenty for any period a device takes. */
		for (int waited = 0; waited < 10000 && !s.counted; waited += 10)
			sleep_ms(10);
		CHECK(craudio_stream_stop(stream) == CR_SUCCESS);
		CHECK(s.counted);
		if (s.counted) {
			alloc_trace_attribute();
			char what[160];
			snprintf(what, sizeof(what), "Carrottus Audio's stream callback writing silence, %u-frame period, callbacks %d to %d",
			         config.period_frames, AUDIO_FIRST_COUNTED, AUDIO_FIRST_COUNTED + ITERATIONS);
			report(what);
		}
		craudio_stream_destroy(stream);
	}
	CHECK(craudio_instance_destroy(instance) == CR_SUCCESS);
}

// ---- Carrottus Net ---------------------------------------------------------------

#if CRNET_EXT_WEBSOCKET
static void test_net_websocket(crnet_instance instance, crnet_tcp_listener listener, uint16_t port);
#endif

typedef struct idle_connection {
	crnet_tcp_socket client, server;
	crnet_wait_set set;
} idle_connection;

static bool net_round(const idle_connection *c) {
	uint8_t buffer[64];
	uint64_t received = 0;
	crnet_wait_event events[4];
	uint32_t count = 0;
	return crnet_tcp_socket_recv(c->client, buffer, sizeof(buffer), CRNET_NO_WAIT, &received) == CR_RESULT_NET_TIMED_OUT &&
	       crnet_tcp_socket_recv(c->server, buffer, sizeof(buffer), CRNET_NO_WAIT, &received) == CR_RESULT_NET_TIMED_OUT &&
	       crnet_wait_set_wait(c->set, CRNET_NO_WAIT, events, 4, &count) == CR_RESULT_NET_TIMED_OUT;
}

static void test_net(void) {
	const CR_ALLOCATOR allocator = cr_allocator_system();
	crnet_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type   = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	instance_desc.allocator   = &allocator;
	crnet_instance instance = NULL;
	if (crnet_instance_create(&instance_desc, &instance, NULL) != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus Net: no instance, skipped\n");
		return;
	}
	crnet_tcp_listener_desc listen_desc;
	memset(&listen_desc, 0, sizeof(listen_desc));
	listen_desc.base.type         = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	listen_desc.address           = "127.0.0.1";
	listen_desc.backlog           = 1;
	crnet_tcp_listener listener = NULL;
	if (crnet_tcp_listener_create(instance, &listen_desc, &listener, NULL) != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus Net: no loopback listener, skipped\n");
		crnet_instance_destroy(instance);
		return;
	}
	uint16_t port = 0;
	CHECK(crnet_tcp_listener_get_port(listener, &port) == CR_SUCCESS);
	idle_connection c;
	memset(&c, 0, sizeof(c));
	CHECK(crnet_tcp_socket_create(instance, &c.client) == CR_SUCCESS);
	crnet_tcp_socket_connect_desc connect_desc;
	memset(&connect_desc, 0, sizeof(connect_desc));
	connect_desc.base.type  = CR_STRUCT_TYPE_NET_TCP_SOCKET_CONNECT_DESC;
	connect_desc.address    = "127.0.0.1";
	connect_desc.port       = port;
	connect_desc.timeout_ms = 5000;
	if (c.client)
		CHECK(crnet_tcp_socket_connect(c.client, &connect_desc) == CR_SUCCESS);
	CHECK(crnet_tcp_listener_accept(listener, 5000, &c.server) == CR_SUCCESS);
	CHECK(crnet_wait_set_create(instance, &c.set, NULL) == CR_SUCCESS);
	static int client_tag, server_tag;
	if (c.client && c.server && c.set) {
		CHECK(crnet_wait_set_add_tcp_socket(c.set, c.client, CRNET_WAIT_FOR_INCOMING, &client_tag) == CR_SUCCESS);
		CHECK(crnet_wait_set_add_tcp_socket(c.set, c.server, CRNET_WAIT_FOR_INCOMING, &server_tag) == CR_SUCCESS);
		bool ok = true;
		for (int i = 0; i < WARM_UP && ok; ++i)
			ok = net_round(&c);
		CHECK(ok);
		alloc_trace_start();
		for (int i = 0; i < ITERATIONS && ok; ++i)
			ok = net_round(&c);
		alloc_trace_stop();
		CHECK(ok);
		report("Carrottus Net's receives with CRNET_NO_WAIT on an idle loopback connection's two ends and a wait set's poll");
		uint64_t best = UINT64_MAX;
		for (int run = 0; run < TIMED_RUNS && ok; ++run) {
			const uint64_t start = crperf_get_monotonic_time_ns();
			for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
				ok = net_round(&c);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			if (took < best)
				best = took;
		}
		if (ok)
			print_time("Carrottus Net idle round (two receives, a wait set poll)", best, TIMED_ITERATIONS);
	}
#if CRNET_EXT_WEBSOCKET
	test_net_websocket(instance, listener, port);
#endif
	crnet_wait_set_destroy(c.set);
	crnet_tcp_socket_destroy(c.server);
	crnet_tcp_socket_destroy(c.client);
	crnet_tcp_listener_destroy(listener);
	crnet_instance_destroy(instance);
}

// ---- Carrottus Net: a WebSocket ---------------------------------------------

#if CRNET_EXT_WEBSOCKET

/* SHA-1 and base64, for the one thing the server here computes: the
 * opening's Sec-WebSocket-Accept (RFC 6455). */
static void ws_sha1(const uint8_t *data, size_t size, uint8_t out[20]) {
	uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	uint8_t  block[64];
	uint64_t bits  = (uint64_t)size * 8;
	size_t   total = ((size + 8) / 64 + 1) * 64;
	for (size_t at = 0; at < total; at += 64) {
		for (size_t i = 0; i < 64; ++i) {
			size_t n = at + i;
			if (n < size)
				block[i] = data[n];
			else if (n == size)
				block[i] = 0x80;
			else if (n >= total - 8)
				block[i] = (uint8_t)(bits >> (8 * (total - 1 - n)));
			else
				block[i] = 0;
		}
		uint32_t w[80];
		for (int i = 0; i < 16; ++i)
			w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
		for (int i = 16; i < 80; ++i) {
			uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
			w[i]       = x << 1 | x >> 31;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; ++i) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCu;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6u;
			}
			uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
			e          = d;
			d          = c;
			c          = b << 30 | b >> 2;
			b          = a;
			a          = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (int i = 0; i < 20; ++i)
		out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static void ws_base64(const uint8_t *data, size_t size, char *out) {
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t            o        = 0;
	for (size_t i = 0; i < size; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < size ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
		out[o++]   = digits[v >> 18 & 63];
		out[o++]   = digits[v >> 12 & 63];
		out[o++]   = i + 1 < size ? digits[v >> 6 & 63] : '=';
		out[o++]   = i + 2 < size ? digits[v & 63] : '=';
	}
	out[o] = '\0';
}

/* The server's half of the opening, on an accepted socket: reads the
 * request and answers 101. */
static bool ws_server_open(crnet_tcp_socket server) {
	char     request[4096];
	size_t   size = 0;
	uint64_t got  = 0;
	while (size < sizeof(request) - 1) {
		if (crnet_tcp_socket_recv(server, request + size, sizeof(request) - 1 - size, 5000, &got) != CR_SUCCESS || got == 0)
			return false;
		size += (size_t)got;
		request[size] = '\0';
		if (strstr(request, "\r\n\r\n"))
			break;
	}
	const char *key = strstr(request, "Sec-WebSocket-Key:");
	if (!key)
		key = strstr(request, "sec-websocket-key:");
	if (!key)
		return false;
	key += 18;
	while (*key == ' ')
		key++;
	char   joined[128];
	size_t length = strcspn(key, "\r\n");
	if (length > 60)
		return false;
	snprintf(joined, sizeof(joined), "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)length, key);
	uint8_t digest[20];
	char    accept[32];
	ws_sha1((const uint8_t *)joined, strlen(joined), digest);
	ws_base64(digest, sizeof(digest), accept);
	char answer[256];
	int  answer_size = snprintf(answer, sizeof(answer), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
	uint64_t sent    = 0;
	return crnet_tcp_socket_send(server, answer, (uint64_t)answer_size, 5000, &sent) == CR_SUCCESS && sent == (uint64_t)answer_size;
}

typedef struct websocket_round {
	crnet_websocket  client;
	crnet_tcp_socket server;
} websocket_round;

static bool ws_round(const websocket_round *w) {
	static const uint8_t message[32] = {1, 2, 3, 4};
	static uint8_t       drained[4096];
	uint64_t             got = 0;
	CR_RESULT            result;
	while ((result = crnet_tcp_socket_recv(w->server, drained, sizeof(drained), CRNET_NO_WAIT, &got)) == CR_SUCCESS && got > 0) {
	}
	if (result != CR_RESULT_NET_TIMED_OUT)
		return false;
	result = crnet_websocket_send(w->client, CRNET_WEBSOCKET_MESSAGE_BINARY, message, sizeof(message), CRNET_NO_WAIT);
	if (result != CR_SUCCESS && result != CR_RESULT_NET_TIMED_OUT)
		return false;
	result = crnet_websocket_flush(w->client, CRNET_NO_WAIT);
	return result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT;
}

static void test_net_websocket(crnet_instance instance, crnet_tcp_listener listener, uint16_t port) {
	websocket_round w;
	memset(&w, 0, sizeof(w));
	if (crnet_websocket_create(instance, &w.client) != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus Net: no WebSocket on this stack, skipped\n");
		return;
	}
	char url[64];
	snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", (unsigned)port);
	crnet_websocket_connect_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC;
	desc.url              = url;
	desc.timeout_ms       = CRNET_NO_WAIT;
	desc.max_message_size = 1024;
	desc.proxy.mode       = CRNET_PROXY_NONE;
	uint32_t  status      = 0;
	CR_RESULT result      = crnet_websocket_connect(w.client, &desc, &status);
	bool      ok          = result == CR_SUCCESS || result == CR_RESULT_NET_TIMED_OUT;
	ok                    = ok && crnet_tcp_listener_accept(listener, 5000, &w.server) == CR_SUCCESS && ws_server_open(w.server);
	if (ok && result == CR_RESULT_NET_TIMED_OUT)
		result = crnet_websocket_connect_continue(w.client, 5000, &status);
	ok = ok && result == CR_SUCCESS;
	CHECK(ok);
	for (int i = 0; i < WARM_UP && ok; ++i)
		ok = ws_round(&w);
	CHECK(ok);
	alloc_trace_start();
	for (int i = 0; i < ITERATIONS && ok; ++i)
		ok = ws_round(&w);
	alloc_trace_stop();
	CHECK(ok);
	if (ok)
		report("Carrottus Net's WebSocket sends and flushes with CRNET_NO_WAIT, a message a round, the server's end drained");
	crnet_tcp_socket_destroy(w.server);
	crnet_websocket_destroy(w.client);
}

#endif // CRNET_EXT_WEBSOCKET

// ---- Carrottus Input -------------------------------------------------------------

static void test_input(void) {
#if CRINPUT_COMPILE_GAMECONTROLLER
	const CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_GAMECONTROLLER;
#elif CRINPUT_COMPILE_GAMEINPUT
	const CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_GAMEINPUT;
#elif CRINPUT_COMPILE_XINPUT
	const CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_XINPUT;
#elif CRINPUT_COMPILE_EVDEV
	const CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_EVDEV;
#else
	const CRINPUT_NATIVE_API api = CRINPUT_NATIVE_API_NONE;
#endif
	if (api == CRINPUT_NATIVE_API_NONE) {
		printf("cr_noalloc_tests: Carrottus Input: no native API in this build, skipped\n");
		return;
	}
	const CR_ALLOCATOR allocator = cr_allocator_system();
	CRINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type  = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.allocator  = &allocator;
	desc.native_api = api;
#if CRINPUT_COMPILE_XINPUT || CRINPUT_COMPILE_EVDEV
	desc.read_in_background = true; /* these always read */
#else
	desc.read_in_background = false;
#endif
	desc.motion_sample_capacity = 0;
	crinput_instance instance = NULL;
	const CR_RESULT created    = crinput_instance_create(&desc, &instance);
	if (created != CR_SUCCESS) {
		printf("cr_noalloc_tests: Carrottus Input: no instance (%s), skipped\n", cr_result_str(created));
		return;
	}
	bool ok = true;
	for (int i = 0; i < WARM_UP && ok; ++i)
		ok = crinput_instance_poll(instance) == CR_SUCCESS;
	CHECK(ok);
	const crinput_gamepad_id *ids = NULL;
	uint32_t count                  = 0;
	crinput_instance_get_gamepads(instance, &ids, &count);
	alloc_trace_start();
	for (int i = 0; i < ITERATIONS && ok; ++i)
		ok = crinput_instance_poll(instance) == CR_SUCCESS;
	alloc_trace_stop();
	CHECK(ok);
	char what[128];
	snprintf(what, sizeof(what), "Carrottus Input's poll, %u gamepad(s) connected", count);
	report(what);
	uint64_t best = UINT64_MAX;
	for (int run = 0; run < TIMED_RUNS && ok; ++run) {
		const uint64_t start = crperf_get_monotonic_time_ns();
		for (int i = 0; i < TIMED_ITERATIONS && ok; ++i)
			ok = crinput_instance_poll(instance) == CR_SUCCESS;
		const uint64_t took = crperf_get_monotonic_time_ns() - start;
		if (took < best)
			best = took;
	}
	if (ok)
		print_time("Carrottus Input poll", best, TIMED_ITERATIONS);
	crinput_instance_destroy(instance);
}

int main(int argc, char **argv) {
	uint32_t adapter = 0;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
			adapter = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: cr_noalloc_tests [--adapter N]\n");
			return 2;
		}
	}
	if (!alloc_trace_available()) {
		printf("cr_noalloc_tests: no allocation hook on this platform or build (a sanitizer's heap): nothing measured\n");
		return 0;
	}
	test_hook_counts();
	test_gpu(adapter);
	test_audio();
	test_net();
	test_input();
	if (failures) {
		printf("cr_noalloc_tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("cr_noalloc_tests: no allocation of ours in any hot path\n");
	return 0;
}
