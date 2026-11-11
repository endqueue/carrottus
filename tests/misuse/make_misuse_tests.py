#!/usr/bin/env python3
"""Writes tests/cr_misuse_tests.c from abi.txt, in two halves.

The zeroed half: a call to every public function of Carrottus with every
argument zeroed - NULL handles and pointers, zero sizes and counts, zeroed
structs (ROADMAP.md in the workspace, section 3.1, "a misuse harness from the
headers"). None may crash, and one that returns CR_RESULT must not return
CR_SUCCESS, unless the list below says why zeroes are a valid call. A
function that returns nothing can't say it refused: the test counts those.

The freed half (decision X16, src/crstale.h): every public function whose
first parameter is a handle, called with a handle that has just been
destroyed and every other argument zeroed. None may crash; one that returns
CR_RESULT must not return CR_SUCCESS (or, where zeroes are a valid call,
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

Run after ./check.sh --update-abi changes carrottus/abi.txt, or after a change
to the configuration:
    python3 tests/misuse/make_misuse_tests.py
The output is checked in, so a build needs no Python."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LIBRARY = 'Carrottus'
RESULT = 'CR_RESULT'
OK = 'CR_SUCCESS'
OUTPUT = 'cr_misuse_tests'
HEADERS = ['craudio.h', 'crcore.h', 'crfiles.h', 'crgpu.h', 'crinput.h', 'crmemory.h', 'crnet.h', 'crperf.h', 'crrandom.h']

# Function name prefixes called only when a macro is set, each with its
# macro (a part of the library a build may leave out) and the header that
# declares them, included under the same macro.
GUARDED = {
}

# Functions zeroes are a valid call for, each with the reason.
ZEROES_SUCCEED = {
    'crgpu_instance_destroy': 'destroying NULL succeeds, as crgpu.h says',
    'crrandom_fill': 'asking for no bytes succeeds, as crrandom.h says',
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
    'crnet_resolver_run': 'may start after the resolver is destroyed (crnet.h), so it has no stale check, and '
                            'a resolver that has been run and destroyed is freed memory',
}

# Handle types no public function takes first, so no factory is needed.
# Listed so that a new function taking one first is noticed: its type then
# needs a factory.
NO_FIRST_PARAMETER = ['crgpu_descriptor_set', 'crgpu_pipeline_layout']

# Included by the freed half, beside HEADERS.
FREED_INCLUDES = ['crgpu_test_shaders.h', '<stdlib.h>', '<string.h>']

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
 * destroyed handle's memory then stays readable, with CR_DEAD in its tag,
 * whatever the system's allocator does with memory it is given back (the
 * Windows heap decommits freed pages, and reading one faults), and no new
 * object can be made in it. Locked, for an object freed on a thread of the
 * platform's. Since card X27 a Debug build keeps a destroyed handle's
 * block itself until the handle's owner is destroyed (src/crstale.h), but
 * not every handle has an owner: an instance, a CrAudio stream and a
 * CrNet resolver are freed at once, so this stays. */
typedef struct kept_block {
	struct kept_block *next;
	void *memory;
	size_t size, alignment;
} kept_block;

static CR_ALLOCATOR system_allocator;
static CR_ALLOCATOR allocator;
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

/* CrGPU's: one instance and a device on adapter 0, which every object a
 * factory makes of a device comes from, and a hidden window on Windows for
 * the surface and the swap chain. */
static crgpu_instance gpu_instance;
static crgpu_device gpu_device;
static crgpu_command_queue gpu_queue;
static CRGPU_FEATURES gpu_features;
static CRGPU_SHADER_FORMAT gpu_shader_format;
#if defined(_WIN32)
static HWND gpu_window;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
	return DefWindowProcW(window, message, w, l);
}
#endif

static crgpu_instance make_gpu_instance(void) {
	crgpu_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	desc.allocator          = &allocator;
	desc.application_name   = "cr_misuse_tests";
	desc.engine_name        = "cr_misuse_tests";
	crgpu_instance result = NULL;
	if (crgpu_instance_create(&desc, &result) != CR_SUCCESS)
		return NULL;
	return result;
}

/* A device on [instance]'s adapter 0 with [features], or NULL. */
static crgpu_device make_gpu_device(crgpu_instance instance, CRGPU_FEATURES features) {
	const crgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	if (crgpu_adapter_enumerate(instance, &adapters, &count) != CR_SUCCESS || count == 0)
		return NULL;
	crgpu_device_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	desc.required_features  = features;
	desc.direct_queue_count = 1;
	desc.max_descriptors    = 16;
	desc.max_samplers       = 4;
	crgpu_device device   = NULL;
	if (crgpu_device_create(adapters[0], &desc, &device) != CR_SUCCESS)
		return NULL;
	return device;
}

static void freed_setup(void) {
	system_allocator   = cr_allocator_system();
	allocator          = system_allocator;
	allocator.allocate = keeping_allocate;
	allocator.free     = keeping_free;
	gpu_instance       = make_gpu_instance();
	if (gpu_instance) {
		const crgpu_adapter *adapters = NULL;
		uint32_t count                  = 0;
		crgpu_adapter_enumerate(gpu_instance, &adapters, &count);
		if (count > 0) {
			CRGPU_ADAPTER_PROPERTIES properties;
			memset(&properties, 0, sizeof(properties));
			properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
			if (crgpu_adapter_get_properties(adapters[0], &properties) == CR_SUCCESS) {
				gpu_features      = properties.supported_features & (CRGPU_FEATURE_PRESENT | CRGPU_FEATURE_PASS_TIMESTAMPS);
				gpu_shader_format = crgpu_test_shader_format(properties.shader_formats);
			}
		}
		gpu_device = make_gpu_device(gpu_instance, gpu_features);
		if (gpu_device)
			crgpu_command_queue_get(gpu_device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &gpu_queue);
	}
#if defined(_WIN32)
	WNDCLASSEXW window_class;
	memset(&window_class, 0, sizeof(window_class));
	window_class.cbSize        = sizeof(window_class);
	window_class.lpfnWndProc   = window_proc;
	window_class.hInstance     = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"cr_misuse_tests";
	RegisterClassExW(&window_class);
	gpu_window = CreateWindowExW(0, L"cr_misuse_tests", L"cr_misuse_tests", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, NULL, NULL,
	                             GetModuleHandleW(NULL), NULL);
#endif
}

static void freed_teardown(void) {
	crgpu_device_destroy(gpu_device);
	crgpu_instance_destroy(gpu_instance);
#if defined(_WIN32)
	if (gpu_window)
		DestroyWindow(gpu_window);
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(L"cr_misuse_tests", GetModuleHandleW(NULL));
#endif
	keeping_release_all();
}

static bool have_device(void) {
	if (!gpu_device)
		why = "no CrGPU device on adapter 0";
	return gpu_device != NULL;
}

static crgpu_buffer make_buffer(void) {
	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = CRGPU_BUFFER_USAGE_UNIFORM;
	desc.memory_kind      = CRGPU_MEMORY_KIND_UPLOAD;
	desc.size             = 256;
	crgpu_buffer buffer = NULL;
	if (crgpu_buffer_create(gpu_device, &desc, &buffer) != CR_SUCCESS)
		return NULL;
	return buffer;
}

static crgpu_image make_image(void) {
	crgpu_image_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_IMAGE_DESC;
	desc.usage              = CRGPU_IMAGE_USAGE_SAMPLED;
	desc.type               = CRGPU_IMAGE_TYPE_2D;
	desc.format             = CRGPU_FORMAT_R8G8B8A8_UNORM;
	desc.clear_value.format = CRGPU_FORMAT_R8G8B8A8_UNORM;
	desc.width              = 4;
	desc.height             = 4;
	desc.depth              = 1;
	desc.array_layers       = 1;
	desc.mip_levels         = 1;
	crgpu_image image     = NULL;
	if (crgpu_image_create(gpu_device, &desc, &image) != CR_SUCCESS)
		return NULL;
	return image;
}

static crgpu_shader_module make_module(const crgpu_test_shader *shader) {
	const crgpu_shader_module_desc desc = crgpu_test_module_desc(shader, gpu_shader_format, NULL);
	crgpu_shader_module module          = NULL;
	if (crgpu_shader_module_create(gpu_device, &desc, &module) != CR_SUCCESS)
		return NULL;
	return module;
}

/* [count] bindings of [type], 0 up, seen by [stages]. */
static crgpu_descriptor_set_layout make_set_layout(const CRGPU_DESCRIPTOR_TYPE *types, uint32_t count, CRGPU_SHADER_STAGE stages) {
	crgpu_descriptor_set_layout_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < count; ++i) {
		desc.bindings[i].binding         = i;
		desc.bindings[i].descriptor_type = types[i];
		desc.bindings[i].count           = 1;
		desc.bindings[i].stage_flags     = stages;
	}
	desc.binding_count                   = count;
	crgpu_descriptor_set_layout layout = NULL;
	if (crgpu_descriptor_set_layout_create(gpu_device, &desc, &layout) != CR_SUCCESS)
		return NULL;
	return layout;
}

static crgpu_command_allocator make_command_allocator(void) {
	crgpu_command_allocator_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                      = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	desc.type                           = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	crgpu_command_allocator allocator = NULL;
	if (crgpu_command_allocator_create(gpu_device, &desc, &allocator) != CR_SUCCESS)
		return NULL;
	return allocator;
}

static crgpu_surface make_surface(void) {
#if defined(_WIN32)
	if (!gpu_window) {
		why = "no window";
		return NULL;
	}
	crgpu_surface surface = NULL;
	if (crgpu_surface_create(gpu_instance, gpu_window, NULL, &surface) != CR_SUCCESS) {
		why = "crgpu_surface_create failed";
		return NULL;
	}
	return surface;
#else
	why = "a surface needs a window, which this test makes on Windows only";
	return NULL;
#endif
}

static craudio_instance make_audio_instance(void) {
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
	CRAUDIO_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type              = CR_STRUCT_TYPE_AUDIO_INSTANCE_DESC;
	desc.allocator              = &allocator;
	desc.native_api             = api;
	craudio_instance instance = NULL;
	if (api == CRAUDIO_NATIVE_API_NONE || craudio_instance_create(&desc, &instance) != CR_SUCCESS) {
		why = "no CrAudio instance in this build";
		return NULL;
	}
	return instance;
}

/* [instance]'s first output device, or NULL. */
static craudio_device first_output(craudio_instance instance) {
	craudio_device *devices = NULL;
	uint32_t count            = 0;
	if (craudio_device_enumerate(instance, CRAUDIO_DIRECTION_OUTPUT, &devices, &count) != CR_SUCCESS || count == 0) {
		why = "no audio output device";
		return NULL;
	}
	return devices[0];
}

static void silent_callback(craudio_stream stream, void *frames, uint32_t frame_count, const CRAUDIO_CALLBACK_INFO *info, void *user_data) {
	(void)stream;
	(void)frames;
	(void)frame_count;
	(void)info;
	(void)user_data;
}

static crfiles_instance make_file_instance(void) {
	crfiles_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type             = CR_STRUCT_TYPE_FILES_INSTANCE_DESC;
	desc.allocator             = &allocator;
	crfiles_instance instance = NULL;
	if (crfiles_instance_create(&desc, &instance) != CR_SUCCESS) {
		why = "crfiles_instance_create failed";
		return NULL;
	}
	return instance;
}

#define FREED_FILE "cr_misuse_tests.tmp"

/* A file of 4096 bytes, made in the working directory, or NULL. */
static crfiles_file make_file(crfiles_instance instance) {
	crfiles_open_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type     = CR_STRUCT_TYPE_FILES_OPEN_DESC;
	desc.path          = FREED_FILE;
	desc.access        = CRFILES_ACCESS_READ_WRITE;
	desc.create        = CRFILES_CREATE_TRUNCATE;
	desc.share         = CRFILES_SHARE_READ;
	crfiles_file file = NULL;
	if (crfiles_file_open(instance, &desc, &file) != CR_SUCCESS) {
		why = "no file could be made in the working directory";
		return NULL;
	}
	if (crfiles_file_set_size(file, 4096) != CR_SUCCESS) {
		why = "crfiles_file_set_size failed";
		crfiles_file_close(file);
		return NULL;
	}
	return file;
}

static crnet_instance make_net_instance(void) {
	crnet_instance_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type            = CR_STRUCT_TYPE_NET_INSTANCE_DESC;
	desc.allocator            = &allocator;
	crnet_instance instance = NULL;
	if (crnet_instance_create(&desc, &instance, NULL) != CR_SUCCESS) {
		why = "crnet_instance_create failed";
		return NULL;
	}
	return instance;
}

static crnet_http_client make_http_client(crnet_instance instance) {
	crnet_http_client_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type             = CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC;
	desc.proxy.mode            = CRNET_PROXY_NONE;
	crnet_http_client client = NULL;
	if (crnet_http_client_create(instance, &desc, &client) != CR_SUCCESS) {
		why = "crnet_http_client_create failed";
		return NULL;
	}
	return client;
}
'''

# For each handle type, the body of a C function returning that type: a
# handle made through the public API and destroyed, or NULL with `why` set.
FACTORIES = {
    # ---- CrAudio
    'craudio_instance': r'''
	craudio_instance instance = make_audio_instance();
	if (instance)
		craudio_instance_destroy(instance);
	return instance;''',
    'craudio_device': r'''
	/* A device is the instance's: it goes with it. */
	craudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	craudio_device device = first_output(instance);
	craudio_instance_destroy(instance);
	return device;''',
    'craudio_stream': r'''
	craudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	craudio_device device = first_output(instance);
	craudio_stream stream = NULL;
	if (device) {
		CRAUDIO_DEVICE_PROPERTIES properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = CR_STRUCT_TYPE_AUDIO_DEVICE_PROPERTIES;
		craudio_device_get_properties(device, &properties);
		CRAUDIO_STREAM_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type = CR_STRUCT_TYPE_AUDIO_STREAM_DESC;
		desc.device    = device;
		desc.format    = properties.native_format;
		if (desc.format.sample_format == CRAUDIO_SAMPLE_FORMAT_UNKNOWN) {
			desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
			desc.format.sample_rate   = 48000;
			desc.format.channel_count = 2;
			desc.format.interleaved   = true;
		}
		/* The platform's own thread where it is the only mechanism. */
#if CRAUDIO_COMPILE_COREAUDIO || CRAUDIO_COMPILE_REMOTEIO
		desc.thread.priority = CRAUDIO_THREAD_PRIORITY_PLATFORM;
#else
		desc.thread.priority = CRAUDIO_THREAD_PRIORITY_NORMAL;
#endif
		desc.callback        = silent_callback;
		desc.period_frames   = desc.format.sample_rate / 100;
		/* Each rejected field takes the probe's suggestion, a few rounds. */
		for (int round = 0; round < 8; ++round) {
			CRAUDIO_STREAM_CONFIG config;
			memset(&config, 0, sizeof(config));
			config.base.type   = CR_STRUCT_TYPE_AUDIO_STREAM_CONFIG;
			const CR_RESULT r = craudio_stream_probe(&desc, &config);
			if (r == CR_SUCCESS)
				break;
			if (r == CR_RESULT_AUDIO_FORMAT_NOT_SUPPORTED && config.format.sample_format != CRAUDIO_SAMPLE_FORMAT_UNKNOWN)
				desc.format = config.format;
			else if (r == CR_RESULT_AUDIO_PERIOD_OUT_OF_RANGE && config.period_frames)
				desc.period_frames = config.period_frames;
			else if (r == CR_RESULT_AUDIO_BUFFER_OUT_OF_RANGE)
				desc.buffer_frames = config.buffer_frames;
			else
				break;
		}
		if (craudio_stream_create(&desc, &stream) == CR_SUCCESS)
			craudio_stream_destroy(stream);
		else
			why = "the output device took no stream";
	}
	craudio_instance_destroy(instance);
	return stream;''',
    # ---- CrFiles
    'crfiles_instance': r'''
	crfiles_instance instance = make_file_instance();
	if (instance)
		crfiles_instance_destroy(instance);
	return instance;''',
    'crfiles_file': r'''
	crfiles_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	crfiles_file file = make_file(instance);
	if (file)
		crfiles_file_close(file);
	crfiles_path_remove(instance, FREED_FILE);
	crfiles_instance_destroy(instance);
	return file;''',
    'crfiles_mapping': r'''
	crfiles_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	crfiles_file file       = make_file(instance);
	crfiles_mapping mapping = NULL;
	if (file) {
		if (crfiles_file_map(file, 0, 4096, &mapping) == CR_SUCCESS)
			crfiles_mapping_release(mapping);
		else
			why = "crfiles_file_map failed";
		crfiles_file_close(file);
	}
	crfiles_path_remove(instance, FREED_FILE);
	crfiles_instance_destroy(instance);
	return mapping;''',
    'crfiles_listing': r'''
	crfiles_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	crfiles_listing_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type           = CR_STRUCT_TYPE_FILES_LISTING_DESC;
	desc.path                = ".";
	crfiles_listing listing = NULL;
	if (crfiles_listing_open(instance, &desc, &listing) == CR_SUCCESS)
		crfiles_listing_close(listing);
	else
		why = "crfiles_listing_open failed on the working directory";
	crfiles_instance_destroy(instance);
	return listing;''',
    'crfiles_library': r'''
	/* A system library by name, there on every platform CrFiles builds
	 * for, searched for by the platform. */
	crfiles_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	crfiles_library_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_FILES_LIBRARY_DESC;
#if CARROTTUS_PLATFORM_WIN32
	desc.path = "kernel32.dll";
#elif CARROTTUS_PLATFORM_APPLE
	desc.path = "libSystem.B.dylib";
#else
	desc.path = "libc.so.6";
#endif
	desc.binding             = CRFILES_LIBRARY_BINDING_NOW;
	desc.scope               = CRFILES_LIBRARY_SCOPE_LOCAL;
	desc.dependencies        = CRFILES_LIBRARY_DEPENDENCIES_PLATFORM_ORDER;
	crfiles_library library = NULL;
	if (crfiles_library_open(instance, &desc, &library) == CR_SUCCESS)
		crfiles_library_close(library);
	else
		why = "crfiles_library_open failed on the system's C library";
	crfiles_instance_destroy(instance);
	return library;''',
    # ---- CrGPU
    'crgpu_instance': r'''
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_instance_destroy(instance);
	return instance;''',
    'crgpu_adapter': r'''
	/* An adapter is the instance's: it goes with it. */
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	const crgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	crgpu_adapter_enumerate(instance, &adapters, &count);
	crgpu_adapter adapter = count > 0 ? adapters[0] : NULL;
	if (!adapter)
		why = "no adapter";
	crgpu_instance_destroy(instance);
	return adapter;''',
    'crgpu_device': r'''
	/* Its own instance: the shared one's adapter 0 already has a device. */
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_device device = make_gpu_device(instance, 0);
	if (device)
		crgpu_device_destroy(device);
	else
		why = "no device on adapter 0";
	crgpu_instance_destroy(instance);
	return device;''',
    'crgpu_command_queue': r'''
	/* A queue is the device's: it goes with it. */
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_device device       = make_gpu_device(instance, 0);
	crgpu_command_queue queue = NULL;
	if (device) {
		crgpu_command_queue_get(device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &queue);
		crgpu_device_destroy(device);
	}
	if (!queue)
		why = "no device on adapter 0";
	crgpu_instance_destroy(instance);
	return queue;''',
    'crgpu_buffer': r'''
	if (!have_device())
		return NULL;
	crgpu_buffer buffer = make_buffer();
	if (buffer)
		crgpu_buffer_destroy(buffer);
	else
		why = "crgpu_buffer_create failed";
	return buffer;''',
    'crgpu_buffer_view': r'''
	if (!have_device())
		return NULL;
	crgpu_buffer buffer = make_buffer();
	if (!buffer) {
		why = "crgpu_buffer_create failed";
		return NULL;
	}
	crgpu_buffer_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type           = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	desc.parent_buffer       = buffer;
	desc.stride              = 16;
	desc.size                = 256;
	crgpu_buffer_view view = NULL;
	if (crgpu_buffer_view_create(buffer, &desc, &view) == CR_SUCCESS)
		crgpu_buffer_view_destroy(view);
	else
		why = "crgpu_buffer_view_create failed";
	crgpu_buffer_destroy(buffer);
	return view;''',
    'crgpu_image': r'''
	if (!have_device())
		return NULL;
	crgpu_image image = make_image();
	if (image)
		crgpu_image_destroy(image);
	else
		why = "crgpu_image_create failed";
	return image;''',
    'crgpu_image_view': r'''
	if (!have_device())
		return NULL;
	crgpu_image image = make_image();
	if (!image) {
		why = "crgpu_image_create failed";
		return NULL;
	}
	crgpu_image_view_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	desc.parent_image                        = image;
	desc.type                                = CRGPU_IMAGE_VIEW_TYPE_2D;
	desc.subresource_range.aspect_mask       = CRGPU_IMAGE_ASPECT_COLOR;
	desc.subresource_range.mip_level_count   = 1;
	desc.subresource_range.array_layer_count = 1;
	crgpu_image_view view                  = NULL;
	if (crgpu_image_view_create(image, &desc, &view) == CR_SUCCESS)
		crgpu_image_view_destroy(view);
	else
		why = "crgpu_image_view_create failed";
	crgpu_image_destroy(image);
	return view;''',
    'crgpu_heap': r'''
	if (!have_device())
		return NULL;
	crgpu_heap_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type      = CR_STRUCT_TYPE_GPU_HEAP_DESC;
	desc.memory_kind    = CRGPU_MEMORY_KIND_DEVICE;
	desc.resource_class = CRGPU_HEAP_RESOURCE_CLASS_BUFFERS;
	desc.size           = CRGPU_HEAP_SIZE_GRANULARITY;
	crgpu_heap heap   = NULL;
	if (crgpu_heap_create(gpu_device, &desc, &heap) == CR_SUCCESS)
		crgpu_heap_destroy(heap);
	else
		why = "crgpu_heap_create failed";
	return heap;''',
    'crgpu_command_allocator': r'''
	if (!have_device())
		return NULL;
	crgpu_command_allocator allocator = make_command_allocator();
	if (allocator)
		crgpu_command_allocator_destroy(allocator);
	else
		why = "crgpu_command_allocator_create failed";
	return allocator;''',
    'crgpu_command_list': r'''
	if (!have_device())
		return NULL;
	crgpu_command_allocator allocator = make_command_allocator();
	if (!allocator) {
		why = "crgpu_command_allocator_create failed";
		return NULL;
	}
	crgpu_command_list cmd = NULL;
	if (crgpu_command_list_create(allocator, &cmd) == CR_SUCCESS)
		crgpu_command_list_destroy(cmd);
	else
		why = "crgpu_command_list_create failed";
	crgpu_command_allocator_destroy(allocator);
	return cmd;''',
    'crgpu_fence': r'''
	if (!have_device())
		return NULL;
	crgpu_fence fence = NULL;
	if (crgpu_fence_create(gpu_device, CRGPU_FENCE_FLAG_NONE, 0, &fence) == CR_SUCCESS)
		crgpu_fence_destroy(fence);
	else
		why = "crgpu_fence_create failed";
	return fence;''',
    'crgpu_semaphore': r'''
	if (!have_device())
		return NULL;
	crgpu_semaphore semaphore = NULL;
	if (crgpu_semaphore_create(gpu_device, &semaphore) == CR_SUCCESS)
		crgpu_semaphore_destroy(semaphore);
	else
		why = "crgpu_semaphore_create failed";
	return semaphore;''',
    'crgpu_timestamp_set': r'''
	if (!have_device())
		return NULL;
	if (!(gpu_features & CRGPU_FEATURE_PASS_TIMESTAMPS)) {
		why = "adapter 0 has no pass timestamps";
		return NULL;
	}
	crgpu_timestamp_set set = NULL;
	if (crgpu_timestamp_set_create(gpu_device, gpu_queue, 2, &set) == CR_SUCCESS)
		crgpu_timestamp_set_destroy(set);
	else
		why = "crgpu_timestamp_set_create failed";
	return set;''',
    'crgpu_acceleration_structure': r'''
	/* Its own instance and device, on the first adapter with ray queries
	 * (the shared device is adapter 0's, which may have none), both gone
	 * after: only the handle is read once it is destroyed. */
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "no CrGPU instance";
		return NULL;
	}
	const crgpu_adapter *adapters = NULL;
	uint32_t count                  = 0;
	crgpu_adapter_enumerate(instance, &adapters, &count);
	crgpu_adapter adapter = NULL;
	for (uint32_t i = 0; i < count && !adapter; ++i) {
		CRGPU_ADAPTER_PROPERTIES properties;
		memset(&properties, 0, sizeof(properties));
		properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
		if (crgpu_adapter_get_properties(adapters[i], &properties) == CR_SUCCESS &&
		    (properties.supported_features & CRGPU_FEATURE_RAY_QUERY))
			adapter = adapters[i];
	}
	crgpu_acceleration_structure structure = NULL;
	if (!adapter) {
		why = "no adapter has ray queries";
	} else {
		crgpu_device_desc device_desc;
		memset(&device_desc, 0, sizeof(device_desc));
		device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
		device_desc.required_features  = CRGPU_FEATURE_RAY_QUERY;
		device_desc.direct_queue_count = 1;
		crgpu_device device          = NULL;
		if (crgpu_device_create(adapter, &device_desc, &device) != CR_SUCCESS) {
			why = "crgpu_device_create with CRGPU_FEATURE_RAY_QUERY failed";
		} else {
			crgpu_acceleration_structure_desc desc;
			memset(&desc, 0, sizeof(desc));
			desc.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
			desc.level     = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
			desc.size      = 4096;
			if (crgpu_acceleration_structure_create(device, &desc, &structure) == CR_SUCCESS)
				crgpu_acceleration_structure_destroy(structure);
			else
				why = "crgpu_acceleration_structure_create failed";
			crgpu_device_destroy(device);
		}
	}
	crgpu_instance_destroy(instance);
	return structure;''',
    'crgpu_sampler': r'''
	if (!have_device())
		return NULL;
	crgpu_sampler_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type          = CR_STRUCT_TYPE_GPU_SAMPLER_DESC;
	desc.mag_filter         = CRGPU_FILTER_NEAREST;
	desc.min_filter         = CRGPU_FILTER_NEAREST;
	desc.mipmap_filter      = CRGPU_FILTER_NEAREST;
	desc.address_mode_u     = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_v     = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_w     = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.max_anisotropy     = 1.0f;
	crgpu_sampler sampler = NULL;
	if (crgpu_sampler_create(gpu_device, &desc, &sampler) == CR_SUCCESS)
		crgpu_sampler_destroy(sampler);
	else
		why = "crgpu_sampler_create failed";
	return sampler;''',
    'crgpu_shader_module': r'''
	if (!have_device())
		return NULL;
	crgpu_shader_module module =
	    make_module(&crgpu_test_objects_double_comp);
	if (module)
		crgpu_shader_module_destroy(module);
	else
		why = "crgpu_shader_module_create failed";
	return module;''',
    'crgpu_descriptor_set_layout': r'''
	if (!have_device())
		return NULL;
	const CRGPU_DESCRIPTOR_TYPE types[1] = {CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
	crgpu_descriptor_set_layout layout   = make_set_layout(types, 1, CRGPU_SHADER_STAGE_FRAGMENT);
	if (layout)
		crgpu_descriptor_set_layout_destroy(layout);
	else
		why = "crgpu_descriptor_set_layout_create failed";
	return layout;''',
    'crgpu_descriptor_pool': r'''
	if (!have_device())
		return NULL;
	crgpu_descriptor_pool_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                     = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	desc.max_sets                      = 1;
	desc.pool_sizes[0].descriptor_type = CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	desc.pool_sizes[0].count           = 1;
	desc.pool_size_count               = 1;
	crgpu_descriptor_pool pool       = NULL;
	if (crgpu_descriptor_pool_create(gpu_device, &desc, &pool) == CR_SUCCESS)
		crgpu_descriptor_pool_destroy(pool);
	else
		why = "crgpu_descriptor_pool_create failed";
	return pool;''',
    'crgpu_compute_pipeline': r'''
	if (!have_device())
		return NULL;
	crgpu_shader_module module =
	    make_module(&crgpu_test_objects_double_comp);
	const CRGPU_DESCRIPTOR_TYPE types[2] = {CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	crgpu_descriptor_set_layout layout   = make_set_layout(types, 2, CRGPU_SHADER_STAGE_COMPUTE);
	crgpu_compute_pipeline pipeline      = NULL;
	if (module && layout) {
		crgpu_compute_pipeline_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type                           = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
		desc.compute_module                      = module;
		desc.descriptor_set_layouts[0]           = layout;
		desc.descriptor_set_layout_count         = 1;
		desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_COMPUTE;
		desc.push_constant_ranges[0].size        = 2 * sizeof(uint32_t);
		desc.push_constant_range_count           = 1;
		if (crgpu_compute_pipeline_create(gpu_device, &desc, &pipeline) == CR_SUCCESS)
			crgpu_compute_pipeline_destroy(pipeline);
		else
			why = "crgpu_compute_pipeline_create failed";
	} else {
		why = "no shader module or set layout for a compute pipeline";
	}
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
	return pipeline;''',
    'crgpu_graphics_pipeline': r'''
	if (!have_device())
		return NULL;
	/* tests/shaders/objects_draw.vert and .frag, as crgpu_objects_test
	 * draws with them. */
	crgpu_shader_module vertex =
	    make_module(&crgpu_test_objects_draw_vert);
	crgpu_shader_module fragment =
	    make_module(&crgpu_test_objects_draw_frag);
	const CRGPU_DESCRIPTOR_TYPE types[3] = {CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
	                                          CRGPU_DESCRIPTOR_TYPE_SAMPLER};
	crgpu_descriptor_set_layout layout   = make_set_layout(types, 3, CRGPU_SHADER_STAGE_FRAGMENT);
	crgpu_graphics_pipeline pipeline       = NULL;
	if (vertex && fragment && layout) {
		crgpu_graphics_pipeline_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.base.type                           = CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC;
		desc.vertex_module                       = vertex;
		desc.fragment_module                     = fragment;
		desc.vertex_attributes[0].location       = 0;
		desc.vertex_attributes[0].binding        = 0;
		desc.vertex_attributes[0].format         = CRGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attributes[1].location       = 1;
		desc.vertex_attributes[1].binding        = 1;
		desc.vertex_attributes[1].format         = CRGPU_FORMAT_R32G32_FLOAT;
		desc.vertex_attribute_count              = 2;
		desc.vertex_bindings[0].binding          = 0;
		desc.vertex_bindings[0].stride           = 2 * sizeof(float);
		desc.vertex_bindings[1].binding          = 1;
		desc.vertex_bindings[1].stride           = 2 * sizeof(float);
		desc.vertex_bindings[1].per_instance     = true;
		desc.vertex_binding_count                = 2;
		desc.primitive_topology                  = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		desc.cull_mode                           = CRGPU_CULL_MODE_NONE;
		desc.depth_compare_op                    = CRGPU_COMPARE_OP_ALWAYS;
		desc.color_attachment_formats[0]         = CRGPU_FORMAT_R8G8B8A8_UNORM;
		desc.color_attachment_count              = 1;
		desc.depth_format                        = CRGPU_FORMAT_D32_FLOAT;
		desc.descriptor_set_layouts[0]           = layout;
		desc.descriptor_set_layout_count         = 1;
		desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_VERTEX | CRGPU_SHADER_STAGE_FRAGMENT;
		desc.push_constant_ranges[0].size        = 8 * sizeof(float);
		desc.push_constant_range_count           = 1;
		if (crgpu_graphics_pipeline_create(gpu_device, &desc, &pipeline) == CR_SUCCESS)
			crgpu_graphics_pipeline_destroy(pipeline);
		else
			why = "crgpu_graphics_pipeline_create failed";
	} else {
		why = "no shader modules or set layout for a pipeline";
	}
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(fragment);
	crgpu_shader_module_destroy(vertex);
	return pipeline;''',
    'crgpu_surface': r'''
	if (!gpu_instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_surface surface = make_surface();
	if (surface)
		crgpu_surface_destroy(surface);
	return surface;''',
    'crgpu_swap_chain': r'''
	if (!have_device())
		return NULL;
	if (!(gpu_features & CRGPU_FEATURE_PRESENT)) {
		why = "adapter 0 can't present";
		return NULL;
	}
	crgpu_surface surface = make_surface();
	if (!surface)
		return NULL;
	crgpu_swap_chain_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	desc.surface   = surface;
	desc.queue     = gpu_queue;
	desc.width     = 64;
	desc.height    = 64;
#if CRGPU_COMPILE_METAL_API
	desc.buffer_count = 1;
#else
	desc.buffer_count = 2;
#endif
	desc.format                   = CRGPU_FORMAT_B8G8R8A8_UNORM;
	desc.present_mode             = CRGPU_PRESENT_MODE_FIFO;
	desc.present_scaling          = CRGPU_PRESENT_SCALING_ONE_TO_ONE;
	desc.fullscreen_mode          = CRGPU_FULLSCREEN_MODE_WINDOWED;
	crgpu_swap_chain swap_chain = NULL;
	if (crgpu_swap_chain_create(gpu_device, &desc, &swap_chain) == CR_SUCCESS)
		crgpu_swap_chain_destroy(swap_chain);
	else
		why = "crgpu_swap_chain_create failed";
	crgpu_surface_destroy(surface);
	return swap_chain;''',
    # ---- CrInput
    'crinput_instance': r'''
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
	CRINPUT_INSTANCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type  = CR_STRUCT_TYPE_INPUT_INSTANCE_DESC;
	desc.allocator  = &allocator;
	desc.native_api = api;
#if CRINPUT_COMPILE_XINPUT || CRINPUT_COMPILE_EVDEV
	desc.read_in_background = true; /* these always read */
#endif
	crinput_instance instance = NULL;
	if (api == CRINPUT_NATIVE_API_NONE || crinput_instance_create(&desc, &instance) != CR_SUCCESS) {
		why = "no CrInput instance in this build";
		return NULL;
	}
	crinput_instance_destroy(instance);
	return instance;''',
    # ---- CrNet
    'crnet_instance': r'''
	crnet_instance instance = make_net_instance();
	if (instance)
		crnet_instance_destroy(instance);
	return instance;''',
    'crnet_resolver': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_resolver_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type            = CR_STRUCT_TYPE_NET_RESOLVER_DESC;
	desc.host                 = "localhost";
	crnet_resolver resolver = NULL;
	if (crnet_resolver_create(instance, &desc, &resolver) == CR_SUCCESS) {
		/* Run once, aborted first so that it returns at once, and
		 * destroyed: then it is freed. */
		crnet_resolver_abort(resolver);
		crnet_resolver_run(resolver);
		crnet_resolver_destroy(resolver);
	} else {
		why = "crnet_resolver_create failed";
	}
	crnet_instance_destroy(instance);
	return resolver;''',
    'crnet_tcp_listener': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_tcp_listener_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                = CR_STRUCT_TYPE_NET_TCP_LISTENER_DESC;
	desc.address                  = "127.0.0.1";
	desc.backlog                  = 1;
	crnet_tcp_listener listener = NULL;
	if (crnet_tcp_listener_create(instance, &desc, &listener, NULL) == CR_SUCCESS)
		crnet_tcp_listener_destroy(listener);
	else
		why = "no loopback listener";
	crnet_instance_destroy(instance);
	return listener;''',
    'crnet_tcp_socket': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_tcp_socket socket = NULL;
	if (crnet_tcp_socket_create(instance, &socket) == CR_SUCCESS)
		crnet_tcp_socket_destroy(socket);
	else
		why = "crnet_tcp_socket_create failed";
	crnet_instance_destroy(instance);
	return socket;''',
    'crnet_http_client': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_http_client client = make_http_client(instance);
	if (client)
		crnet_http_client_destroy(client);
	crnet_instance_destroy(instance);
	return client;''',
    'crnet_http_transfer': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_http_client client     = make_http_client(instance);
	crnet_http_transfer transfer = NULL;
	if (client) {
		if (crnet_http_transfer_create(client, &transfer) == CR_SUCCESS)
			crnet_http_transfer_destroy(transfer);
		else
			why = "crnet_http_transfer_create failed";
		crnet_http_client_destroy(client);
	}
	crnet_instance_destroy(instance);
	return transfer;''',
    'crnet_websocket': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_websocket socket = NULL;
	if (crnet_websocket_create(instance, &socket) == CR_SUCCESS)
		crnet_websocket_destroy(socket);
	else
		why = "crnet_websocket_create failed (no WebSocket in this build)";
	crnet_instance_destroy(instance);
	return socket;''',
    'crnet_wait_set': r'''
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_wait_set set = NULL;
	if (crnet_wait_set_create(instance, &set, NULL) == CR_SUCCESS)
		crnet_wait_set_destroy(set);
	else
		why = "crnet_wait_set_create failed";
	crnet_instance_destroy(instance);
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
