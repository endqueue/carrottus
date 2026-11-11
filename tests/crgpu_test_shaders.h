// The tests' shaders, as cmake/cr_shaders.cmake builds them from
// tests/shaders/*.slang: SPIR-V on every platform, DXIL where the build
// made it (CARROTTUS_TEST_SHADERS_DXIL), a .metallib where it made one
// (CARROTTUS_TEST_SHADERS_METALLIB). A test hands a module the format its
// adapter reports (CRGPU_ADAPTER_PROPERTIES::shader_formats).

#ifndef CRGPU_TEST_SHADERS_H
#define CRGPU_TEST_SHADERS_H

#include "crgpu.h"
#include <stdint.h>

#include "crgpu_test_objects_draw_vert.spv.inc"
#include "crgpu_test_objects_draw_frag.spv.inc"
#include "crgpu_test_objects_draw_mesh.spv.inc"
#include "crgpu_test_objects_double_comp.spv.inc"
#include "crgpu_test_objects_first_use_comp.spv.inc"
#include "crgpu_test_rq_trace.spv.inc"
#include "crgpu_test_rq_shadow.spv.inc"
#include "crgpu_test_rq_shadow_vert.spv.inc"
#include "crgpu_test_rq_instances_row64.spv.inc"
#include "crgpu_test_rq_instances_col72.spv.inc"
#include "crgpu_test_bindings_vert.spv.inc"
#include "crgpu_test_bindings_whole_frag.spv.inc"
#include "crgpu_test_bindings_part_frag.spv.inc"
#include "crgpu_test_bindings_extra_frag.spv.inc"
#include "crgpu_test_bindings_type_frag.spv.inc"
#include "crgpu_test_bindings_count_frag.spv.inc"
#include "crgpu_test_bindings_whole_comp.spv.inc"
#include "crgpu_test_bindings_part_comp.spv.inc"
#include "crgpu_test_bindings_extra_comp.spv.inc"
#include "crgpu_test_bindings_type_comp.spv.inc"
#include "crgpu_test_bindings_none_comp.spv.inc"
#if CARROTTUS_TEST_SHADERS_DXIL
#include "crgpu_test_objects_draw_vert.dxil.inc"
#include "crgpu_test_objects_draw_frag.dxil.inc"
#include "crgpu_test_objects_draw_mesh.dxil.inc"
#include "crgpu_test_objects_double_comp.dxil.inc"
#include "crgpu_test_objects_first_use_comp.dxil.inc"
#include "crgpu_test_rq_trace.dxil.inc"
#include "crgpu_test_rq_shadow.dxil.inc"
#include "crgpu_test_rq_shadow_vert.dxil.inc"
#include "crgpu_test_rq_instances_row64.dxil.inc"
#include "crgpu_test_rq_instances_col72.dxil.inc"
#include "crgpu_test_bindings_vert.dxil.inc"
#include "crgpu_test_bindings_whole_frag.dxil.inc"
#include "crgpu_test_bindings_part_frag.dxil.inc"
#include "crgpu_test_bindings_extra_frag.dxil.inc"
#include "crgpu_test_bindings_type_frag.dxil.inc"
#include "crgpu_test_bindings_count_frag.dxil.inc"
#include "crgpu_test_bindings_whole_comp.dxil.inc"
#include "crgpu_test_bindings_part_comp.dxil.inc"
#include "crgpu_test_bindings_extra_comp.dxil.inc"
#include "crgpu_test_bindings_type_comp.dxil.inc"
#include "crgpu_test_bindings_none_comp.dxil.inc"
#define CRGPU_TEST_DXIL(name) name##_dxil, name##_dxil_size
#else
#define CRGPU_TEST_DXIL(name) NULL, 0
#endif
#if CARROTTUS_TEST_SHADERS_METALLIB
#include "crgpu_test_objects_draw_vert.metallib.inc"
#include "crgpu_test_objects_draw_frag.metallib.inc"
#include "crgpu_test_objects_draw_mesh.metallib.inc"
#include "crgpu_test_objects_double_comp.metallib.inc"
#include "crgpu_test_objects_first_use_comp.metallib.inc"
#include "crgpu_test_rq_trace.metallib.inc"
#include "crgpu_test_rq_shadow.metallib.inc"
#include "crgpu_test_rq_shadow_vert.metallib.inc"
#include "crgpu_test_rq_instances_row64.metallib.inc"
#include "crgpu_test_rq_instances_col72.metallib.inc"
#include "crgpu_test_bindings_vert.metallib.inc"
#include "crgpu_test_bindings_whole_frag.metallib.inc"
#include "crgpu_test_bindings_part_frag.metallib.inc"
#include "crgpu_test_bindings_extra_frag.metallib.inc"
#include "crgpu_test_bindings_type_frag.metallib.inc"
#include "crgpu_test_bindings_count_frag.metallib.inc"
#include "crgpu_test_bindings_whole_comp.metallib.inc"
#include "crgpu_test_bindings_part_comp.metallib.inc"
#include "crgpu_test_bindings_extra_comp.metallib.inc"
#include "crgpu_test_bindings_type_comp.metallib.inc"
#include "crgpu_test_bindings_none_comp.metallib.inc"
#define CRGPU_TEST_METALLIB(name) name##_metallib, name##_metallib_size
#else
#define CRGPU_TEST_METALLIB(name) NULL, 0
#endif

// One shader: its entry point (the source's stem), stage and workgroup
// size, and its code in each format the build made.
typedef struct crgpu_test_shader {
	const char *entry_point;
	CRGPU_SHADER_STAGE stage;
	uint32_t workgroup_size[3];
	const uint32_t *spirv;
	uint64_t spirv_size;
	const uint32_t *dxil;
	uint64_t dxil_size;
	const uint32_t *metallib;
	uint64_t metallib_size;
} crgpu_test_shader;

#define CRGPU_TEST_SHADER(name, entry, stage, x, y, z) \
	{entry, stage, {x, y, z}, name##_spirv, name##_spirv_size, CRGPU_TEST_DXIL(name), CRGPU_TEST_METALLIB(name)}

static const crgpu_test_shader crgpu_test_objects_draw_vert =
    CRGPU_TEST_SHADER(crgpu_test_objects_draw_vert, "objects_draw_vert", CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
static const crgpu_test_shader crgpu_test_objects_draw_frag =
    CRGPU_TEST_SHADER(crgpu_test_objects_draw_frag, "objects_draw_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_objects_draw_mesh =
    CRGPU_TEST_SHADER(crgpu_test_objects_draw_mesh, "objects_draw_mesh", CRGPU_SHADER_STAGE_MESH, 1, 1, 1);
static const crgpu_test_shader crgpu_test_objects_double_comp =
    CRGPU_TEST_SHADER(crgpu_test_objects_double_comp, "objects_double_comp", CRGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);
static const crgpu_test_shader crgpu_test_objects_first_use_comp =
    CRGPU_TEST_SHADER(crgpu_test_objects_first_use_comp, "objects_first_use_comp", CRGPU_SHADER_STAGE_COMPUTE, 8, 8, 1);
static const crgpu_test_shader crgpu_test_rq_trace =
    CRGPU_TEST_SHADER(crgpu_test_rq_trace, "rq_trace", CRGPU_SHADER_STAGE_COMPUTE, 8, 8, 1);
static const crgpu_test_shader crgpu_test_rq_shadow =
    CRGPU_TEST_SHADER(crgpu_test_rq_shadow, "rq_shadow", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_rq_shadow_vert =
    CRGPU_TEST_SHADER(crgpu_test_rq_shadow_vert, "rq_shadow_vert", CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
static const crgpu_test_shader crgpu_test_rq_instances_row64 =
    CRGPU_TEST_SHADER(crgpu_test_rq_instances_row64, "rq_instances_row64", CRGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);
static const crgpu_test_shader crgpu_test_rq_instances_col72 =
    CRGPU_TEST_SHADER(crgpu_test_rq_instances_col72, "rq_instances_col72", CRGPU_SHADER_STAGE_COMPUTE, 64, 1, 1);
// Every binding of a set (card S27): crgpu_objects_test's test_set_bindings.
static const crgpu_test_shader crgpu_test_bindings_vert =
    CRGPU_TEST_SHADER(crgpu_test_bindings_vert, "bindings_vert", CRGPU_SHADER_STAGE_VERTEX, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_whole_frag =
    CRGPU_TEST_SHADER(crgpu_test_bindings_whole_frag, "bindings_whole_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_part_frag =
    CRGPU_TEST_SHADER(crgpu_test_bindings_part_frag, "bindings_part_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_extra_frag =
    CRGPU_TEST_SHADER(crgpu_test_bindings_extra_frag, "bindings_extra_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_type_frag =
    CRGPU_TEST_SHADER(crgpu_test_bindings_type_frag, "bindings_type_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_count_frag =
    CRGPU_TEST_SHADER(crgpu_test_bindings_count_frag, "bindings_count_frag", CRGPU_SHADER_STAGE_FRAGMENT, 0, 0, 0);
static const crgpu_test_shader crgpu_test_bindings_whole_comp =
    CRGPU_TEST_SHADER(crgpu_test_bindings_whole_comp, "bindings_whole_comp", CRGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);
static const crgpu_test_shader crgpu_test_bindings_part_comp =
    CRGPU_TEST_SHADER(crgpu_test_bindings_part_comp, "bindings_part_comp", CRGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);
static const crgpu_test_shader crgpu_test_bindings_extra_comp =
    CRGPU_TEST_SHADER(crgpu_test_bindings_extra_comp, "bindings_extra_comp", CRGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);
static const crgpu_test_shader crgpu_test_bindings_type_comp =
    CRGPU_TEST_SHADER(crgpu_test_bindings_type_comp, "bindings_type_comp", CRGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);
static const crgpu_test_shader crgpu_test_bindings_none_comp =
    CRGPU_TEST_SHADER(crgpu_test_bindings_none_comp, "bindings_none_comp", CRGPU_SHADER_STAGE_COMPUTE, 1, 1, 1);

// [shader]'s module desc in [format] (one of the adapter's shader_formats),
// with [debug_name]. code is NULL when the build made no code in [format].
static inline crgpu_shader_module_desc crgpu_test_module_desc(
    const crgpu_test_shader *shader,
    CRGPU_SHADER_FORMAT format,
    const char *debug_name) {
	crgpu_shader_module_desc desc = {0};
	desc.base.type                  = CR_STRUCT_TYPE_GPU_SHADER_MODULE_DESC;
	desc.stage                      = shader->stage;
	desc.format                     = format;
	desc.entry_point                = shader->entry_point;
	desc.workgroup_size[0]          = shader->workgroup_size[0];
	desc.workgroup_size[1]          = shader->workgroup_size[1];
	desc.workgroup_size[2]          = shader->workgroup_size[2];
	desc.debug_name                 = debug_name;
	switch (format) {
	case CRGPU_SHADER_FORMAT_SPIRV:
		desc.code      = shader->spirv;
		desc.code_size = shader->spirv_size;
		break;
	case CRGPU_SHADER_FORMAT_DXIL:
		desc.code      = shader->dxil;
		desc.code_size = shader->dxil_size;
		break;
	case CRGPU_SHADER_FORMAT_METALLIB:
		desc.code      = shader->metallib;
		desc.code_size = shader->metallib_size;
		break;
	default:
		break;
	}
	return desc;
}

// The one format an adapter's shader_formats names.
static inline CRGPU_SHADER_FORMAT crgpu_test_shader_format(uint32_t shader_formats) {
	for (CRGPU_SHADER_FORMAT format = CRGPU_SHADER_FORMAT_SPIRV; format <= CRGPU_SHADER_FORMAT_METALLIB; ++format)
		if (shader_formats & (1u << format))
			return format;
	return CRGPU_SHADER_FORMAT_NONE;
}

#endif
