// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.
// Every public function of SpudLib called with every argument zeroed
// (ROADMAP.md in the workspace, section 3.1). None may crash; one
// returning SPUDRESULT returns an error unless the generator lists zeroes as
// a valid call for it. One returning nothing is called and counted, since
// it can't refuse.
//
// Then, in a Debug build not under a sanitizer, every public function whose
// first parameter is a handle, called with one just destroyed (decision
// X16): no crash, and an error, or the null fallback of a getter.

#include "spudaudio.h"
#include "spudcore.h"
#include "spudfiles.h"
#include "spudgpu.h"
#include "spudinput.h"
#include "spudmemory.h"
#include "spudnet.h"
#include "spudperf.h"
#include "spudrandom.h"
#include <stdbool.h>
#include <stdio.h>

static int failures = 0;

static void expect_error(const char *name, SPUDRESULT result)
{
	if (result == SPUD_SUCCESS) {
		printf("FAIL %s: returned SPUD_SUCCESS for zeroes\n", name);
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
#include "spudgpu_test_shaders.h"
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
 * destroyed handle's memory then stays readable, with SPUD_DEAD in its tag,
 * whatever the system's allocator does with memory it is given back (the
 * Windows heap decommits freed pages, and reading one faults), and no new
 * object can be made in it. Locked, for an object freed on a thread of the
 * platform's. */
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

static int freed_failures = 0;

static void expect_refused(const char *name, SPUDRESULT result)
{
	if (result == SPUD_SUCCESS) {
		printf("FAIL %s: returned SPUD_SUCCESS for a destroyed handle\n", name);
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

/* A spudaudio_instance just destroyed, or NULL with `why` set. */
static spudaudio_instance make_dead_spudaudio_instance(void) {
	spudaudio_instance instance = make_audio_instance();
	if (instance)
		spudaudio_destroy_instance(instance);
	return instance;
}

static spudaudio_instance dead_spudaudio_instance(void) {
	static bool told = false;
	why = "";
	spudaudio_instance handle = make_dead_spudaudio_instance();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudaudio_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudaudio_stream just destroyed, or NULL with `why` set. */
static spudaudio_stream make_dead_spudaudio_stream(void) {
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
	return stream;
}

static spudaudio_stream dead_spudaudio_stream(void) {
	static bool told = false;
	why = "";
	spudaudio_stream handle = make_dead_spudaudio_stream();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudaudio_stream: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudaudio_device just destroyed, or NULL with `why` set. */
static spudaudio_device make_dead_spudaudio_device(void) {
	/* A device is the instance's: it goes with it. */
	spudaudio_instance instance = make_audio_instance();
	if (!instance)
		return NULL;
	spudaudio_device device = first_output(instance);
	spudaudio_destroy_instance(instance);
	return device;
}

static spudaudio_device dead_spudaudio_device(void) {
	static bool told = false;
	why = "";
	spudaudio_device handle = make_dead_spudaudio_device();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudaudio_device: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudfile_instance just destroyed, or NULL with `why` set. */
static spudfile_instance make_dead_spudfile_instance(void) {
	spudfile_instance instance = make_file_instance();
	if (instance)
		spudfile_instance_destroy(instance);
	return instance;
}

static spudfile_instance dead_spudfile_instance(void) {
	static bool told = false;
	why = "";
	spudfile_instance handle = make_dead_spudfile_instance();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudfile_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudfile_file just destroyed, or NULL with `why` set. */
static spudfile_file make_dead_spudfile_file(void) {
	spudfile_instance instance = make_file_instance();
	if (!instance)
		return NULL;
	spudfile_file file = make_file(instance);
	if (file)
		spudfile_file_close(file);
	spudfile_path_remove(instance, FREED_FILE);
	spudfile_instance_destroy(instance);
	return file;
}

static spudfile_file dead_spudfile_file(void) {
	static bool told = false;
	why = "";
	spudfile_file handle = make_dead_spudfile_file();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudfile_file: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudfile_listing just destroyed, or NULL with `why` set. */
static spudfile_listing make_dead_spudfile_listing(void) {
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
	return listing;
}

static spudfile_listing dead_spudfile_listing(void) {
	static bool told = false;
	why = "";
	spudfile_listing handle = make_dead_spudfile_listing();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudfile_listing: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudfile_mapping just destroyed, or NULL with `why` set. */
static spudfile_mapping make_dead_spudfile_mapping(void) {
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
	return mapping;
}

static spudfile_mapping dead_spudfile_mapping(void) {
	static bool told = false;
	why = "";
	spudfile_mapping handle = make_dead_spudfile_mapping();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudfile_mapping: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_adapter just destroyed, or NULL with `why` set. */
static spudgpu_adapter make_dead_spudgpu_adapter(void) {
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
	return adapter;
}

static spudgpu_adapter dead_spudgpu_adapter(void) {
	static bool told = false;
	why = "";
	spudgpu_adapter handle = make_dead_spudgpu_adapter();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_adapter: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_command_list just destroyed, or NULL with `why` set. */
static spudgpu_command_list make_dead_spudgpu_command_list(void) {
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
	return cmd;
}

static spudgpu_command_list dead_spudgpu_command_list(void) {
	static bool told = false;
	why = "";
	spudgpu_command_list handle = make_dead_spudgpu_command_list();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_command_list: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_device just destroyed, or NULL with `why` set. */
static spudgpu_device make_dead_spudgpu_device(void) {
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
	return device;
}

static spudgpu_device dead_spudgpu_device(void) {
	static bool told = false;
	why = "";
	spudgpu_device handle = make_dead_spudgpu_device();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_device: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_buffer just destroyed, or NULL with `why` set. */
static spudgpu_buffer make_dead_spudgpu_buffer(void) {
	if (!have_device())
		return NULL;
	spudgpu_buffer buffer = make_buffer();
	if (buffer)
		spudgpu_destroy_buffer(buffer);
	else
		why = "spudgpu_create_buffer failed";
	return buffer;
}

static spudgpu_buffer dead_spudgpu_buffer(void) {
	static bool told = false;
	why = "";
	spudgpu_buffer handle = make_dead_spudgpu_buffer();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_buffer: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_command_allocator just destroyed, or NULL with `why` set. */
static spudgpu_command_allocator make_dead_spudgpu_command_allocator(void) {
	if (!have_device())
		return NULL;
	spudgpu_command_allocator allocator = make_command_allocator();
	if (allocator)
		spudgpu_destroy_command_allocator(allocator);
	else
		why = "spudgpu_create_command_allocator failed";
	return allocator;
}

static spudgpu_command_allocator dead_spudgpu_command_allocator(void) {
	static bool told = false;
	why = "";
	spudgpu_command_allocator handle = make_dead_spudgpu_command_allocator();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_command_allocator: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_image just destroyed, or NULL with `why` set. */
static spudgpu_image make_dead_spudgpu_image(void) {
	if (!have_device())
		return NULL;
	spudgpu_image image = make_image();
	if (image)
		spudgpu_destroy_image(image);
	else
		why = "spudgpu_create_image failed";
	return image;
}

static spudgpu_image dead_spudgpu_image(void) {
	static bool told = false;
	why = "";
	spudgpu_image handle = make_dead_spudgpu_image();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_image: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_heap just destroyed, or NULL with `why` set. */
static spudgpu_heap make_dead_spudgpu_heap(void) {
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
	return heap;
}

static spudgpu_heap dead_spudgpu_heap(void) {
	static bool told = false;
	why = "";
	spudgpu_heap handle = make_dead_spudgpu_heap();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_heap: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_instance just destroyed, or NULL with `why` set. */
static spudgpu_instance make_dead_spudgpu_instance(void) {
	spudgpu_instance instance = make_gpu_instance();
	if (!instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_destroy_instance(instance);
	return instance;
}

static spudgpu_instance dead_spudgpu_instance(void) {
	static bool told = false;
	why = "";
	spudgpu_instance handle = make_dead_spudgpu_instance();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_acceleration_structure just destroyed, or NULL with `why` set. */
static spudgpu_acceleration_structure make_dead_spudgpu_acceleration_structure(void) {
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
	return structure;
}

static spudgpu_acceleration_structure dead_spudgpu_acceleration_structure(void) {
	static bool told = false;
	why = "";
	spudgpu_acceleration_structure handle = make_dead_spudgpu_acceleration_structure();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_acceleration_structure: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_buffer_view just destroyed, or NULL with `why` set. */
static spudgpu_buffer_view make_dead_spudgpu_buffer_view(void) {
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
	return view;
}

static spudgpu_buffer_view dead_spudgpu_buffer_view(void) {
	static bool told = false;
	why = "";
	spudgpu_buffer_view handle = make_dead_spudgpu_buffer_view();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_buffer_view: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_compute_pipeline just destroyed, or NULL with `why` set. */
static spudgpu_compute_pipeline make_dead_spudgpu_compute_pipeline(void) {
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
	return pipeline;
}

static spudgpu_compute_pipeline dead_spudgpu_compute_pipeline(void) {
	static bool told = false;
	why = "";
	spudgpu_compute_pipeline handle = make_dead_spudgpu_compute_pipeline();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_compute_pipeline: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_descriptor_pool just destroyed, or NULL with `why` set. */
static spudgpu_descriptor_pool make_dead_spudgpu_descriptor_pool(void) {
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
	return pool;
}

static spudgpu_descriptor_pool dead_spudgpu_descriptor_pool(void) {
	static bool told = false;
	why = "";
	spudgpu_descriptor_pool handle = make_dead_spudgpu_descriptor_pool();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_descriptor_pool: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_descriptor_set_layout just destroyed, or NULL with `why` set. */
static spudgpu_descriptor_set_layout make_dead_spudgpu_descriptor_set_layout(void) {
	if (!have_device())
		return NULL;
	const SPUDGPU_DESCRIPTOR_TYPE types[1] = {SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
	spudgpu_descriptor_set_layout layout   = make_set_layout(types, 1, SPUDGPU_SHADER_STAGE_FRAGMENT);
	if (layout)
		spudgpu_destroy_descriptor_set_layout(layout);
	else
		why = "spudgpu_create_descriptor_set_layout failed";
	return layout;
}

static spudgpu_descriptor_set_layout dead_spudgpu_descriptor_set_layout(void) {
	static bool told = false;
	why = "";
	spudgpu_descriptor_set_layout handle = make_dead_spudgpu_descriptor_set_layout();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_descriptor_set_layout: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_fence just destroyed, or NULL with `why` set. */
static spudgpu_fence make_dead_spudgpu_fence(void) {
	if (!have_device())
		return NULL;
	spudgpu_fence fence = NULL;
	if (spudgpu_create_fence(gpu_device, SPUDGPU_FENCE_FLAG_NONE, 0, &fence) == SPUD_SUCCESS)
		spudgpu_destroy_fence(fence);
	else
		why = "spudgpu_create_fence failed";
	return fence;
}

static spudgpu_fence dead_spudgpu_fence(void) {
	static bool told = false;
	why = "";
	spudgpu_fence handle = make_dead_spudgpu_fence();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_fence: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_image_view just destroyed, or NULL with `why` set. */
static spudgpu_image_view make_dead_spudgpu_image_view(void) {
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
	return view;
}

static spudgpu_image_view dead_spudgpu_image_view(void) {
	static bool told = false;
	why = "";
	spudgpu_image_view handle = make_dead_spudgpu_image_view();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_image_view: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_sampler just destroyed, or NULL with `why` set. */
static spudgpu_sampler make_dead_spudgpu_sampler(void) {
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
	return sampler;
}

static spudgpu_sampler dead_spudgpu_sampler(void) {
	static bool told = false;
	why = "";
	spudgpu_sampler handle = make_dead_spudgpu_sampler();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_sampler: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_semaphore just destroyed, or NULL with `why` set. */
static spudgpu_semaphore make_dead_spudgpu_semaphore(void) {
	if (!have_device())
		return NULL;
	spudgpu_semaphore semaphore = NULL;
	if (spudgpu_create_semaphore(gpu_device, &semaphore) == SPUD_SUCCESS)
		spudgpu_destroy_semaphore(semaphore);
	else
		why = "spudgpu_create_semaphore failed";
	return semaphore;
}

static spudgpu_semaphore dead_spudgpu_semaphore(void) {
	static bool told = false;
	why = "";
	spudgpu_semaphore handle = make_dead_spudgpu_semaphore();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_semaphore: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_shader_module just destroyed, or NULL with `why` set. */
static spudgpu_shader_module make_dead_spudgpu_shader_module(void) {
	if (!have_device())
		return NULL;
	spudgpu_shader_module module =
	    make_module(&spudgpu_test_objects_double_comp);
	if (module)
		spudgpu_destroy_shader_module(module);
	else
		why = "spudgpu_create_shader_module failed";
	return module;
}

static spudgpu_shader_module dead_spudgpu_shader_module(void) {
	static bool told = false;
	why = "";
	spudgpu_shader_module handle = make_dead_spudgpu_shader_module();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_shader_module: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_shader_pipeline just destroyed, or NULL with `why` set. */
static spudgpu_shader_pipeline make_dead_spudgpu_shader_pipeline(void) {
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
	return pipeline;
}

static spudgpu_shader_pipeline dead_spudgpu_shader_pipeline(void) {
	static bool told = false;
	why = "";
	spudgpu_shader_pipeline handle = make_dead_spudgpu_shader_pipeline();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_shader_pipeline: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_surface just destroyed, or NULL with `why` set. */
static spudgpu_surface make_dead_spudgpu_surface(void) {
	if (!gpu_instance) {
		why = "spudgpu_create_instance failed";
		return NULL;
	}
	spudgpu_surface surface = make_surface();
	if (surface)
		spudgpu_destroy_surface(surface);
	return surface;
}

static spudgpu_surface dead_spudgpu_surface(void) {
	static bool told = false;
	why = "";
	spudgpu_surface handle = make_dead_spudgpu_surface();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_surface: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_swap_chain just destroyed, or NULL with `why` set. */
static spudgpu_swap_chain make_dead_spudgpu_swap_chain(void) {
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
	return swap_chain;
}

static spudgpu_swap_chain dead_spudgpu_swap_chain(void) {
	static bool told = false;
	why = "";
	spudgpu_swap_chain handle = make_dead_spudgpu_swap_chain();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_swap_chain: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_timestamp_set just destroyed, or NULL with `why` set. */
static spudgpu_timestamp_set make_dead_spudgpu_timestamp_set(void) {
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
	return set;
}

static spudgpu_timestamp_set dead_spudgpu_timestamp_set(void) {
	static bool told = false;
	why = "";
	spudgpu_timestamp_set handle = make_dead_spudgpu_timestamp_set();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_timestamp_set: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudgpu_command_queue just destroyed, or NULL with `why` set. */
static spudgpu_command_queue make_dead_spudgpu_command_queue(void) {
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
	return queue;
}

static spudgpu_command_queue dead_spudgpu_command_queue(void) {
	static bool told = false;
	why = "";
	spudgpu_command_queue handle = make_dead_spudgpu_command_queue();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudgpu_command_queue: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudinput_instance just destroyed, or NULL with `why` set. */
static spudinput_instance make_dead_spudinput_instance(void) {
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
	return instance;
}

static spudinput_instance dead_spudinput_instance(void) {
	static bool told = false;
	why = "";
	spudinput_instance handle = make_dead_spudinput_instance();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudinput_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_instance just destroyed, or NULL with `why` set. */
static spudnet_instance make_dead_spudnet_instance(void) {
	spudnet_instance instance = make_net_instance();
	if (instance)
		spudnet_instance_destroy(instance);
	return instance;
}

static spudnet_instance dead_spudnet_instance(void) {
	static bool told = false;
	why = "";
	spudnet_instance handle = make_dead_spudnet_instance();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_instance: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_http_client just destroyed, or NULL with `why` set. */
static spudnet_http_client make_dead_spudnet_http_client(void) {
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_http_client client = make_http_client(instance);
	if (client)
		spudnet_http_client_destroy(client);
	spudnet_instance_destroy(instance);
	return client;
}

static spudnet_http_client dead_spudnet_http_client(void) {
	static bool told = false;
	why = "";
	spudnet_http_client handle = make_dead_spudnet_http_client();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_http_client: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_http_transfer just destroyed, or NULL with `why` set. */
static spudnet_http_transfer make_dead_spudnet_http_transfer(void) {
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
	return transfer;
}

static spudnet_http_transfer dead_spudnet_http_transfer(void) {
	static bool told = false;
	why = "";
	spudnet_http_transfer handle = make_dead_spudnet_http_transfer();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_http_transfer: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_resolver just destroyed, or NULL with `why` set. */
static spudnet_resolver make_dead_spudnet_resolver(void) {
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
	return resolver;
}

static spudnet_resolver dead_spudnet_resolver(void) {
	static bool told = false;
	why = "";
	spudnet_resolver handle = make_dead_spudnet_resolver();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_resolver: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_tcp_listener just destroyed, or NULL with `why` set. */
static spudnet_tcp_listener make_dead_spudnet_tcp_listener(void) {
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
	return listener;
}

static spudnet_tcp_listener dead_spudnet_tcp_listener(void) {
	static bool told = false;
	why = "";
	spudnet_tcp_listener handle = make_dead_spudnet_tcp_listener();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_tcp_listener: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_tcp_socket just destroyed, or NULL with `why` set. */
static spudnet_tcp_socket make_dead_spudnet_tcp_socket(void) {
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_tcp_socket socket = NULL;
	if (spudnet_tcp_socket_create(instance, &socket) == SPUD_SUCCESS)
		spudnet_tcp_socket_destroy(socket);
	else
		why = "spudnet_tcp_socket_create failed";
	spudnet_instance_destroy(instance);
	return socket;
}

static spudnet_tcp_socket dead_spudnet_tcp_socket(void) {
	static bool told = false;
	why = "";
	spudnet_tcp_socket handle = make_dead_spudnet_tcp_socket();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_tcp_socket: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_wait_set just destroyed, or NULL with `why` set. */
static spudnet_wait_set make_dead_spudnet_wait_set(void) {
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_wait_set set = NULL;
	if (spudnet_wait_set_create(instance, &set, NULL) == SPUD_SUCCESS)
		spudnet_wait_set_destroy(set);
	else
		why = "spudnet_wait_set_create failed";
	spudnet_instance_destroy(instance);
	return set;
}

static spudnet_wait_set dead_spudnet_wait_set(void) {
	static bool told = false;
	why = "";
	spudnet_wait_set handle = make_dead_spudnet_wait_set();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_wait_set: %s\n", why);
		told = true;
	}
	return handle;
}

/* A spudnet_websocket just destroyed, or NULL with `why` set. */
static spudnet_websocket make_dead_spudnet_websocket(void) {
	spudnet_instance instance = make_net_instance();
	if (!instance)
		return NULL;
	spudnet_websocket socket = NULL;
	if (spudnet_websocket_create(instance, &socket) == SPUD_SUCCESS)
		spudnet_websocket_destroy(socket);
	else
		why = "spudnet_websocket_create failed (no WebSocket in this build)";
	spudnet_instance_destroy(instance);
	return socket;
}

static spudnet_websocket dead_spudnet_websocket(void) {
	static bool told = false;
	why = "";
	spudnet_websocket handle = make_dead_spudnet_websocket();
	if (!handle && !told) {
		printf("spud_misuse_tests: not covered: spudnet_websocket: %s\n", why);
		told = true;
	}
	return handle;
}

static int freed_half(int *out_calls, int *out_unchecked, int *out_skipped)
{
	int calls = 0;
	int unchecked = 0;
	int skipped = 0;
	/* spudgpu_descriptor_set: no public function takes one first. */
	/* spudgpu_pipeline_layout: no public function takes one first. */
	/* spudnet_resolver_run: not called with a destroyed handle: may start after the resolver is destroyed (spudnet.h), so it has no stale check, and a resolver that has been run and destroyed is freed memory. */
	freed_setup();
	{
		spudaudio_instance dead = dead_spudaudio_instance();
		if (dead) {
			expect_refused("spudaudio_destroy_instance", spudaudio_destroy_instance(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			spudaudio_destroy_stream(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_instance dead = dead_spudaudio_instance();
		if (dead) {
			expect_refused("spudaudio_enumerate_devices", spudaudio_enumerate_devices(dead, (SPUDAUDIO_DIRECTION){0}, (spudaudio_device **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_device dead = dead_spudaudio_device();
		if (dead) {
			expect_refused("spudaudio_get_device_properties", spudaudio_get_device_properties(dead, (SPUDAUDIO_DEVICE_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_instance dead = dead_spudaudio_instance();
		if (dead) {
			expect_fallback("spudaudio_get_native_api", spudaudio_get_native_api(dead) == spudaudio_get_native_api((spudaudio_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_device dead = dead_spudaudio_device();
		if (dead) {
			expect_refused("spudaudio_get_timing_caps", spudaudio_get_timing_caps(dead, (const SPUDAUDIO_FORMAT *){0}, (SPUDAUDIO_STREAM_FLAGS){0}, (SPUDAUDIO_TIMING_CAPS *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_instance dead = dead_spudaudio_instance();
		if (dead) {
			expect_refused("spudaudio_set_device_event_callback", spudaudio_set_device_event_callback(dead, (SPUDAUDIO_DEVICE_EVENT_CALLBACK){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_device dead = dead_spudaudio_device();
		if (dead) {
			expect_refused("spudaudio_set_device_sample_rate", spudaudio_set_device_sample_rate(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			expect_refused("spudaudio_stream_get_config", spudaudio_stream_get_config(dead, (SPUDAUDIO_STREAM_CONFIG *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			expect_refused("spudaudio_stream_get_latency_frames", spudaudio_stream_get_latency_frames(dead, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			expect_refused("spudaudio_stream_get_status", spudaudio_stream_get_status(dead, (SPUDAUDIO_STREAM_STATUS *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			expect_refused("spudaudio_stream_start", spudaudio_stream_start(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudaudio_stream dead = dead_spudaudio_stream();
		if (dead) {
			expect_refused("spudaudio_stream_stop", spudaudio_stream_stop(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_directory_create", spudfile_directory_create(dead, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			spudfile_file_close(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_flush", spudfile_file_flush(dead, (SPUDFILE_FLUSH_LEVEL){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_get_size", spudfile_file_get_size(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_lock", spudfile_file_lock(dead, (SPUDFILE_LOCK){0}, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_map", spudfile_file_map(dead, (uint64_t){0}, (uint64_t){0}, (spudfile_mapping *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_file_open", spudfile_file_open(dead, (const spudfile_open_desc *){0}, (spudfile_file *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_read_at", spudfile_file_read_at(dead, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_set_size", spudfile_file_set_size(dead, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_unlock", spudfile_file_unlock(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_file dead = dead_spudfile_file();
		if (dead) {
			expect_refused("spudfile_file_write_at", spudfile_file_write_at(dead, (uint64_t){0}, (const void *){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			spudfile_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_listing dead = dead_spudfile_listing();
		if (dead) {
			spudfile_listing_close(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_listing dead = dead_spudfile_listing();
		if (dead) {
			expect_refused("spudfile_listing_next", spudfile_listing_next(dead, (SPUDFILE_LISTING_ENTRY *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_listing_open", spudfile_listing_open(dead, (const spudfile_listing_desc *){0}, (spudfile_listing *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_mapping dead = dead_spudfile_mapping();
		if (dead) {
			expect_fallback("spudfile_mapping_get_size", spudfile_mapping_get_size(dead) == spudfile_mapping_get_size((spudfile_mapping){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_mapping dead = dead_spudfile_mapping();
		if (dead) {
			spudfile_mapping_release(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_path_get_info", spudfile_path_get_info(dead, (const char *){0}, (SPUDFILE_PATH_INFO *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_path_remove", spudfile_path_remove(dead, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudfile_instance dead = dead_spudfile_instance();
		if (dead) {
			expect_refused("spudfile_path_replace", spudfile_path_replace(dead, (const char *){0}, (const char *){0}, (SPUDFILE_FLUSH_LEVEL){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_adapter dead = dead_spudgpu_adapter();
		if (dead) {
			expect_refused("spudgpu_adapter_supports_surface", spudgpu_adapter_supports_surface(dead, (spudgpu_surface){0}, (bool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_begin_bundle_command_list(dead, (const spudgpu_bundle_inheritance_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_begin_command_list(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_bindless_register_sampled_image", spudgpu_bindless_register_sampled_image(dead, (spudgpu_image_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_bindless_register_storage_buffer", spudgpu_bindless_register_storage_buffer(dead, (spudgpu_buffer_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_bindless_register_storage_image", spudgpu_bindless_register_storage_image(dead, (spudgpu_image_view){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			spudgpu_bindless_unregister_sampled_image(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			spudgpu_bindless_unregister_storage_buffer(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			spudgpu_bindless_unregister_storage_image(dead, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_acceleration_structure_barrier(dead, (const spudgpu_acceleration_structure_barrier *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_begin_debug_label(dead, (const char *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_begin_rendering(dead, (const spudgpu_rendering_begin_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_bindless_resources(dead, (spudgpu_shader_pipeline){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_bindless_resources_compute(dead, (spudgpu_compute_pipeline){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_compute_pipeline(dead, (spudgpu_compute_pipeline){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_descriptor_sets(dead, (spudgpu_shader_pipeline){0}, (uint32_t){0}, (const spudgpu_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_descriptor_sets_compute(dead, (spudgpu_compute_pipeline){0}, (uint32_t){0}, (const spudgpu_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_bind_pipeline(dead, (spudgpu_shader_pipeline){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_blit_image(dead, (spudgpu_image){0}, (spudgpu_image){0}, (const spudgpu_image_blit_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			expect_refused("spudgpu_cmd_build_acceleration_structures", spudgpu_cmd_build_acceleration_structures(dead, (const spudgpu_acceleration_structure_build *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_clear_color_attachment(dead, (spudgpu_image_view){0}, (float){0}, (float){0}, (float){0}, (float){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_clear_depth_attachment(dead, (spudgpu_image_view){0}, (bool){0}, (bool){0}, (float){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			expect_refused("spudgpu_cmd_copy_acceleration_structure", spudgpu_cmd_copy_acceleration_structure(dead, (spudgpu_acceleration_structure){0}, (spudgpu_acceleration_structure){0}, (SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_copy_buffer(dead, (spudgpu_buffer){0}, (spudgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (uint64_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_copy_buffer_to_image(dead, (spudgpu_buffer){0}, (spudgpu_image){0}, (const spudgpu_image_buffer_copy_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_copy_image_to_buffer(dead, (spudgpu_image){0}, (spudgpu_buffer){0}, (const spudgpu_image_buffer_copy_desc *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_dispatch(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_dispatch_mesh(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw(dead, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw_indexed(dead, (uint32_t){0}, (uint32_t){0}, (int32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw_indexed_indirect(dead, (spudgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw_indexed_instanced(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw_indirect(dead, (spudgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_draw_instanced(dead, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_end_debug_label(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_end_rendering(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_execute_bundle(dead, (spudgpu_command_list){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_image_barrier(dead, (spudgpu_image){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_image_barrier_subresource(dead, (spudgpu_image){0}, (const spudgpu_image_view_desc_subresource_range *){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_image_barrier_view(dead, (spudgpu_image_view){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_pipeline_barrier(dead, (const spudgpu_buffer_barrier *){0}, (uint32_t){0}, (const spudgpu_image_barrier *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_push_constants(dead, (spudgpu_shader_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_push_constants_compute(dead, (spudgpu_compute_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_reset_timestamps(dead, (spudgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_resolve_timestamps(dead, (spudgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_set_depth_bounds(dead, (float){0}, (float){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_set_index_buffer(dead, (spudgpu_buffer_view){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_set_scissor_rects(dead, (uint32_t){0}, (uint32_t){0}, (const SPUDGPU_SCISSOR_RECT *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_set_vertex_buffers(dead, (uint32_t){0}, (uint32_t){0}, (spudgpu_buffer_view *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_cmd_set_viewports(dead, (uint32_t){0}, (uint32_t){0}, (const SPUDGPU_VIEWPORT *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			expect_refused("spudgpu_cmd_write_acceleration_structure_compacted_sizes", spudgpu_cmd_write_acceleration_structure_compacted_sizes(dead, (const spudgpu_acceleration_structure *){0}, (uint32_t){0}, (spudgpu_buffer){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_acceleration_structure", spudgpu_create_acceleration_structure(dead, (const spudgpu_acceleration_structure_desc *){0}, (spudgpu_acceleration_structure *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_buffer", spudgpu_create_buffer(dead, (const spudgpu_buffer_desc *){0}, (spudgpu_buffer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_create_buffer_view", spudgpu_create_buffer_view(dead, (const spudgpu_buffer_view_desc *){0}, (spudgpu_buffer_view *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_command_allocator", spudgpu_create_command_allocator(dead, (const spudgpu_command_allocator_desc *){0}, (spudgpu_command_allocator *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_allocator dead = dead_spudgpu_command_allocator();
		if (dead) {
			expect_refused("spudgpu_create_command_list", spudgpu_create_command_list(dead, (spudgpu_command_list *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_compute_pipeline", spudgpu_create_compute_pipeline(dead, (const spudgpu_compute_pipeline_desc *){0}, (spudgpu_compute_pipeline *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_descriptor_pool", spudgpu_create_descriptor_pool(dead, (const spudgpu_descriptor_pool_desc *){0}, (spudgpu_descriptor_pool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_descriptor_set_layout", spudgpu_create_descriptor_set_layout(dead, (const spudgpu_descriptor_set_layout_desc *){0}, (spudgpu_descriptor_set_layout *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_descriptor_sets", spudgpu_create_descriptor_sets(dead, (const spudgpu_descriptor_set_desc *){0}, (spudgpu_descriptor_set *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_adapter dead = dead_spudgpu_adapter();
		if (dead) {
			expect_refused("spudgpu_create_device", spudgpu_create_device(dead, (const spudgpu_device_desc *){0}, (spudgpu_device *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_fence", spudgpu_create_fence(dead, (SPUDGPU_FENCE_FLAGS){0}, (uint64_t){0}, (spudgpu_fence *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_heap", spudgpu_create_heap(dead, (const spudgpu_heap_desc *){0}, (spudgpu_heap *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_image", spudgpu_create_image(dead, (const spudgpu_image_desc *){0}, (spudgpu_image *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image dead = dead_spudgpu_image();
		if (dead) {
			expect_refused("spudgpu_create_image_view", spudgpu_create_image_view(dead, (const spudgpu_image_view_desc *){0}, (spudgpu_image_view *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_create_placed_acceleration_structure", spudgpu_create_placed_acceleration_structure(dead, (uint64_t){0}, (const spudgpu_acceleration_structure_desc *){0}, (spudgpu_acceleration_structure *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_create_placed_buffer", spudgpu_create_placed_buffer(dead, (uint64_t){0}, (const spudgpu_buffer_desc *){0}, (spudgpu_buffer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_create_placed_image", spudgpu_create_placed_image(dead, (uint64_t){0}, (const spudgpu_image_desc *){0}, (spudgpu_image *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_sampler", spudgpu_create_sampler(dead, (const spudgpu_sampler_desc *){0}, (spudgpu_sampler *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_semaphore", spudgpu_create_semaphore(dead, (spudgpu_semaphore *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_shader_module", spudgpu_create_shader_module(dead, (const spudgpu_shader_module_desc *){0}, (spudgpu_shader_module *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_shader_pipeline", spudgpu_create_shader_pipeline(dead, (const spudgpu_shader_pipeline_desc *){0}, (spudgpu_shader_pipeline *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_instance dead = dead_spudgpu_instance();
		if (dead) {
			expect_refused("spudgpu_create_surface", spudgpu_create_surface(dead, (void *){0}, (void *){0}, (spudgpu_surface *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_instance dead = dead_spudgpu_instance();
		if (dead) {
			expect_fallback("spudgpu_create_surface_from_callback", spudgpu_create_surface_from_callback(dead, (void *){0}, (spudgpu_surface_create_fn){0}) == spudgpu_create_surface_from_callback((spudgpu_instance){0}, (void *){0}, (spudgpu_surface_create_fn){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_swap_chain", spudgpu_create_swap_chain(dead, (const spudgpu_swap_chain_desc *){0}, (spudgpu_swap_chain *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_create_timestamp_set", spudgpu_create_timestamp_set(dead, (spudgpu_command_queue){0}, (uint32_t){0}, (spudgpu_timestamp_set *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_acceleration_structure dead = dead_spudgpu_acceleration_structure();
		if (dead) {
			spudgpu_destroy_acceleration_structure(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			spudgpu_destroy_buffer(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer_view dead = dead_spudgpu_buffer_view();
		if (dead) {
			spudgpu_destroy_buffer_view(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_allocator dead = dead_spudgpu_command_allocator();
		if (dead) {
			spudgpu_destroy_command_allocator(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_destroy_command_list(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_compute_pipeline dead = dead_spudgpu_compute_pipeline();
		if (dead) {
			spudgpu_destroy_compute_pipeline(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_descriptor_pool dead = dead_spudgpu_descriptor_pool();
		if (dead) {
			spudgpu_destroy_descriptor_pool(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_descriptor_set_layout dead = dead_spudgpu_descriptor_set_layout();
		if (dead) {
			spudgpu_destroy_descriptor_set_layout(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			spudgpu_destroy_device(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_fence dead = dead_spudgpu_fence();
		if (dead) {
			spudgpu_destroy_fence(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			spudgpu_destroy_heap(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image dead = dead_spudgpu_image();
		if (dead) {
			spudgpu_destroy_image(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image_view dead = dead_spudgpu_image_view();
		if (dead) {
			spudgpu_destroy_image_view(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_instance dead = dead_spudgpu_instance();
		if (dead) {
			expect_fallback("spudgpu_destroy_instance", spudgpu_destroy_instance(dead) == spudgpu_destroy_instance((spudgpu_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_sampler dead = dead_spudgpu_sampler();
		if (dead) {
			spudgpu_destroy_sampler(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_semaphore dead = dead_spudgpu_semaphore();
		if (dead) {
			spudgpu_destroy_semaphore(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_shader_module dead = dead_spudgpu_shader_module();
		if (dead) {
			spudgpu_destroy_shader_module(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_shader_pipeline dead = dead_spudgpu_shader_pipeline();
		if (dead) {
			spudgpu_destroy_shader_pipeline(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_surface dead = dead_spudgpu_surface();
		if (dead) {
			spudgpu_destroy_surface(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			spudgpu_destroy_swap_chain(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_timestamp_set dead = dead_spudgpu_timestamp_set();
		if (dead) {
			spudgpu_destroy_timestamp_set(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_list dead = dead_spudgpu_command_list();
		if (dead) {
			spudgpu_end_command_list(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_instance dead = dead_spudgpu_instance();
		if (dead) {
			expect_refused("spudgpu_enumerate_adapters", spudgpu_enumerate_adapters(dead, (const spudgpu_adapter **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_flush_buffer", spudgpu_flush_buffer(dead, (uint64_t){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_get_acceleration_structure_build_sizes", spudgpu_get_acceleration_structure_build_sizes(dead, (const spudgpu_acceleration_structure_inputs *){0}, (spudgpu_acceleration_structure_sizes *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_get_acceleration_structure_instance_layout", spudgpu_get_acceleration_structure_instance_layout(dead, (spudgpu_acceleration_structure_instance_layout *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_acceleration_structure dead = dead_spudgpu_acceleration_structure();
		if (dead) {
			expect_refused("spudgpu_get_acceleration_structure_reference", spudgpu_get_acceleration_structure_reference(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_adapter dead = dead_spudgpu_adapter();
		if (dead) {
			expect_refused("spudgpu_get_adapter_properties", spudgpu_get_adapter_properties(dead, (SPUDGPU_ADAPTER_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#if defined(__linux__) || defined(_WIN32)
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_fallback("spudgpu_get_bindless_descriptor_set_layout", spudgpu_get_bindless_descriptor_set_layout(dead) == spudgpu_get_bindless_descriptor_set_layout((spudgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
#endif
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_get_buffer_desc", spudgpu_get_buffer_desc(dead, (spudgpu_buffer_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_get_buffer_gpu_address", spudgpu_get_buffer_gpu_address(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_get_buffer_placement_requirements", spudgpu_get_buffer_placement_requirements(dead, (const spudgpu_buffer_desc *){0}, (spudgpu_placement_requirements *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer_view dead = dead_spudgpu_buffer_view();
		if (dead) {
			expect_refused("spudgpu_get_buffer_view_desc", spudgpu_get_buffer_view_desc(dead, (spudgpu_buffer_view_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_get_command_queue", spudgpu_get_command_queue(dead, (SPUDGPU_COMMAND_LIST_TYPE){0}, (uint32_t){0}, (spudgpu_command_queue *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_compute_pipeline dead = dead_spudgpu_compute_pipeline();
		if (dead) {
			expect_refused("spudgpu_get_compute_pipeline_desc", spudgpu_get_compute_pipeline_desc(dead, (spudgpu_compute_pipeline_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_fallback("spudgpu_get_device_adapter", spudgpu_get_device_adapter(dead) == spudgpu_get_device_adapter((spudgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_fallback("spudgpu_get_device_features", spudgpu_get_device_features(dead) == spudgpu_get_device_features((spudgpu_device){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_fence dead = dead_spudgpu_fence();
		if (dead) {
			expect_fallback("spudgpu_get_fence_value", spudgpu_get_fence_value(dead) == spudgpu_get_fence_value((spudgpu_fence){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_get_heap_desc", spudgpu_get_heap_desc(dead, (spudgpu_heap_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image dead = dead_spudgpu_image();
		if (dead) {
			expect_refused("spudgpu_get_image_buffer_copy_offset_alignment", spudgpu_get_image_buffer_copy_offset_alignment(dead, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image dead = dead_spudgpu_image();
		if (dead) {
			spudgpu_get_image_buffer_copy_size(dead, (uint32_t){0}, (uint64_t *){0}, (uint64_t *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image dead = dead_spudgpu_image();
		if (dead) {
			expect_refused("spudgpu_get_image_desc", spudgpu_get_image_desc(dead, (spudgpu_image_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_heap dead = dead_spudgpu_heap();
		if (dead) {
			expect_refused("spudgpu_get_image_placement_requirements", spudgpu_get_image_placement_requirements(dead, (const spudgpu_image_desc *){0}, (spudgpu_placement_requirements *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_image_view dead = dead_spudgpu_image_view();
		if (dead) {
			expect_refused("spudgpu_get_image_view_desc", spudgpu_get_image_view_desc(dead, (spudgpu_image_view_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_instance dead = dead_spudgpu_instance();
		if (dead) {
			expect_fallback("spudgpu_get_native_gpu_api", spudgpu_get_native_gpu_api(dead) == spudgpu_get_native_gpu_api((spudgpu_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_get_surface_present_scalings", spudgpu_get_surface_present_scalings(dead, (spudgpu_surface){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			expect_refused("spudgpu_get_swap_chain_desc", spudgpu_get_swap_chain_desc(dead, (spudgpu_swap_chain_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			expect_fallback("spudgpu_get_swap_chain_image_view", spudgpu_get_swap_chain_image_view(dead, (uint32_t){0}) == spudgpu_get_swap_chain_image_view((spudgpu_swap_chain){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_timestamp_set dead = dead_spudgpu_timestamp_set();
		if (dead) {
			expect_refused("spudgpu_get_timestamps", spudgpu_get_timestamps(dead, (uint32_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_invalidate_buffer", spudgpu_invalidate_buffer(dead, (uint64_t){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			expect_refused("spudgpu_map_buffer", spudgpu_map_buffer(dead, (uint64_t){0}, (uint64_t){0}, (void **){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_queue dead = dead_spudgpu_command_queue();
		if (dead) {
			expect_refused("spudgpu_queue_submit", spudgpu_queue_submit(dead, (const spudgpu_submit_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_queue dead = dead_spudgpu_command_queue();
		if (dead) {
			spudgpu_queue_wait_idle(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_allocator dead = dead_spudgpu_command_allocator();
		if (dead) {
			expect_refused("spudgpu_reset_command_allocator", spudgpu_reset_command_allocator(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_descriptor_pool dead = dead_spudgpu_descriptor_pool();
		if (dead) {
			spudgpu_reset_descriptor_pool(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_signal_fence", spudgpu_signal_fence(dead, (spudgpu_fence){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_queue dead = dead_spudgpu_command_queue();
		if (dead) {
			expect_refused("spudgpu_submit_command_lists", spudgpu_submit_command_lists(dead, (spudgpu_command_list *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_command_queue dead = dead_spudgpu_command_queue();
		if (dead) {
			expect_refused("spudgpu_submit_command_lists_synced", spudgpu_submit_command_lists_synced(dead, (spudgpu_command_list *){0}, (uint32_t){0}, (spudgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			expect_fallback("spudgpu_swap_chain_acquire_next_image", spudgpu_swap_chain_acquire_next_image(dead) == spudgpu_swap_chain_acquire_next_image((spudgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			expect_fallback("spudgpu_swap_chain_get_image_available_semaphore", spudgpu_swap_chain_get_image_available_semaphore(dead) == spudgpu_swap_chain_get_image_available_semaphore((spudgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			expect_fallback("spudgpu_swap_chain_get_render_finished_semaphore", spudgpu_swap_chain_get_render_finished_semaphore(dead) == spudgpu_swap_chain_get_render_finished_semaphore((spudgpu_swap_chain){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_swap_chain dead = dead_spudgpu_swap_chain();
		if (dead) {
			spudgpu_swap_chain_present(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_buffer dead = dead_spudgpu_buffer();
		if (dead) {
			spudgpu_unmap_buffer(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			spudgpu_update_descriptor_sets(dead, (const spudgpu_write_descriptor_set *){0}, (uint32_t){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_wait_for_fences", spudgpu_wait_for_fences(dead, (spudgpu_fence *){0}, (const uint64_t *){0}, (uint32_t){0}, (bool){0}, (uint64_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudgpu_device dead = dead_spudgpu_device();
		if (dead) {
			expect_refused("spudgpu_write_acceleration_structure_instances", spudgpu_write_acceleration_structure_instances(dead, (const spudgpu_acceleration_structure_instance *){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_axis_label", spudinput_gamepad_get_axis_label(dead, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_AXIS){0}, (char *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_battery", spudinput_gamepad_get_battery(dead, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_BATTERY *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_button_label", spudinput_gamepad_get_button_label(dead, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_BUTTONS){0}, (char *){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_motion", spudinput_gamepad_get_motion(dead, (spudinput_gamepad_id){0}, (const SPUDINPUT_GAMEPAD_MOTION_SAMPLE **){0}, (uint32_t *){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_properties", spudinput_gamepad_get_properties(dead, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_PROPERTIES *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_state", spudinput_gamepad_get_state(dead, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_STATE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_get_touch", spudinput_gamepad_get_touch(dead, (spudinput_gamepad_id){0}, (uint32_t){0}, (SPUDINPUT_GAMEPAD_TOUCH_STATE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_set_light_color", spudinput_gamepad_set_light_color(dead, (spudinput_gamepad_id){0}, (float){0}, (float){0}, (float){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_set_motion_enabled", spudinput_gamepad_set_motion_enabled(dead, (spudinput_gamepad_id){0}, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_set_player_indicator", spudinput_gamepad_set_player_indicator(dead, (spudinput_gamepad_id){0}, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_gamepad_set_rumble", spudinput_gamepad_set_rumble(dead, (spudinput_gamepad_id){0}, (const SPUDINPUT_GAMEPAD_RUMBLE *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			spudinput_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_instance_get_gamepads", spudinput_instance_get_gamepads(dead, (const spudinput_gamepad_id **){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_fallback("spudinput_instance_get_native_api", spudinput_instance_get_native_api(dead) == spudinput_instance_get_native_api((spudinput_instance){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudinput_instance dead = dead_spudinput_instance();
		if (dead) {
			expect_refused("spudinput_instance_poll", spudinput_instance_poll(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_get_local_ipv4_addresses", spudnet_get_local_ipv4_addresses(dead, (char (*)[16]){0}, (uint32_t){0}, (uint32_t *){0}, (spudnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_http_client_create", spudnet_http_client_create(dead, (const spudnet_http_client_desc *){0}, (spudnet_http_client *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_client dead = dead_spudnet_http_client();
		if (dead) {
			spudnet_http_client_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			spudnet_http_transfer_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_client dead = dead_spudnet_http_client();
		if (dead) {
			expect_refused("spudnet_http_transfer_create", spudnet_http_transfer_create(dead, (spudnet_http_transfer *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			spudnet_http_transfer_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			spudnet_http_transfer_get_error(dead, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_refused("spudnet_http_transfer_get_header_at", spudnet_http_transfer_get_header_at(dead, (uint32_t){0}, (const char **){0}, (const char **){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_fallback("spudnet_http_transfer_get_header_count", spudnet_http_transfer_get_header_count(dead) == spudnet_http_transfer_get_header_count((spudnet_http_transfer){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_fallback("spudnet_http_transfer_get_status", spudnet_http_transfer_get_status(dead) == spudnet_http_transfer_get_status((spudnet_http_transfer){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_refused("spudnet_http_transfer_receive_response", spudnet_http_transfer_receive_response(dead, (uint32_t){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_refused("spudnet_http_transfer_recv", spudnet_http_transfer_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_refused("spudnet_http_transfer_send", spudnet_http_transfer_send(dead, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_http_transfer dead = dead_spudnet_http_transfer();
		if (dead) {
			expect_refused("spudnet_http_transfer_start", spudnet_http_transfer_start(dead, (const spudnet_http_transfer_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			spudnet_instance_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_resolver dead = dead_spudnet_resolver();
		if (dead) {
			spudnet_resolver_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_resolver_create", spudnet_resolver_create(dead, (const spudnet_resolver_desc *){0}, (spudnet_resolver *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_resolver dead = dead_spudnet_resolver();
		if (dead) {
			spudnet_resolver_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_resolver dead = dead_spudnet_resolver();
		if (dead) {
			spudnet_resolver_get_error(dead, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_resolver dead = dead_spudnet_resolver();
		if (dead) {
			expect_refused("spudnet_resolver_wait", spudnet_resolver_wait(dead, (uint32_t){0}, (char (*)[46]){0}, (uint32_t){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_listener dead = dead_spudnet_tcp_listener();
		if (dead) {
			spudnet_tcp_listener_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_listener dead = dead_spudnet_tcp_listener();
		if (dead) {
			expect_refused("spudnet_tcp_listener_accept", spudnet_tcp_listener_accept(dead, (uint32_t){0}, (spudnet_tcp_socket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_tcp_listener_create", spudnet_tcp_listener_create(dead, (const spudnet_tcp_listener_desc *){0}, (spudnet_tcp_listener *){0}, (spudnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_listener dead = dead_spudnet_tcp_listener();
		if (dead) {
			spudnet_tcp_listener_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_listener dead = dead_spudnet_tcp_listener();
		if (dead) {
			spudnet_tcp_listener_get_error(dead, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_listener dead = dead_spudnet_tcp_listener();
		if (dead) {
			expect_refused("spudnet_tcp_listener_get_port", spudnet_tcp_listener_get_port(dead, (uint16_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			spudnet_tcp_socket_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_connect", spudnet_tcp_socket_connect(dead, (const spudnet_tcp_socket_connect_desc *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_tcp_socket_create", spudnet_tcp_socket_create(dead, (spudnet_tcp_socket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			spudnet_tcp_socket_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_finish_sending", spudnet_tcp_socket_finish_sending(dead));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			spudnet_tcp_socket_get_error(dead, (SPUDNET_ERROR_SIDE){0}, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_get_peer_address", spudnet_tcp_socket_get_peer_address(dead, (char *){0}, (uint16_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_recv", spudnet_tcp_socket_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_send", spudnet_tcp_socket_send(dead, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_tcp_socket dead = dead_spudnet_tcp_socket();
		if (dead) {
			expect_refused("spudnet_tcp_socket_set_no_delay", spudnet_tcp_socket_set_no_delay(dead, (bool){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			expect_refused("spudnet_wait_set_add_tcp_listener", spudnet_wait_set_add_tcp_listener(dead, (spudnet_tcp_listener){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			expect_refused("spudnet_wait_set_add_tcp_socket", spudnet_wait_set_add_tcp_socket(dead, (spudnet_tcp_socket){0}, (uint32_t){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			expect_refused("spudnet_wait_set_add_websocket", spudnet_wait_set_add_websocket(dead, (spudnet_websocket){0}, (void *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_wait_set_create", spudnet_wait_set_create(dead, (spudnet_wait_set *){0}, (spudnet_error *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_get_error(dead, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_remove_tcp_listener(dead, (spudnet_tcp_listener){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_remove_tcp_socket(dead, (spudnet_tcp_socket){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_remove_websocket(dead, (spudnet_websocket){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			expect_refused("spudnet_wait_set_wait", spudnet_wait_set_wait(dead, (uint32_t){0}, (spudnet_wait_event *){0}, (uint32_t){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_wait_set dead = dead_spudnet_wait_set();
		if (dead) {
			spudnet_wait_set_wake(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			spudnet_websocket_abort(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			expect_refused("spudnet_websocket_close", spudnet_websocket_close(dead, (uint16_t){0}, (const char *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			expect_refused("spudnet_websocket_connect", spudnet_websocket_connect(dead, (const spudnet_websocket_connect_desc *){0}, (uint32_t *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_instance dead = dead_spudnet_instance();
		if (dead) {
			expect_refused("spudnet_websocket_create", spudnet_websocket_create(dead, (spudnet_websocket *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			spudnet_websocket_destroy(dead);
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			expect_fallback("spudnet_websocket_get_close_code", spudnet_websocket_get_close_code(dead) == spudnet_websocket_get_close_code((spudnet_websocket){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			spudnet_websocket_get_error(dead, (SPUDNET_ERROR_SIDE){0}, (spudnet_error *){0});
			unchecked++;
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			expect_refused("spudnet_websocket_recv", spudnet_websocket_recv(dead, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}, (SPUDNET_WEBSOCKET_MESSAGE *){0}, (bool *){0}));
			calls++;
		} else {
			skipped++;
		}
	}
	{
		spudnet_websocket dead = dead_spudnet_websocket();
		if (dead) {
			expect_refused("spudnet_websocket_send", spudnet_websocket_send(dead, (SPUDNET_WEBSOCKET_MESSAGE){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}));
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
	(void)spud_allocator_system();
	calls++;
	expect_error("spud_debug_name_set", spud_debug_name_set((void *){0}, (const char *){0}));
	calls++;
	(void)spud_get_version();
	calls++;
	expect_error("spudaudio_create_instance", spudaudio_create_instance((const SPUDAUDIO_INSTANCE_DESC *){0}, (spudaudio_instance *){0}));
	calls++;
	expect_error("spudaudio_create_stream", spudaudio_create_stream((const SPUDAUDIO_STREAM_DESC *){0}, (spudaudio_stream *){0}));
	calls++;
	expect_error("spudaudio_destroy_instance", spudaudio_destroy_instance((spudaudio_instance){0}));
	calls++;
	spudaudio_destroy_stream((spudaudio_stream){0});
	unchecked++;
	calls++;
	expect_error("spudaudio_enumerate_devices", spudaudio_enumerate_devices((spudaudio_instance){0}, (SPUDAUDIO_DIRECTION){0}, (spudaudio_device **){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudaudio_get_device_properties", spudaudio_get_device_properties((spudaudio_device){0}, (SPUDAUDIO_DEVICE_PROPERTIES *){0}));
	calls++;
	(void)spudaudio_get_native_api((spudaudio_instance){0});
	calls++;
	expect_error("spudaudio_get_timing_caps", spudaudio_get_timing_caps((spudaudio_device){0}, (const SPUDAUDIO_FORMAT *){0}, (SPUDAUDIO_STREAM_FLAGS){0}, (SPUDAUDIO_TIMING_CAPS *){0}));
	calls++;
	expect_error("spudaudio_probe_stream", spudaudio_probe_stream((const SPUDAUDIO_STREAM_DESC *){0}, (SPUDAUDIO_STREAM_CONFIG *){0}));
	calls++;
	(void)spudaudio_sample_format_byte_size((SPUDAUDIO_SAMPLE_FORMAT){0});
	calls++;
	expect_error("spudaudio_set_device_event_callback", spudaudio_set_device_event_callback((spudaudio_instance){0}, (SPUDAUDIO_DEVICE_EVENT_CALLBACK){0}, (void *){0}));
	calls++;
	expect_error("spudaudio_set_device_sample_rate", spudaudio_set_device_sample_rate((spudaudio_device){0}, (uint32_t){0}));
	calls++;
	expect_error("spudaudio_stream_get_config", spudaudio_stream_get_config((spudaudio_stream){0}, (SPUDAUDIO_STREAM_CONFIG *){0}));
	calls++;
	expect_error("spudaudio_stream_get_latency_frames", spudaudio_stream_get_latency_frames((spudaudio_stream){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudaudio_stream_get_status", spudaudio_stream_get_status((spudaudio_stream){0}, (SPUDAUDIO_STREAM_STATUS *){0}));
	calls++;
	expect_error("spudaudio_stream_start", spudaudio_stream_start((spudaudio_stream){0}));
	calls++;
	expect_error("spudaudio_stream_stop", spudaudio_stream_stop((spudaudio_stream){0}));
	calls++;
	expect_error("spudfile_directory_create", spudfile_directory_create((spudfile_instance){0}, (const char *){0}));
	calls++;
	spudfile_file_close((spudfile_file){0});
	unchecked++;
	calls++;
	expect_error("spudfile_file_flush", spudfile_file_flush((spudfile_file){0}, (SPUDFILE_FLUSH_LEVEL){0}));
	calls++;
	expect_error("spudfile_file_get_size", spudfile_file_get_size((spudfile_file){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudfile_file_lock", spudfile_file_lock((spudfile_file){0}, (SPUDFILE_LOCK){0}, (bool){0}));
	calls++;
	expect_error("spudfile_file_map", spudfile_file_map((spudfile_file){0}, (uint64_t){0}, (uint64_t){0}, (spudfile_mapping *){0}));
	calls++;
	expect_error("spudfile_file_open", spudfile_file_open((spudfile_instance){0}, (const spudfile_open_desc *){0}, (spudfile_file *){0}));
	calls++;
	expect_error("spudfile_file_read_at", spudfile_file_read_at((spudfile_file){0}, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudfile_file_set_size", spudfile_file_set_size((spudfile_file){0}, (uint64_t){0}));
	calls++;
	expect_error("spudfile_file_unlock", spudfile_file_unlock((spudfile_file){0}));
	calls++;
	expect_error("spudfile_file_write_at", spudfile_file_write_at((spudfile_file){0}, (uint64_t){0}, (const void *){0}, (uint64_t){0}));
	calls++;
	expect_error("spudfile_instance_create", spudfile_instance_create((const spudfile_instance_desc *){0}, (spudfile_instance *){0}));
	calls++;
	spudfile_instance_destroy((spudfile_instance){0});
	unchecked++;
	calls++;
	spudfile_listing_close((spudfile_listing){0});
	unchecked++;
	calls++;
	expect_error("spudfile_listing_next", spudfile_listing_next((spudfile_listing){0}, (SPUDFILE_LISTING_ENTRY *){0}));
	calls++;
	expect_error("spudfile_listing_open", spudfile_listing_open((spudfile_instance){0}, (const spudfile_listing_desc *){0}, (spudfile_listing *){0}));
	calls++;
	(void)spudfile_mapping_get_size((spudfile_mapping){0});
	calls++;
	spudfile_mapping_release((spudfile_mapping){0});
	unchecked++;
	calls++;
	expect_error("spudfile_path_get_info", spudfile_path_get_info((spudfile_instance){0}, (const char *){0}, (SPUDFILE_PATH_INFO *){0}));
	calls++;
	expect_error("spudfile_path_remove", spudfile_path_remove((spudfile_instance){0}, (const char *){0}));
	calls++;
	expect_error("spudfile_path_replace", spudfile_path_replace((spudfile_instance){0}, (const char *){0}, (const char *){0}, (SPUDFILE_FLUSH_LEVEL){0}));
	calls++;
	expect_error("spudgpu_adapter_supports_surface", spudgpu_adapter_supports_surface((spudgpu_adapter){0}, (spudgpu_surface){0}, (bool *){0}));
	calls++;
#if defined(__linux__) || defined(_WIN32)
	spudgpu_begin_bundle_command_list((spudgpu_command_list){0}, (const spudgpu_bundle_inheritance_desc *){0});
	unchecked++;
	calls++;
#endif
	spudgpu_begin_command_list((spudgpu_command_list){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	expect_error("spudgpu_bindless_register_sampled_image", spudgpu_bindless_register_sampled_image((spudgpu_device){0}, (spudgpu_image_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	expect_error("spudgpu_bindless_register_storage_buffer", spudgpu_bindless_register_storage_buffer((spudgpu_device){0}, (spudgpu_buffer_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	expect_error("spudgpu_bindless_register_storage_image", spudgpu_bindless_register_storage_image((spudgpu_device){0}, (spudgpu_image_view){0}, (uint32_t *){0}));
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	spudgpu_bindless_unregister_sampled_image((spudgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	spudgpu_bindless_unregister_storage_buffer((spudgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	spudgpu_bindless_unregister_storage_image((spudgpu_device){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
	spudgpu_cmd_acceleration_structure_barrier((spudgpu_command_list){0}, (const spudgpu_acceleration_structure_barrier *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_begin_debug_label((spudgpu_command_list){0}, (const char *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_begin_rendering((spudgpu_command_list){0}, (const spudgpu_rendering_begin_desc *){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	spudgpu_cmd_bind_bindless_resources((spudgpu_command_list){0}, (spudgpu_shader_pipeline){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
#if defined(__linux__) || defined(_WIN32)
	spudgpu_cmd_bind_bindless_resources_compute((spudgpu_command_list){0}, (spudgpu_compute_pipeline){0}, (uint32_t){0});
	unchecked++;
	calls++;
#endif
	spudgpu_cmd_bind_compute_pipeline((spudgpu_command_list){0}, (spudgpu_compute_pipeline){0});
	unchecked++;
	calls++;
	spudgpu_cmd_bind_descriptor_sets((spudgpu_command_list){0}, (spudgpu_shader_pipeline){0}, (uint32_t){0}, (const spudgpu_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_bind_descriptor_sets_compute((spudgpu_command_list){0}, (spudgpu_compute_pipeline){0}, (uint32_t){0}, (const spudgpu_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_bind_pipeline((spudgpu_command_list){0}, (spudgpu_shader_pipeline){0});
	unchecked++;
	calls++;
	spudgpu_cmd_blit_image((spudgpu_command_list){0}, (spudgpu_image){0}, (spudgpu_image){0}, (const spudgpu_image_blit_desc *){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_cmd_build_acceleration_structures", spudgpu_cmd_build_acceleration_structures((spudgpu_command_list){0}, (const spudgpu_acceleration_structure_build *){0}, (uint32_t){0}));
	calls++;
	spudgpu_cmd_clear_color_attachment((spudgpu_command_list){0}, (spudgpu_image_view){0}, (float){0}, (float){0}, (float){0}, (float){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_clear_depth_attachment((spudgpu_command_list){0}, (spudgpu_image_view){0}, (bool){0}, (bool){0}, (float){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_cmd_copy_acceleration_structure", spudgpu_cmd_copy_acceleration_structure((spudgpu_command_list){0}, (spudgpu_acceleration_structure){0}, (spudgpu_acceleration_structure){0}, (SPUDGPU_ACCELERATION_STRUCTURE_COPY_MODE){0}));
	calls++;
	spudgpu_cmd_copy_buffer((spudgpu_command_list){0}, (spudgpu_buffer){0}, (spudgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (uint64_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_copy_buffer_to_image((spudgpu_command_list){0}, (spudgpu_buffer){0}, (spudgpu_image){0}, (const spudgpu_image_buffer_copy_desc *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_copy_image_to_buffer((spudgpu_command_list){0}, (spudgpu_image){0}, (spudgpu_buffer){0}, (const spudgpu_image_buffer_copy_desc *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_dispatch((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_dispatch_mesh((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw_indexed((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw_indexed_indirect((spudgpu_command_list){0}, (spudgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw_indexed_instanced((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (int32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw_indirect((spudgpu_command_list){0}, (spudgpu_buffer){0}, (uint64_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_draw_instanced((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_end_debug_label((spudgpu_command_list){0});
	unchecked++;
	calls++;
	spudgpu_cmd_end_rendering((spudgpu_command_list){0});
	unchecked++;
	calls++;
#if defined(__linux__) || defined(_WIN32)
	spudgpu_cmd_execute_bundle((spudgpu_command_list){0}, (spudgpu_command_list){0});
	unchecked++;
	calls++;
#endif
	spudgpu_cmd_image_barrier((spudgpu_command_list){0}, (spudgpu_image){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	spudgpu_cmd_image_barrier_subresource((spudgpu_command_list){0}, (spudgpu_image){0}, (const spudgpu_image_view_desc_subresource_range *){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	spudgpu_cmd_image_barrier_view((spudgpu_command_list){0}, (spudgpu_image_view){0}, (SPUDGPU_IMAGE_LAYOUT){0}, (SPUDGPU_IMAGE_LAYOUT){0});
	unchecked++;
	calls++;
	spudgpu_cmd_pipeline_barrier((spudgpu_command_list){0}, (const spudgpu_buffer_barrier *){0}, (uint32_t){0}, (const spudgpu_image_barrier *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_push_constants((spudgpu_command_list){0}, (spudgpu_shader_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_push_constants_compute((spudgpu_command_list){0}, (spudgpu_compute_pipeline){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_reset_timestamps((spudgpu_command_list){0}, (spudgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_resolve_timestamps((spudgpu_command_list){0}, (spudgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0});
	unchecked++;
	calls++;
	spudgpu_cmd_set_depth_bounds((spudgpu_command_list){0}, (float){0}, (float){0});
	unchecked++;
	calls++;
	spudgpu_cmd_set_index_buffer((spudgpu_command_list){0}, (spudgpu_buffer_view){0});
	unchecked++;
	calls++;
	spudgpu_cmd_set_scissor_rects((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (const SPUDGPU_SCISSOR_RECT *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_set_vertex_buffers((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (spudgpu_buffer_view *){0});
	unchecked++;
	calls++;
	spudgpu_cmd_set_viewports((spudgpu_command_list){0}, (uint32_t){0}, (uint32_t){0}, (const SPUDGPU_VIEWPORT *){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_cmd_write_acceleration_structure_compacted_sizes", spudgpu_cmd_write_acceleration_structure_compacted_sizes((spudgpu_command_list){0}, (const spudgpu_acceleration_structure *){0}, (uint32_t){0}, (spudgpu_buffer){0}, (uint64_t){0}));
	calls++;
	expect_error("spudgpu_create_acceleration_structure", spudgpu_create_acceleration_structure((spudgpu_device){0}, (const spudgpu_acceleration_structure_desc *){0}, (spudgpu_acceleration_structure *){0}));
	calls++;
	expect_error("spudgpu_create_buffer", spudgpu_create_buffer((spudgpu_device){0}, (const spudgpu_buffer_desc *){0}, (spudgpu_buffer *){0}));
	calls++;
	expect_error("spudgpu_create_buffer_view", spudgpu_create_buffer_view((spudgpu_buffer){0}, (const spudgpu_buffer_view_desc *){0}, (spudgpu_buffer_view *){0}));
	calls++;
	expect_error("spudgpu_create_command_allocator", spudgpu_create_command_allocator((spudgpu_device){0}, (const spudgpu_command_allocator_desc *){0}, (spudgpu_command_allocator *){0}));
	calls++;
	expect_error("spudgpu_create_command_list", spudgpu_create_command_list((spudgpu_command_allocator){0}, (spudgpu_command_list *){0}));
	calls++;
	expect_error("spudgpu_create_compute_pipeline", spudgpu_create_compute_pipeline((spudgpu_device){0}, (const spudgpu_compute_pipeline_desc *){0}, (spudgpu_compute_pipeline *){0}));
	calls++;
	expect_error("spudgpu_create_descriptor_pool", spudgpu_create_descriptor_pool((spudgpu_device){0}, (const spudgpu_descriptor_pool_desc *){0}, (spudgpu_descriptor_pool *){0}));
	calls++;
	expect_error("spudgpu_create_descriptor_set_layout", spudgpu_create_descriptor_set_layout((spudgpu_device){0}, (const spudgpu_descriptor_set_layout_desc *){0}, (spudgpu_descriptor_set_layout *){0}));
	calls++;
	expect_error("spudgpu_create_descriptor_sets", spudgpu_create_descriptor_sets((spudgpu_device){0}, (const spudgpu_descriptor_set_desc *){0}, (spudgpu_descriptor_set *){0}));
	calls++;
	expect_error("spudgpu_create_device", spudgpu_create_device((spudgpu_adapter){0}, (const spudgpu_device_desc *){0}, (spudgpu_device *){0}));
	calls++;
	expect_error("spudgpu_create_fence", spudgpu_create_fence((spudgpu_device){0}, (SPUDGPU_FENCE_FLAGS){0}, (uint64_t){0}, (spudgpu_fence *){0}));
	calls++;
	expect_error("spudgpu_create_heap", spudgpu_create_heap((spudgpu_device){0}, (const spudgpu_heap_desc *){0}, (spudgpu_heap *){0}));
	calls++;
	expect_error("spudgpu_create_image", spudgpu_create_image((spudgpu_device){0}, (const spudgpu_image_desc *){0}, (spudgpu_image *){0}));
	calls++;
	expect_error("spudgpu_create_image_view", spudgpu_create_image_view((spudgpu_image){0}, (const spudgpu_image_view_desc *){0}, (spudgpu_image_view *){0}));
	calls++;
	expect_error("spudgpu_create_instance", spudgpu_create_instance((const spudgpu_instance_desc *){0}, (spudgpu_instance *){0}));
	calls++;
	expect_error("spudgpu_create_placed_acceleration_structure", spudgpu_create_placed_acceleration_structure((spudgpu_heap){0}, (uint64_t){0}, (const spudgpu_acceleration_structure_desc *){0}, (spudgpu_acceleration_structure *){0}));
	calls++;
	expect_error("spudgpu_create_placed_buffer", spudgpu_create_placed_buffer((spudgpu_heap){0}, (uint64_t){0}, (const spudgpu_buffer_desc *){0}, (spudgpu_buffer *){0}));
	calls++;
	expect_error("spudgpu_create_placed_image", spudgpu_create_placed_image((spudgpu_heap){0}, (uint64_t){0}, (const spudgpu_image_desc *){0}, (spudgpu_image *){0}));
	calls++;
	expect_error("spudgpu_create_sampler", spudgpu_create_sampler((spudgpu_device){0}, (const spudgpu_sampler_desc *){0}, (spudgpu_sampler *){0}));
	calls++;
	expect_error("spudgpu_create_semaphore", spudgpu_create_semaphore((spudgpu_device){0}, (spudgpu_semaphore *){0}));
	calls++;
	expect_error("spudgpu_create_shader_module", spudgpu_create_shader_module((spudgpu_device){0}, (const spudgpu_shader_module_desc *){0}, (spudgpu_shader_module *){0}));
	calls++;
	expect_error("spudgpu_create_shader_pipeline", spudgpu_create_shader_pipeline((spudgpu_device){0}, (const spudgpu_shader_pipeline_desc *){0}, (spudgpu_shader_pipeline *){0}));
	calls++;
	expect_error("spudgpu_create_surface", spudgpu_create_surface((spudgpu_instance){0}, (void *){0}, (void *){0}, (spudgpu_surface *){0}));
	calls++;
	(void)spudgpu_create_surface_from_callback((spudgpu_instance){0}, (void *){0}, (spudgpu_surface_create_fn){0});
	calls++;
	expect_error("spudgpu_create_swap_chain", spudgpu_create_swap_chain((spudgpu_device){0}, (const spudgpu_swap_chain_desc *){0}, (spudgpu_swap_chain *){0}));
	calls++;
	expect_error("spudgpu_create_timestamp_set", spudgpu_create_timestamp_set((spudgpu_device){0}, (spudgpu_command_queue){0}, (uint32_t){0}, (spudgpu_timestamp_set *){0}));
	calls++;
	spudgpu_destroy_acceleration_structure((spudgpu_acceleration_structure){0});
	unchecked++;
	calls++;
	spudgpu_destroy_buffer((spudgpu_buffer){0});
	unchecked++;
	calls++;
	spudgpu_destroy_buffer_view((spudgpu_buffer_view){0});
	unchecked++;
	calls++;
	spudgpu_destroy_command_allocator((spudgpu_command_allocator){0});
	unchecked++;
	calls++;
	spudgpu_destroy_command_list((spudgpu_command_list){0});
	unchecked++;
	calls++;
	spudgpu_destroy_compute_pipeline((spudgpu_compute_pipeline){0});
	unchecked++;
	calls++;
	spudgpu_destroy_descriptor_pool((spudgpu_descriptor_pool){0});
	unchecked++;
	calls++;
	spudgpu_destroy_descriptor_set_layout((spudgpu_descriptor_set_layout){0});
	unchecked++;
	calls++;
	spudgpu_destroy_device((spudgpu_device){0});
	unchecked++;
	calls++;
	spudgpu_destroy_fence((spudgpu_fence){0});
	unchecked++;
	calls++;
	spudgpu_destroy_heap((spudgpu_heap){0});
	unchecked++;
	calls++;
	spudgpu_destroy_image((spudgpu_image){0});
	unchecked++;
	calls++;
	spudgpu_destroy_image_view((spudgpu_image_view){0});
	unchecked++;
	calls++;
	(void)spudgpu_destroy_instance((spudgpu_instance){0}); // zeroes are valid: destroying NULL succeeds, as spudgpu.h says
	calls++;
	spudgpu_destroy_sampler((spudgpu_sampler){0});
	unchecked++;
	calls++;
	spudgpu_destroy_semaphore((spudgpu_semaphore){0});
	unchecked++;
	calls++;
	spudgpu_destroy_shader_module((spudgpu_shader_module){0});
	unchecked++;
	calls++;
	spudgpu_destroy_shader_pipeline((spudgpu_shader_pipeline){0});
	unchecked++;
	calls++;
	spudgpu_destroy_surface((spudgpu_surface){0});
	unchecked++;
	calls++;
	spudgpu_destroy_swap_chain((spudgpu_swap_chain){0});
	unchecked++;
	calls++;
	spudgpu_destroy_timestamp_set((spudgpu_timestamp_set){0});
	unchecked++;
	calls++;
	spudgpu_end_command_list((spudgpu_command_list){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_enumerate_adapters", spudgpu_enumerate_adapters((spudgpu_instance){0}, (const spudgpu_adapter **){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudgpu_flush_buffer", spudgpu_flush_buffer((spudgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}));
	calls++;
	(void)spudgpu_format_bit_count((SPUDGPU_FORMAT){0});
	calls++;
	(void)spudgpu_format_from_string((const char *){0});
	calls++;
	expect_error("spudgpu_get_acceleration_structure_build_sizes", spudgpu_get_acceleration_structure_build_sizes((spudgpu_device){0}, (const spudgpu_acceleration_structure_inputs *){0}, (spudgpu_acceleration_structure_sizes *){0}));
	calls++;
	expect_error("spudgpu_get_acceleration_structure_instance_layout", spudgpu_get_acceleration_structure_instance_layout((spudgpu_device){0}, (spudgpu_acceleration_structure_instance_layout *){0}));
	calls++;
	expect_error("spudgpu_get_acceleration_structure_reference", spudgpu_get_acceleration_structure_reference((spudgpu_acceleration_structure){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudgpu_get_adapter_properties", spudgpu_get_adapter_properties((spudgpu_adapter){0}, (SPUDGPU_ADAPTER_PROPERTIES *){0}));
	calls++;
#if defined(__linux__) || defined(_WIN32)
	(void)spudgpu_get_bindless_descriptor_set_layout((spudgpu_device){0});
	calls++;
#endif
	expect_error("spudgpu_get_buffer_desc", spudgpu_get_buffer_desc((spudgpu_buffer){0}, (spudgpu_buffer_desc *){0}));
	calls++;
	expect_error("spudgpu_get_buffer_gpu_address", spudgpu_get_buffer_gpu_address((spudgpu_buffer){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudgpu_get_buffer_placement_requirements", spudgpu_get_buffer_placement_requirements((spudgpu_heap){0}, (const spudgpu_buffer_desc *){0}, (spudgpu_placement_requirements *){0}));
	calls++;
	expect_error("spudgpu_get_buffer_view_desc", spudgpu_get_buffer_view_desc((spudgpu_buffer_view){0}, (spudgpu_buffer_view_desc *){0}));
	calls++;
	expect_error("spudgpu_get_command_queue", spudgpu_get_command_queue((spudgpu_device){0}, (SPUDGPU_COMMAND_LIST_TYPE){0}, (uint32_t){0}, (spudgpu_command_queue *){0}));
	calls++;
	expect_error("spudgpu_get_compute_pipeline_desc", spudgpu_get_compute_pipeline_desc((spudgpu_compute_pipeline){0}, (spudgpu_compute_pipeline_desc *){0}));
	calls++;
	(void)spudgpu_get_device_adapter((spudgpu_device){0});
	calls++;
	(void)spudgpu_get_device_features((spudgpu_device){0});
	calls++;
	(void)spudgpu_get_fence_value((spudgpu_fence){0});
	calls++;
	expect_error("spudgpu_get_heap_desc", spudgpu_get_heap_desc((spudgpu_heap){0}, (spudgpu_heap_desc *){0}));
	calls++;
	expect_error("spudgpu_get_image_buffer_copy_offset_alignment", spudgpu_get_image_buffer_copy_offset_alignment((spudgpu_image){0}, (uint64_t *){0}));
	calls++;
	spudgpu_get_image_buffer_copy_size((spudgpu_image){0}, (uint32_t){0}, (uint64_t *){0}, (uint64_t *){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_get_image_desc", spudgpu_get_image_desc((spudgpu_image){0}, (spudgpu_image_desc *){0}));
	calls++;
	expect_error("spudgpu_get_image_placement_requirements", spudgpu_get_image_placement_requirements((spudgpu_heap){0}, (const spudgpu_image_desc *){0}, (spudgpu_placement_requirements *){0}));
	calls++;
	expect_error("spudgpu_get_image_view_desc", spudgpu_get_image_view_desc((spudgpu_image_view){0}, (spudgpu_image_view_desc *){0}));
	calls++;
	(void)spudgpu_get_native_gpu_api((spudgpu_instance){0});
	calls++;
	expect_error("spudgpu_get_surface_present_scalings", spudgpu_get_surface_present_scalings((spudgpu_device){0}, (spudgpu_surface){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudgpu_get_swap_chain_desc", spudgpu_get_swap_chain_desc((spudgpu_swap_chain){0}, (spudgpu_swap_chain_desc *){0}));
	calls++;
	(void)spudgpu_get_swap_chain_image_view((spudgpu_swap_chain){0}, (uint32_t){0});
	calls++;
	expect_error("spudgpu_get_timestamps", spudgpu_get_timestamps((spudgpu_timestamp_set){0}, (uint32_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudgpu_invalidate_buffer", spudgpu_invalidate_buffer((spudgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}));
	calls++;
	expect_error("spudgpu_map_buffer", spudgpu_map_buffer((spudgpu_buffer){0}, (uint64_t){0}, (uint64_t){0}, (void **){0}));
	calls++;
	expect_error("spudgpu_queue_submit", spudgpu_queue_submit((spudgpu_command_queue){0}, (const spudgpu_submit_desc *){0}));
	calls++;
	spudgpu_queue_wait_idle((spudgpu_command_queue){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_reset_command_allocator", spudgpu_reset_command_allocator((spudgpu_command_allocator){0}));
	calls++;
	spudgpu_reset_descriptor_pool((spudgpu_descriptor_pool){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_signal_fence", spudgpu_signal_fence((spudgpu_device){0}, (spudgpu_fence){0}, (uint64_t){0}));
	calls++;
	expect_error("spudgpu_submit_command_lists", spudgpu_submit_command_lists((spudgpu_command_queue){0}, (spudgpu_command_list *){0}, (uint32_t){0}));
	calls++;
	expect_error("spudgpu_submit_command_lists_synced", spudgpu_submit_command_lists_synced((spudgpu_command_queue){0}, (spudgpu_command_list *){0}, (uint32_t){0}, (spudgpu_swap_chain){0}));
	calls++;
	(void)spudgpu_swap_chain_acquire_next_image((spudgpu_swap_chain){0});
	calls++;
	(void)spudgpu_swap_chain_get_image_available_semaphore((spudgpu_swap_chain){0});
	calls++;
	(void)spudgpu_swap_chain_get_render_finished_semaphore((spudgpu_swap_chain){0});
	calls++;
	spudgpu_swap_chain_present((spudgpu_swap_chain){0});
	unchecked++;
	calls++;
	spudgpu_unmap_buffer((spudgpu_buffer){0});
	unchecked++;
	calls++;
	spudgpu_update_descriptor_sets((spudgpu_device){0}, (const spudgpu_write_descriptor_set *){0}, (uint32_t){0});
	unchecked++;
	calls++;
	expect_error("spudgpu_wait_for_fences", spudgpu_wait_for_fences((spudgpu_device){0}, (spudgpu_fence *){0}, (const uint64_t *){0}, (uint32_t){0}, (bool){0}, (uint64_t){0}));
	calls++;
	expect_error("spudgpu_write_acceleration_structure_instances", spudgpu_write_acceleration_structure_instances((spudgpu_device){0}, (const spudgpu_acceleration_structure_instance *){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("spudinput_gamepad_get_axis_label", spudinput_gamepad_get_axis_label((spudinput_instance){0}, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_AXIS){0}, (char *){0}, (uint32_t){0}));
	calls++;
	expect_error("spudinput_gamepad_get_battery", spudinput_gamepad_get_battery((spudinput_instance){0}, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_BATTERY *){0}));
	calls++;
	expect_error("spudinput_gamepad_get_button_label", spudinput_gamepad_get_button_label((spudinput_instance){0}, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_BUTTONS){0}, (char *){0}, (uint32_t){0}));
	calls++;
	expect_error("spudinput_gamepad_get_motion", spudinput_gamepad_get_motion((spudinput_instance){0}, (spudinput_gamepad_id){0}, (const SPUDINPUT_GAMEPAD_MOTION_SAMPLE **){0}, (uint32_t *){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudinput_gamepad_get_properties", spudinput_gamepad_get_properties((spudinput_instance){0}, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_PROPERTIES *){0}));
	calls++;
	expect_error("spudinput_gamepad_get_state", spudinput_gamepad_get_state((spudinput_instance){0}, (spudinput_gamepad_id){0}, (SPUDINPUT_GAMEPAD_STATE *){0}));
	calls++;
	expect_error("spudinput_gamepad_get_touch", spudinput_gamepad_get_touch((spudinput_instance){0}, (spudinput_gamepad_id){0}, (uint32_t){0}, (SPUDINPUT_GAMEPAD_TOUCH_STATE *){0}));
	calls++;
	expect_error("spudinput_gamepad_set_light_color", spudinput_gamepad_set_light_color((spudinput_instance){0}, (spudinput_gamepad_id){0}, (float){0}, (float){0}, (float){0}));
	calls++;
	expect_error("spudinput_gamepad_set_motion_enabled", spudinput_gamepad_set_motion_enabled((spudinput_instance){0}, (spudinput_gamepad_id){0}, (bool){0}));
	calls++;
	expect_error("spudinput_gamepad_set_player_indicator", spudinput_gamepad_set_player_indicator((spudinput_instance){0}, (spudinput_gamepad_id){0}, (uint32_t){0}));
	calls++;
	expect_error("spudinput_gamepad_set_rumble", spudinput_gamepad_set_rumble((spudinput_instance){0}, (spudinput_gamepad_id){0}, (const SPUDINPUT_GAMEPAD_RUMBLE *){0}));
	calls++;
	expect_error("spudinput_instance_create", spudinput_instance_create((const SPUDINPUT_INSTANCE_DESC *){0}, (spudinput_instance *){0}));
	calls++;
	spudinput_instance_destroy((spudinput_instance){0});
	unchecked++;
	calls++;
	expect_error("spudinput_instance_get_gamepads", spudinput_instance_get_gamepads((spudinput_instance){0}, (const spudinput_gamepad_id **){0}, (uint32_t *){0}));
	calls++;
	(void)spudinput_instance_get_native_api((spudinput_instance){0});
	calls++;
	expect_error("spudinput_instance_poll", spudinput_instance_poll((spudinput_instance){0}));
	calls++;
	expect_error("spudmem_commit", spudmem_commit((void *){0}, (uint64_t){0}));
	calls++;
	expect_error("spudmem_decommit", spudmem_decommit((void *){0}, (uint64_t){0}));
	calls++;
	(void)spudmem_get_page_size();
	calls++;
	(void)spudmem_get_reserve_granularity();
	calls++;
	expect_error("spudmem_release", spudmem_release((void *){0}, (uint64_t){0}));
	calls++;
	expect_error("spudmem_reserve", spudmem_reserve((uint64_t){0}, (void **){0}));
	calls++;
	expect_error("spudnet_get_local_ipv4_addresses", spudnet_get_local_ipv4_addresses((spudnet_instance){0}, (char (*)[16]){0}, (uint32_t){0}, (uint32_t *){0}, (spudnet_error *){0}));
	calls++;
	expect_error("spudnet_http_client_create", spudnet_http_client_create((spudnet_instance){0}, (const spudnet_http_client_desc *){0}, (spudnet_http_client *){0}));
	calls++;
	spudnet_http_client_destroy((spudnet_http_client){0});
	unchecked++;
	calls++;
	spudnet_http_transfer_abort((spudnet_http_transfer){0});
	unchecked++;
	calls++;
	expect_error("spudnet_http_transfer_create", spudnet_http_transfer_create((spudnet_http_client){0}, (spudnet_http_transfer *){0}));
	calls++;
	spudnet_http_transfer_destroy((spudnet_http_transfer){0});
	unchecked++;
	calls++;
	spudnet_http_transfer_get_error((spudnet_http_transfer){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	expect_error("spudnet_http_transfer_get_header_at", spudnet_http_transfer_get_header_at((spudnet_http_transfer){0}, (uint32_t){0}, (const char **){0}, (const char **){0}));
	calls++;
	(void)spudnet_http_transfer_get_header_count((spudnet_http_transfer){0});
	calls++;
	(void)spudnet_http_transfer_get_status((spudnet_http_transfer){0});
	calls++;
	expect_error("spudnet_http_transfer_receive_response", spudnet_http_transfer_receive_response((spudnet_http_transfer){0}, (uint32_t){0}));
	calls++;
	expect_error("spudnet_http_transfer_recv", spudnet_http_transfer_recv((spudnet_http_transfer){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudnet_http_transfer_send", spudnet_http_transfer_send((spudnet_http_transfer){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudnet_http_transfer_start", spudnet_http_transfer_start((spudnet_http_transfer){0}, (const spudnet_http_transfer_desc *){0}));
	calls++;
	expect_error("spudnet_instance_create", spudnet_instance_create((const spudnet_instance_desc *){0}, (spudnet_instance *){0}, (spudnet_error *){0}));
	calls++;
	spudnet_instance_destroy((spudnet_instance){0});
	unchecked++;
	calls++;
	spudnet_resolver_abort((spudnet_resolver){0});
	unchecked++;
	calls++;
	expect_error("spudnet_resolver_create", spudnet_resolver_create((spudnet_instance){0}, (const spudnet_resolver_desc *){0}, (spudnet_resolver *){0}));
	calls++;
	spudnet_resolver_destroy((spudnet_resolver){0});
	unchecked++;
	calls++;
	spudnet_resolver_get_error((spudnet_resolver){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	expect_error("spudnet_resolver_run", spudnet_resolver_run((spudnet_resolver){0}));
	calls++;
	expect_error("spudnet_resolver_wait", spudnet_resolver_wait((spudnet_resolver){0}, (uint32_t){0}, (char (*)[46]){0}, (uint32_t){0}, (uint32_t *){0}));
	calls++;
	spudnet_tcp_listener_abort((spudnet_tcp_listener){0});
	unchecked++;
	calls++;
	expect_error("spudnet_tcp_listener_accept", spudnet_tcp_listener_accept((spudnet_tcp_listener){0}, (uint32_t){0}, (spudnet_tcp_socket *){0}));
	calls++;
	expect_error("spudnet_tcp_listener_create", spudnet_tcp_listener_create((spudnet_instance){0}, (const spudnet_tcp_listener_desc *){0}, (spudnet_tcp_listener *){0}, (spudnet_error *){0}));
	calls++;
	spudnet_tcp_listener_destroy((spudnet_tcp_listener){0});
	unchecked++;
	calls++;
	spudnet_tcp_listener_get_error((spudnet_tcp_listener){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	expect_error("spudnet_tcp_listener_get_port", spudnet_tcp_listener_get_port((spudnet_tcp_listener){0}, (uint16_t *){0}));
	calls++;
	spudnet_tcp_socket_abort((spudnet_tcp_socket){0});
	unchecked++;
	calls++;
	expect_error("spudnet_tcp_socket_connect", spudnet_tcp_socket_connect((spudnet_tcp_socket){0}, (const spudnet_tcp_socket_connect_desc *){0}));
	calls++;
	expect_error("spudnet_tcp_socket_create", spudnet_tcp_socket_create((spudnet_instance){0}, (spudnet_tcp_socket *){0}));
	calls++;
	spudnet_tcp_socket_destroy((spudnet_tcp_socket){0});
	unchecked++;
	calls++;
	expect_error("spudnet_tcp_socket_finish_sending", spudnet_tcp_socket_finish_sending((spudnet_tcp_socket){0}));
	calls++;
	spudnet_tcp_socket_get_error((spudnet_tcp_socket){0}, (SPUDNET_ERROR_SIDE){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	expect_error("spudnet_tcp_socket_get_peer_address", spudnet_tcp_socket_get_peer_address((spudnet_tcp_socket){0}, (char *){0}, (uint16_t *){0}));
	calls++;
	expect_error("spudnet_tcp_socket_recv", spudnet_tcp_socket_recv((spudnet_tcp_socket){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudnet_tcp_socket_send", spudnet_tcp_socket_send((spudnet_tcp_socket){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}));
	calls++;
	expect_error("spudnet_tcp_socket_set_no_delay", spudnet_tcp_socket_set_no_delay((spudnet_tcp_socket){0}, (bool){0}));
	calls++;
	spudnet_wait_set_abort((spudnet_wait_set){0});
	unchecked++;
	calls++;
	expect_error("spudnet_wait_set_add_tcp_listener", spudnet_wait_set_add_tcp_listener((spudnet_wait_set){0}, (spudnet_tcp_listener){0}, (void *){0}));
	calls++;
	expect_error("spudnet_wait_set_add_tcp_socket", spudnet_wait_set_add_tcp_socket((spudnet_wait_set){0}, (spudnet_tcp_socket){0}, (uint32_t){0}, (void *){0}));
	calls++;
	expect_error("spudnet_wait_set_add_websocket", spudnet_wait_set_add_websocket((spudnet_wait_set){0}, (spudnet_websocket){0}, (void *){0}));
	calls++;
	expect_error("spudnet_wait_set_create", spudnet_wait_set_create((spudnet_instance){0}, (spudnet_wait_set *){0}, (spudnet_error *){0}));
	calls++;
	spudnet_wait_set_destroy((spudnet_wait_set){0});
	unchecked++;
	calls++;
	spudnet_wait_set_get_error((spudnet_wait_set){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	spudnet_wait_set_remove_tcp_listener((spudnet_wait_set){0}, (spudnet_tcp_listener){0});
	unchecked++;
	calls++;
	spudnet_wait_set_remove_tcp_socket((spudnet_wait_set){0}, (spudnet_tcp_socket){0});
	unchecked++;
	calls++;
	spudnet_wait_set_remove_websocket((spudnet_wait_set){0}, (spudnet_websocket){0});
	unchecked++;
	calls++;
	expect_error("spudnet_wait_set_wait", spudnet_wait_set_wait((spudnet_wait_set){0}, (uint32_t){0}, (spudnet_wait_event *){0}, (uint32_t){0}, (uint32_t *){0}));
	calls++;
	spudnet_wait_set_wake((spudnet_wait_set){0});
	unchecked++;
	calls++;
	spudnet_websocket_abort((spudnet_websocket){0});
	unchecked++;
	calls++;
	expect_error("spudnet_websocket_close", spudnet_websocket_close((spudnet_websocket){0}, (uint16_t){0}, (const char *){0}));
	calls++;
	expect_error("spudnet_websocket_connect", spudnet_websocket_connect((spudnet_websocket){0}, (const spudnet_websocket_connect_desc *){0}, (uint32_t *){0}));
	calls++;
	expect_error("spudnet_websocket_create", spudnet_websocket_create((spudnet_instance){0}, (spudnet_websocket *){0}));
	calls++;
	spudnet_websocket_destroy((spudnet_websocket){0});
	unchecked++;
	calls++;
	(void)spudnet_websocket_get_close_code((spudnet_websocket){0});
	calls++;
	spudnet_websocket_get_error((spudnet_websocket){0}, (SPUDNET_ERROR_SIDE){0}, (spudnet_error *){0});
	unchecked++;
	calls++;
	expect_error("spudnet_websocket_recv", spudnet_websocket_recv((spudnet_websocket){0}, (void *){0}, (uint64_t){0}, (uint32_t){0}, (uint64_t *){0}, (SPUDNET_WEBSOCKET_MESSAGE *){0}, (bool *){0}));
	calls++;
	expect_error("spudnet_websocket_send", spudnet_websocket_send((spudnet_websocket){0}, (SPUDNET_WEBSOCKET_MESSAGE){0}, (const void *){0}, (uint64_t){0}, (uint32_t){0}));
	calls++;
	(void)spudperf_catch_current_clock_cycle();
	calls++;
	(void)spudperf_get_current_time_milliseconds();
	calls++;
	(void)spudperf_get_monotonic_time_ns();
	calls++;
	spudperf_get_ram_usage((uint64_t *){0}, (uint64_t *){0});
	unchecked++;
	calls++;
	spudperf_sleep_until_monotonic_ms((uint64_t){0});
	unchecked++;
	calls++;
	spudperf_sleep_until_monotonic_ns((uint64_t){0});
	unchecked++;
	calls++;
	(void)spudrandom_fill((void *){0}, (size_t){0}); // zeroes are valid: asking for no bytes succeeds, as spudrandom.h says
	calls++;
	printf("spud_misuse_tests: %d calls with zeroes, %d of them returning nothing to check, %d failure(s)\n", calls, unchecked, failures);
#if MISUSE_FREED
	int freed_calls = 0, freed_unchecked = 0, freed_skipped = 0;
	failures += freed_half(&freed_calls, &freed_unchecked, &freed_skipped);
	printf("spud_misuse_tests: %d calls with a destroyed handle, %d of them returning nothing to check, %d skipped for want of a handle, %d failure(s) in all\n", freed_calls, freed_unchecked, freed_skipped, failures);
#elif MISUSE_SANITIZED
	printf("spud_misuse_tests: the freed-handle half skipped: under a sanitizer, which reports the check's read of freed memory\n");
#else
	printf("spud_misuse_tests: the freed-handle half skipped: not a Debug build, which has no check\n");
#endif
	return failures ? 1 : 0;
}
