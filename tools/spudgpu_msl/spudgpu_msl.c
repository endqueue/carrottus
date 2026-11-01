/*
 * spudgpu_msl: SPIR-V in, Metal source out, in the binding layout SpudGPU's
 * Metal backend reads (spudgpu.h, spudgpu_shader_module_desc). A build tool
 * for the caller's shader build, run on the build machine; never part of
 * the library, which compiles nothing (card A9, revised). It carries the
 * SPIRV-Cross options and remaps the Metal backend applied at run time
 * until 2026-10-31 (card S20), for card A29's fallback route: Slang to
 * SPIR-V, then this, then Apple's compiler.
 *
 *     spudgpu_msl --platform macos|ios --msl-version <MMmmpp> <in.spv> <out.metal>
 *
 * Every choice is the command line's: no platform and no Metal language
 * version is assumed. The module's one entry point keeps its name (main
 * excepted, which Metal reserves and SPIRV-Cross renames main0). Exits 0,
 * or 1 with the reason on stderr.
 */

#include <spirv_cross_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The layout spudgpu.h states for Metal: set s's argument buffer at buffer
 * index 26 + s (SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS sets below the push
 * constants), the push constants at buffer index 30, and a combined image
 * sampler's sampler at [[id(1024 + binding)]]. */
#define SPUDGPU_MSL_MAX_SETS 4
#define SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER 30
#define SPUDGPU_MSL_SET_BUFFER_BASE (SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER - SPUDGPU_MSL_MAX_SETS)
#define SPUDGPU_MSL_COMBINED_SAMPLER_BASE 1024u

static int fail(const char *what, const char *detail) {
	fprintf(stderr, "spudgpu_msl: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

int main(int argc, char **argv) {
	const char *platform    = NULL;
	const char *msl_version = NULL;
	const char *in_path     = NULL;
	const char *out_path    = NULL;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc)
			platform = argv[++i];
		else if (strcmp(argv[i], "--msl-version") == 0 && i + 1 < argc)
			msl_version = argv[++i];
		else if (!in_path)
			in_path = argv[i];
		else if (!out_path)
			out_path = argv[i];
		else
			return fail("unexpected argument", argv[i]);
	}
	if (!platform || !msl_version || !in_path || !out_path)
		return fail("usage: spudgpu_msl --platform macos|ios --msl-version <MMmmpp> <in.spv> <out.metal>", NULL);
	unsigned msl_platform;
	if (strcmp(platform, "macos") == 0)
		msl_platform = SPVC_MSL_PLATFORM_MACOS;
	else if (strcmp(platform, "ios") == 0)
		msl_platform = SPVC_MSL_PLATFORM_IOS;
	else
		return fail("--platform is neither macos nor ios", platform);
	char *version_end      = NULL;
	unsigned long version  = strtoul(msl_version, &version_end, 10);
	if (!version_end || *version_end || version == 0)
		return fail("--msl-version is not a number such as 30100", msl_version);

	FILE *in = fopen(in_path, "rb");
	if (!in)
		return fail("can't open", in_path);
	fseek(in, 0, SEEK_END);
	const long size = ftell(in);
	fseek(in, 0, SEEK_SET);
	if (size <= 0 || size % 4 != 0) {
		fclose(in);
		return fail("not SPIR-V words", in_path);
	}
	SpvId *words = (SpvId *)malloc((size_t)size);
	if (!words) {
		fclose(in);
		return fail("out of memory", NULL);
	}
	const size_t read = fread(words, 1, (size_t)size, in);
	fclose(in);
	if (read != (size_t)size) {
		free(words);
		return fail("can't read", in_path);
	}

	spvc_context context = NULL;
	if (spvc_context_create(&context) != SPVC_SUCCESS) {
		free(words);
		return fail("SPIRV-Cross context", NULL);
	}
	int status               = 1;
	spvc_parsed_ir parsed_ir = NULL;
	spvc_compiler compiler   = NULL;
	if (spvc_context_parse_spirv(context, words, (size_t)size / sizeof(SpvId), &parsed_ir) != SPVC_SUCCESS) {
		fail("SPIR-V parse failed", spvc_context_get_last_error_string(context));
		goto done;
	}
	if (spvc_context_create_compiler(context, SPVC_BACKEND_MSL, parsed_ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) !=
	    SPVC_SUCCESS) {
		fail("SPIRV-Cross compiler", spvc_context_get_last_error_string(context));
		goto done;
	}

	const spvc_entry_point *entry_points = NULL;
	size_t entry_point_count             = 0;
	spvc_compiler_get_entry_points(compiler, &entry_points, &entry_point_count);
	if (entry_point_count != 1) {
		fail("a module has one entry point (spudgpu.h)", NULL);
		goto done;
	}
	const SpvExecutionModel model = entry_points[0].execution_model;

	spvc_compiler_options options = NULL;
	spvc_compiler_create_compiler_options(compiler, &options);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION, (unsigned)version);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM, msl_platform);
	// SPIRV-Cross refuses a vertex shader's base vertex or instance on iOS
	// unless told the GPU has them (A9 and later). Every GPU that runs Metal
	// 3 is an A13 or later, so from that version it is a fact, not a choice.
	if (msl_platform == SPVC_MSL_PLATFORM_IOS && version >= 30000)
		spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_IOS_SUPPORT_BASE_VERTEX_INSTANCE, SPVC_TRUE);
	// Every set is an argument buffer (tier 1), each member at its binding's
	// own [[id]], every resource the shader declares kept in it whether the
	// entry point reads it or not: the backend encodes a set from its
	// layout's every binding.
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_ARGUMENT_BUFFERS, SPVC_TRUE);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_ARGUMENT_BUFFERS_TIER, 1u);
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING, SPVC_TRUE);
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_FORCE_ACTIVE_ARGUMENT_BUFFER_RESOURCES, SPVC_TRUE);
	// No Y flip: clip space is +Y up on every backend (card S19).
	spvc_compiler_install_compiler_options(compiler, options);

	for (unsigned set = 0; set < SPUDGPU_MSL_MAX_SETS; ++set) {
		spvc_msl_resource_binding_2 binding;
		spvc_msl_resource_binding_init_2(&binding);
		binding.stage      = model;
		binding.desc_set   = set;
		binding.binding    = SPVC_MSL_ARGUMENT_BUFFER_BINDING;
		binding.msl_buffer = SPUDGPU_MSL_SET_BUFFER_BASE + set;
		spvc_compiler_msl_add_resource_binding_2(compiler, &binding);
	}
	{
		spvc_resources resources                      = NULL;
		const spvc_reflected_resource *sampled_images = NULL;
		size_t sampled_image_count                    = 0;
		if (spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS &&
		    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &sampled_images, &sampled_image_count) ==
		        SPVC_SUCCESS) {
			for (size_t i = 0; i < sampled_image_count; ++i) {
				const unsigned set     = spvc_compiler_get_decoration(compiler, sampled_images[i].id, SpvDecorationDescriptorSet);
				const unsigned binding = spvc_compiler_get_decoration(compiler, sampled_images[i].id, SpvDecorationBinding);
				spvc_msl_resource_binding_2 combined;
				spvc_msl_resource_binding_init_2(&combined);
				combined.stage       = model;
				combined.desc_set    = set;
				combined.binding     = binding;
				combined.msl_texture = binding;
				combined.msl_sampler = SPUDGPU_MSL_COMBINED_SAMPLER_BASE + binding;
				spvc_compiler_msl_add_resource_binding_2(compiler, &combined);
			}
		}
	}
	{
		spvc_msl_resource_binding_2 push_constants;
		spvc_msl_resource_binding_init_2(&push_constants);
		push_constants.stage      = model;
		push_constants.desc_set   = SPVC_MSL_PUSH_CONSTANT_DESC_SET;
		push_constants.binding    = SPVC_MSL_PUSH_CONSTANT_BINDING;
		push_constants.msl_buffer = SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER;
		spvc_compiler_msl_add_resource_binding_2(compiler, &push_constants);
	}

	const char *source = NULL;
	if (spvc_compiler_compile(compiler, &source) != SPVC_SUCCESS || !source) {
		fail("SPIR-V to Metal failed", spvc_context_get_last_error_string(context));
		goto done;
	}
	FILE *out = fopen(out_path, "wb");
	if (!out) {
		fail("can't write", out_path);
		goto done;
	}
	const size_t length = strlen(source);
	status              = fwrite(source, 1, length, out) == length ? 0 : fail("can't write", out_path);
	if (fclose(out) != 0)
		status = fail("can't write", out_path);

done:
	spvc_context_destroy(context);
	free(words);
	return status;
}
