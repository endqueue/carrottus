// The tests' shaders, as cmake/spud_slang.cmake builds them from
// tests/shaders/*.slang: SPIR-V on every platform, DXIL where the build
// made it (SPUDLIB_TEST_SHADERS_DXIL), a .metallib where it made one
// (SPUDLIB_TEST_SHADERS_METALLIB). A test hands a module the format its
// adapter reports (SPUDGPU_ADAPTER_PROPERTIES::shader_formats).

#ifndef SPUDGPU_TEST_SHADERS_H
#define SPUDGPU_TEST_SHADERS_H

#include "spudgpu.h"
#include <stdint.h>

#include "spudgpu_test_objects_draw_vert.spv.inc"
#include "spudgpu_test_objects_draw_frag.spv.inc"
#include "spudgpu_test_objects_draw_mesh.spv.inc"
#include "spudgpu_test_objects_double_comp.spv.inc"
#include "spudgpu_test_rq_trace.spv.inc"
#include "spudgpu_test_rq_shadow.spv.inc"
#include "spudgpu_test_rq_shadow_vert.spv.inc"
#include "spudgpu_test_rq_instances_row64.spv.inc"
#include "spudgpu_test_rq_instances_col72.spv.inc"
#if SPUDLIB_TEST_SHADERS_DXIL
#include "spudgpu_test_objects_draw_vert.dxil.inc"
#include "spudgpu_test_objects_draw_frag.dxil.inc"
#include "spudgpu_test_objects_draw_mesh.dxil.inc"
#include "spudgpu_test_objects_double_comp.dxil.inc"
#include "spudgpu_test_rq_trace.dxil.inc"
#include "spudgpu_test_rq_shadow.dxil.inc"
#include "spudgpu_test_rq_shadow_vert.dxil.inc"
#include "spudgpu_test_rq_instances_row64.dxil.inc"
#include "spudgpu_test_rq_instances_col72.dxil.inc"
#define SPUDGPU_TEST_DXIL(name) name##_dxil, name##_dxil_size
#else
#define SPUDGPU_TEST_DXIL(name) NULL, 0
#endif
#if SPUDLIB_TEST_SHADERS_METALLIB
#include "spudgpu_test_objects_draw_vert.metallib.inc"
#include "spudgpu_test_objects_draw_frag.metallib.inc"
#include "spudgpu_test_objects_draw_mesh.metallib.inc"
#include "spudgpu_test_objects_double_comp.metallib.inc"
#include "spudgpu_test_rq_trace.metallib.inc"
#include "spudgpu_test_rq_shadow.metallib.inc"
#include "spudgpu_test_rq_shadow_vert.metallib.inc"
#include "spudgpu_test_rq_instances_row64.metallib.inc"
#include "spudgpu_test_rq_instances_col72.metallib.inc"
#define SPUDGPU_TEST_METALLIB(name) name##_metallib, name##_metallib_size
#else
#define SPUDGPU_TEST_METALLIB(name) NULL, 0
#endif

// One shader: its entry point (the source's stem), stage and workgroup
// size, and its code in each format the build made.
typedef struct spudgpu_test_shader {
	const char *entry_point;
	SPUDGPU_SHADER_STAGE stage;
	uint32_t workgroup_size[3];
	const uint32_t *spirv;
	uint64_t spirv_size;
	const uint32_t *dxil;
	uint64_t dxil_size;
	const uint32_t *metallib;
	uint64_t metallib_size;
} spudgpu_test_shader;

#define SPUDGPU_TEST_SHADER(name, entry, stage, x, y, z) \
	{entry, stage, {x, y, z}, name##_spirv, name##_spirv_size, SPUDGPU_TEST_DXIL(name), SPUDGPU_TEST_METALLIB(name)}

static const spudgpu_test_shader spudgpu_test_objects_draw_vert =
    SPUDGPU_TEST_SHADER(spudgpu_test_objects_draw_vert, "objects_draw_vert", SPUDGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
static const spudgpu_test_shader spudgpu_test_objects_draw_frag =
    SPUDGPU_TEST_SHADER(spudgpu_test_objects_draw_frag, "objects_draw_frag", SPUDGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const spudgpu_test_shader spudgpu_test_objects_draw_mesh =
    SPUDGPU_TEST_SHADER(spudgpu_test_objects_draw_mesh, "objects_draw_mesh", SPUDGPU_SHADER_STAGE_MESH, 1, 1, 1);
static const spudgpu_test_shader spudgpu_test_objects_double_comp =
    SPUDGPU_TEST_SHADER(spudgpu_test_objects_double_comp, "objects_double_comp", SPUDGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);
static const spudgpu_test_shader spudgpu_test_rq_trace =
    SPUDGPU_TEST_SHADER(spudgpu_test_rq_trace, "rq_trace", SPUDGPU_SHADER_STAGE_COMPUTE, 8, 8, 1);
static const spudgpu_test_shader spudgpu_test_rq_shadow =
    SPUDGPU_TEST_SHADER(spudgpu_test_rq_shadow, "rq_shadow", SPUDGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const spudgpu_test_shader spudgpu_test_rq_shadow_vert =
    SPUDGPU_TEST_SHADER(spudgpu_test_rq_shadow_vert, "rq_shadow_vert", SPUDGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
static const spudgpu_test_shader spudgpu_test_rq_instances_row64 =
    SPUDGPU_TEST_SHADER(spudgpu_test_rq_instances_row64, "rq_instances_row64", SPUDGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);
static const spudgpu_test_shader spudgpu_test_rq_instances_col72 =
    SPUDGPU_TEST_SHADER(spudgpu_test_rq_instances_col72, "rq_instances_col72", SPUDGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);

// [shader]'s module desc in [format] (one of the adapter's shader_formats),
// with [debug_name]. code is NULL when the build made no code in [format].
static inline spudgpu_shader_module_desc spudgpu_test_module_desc(
    const spudgpu_test_shader *shader,
    SPUDGPU_SHADER_FORMAT format,
    const char *debug_name) {
	spudgpu_shader_module_desc desc = {0};
	desc.base.type                  = SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC;
	desc.stage                      = shader->stage;
	desc.format                     = format;
	desc.entry_point                = shader->entry_point;
	desc.workgroup_size[0]          = shader->workgroup_size[0];
	desc.workgroup_size[1]          = shader->workgroup_size[1];
	desc.workgroup_size[2]          = shader->workgroup_size[2];
	desc.debug_name                 = debug_name;
	switch (format) {
	case SPUDGPU_SHADER_FORMAT_SPIRV:
		desc.code      = shader->spirv;
		desc.code_size = shader->spirv_size;
		break;
	case SPUDGPU_SHADER_FORMAT_DXIL:
		desc.code      = shader->dxil;
		desc.code_size = shader->dxil_size;
		break;
	case SPUDGPU_SHADER_FORMAT_METALLIB:
		desc.code      = shader->metallib;
		desc.code_size = shader->metallib_size;
		break;
	default:
		break;
	}
	return desc;
}

// The one format an adapter's shader_formats names.
static inline SPUDGPU_SHADER_FORMAT spudgpu_test_shader_format(uint32_t shader_formats) {
	for (SPUDGPU_SHADER_FORMAT format = SPUDGPU_SHADER_FORMAT_SPIRV; format <= SPUDGPU_SHADER_FORMAT_METALLIB; ++format)
		if (shader_formats & (1u << format))
			return format;
	return SPUDGPU_SHADER_FORMAT_NONE;
}

#endif
