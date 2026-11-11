/*
 * crgpu_rq_measure — what CRGPU_EXT_RAY_QUERY costs on one adapter.
 * Not a ctest: it measures, it checks nothing but that every call
 * succeeds, and it needs a GPU with CRGPU_FEATURE_RAY_QUERY.
 *
 *   crgpu_rq_measure [--adapter N] [--dragon path/to/Dragon_LOD0.bin]
 *
 * - A bottom level's build and memory, before and after compaction: a
 *   12-triangle cube, and the Dragon mesh crgpusamples' CrGPUMeshShaders
 *   loads (its index and position buffer views), when its file is given.
 * - A top level of 1, 1,000 and 100,000 instances of the cube: its build,
 *   an update (refit) of it, and its instances written on the CPU
 *   (crgpu_device_write_acceleration_structure_instances() into a mapped
 *   buffer) and on the GPU (tests/shaders/rq_instances_row64.slang or
 *   _col72, the caller's writer for the device's layout).
 * - rq_trace.slang tracing one ray per pixel at 1920 x 1080 over the
 *   Dragon (the cube without the file).
 *
 * GPU work is timed on the CPU: a command list holding only the work,
 * submitted and waited for, RUNS times; the best run, less the best run of
 * an empty list (what submitting and waiting cost alone), is printed. Every
 * build into a structure already built is a rebuild of the same inputs.
 * Run it in Release.
 */

#include <crgpu.h>
#include <crperf.h>

#include "crgpu_test_shaders.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNS 20
#define TRACE_W 1920u
#define TRACE_H 1080u

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

typedef struct gpu {
	crgpu_device device;
	crgpu_command_queue queue;
	crgpu_command_allocator allocator;
	crgpu_command_list cmd;
	CRGPU_SHADER_FORMAT format;
	CRGPU_RAY_QUERY_LIMITS limits;
	crgpu_acceleration_structure_instance_layout layout;
	uint64_t empty_ns;
} gpu;

typedef void (*record_fn)(gpu *g, void *context);

/* The best of RUNS submissions of what [record] records, waited for, in
 * nanoseconds; less the empty list's when [less_empty]. */
static uint64_t time_gpu(gpu *g, record_fn record, void *context, bool less_empty) {
	uint64_t best = UINT64_MAX;
	for (int run = 0; run < RUNS; ++run) {
		CHECK(crgpu_command_allocator_reset(g->allocator) == CR_SUCCESS);
		crgpu_command_list_begin(g->cmd);
		if (record)
			record(g, context);
		crgpu_command_list_end(g->cmd);
		const uint64_t start = crperf_get_monotonic_time_ns();
		CHECK(crgpu_command_queue_submit_lists(g->queue, &g->cmd, 1) == CR_SUCCESS);
		crgpu_command_queue_wait_idle(g->queue);
		const uint64_t took = crperf_get_monotonic_time_ns() - start;
		if (took < best)
			best = took;
	}
	if (less_empty)
		return best > g->empty_ns ? best - g->empty_ns : 0;
	return best;
}

static double us(uint64_t ns) {
	return (double)ns / 1000.0;
}

static crgpu_buffer make_buffer(gpu *g, CRGPU_MEMORY_KIND kind, CRGPU_BUFFER_USAGE usage, uint64_t size, const void *contents) {
	crgpu_buffer_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type        = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	desc.usage            = usage;
	desc.memory_kind      = kind;
	desc.size             = size;
	crgpu_buffer buffer = NULL;
	CHECK(crgpu_buffer_create(g->device, &desc, &buffer) == CR_SUCCESS);
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

static void buffer_barrier(crgpu_command_list cmd, crgpu_buffer buffer, CRGPU_RESOURCE_STATE before, CRGPU_RESOURCE_STATE after) {
	crgpu_buffer_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type    = CR_STRUCT_TYPE_GPU_BUFFER_BARRIER;
	b.buffer       = buffer;
	b.state_before = before;
	b.state_after  = after;
	crgpu_command_list_pipeline_barrier(cmd, &b, 1, NULL, 0);
}

static void structure_barrier(crgpu_command_list cmd, crgpu_acceleration_structure structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS before,
                              CRGPU_ACCELERATION_STRUCTURE_ACCESS after) {
	crgpu_acceleration_structure_barrier b;
	memset(&b, 0, sizeof(b));
	b.base.type     = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER;
	b.structure     = structure;
	b.access_before = before;
	b.access_after  = after;
	crgpu_command_list_acceleration_structure_barrier(cmd, &b, 1);
}

static crgpu_acceleration_structure_sizes sizes_of(gpu *g, const crgpu_acceleration_structure_inputs *inputs) {
	crgpu_acceleration_structure_sizes sizes;
	memset(&sizes, 0, sizeof(sizes));
	sizes.base.type = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	CHECK(crgpu_device_get_acceleration_structure_build_sizes(g->device, inputs, &sizes) == CR_SUCCESS);
	return sizes;
}

static crgpu_acceleration_structure make_structure(gpu *g, CRGPU_ACCELERATION_STRUCTURE_LEVEL level, uint64_t size, uint32_t max_bottom_levels,
                                                     bool allow_compaction) {
	crgpu_acceleration_structure_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	desc.level                               = level;
	desc.size                                = size;
	desc.max_bottom_levels                   = max_bottom_levels;
	desc.allow_compaction                    = allow_compaction;
	crgpu_acceleration_structure structure = NULL;
	CHECK(crgpu_acceleration_structure_create(g->device, &desc, &structure) == CR_SUCCESS);
	return structure;
}

/* One build (or update) recorded with its scratch barrier and its
 * destination's barrier into BUILD_WRITE, its sources' into BUILD_READ. */
typedef struct build_job {
	crgpu_acceleration_structure_build build;
	const crgpu_acceleration_structure *reads;
	uint32_t read_count;
} build_job;

static void record_build(gpu *g, void *context) {
	build_job *job = (build_job *)context;
	buffer_barrier(g->cmd, job->build.scratch, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	for (uint32_t i = 0; i < job->read_count; ++i)
		structure_barrier(g->cmd, job->reads[i], CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
	structure_barrier(g->cmd, job->build.destination, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE,
	                  job->build.mode == CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE ? CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ
	                                                                                    : CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
	CHECK(crgpu_command_list_build_acceleration_structures(g->cmd, &job->build, 1) == CR_SUCCESS);
}

/* A bottom level of [inputs]: built RUNS times and timed, its compacted
 * size read back, a compacted copy made. Returns the built structure. */
static crgpu_acceleration_structure measure_bottom(gpu *g, const char *name, const crgpu_acceleration_structure_inputs *inputs, uint64_t triangles) {
	const crgpu_acceleration_structure_sizes sizes = sizes_of(g, inputs);
	crgpu_acceleration_structure structure         = make_structure(g, CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, sizes.storage_size, 0, true);
	crgpu_buffer scratch = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, sizes.build_scratch_size, NULL);
	if (!structure || !scratch) {
		crgpu_buffer_destroy(scratch);
		return structure;
	}
	build_job job;
	memset(&job, 0, sizeof(job));
	job.build.base.type   = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
	job.build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
	job.build.inputs      = inputs;
	job.build.destination = structure;
	job.build.scratch     = scratch;
	const uint64_t build_ns = time_gpu(g, record_build, &job, true);

	// The compacted size, read back.
	crgpu_buffer sizes_buffer = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE,
	                                          CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST | CRGPU_BUFFER_USAGE_TRANSFER_SRC, 8, NULL);
	crgpu_buffer readback = make_buffer(g, CRGPU_MEMORY_KIND_READBACK, CRGPU_BUFFER_USAGE_TRANSFER_DST, 8, NULL);
	uint64_t compacted      = 0;
	if (sizes_buffer && readback) {
		CHECK(crgpu_command_allocator_reset(g->allocator) == CR_SUCCESS);
		crgpu_command_list_begin(g->cmd);
		structure_barrier(g->cmd, structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		buffer_barrier(g->cmd, sizes_buffer, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST);
		CHECK(crgpu_command_list_write_acceleration_structure_compacted_sizes(g->cmd, &structure, 1, sizes_buffer, 0) == CR_SUCCESS);
		buffer_barrier(g->cmd, sizes_buffer, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST, CRGPU_RESOURCE_STATE_COPY_SOURCE);
		crgpu_command_list_copy_buffer(g->cmd, sizes_buffer, readback, 0, 0, 8);
		crgpu_command_list_end(g->cmd);
		CHECK(crgpu_command_queue_submit_lists(g->queue, &g->cmd, 1) == CR_SUCCESS);
		crgpu_command_queue_wait_idle(g->queue);
		void *mapped = NULL;
		if (crgpu_buffer_map(readback, 0, 8, &mapped) == CR_SUCCESS) {
			crgpu_buffer_invalidate(readback, 0, 8);
			memcpy(&compacted, mapped, 8);
			crgpu_buffer_unmap(readback);
		}
	}
	uint64_t compact_ns = 0;
	if (compacted > 0) {
		crgpu_acceleration_structure compact = make_structure(g, CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM, compacted, 0, false);
		if (compact) {
			CHECK(crgpu_command_allocator_reset(g->allocator) == CR_SUCCESS);
			crgpu_command_list_begin(g->cmd);
			structure_barrier(g->cmd, structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
			structure_barrier(g->cmd, compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
			CHECK(crgpu_command_list_copy_acceleration_structure(g->cmd, structure, compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) == CR_SUCCESS);
			crgpu_command_list_end(g->cmd);
			const uint64_t start = crperf_get_monotonic_time_ns();
			CHECK(crgpu_command_queue_submit_lists(g->queue, &g->cmd, 1) == CR_SUCCESS);
			crgpu_command_queue_wait_idle(g->queue);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			compact_ns          = took > g->empty_ns ? took - g->empty_ns : 0;
			crgpu_acceleration_structure_destroy(compact);
		}
	}
	printf("crgpu_rq_measure: bottom level, %s: %llu triangles; built in %.1f us (%.1f Mtri/s); %llu bytes, %llu compacted (%.0f%%), "
	       "scratch %llu; the compacting copy %.1f us\n",
	       name, (unsigned long long)triangles, us(build_ns), build_ns ? (double)triangles * 1000.0 / (double)build_ns : 0.0,
	       (unsigned long long)sizes.storage_size, (unsigned long long)compacted,
	       sizes.storage_size ? 100.0 * (double)compacted / (double)sizes.storage_size : 0.0, (unsigned long long)sizes.build_scratch_size,
	       us(compact_ns));
	crgpu_buffer_destroy(readback);
	crgpu_buffer_destroy(sizes_buffer);
	crgpu_buffer_destroy(scratch);
	return structure;
}

/* The GPU writer's run: its pipeline and set over the input and output. */
typedef struct writer_job {
	crgpu_compute_pipeline pipeline;
	crgpu_descriptor_set set;
	crgpu_buffer output;
	uint32_t count;
} writer_job;

static void record_writer(gpu *g, void *context) {
	writer_job *job = (writer_job *)context;
	buffer_barrier(g->cmd, job->output, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	crgpu_command_list_bind_compute_pipeline(g->cmd, job->pipeline);
	crgpu_command_list_bind_compute_descriptor_sets(g->cmd, job->pipeline, 0, &job->set, 1);
	const uint32_t push[2] = {job->count, 0};
	crgpu_command_list_push_compute_constants(g->cmd, job->pipeline, 0, sizeof(push), push);
	crgpu_command_list_dispatch(g->cmd, (job->count + 63) / 64, 1, 1);
}

/* A compute pipeline of [shader] over one set of [types]. */
static crgpu_compute_pipeline make_compute(gpu *g, const crgpu_test_shader *shader, const CRGPU_DESCRIPTOR_TYPE *types, uint32_t count,
                                             uint32_t push_bytes, crgpu_shader_module *out_module, crgpu_descriptor_set_layout *out_layout,
                                             crgpu_descriptor_pool *out_pool, crgpu_descriptor_set *out_set) {
	crgpu_shader_module_desc module_desc = crgpu_test_module_desc(shader, g->format, shader->entry_point);
	*out_module                            = NULL;
	if (module_desc.code)
		CHECK(crgpu_shader_module_create(g->device, &module_desc, out_module) == CR_SUCCESS);
	crgpu_descriptor_set_layout_desc layout_desc;
	memset(&layout_desc, 0, sizeof(layout_desc));
	layout_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	crgpu_descriptor_pool_desc pool_desc;
	memset(&pool_desc, 0, sizeof(pool_desc));
	pool_desc.base.type = CR_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets  = 1;
	for (uint32_t i = 0; i < count; ++i) {
		layout_desc.bindings[i].binding         = i;
		layout_desc.bindings[i].descriptor_type = types[i];
		layout_desc.bindings[i].count           = 1;
		layout_desc.bindings[i].stage_flags     = CRGPU_SHADER_STAGE_COMPUTE;
		pool_desc.pool_sizes[i].descriptor_type = types[i];
		pool_desc.pool_sizes[i].count           = 1;
	}
	layout_desc.binding_count = count;
	pool_desc.pool_size_count = count;
	*out_layout               = NULL;
	*out_pool                 = NULL;
	*out_set                  = NULL;
	CHECK(crgpu_descriptor_set_layout_create(g->device, &layout_desc, out_layout) == CR_SUCCESS);
	CHECK(crgpu_descriptor_pool_create(g->device, &pool_desc, out_pool) == CR_SUCCESS);
	if (*out_layout && *out_pool) {
		crgpu_descriptor_set_desc set_desc;
		memset(&set_desc, 0, sizeof(set_desc));
		set_desc.base.type      = CR_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
		set_desc.pool           = *out_pool;
		set_desc.set_layouts[0] = *out_layout;
		set_desc.set_count      = 1;
		CHECK(crgpu_descriptor_set_create(g->device, &set_desc, out_set) == CR_SUCCESS);
	}
	crgpu_compute_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.base.type                           = CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	desc.compute_module                      = *out_module;
	desc.descriptor_set_layouts[0]           = *out_layout;
	desc.descriptor_set_layout_count         = 1;
	desc.push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_COMPUTE;
	desc.push_constant_ranges[0].size        = push_bytes;
	desc.push_constant_range_count           = 1;
	crgpu_compute_pipeline pipeline        = NULL;
	if (*out_module && *out_layout)
		CHECK(crgpu_compute_pipeline_create(g->device, &desc, &pipeline) == CR_SUCCESS);
	return pipeline;
}

static void write_buffers(gpu *g, crgpu_descriptor_set set, const crgpu_buffer *buffers, const uint64_t *sizes, uint32_t first, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i) {
		const crgpu_descriptor_buffer_info info = {buffers[i], 0, sizes[i]};
		crgpu_write_descriptor_set write;
		memset(&write, 0, sizeof(write));
		write.base.type        = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		write.dst_set          = set;
		write.dst_binding      = first + i;
		write.descriptor_count = 1;
		write.descriptor_type  = CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.buffer_info      = &info;
		crgpu_device_update_descriptor_sets(g->device, &write, 1);
	}
}

/* A top level of [count] instances of [bottom]: its build and refit, and
 * its instances written by the CPU call and by the GPU writer. */
static void measure_top(gpu *g, crgpu_acceleration_structure bottom, uint32_t count) {
	uint64_t reference = 0;
	CHECK(crgpu_acceleration_structure_get_reference(bottom, &reference) == CR_SUCCESS);
	crgpu_acceleration_structure_instance *instances =
	    (crgpu_acceleration_structure_instance *)calloc(count, sizeof(crgpu_acceleration_structure_instance));
	uint32_t *words = (uint32_t *)calloc((size_t)count * 20u, sizeof(uint32_t));
	if (!instances || !words) {
		free(instances);
		free(words);
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		crgpu_acceleration_structure_instance *in = &instances[i];
		in->transform[0]                            = 1.0f;
		in->transform[3]                            = (float)(i % 1000u) * 2.0f;
		in->transform[5]                            = 1.0f;
		in->transform[7]                            = (float)(i / 1000u) * 2.0f;
		in->transform[10]                           = 1.0f;
		in->user_index                              = i & 0xFFFFFFu;
		in->mask                                    = 0xFF;
		in->bottom_level_reference                  = reference;
		memcpy(&words[i * 20u], in->transform, 48);
		words[i * 20u + 12] = in->user_index;
		words[i * 20u + 13] = in->mask;
		words[i * 20u + 16] = (uint32_t)reference;
		words[i * 20u + 17] = (uint32_t)(reference >> 32);
	}
	const uint64_t bytes = (uint64_t)g->layout.size * count;
	crgpu_buffer upload =
	    make_buffer(g, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, bytes, NULL);
	// The CPU call, into the mapped buffer.
	uint64_t cpu_best = UINT64_MAX;
	void *mapped      = NULL;
	if (upload && crgpu_buffer_map(upload, 0, bytes, &mapped) == CR_SUCCESS) {
		for (int run = 0; run < RUNS; ++run) {
			const uint64_t start = crperf_get_monotonic_time_ns();
			CHECK(crgpu_device_write_acceleration_structure_instances(g->device, instances, count, mapped) == CR_SUCCESS);
			const uint64_t took = crperf_get_monotonic_time_ns() - start;
			if (took < cpu_best)
				cpu_best = took;
		}
		CHECK(crgpu_buffer_flush(upload, 0, bytes) == CR_SUCCESS);
		crgpu_buffer_unmap(upload);
	}

	// The GPU writer, into a device buffer from a device copy of its input.
	const crgpu_test_shader *shader = g->layout.kind == CRGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72 ? &crgpu_test_rq_instances_col72
	                                                                                                                    : &crgpu_test_rq_instances_row64;
	const CRGPU_DESCRIPTOR_TYPE types[2] = {CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	crgpu_shader_module module           = NULL;
	crgpu_descriptor_set_layout layout   = NULL;
	crgpu_descriptor_pool pool           = NULL;
	writer_job writer;
	memset(&writer, 0, sizeof(writer));
	writer.count    = count;
	writer.pipeline = make_compute(g, shader, types, 2, 8, &module, &layout, &pool, &writer.set);
	const uint64_t in_bytes = (uint64_t)count * 80u;
	crgpu_buffer input =
	    make_buffer(g, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST, in_bytes, NULL);
	crgpu_buffer staging = make_buffer(g, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_TRANSFER_SRC, in_bytes, words);
	writer.output          = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE,
	                                     CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, bytes, NULL);
	uint64_t gpu_ns = 0;
	if (writer.pipeline && writer.set && input && staging && writer.output) {
		const crgpu_buffer buffers[2] = {input, writer.output};
		const uint64_t sizes[2]         = {in_bytes, bytes};
		write_buffers(g, writer.set, buffers, sizes, 0, 2);
		CHECK(crgpu_command_allocator_reset(g->allocator) == CR_SUCCESS);
		crgpu_command_list_begin(g->cmd);
		buffer_barrier(g->cmd, input, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_COPY_DEST);
		crgpu_command_list_copy_buffer(g->cmd, staging, input, 0, 0, in_bytes);
		crgpu_command_list_end(g->cmd);
		CHECK(crgpu_command_queue_submit_lists(g->queue, &g->cmd, 1) == CR_SUCCESS);
		crgpu_command_queue_wait_idle(g->queue);
		gpu_ns = time_gpu(g, record_writer, &writer, true);
	}

	// The top level, from the CPU-written instances: built, then refitted.
	const crgpu_acceleration_structure bottoms[1] = {bottom};
	crgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type                                = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level                                    = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	inputs.flags                                    = CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE | CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE;
	inputs.instances                                = upload;
	inputs.instance_count                           = count;
	inputs.bottom_levels                            = bottoms;
	inputs.bottom_level_count                       = 1;
	const crgpu_acceleration_structure_sizes size = sizes_of(g, &inputs);
	crgpu_acceleration_structure top              = make_structure(g, CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, size.storage_size, 1, false);
	uint64_t scratch_size = size.build_scratch_size > size.update_scratch_size ? size.build_scratch_size : size.update_scratch_size;
	crgpu_buffer scratch = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, scratch_size, NULL);
	uint64_t build_ns = 0, refit_ns = 0;
	if (top && scratch && upload) {
		build_job job;
		memset(&job, 0, sizeof(job));
		job.build.base.type   = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		job.build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		job.build.inputs      = &inputs;
		job.build.destination = top;
		job.build.scratch     = scratch;
		job.reads             = bottoms;
		job.read_count        = 1;
		build_ns              = time_gpu(g, record_build, &job, true);
		job.build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE;
		job.build.source      = top;
		refit_ns              = time_gpu(g, record_build, &job, true);
	}
	printf("crgpu_rq_measure: top level, %u instance(s): built in %.1f us, refitted in %.1f us; %llu bytes; instances written on the CPU in "
	       "%.1f us (%.1f ns each), on the GPU in %.1f us\n",
	       count, us(build_ns), us(refit_ns), (unsigned long long)size.storage_size, us(cpu_best), (double)cpu_best / (double)count, us(gpu_ns));
	crgpu_acceleration_structure_destroy(top);
	crgpu_buffer_destroy(scratch);
	crgpu_buffer_destroy(writer.output);
	crgpu_buffer_destroy(staging);
	crgpu_buffer_destroy(input);
	crgpu_buffer_destroy(upload);
	crgpu_descriptor_pool_destroy(pool);
	crgpu_compute_pipeline_destroy(writer.pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
	free(words);
	free(instances);
}

/* rq_trace.slang's parameters. */
typedef struct trace_params {
	float origin[4];
	float step_and_tmax[4];
	uint32_t width, height, cull_mask, ray_flags;
} trace_params;

typedef struct trace_job {
	crgpu_compute_pipeline pipeline;
	crgpu_descriptor_set set;
	crgpu_buffer hits;
	crgpu_acceleration_structure top;
	trace_params params;
} trace_job;

static void record_trace(gpu *g, void *context) {
	trace_job *job = (trace_job *)context;
	structure_barrier(g->cmd, job->top, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	buffer_barrier(g->cmd, job->hits, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	crgpu_command_list_bind_compute_pipeline(g->cmd, job->pipeline);
	crgpu_command_list_bind_compute_descriptor_sets(g->cmd, job->pipeline, 0, &job->set, 1);
	crgpu_command_list_push_compute_constants(g->cmd, job->pipeline, 0, sizeof(job->params), &job->params);
	crgpu_command_list_dispatch(g->cmd, (TRACE_W + 7) / 8, (TRACE_H + 7) / 8, 1);
}

/* One ray per pixel at 1920 x 1080 over a top level of one instance of
 * [bottom], whose bounds are [lo, hi]: the grid covers its x and y, the
 * rays start below its lowest z and go up. */
static void measure_trace(gpu *g, crgpu_acceleration_structure bottom, const char *name, const float lo[3], const float hi[3]) {
	uint64_t reference = 0;
	CHECK(crgpu_acceleration_structure_get_reference(bottom, &reference) == CR_SUCCESS);
	crgpu_acceleration_structure_instance instance;
	memset(&instance, 0, sizeof(instance));
	instance.transform[0]           = 1.0f;
	instance.transform[5]           = 1.0f;
	instance.transform[10]          = 1.0f;
	instance.mask                   = 0xFF;
	instance.bottom_level_reference = reference;
	uint8_t bytes[72];
	CHECK(crgpu_device_write_acceleration_structure_instances(g->device, &instance, 1, bytes) == CR_SUCCESS);
	crgpu_buffer instances = make_buffer(g, CRGPU_MEMORY_KIND_UPLOAD, CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, g->layout.size, bytes);
	const crgpu_acceleration_structure bottoms[1] = {bottom};
	crgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type                                = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level                                    = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	inputs.flags                                    = CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE;
	inputs.instances                                = instances;
	inputs.instance_count                           = 1;
	inputs.bottom_levels                            = bottoms;
	inputs.bottom_level_count                       = 1;
	const crgpu_acceleration_structure_sizes size = sizes_of(g, &inputs);
	trace_job job;
	memset(&job, 0, sizeof(job));
	job.top                = make_structure(g, CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP, size.storage_size, 1, false);
	crgpu_buffer scratch = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, size.build_scratch_size, NULL);
	job.hits               = make_buffer(g, CRGPU_MEMORY_KIND_DEVICE, CRGPU_BUFFER_USAGE_STORAGE, 32ull * TRACE_W * TRACE_H, NULL);
	const CRGPU_DESCRIPTOR_TYPE types[2] = {CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	crgpu_shader_module module           = NULL;
	crgpu_descriptor_set_layout layout   = NULL;
	crgpu_descriptor_pool pool           = NULL;
	job.pipeline = make_compute(g, &crgpu_test_rq_trace, types, 2, sizeof(trace_params), &module, &layout, &pool, &job.set);
	if (job.top && scratch && job.hits && instances && job.pipeline && job.set) {
		build_job build;
		memset(&build, 0, sizeof(build));
		build.build.base.type   = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.build.mode        = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.build.inputs      = &inputs;
		build.build.destination = job.top;
		build.build.scratch     = scratch;
		build.reads             = bottoms;
		build.read_count        = 1;
		CHECK(crgpu_command_allocator_reset(g->allocator) == CR_SUCCESS);
		crgpu_command_list_begin(g->cmd);
		record_build(g, &build);
		crgpu_command_list_end(g->cmd);
		CHECK(crgpu_command_queue_submit_lists(g->queue, &g->cmd, 1) == CR_SUCCESS);
		crgpu_command_queue_wait_idle(g->queue);
		crgpu_write_descriptor_set write;
		memset(&write, 0, sizeof(write));
		write.base.type               = CR_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
		write.dst_set                 = job.set;
		write.descriptor_count        = 1;
		write.descriptor_type         = CRGPU_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE;
		write.acceleration_structures = &job.top;
		crgpu_device_update_descriptor_sets(g->device, &write, 1);
		const crgpu_buffer hits[1] = {job.hits};
		const uint64_t hit_bytes[1]  = {32ull * TRACE_W * TRACE_H};
		write_buffers(g, job.set, hits, hit_bytes, 1, 1);
		job.params.origin[0]        = lo[0];
		job.params.origin[1]        = lo[1];
		job.params.origin[2]        = lo[2] - 1.0f;
		job.params.step_and_tmax[0] = (hi[0] - lo[0]) / (float)TRACE_W;
		job.params.step_and_tmax[1] = (hi[1] - lo[1]) / (float)TRACE_H;
		job.params.step_and_tmax[2] = (hi[2] - lo[2]) + 2.0f;
		job.params.width            = TRACE_W;
		job.params.height           = TRACE_H;
		job.params.cull_mask        = 0xFF;
		const uint64_t ns           = time_gpu(g, record_trace, &job, true);
		printf("crgpu_rq_measure: rq_trace, one ray per pixel at %u x %u over the %s: %.1f us (%.0f Mrays/s)\n", TRACE_W, TRACE_H, name, us(ns),
		       ns ? (double)TRACE_W * TRACE_H * 1000.0 / (double)ns : 0.0);
	}
	crgpu_descriptor_pool_destroy(pool);
	crgpu_compute_pipeline_destroy(job.pipeline);
	crgpu_descriptor_set_layout_destroy(layout);
	crgpu_shader_module_destroy(module);
	crgpu_buffer_destroy(job.hits);
	crgpu_buffer_destroy(scratch);
	crgpu_acceleration_structure_destroy(job.top);
	crgpu_buffer_destroy(instances);
}

// ---- Dragon_LOD0.bin, as CrGPUMeshShaders reads it -------------------------

typedef struct dragon_header {
	uint32_t prolog, version, mesh_count, accessor_count, buffer_view_count, buffer_size;
} dragon_header;
typedef struct dragon_mesh {
	uint32_t indices, index_subsets, attributes[5], meshlets, meshlet_subsets, unique_vertex_indices, primitive_indices, cull_data;
} dragon_mesh;
typedef struct dragon_accessor {
	uint32_t buffer_view, offset, size, stride, count;
} dragon_accessor;
typedef struct dragon_view {
	uint32_t offset, size;
} dragon_view;

static uint8_t *read_file(const char *path, size_t *out_size) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = size > 0 ? (uint8_t *)malloc((size_t)size) : NULL;
	if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*out_size = (size_t)size;
	return data;
}

int main(int argc, char **argv) {
	uint32_t adapter_index = 0;
	const char *dragon     = NULL;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--adapter") == 0 && i + 1 < argc) {
			adapter_index = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--dragon") == 0 && i + 1 < argc) {
			dragon = argv[++i];
		} else {
			fprintf(stderr, "usage: crgpu_rq_measure [--adapter N] [--dragon Dragon_LOD0.bin]\n");
			return 2;
		}
	}
	CR_ALLOCATOR allocator = cr_allocator_system();
	crgpu_instance_desc instance_desc;
	memset(&instance_desc, 0, sizeof(instance_desc));
	instance_desc.base.type        = CR_STRUCT_TYPE_GPU_INSTANCE_DESC;
	instance_desc.allocator        = &allocator;
	instance_desc.application_name = "crgpu_rq_measure";
	instance_desc.engine_name      = "crgpu_rq_measure";
	crgpu_instance instance      = NULL;
	if (crgpu_instance_create(&instance_desc, &instance) != CR_SUCCESS)
		return 1;
	const crgpu_adapter *adapters = NULL;
	uint32_t adapter_count          = 0;
	crgpu_adapter_enumerate(instance, &adapters, &adapter_count);
	if (adapter_index >= adapter_count) {
		fprintf(stderr, "crgpu_rq_measure: no adapter %u\n", adapter_index);
		crgpu_instance_destroy(instance);
		return 2;
	}
	gpu g;
	memset(&g, 0, sizeof(g));
	g.limits.base.type = CR_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS;
	CRGPU_ADAPTER_PROPERTIES properties;
	memset(&properties, 0, sizeof(properties));
	properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	properties.base.next = &g.limits.base;
	CHECK(crgpu_adapter_get_properties(adapters[adapter_index], &properties) == CR_SUCCESS);
	printf("crgpu_rq_measure: adapter %u: %s\n", adapter_index, properties.description);
	if (!(properties.supported_features & CRGPU_FEATURE_RAY_QUERY)) {
		printf("crgpu_rq_measure: the adapter has no CRGPU_FEATURE_RAY_QUERY\n");
		crgpu_instance_destroy(instance);
		return 1;
	}
	for (CRGPU_SHADER_FORMAT f = CRGPU_SHADER_FORMAT_SPIRV; f <= CRGPU_SHADER_FORMAT_METALLIB; ++f)
		if (properties.shader_formats & (1u << f))
			g.format = f;
	crgpu_device_desc device_desc;
	memset(&device_desc, 0, sizeof(device_desc));
	device_desc.base.type          = CR_STRUCT_TYPE_GPU_DEVICE_DESC;
	device_desc.required_features  = CRGPU_FEATURE_RAY_QUERY;
	device_desc.direct_queue_count = 1;
	device_desc.max_descriptors    = 64;
	device_desc.max_samplers       = 4;
	CHECK(crgpu_device_create(adapters[adapter_index], &device_desc, &g.device) == CR_SUCCESS);
	if (!g.device) {
		crgpu_instance_destroy(instance);
		return 1;
	}
	CHECK(crgpu_command_queue_get(g.device, CRGPU_COMMAND_LIST_TYPE_DIRECT, 0, &g.queue) == CR_SUCCESS);
	crgpu_command_allocator_desc allocator_desc;
	memset(&allocator_desc, 0, sizeof(allocator_desc));
	allocator_desc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
	allocator_desc.type      = CRGPU_COMMAND_LIST_TYPE_DIRECT;
	CHECK(crgpu_command_allocator_create(g.device, &allocator_desc, &g.allocator) == CR_SUCCESS);
	if (g.allocator)
		CHECK(crgpu_command_list_create(g.allocator, &g.cmd) == CR_SUCCESS);
	CHECK(crgpu_device_get_acceleration_structure_instance_layout(g.device, &g.layout) == CR_SUCCESS);
	if (!g.cmd) {
		crgpu_device_destroy(g.device);
		crgpu_instance_destroy(instance);
		return 1;
	}
	g.empty_ns = time_gpu(&g, NULL, NULL, false);
	printf("crgpu_rq_measure: an empty list submitted and waited for: %.1f us (taken off every time below)\n", us(g.empty_ns));

	// The cube: 8 corners, 12 triangles.
	const float corners[24]    = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1};
	const uint32_t indices[36] = {0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7, 0, 4, 1, 1, 4, 5, 2, 3, 6, 6, 3, 7, 0, 2, 4, 4, 2, 6, 1, 5, 3, 3, 5, 7};
	const CRGPU_BUFFER_USAGE input = CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT;
	crgpu_buffer cube_vertices     = make_buffer(&g, CRGPU_MEMORY_KIND_UPLOAD, input, sizeof(corners), corners);
	crgpu_buffer cube_indices      = make_buffer(&g, CRGPU_MEMORY_KIND_UPLOAD, input, sizeof(indices), indices);
	crgpu_acceleration_structure_geometry geometry;
	memset(&geometry, 0, sizeof(geometry));
	geometry.kind         = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
	geometry.flags        = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
	geometry.data         = cube_vertices;
	geometry.data_stride  = 12;
	geometry.data_count   = 8;
	geometry.index_format = CRGPU_FORMAT_R32_UINT;
	geometry.indices      = cube_indices;
	geometry.index_count  = 36;
	crgpu_acceleration_structure_inputs inputs;
	memset(&inputs, 0, sizeof(inputs));
	inputs.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level          = CRGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs.flags          = CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | CRGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION;
	inputs.geometries     = &geometry;
	inputs.geometry_count = 1;
	crgpu_acceleration_structure cube = (cube_vertices && cube_indices) ? measure_bottom(&g, "a cube", &inputs, 12) : NULL;

	// The Dragon, its positions (the interleaved position and normal view)
	// and its indices.
	crgpu_acceleration_structure dragon_bottom = NULL;
	crgpu_buffer dragon_vertices = NULL, dragon_indices = NULL;
	float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
	if (dragon) {
		size_t size   = 0;
		uint8_t *data = read_file(dragon, &size);
		const dragon_header *header = (const dragon_header *)data;
		if (!data || size < sizeof(dragon_header) || header->prolog != 0x4D53484Cu || header->mesh_count == 0) {
			fprintf(stderr, "crgpu_rq_measure: %s is not a meshlet file\n", dragon);
			++failures;
		} else {
			const dragon_mesh *mesh          = (const dragon_mesh *)(data + sizeof(dragon_header));
			const dragon_accessor *accessors = (const dragon_accessor *)(mesh + header->mesh_count);
			const dragon_view *views         = (const dragon_view *)(accessors + header->accessor_count);
			const uint8_t *blob              = (const uint8_t *)(views + header->buffer_view_count);
			const dragon_accessor *position  = &accessors[mesh->attributes[0]];
			const dragon_accessor *index     = &accessors[mesh->indices];
			const dragon_view *vertex_view   = &views[position->buffer_view];
			const dragon_view *index_view    = &views[index->buffer_view];
			const uint32_t stride            = position->stride ? position->stride : 12;
			for (uint32_t v = 0; v < position->count; ++v) {
				float p[3];
				memcpy(p, blob + vertex_view->offset + position->offset + (size_t)v * stride, 12);
				for (int a = 0; a < 3; ++a) {
					if (v == 0 || p[a] < lo[a])
						lo[a] = p[a];
					if (v == 0 || p[a] > hi[a])
						hi[a] = p[a];
				}
			}
			dragon_vertices = make_buffer(&g, CRGPU_MEMORY_KIND_UPLOAD, input, vertex_view->size, blob + vertex_view->offset);
			dragon_indices  = make_buffer(&g, CRGPU_MEMORY_KIND_UPLOAD, input, index_view->size, blob + index_view->offset);
			memset(&geometry, 0, sizeof(geometry));
			geometry.kind         = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
			geometry.flags        = CRGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
			geometry.data         = dragon_vertices;
			geometry.data_offset  = position->offset;
			geometry.data_stride  = stride;
			geometry.data_count   = position->count;
			geometry.index_format = index->size == 2 ? CRGPU_FORMAT_R16_UINT : CRGPU_FORMAT_R32_UINT;
			geometry.indices      = dragon_indices;
			geometry.index_offset = index->offset;
			geometry.index_count  = index->count;
			printf("crgpu_rq_measure: the Dragon: %u vertices (stride %u), %u indices of %u bytes\n", position->count, stride, index->count,
			       index->size);
			if (dragon_vertices && dragon_indices)
				dragon_bottom = measure_bottom(&g, "the Dragon", &inputs, index->count / 3);
		}
		free(data);
	}

	if (cube) {
		const uint32_t counts[3] = {1, 1000, 100000};
		for (int i = 0; i < 3; ++i)
			measure_top(&g, cube, counts[i]);
	}
	if (dragon_bottom)
		measure_trace(&g, dragon_bottom, "Dragon", lo, hi);
	else if (cube)
		measure_trace(&g, cube, "cube", lo, hi);

	crgpu_acceleration_structure_destroy(dragon_bottom);
	crgpu_acceleration_structure_destroy(cube);
	crgpu_buffer_destroy(dragon_indices);
	crgpu_buffer_destroy(dragon_vertices);
	crgpu_buffer_destroy(cube_indices);
	crgpu_buffer_destroy(cube_vertices);
	crgpu_command_list_destroy(g.cmd);
	crgpu_command_allocator_destroy(g.allocator);
	crgpu_device_destroy(g.device);
	crgpu_instance_destroy(instance);
	if (failures) {
		fprintf(stderr, "crgpu_rq_measure: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
