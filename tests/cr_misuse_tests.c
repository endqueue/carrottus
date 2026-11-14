// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.
// Every public function of Carrottus called with every argument zeroed
// (ROADMAP.md in the workspace, section 3.1). None may crash; one
// returning CR_RESULT returns an error unless the generator lists zeroes as
// a valid call for it. One returning nothing is called and counted, since
// it can't refuse.
//
// Then, in a Debug build not under a sanitizer, every public function whose
// first parameter is a handle, called with one just destroyed (decision
// X16): no crash, and an error, or the null fallback of a getter.

#include "craudio.h"
#include "crcore.h"
#include "crfiles.h"
#include "crgpu.h"
#include "crinput.h"
#include "crmemory.h"
#include "crnet.h"
#include "crperf.h"
#include "crrandom.h"
#include <stdbool.h>
#include <stdio.h>

static int failures = 0;

static void expect_error(const char *name, CR_RESULT result)
{
	if (result == CR_SUCCESS) {
		printf("FAIL %s: returned CR_SUCCESS for zeroes\n", name);
		failures++;
	}
}

// The freed half: Debug only, since the check is; never under a sanitizer,
// which reports the read of freed memory the check makes.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MISUSE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MISUSE_SANITIZED 1
#endif
#endif
#ifndef MISUSE_SANITIZED
#define MISUSE_SANITIZED 0
#endif
#if defined(_DEBUG) && _DEBUG && !MISUSE_SANITIZED
#define MISUSE_FREED 1
#else
#define MISUSE_FREED 0
#endif

#if MISUSE_FREED
#include "crgpu_test_shaders.h"
#include <stdlib.h>
#include <string.h>

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

static int freed_failures = 0;

static void expect_refused(const char *name, CR_RESULT result)
{
	if (result == CR_SUCCESS) {
		printf("FAIL %s: returned CR_SUCCESS for a destroyed handle\n", name);
		freed_failures++;
	}
}

static void expect_fallback(const char *name, bool same)
{
	if (!same) {
		printf("FAIL %s: returned other than for NULL for a destroyed handle\n", name);
		freed_failures++;
	}
}

/* A craudio_instance just destroyed, or NULL with `why` set. */
static craudio_instance make_dead_craudio_instance(void) {
	craudio_instance instance = make_audio_instance();
	if (instance)
		craudio_instance_destroy(instance);
	return instance;
}

static craudio_instance dead_craudio_instance(void) {
	static bool told = false;
	why = "";
	craudio_instance handle = make_dead_craudio_instance();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: craudio_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A craudio_device just destroyed, or NULL with `why` set. */
static craudio_device make_dead_craudio_device(void) {
	/* A device is the instance's: it goes with it. */
	craudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	craudio_device device = first_output(instance);
	craudio_instance_destroy(instance);
	return device;
}

static craudio_device dead_craudio_device(void) {
	static bool told = false;
	why = "";
	craudio_device handle = make_dead_craudio_device();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: craudio_device: %s\n", why);
		told = true;
	}
	return handle;
}

/* A craudio_stream just destroyed, or NULL with `why` set. */
static craudio_stream make_dead_craudio_stream(void) {
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
	return stream;
}

static craudio_stream dead_craudio_stream(void) {
	static bool told = false;
	why = "";
	craudio_stream handle = make_dead_craudio_stream();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: craudio_stream: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crfiles_instance just destroyed, or NULL with `why` set. */
static crfiles_instance make_dead_crfiles_instance(void) {
	crfiles_instance instance = make_file_instance();
	if (instance)
		crfiles_instance_destroy(instance);
	return instance;
}

static crfiles_instance dead_crfiles_instance(void) {
	static bool told = false;
	why = "";
	crfiles_instance handle = make_dead_crfiles_instance();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crfiles_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crfiles_file just destroyed, or NULL with `why` set. */
static crfiles_file make_dead_crfiles_file(void) {
	crfiles_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	crfiles_file file = make_file(instance);
	if (file)
		crfiles_file_close(file);
	crfiles_path_remove(instance, FREED_FILE);
	crfiles_instance_destroy(instance);
	return file;
}

static crfiles_file dead_crfiles_file(void) {
	static bool told = false;
	why = "";
	crfiles_file handle = make_dead_crfiles_file();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crfiles_file: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crfiles_library just destroyed, or NULL with `why` set. */
static crfiles_library make_dead_crfiles_library(void) {
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
	return library;
}

static crfiles_library dead_crfiles_library(void) {
	static bool told = false;
	why = "";
	crfiles_library handle = make_dead_crfiles_library();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crfiles_library: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crfiles_listing just destroyed, or NULL with `why` set. */
static crfiles_listing make_dead_crfiles_listing(void) {
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
	return listing;
}

static crfiles_listing dead_crfiles_listing(void) {
	static bool told = false;
	why = "";
	crfiles_listing handle = make_dead_crfiles_listing();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crfiles_listing: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crfiles_mapping just destroyed, or NULL with `why` set. */
static crfiles_mapping make_dead_crfiles_mapping(void) {
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
	return mapping;
}

static crfiles_mapping dead_crfiles_mapping(void) {
	static bool told = false;
	why = "";
	crfiles_mapping handle = make_dead_crfiles_mapping();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crfiles_mapping: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_device just destroyed, or NULL with `why` set. */
static crgpu_device make_dead_crgpu_device(void) {
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
	return device;
}

static crgpu_device dead_crgpu_device(void) {
	static bool told = false;
	why = "";
	crgpu_device handle = make_dead_crgpu_device();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_device: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_heap just destroyed, or NULL with `why` set. */
static crgpu_heap make_dead_crgpu_heap(void) {
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
	return heap;
}

static crgpu_heap dead_crgpu_heap(void) {
	static bool told = false;
	why = "";
	crgpu_heap handle = make_dead_crgpu_heap();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_heap: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_acceleration_structure just destroyed, or NULL with `why` set. */
static crgpu_acceleration_structure make_dead_crgpu_acceleration_structure(void) {
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
	return structure;
}

static crgpu_acceleration_structure dead_crgpu_acceleration_structure(void) {
	static bool told = false;
	why = "";
	crgpu_acceleration_structure handle = make_dead_crgpu_acceleration_structure();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_acceleration_structure: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_instance just destroyed, or NULL with `why` set. */
static crgpu_instance make_dead_crgpu_instance(void) {
	crgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_instance_destroy(instance);
	return instance;
}

static crgpu_instance dead_crgpu_instance(void) {
	static bool told = false;
	why = "";
	crgpu_instance handle = make_dead_crgpu_instance();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_adapter just destroyed, or NULL with `why` set. */
static crgpu_adapter make_dead_crgpu_adapter(void) {
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
	return adapter;
}

static crgpu_adapter dead_crgpu_adapter(void) {
	static bool told = false;
	why = "";
	crgpu_adapter handle = make_dead_crgpu_adapter();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_adapter: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_buffer just destroyed, or NULL with `why` set. */
static crgpu_buffer make_dead_crgpu_buffer(void) {
	if (!have_device())
		return NULL;
	crgpu_buffer buffer = make_buffer();
	if (buffer)
		crgpu_buffer_destroy(buffer);
	else
		why = "crgpu_buffer_create failed";
	return buffer;
}

static crgpu_buffer dead_crgpu_buffer(void) {
	static bool told = false;
	why = "";
	crgpu_buffer handle = make_dead_crgpu_buffer();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_buffer: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_buffer_view just destroyed, or NULL with `why` set. */
static crgpu_buffer_view make_dead_crgpu_buffer_view(void) {
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
	return view;
}

static crgpu_buffer_view dead_crgpu_buffer_view(void) {
	static bool told = false;
	why = "";
	crgpu_buffer_view handle = make_dead_crgpu_buffer_view();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_buffer_view: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_command_allocator just destroyed, or NULL with `why` set. */
static crgpu_command_allocator make_dead_crgpu_command_allocator(void) {
	if (!have_device())
		return NULL;
	crgpu_command_allocator allocator = make_command_allocator();
	if (allocator)
		crgpu_command_allocator_destroy(allocator);
	else
		why = "crgpu_command_allocator_create failed";
	return allocator;
}

static crgpu_command_allocator dead_crgpu_command_allocator(void) {
	static bool told = false;
	why = "";
	crgpu_command_allocator handle = make_dead_crgpu_command_allocator();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_command_allocator: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_command_list just destroyed, or NULL with `why` set. */
static crgpu_command_list make_dead_crgpu_command_list(void) {
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
	return cmd;
}

static crgpu_command_list dead_crgpu_command_list(void) {
	static bool told = false;
	why = "";
	crgpu_command_list handle = make_dead_crgpu_command_list();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_command_list: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_command_queue just destroyed, or NULL with `why` set. */
static crgpu_command_queue make_dead_crgpu_command_queue(void) {
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
	return queue;
}

static crgpu_command_queue dead_crgpu_command_queue(void) {
	static bool told = false;
	why = "";
	crgpu_command_queue handle = make_dead_crgpu_command_queue();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_command_queue: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_compute_pipeline just destroyed, or NULL with `why` set. */
static crgpu_compute_pipeline make_dead_crgpu_compute_pipeline(void) {
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
	return pipeline;
}

static crgpu_compute_pipeline dead_crgpu_compute_pipeline(void) {
	static bool told = false;
	why = "";
	crgpu_compute_pipeline handle = make_dead_crgpu_compute_pipeline();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_compute_pipeline: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_descriptor_pool just destroyed, or NULL with `why` set. */
static crgpu_descriptor_pool make_dead_crgpu_descriptor_pool(void) {
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
	return pool;
}

static crgpu_descriptor_pool dead_crgpu_descriptor_pool(void) {
	static bool told = false;
	why = "";
	crgpu_descriptor_pool handle = make_dead_crgpu_descriptor_pool();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_descriptor_pool: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_descriptor_set_layout just destroyed, or NULL with `why` set. */
static crgpu_descriptor_set_layout make_dead_crgpu_descriptor_set_layout(void) {
	if (!have_device())
		return NULL;
	const CRGPU_DESCRIPTOR_TYPE types[1] = {CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
	crgpu_descriptor_set_layout layout   = make_set_layout(types, 1, CRGPU_SHADER_STAGE_FRAGMENT);
	if (layout)
		crgpu_descriptor_set_layout_destroy(layout);
	else
		why = "crgpu_descriptor_set_layout_create failed";
	return layout;
}

static crgpu_descriptor_set_layout dead_crgpu_descriptor_set_layout(void) {
	static bool told = false;
	why = "";
	crgpu_descriptor_set_layout handle = make_dead_crgpu_descriptor_set_layout();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_descriptor_set_layout: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_fence just destroyed, or NULL with `why` set. */
static crgpu_fence make_dead_crgpu_fence(void) {
	if (!have_device())
		return NULL;
	crgpu_fence fence = NULL;
	if (crgpu_fence_create(gpu_device, CRGPU_FENCE_FLAG_NONE, 0, &fence) == CR_SUCCESS)
		crgpu_fence_destroy(fence);
	else
		why = "crgpu_fence_create failed";
	return fence;
}

static crgpu_fence dead_crgpu_fence(void) {
	static bool told = false;
	why = "";
	crgpu_fence handle = make_dead_crgpu_fence();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_fence: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_graphics_pipeline just destroyed, or NULL with `why` set. */
static crgpu_graphics_pipeline make_dead_crgpu_graphics_pipeline(void) {
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
	return pipeline;
}

static crgpu_graphics_pipeline dead_crgpu_graphics_pipeline(void) {
	static bool told = false;
	why = "";
	crgpu_graphics_pipeline handle = make_dead_crgpu_graphics_pipeline();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_graphics_pipeline: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_image just destroyed, or NULL with `why` set. */
static crgpu_image make_dead_crgpu_image(void) {
	if (!have_device())
		return NULL;
	crgpu_image image = make_image();
	if (image)
		crgpu_image_destroy(image);
	else
		why = "crgpu_image_create failed";
	return image;
}

static crgpu_image dead_crgpu_image(void) {
	static bool told = false;
	why = "";
	crgpu_image handle = make_dead_crgpu_image();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_image: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_image_view just destroyed, or NULL with `why` set. */
static crgpu_image_view make_dead_crgpu_image_view(void) {
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
	return view;
}

static crgpu_image_view dead_crgpu_image_view(void) {
	static bool told = false;
	why = "";
	crgpu_image_view handle = make_dead_crgpu_image_view();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_image_view: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_sampler just destroyed, or NULL with `why` set. */
static crgpu_sampler make_dead_crgpu_sampler(void) {
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
	return sampler;
}

static crgpu_sampler dead_crgpu_sampler(void) {
	static bool told = false;
	why = "";
	crgpu_sampler handle = make_dead_crgpu_sampler();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_sampler: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_semaphore just destroyed, or NULL with `why` set. */
static crgpu_semaphore make_dead_crgpu_semaphore(void) {
	if (!have_device())
		return NULL;
	crgpu_semaphore semaphore = NULL;
	if (crgpu_semaphore_create(gpu_device, &semaphore) == CR_SUCCESS)
		crgpu_semaphore_destroy(semaphore);
	else
		why = "crgpu_semaphore_create failed";
	return semaphore;
}

static crgpu_semaphore dead_crgpu_semaphore(void) {
	static bool told = false;
	why = "";
	crgpu_semaphore handle = make_dead_crgpu_semaphore();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_semaphore: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_shader_module just destroyed, or NULL with `why` set. */
static crgpu_shader_module make_dead_crgpu_shader_module(void) {
	if (!have_device())
		return NULL;
	crgpu_shader_module module =
	    make_module(&crgpu_test_objects_double_comp);
	if (module)
		crgpu_shader_module_destroy(module);
	else
		why = "crgpu_shader_module_create failed";
	return module;
}

static crgpu_shader_module dead_crgpu_shader_module(void) {
	static bool told = false;
	why = "";
	crgpu_shader_module handle = make_dead_crgpu_shader_module();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_shader_module: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_surface just destroyed, or NULL with `why` set. */
static crgpu_surface make_dead_crgpu_surface(void) {
	if (!gpu_instance) {
		why = "crgpu_instance_create failed";
		return NULL;
	}
	crgpu_surface surface = make_surface();
	if (surface)
		crgpu_surface_destroy(surface);
	return surface;
}

static crgpu_surface dead_crgpu_surface(void) {
	static bool told = false;
	why = "";
	crgpu_surface handle = make_dead_crgpu_surface();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_surface: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_swap_chain just destroyed, or NULL with `why` set. */
static crgpu_swap_chain make_dead_crgpu_swap_chain(void) {
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
	return swap_chain;
}

static crgpu_swap_chain dead_crgpu_swap_chain(void) {
	static bool told = false;
	why = "";
	crgpu_swap_chain handle = make_dead_crgpu_swap_chain();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_swap_chain: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crgpu_timestamp_set just destroyed, or NULL with `why` set. */
static crgpu_timestamp_set make_dead_crgpu_timestamp_set(void) {
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
	return set;
}

static crgpu_timestamp_set dead_crgpu_timestamp_set(void) {
	static bool told = false;
	why = "";
	crgpu_timestamp_set handle = make_dead_crgpu_timestamp_set();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crgpu_timestamp_set: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crinput_instance just destroyed, or NULL with `why` set. */
static crinput_instance make_dead_crinput_instance(void) {
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
	return instance;
}

static crinput_instance dead_crinput_instance(void) {
	static bool told = false;
	why = "";
	crinput_instance handle = make_dead_crinput_instance();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crinput_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_instance just destroyed, or NULL with `why` set. */
static crnet_instance make_dead_crnet_instance(void) {
	crnet_instance instance = make_net_instance();
	if (instance)
		crnet_instance_destroy(instance);
	return instance;
}

static crnet_instance dead_crnet_instance(void) {
	static bool told = false;
	why = "";
	crnet_instance handle = make_dead_crnet_instance();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_http_client just destroyed, or NULL with `why` set. */
static crnet_http_client make_dead_crnet_http_client(void) {
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_http_client client = make_http_client(instance);
	if (client)
		crnet_http_client_destroy(client);
	crnet_instance_destroy(instance);
	return client;
}

static crnet_http_client dead_crnet_http_client(void) {
	static bool told = false;
	why = "";
	crnet_http_client handle = make_dead_crnet_http_client();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_http_client: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_http_transfer just destroyed, or NULL with `why` set. */
static crnet_http_transfer make_dead_crnet_http_transfer(void) {
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_http_client client     = make_http_client(instance);
	crnet_http_transfer transfer = NULL;
	if (client) {
		if (crnet_http_transfer_create(client, &transfer) == CR_SUCCESS) {
			// Started, as a wait set takes one (crnet_wait_set_add_http_transfer):
			// begun with CRNET_NO_WAIT towards a port nothing serves, so the
			// destroyed handle is one whose request was under way.
			crnet_http_transfer_desc desc;
			memset(&desc, 0, sizeof(desc));
			desc.base.type  = CR_STRUCT_TYPE_NET_HTTP_TRANSFER_DESC;
			desc.method     = "GET";
			desc.url        = "http://127.0.0.1:9/";
			desc.timeout_ms = CRNET_NO_WAIT;
			(void)crnet_http_transfer_start(transfer, &desc);
			crnet_http_transfer_destroy(transfer);
		} else {
			why = "crnet_http_transfer_create failed";
		}
		crnet_http_client_destroy(client);
	}
	crnet_instance_destroy(instance);
	return transfer;
}

static crnet_http_transfer dead_crnet_http_transfer(void) {
	static bool told = false;
	why = "";
	crnet_http_transfer handle = make_dead_crnet_http_transfer();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_http_transfer: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_resolver just destroyed, or NULL with `why` set. */
static crnet_resolver make_dead_crnet_resolver(void) {
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
	return resolver;
}

static crnet_resolver dead_crnet_resolver(void) {
	static bool told = false;
	why = "";
	crnet_resolver handle = make_dead_crnet_resolver();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_resolver: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_tcp_listener just destroyed, or NULL with `why` set. */
static crnet_tcp_listener make_dead_crnet_tcp_listener(void) {
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
	return listener;
}

static crnet_tcp_listener dead_crnet_tcp_listener(void) {
	static bool told = false;
	why = "";
	crnet_tcp_listener handle = make_dead_crnet_tcp_listener();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_tcp_listener: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_tcp_socket just destroyed, or NULL with `why` set. */
static crnet_tcp_socket make_dead_crnet_tcp_socket(void) {
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_tcp_socket socket = NULL;
	if (crnet_tcp_socket_create(instance, &socket) == CR_SUCCESS)
		crnet_tcp_socket_destroy(socket);
	else
		why = "crnet_tcp_socket_create failed";
	crnet_instance_destroy(instance);
	return socket;
}

static crnet_tcp_socket dead_crnet_tcp_socket(void) {
	static bool told = false;
	why = "";
	crnet_tcp_socket handle = make_dead_crnet_tcp_socket();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_tcp_socket: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_wait_set just destroyed, or NULL with `why` set. */
static crnet_wait_set make_dead_crnet_wait_set(void) {
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_wait_set set = NULL;
	if (crnet_wait_set_create(instance, &set, NULL) == CR_SUCCESS)
		crnet_wait_set_destroy(set);
	else
		why = "crnet_wait_set_create failed";
	crnet_instance_destroy(instance);
	return set;
}

static crnet_wait_set dead_crnet_wait_set(void) {
	static bool told = false;
	why = "";
	crnet_wait_set handle = make_dead_crnet_wait_set();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_wait_set: %s\n", why);
		told = true;
	}
	return handle;
}

/* A crnet_websocket just destroyed, or NULL with `why` set. */
static crnet_websocket make_dead_crnet_websocket(void) {
	crnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	crnet_websocket socket = NULL;
	if (crnet_websocket_create(instance, &socket) == CR_SUCCESS)
		crnet_websocket_destroy(socket);
	else
		why = "crnet_websocket_create failed (no WebSocket in this build)";
	crnet_instance_destroy(instance);
	return socket;
}

static crnet_websocket dead_crnet_websocket(void) {
	static bool told = false;
	why = "";
	crnet_websocket handle = make_dead_crnet_websocket();
	if (!handle && !told) {
		printf("cr_misuse_tests: not covered: crnet_websocket: %s\n", why);
		told = true;
	}
	return handle;
}

static int freed_half(int *out_calls, int *out_unchecked, int *out_skipped)
{
	int calls = 0;
	int unchecked = 0;
	int skipped = 0;
	/* crgpu_descriptor_set: no public function takes one first. */
	/* crgpu_pipeline_layout: no public function takes one first. */
	/* crnet_resolver_run: not called with a destroyed handle: may start after the resolver is destroyed (crnet.h), so it has no stale check, and a resolver that has been run and destroyed is freed memory. */
	freed_setup();
	{
		craudio_instance dead = dead_craudio_instance();
		if (dead) {
			expect_refused("craudio_device_enumerate", craudio_device_enumerate(dead, (CRAUDIO_DIRECTION){0}, (craudio_device **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_device dead = dead_craudio_device();
		if (dead) {
			expect_refused("craudio_device_get_properties", craudio_device_get_properties(dead, (CRAUDIO_DEVICE_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_device dead = dead_craudio_device();
		if (dead) {
			expect_refused("craudio_device_get_timing_caps", craudio_device_get_timing_caps(dead, (const CRAUDIO_FORMAT *){0}, (CRAUDIO_STREAM_FLAGS){0}, (CRAUDIO_TIMING_CAPS *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_device dead = dead_craudio_device();
		if (dead) {
			expect_refused("craudio_device_set_sample_rate", craudio_device_set_sample_rate(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_instance dead = dead_craudio_instance();
		if (dead) {
			expect_refused("craudio_instance_destroy", craudio_instance_destroy(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_instance dead = dead_craudio_instance();
		if (dead) {
			expect_fallback("craudio_instance_get_native_api", craudio_instance_get_native_api(dead) == craudio_instance_get_native_api((craudio_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_instance dead = dead_craudio_instance();
		if (dead) {
			expect_refused("craudio_instance_set_device_event_callback", craudio_instance_set_device_event_callback(dead, (CRAUDIO_DEVICE_EVENT_CALLBACK){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			craudio_stream_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			expect_refused("craudio_stream_get_config", craudio_stream_get_config(dead, (CRAUDIO_STREAM_CONFIG *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			expect_refused("craudio_stream_get_latency_frames", craudio_stream_get_latency_frames(dead, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			expect_refused("craudio_stream_get_status", craudio_stream_get_status(dead, (CRAUDIO_STREAM_STATUS *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			expect_refused("craudio_stream_start", craudio_stream_start(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		craudio_stream dead = dead_craudio_stream();
		if (dead) {
			expect_refused("craudio_stream_stop", craudio_stream_stop(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_directory_create", crfiles_directory_create(dead, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_directory_remove", crfiles_directory_remove(dead, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_directory_rename", crfiles_directory_rename(dead, (const char *){0}, (const char *){0}, (CRFILES_FLUSH_LEVEL){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			crfiles_file_close(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_flush", crfiles_file_flush(dead, (CRFILES_FLUSH_LEVEL){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_get_size", crfiles_file_get_size(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_lock", crfiles_file_lock(dead, (CRFILES_LOCK){0}, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_map", crfiles_file_map(dead, (uint64_t){0}, (uint64_t){0}, (crfiles_mapping *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_file_open", crfiles_file_open(dead, (const crfiles_open_desc *){0}, (crfiles_file *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_read_at", crfiles_file_read_at(dead, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_set_size", crfiles_file_set_size(dead, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_unlock", crfiles_file_unlock(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_file dead = dead_crfiles_file();
		if (dead) {
			expect_refused("crfiles_file_write_at", crfiles_file_write_at(dead, (uint64_t){0}, (const void *){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			crfiles_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_library dead = dead_crfiles_library();
		if (dead) {
			crfiles_library_close(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_library dead = dead_crfiles_library();
		if (dead) {
			expect_refused("crfiles_library_find", crfiles_library_find(dead, (const char *){0}, (crfiles_library_function *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_library_open", crfiles_library_open(dead, (const crfiles_library_desc *){0}, (crfiles_library *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_listing dead = dead_crfiles_listing();
		if (dead) {
			crfiles_listing_close(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_listing dead = dead_crfiles_listing();
		if (dead) {
			expect_refused("crfiles_listing_next", crfiles_listing_next(dead, (CRFILES_LISTING_ENTRY *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_listing_open", crfiles_listing_open(dead, (const crfiles_listing_desc *){0}, (crfiles_listing *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_mapping dead = dead_crfiles_mapping();
		if (dead) {
			expect_fallback("crfiles_mapping_get_size", crfiles_mapping_get_size(dead) == crfiles_mapping_get_size((crfiles_mapping){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_mapping dead = dead_crfiles_mapping();
		if (dead) {
			crfiles_mapping_release(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_path_copy_mode", crfiles_path_copy_mode(dead, (const char *){0}, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_path_get_info", crfiles_path_get_info(dead, (const char *){0}, (CRFILES_PATH_INFO *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_path_remove", crfiles_path_remove(dead, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crfiles_instance dead = dead_crfiles_instance();
		if (dead) {
			expect_refused("crfiles_path_replace", crfiles_path_replace(dead, (const char *){0}, (const char *){0}, (CRFILES_FLUSH_LEVEL){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_acceleration_structure_create", crgpu_acceleration_structure_create(dead, (const crgpu_acceleration_structure_desc *){0}, (crgpu_acceleration_structure *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_acceleration_structure_create_placed", crgpu_acceleration_structure_create_placed(dead, (uint64_t){0}, (const crgpu_acceleration_structure_desc *){0}, (crgpu_acceleration_structure *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_acceleration_structure dead = dead_crgpu_acceleration_structure();
		if (dead) {
			crgpu_acceleration_structure_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_acceleration_structure dead = dead_crgpu_acceleration_structure();
		if (dead) {
			expect_refused("crgpu_acceleration_structure_get_reference", crgpu_acceleration_structure_get_reference(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_instance dead = dead_crgpu_instance();
		if (dead) {
			expect_refused("crgpu_adapter_enumerate", crgpu_adapter_enumerate(dead, (const crgpu_adapter **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_adapter dead = dead_crgpu_adapter();
		if (dead) {
			expect_refused("crgpu_adapter_get_properties", crgpu_adapter_get_properties(dead, (CRGPU_ADAPTER_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_adapter dead = dead_crgpu_adapter();
		if (dead) {
			expect_refused("crgpu_adapter_supports_surface", crgpu_adapter_supports_surface(dead, (crgpu_surface){0}, (bool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_bindless_register_sampled_image", crgpu_bindless_register_sampled_image(dead, (crgpu_image_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_bindless_register_storage_buffer", crgpu_bindless_register_storage_buffer(dead, (crgpu_buffer_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_bindless_register_storage_image", crgpu_bindless_register_storage_image(dead, (crgpu_image_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			crgpu_bindless_unregister_sampled_image(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			crgpu_bindless_unregister_storage_buffer(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			crgpu_bindless_unregister_storage_image(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_buffer_create", crgpu_buffer_create(dead, (const crgpu_buffer_desc *){0}, (crgpu_buffer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_buffer_create_placed", crgpu_buffer_create_placed(dead, (uint64_t){0}, (const crgpu_buffer_desc *){0}, (crgpu_buffer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			crgpu_buffer_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_flush", crgpu_buffer_flush(dead, (uint64_t){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_get_desc", crgpu_buffer_get_desc(dead, (crgpu_buffer_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_get_gpu_address", crgpu_buffer_get_gpu_address(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_invalidate", crgpu_buffer_invalidate(dead, (uint64_t){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_map", crgpu_buffer_map(dead, (uint64_t){0}, (uint64_t){0}, (void **){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			crgpu_buffer_unmap(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer dead = dead_crgpu_buffer();
		if (dead) {
			expect_refused("crgpu_buffer_view_create", crgpu_buffer_view_create(dead, (const crgpu_buffer_view_desc *){0}, (crgpu_buffer_view *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer_view dead = dead_crgpu_buffer_view();
		if (dead) {
			crgpu_buffer_view_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_buffer_view dead = dead_crgpu_buffer_view();
		if (dead) {
			expect_refused("crgpu_buffer_view_get_desc", crgpu_buffer_view_get_desc(dead, (crgpu_buffer_view_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_command_allocator_create", crgpu_command_allocator_create(dead, (const crgpu_command_allocator_desc *){0}, (crgpu_command_allocator *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_allocator dead = dead_crgpu_command_allocator();
		if (dead) {
			crgpu_command_allocator_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_allocator dead = dead_crgpu_command_allocator();
		if (dead) {
			expect_refused("crgpu_command_allocator_reset", crgpu_command_allocator_reset(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_acceleration_structure_barrier(dead, (const crgpu_acceleration_structure_barrier *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_begin(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_begin_bundle(dead, (const crgpu_bundle_inheritance_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_begin_debug_label(dead, (const char *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_begin_rendering(dead, (const crgpu_rendering_begin_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_compute_bindless_resources(dead, (crgpu_compute_pipeline){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_compute_descriptor_sets(dead, (crgpu_compute_pipeline){0}, (uint32_t){0}, (const crgpu_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_compute_pipeline(dead, (crgpu_compute_pipeline){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_graphics_bindless_resources(dead, (crgpu_graphics_pipeline){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_graphics_descriptor_sets(dead, (crgpu_graphics_pipeline){0}, (uint32_t){0}, (const crgpu_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_bind_graphics_pipeline(dead, (crgpu_graphics_pipeline){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_blit_image(dead, (crgpu_image){0}, (crgpu_image){0}, (const crgpu_image_blit_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			expect_refused("crgpu_command_list_build_acceleration_structures", crgpu_command_list_build_acceleration_structures(dead, (const crgpu_acceleration_structure_build *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_clear_color_attachment(dead, (crgpu_image_view){0}, (float){0}, (float){0}, (float){0}, (float){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_clear_depth_attachment(dead, (crgpu_image_view){0}, (bool){0}, (bool){0}, (float){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			expect_refused("crgpu_command_list_copy_acceleration_structure", crgpu_command_list_copy_acceleration_structure(dead, (crgpu_acceleration_structure){0}, (crgpu_acceleration_structure){0}, (CRGPU_ACCELERATION_STRUCTURE_COPY_MODE){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_copy_buffer(dead, (crgpu_buffer){0}, (crgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (uint64_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_copy_buffer_to_image(dead, (crgpu_buffer){0}, (crgpu_image){0}, (const crgpu_image_buffer_copy_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_copy_image_to_buffer(dead, (crgpu_image){0}, (crgpu_buffer){0}, (const crgpu_image_buffer_copy_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_allocator dead = dead_crgpu_command_allocator();
		if (dead) {
			expect_refused("crgpu_command_list_create", crgpu_command_list_create(dead, (crgpu_command_list *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_dispatch(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_dispatch_mesh(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw(dead, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw_indexed(dead, (uint32_t){0}, (uint32_t){0}, (int32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw_indexed_indirect(dead, (crgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw_indexed_instanced(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw_indirect(dead, (crgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_draw_instanced(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_end(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_end_debug_label(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_end_rendering(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_execute_bundle(dead, (crgpu_command_list){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_image_barrier(dead, (crgpu_image){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_image_barrier_subresource(dead, (crgpu_image){0}, (const crgpu_image_view_desc_subresource_range *){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_image_barrier_view(dead, (crgpu_image_view){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_pipeline_barrier(dead, (const crgpu_buffer_barrier *){0}, (uint32_t){0}, (const crgpu_image_barrier *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_push_compute_constants(dead, (crgpu_compute_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_push_graphics_constants(dead, (crgpu_graphics_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_reset_timestamps(dead, (crgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_resolve_timestamps(dead, (crgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_set_depth_bounds(dead, (float){0}, (float){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_set_index_buffer(dead, (crgpu_buffer_view){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_set_scissor_rects(dead, (uint32_t){0}, (uint32_t){0}, (const CRGPU_SCISSOR_RECT *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_set_vertex_buffers(dead, (uint32_t){0}, (uint32_t){0}, (crgpu_buffer_view *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			crgpu_command_list_set_viewports(dead, (uint32_t){0}, (uint32_t){0}, (const CRGPU_VIEWPORT *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_list dead = dead_crgpu_command_list();
		if (dead) {
			expect_refused("crgpu_command_list_write_acceleration_structure_compacted_sizes", crgpu_command_list_write_acceleration_structure_compacted_sizes(dead, (const crgpu_acceleration_structure *){0}, (uint32_t){0}, (crgpu_buffer){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_command_queue_get", crgpu_command_queue_get(dead, (CRGPU_COMMAND_LIST_TYPE){0}, (uint32_t){0}, (crgpu_command_queue *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_queue dead = dead_crgpu_command_queue();
		if (dead) {
			expect_refused("crgpu_command_queue_submit", crgpu_command_queue_submit(dead, (const crgpu_submit_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_queue dead = dead_crgpu_command_queue();
		if (dead) {
			expect_refused("crgpu_command_queue_submit_lists", crgpu_command_queue_submit_lists(dead, (crgpu_command_list *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_queue dead = dead_crgpu_command_queue();
		if (dead) {
			expect_refused("crgpu_command_queue_submit_lists_synced", crgpu_command_queue_submit_lists_synced(dead, (crgpu_command_list *){0}, (uint32_t){0}, (crgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_command_queue dead = dead_crgpu_command_queue();
		if (dead) {
			crgpu_command_queue_wait_idle(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_compute_pipeline_create", crgpu_compute_pipeline_create(dead, (const crgpu_compute_pipeline_desc *){0}, (crgpu_compute_pipeline *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_compute_pipeline dead = dead_crgpu_compute_pipeline();
		if (dead) {
			crgpu_compute_pipeline_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_compute_pipeline dead = dead_crgpu_compute_pipeline();
		if (dead) {
			expect_refused("crgpu_compute_pipeline_get_desc", crgpu_compute_pipeline_get_desc(dead, (crgpu_compute_pipeline_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_descriptor_pool_create", crgpu_descriptor_pool_create(dead, (const crgpu_descriptor_pool_desc *){0}, (crgpu_descriptor_pool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_descriptor_pool dead = dead_crgpu_descriptor_pool();
		if (dead) {
			crgpu_descriptor_pool_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_descriptor_pool dead = dead_crgpu_descriptor_pool();
		if (dead) {
			crgpu_descriptor_pool_reset(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_descriptor_set_create", crgpu_descriptor_set_create(dead, (const crgpu_descriptor_set_desc *){0}, (crgpu_descriptor_set *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_descriptor_set_layout_create", crgpu_descriptor_set_layout_create(dead, (const crgpu_descriptor_set_layout_desc *){0}, (crgpu_descriptor_set_layout *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_descriptor_set_layout dead = dead_crgpu_descriptor_set_layout();
		if (dead) {
			crgpu_descriptor_set_layout_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_adapter dead = dead_crgpu_adapter();
		if (dead) {
			expect_refused("crgpu_device_create", crgpu_device_create(dead, (const crgpu_device_desc *){0}, (crgpu_device *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			crgpu_device_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_device_get_acceleration_structure_build_sizes", crgpu_device_get_acceleration_structure_build_sizes(dead, (const crgpu_acceleration_structure_inputs *){0}, (crgpu_acceleration_structure_sizes *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_device_get_acceleration_structure_instance_layout", crgpu_device_get_acceleration_structure_instance_layout(dead, (crgpu_acceleration_structure_instance_layout *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_fallback("crgpu_device_get_adapter", crgpu_device_get_adapter(dead) == crgpu_device_get_adapter((crgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_fallback("crgpu_device_get_bindless_descriptor_set_layout", crgpu_device_get_bindless_descriptor_set_layout(dead) == crgpu_device_get_bindless_descriptor_set_layout((crgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_fallback("crgpu_device_get_features", crgpu_device_get_features(dead) == crgpu_device_get_features((crgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			crgpu_device_update_descriptor_sets(dead, (const crgpu_write_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_device_write_acceleration_structure_instances", crgpu_device_write_acceleration_structure_instances(dead, (const crgpu_acceleration_structure_instance *){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_fence_create", crgpu_fence_create(dead, (CRGPU_FENCE_FLAGS){0}, (uint64_t){0}, (crgpu_fence *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_fence dead = dead_crgpu_fence();
		if (dead) {
			crgpu_fence_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_fence dead = dead_crgpu_fence();
		if (dead) {
			expect_fallback("crgpu_fence_get_value", crgpu_fence_get_value(dead) == crgpu_fence_get_value((crgpu_fence){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_fence_signal", crgpu_fence_signal(dead, (crgpu_fence){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_fence_wait", crgpu_fence_wait(dead, (crgpu_fence *){0}, (const uint64_t *){0}, (uint32_t){0}, (bool){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_graphics_pipeline_create", crgpu_graphics_pipeline_create(dead, (const crgpu_graphics_pipeline_desc *){0}, (crgpu_graphics_pipeline *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_graphics_pipeline dead = dead_crgpu_graphics_pipeline();
		if (dead) {
			crgpu_graphics_pipeline_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_heap_create", crgpu_heap_create(dead, (const crgpu_heap_desc *){0}, (crgpu_heap *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			crgpu_heap_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_heap_get_buffer_placement_requirements", crgpu_heap_get_buffer_placement_requirements(dead, (const crgpu_buffer_desc *){0}, (crgpu_placement_requirements *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_heap_get_desc", crgpu_heap_get_desc(dead, (crgpu_heap_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_heap_get_image_placement_requirements", crgpu_heap_get_image_placement_requirements(dead, (const crgpu_image_desc *){0}, (crgpu_placement_requirements *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_image_create", crgpu_image_create(dead, (const crgpu_image_desc *){0}, (crgpu_image *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_heap dead = dead_crgpu_heap();
		if (dead) {
			expect_refused("crgpu_image_create_placed", crgpu_image_create_placed(dead, (uint64_t){0}, (const crgpu_image_desc *){0}, (crgpu_image *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image dead = dead_crgpu_image();
		if (dead) {
			crgpu_image_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image dead = dead_crgpu_image();
		if (dead) {
			expect_refused("crgpu_image_get_buffer_copy_offset_alignment", crgpu_image_get_buffer_copy_offset_alignment(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image dead = dead_crgpu_image();
		if (dead) {
			crgpu_image_get_buffer_copy_size(dead, (uint32_t){0}, (uint64_t *){0}, (uint64_t *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image dead = dead_crgpu_image();
		if (dead) {
			expect_refused("crgpu_image_get_desc", crgpu_image_get_desc(dead, (crgpu_image_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image dead = dead_crgpu_image();
		if (dead) {
			expect_refused("crgpu_image_view_create", crgpu_image_view_create(dead, (const crgpu_image_view_desc *){0}, (crgpu_image_view *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image_view dead = dead_crgpu_image_view();
		if (dead) {
			crgpu_image_view_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_image_view dead = dead_crgpu_image_view();
		if (dead) {
			expect_refused("crgpu_image_view_get_desc", crgpu_image_view_get_desc(dead, (crgpu_image_view_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_instance dead = dead_crgpu_instance();
		if (dead) {
			expect_fallback("crgpu_instance_destroy", crgpu_instance_destroy(dead) == crgpu_instance_destroy((crgpu_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_instance dead = dead_crgpu_instance();
		if (dead) {
			expect_fallback("crgpu_instance_get_native_gpu_api", crgpu_instance_get_native_gpu_api(dead) == crgpu_instance_get_native_gpu_api((crgpu_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_sampler_create", crgpu_sampler_create(dead, (const crgpu_sampler_desc *){0}, (crgpu_sampler *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_sampler dead = dead_crgpu_sampler();
		if (dead) {
			crgpu_sampler_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_semaphore_create", crgpu_semaphore_create(dead, (crgpu_semaphore *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_semaphore dead = dead_crgpu_semaphore();
		if (dead) {
			crgpu_semaphore_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_shader_module_create", crgpu_shader_module_create(dead, (const crgpu_shader_module_desc *){0}, (crgpu_shader_module *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_shader_module dead = dead_crgpu_shader_module();
		if (dead) {
			crgpu_shader_module_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_instance dead = dead_crgpu_instance();
		if (dead) {
			expect_refused("crgpu_surface_create", crgpu_surface_create(dead, (void *){0}, (void *){0}, (crgpu_surface *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_instance dead = dead_crgpu_instance();
		if (dead) {
			expect_fallback("crgpu_surface_create_from_callback", crgpu_surface_create_from_callback(dead, (void *){0}, (crgpu_surface_create_fn){0}) == crgpu_surface_create_from_callback((crgpu_instance){0}, (void *){0}, (crgpu_surface_create_fn){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_surface dead = dead_crgpu_surface();
		if (dead) {
			crgpu_surface_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_surface_get_present_scalings", crgpu_surface_get_present_scalings(dead, (crgpu_surface){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			expect_fallback("crgpu_swap_chain_acquire_next_image", crgpu_swap_chain_acquire_next_image(dead) == crgpu_swap_chain_acquire_next_image((crgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_swap_chain_create", crgpu_swap_chain_create(dead, (const crgpu_swap_chain_desc *){0}, (crgpu_swap_chain *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			crgpu_swap_chain_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			expect_refused("crgpu_swap_chain_get_desc", crgpu_swap_chain_get_desc(dead, (crgpu_swap_chain_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			expect_fallback("crgpu_swap_chain_get_image_available_semaphore", crgpu_swap_chain_get_image_available_semaphore(dead) == crgpu_swap_chain_get_image_available_semaphore((crgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			expect_fallback("crgpu_swap_chain_get_image_view", crgpu_swap_chain_get_image_view(dead, (uint32_t){0}) == crgpu_swap_chain_get_image_view((crgpu_swap_chain){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			expect_fallback("crgpu_swap_chain_get_render_finished_semaphore", crgpu_swap_chain_get_render_finished_semaphore(dead) == crgpu_swap_chain_get_render_finished_semaphore((crgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_swap_chain dead = dead_crgpu_swap_chain();
		if (dead) {
			crgpu_swap_chain_present(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_device dead = dead_crgpu_device();
		if (dead) {
			expect_refused("crgpu_timestamp_set_create", crgpu_timestamp_set_create(dead, (crgpu_command_queue){0}, (uint32_t){0}, (crgpu_timestamp_set *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_timestamp_set dead = dead_crgpu_timestamp_set();
		if (dead) {
			crgpu_timestamp_set_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crgpu_timestamp_set dead = dead_crgpu_timestamp_set();
		if (dead) {
			expect_refused("crgpu_timestamp_set_get", crgpu_timestamp_set_get(dead, (uint32_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_axis_label", crinput_gamepad_get_axis_label(dead, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_AXIS){0}, (char *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_battery", crinput_gamepad_get_battery(dead, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_BATTERY *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_button_label", crinput_gamepad_get_button_label(dead, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_BUTTONS){0}, (char *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_motion", crinput_gamepad_get_motion(dead, (crinput_gamepad_id){0}, (const CRINPUT_GAMEPAD_MOTION_SAMPLE **){0}, (uint32_t *){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_properties", crinput_gamepad_get_properties(dead, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_state", crinput_gamepad_get_state(dead, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_STATE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_get_touch", crinput_gamepad_get_touch(dead, (crinput_gamepad_id){0}, (uint32_t){0}, (CRINPUT_GAMEPAD_TOUCH_STATE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_set_light_color", crinput_gamepad_set_light_color(dead, (crinput_gamepad_id){0}, (float){0}, (float){0}, (float){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_set_motion_enabled", crinput_gamepad_set_motion_enabled(dead, (crinput_gamepad_id){0}, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_set_player_indicator", crinput_gamepad_set_player_indicator(dead, (crinput_gamepad_id){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_gamepad_set_rumble", crinput_gamepad_set_rumble(dead, (crinput_gamepad_id){0}, (const CRINPUT_GAMEPAD_RUMBLE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			crinput_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_instance_get_gamepads", crinput_instance_get_gamepads(dead, (const crinput_gamepad_id **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_fallback("crinput_instance_get_native_api", crinput_instance_get_native_api(dead) == crinput_instance_get_native_api((crinput_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crinput_instance dead = dead_crinput_instance();
		if (dead) {
			expect_refused("crinput_instance_poll", crinput_instance_poll(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_http_client_create", crnet_http_client_create(dead, (const crnet_http_client_desc *){0}, (crnet_http_client *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_client dead = dead_crnet_http_client();
		if (dead) {
			crnet_http_client_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			crnet_http_transfer_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_client dead = dead_crnet_http_client();
		if (dead) {
			expect_refused("crnet_http_transfer_create", crnet_http_transfer_create(dead, (crnet_http_transfer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			crnet_http_transfer_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			crnet_http_transfer_get_error(dead, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_refused("crnet_http_transfer_get_header_at", crnet_http_transfer_get_header_at(dead, (uint32_t){0}, (const char **){0}, (const char **){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_fallback("crnet_http_transfer_get_header_count", crnet_http_transfer_get_header_count(dead) == crnet_http_transfer_get_header_count((crnet_http_transfer){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_fallback("crnet_http_transfer_get_status", crnet_http_transfer_get_status(dead) == crnet_http_transfer_get_status((crnet_http_transfer){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_refused("crnet_http_transfer_receive_response", crnet_http_transfer_receive_response(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_refused("crnet_http_transfer_recv", crnet_http_transfer_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_refused("crnet_http_transfer_send", crnet_http_transfer_send(dead, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_http_transfer dead = dead_crnet_http_transfer();
		if (dead) {
			expect_refused("crnet_http_transfer_start", crnet_http_transfer_start(dead, (const crnet_http_transfer_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			crnet_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_instance_get_local_ipv4_addresses", crnet_instance_get_local_ipv4_addresses(dead, (char (*)[16]){0}, (uint32_t){0}, (uint32_t *){0}, (crnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_resolver dead = dead_crnet_resolver();
		if (dead) {
			crnet_resolver_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_resolver_create", crnet_resolver_create(dead, (const crnet_resolver_desc *){0}, (crnet_resolver *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_resolver dead = dead_crnet_resolver();
		if (dead) {
			crnet_resolver_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_resolver dead = dead_crnet_resolver();
		if (dead) {
			crnet_resolver_get_error(dead, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_resolver dead = dead_crnet_resolver();
		if (dead) {
			expect_refused("crnet_resolver_wait", crnet_resolver_wait(dead, (uint32_t){0}, (char (*)[46]){0}, (uint32_t){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_listener dead = dead_crnet_tcp_listener();
		if (dead) {
			crnet_tcp_listener_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_listener dead = dead_crnet_tcp_listener();
		if (dead) {
			expect_refused("crnet_tcp_listener_accept", crnet_tcp_listener_accept(dead, (uint32_t){0}, (crnet_tcp_socket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_tcp_listener_create", crnet_tcp_listener_create(dead, (const crnet_tcp_listener_desc *){0}, (crnet_tcp_listener *){0}, (crnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_listener dead = dead_crnet_tcp_listener();
		if (dead) {
			crnet_tcp_listener_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_listener dead = dead_crnet_tcp_listener();
		if (dead) {
			crnet_tcp_listener_get_error(dead, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_listener dead = dead_crnet_tcp_listener();
		if (dead) {
			expect_refused("crnet_tcp_listener_get_port", crnet_tcp_listener_get_port(dead, (uint16_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			crnet_tcp_socket_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_connect", crnet_tcp_socket_connect(dead, (const crnet_tcp_socket_connect_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_connect_continue", crnet_tcp_socket_connect_continue(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_tcp_socket_create", crnet_tcp_socket_create(dead, (crnet_tcp_socket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			crnet_tcp_socket_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_finish_sending", crnet_tcp_socket_finish_sending(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			crnet_tcp_socket_get_error(dead, (CRNET_ERROR_SIDE){0}, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_get_peer_address", crnet_tcp_socket_get_peer_address(dead, (char *){0}, (uint16_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_recv", crnet_tcp_socket_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_send", crnet_tcp_socket_send(dead, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_tcp_socket dead = dead_crnet_tcp_socket();
		if (dead) {
			expect_refused("crnet_tcp_socket_set_no_delay", crnet_tcp_socket_set_no_delay(dead, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			expect_refused("crnet_wait_set_add_http_transfer", crnet_wait_set_add_http_transfer(dead, (crnet_http_transfer){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			expect_refused("crnet_wait_set_add_tcp_listener", crnet_wait_set_add_tcp_listener(dead, (crnet_tcp_listener){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			expect_refused("crnet_wait_set_add_tcp_socket", crnet_wait_set_add_tcp_socket(dead, (crnet_tcp_socket){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			expect_refused("crnet_wait_set_add_websocket", crnet_wait_set_add_websocket(dead, (crnet_websocket){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_wait_set_create", crnet_wait_set_create(dead, (crnet_wait_set *){0}, (crnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_get_error(dead, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_remove_http_transfer(dead, (crnet_http_transfer){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_remove_tcp_listener(dead, (crnet_tcp_listener){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_remove_tcp_socket(dead, (crnet_tcp_socket){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_remove_websocket(dead, (crnet_websocket){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			expect_refused("crnet_wait_set_wait", crnet_wait_set_wait(dead, (uint32_t){0}, (crnet_wait_event *){0}, (uint32_t){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_wait_set dead = dead_crnet_wait_set();
		if (dead) {
			crnet_wait_set_wake(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			crnet_websocket_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_close", crnet_websocket_close(dead, (uint16_t){0}, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_connect", crnet_websocket_connect(dead, (const crnet_websocket_connect_desc *){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_connect_continue", crnet_websocket_connect_continue(dead, (uint32_t){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_instance dead = dead_crnet_instance();
		if (dead) {
			expect_refused("crnet_websocket_create", crnet_websocket_create(dead, (crnet_websocket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			crnet_websocket_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_flush", crnet_websocket_flush(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_fallback("crnet_websocket_get_close_code", crnet_websocket_get_close_code(dead) == crnet_websocket_get_close_code((crnet_websocket){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			crnet_websocket_get_error(dead, (CRNET_ERROR_SIDE){0}, (crnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_recv", crnet_websocket_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}, (CRNET_WEBSOCKET_MESSAGE *){0}, (bool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		crnet_websocket dead = dead_crnet_websocket();
		if (dead) {
			expect_refused("crnet_websocket_send", crnet_websocket_send(dead, (CRNET_WEBSOCKET_MESSAGE){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	freed_teardown();
	*out_calls     = calls;
	*out_unchecked = unchecked;
	*out_skipped   = skipped;
	return freed_failures;
}
#endif // MISUSE_FREED

int main(void)
{
	int calls = 0;
	int unchecked = 0;
	(void)cr_allocator_system();
	calls++;
	expect_error("cr_debug_name_set", cr_debug_name_set((void *){0}, (const char *){0}));
	calls++;
	(void)cr_get_version();
	calls++;
	expect_error("craudio_device_enumerate", craudio_device_enumerate((craudio_instance){0}, (CRAUDIO_DIRECTION){0}, (craudio_device **){0}, (uint32_t *){0}));
	calls++;
	expect_error("craudio_device_get_properties", craudio_device_get_properties((craudio_device){0}, (CRAUDIO_DEVICE_PROPERTIES *){0}));
	calls++;
	expect_error("craudio_device_get_timing_caps", craudio_device_get_timing_caps((craudio_device){0}, (const CRAUDIO_FORMAT *){0}, (CRAUDIO_STREAM_FLAGS){0}, (CRAUDIO_TIMING_CAPS *){0}));
	calls++;
	expect_error("craudio_device_set_sample_rate", craudio_device_set_sample_rate((craudio_device){0}, (uint32_t){0}));
	calls++;
	expect_error("craudio_instance_create", craudio_instance_create((const CRAUDIO_INSTANCE_DESC *){0}, (craudio_instance *){0}));
	calls++;
	expect_error("craudio_instance_destroy", craudio_instance_destroy((craudio_instance){0}));
	calls++;
	(void)craudio_instance_get_native_api((craudio_instance){0});
	calls++;
	expect_error("craudio_instance_set_device_event_callback", craudio_instance_set_device_event_callback((craudio_instance){0}, (CRAUDIO_DEVICE_EVENT_CALLBACK){0}, (void *){0}));
	calls++;
	(void)craudio_sample_format_get_byte_size((CRAUDIO_SAMPLE_FORMAT){0});
	calls++;
	expect_error("craudio_stream_create", craudio_stream_create((const CRAUDIO_STREAM_DESC *){0}, (craudio_stream *){0}));
	calls++;
	craudio_stream_destroy((craudio_stream){0});
	unchecked++;
	calls++;
	expect_error("craudio_stream_get_config", craudio_stream_get_config((craudio_stream){0}, (CRAUDIO_STREAM_CONFIG *){0}));
	calls++;
	expect_error("craudio_stream_get_latency_frames", craudio_stream_get_latency_frames((craudio_stream){0}, (uint32_t *){0}));
	calls++;
	expect_error("craudio_stream_get_status", craudio_stream_get_status((craudio_stream){0}, (CRAUDIO_STREAM_STATUS *){0}));
	calls++;
	expect_error("craudio_stream_probe", craudio_stream_probe((const CRAUDIO_STREAM_DESC *){0}, (CRAUDIO_STREAM_CONFIG *){0}));
	calls++;
	expect_error("craudio_stream_start", craudio_stream_start((craudio_stream){0}));
	calls++;
	expect_error("craudio_stream_stop", craudio_stream_stop((craudio_stream){0}));
	calls++;
	expect_error("crfiles_directory_create", crfiles_directory_create((crfiles_instance){0}, (const char *){0}));
	calls++;
	expect_error("crfiles_directory_remove", crfiles_directory_remove((crfiles_instance){0}, (const char *){0}));
	calls++;
	expect_error("crfiles_directory_rename", crfiles_directory_rename((crfiles_instance){0}, (const char *){0}, (const char *){0}, (CRFILES_FLUSH_LEVEL){0}));
	calls++;
	crfiles_file_close((crfiles_file){0});
	unchecked++;
	calls++;
	expect_error("crfiles_file_flush", crfiles_file_flush((crfiles_file){0}, (CRFILES_FLUSH_LEVEL){0}));
	calls++;
	expect_error("crfiles_file_get_size", crfiles_file_get_size((crfiles_file){0}, (uint64_t *){0}));
	calls++;
	expect_error("crfiles_file_lock", crfiles_file_lock((crfiles_file){0}, (CRFILES_LOCK){0}, (bool){0}));
	calls++;
	expect_error("crfiles_file_map", crfiles_file_map((crfiles_file){0}, (uint64_t){0}, (uint64_t){0}, (crfiles_mapping *){0}));
	calls++;
	expect_error("crfiles_file_open", crfiles_file_open((crfiles_instance){0}, (const crfiles_open_desc *){0}, (crfiles_file *){0}));
	calls++;
	expect_error("crfiles_file_read_at", crfiles_file_read_at((crfiles_file){0}, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crfiles_file_set_size", crfiles_file_set_size((crfiles_file){0}, (uint64_t){0}));
	calls++;
	expect_error("crfiles_file_unlock", crfiles_file_unlock((crfiles_file){0}));
	calls++;
	expect_error("crfiles_file_write_at", crfiles_file_write_at((crfiles_file){0}, (uint64_t){0}, (const void *){0}, (uint64_t){0}));
	calls++;
	expect_error("crfiles_instance_create", crfiles_instance_create((const crfiles_instance_desc *){0}, (crfiles_instance *){0}));
	calls++;
	crfiles_instance_destroy((crfiles_instance){0});
	unchecked++;
	calls++;
	crfiles_library_close((crfiles_library){0});
	unchecked++;
	calls++;
	expect_error("crfiles_library_find", crfiles_library_find((crfiles_library){0}, (const char *){0}, (crfiles_library_function *){0}));
	calls++;
	expect_error("crfiles_library_open", crfiles_library_open((crfiles_instance){0}, (const crfiles_library_desc *){0}, (crfiles_library *){0}));
	calls++;
	crfiles_listing_close((crfiles_listing){0});
	unchecked++;
	calls++;
	expect_error("crfiles_listing_next", crfiles_listing_next((crfiles_listing){0}, (CRFILES_LISTING_ENTRY *){0}));
	calls++;
	expect_error("crfiles_listing_open", crfiles_listing_open((crfiles_instance){0}, (const crfiles_listing_desc *){0}, (crfiles_listing *){0}));
	calls++;
	(void)crfiles_mapping_get_size((crfiles_mapping){0});
	calls++;
	crfiles_mapping_release((crfiles_mapping){0});
	unchecked++;
	calls++;
	expect_error("crfiles_path_copy_mode", crfiles_path_copy_mode((crfiles_instance){0}, (const char *){0}, (const char *){0}));
	calls++;
	expect_error("crfiles_path_get_info", crfiles_path_get_info((crfiles_instance){0}, (const char *){0}, (CRFILES_PATH_INFO *){0}));
	calls++;
	expect_error("crfiles_path_remove", crfiles_path_remove((crfiles_instance){0}, (const char *){0}));
	calls++;
	expect_error("crfiles_path_replace", crfiles_path_replace((crfiles_instance){0}, (const char *){0}, (const char *){0}, (CRFILES_FLUSH_LEVEL){0}));
	calls++;
	expect_error("crgpu_acceleration_structure_create", crgpu_acceleration_structure_create((crgpu_device){0}, (const crgpu_acceleration_structure_desc *){0}, (crgpu_acceleration_structure *){0}));
	calls++;
	expect_error("crgpu_acceleration_structure_create_placed", crgpu_acceleration_structure_create_placed((crgpu_heap){0}, (uint64_t){0}, (const crgpu_acceleration_structure_desc *){0}, (crgpu_acceleration_structure *){0}));
	calls++;
	crgpu_acceleration_structure_destroy((crgpu_acceleration_structure){0});
	unchecked++;
	calls++;
	expect_error("crgpu_acceleration_structure_get_reference", crgpu_acceleration_structure_get_reference((crgpu_acceleration_structure){0}, (uint64_t *){0}));
	calls++;
	expect_error("crgpu_adapter_enumerate", crgpu_adapter_enumerate((crgpu_instance){0}, (const crgpu_adapter **){0}, (uint32_t *){0}));
	calls++;
	expect_error("crgpu_adapter_get_properties", crgpu_adapter_get_properties((crgpu_adapter){0}, (CRGPU_ADAPTER_PROPERTIES *){0}));
	calls++;
	expect_error("crgpu_adapter_supports_surface", crgpu_adapter_supports_surface((crgpu_adapter){0}, (crgpu_surface){0}, (bool *){0}));
	calls++;
#if defined(__linux__) || defined(_WIN32)
	expect_error("crgpu_bindless_register_sampled_image", crgpu_bindless_register_sampled_image((crgpu_device){0}, (crgpu_image_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	expect_error("crgpu_bindless_register_storage_buffer", crgpu_bindless_register_storage_buffer((crgpu_device){0}, (crgpu_buffer_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	expect_error("crgpu_bindless_register_storage_image", crgpu_bindless_register_storage_image((crgpu_device){0}, (crgpu_image_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	crgpu_bindless_unregister_sampled_image((crgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	crgpu_bindless_unregister_storage_buffer((crgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	crgpu_bindless_unregister_storage_image((crgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
	expect_error("crgpu_buffer_create", crgpu_buffer_create((crgpu_device){0}, (const crgpu_buffer_desc *){0}, (crgpu_buffer *){0}));
	calls++;
	expect_error("crgpu_buffer_create_placed", crgpu_buffer_create_placed((crgpu_heap){0}, (uint64_t){0}, (const crgpu_buffer_desc *){0}, (crgpu_buffer *){0}));
	calls++;
	crgpu_buffer_destroy((crgpu_buffer){0});
	unchecked++;
	calls++;
	expect_error("crgpu_buffer_flush", crgpu_buffer_flush((crgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}));
	calls++;
	expect_error("crgpu_buffer_get_desc", crgpu_buffer_get_desc((crgpu_buffer){0}, (crgpu_buffer_desc *){0}));
	calls++;
	expect_error("crgpu_buffer_get_gpu_address", crgpu_buffer_get_gpu_address((crgpu_buffer){0}, (uint64_t *){0}));
	calls++;
	expect_error("crgpu_buffer_invalidate", crgpu_buffer_invalidate((crgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}));
	calls++;
	expect_error("crgpu_buffer_map", crgpu_buffer_map((crgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (void **){0}));
	calls++;
	crgpu_buffer_unmap((crgpu_buffer){0});
	unchecked++;
	calls++;
	expect_error("crgpu_buffer_view_create", crgpu_buffer_view_create((crgpu_buffer){0}, (const crgpu_buffer_view_desc *){0}, (crgpu_buffer_view *){0}));
	calls++;
	crgpu_buffer_view_destroy((crgpu_buffer_view){0});
	unchecked++;
	calls++;
	expect_error("crgpu_buffer_view_get_desc", crgpu_buffer_view_get_desc((crgpu_buffer_view){0}, (crgpu_buffer_view_desc *){0}));
	calls++;
	expect_error("crgpu_command_allocator_create", crgpu_command_allocator_create((crgpu_device){0}, (const crgpu_command_allocator_desc *){0}, (crgpu_command_allocator *){0}));
	calls++;
	crgpu_command_allocator_destroy((crgpu_command_allocator){0});
	unchecked++;
	calls++;
	expect_error("crgpu_command_allocator_reset", crgpu_command_allocator_reset((crgpu_command_allocator){0}));
	calls++;
	crgpu_command_list_acceleration_structure_barrier((crgpu_command_list){0}, (const crgpu_acceleration_structure_barrier *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_begin((crgpu_command_list){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	crgpu_command_list_begin_bundle((crgpu_command_list){0}, (const crgpu_bundle_inheritance_desc *){0});
	unchecked++;
	calls++;
#endif
	crgpu_command_list_begin_debug_label((crgpu_command_list){0}, (const char *){0});
	unchecked++;
	calls++;
	crgpu_command_list_begin_rendering((crgpu_command_list){0}, (const crgpu_rendering_begin_desc *){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	crgpu_command_list_bind_compute_bindless_resources((crgpu_command_list){0}, (crgpu_compute_pipeline){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
	crgpu_command_list_bind_compute_descriptor_sets((crgpu_command_list){0}, (crgpu_compute_pipeline){0}, (uint32_t){0}, (const crgpu_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_bind_compute_pipeline((crgpu_command_list){0}, (crgpu_compute_pipeline){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	crgpu_command_list_bind_graphics_bindless_resources((crgpu_command_list){0}, (crgpu_graphics_pipeline){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
	crgpu_command_list_bind_graphics_descriptor_sets((crgpu_command_list){0}, (crgpu_graphics_pipeline){0}, (uint32_t){0}, (const crgpu_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_bind_graphics_pipeline((crgpu_command_list){0}, (crgpu_graphics_pipeline){0});
	unchecked++;
	calls++;
	crgpu_command_list_blit_image((crgpu_command_list){0}, (crgpu_image){0}, (crgpu_image){0}, (const crgpu_image_blit_desc *){0});
	unchecked++;
	calls++;
	expect_error("crgpu_command_list_build_acceleration_structures", crgpu_command_list_build_acceleration_structures((crgpu_command_list){0}, (const crgpu_acceleration_structure_build *){0}, (uint32_t){0}));
	calls++;
	crgpu_command_list_clear_color_attachment((crgpu_command_list){0}, (crgpu_image_view){0}, (float){0}, (float){0}, (float){0}, (float){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_clear_depth_attachment((crgpu_command_list){0}, (crgpu_image_view){0}, (bool){0}, (bool){0}, (float){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	expect_error("crgpu_command_list_copy_acceleration_structure", crgpu_command_list_copy_acceleration_structure((crgpu_command_list){0}, (crgpu_acceleration_structure){0}, (crgpu_acceleration_structure){0}, (CRGPU_ACCELERATION_STRUCTURE_COPY_MODE){0}));
	calls++;
	crgpu_command_list_copy_buffer((crgpu_command_list){0}, (crgpu_buffer){0}, (crgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (uint64_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_copy_buffer_to_image((crgpu_command_list){0}, (crgpu_buffer){0}, (crgpu_image){0}, (const crgpu_image_buffer_copy_desc *){0});
	unchecked++;
	calls++;
	crgpu_command_list_copy_image_to_buffer((crgpu_command_list){0}, (crgpu_image){0}, (crgpu_buffer){0}, (const crgpu_image_buffer_copy_desc *){0});
	unchecked++;
	calls++;
	expect_error("crgpu_command_list_create", crgpu_command_list_create((crgpu_command_allocator){0}, (crgpu_command_list *){0}));
	calls++;
	crgpu_command_list_destroy((crgpu_command_list){0});
	unchecked++;
	calls++;
	crgpu_command_list_dispatch((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_dispatch_mesh((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw_indexed((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw_indexed_indirect((crgpu_command_list){0}, (crgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw_indexed_instanced((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw_indirect((crgpu_command_list){0}, (crgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_draw_instanced((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_end((crgpu_command_list){0});
	unchecked++;
	calls++;
	crgpu_command_list_end_debug_label((crgpu_command_list){0});
	unchecked++;
	calls++;
	crgpu_command_list_end_rendering((crgpu_command_list){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	crgpu_command_list_execute_bundle((crgpu_command_list){0}, (crgpu_command_list){0});
	unchecked++;
	calls++;
#endif
	crgpu_command_list_image_barrier((crgpu_command_list){0}, (crgpu_image){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	crgpu_command_list_image_barrier_subresource((crgpu_command_list){0}, (crgpu_image){0}, (const crgpu_image_view_desc_subresource_range *){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	crgpu_command_list_image_barrier_view((crgpu_command_list){0}, (crgpu_image_view){0}, (CRGPU_IMAGE_LAYOUT){0}, (CRGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	crgpu_command_list_pipeline_barrier((crgpu_command_list){0}, (const crgpu_buffer_barrier *){0}, (uint32_t){0}, (const crgpu_image_barrier *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_push_compute_constants((crgpu_command_list){0}, (crgpu_compute_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
	unchecked++;
	calls++;
	crgpu_command_list_push_graphics_constants((crgpu_command_list){0}, (crgpu_graphics_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
	unchecked++;
	calls++;
	crgpu_command_list_reset_timestamps((crgpu_command_list){0}, (crgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_resolve_timestamps((crgpu_command_list){0}, (crgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	crgpu_command_list_set_depth_bounds((crgpu_command_list){0}, (float){0}, (float){0});
	unchecked++;
	calls++;
	crgpu_command_list_set_index_buffer((crgpu_command_list){0}, (crgpu_buffer_view){0});
	unchecked++;
	calls++;
	crgpu_command_list_set_scissor_rects((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (const CRGPU_SCISSOR_RECT *){0});
	unchecked++;
	calls++;
	crgpu_command_list_set_vertex_buffers((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (crgpu_buffer_view *){0});
	unchecked++;
	calls++;
	crgpu_command_list_set_viewports((crgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (const CRGPU_VIEWPORT *){0});
	unchecked++;
	calls++;
	expect_error("crgpu_command_list_write_acceleration_structure_compacted_sizes", crgpu_command_list_write_acceleration_structure_compacted_sizes((crgpu_command_list){0}, (const crgpu_acceleration_structure *){0}, (uint32_t){0}, (crgpu_buffer){0}, (uint64_t){0}));
	calls++;
	expect_error("crgpu_command_queue_get", crgpu_command_queue_get((crgpu_device){0}, (CRGPU_COMMAND_LIST_TYPE){0}, (uint32_t){0}, (crgpu_command_queue *){0}));
	calls++;
	expect_error("crgpu_command_queue_submit", crgpu_command_queue_submit((crgpu_command_queue){0}, (const crgpu_submit_desc *){0}));
	calls++;
	expect_error("crgpu_command_queue_submit_lists", crgpu_command_queue_submit_lists((crgpu_command_queue){0}, (crgpu_command_list *){0}, (uint32_t){0}));
	calls++;
	expect_error("crgpu_command_queue_submit_lists_synced", crgpu_command_queue_submit_lists_synced((crgpu_command_queue){0}, (crgpu_command_list *){0}, (uint32_t){0}, (crgpu_swap_chain){0}));
	calls++;
	crgpu_command_queue_wait_idle((crgpu_command_queue){0});
	unchecked++;
	calls++;
	expect_error("crgpu_compute_pipeline_create", crgpu_compute_pipeline_create((crgpu_device){0}, (const crgpu_compute_pipeline_desc *){0}, (crgpu_compute_pipeline *){0}));
	calls++;
	crgpu_compute_pipeline_destroy((crgpu_compute_pipeline){0});
	unchecked++;
	calls++;
	expect_error("crgpu_compute_pipeline_get_desc", crgpu_compute_pipeline_get_desc((crgpu_compute_pipeline){0}, (crgpu_compute_pipeline_desc *){0}));
	calls++;
	expect_error("crgpu_descriptor_pool_create", crgpu_descriptor_pool_create((crgpu_device){0}, (const crgpu_descriptor_pool_desc *){0}, (crgpu_descriptor_pool *){0}));
	calls++;
	crgpu_descriptor_pool_destroy((crgpu_descriptor_pool){0});
	unchecked++;
	calls++;
	crgpu_descriptor_pool_reset((crgpu_descriptor_pool){0});
	unchecked++;
	calls++;
	expect_error("crgpu_descriptor_set_create", crgpu_descriptor_set_create((crgpu_device){0}, (const crgpu_descriptor_set_desc *){0}, (crgpu_descriptor_set *){0}));
	calls++;
	expect_error("crgpu_descriptor_set_layout_create", crgpu_descriptor_set_layout_create((crgpu_device){0}, (const crgpu_descriptor_set_layout_desc *){0}, (crgpu_descriptor_set_layout *){0}));
	calls++;
	crgpu_descriptor_set_layout_destroy((crgpu_descriptor_set_layout){0});
	unchecked++;
	calls++;
	expect_error("crgpu_device_create", crgpu_device_create((crgpu_adapter){0}, (const crgpu_device_desc *){0}, (crgpu_device *){0}));
	calls++;
	crgpu_device_destroy((crgpu_device){0});
	unchecked++;
	calls++;
	expect_error("crgpu_device_get_acceleration_structure_build_sizes", crgpu_device_get_acceleration_structure_build_sizes((crgpu_device){0}, (const crgpu_acceleration_structure_inputs *){0}, (crgpu_acceleration_structure_sizes *){0}));
	calls++;
	expect_error("crgpu_device_get_acceleration_structure_instance_layout", crgpu_device_get_acceleration_structure_instance_layout((crgpu_device){0}, (crgpu_acceleration_structure_instance_layout *){0}));
	calls++;
	(void)crgpu_device_get_adapter((crgpu_device){0});
	calls++;
#if defined(__linux__) || defined(_WIN32)
	(void)crgpu_device_get_bindless_descriptor_set_layout((crgpu_device){0});
	calls++;
#endif
	(void)crgpu_device_get_features((crgpu_device){0});
	calls++;
	crgpu_device_update_descriptor_sets((crgpu_device){0}, (const crgpu_write_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	expect_error("crgpu_device_write_acceleration_structure_instances", crgpu_device_write_acceleration_structure_instances((crgpu_device){0}, (const crgpu_acceleration_structure_instance *){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("crgpu_fence_create", crgpu_fence_create((crgpu_device){0}, (CRGPU_FENCE_FLAGS){0}, (uint64_t){0}, (crgpu_fence *){0}));
	calls++;
	crgpu_fence_destroy((crgpu_fence){0});
	unchecked++;
	calls++;
	(void)crgpu_fence_get_value((crgpu_fence){0});
	calls++;
	expect_error("crgpu_fence_signal", crgpu_fence_signal((crgpu_device){0}, (crgpu_fence){0}, (uint64_t){0}));
	calls++;
	expect_error("crgpu_fence_wait", crgpu_fence_wait((crgpu_device){0}, (crgpu_fence *){0}, (const uint64_t *){0}, (uint32_t){0}, (bool){0}, (uint64_t){0}));
	calls++;
	(void)crgpu_format_from_string((const char *){0});
	calls++;
	(void)crgpu_format_get_bit_count((CRGPU_FORMAT){0});
	calls++;
	expect_error("crgpu_graphics_pipeline_create", crgpu_graphics_pipeline_create((crgpu_device){0}, (const crgpu_graphics_pipeline_desc *){0}, (crgpu_graphics_pipeline *){0}));
	calls++;
	crgpu_graphics_pipeline_destroy((crgpu_graphics_pipeline){0});
	unchecked++;
	calls++;
	expect_error("crgpu_heap_create", crgpu_heap_create((crgpu_device){0}, (const crgpu_heap_desc *){0}, (crgpu_heap *){0}));
	calls++;
	crgpu_heap_destroy((crgpu_heap){0});
	unchecked++;
	calls++;
	expect_error("crgpu_heap_get_buffer_placement_requirements", crgpu_heap_get_buffer_placement_requirements((crgpu_heap){0}, (const crgpu_buffer_desc *){0}, (crgpu_placement_requirements *){0}));
	calls++;
	expect_error("crgpu_heap_get_desc", crgpu_heap_get_desc((crgpu_heap){0}, (crgpu_heap_desc *){0}));
	calls++;
	expect_error("crgpu_heap_get_image_placement_requirements", crgpu_heap_get_image_placement_requirements((crgpu_heap){0}, (const crgpu_image_desc *){0}, (crgpu_placement_requirements *){0}));
	calls++;
	expect_error("crgpu_image_create", crgpu_image_create((crgpu_device){0}, (const crgpu_image_desc *){0}, (crgpu_image *){0}));
	calls++;
	expect_error("crgpu_image_create_placed", crgpu_image_create_placed((crgpu_heap){0}, (uint64_t){0}, (const crgpu_image_desc *){0}, (crgpu_image *){0}));
	calls++;
	crgpu_image_destroy((crgpu_image){0});
	unchecked++;
	calls++;
	expect_error("crgpu_image_get_buffer_copy_offset_alignment", crgpu_image_get_buffer_copy_offset_alignment((crgpu_image){0}, (uint64_t *){0}));
	calls++;
	crgpu_image_get_buffer_copy_size((crgpu_image){0}, (uint32_t){0}, (uint64_t *){0}, (uint64_t *){0});
	unchecked++;
	calls++;
	expect_error("crgpu_image_get_desc", crgpu_image_get_desc((crgpu_image){0}, (crgpu_image_desc *){0}));
	calls++;
	expect_error("crgpu_image_view_create", crgpu_image_view_create((crgpu_image){0}, (const crgpu_image_view_desc *){0}, (crgpu_image_view *){0}));
	calls++;
	crgpu_image_view_destroy((crgpu_image_view){0});
	unchecked++;
	calls++;
	expect_error("crgpu_image_view_get_desc", crgpu_image_view_get_desc((crgpu_image_view){0}, (crgpu_image_view_desc *){0}));
	calls++;
	expect_error("crgpu_instance_create", crgpu_instance_create((const crgpu_instance_desc *){0}, (crgpu_instance *){0}));
	calls++;
	(void)crgpu_instance_destroy((crgpu_instance){0}); // zeroes are valid: destroying NULL succeeds, as crgpu.h says
	calls++;
	(void)crgpu_instance_get_native_gpu_api((crgpu_instance){0});
	calls++;
	expect_error("crgpu_sampler_create", crgpu_sampler_create((crgpu_device){0}, (const crgpu_sampler_desc *){0}, (crgpu_sampler *){0}));
	calls++;
	crgpu_sampler_destroy((crgpu_sampler){0});
	unchecked++;
	calls++;
	expect_error("crgpu_semaphore_create", crgpu_semaphore_create((crgpu_device){0}, (crgpu_semaphore *){0}));
	calls++;
	crgpu_semaphore_destroy((crgpu_semaphore){0});
	unchecked++;
	calls++;
	expect_error("crgpu_shader_module_create", crgpu_shader_module_create((crgpu_device){0}, (const crgpu_shader_module_desc *){0}, (crgpu_shader_module *){0}));
	calls++;
	crgpu_shader_module_destroy((crgpu_shader_module){0});
	unchecked++;
	calls++;
	expect_error("crgpu_surface_create", crgpu_surface_create((crgpu_instance){0}, (void *){0}, (void *){0}, (crgpu_surface *){0}));
	calls++;
	(void)crgpu_surface_create_from_callback((crgpu_instance){0}, (void *){0}, (crgpu_surface_create_fn){0});
	calls++;
	crgpu_surface_destroy((crgpu_surface){0});
	unchecked++;
	calls++;
	expect_error("crgpu_surface_get_present_scalings", crgpu_surface_get_present_scalings((crgpu_device){0}, (crgpu_surface){0}, (uint32_t *){0}));
	calls++;
	(void)crgpu_swap_chain_acquire_next_image((crgpu_swap_chain){0});
	calls++;
	expect_error("crgpu_swap_chain_create", crgpu_swap_chain_create((crgpu_device){0}, (const crgpu_swap_chain_desc *){0}, (crgpu_swap_chain *){0}));
	calls++;
	crgpu_swap_chain_destroy((crgpu_swap_chain){0});
	unchecked++;
	calls++;
	expect_error("crgpu_swap_chain_get_desc", crgpu_swap_chain_get_desc((crgpu_swap_chain){0}, (crgpu_swap_chain_desc *){0}));
	calls++;
	(void)crgpu_swap_chain_get_image_available_semaphore((crgpu_swap_chain){0});
	calls++;
	(void)crgpu_swap_chain_get_image_view((crgpu_swap_chain){0}, (uint32_t){0});
	calls++;
	(void)crgpu_swap_chain_get_render_finished_semaphore((crgpu_swap_chain){0});
	calls++;
	crgpu_swap_chain_present((crgpu_swap_chain){0});
	unchecked++;
	calls++;
	expect_error("crgpu_timestamp_set_create", crgpu_timestamp_set_create((crgpu_device){0}, (crgpu_command_queue){0}, (uint32_t){0}, (crgpu_timestamp_set *){0}));
	calls++;
	crgpu_timestamp_set_destroy((crgpu_timestamp_set){0});
	unchecked++;
	calls++;
	expect_error("crgpu_timestamp_set_get", crgpu_timestamp_set_get((crgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crinput_gamepad_get_axis_label", crinput_gamepad_get_axis_label((crinput_instance){0}, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_AXIS){0}, (char *){0}, (uint32_t){0}));
	calls++;
	expect_error("crinput_gamepad_get_battery", crinput_gamepad_get_battery((crinput_instance){0}, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_BATTERY *){0}));
	calls++;
	expect_error("crinput_gamepad_get_button_label", crinput_gamepad_get_button_label((crinput_instance){0}, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_BUTTONS){0}, (char *){0}, (uint32_t){0}));
	calls++;
	expect_error("crinput_gamepad_get_motion", crinput_gamepad_get_motion((crinput_instance){0}, (crinput_gamepad_id){0}, (const CRINPUT_GAMEPAD_MOTION_SAMPLE **){0}, (uint32_t *){0}, (uint32_t *){0}));
	calls++;
	expect_error("crinput_gamepad_get_properties", crinput_gamepad_get_properties((crinput_instance){0}, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_PROPERTIES *){0}));
	calls++;
	expect_error("crinput_gamepad_get_state", crinput_gamepad_get_state((crinput_instance){0}, (crinput_gamepad_id){0}, (CRINPUT_GAMEPAD_STATE *){0}));
	calls++;
	expect_error("crinput_gamepad_get_touch", crinput_gamepad_get_touch((crinput_instance){0}, (crinput_gamepad_id){0}, (uint32_t){0}, (CRINPUT_GAMEPAD_TOUCH_STATE *){0}));
	calls++;
	expect_error("crinput_gamepad_set_light_color", crinput_gamepad_set_light_color((crinput_instance){0}, (crinput_gamepad_id){0}, (float){0}, (float){0}, (float){0}));
	calls++;
	expect_error("crinput_gamepad_set_motion_enabled", crinput_gamepad_set_motion_enabled((crinput_instance){0}, (crinput_gamepad_id){0}, (bool){0}));
	calls++;
	expect_error("crinput_gamepad_set_player_indicator", crinput_gamepad_set_player_indicator((crinput_instance){0}, (crinput_gamepad_id){0}, (uint32_t){0}));
	calls++;
	expect_error("crinput_gamepad_set_rumble", crinput_gamepad_set_rumble((crinput_instance){0}, (crinput_gamepad_id){0}, (const CRINPUT_GAMEPAD_RUMBLE *){0}));
	calls++;
	expect_error("crinput_instance_create", crinput_instance_create((const CRINPUT_INSTANCE_DESC *){0}, (crinput_instance *){0}));
	calls++;
	crinput_instance_destroy((crinput_instance){0});
	unchecked++;
	calls++;
	expect_error("crinput_instance_get_gamepads", crinput_instance_get_gamepads((crinput_instance){0}, (const crinput_gamepad_id **){0}, (uint32_t *){0}));
	calls++;
	(void)crinput_instance_get_native_api((crinput_instance){0});
	calls++;
	expect_error("crinput_instance_poll", crinput_instance_poll((crinput_instance){0}));
	calls++;
	expect_error("crmem_commit", crmem_commit((void *){0}, (uint64_t){0}));
	calls++;
	expect_error("crmem_decommit", crmem_decommit((void *){0}, (uint64_t){0}));
	calls++;
	(void)crmem_get_page_size();
	calls++;
	(void)crmem_get_reserve_granularity();
	calls++;
	expect_error("crmem_release", crmem_release((void *){0}, (uint64_t){0}));
	calls++;
	expect_error("crmem_reserve", crmem_reserve((uint64_t){0}, (void **){0}));
	calls++;
	expect_error("crnet_http_client_create", crnet_http_client_create((crnet_instance){0}, (const crnet_http_client_desc *){0}, (crnet_http_client *){0}));
	calls++;
	crnet_http_client_destroy((crnet_http_client){0});
	unchecked++;
	calls++;
	crnet_http_transfer_abort((crnet_http_transfer){0});
	unchecked++;
	calls++;
	expect_error("crnet_http_transfer_create", crnet_http_transfer_create((crnet_http_client){0}, (crnet_http_transfer *){0}));
	calls++;
	crnet_http_transfer_destroy((crnet_http_transfer){0});
	unchecked++;
	calls++;
	crnet_http_transfer_get_error((crnet_http_transfer){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	expect_error("crnet_http_transfer_get_header_at", crnet_http_transfer_get_header_at((crnet_http_transfer){0}, (uint32_t){0}, (const char **){0}, (const char **){0}));
	calls++;
	(void)crnet_http_transfer_get_header_count((crnet_http_transfer){0});
	calls++;
	(void)crnet_http_transfer_get_status((crnet_http_transfer){0});
	calls++;
	expect_error("crnet_http_transfer_receive_response", crnet_http_transfer_receive_response((crnet_http_transfer){0}, (uint32_t){0}));
	calls++;
	expect_error("crnet_http_transfer_recv", crnet_http_transfer_recv((crnet_http_transfer){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crnet_http_transfer_send", crnet_http_transfer_send((crnet_http_transfer){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crnet_http_transfer_start", crnet_http_transfer_start((crnet_http_transfer){0}, (const crnet_http_transfer_desc *){0}));
	calls++;
	expect_error("crnet_instance_create", crnet_instance_create((const crnet_instance_desc *){0}, (crnet_instance *){0}, (crnet_error *){0}));
	calls++;
	crnet_instance_destroy((crnet_instance){0});
	unchecked++;
	calls++;
	expect_error("crnet_instance_get_local_ipv4_addresses", crnet_instance_get_local_ipv4_addresses((crnet_instance){0}, (char (*)[16]){0}, (uint32_t){0}, (uint32_t *){0}, (crnet_error *){0}));
	calls++;
	crnet_resolver_abort((crnet_resolver){0});
	unchecked++;
	calls++;
	expect_error("crnet_resolver_create", crnet_resolver_create((crnet_instance){0}, (const crnet_resolver_desc *){0}, (crnet_resolver *){0}));
	calls++;
	crnet_resolver_destroy((crnet_resolver){0});
	unchecked++;
	calls++;
	crnet_resolver_get_error((crnet_resolver){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	expect_error("crnet_resolver_run", crnet_resolver_run((crnet_resolver){0}));
	calls++;
	expect_error("crnet_resolver_wait", crnet_resolver_wait((crnet_resolver){0}, (uint32_t){0}, (char (*)[46]){0}, (uint32_t){0}, (uint32_t *){0}));
	calls++;
	crnet_tcp_listener_abort((crnet_tcp_listener){0});
	unchecked++;
	calls++;
	expect_error("crnet_tcp_listener_accept", crnet_tcp_listener_accept((crnet_tcp_listener){0}, (uint32_t){0}, (crnet_tcp_socket *){0}));
	calls++;
	expect_error("crnet_tcp_listener_create", crnet_tcp_listener_create((crnet_instance){0}, (const crnet_tcp_listener_desc *){0}, (crnet_tcp_listener *){0}, (crnet_error *){0}));
	calls++;
	crnet_tcp_listener_destroy((crnet_tcp_listener){0});
	unchecked++;
	calls++;
	crnet_tcp_listener_get_error((crnet_tcp_listener){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	expect_error("crnet_tcp_listener_get_port", crnet_tcp_listener_get_port((crnet_tcp_listener){0}, (uint16_t *){0}));
	calls++;
	crnet_tcp_socket_abort((crnet_tcp_socket){0});
	unchecked++;
	calls++;
	expect_error("crnet_tcp_socket_connect", crnet_tcp_socket_connect((crnet_tcp_socket){0}, (const crnet_tcp_socket_connect_desc *){0}));
	calls++;
	expect_error("crnet_tcp_socket_connect_continue", crnet_tcp_socket_connect_continue((crnet_tcp_socket){0}, (uint32_t){0}));
	calls++;
	expect_error("crnet_tcp_socket_create", crnet_tcp_socket_create((crnet_instance){0}, (crnet_tcp_socket *){0}));
	calls++;
	crnet_tcp_socket_destroy((crnet_tcp_socket){0});
	unchecked++;
	calls++;
	expect_error("crnet_tcp_socket_finish_sending", crnet_tcp_socket_finish_sending((crnet_tcp_socket){0}));
	calls++;
	crnet_tcp_socket_get_error((crnet_tcp_socket){0}, (CRNET_ERROR_SIDE){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	expect_error("crnet_tcp_socket_get_peer_address", crnet_tcp_socket_get_peer_address((crnet_tcp_socket){0}, (char *){0}, (uint16_t *){0}));
	calls++;
	expect_error("crnet_tcp_socket_recv", crnet_tcp_socket_recv((crnet_tcp_socket){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crnet_tcp_socket_send", crnet_tcp_socket_send((crnet_tcp_socket){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("crnet_tcp_socket_set_no_delay", crnet_tcp_socket_set_no_delay((crnet_tcp_socket){0}, (bool){0}));
	calls++;
	crnet_wait_set_abort((crnet_wait_set){0});
	unchecked++;
	calls++;
	expect_error("crnet_wait_set_add_http_transfer", crnet_wait_set_add_http_transfer((crnet_wait_set){0}, (crnet_http_transfer){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("crnet_wait_set_add_tcp_listener", crnet_wait_set_add_tcp_listener((crnet_wait_set){0}, (crnet_tcp_listener){0}, (void *){0}));
	calls++;
	expect_error("crnet_wait_set_add_tcp_socket", crnet_wait_set_add_tcp_socket((crnet_wait_set){0}, (crnet_tcp_socket){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("crnet_wait_set_add_websocket", crnet_wait_set_add_websocket((crnet_wait_set){0}, (crnet_websocket){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("crnet_wait_set_create", crnet_wait_set_create((crnet_instance){0}, (crnet_wait_set *){0}, (crnet_error *){0}));
	calls++;
	crnet_wait_set_destroy((crnet_wait_set){0});
	unchecked++;
	calls++;
	crnet_wait_set_get_error((crnet_wait_set){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	crnet_wait_set_remove_http_transfer((crnet_wait_set){0}, (crnet_http_transfer){0});
	unchecked++;
	calls++;
	crnet_wait_set_remove_tcp_listener((crnet_wait_set){0}, (crnet_tcp_listener){0});
	unchecked++;
	calls++;
	crnet_wait_set_remove_tcp_socket((crnet_wait_set){0}, (crnet_tcp_socket){0});
	unchecked++;
	calls++;
	crnet_wait_set_remove_websocket((crnet_wait_set){0}, (crnet_websocket){0});
	unchecked++;
	calls++;
	expect_error("crnet_wait_set_wait", crnet_wait_set_wait((crnet_wait_set){0}, (uint32_t){0}, (crnet_wait_event *){0}, (uint32_t){0}, (uint32_t *){0}));
	calls++;
	crnet_wait_set_wake((crnet_wait_set){0});
	unchecked++;
	calls++;
	crnet_websocket_abort((crnet_websocket){0});
	unchecked++;
	calls++;
	expect_error("crnet_websocket_close", crnet_websocket_close((crnet_websocket){0}, (uint16_t){0}, (const char *){0}));
	calls++;
	expect_error("crnet_websocket_connect", crnet_websocket_connect((crnet_websocket){0}, (const crnet_websocket_connect_desc *){0}, (uint32_t *){0}));
	calls++;
	expect_error("crnet_websocket_connect_continue", crnet_websocket_connect_continue((crnet_websocket){0}, (uint32_t){0}, (uint32_t *){0}));
	calls++;
	expect_error("crnet_websocket_create", crnet_websocket_create((crnet_instance){0}, (crnet_websocket *){0}));
	calls++;
	crnet_websocket_destroy((crnet_websocket){0});
	unchecked++;
	calls++;
	expect_error("crnet_websocket_flush", crnet_websocket_flush((crnet_websocket){0}, (uint32_t){0}));
	calls++;
	(void)crnet_websocket_get_close_code((crnet_websocket){0});
	calls++;
	crnet_websocket_get_error((crnet_websocket){0}, (CRNET_ERROR_SIDE){0}, (crnet_error *){0});
	unchecked++;
	calls++;
	expect_error("crnet_websocket_recv", crnet_websocket_recv((crnet_websocket){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}, (CRNET_WEBSOCKET_MESSAGE *){0}, (bool *){0}));
	calls++;
	expect_error("crnet_websocket_send", crnet_websocket_send((crnet_websocket){0}, (CRNET_WEBSOCKET_MESSAGE){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}));
	calls++;
	(void)crperf_catch_current_clock_cycle();
	calls++;
	(void)crperf_get_current_time_milliseconds();
	calls++;
	(void)crperf_get_monotonic_time_ns();
	calls++;
	crperf_get_ram_usage((uint64_t *){0}, (uint64_t *){0});
	unchecked++;
	calls++;
	crperf_sleep_until_monotonic_ms((uint64_t){0});
	unchecked++;
	calls++;
	crperf_sleep_until_monotonic_ns((uint64_t){0});
	unchecked++;
	calls++;
	(void)crrandom_fill((void *){0}, (size_t){0}); // zeroes are valid: asking for no bytes succeeds, as crrandom.h says
	calls++;
	printf("cr_misuse_tests: %d calls with zeroes, %d of them returning nothing to check, %d failure(s)\n", calls, unchecked, failures);
#if MISUSE_FREED
	int freed_calls = 0, freed_unchecked = 0, freed_skipped = 0;
	failures += freed_half(&freed_calls, &freed_unchecked, &freed_skipped);
	printf("cr_misuse_tests: %d calls with a destroyed handle, %d of them returning nothing to check, %d skipped for want of a handle, %d failure(s) in all\n", freed_calls, freed_unchecked, freed_skipped, failures);
#elif MISUSE_SANITIZED
	printf("cr_misuse_tests: the freed-handle half skipped: under a sanitizer, which reports the check's read of freed memory\n");
#else
	printf("cr_misuse_tests: the freed-handle half skipped: not a Debug build, which has no check\n");
#endif
	return failures ? 1 : 0;
}
