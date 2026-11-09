#!/usr/bin/env python3
"""Writes tests/spud_misuse_tests.c from abi.txt, in two halves.

The zeroed half: a call to every public function of SpudLib with every
argument zeroed - NULL handles and pointers, zero sizes and counts, zeroed
structs (ROADMAP.md in the workspace, section 3.1, "a misuse harness from the
headers"). None may crash, and one that returns SPUDRESULT must not return
SPUD_SUCCESS, unless the list below says why zeroes are a valid call. A
function that returns nothing can't say it refused: the test counts those.

The freed half (decision X16, src/spudstale.h): every public function whose
first parameter is a handle, called with a handle that has just been
destroyed and every other argument zeroed. None may crash; one that returns
SPUDRESULT must not return SPUD_SUCCESS (or, where zeroes are a valid call,
must return what it returns for NULL), and any other getter must return what
it returns for NULL, its null fallback. A function that returns nothing is
called and counted. Each handle type's destroyed handle comes from its
factory in FACTORIES below, written by hand: C that makes one through the
public API with the smallest valid desc and destroys it. A handle type with
no factory is listed in the output as not covered, with the reason. The
freed half runs in a Debug build only (the check is Debug-only) and never
under a sanitizer, which reports the read of freed memory the check makes;
the test prints which it skipped and why.

Each library that has one keeps its own copy of this generator, the same
but for the configuration at the top.

Run after ./check.sh --update-abi changes spudlib/abi.txt, or after a change
to the configuration:
    python3 tests/misuse/make_misuse_tests.py
The output is checked in, so a build needs no Python."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LIBRARY = 'SpudLib'
RESULT = 'SPUDRESULT'
OK = 'SPUD_SUCCESS'
OUTPUT = 'spud_misuse_tests'
HEADERS = ['spudaudio.h', 'spudcore.h', 'spudfiles.h', 'spudgpu.h', 'spudinput.h', 'spudmemory.h', 'spudnet.h', 'spudperf.h', 'spudrandom.h']

# Function name prefixes called only when a macro is set, each with its
# macro (a part of the library a build may leave out) and the header that
# declares them, included under the same macro.
GUARDED = {
}

# Functions zeroes are a valid call for, each with the reason.
ZEROES_SUCCEED = {
    'spudgpu_destroy_instance': 'destroying NULL succeeds, as spudgpu.h says',
    'spudrandom_fill': 'asking for no bytes succeeds, as spudrandom.h says',
}

# Functions not called: zeroes are outside their contract in a way a
# NULL check can't catch, each with the reason.
NOT_CALLED = {
}

# What a platform tag in abi.txt is in the preprocessor.
PLATFORMS = {
    'apple': 'defined(__APPLE__)',
    'linux': 'defined(__linux__)',
    'windows': 'defined(_WIN32)',
}

# ---- The freed half ----------------------------------------------------------

# What a build defines for a Debug build: the freed half runs only there.
DEBUG_MACRO = '_DEBUG'

# Functions not called with a destroyed handle, each with the reason.
FREED_NOT_CALLED = {
    'spudnet_resolver_run': 'may start after the resolver is destroyed (spudnet.h), so it has no stale check, and '
                            'a resolver that has been run and destroyed is freed memory',
}

# Handle types no public function takes first, so no factory is needed.
# Listed so that a new function taking one first is noticed: its type then
# needs a factory.
NO_FIRST_PARAMETER = ['spudgpu_descriptor_set', 'spudgpu_pipeline_layout']

# Included by the freed half, beside HEADERS.
FREED_INCLUDES = ['spudgpu_test_shaders.h', '<stdlib.h>', '<string.h>']

# The C before the factories: what they share, and freed_setup() and
# freed_teardown(), which the freed half calls first and last. `why` is the
# reason the last factory gave no handle.
FREED_PRELUDE = r'''
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

static const char *why = "";

/* The allocator every object here is made with: the system's, except that
 * a freed block is kept, not given back, until freed_teardown(). A
 * destroyed handle's memory then stays readable, with SPUD_DEAD in its tag,
 * whatever the system's allocator does with memory it is given back (the
 * Windows heap decommits freed pages, and reading one faults), and no new
 * object can be made in it. Locked, for an object freed on a thread of the
 * platform's. Since card X27 a Debug build keeps a destroyed handle's
 * block itself until the handle's owner is destroyed (src/spudstale.h), but
 * not every handle has an owner: an instance, a SpudAudio stream and a
 * SpudNet resolver are freed at once, so this stays. */
typedef struct kept_block {
	struct kept_block *next;
	void *memory;
	size_t size, alignment;
} kept_block;

static SPUD_ALLOCATOR system_allocator;
static SPUD_ALLOCATOR allocator;
static kept_block *kept;
#if defined(_WIN32)
static SRWLOCK kept_lock = SRWLOCK_INIT;
#define KEPT_LOCK() AcquireSRWLockExclusive(&kept_lock)
#define KEPT_UNLOCK() ReleaseSRWLockExclusive(&kept_lock)
#else
static pthread_mutex_t kept_lock = PTHREAD_MUTEX_INITIALIZER;
#define KEPT_LOCK() pthread_mutex_lock(&kept_lock)
#define KEPT_UNLOCK() pthread_mutex_unlock(&kept_lock)
#endif

static void *keeping_allocate(void *user_data, size_t size, size_t alignment) {
	(void)user_data;
	return system_allocator.allocate(system_allocator.user_data, size, alignment);
}

static void keeping_free(void *user_data, void *memory, size_t size, size_t alignment) {
	(void)user_data;
	kept_block *block = (kept_block *)malloc(sizeof(kept_block));
	if (!block) {
		system_allocator.free(system_allocator.user_data, memory, size, alignment);
		return;
	}
	block->memory    = memory;
	block->size      = size;
	block->alignment = alignment;
	KEPT_LOCK();
	block->next = kept;
	kept        = block;
	KEPT_UNLOCK();
}

static void keeping_release_all(void) {
	KEPT_LOCK();
	kept_block *block = kept;
	kept              = NULL;
	KEPT_UNLOCK();
	while (block) {
		kept_block *next = block->next;
		system_allocator.free(system_allocator.user_data, block->memory, block->size, block->alignment);
		free(block);
		block = next;
	}
}

/* SpudGPU's: one instance and a device on adapter 0, which every object a
 * factory makes of a device comes from, and a hidden window on Windows for
 * the surface and the swap chain. */
static spudgpu_instance gpu_instance;
static spudgpu_device gpu_device;
static spudgpu_command_queue gpu_queue;
static SPUDGPU_FEATURES gpu_features;
static SPUDGPU_SHADER_FORMAT gpu_shader_format;
#if defined(_WIN32)
static HWND gpu_window;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
	return DefWindowProcW(window, message, w, l);
}
#endif

static spudgpu_instance make_gpu_instance(void) {
	spudgpu_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_INSTANCE_DESC;
	desc.allocator          = &allocator;
	desc.application_name   = "spud_misuse_tests";
	desc.engine_name        = "spud_misuse_tests";
	spudgpu_instance result = NULL;
	if (spudgpu_create_instance(&desc, &result) != SPUD_SUCCESS)
		return NULL;
	return result;
}

/* A device on [instance]'s adapter 0 with [features], or NULL. */
static spudgpu_device make_gpu_device(spudgpu_instance instance, SPUDGPU_FEATURES features) {
	const spudgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	if (spudgpu_enumerate_adapters(instance, &adapters, &count) != SPUD_SUCCESS || count == 0)
		return NULL;
	spudgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = features;
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	spudgpu_device device   = NULL;
	if (spudgpu_create_device(adapters[0], &desc, &device) != SPUD_SUCCESS)
		return NULL;
	return device;
}

static void freed_setup(void) {
	system_allocator   = spud_allocator_system();
	allocator          = system_allocator;
	allocator.allocate = keeping_allocate;
	allocator.free     = keeping_free;
	gpu_instance       = make_gpu_instance();
	if (gpu_instance) {
		const spudgpu_adapter *adapters = NULL;
		uint32_t count                  = 0;
		spudgpu_enumerate_adapters(gpu_instance, &adapters, &count);
		if (count > 0) {
			SPUDGPU_ADAPTER_PROPERTIES properties;
			memset(&properties, 0, sizeof(properties));
			properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
			if (spudgpu_get_adapter_properties(adapters[0], &properties) == SPUD_SUCCESS) {
				gpu_features      = properties.supported_features & (SPUDGPU_FEATURE_PRESENT | SPUDGPU_FEATURE_PASS_TIMESTAMPS);
				gpu_shader_format = spudgpu_test_shader_format(properties.shader_formats);
			}
		}
		gpu_device = make_gpu_device(gpu_instance, gpu_features);
		if (gpu_device)
			spudgpu_get_command_queue(gpu_device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &gpu_queue);
	}
#if defined(_WIN32)
	WNDCLASSEXW window_class;
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize        = sizeof(window_class);
	window_class.lpfnWndProc   = window_proc;
	window_class.hInstance     = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"spud_misuse_tests";
	RegisterClassExW(&window_class);
	gpu_window = CreateWindowExW(0, L"spud_misuse_tests", L"spud_misuse_tests", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, NULL, NULL,
	                             GetModuleHandleW(NULL), NULL);
#endif
}

static void freed_teardown(void) {
	spudgpu_destroy_device(gpu_device);
	spudgpu_destroy_instance(gpu_instance);
#if defined(_WIN32)
	if (gpu_window)
		DestroyWindow(gpu_window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"spud_misuse_tests", GetModuleHandleW(NULL));
#endif
	keeping_release_all();
}

static bool have_device(void) {
	if (!gpu_device)
		why = "no SpudGPU device on adapter 0";
	return gpu_device != NULL;
}

static spudgpu_buffer make_buffer(void) {
	spudgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = SPUDGPU_BUFFER_USAGE_UNIFORM;
	desc.memory_kind      = SPUDGPU_MEMORY_KIND_UPLOAD;
	desc.size             = 256;
	spudgpu_buffer buffer = NULL;
	if (spudgpu_create_buffer(gpu_device, &desc, &buffer) != SPUD_SUCCESS)
		return NULL;
	return buffer;
}

static spudgpu_image make_image(void) {
	spudgpu_image_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_IMAGE_DESC;
	desc.usage              = SPUDGPU_IMAGE_USAGE_SAMPLED;
	desc.type               = SPUDGPU_IMAGE_TYPE_2D;
	desc.format             = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	desc.clear_value.format = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
	desc.width              = 4;
	desc.height             = 4;
	desc.depth              = 1;
	desc.array_layers       = 1;
	desc.mip_levels         = 1;
	spudgpu_image image     = NULL;
	if (spudgpu_create_image(gpu_device, &desc, &image) != SPUD_SUCCESS)
		return NULL;
	return image;
}

static spudgpu_shader_module make_module(const spudgpu_test_shader *shader) {
	const spudgpu_shader_module_desc desc = spudgpu_test_module_desc(shader, gpu_shader_format, NULL);
	spudgpu_shader_module module          = NULL;
	if (spudgpu_create_shader_module(gpu_device, &desc, &module) != SPUD_SUCCESS)
		return NULL;
	return module;
}

/* [count] bindings of [type], 0 up, seen by [stages]. */
static spudgpu_descriptor_set_layout make_set_layout(const SPUDGPU_DESCRIPTOR_TYPE *types, uint32_t count, SPUDGPU_SHADER_STAGE stages) {
	spudgpu_descriptor_set_layout_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < count; ++i) {
		desc.bindings[i].binding         = i;
		desc.bindings[i].descriptor_type = types[i];
		desc.bindings[i].count           = 1;
		desc.bindings[i].stage_flags     = stages;
	}
	desc.binding_count                   = count;
	spudgpu_descriptor_set_layout layout = NULL;
	if (spudgpu_create_descriptor_set_layout(gpu_device, &desc, &layout) != SPUD_SUCCESS)
		return NULL;
	return layout;
}

static spudgpu_command_allocator make_command_allocator(void) {
	spudgpu_command_allocator_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                      = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	desc.type                           = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	spudgpu_command_allocator allocator = NULL;
	if (spudgpu_create_command_allocator(gpu_device, &desc, &allocator) != SPUD_SUCCESS)
		return NULL;
	return allocator;
}

static spudgpu_surface make_surface(void) {
#if defined(_WIN32)
	if (!gpu_window) {
		why = "no window";
		return NULL;
	}
	spudgpu_surface surface = NULL;
	if (spudgpu_create_surface(gpu_instance, gpu_window, NULL, &surface) != SPUD_SUCCESS) {
		why = "spudgpu_create_surface failed";
		return NULL;
	}
	return surface;
#else
	why = "a surface needs a window, which this test makes on Windows only";
	return NULL;
#endif
}

static spudaudio_instance make_audio_instance(void) {
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
	SPUDAUDIO_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type              = SPUD_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	desc.allocator              = &allocator;
	desc.native_api             = api;
	spudaudio_instance instance = NULL;
	if (api == SPUDAUDIO_NATIVE_API_NONE || spudaudio_create_instance(&desc, &instance) != SPUD_SUCCESS) {
		why = "no SpudAudio instance in this build";
		return NULL;
	}
	return instance;
}

/* [instance]'s first output device, or NULL. */
static spudaudio_device first_output(spudaudio_instance instance) {
	spudaudio_device *devices = NULL;
	uint32_t count            = 0;
	if (spudaudio_enumerate_devices(instance, SPUDAUDIO_DIRECTION_OUTPUT, &devices, &count) != SPUD_SUCCESS || count == 0) {
		why = "no audio output device";
		return NULL;
	}
	return devices[0];
}

static void silent_callback(spudaudio_stream stream, void *frames, uint32_t frame_count, const SPUDAUDIO_CALLBACK_INFO *info, void *user_data) {
	(void)stream;
	(void)frames;
	(void)frame_count;
	(void)info;
	(void)user_data;
}

static spudfile_instance make_file_instance(void) {
	spudfile_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type             = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator             = &allocator;
	spudfile_instance instance = NULL;
	if (spudfile_instance_create(&desc, &instance) != SPUD_SUCCESS) {
		why = "spudfile_instance_create failed";
		return NULL;
	}
	return instance;
}

#define FREED_FILE "spud_misuse_tests.tmp"

/* A file of 4096 bytes, made in the working directory, or NULL. */
static spudfile_file make_file(spudfile_instance instance) {
	spudfile_open_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type     = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	desc.path          = FREED_FILE;
	desc.access        = SPUDFILE_ACCESS_READ_WRITE;
	desc.create        = SPUDFILE_CREATE_TRUNCATE;
	desc.share         = SPUDFILE_SHARE_READ;
	spudfile_file file = NULL;
	if (spudfile_file_open(instance, &desc, &file) != SPUD_SUCCESS) {
		why = "no file could be made in the working directory";
		return NULL;
	}
	if (spudfile_file_set_size(file, 4096) != SPUD_SUCCESS) {
		why = "spudfile_file_set_size failed";
		spudfile_file_close(file);
		return NULL;
	}
	return file;
}

static spudnet_instance make_net_instance(void) {
	spudnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type            = SPUD_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator            = &allocator;
	spudnet_instance instance = NULL;
	if (spudnet_instance_create(&desc, &instance, NULL) != SPUD_SUCCESS) {
		why = "spudnet_instance_create failed";
		return NULL;
	}
	return instance;
}

static spudnet_http_client make_http_client(spudnet_instance instance) {
	spudnet_http_client_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type             = SPUD_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode            = SPUDNET_PROXY_NONE;
	spudnet_http_client client = NULL;
	if (spudnet_http_client_create(instance, &desc, &client) != SPUD_SUCCESS) {
		why = "spudnet_http_client_create failed";
		return NULL;
	}
	return client;
}
'''

# For each handle type, the body of a C function returning that type: a
# handle made through the public API and destroyed, or NULL with `why` set.
FACTORIES = {
    # ---- SpudAudio
    'spudaudio_instance': r'''
	spudaudio_instance instance = make_audio_instance();
	if (instance)
		spudaudio_destroy_instance(instance);
	return instance;''',
    'spudaudio_device': r'''
	/* A device is the instance's: it goes with it. */
	spudaudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	spudaudio_device device = first_output(instance);
	spudaudio_destroy_instance(instance);
	return device;''',
    'spudaudio_stream': r'''
	spudaudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	spudaudio_device device = first_output(instance);
	spudaudio_stream stream = NULL;
	if (device) {
		SPUDAUDIO_DEVICE_PROPERTIES properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = SPUD_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		spudaudio_get_device_properties(device, &properties);
		SPUDAUDIO_STREAM_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type = SPUD_STRUCT_TYPE_AUDIO_STREAM_DESC;
		desc.device    = device;
		desc.format    = properties.native_format;
		if (desc.format.sample_format == SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN) {
			desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
			desc.format.sample_rate   = 48000;
			desc.format.channel_count = 2;
			desc.format.interleaved   = true;
		}
		/* The platform's own thread where it is the only mechanism. */
#if SPUDAUDIO_COMPILE_COREAUDIO || SPUDAUDIO_COMPILE_REMOTEIO
		desc.thread.priority = SPUDAUDIO_THREAD_PRIORITY_PLATFORM;
#else
		desc.thread.priority = SPUDAUDIO_THREAD_PRIORITY_NORMAL;
#endif
		desc.callback        = silent_callback;
		desc.period_frames   = desc.format.sample_rate / 100;
		/* Each rejected field takes the probe's suggestion, a few rounds. */
		for (int round = 0; round < 8; ++round) {
			SPUDAUDIO_STREAM_CONFIG config;
			memset(&config, 0, sizeof(config));
			config.base.type   = SPUD_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
			const SPUDRESULT r = spudaudio_probe_stream(&desc, &config);
			if (r == SPUD_SUCCESS)
				break;
			if (r == SPUDRESULT_SAUD_FORMAT_NOT_SUPPORTED && config.format.sample_format != SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN)
				desc.format = config.format;
			else if (r == SPUDRESULT_SAUD_PERIOD_OUT_OF_RANGE && config.period_frames)
				desc.period_frames = config.period_frames;
			else if (r == SPUDRESULT_SAUD_BUFFER_OUT_OF_RANGE)
				desc.buffer_frames = config.buffer_frames;
			else
				break;
		}
		if (spudaudio_create_stream(&desc, &stream) == SPUD_SUCCESS)
			spudaudio_destroy_stream(stream);
		else
			why = "the output device took no stream";
	}
	spudaudio_destroy_instance(instance);
	return stream;''',
    # ---- SpudFiles
    'spudfile_instance': r'''
	spudfile_instance instance = make_file_instance();
	if (instance)
		spudfile_instance_destroy(instance);
	return instance;''',
    'spudfile_file': r'''
	spudfile_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	spudfile_file file = make_file(instance);
	if (file)
		spudfile_file_close(file);
	spudfile_path_remove(instance, FREED_FILE);
	spudfile_instance_destroy(instance);
	return file;''',
    'spudfile_mapping': r'''
	spudfile_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	spudfile_file file       = make_file(instance);
	spudfile_mapping mapping = NULL;
	if (file) {
		if (spudfile_file_map(file, 0, 4096, &mapping) == SPUD_SUCCESS)
			spudfile_mapping_release(mapping);
		else
			why = "spudfile_file_map failed";
		spudfile_file_close(file);
	}
	spudfile_path_remove(instance, FREED_FILE);
	spudfile_instance_destroy(instance);
	return mapping;''',
    'spudfile_listing': r'''
	spudfile_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	spudfile_listing_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type           = SPUD_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.path                = ".";
	spudfile_listing listing = NULL;
	if (spudfile_listing_open(instance, &desc, &listing) == SPUD_SUCCESS)
		spudfile_listing_close(listing);
	else
		why = "spudfile_listing_open failed on the working directory";
	spudfile_instance_destroy(instance);
	return listing;''',
    # ---- SpudGPU
    'spudgpu_instance': r'''
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_destroy_instance(instance);
	return instance;''',
    'spudgpu_adapter': r'''
	/* An adapter is the instance's: it goes with it. */
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	const spudgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	spudgpu_enumerate_adapters(instance, &adapters, &count);
	spudgpu_adapter adapter = count > 0 ? adapters[0] : NULL;
	if (!adapter)
		why = "no adapter";
	spudgpu_destroy_instance(instance);
	return adapter;''',
    'spudgpu_device': r'''
	/* Its own instance: the shared one's adapter 0 already has a device. */
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_device device = make_gpu_device(instance, 0);
	if (device)
		spudgpu_destroy_device(device);
	else
		why = "no device on adapter 0";
	spudgpu_destroy_instance(instance);
	return device;''',
    'spudgpu_command_queue': r'''
	/* A queue is the device's: it goes with it. */
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_device device       = make_gpu_device(instance, 0);
	spudgpu_command_queue queue = NULL;
	if (device) {
		spudgpu_get_command_queue(device, SPUDGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue);
		spudgpu_destroy_device(device);
	}
	if (!queue)
		why = "no device on adapter 0";
	spudgpu_destroy_instance(instance);
	return queue;''',
    'spudgpu_buffer': r'''
	if (!have_device())
		return NULL;
	spudgpu_buffer buffer = make_buffer();
	if (buffer)
		spudgpu_destroy_buffer(buffer);
	else
		why = "spudgpu_create_buffer failed";
	return buffer;''',
    'spudgpu_buffer_view': r'''
	if (!have_device())
		return NULL;
	spudgpu_buffer buffer = make_buffer();
	if (!buffer) {
		why = "spudgpu_create_buffer failed";
		return NULL;
	}
	spudgpu_buffer_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type           = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	desc.parent_buffer       = buffer;
	desc.stride              = 16;
	desc.size                = 256;
	spudgpu_buffer_view view = NULL;
	if (spudgpu_create_buffer_view(buffer, &desc, &view) == SPUD_SUCCESS)
		spudgpu_destroy_buffer_view(view);
	else
		why = "spudgpu_create_buffer_view failed";
	spudgpu_destroy_buffer(buffer);
	return view;''',
    'spudgpu_image': r'''
	if (!have_device())
		return NULL;
	spudgpu_image image = make_image();
	if (image)
		spudgpu_destroy_image(image);
	else
		why = "spudgpu_create_image failed";
	return image;''',
    'spudgpu_image_view': r'''
	if (!have_device())
		return NULL;
	spudgpu_image image = make_image();
	if (!image) {
		why = "spudgpu_create_image failed";
		return NULL;
	}
	spudgpu_image_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	desc.parent_image                        = image;
	desc.type                                = SPUDGPU_IMAGE_VIEW_TYPE_2D;
	desc.subresource_range.aspect_mask       = SPUDGPU_IMAGE_ASPECT_COLOR;
	desc.subresource_range.mip_level_count   = 1;
	desc.subresource_range.array_layer_count = 1;
	spudgpu_image_view view                  = NULL;
	if (spudgpu_create_image_view(image, &desc, &view) == SPUD_SUCCESS)
		spudgpu_destroy_image_view(view);
	else
		why = "spudgpu_create_image_view failed";
	spudgpu_destroy_image(image);
	return view;''',
    'spudgpu_heap': r'''
	if (!have_device())
		return NULL;
	spudgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = SPUD_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = SPUDGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = SPUDGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	desc.size           = SPUDGPU_HEAP_SIZE_GRANULARITY;
	spudgpu_heap heap   = NULL;
	if (spudgpu_create_heap(gpu_device, &desc, &heap) == SPUD_SUCCESS)
		spudgpu_destroy_heap(heap);
	else
		why = "spudgpu_create_heap failed";
	return heap;''',
    'spudgpu_command_allocator': r'''
	if (!have_device())
		return NULL;
	spudgpu_command_allocator allocator = make_command_allocator();
	if (allocator)
		spudgpu_destroy_command_allocator(allocator);
	else
		why = "spudgpu_create_command_allocator failed";
	return allocator;''',
    'spudgpu_command_list': r'''
	if (!have_device())
		return NULL;
	spudgpu_command_allocator allocator = make_command_allocator();
	if (!allocator) {
		why = "spudgpu_create_command_allocator failed";
		return NULL;
	}
	spudgpu_command_list cmd = NULL;
	if (spudgpu_create_command_list(allocator, &cmd) == SPUD_SUCCESS)
		spudgpu_destroy_command_list(cmd);
	else
		why = "spudgpu_create_command_list failed";
	spudgpu_destroy_command_allocator(allocator);
	return cmd;''',
    'spudgpu_fence': r'''
	if (!have_device())
		return NULL;
	spudgpu_fence fence = NULL;
	if (spudgpu_create_fence(gpu_device, SPUDGPU_FENCE_FLAG_NONE, 0, &fence) == SPUD_SUCCESS)
		spudgpu_destroy_fence(fence);
	else
		why = "spudgpu_create_fence failed";
	return fence;''',
    'spudgpu_semaphore': r'''
	if (!have_device())
		return NULL;
	spudgpu_semaphore semaphore = NULL;
	if (spudgpu_create_semaphore(gpu_device, &semaphore) == SPUD_SUCCESS)
		spudgpu_destroy_semaphore(semaphore);
	else
		why = "spudgpu_create_semaphore failed";
	return semaphore;''',
    'spudgpu_timestamp_set': r'''
	if (!have_device())
		return NULL;
	if (!(gpu_features & SPUDGPU_FEATURE_PASS_TIMESTAMPS)) {
		why = "adapter 0 has no pass timestamps";
		return NULL;
	}
	spudgpu_timestamp_set set = NULL;
	if (spudgpu_create_timestamp_set(gpu_device, gpu_queue, 2, &set) == SPUD_SUCCESS)
		spudgpu_destroy_timestamp_set(set);
	else
		why = "spudgpu_create_timestamp_set failed";
	return set;''',
    'spudgpu_acceleration_structure': r'''
	/* Its own instance and device, on the first adapter with ray queries
	 * (the shared device is adapter 0's, which may have none), both gone
	 * after: only the handle is read once it is destroyed. */
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "no SpudGPU instance";
		return NULL;
	}
	const spudgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	spudgpu_enumerate_adapters(instance, &adapters, &count);
	spudgpu_adapter adapter = NULL;
	for (uint32_t i = 0; i < count && !adapter; ++i) {
		SPUDGPU_ADAPTER_PROPERTIES properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
		if (spudgpu_get_adapter_properties(adapters[i], &properties) == SPUD_SUCCESS &&
		    (properties.supported_features & SPUDGPU_FEATURE_RAY_QUERY))
			adapter = adapters[i];
	}
	spudgpu_acceleration_structure structure = NULL;
	if (!adapter) {
		why = "no adapter has ray queries";
	} else {
		spudgpu_device_desc device_desc;
		memset(&device_desc, 0, sizeof(device_desc));
		device_desc.base.type          = SPUD_STRUCT_TYPE_GPU_DEVICE_DESC;
		device_desc.required_features  = SPUDGPU_FEATURE_RAY_QUERY;
		device_desc.direct_queue_count = 1;
		spudgpu_device device          = NULL;
		if (spudgpu_create_device(adapter, &device_desc, &device) != SPUD_SUCCESS) {
			why = "spudgpu_create_device with SPUDGPU_FEATURE_RAY_QUERY failed";
		} else {
			spudgpu_acceleration_structure_desc desc;
			memset(&desc, 0, sizeof(desc));
			desc.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
			desc.level     = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
			desc.size      = 4096;
			if (spudgpu_create_acceleration_structure(device, &desc, &structure) == SPUD_SUCCESS)
				spudgpu_destroy_acceleration_structure(structure);
			else
				why = "spudgpu_create_acceleration_structure failed";
			spudgpu_destroy_device(device);
		}
	}
	spudgpu_destroy_instance(instance);
	return structure;''',
    'spudgpu_sampler': r'''
	if (!have_device())
		return NULL;
	spudgpu_sampler_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = SPUD_STRUCT_TYPE_GPU_SAMPLER_DESC;
	desc.mag_filter         = SPUDGPU_FILTER_NEAREST;
	desc.min_filter         = SPUDGPU_FILTER_NEAREST;
	desc.mipmap_filter      = SPUDGPU_FILTER_NEAREST;
	desc.address_mode_u     = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_v     = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_w     = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.max_anisotropy     = 1.0f;
	spudgpu_sampler sampler = NULL;
	if (spudgpu_create_sampler(gpu_device, &desc, &sampler) == SPUD_SUCCESS)
		spudgpu_destroy_sampler(sampler);
	else
		why = "spudgpu_create_sampler failed";
	return sampler;''',
    'spudgpu_shader_module': r'''
	if (!have_device())
		return NULL;
	spudgpu_shader_module module =
	    make_module(&spudgpu_test_objects_double_comp);
	if (module)
		spudgpu_destroy_shader_module(module);
	else
		why = "spudgpu_create_shader_module failed";
	return module;''',
    'spudgpu_descriptor_set_layout': r'''
	if (!have_device())
		return NULL;
	const SPUDGPU_DESCRIPTOR_TYPE types[1] = {SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
	spudgpu_descriptor_set_layout layout   = make_set_layout(types, 1, SPUDGPU_SHADER_STAGE_FRAGMENT);
	if (layout)
		spudgpu_destroy_descriptor_set_layout(layout);
	else
		why = "spudgpu_create_descriptor_set_layout failed";
	return layout;''',
    'spudgpu_descriptor_pool': r'''
	if (!have_device())
		return NULL;
	spudgpu_descriptor_pool_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                     = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	desc.max_sets                      = 1;
	desc.pool_sizes[0].descriptor_type = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	desc.pool_sizes[0].count           = 1;
	desc.pool_size_count               = 1;
	spudgpu_descriptor_pool pool       = NULL;
	if (spudgpu_create_descriptor_pool(gpu_device, &desc, &pool) == SPUD_SUCCESS)
		spudgpu_destroy_descriptor_pool(pool);
	else
		why = "spudgpu_create_descriptor_pool failed";
	return pool;''',
    'spudgpu_compute_pipeline': r'''
	if (!have_device())
		return NULL;
	spudgpu_shader_module module =
	    make_module(&spudgpu_test_objects_double_comp);
	const SPUDGPU_DESCRIPTOR_TYPE types[2] = {SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	spudgpu_descriptor_set_layout layout   = make_set_layout(types, 2, SPUDGPU_SHADER_STAGE_COMPUTE);
	spudgpu_compute_pipeline pipeline      = NULL;
	if (module && layout) {
		spudgpu_compute_pipeline_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type                           = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
		desc.compute_module                      = module;
		desc.descriptor_set_layouts[0]           = layout;
		desc.descriptor_set_layout_count         = 1;
		desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
		desc.push_constant_ranges[0].size        = 2 * sizeof(uint32_t);
		desc.push_constant_range_count           = 1;
		if (spudgpu_create_compute_pipeline(gpu_device, &desc, &pipeline) == SPUD_SUCCESS)
			spudgpu_destroy_compute_pipeline(pipeline);
		else
			why = "spudgpu_create_compute_pipeline failed";
	} else {
		why = "no shader module or set layout for a compute pipeline";
	}
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(module);
	return pipeline;''',
    'spudgpu_shader_pipeline': r'''
	if (!have_device())
		return NULL;
	/* tests/shaders/objects_draw.vert and .frag, as spudgpu_objects_test
	 * draws with them. */
	spudgpu_shader_module vertex =
	    make_module(&spudgpu_test_objects_draw_vert);
	spudgpu_shader_module fragment =
	    make_module(&spudgpu_test_objects_draw_frag);
	const SPUDGPU_DESCRIPTOR_TYPE types[3] = {SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	                                          SPUDGPU_DESCRIPTOR_TYPE_SAMPLER};
	spudgpu_descriptor_set_layout layout   = make_set_layout(types, 3, SPUDGPU_SHADER_STAGE_FRAGMENT);
	spudgpu_shader_pipeline pipeline       = NULL;
	if (vertex && fragment && layout) {
		spudgpu_shader_pipeline_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type                           = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
		desc.vertex_module                       = vertex;
		desc.fragment_module                     = fragment;
		desc.vertex_attributes[0].location       = 0;
		desc.vertex_attributes[0].binding        = 0;
		desc.vertex_attributes[0].format         = SPUDGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attributes[1].location       = 1;
		desc.vertex_attributes[1].binding        = 1;
		desc.vertex_attributes[1].format         = SPUDGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attribute_count              = 2;
		desc.vertex_bindings[0].binding          = 0;
		desc.vertex_bindings[0].stride           = 2 * sizeof(float);
		desc.vertex_bindings[1].binding          = 1;
		desc.vertex_bindings[1].stride           = 2 * sizeof(float);
		desc.vertex_bindings[1].per_instance     = true;
		desc.vertex_binding_count                = 2;
		desc.primitive_topology                  = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		desc.cull_mode                           = SPUDGPU_CULL_MODE_NONE;
		desc.depth_compare_op                    = SPUDGPU_COMPARE_OP_ALWAYS;
		desc.color_attachment_formats[0]         = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
		desc.color_attachment_count              = 1;
		desc.depth_format                        = SPUDGPU_FORMAT_D32_FLOAT;
		desc.descriptor_set_layouts[0]           = layout;
		desc.descriptor_set_layout_count         = 1;
		desc.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
		desc.push_constant_ranges[0].size        = 8 * sizeof(float);
		desc.push_constant_range_count           = 1;
		if (spudgpu_create_shader_pipeline(gpu_device, &desc, &pipeline) == SPUD_SUCCESS)
			spudgpu_destroy_shader_pipeline(pipeline);
		else
			why = "spudgpu_create_shader_pipeline failed";
	} else {
		why = "no shader modules or set layout for a pipeline";
	}
	spudgpu_destroy_descriptor_set_layout(layout);
	spudgpu_destroy_shader_module(fragment);
	spudgpu_destroy_shader_module(vertex);
	return pipeline;''',
    'spudgpu_surface': r'''
	if (!gpu_instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_surface surface = make_surface();
	if (surface)
		spudgpu_destroy_surface(surface);
	return surface;''',
    'spudgpu_swap_chain': r'''
	if (!have_device())
		return NULL;
	if (!(gpu_features & SPUDGPU_FEATURE_PRESENT)) {
		why = "adapter 0 can't present";
		return NULL;
	}
	spudgpu_surface surface = make_surface();
	if (!surface)
		return NULL;
	spudgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	desc.queue     = gpu_queue;
	desc.width     = 64;
	desc.height    = 64;
#if SPUDGPU_COMPILE_METAL_API
	desc.buffer_count = 1;
#else
	desc.buffer_count = 2;
#endif
	desc.format                   = SPUDGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode             = SPUDGPU_PRESENT_MODE_FIFO;
	desc.present_scaling          = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE;
	desc.fullscreen_mode          = SPUDGPU_FULLSCREEN_MODE_WINDOWED;
	spudgpu_swap_chain swap_chain = NULL;
	if (spudgpu_create_swap_chain(gpu_device, &desc, &swap_chain) == SPUD_SUCCESS)
		spudgpu_destroy_swap_chain(swap_chain);
	else
		why = "spudgpu_create_swap_chain failed";
	spudgpu_destroy_surface(surface);
	return swap_chain;''',
    # ---- SpudInput
    'spudinput_instance': r'''
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
	SPUDINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type  = SPUD_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.allocator  = &allocator;
	desc.native_api = api;
#if SPUDINPUT_COMPILE_XINPUT || SPUDINPUT_COMPILE_EVDEV
	desc.read_in_background = true; /* these always read */
#endif
	spudinput_instance instance = NULL;
	if (api == SPUDINPUT_NATIVE_API_NONE || spudinput_instance_create(&desc, &instance) != SPUD_SUCCESS) {
		why = "no SpudInput instance in this build";
		return NULL;
	}
	spudinput_instance_destroy(instance);
	return instance;''',
    # ---- SpudNet
    'spudnet_instance': r'''
	spudnet_instance instance = make_net_instance();
	if (instance)
		spudnet_instance_destroy(instance);
	return instance;''',
    'spudnet_resolver': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_resolver_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type            = SPUD_STRUCT_TYPE_NET_RESOLVER_DESC;
	desc.host                 = "localhost";
	spudnet_resolver resolver = NULL;
	if (spudnet_resolver_create(instance, &desc, &resolver) == SPUD_SUCCESS) {
		/* Run once, aborted first so that it returns at once, and
		 * destroyed: then it is freed. */
		spudnet_resolver_abort(resolver);
		spudnet_resolver_run(resolver);
		spudnet_resolver_destroy(resolver);
	} else {
		why = "spudnet_resolver_create failed";
	}
	spudnet_instance_destroy(instance);
	return resolver;''',
    'spudnet_tcp_listener': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_tcp_listener_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                = SPUD_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	desc.address                  = "127.0.0.1";
	desc.backlog                  = 1;
	spudnet_tcp_listener listener = NULL;
	if (spudnet_tcp_listener_create(instance, &desc, &listener, NULL) == SPUD_SUCCESS)
		spudnet_tcp_listener_destroy(listener);
	else
		why = "no loopback listener";
	spudnet_instance_destroy(instance);
	return listener;''',
    'spudnet_tcp_socket': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_tcp_socket socket = NULL;
	if (spudnet_tcp_socket_create(instance, &socket) == SPUD_SUCCESS)
		spudnet_tcp_socket_destroy(socket);
	else
		why = "spudnet_tcp_socket_create failed";
	spudnet_instance_destroy(instance);
	return socket;''',
    'spudnet_http_client': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_http_client client = make_http_client(instance);
	if (client)
		spudnet_http_client_destroy(client);
	spudnet_instance_destroy(instance);
	return client;''',
    'spudnet_http_transfer': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_http_client client     = make_http_client(instance);
	spudnet_http_transfer transfer = NULL;
	if (client) {
		if (spudnet_http_transfer_create(client, &transfer) == SPUD_SUCCESS)
			spudnet_http_transfer_destroy(transfer);
		else
			why = "spudnet_http_transfer_create failed";
		spudnet_http_client_destroy(client);
	}
	spudnet_instance_destroy(instance);
	return transfer;''',
    'spudnet_websocket': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_websocket socket = NULL;
	if (spudnet_websocket_create(instance, &socket) == SPUD_SUCCESS)
		spudnet_websocket_destroy(socket);
	else
		why = "spudnet_websocket_create failed (no WebSocket in this build)";
	spudnet_instance_destroy(instance);
	return socket;''',
    'spudnet_wait_set': r'''
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_wait_set set = NULL;
	if (spudnet_wait_set_create(instance, &set, NULL) == SPUD_SUCCESS)
		spudnet_wait_set_destroy(set);
	else
		why = "spudnet_wait_set_create failed";
	spudnet_instance_destroy(instance);
	return set;''',
}

NL = '\\n'


def read_handles():
    handles = []
    for h in HEADERS:
        for m in re.finditer(r'typedef struct \w+ \*(\w+);', open(os.path.join(ROOT, 'include', h)).read()):
            handles.append(m.group(1))
    return handles


def guard_of(tags, name):
    guard = None
    if tags:
        guard = ' || '.join(PLATFORMS[t] for t in tags.split(','))
    for prefix, (macro, header) in GUARDED.items():
        if name.startswith(prefix):
            guard = macro if not guard else '(%s) && %s' % (guard, macro)
    return guard


def split_params(args):
    return [a.strip() for a in args.split(',')] if args.strip() not in ('', 'void') else []


def main():
    functions = []
    for line in open(os.path.join(ROOT, 'abi.txt')):
        m = re.match(r'(?:\[([\w,]+)\] )?fn (\w+) (.+?) \((.*)\)$', line.strip())
        if m:
            functions.append((m.group(1), m.group(2), m.group(3), m.group(4)))
    handles = read_handles()
    out = []
    out.append('// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.\n')
    out.append('// Every public function of %s called with every argument zeroed\n' % LIBRARY)
    out.append('// (ROADMAP.md in the workspace, section 3.1). None may crash; one\n')
    out.append('// returning %s returns an error unless the generator lists zeroes as\n' % RESULT)
    out.append('// a valid call for it. One returning nothing is called and counted, since\n')
    out.append('// it can\'t refuse.\n')
    out.append('//\n')
    out.append('// Then, in a Debug build not under a sanitizer, every public function whose\n')
    out.append('// first parameter is a handle, called with one just destroyed (decision\n')
    out.append('// X16): no crash, and an error, or the null fallback of a getter.\n\n')
    out.append(''.join('#include "%s"\n' % h for h in HEADERS))
    for prefix, (macro, header) in GUARDED.items():
        out.append('#if %s\n#include "%s"\n#endif\n' % (macro, header))
    out.append('#include <stdbool.h>\n#include <stdio.h>\n\nstatic int failures = 0;\n\n')
    out.append('static void expect_error(const char *name, %s result)\n{\n' % RESULT)
    out.append('\tif (result == %s) {\n' % OK)
    out.append('\t\tprintf("FAIL %%s: returned %s for zeroes%s", name);\n\t\tfailures++;\n\t}\n}\n\n' % (OK, NL))

    # ---- The freed half's switch and code.
    out.append('// The freed half: Debug only, since the check is; never under a sanitizer,\n')
    out.append('// which reports the read of freed memory the check makes.\n')
    out.append('#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)\n')
    out.append('#define MISUSE_SANITIZED 1\n')
    out.append('#elif defined(__has_feature)\n')
    out.append('#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)\n')
    out.append('#define MISUSE_SANITIZED 1\n#endif\n#endif\n')
    out.append('#ifndef MISUSE_SANITIZED\n#define MISUSE_SANITIZED 0\n#endif\n')
    out.append('#if defined(%s) && %s && !MISUSE_SANITIZED\n#define MISUSE_FREED 1\n#else\n#define MISUSE_FREED 0\n#endif\n\n' % (DEBUG_MACRO, DEBUG_MACRO))
    out.append('#if MISUSE_FREED\n')
    for inc in FREED_INCLUDES:
        out.append('#include %s\n' % (inc if inc.startswith('<') else '"%s"' % inc))
    out.append(FREED_PRELUDE)
    out.append('\nstatic int freed_failures = 0;\n\n')
    out.append('static void expect_refused(const char *name, %s result)\n{\n' % RESULT)
    out.append('\tif (result == %s) {\n' % OK)
    out.append('\t\tprintf("FAIL %%s: returned %s for a destroyed handle%s", name);\n\t\tfreed_failures++;\n\t}\n}\n\n' % (OK, NL))
    out.append('static void expect_fallback(const char *name, bool same)\n{\n')
    out.append('\tif (!same) {\n')
    out.append('\t\tprintf("FAIL %%s: returned other than for NULL for a destroyed handle%s", name);\n\t\tfreed_failures++;\n\t}\n}\n\n' % NL)

    # Which handle types some function takes first.
    first_types = []
    for tags, name, ret, args in functions:
        params = split_params(args)
        if params and params[0] in handles and params[0] not in first_types:
            first_types.append(params[0])
    for t in handles:
        if t not in first_types and t not in NO_FIRST_PARAMETER:
            raise SystemExit('%s: no function takes it first; add it to NO_FIRST_PARAMETER' % t)
    for t in first_types:
        if t in FACTORIES:
            out.append('/* A %s just destroyed, or NULL with `why` set. */\n' % t)
            out.append('static %s make_dead_%s(void) {%s\n}\n\n' % (t, t, FACTORIES[t]))
            # The first time a factory gives nothing, say why.
            out.append('static %s dead_%s(void) {\n' % (t, t))
            out.append('\tstatic bool told = false;\n')
            out.append('\twhy = "";\n')
            out.append('\t%s handle = make_dead_%s();\n' % (t, t))
            out.append('\tif (!handle && !told) {\n')
            out.append('\t\tprintf("%s: not covered: %s: %%s%s", why);\n' % (OUTPUT, t, NL))
            out.append('\t\ttold = true;\n\t}\n\treturn handle;\n}\n\n')

    out.append('static int freed_half(int *out_calls, int *out_unchecked, int *out_skipped)\n{\n')
    out.append('\tint calls = 0;\n\tint unchecked = 0;\n\tint skipped = 0;\n')
    for t in handles:
        if t in NO_FIRST_PARAMETER:
            out.append('\t/* %s: no public function takes one first. */\n' % t)
        elif t not in FACTORIES:
            out.append('\tprintf("%s: not covered: %s: no factory in the generator%s");\n' % (OUTPUT, t, NL))
    for name, reason in FREED_NOT_CALLED.items():
        out.append('\t/* %s: not called with a destroyed handle: %s. */\n' % (name, reason))
    out.append('\tfreed_setup();\n')
    for tags, name, ret, args in functions:
        params = split_params(args)
        if not params or params[0] not in FACTORIES or name in FREED_NOT_CALLED or name in NOT_CALLED:
            continue
        guard = guard_of(tags, name)
        if guard:
            out.append('#if %s\n' % guard)
        rest = ''.join(', (%s){0}' % p for p in params[1:])
        dead_call = '%s(dead%s)' % (name, rest)
        null_call = '%s((%s){0}%s)' % (name, params[0], rest)
        out.append('\t{\n\t\t%s dead = dead_%s();\n\t\tif (dead) {\n' % (params[0], params[0]))
        if ret == RESULT and name not in ZEROES_SUCCEED:
            out.append('\t\t\texpect_refused("%s", %s);\n' % (name, dead_call))
        elif ret == 'void':
            out.append('\t\t\t%s;\n\t\t\tunchecked++;\n' % dead_call)
        else:
            out.append('\t\t\texpect_fallback("%s", %s == %s);\n' % (name, dead_call, null_call))
        out.append('\t\t\tcalls++;\n\t\t} else {\n\t\t\tskipped++;\n\t\t}\n\t}\n')
        if guard:
            out.append('#endif\n')
    out.append('\tfreed_teardown();\n')
    out.append('\t*out_calls     = calls;\n\t*out_unchecked = unchecked;\n\t*out_skipped   = skipped;\n')
    out.append('\treturn freed_failures;\n}\n')
    out.append('#endif // MISUSE_FREED\n\n')

    # ---- main: the zeroed half, then the freed half.
    out.append('int main(void)\n{\n\tint calls = 0;\n\tint unchecked = 0;\n')
    for tags, name, ret, args in functions:
        guard = guard_of(tags, name)
        if guard:
            out.append('#if %s\n' % guard)
        if name in NOT_CALLED:
            out.append('\t// %s: not called: %s\n' % (name, NOT_CALLED[name]))
        else:
            params = split_params(args)
            call = '%s(%s)' % (name, ', '.join('(%s){0}' % p for p in params))
            if ret == RESULT:
                if name in ZEROES_SUCCEED:
                    out.append('\t(void)%s; // zeroes are valid: %s\n' % (call, ZEROES_SUCCEED[name]))
                else:
                    out.append('\texpect_error("%s", %s);\n' % (name, call))
            elif ret == 'void':
                out.append('\t%s;\n\tunchecked++;\n' % call)
            else:
                out.append('\t(void)%s;\n' % call)
            out.append('\tcalls++;\n')
        if guard:
            out.append('#endif\n')
    out.append('\tprintf("%s: %%d calls with zeroes, %%d of them returning nothing to check, %%d '
               'failure(s)%s", calls, unchecked, failures);\n' % (OUTPUT, NL))
    out.append('#if MISUSE_FREED\n')
    out.append('\tint freed_calls = 0, freed_unchecked = 0, freed_skipped = 0;\n')
    out.append('\tfailures += freed_half(&freed_calls, &freed_unchecked, &freed_skipped);\n')
    out.append('\tprintf("%s: %%d calls with a destroyed handle, %%d of them returning nothing to check, %%d '
               'skipped for want of a handle, %%d failure(s) in all%s", freed_calls, freed_unchecked, freed_skipped, failures);\n'
               % (OUTPUT, NL))
    out.append('#elif MISUSE_SANITIZED\n')
    out.append('\tprintf("%s: the freed-handle half skipped: under a sanitizer, which reports the check\'s read of freed memory%s");\n'
               % (OUTPUT, NL))
    out.append('#else\n')
    out.append('\tprintf("%s: the freed-handle half skipped: not a Debug build, which has no check%s");\n' % (OUTPUT, NL))
    out.append('#endif\n')
    out.append('\treturn failures ? 1 : 0;\n}\n')
    open(os.path.join(ROOT, 'tests', OUTPUT + '.c'), 'w', newline='\n').write(''.join(out))


main()
