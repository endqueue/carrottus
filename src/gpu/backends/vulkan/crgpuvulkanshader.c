
#if CRGPU_COMPILE_VULKAN_API

#include "crgpu.h"
#include "crgpuvulkan.h"
#include "../../crgpushared.h"
#include <stdlib.h>
#include <string.h>
#include "../../../crstructbase.h"

VkPrimitiveTopology
crgpuvulkan___primitive_topology(CRGPU_PRIMITIVE_TOPOLOGY topology) {
	switch (topology) {
	case CRGPU_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case CRGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case CRGPU_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case CRGPU_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
	case CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	default:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

VkShaderStageFlagBits
crgpuvulkan___shader_stage_flag_internal(CRGPU_SHADER_STAGE stage) {
	switch (stage) {
	case CRGPU_SHADER_STAGE_VERTEX:
		return VK_SHADER_STAGE_VERTEX_BIT;
	case CRGPU_SHADER_STAGE_FRAGMENT:
		return VK_SHADER_STAGE_FRAGMENT_BIT;
	case CRGPU_SHADER_STAGE_COMPUTE:
		return VK_SHADER_STAGE_COMPUTE_BIT;
	case CRGPU_SHADER_STAGE_GEOMETRY:
		return VK_SHADER_STAGE_GEOMETRY_BIT;
	case CRGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	case CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	case CRGPU_SHADER_STAGE_MESH:
		return VK_SHADER_STAGE_MESH_BIT_EXT;
	case CRGPU_SHADER_STAGE_TASK:
		return VK_SHADER_STAGE_TASK_BIT_EXT;
	default:
		return (VkShaderStageFlagBits)0;
	}
}

VkFormat crgpuvulkan___vertex_attr_format_internal(CRGPU_FORMAT fmt) {
	return convert_cr_to_vulkan_format((VkFormat)fmt);
}

VkResult crgpuvulkan___create_shader_module_internal(
    VkDevice vk_device,
    const uint32_t *spirv_code,
    size_t spirv_size,
    VkShaderModule *out_module) {
	VkShaderModuleCreateInfo createInfo = {0};
	createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = spirv_size;
	createInfo.pCode    = spirv_code;
	return vkCreateShaderModule(vk_device, &createInfo, NULL, out_module);
}

// Depth/stencil-combined formats need stencilAttachmentFormat set to the same
// format for VkPipelineRenderingCreateInfo; depth-only formats must leave it
// VK_FORMAT_UNDEFINED (Vulkan validation rejects a stencil format on a view
// with no stencil aspect).
// CRGPU_BLEND_FACTOR as VkBlendFactor, whose numbering differs.
static VkBlendFactor crgpuvulkan___blend_factor(CRGPU_BLEND_FACTOR factor) {
	switch (factor) {
	case CRGPU_BLEND_FACTOR_ZERO:
		return VK_BLEND_FACTOR_ZERO;
	case CRGPU_BLEND_FACTOR_ONE:
		return VK_BLEND_FACTOR_ONE;
	case CRGPU_BLEND_FACTOR_SRC_ALPHA:
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case CRGPU_BLEND_FACTOR_DST_ALPHA:
		return VK_BLEND_FACTOR_DST_ALPHA;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case CRGPU_BLEND_FACTOR_SRC_COLOR:
		return VK_BLEND_FACTOR_SRC_COLOR;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case CRGPU_BLEND_FACTOR_DST_COLOR:
		return VK_BLEND_FACTOR_DST_COLOR;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	default:
		return VK_BLEND_FACTOR_ZERO;
	}
}

static int crgpuvulkan___format_has_stencil(VkFormat fmt) {
	switch (fmt) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return 1;
	default:
		return 0;
	}
}

VkShaderStageFlags crgpuvulkan___shader_stages(CRGPU_SHADER_STAGE stages) {
	VkShaderStageFlags sf = 0;
	if (stages & CRGPU_SHADER_STAGE_VERTEX)
		sf |= VK_SHADER_STAGE_VERTEX_BIT;
	if (stages & CRGPU_SHADER_STAGE_FRAGMENT)
		sf |= VK_SHADER_STAGE_FRAGMENT_BIT;
	if (stages & CRGPU_SHADER_STAGE_COMPUTE)
		sf |= VK_SHADER_STAGE_COMPUTE_BIT;
	if (stages & CRGPU_SHADER_STAGE_GEOMETRY)
		sf |= VK_SHADER_STAGE_GEOMETRY_BIT;
	if (stages & CRGPU_SHADER_STAGE_TESSELLATION_CONTROL)
		sf |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	if (stages & CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION)
		sf |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	if (stages & CRGPU_SHADER_STAGE_MESH)
		sf |= VK_SHADER_STAGE_MESH_BIT_EXT;
	if (stages & CRGPU_SHADER_STAGE_TASK)
		sf |= VK_SHADER_STAGE_TASK_BIT_EXT;
	return sf;
}

// Whether [words] (word_count of them) is SPIR-V with an OpEntryPoint named
// [name] for [stage]: CR_SUCCESS; CR_RESULT_GPU_INVALID_SHADER_CODE for
// words that aren't SPIR-V (the magic word, or an instruction running past
// the end); CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND for none. Reads the
// header and walks the instructions as far as the entry points, which
// come first in a module's layout.
static CR_RESULT crgpuvulkan___find_entry_point(
    const uint32_t *words,
    size_t word_count,
    CRGPU_SHADER_STAGE stage,
    const char *name) {
	const uint32_t magic          = 0x07230203u;
	const uint32_t op_entry_point = 15;
	const uint32_t op_function    = 54;
	uint32_t model;
	switch (stage) {
	case CRGPU_SHADER_STAGE_VERTEX:
		model = 0;
		break;
	case CRGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		model = 1;
		break;
	case CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		model = 2;
		break;
	case CRGPU_SHADER_STAGE_GEOMETRY:
		model = 3;
		break;
	case CRGPU_SHADER_STAGE_FRAGMENT:
		model = 4;
		break;
	case CRGPU_SHADER_STAGE_COMPUTE:
		model = 5;
		break;
	case CRGPU_SHADER_STAGE_TASK:
		model = 5364; // TaskEXT
		break;
	default:
		model = 5365; // MeshEXT
		break;
	}
	if (word_count < 5 || words[0] != magic)
		return CR_RESULT_GPU_INVALID_SHADER_CODE;
	const size_t name_length = strlen(name);
	size_t i                 = 5;
	while (i < word_count) {
		const uint32_t count  = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFFu;
		if (count == 0 || count > word_count - i)
			return CR_RESULT_GPU_INVALID_SHADER_CODE;
		if (opcode == op_function)
			break; // Past the entry points.
		// OpEntryPoint: model, id, then the name, a nul-terminated string
		// packed into the words that follow.
		if (opcode == op_entry_point && count >= 4 && words[i + 1] == model) {
			const char *literal       = (const char *)&words[i + 3];
			const size_t literal_room = (size_t)(count - 3) * 4;
			if (name_length < literal_room && memcmp(literal, name, name_length + 1) == 0)
				return CR_SUCCESS;
		}
		i += count;
	}
	return CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND;
}

#if __cplusplus
extern "C" {

#endif

CR_RESULT crgpu_shader_module_create(
    crgpu_device device,
    const crgpu_shader_module_desc *desc,
    crgpu_shader_module *out_module) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_shader_module_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_SHADER_MODULE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_module)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	const CR_RESULT desc_sr = crgpu___internal_check_shader_module_desc(desc, device->_features);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	const CR_RESULT entry_sr = crgpuvulkan___find_entry_point(
	    (const uint32_t *)desc->code, (size_t)(desc->code_size / 4), desc->stage, desc->entry_point);
	if (entry_sr != CR_SUCCESS)
		return entry_sr;

	crgpu_shader_module_vulkan result = {0};
	result.head.allocator               = device->head.allocator;
	CR_BIRTH_IN(&result, &device->_shared->quarantine);
	result._device                      = *((crgpu_device_vulkan *)device);
	result._stage_vk = crgpuvulkan___shader_stage_flag_internal(desc->stage);
	result._entry_point = cr_string_copy(&result.head.allocator, desc->entry_point);
	if (!result._entry_point)
		return CR_RESULT_OUT_OF_MEMORY;

	// vkCreateShaderModule copies the words.
	if (crgpuvulkan___create_shader_module_internal(
	        result._device._logical_device_vk,
	        (const uint32_t *)desc->code, desc->code_size,
	        &result._shader_module_vk) != VK_SUCCESS) {
		cr_string_free(&result.head.allocator, result._entry_point);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	crgpu_shader_module_vulkan *pResult =
	    CR_NEW(&result.head.allocator, crgpu_shader_module_vulkan);
	if (!pResult) {
		vkDestroyShaderModule(result._device._logical_device_vk, result._shader_module_vk, NULL);
		cr_string_free(&result.head.allocator, result._entry_point);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(crgpu_shader_module_vulkan));
	// From here crgpu_shader_module_destroy frees it, and takes the count
	// back.
	CRGPU___LIVE_ADD(&pResult->_device._shared->live, CRGPU___OBJECT_SHADER_MODULE);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_shader_module_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	*out_module = pResult;
	return CR_SUCCESS;
}

void crgpu_shader_module_destroy(crgpu_shader_module shader_module) {
	if (!shader_module)
		return;
	if (CR_STALE(shader_module, "crgpu_shader_module_destroy", "shader_module"))
		return;
	vkDestroyShaderModule(
	    shader_module->_device._logical_device_vk,
	    shader_module->_shader_module_vk, NULL);
	CRGPU___LIVE_REMOVE(&shader_module->_device._shared->live, CRGPU___OBJECT_SHADER_MODULE);
	CR_KILL(shader_module);
	const CR_ALLOCATOR allocator = shader_module->head.allocator;
	cr_string_free(&allocator, shader_module->_entry_point);
	cr_string_free(&allocator, shader_module->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(shader_module), &allocator, crgpu_shader_module_vulkan, shader_module);
}

CR_RESULT crgpu_graphics_pipeline_create(
    crgpu_device device,
    const crgpu_graphics_pipeline_desc *desc,
    crgpu_graphics_pipeline *out_pipeline) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_graphics_pipeline_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_pipeline)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;

	// Validate: at minimum a fragment shader plus either a vertex module
	// (classic pipeline) or a mesh module (CRGPU_EXT_MESH_SHADING - mesh
	// pipelines have no vertex-fetch/input-assembly stage at all, see
	// crgpu.h) is required. The two are mutually exclusive.
	if ((!desc->vertex_module && !desc->mesh_module) || !desc->fragment_module)
		return CR_RESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED;
	if (desc->vertex_module && desc->mesh_module)
		return CR_RESULT_GPU_INVALID_SHADER_STAGE;
	if (desc->mesh_module && !(device->_features & CRGPU_FEATURE_MESH_SHADING))
		return CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	CR_RESULT outputs = crgpu___internal_check_pipeline_outputs(desc, device->_features);
	if (outputs != CR_SUCCESS)
		return outputs;

	crgpu_shader_pipeline_vulkan result = {0};
	result.head.allocator                 = device->head.allocator;
	CR_BIRTH_IN(&result, &device->_shared->quarantine);
	result._device                        = *device;
	result._desc                          = *desc;

	bool is_mesh_pipeline = desc->mesh_module != NULL;

	// ------------------------------------------------------------------
	// 1. Shader stages
	// ------------------------------------------------------------------
	VkPipelineShaderStageCreateInfo shader_stages[8] = {0};
	uint32_t stage_count                             = 0;

	// Vertex stage (classic pipeline only)
	if (desc->vertex_module) {
		crgpu_shader_module_vulkan *vert =
		    (crgpu_shader_module_vulkan *)desc->vertex_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		shader_stages[stage_count].module = vert->_shader_module_vk;
		shader_stages[stage_count].pName =
		    vert->_entry_point;
		stage_count++;
	}

	// Optional task (amplification) stage - mesh pipeline only
	if (desc->task_module) {
		crgpu_shader_module_vulkan *task =
		    (crgpu_shader_module_vulkan *)desc->task_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage  = VK_SHADER_STAGE_TASK_BIT_EXT;
		shader_stages[stage_count].module = task->_shader_module_vk;
		shader_stages[stage_count].pName =
		    task->_entry_point;
		stage_count++;
	}

	// Mesh stage (mesh pipeline only)
	if (desc->mesh_module) {
		crgpu_shader_module_vulkan *mesh =
		    (crgpu_shader_module_vulkan *)desc->mesh_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage  = VK_SHADER_STAGE_MESH_BIT_EXT;
		shader_stages[stage_count].module = mesh->_shader_module_vk;
		shader_stages[stage_count].pName =
		    mesh->_entry_point;
		stage_count++;
	}

	// Fragment stage (required for graphics pipelines)
	{
		crgpu_shader_module_vulkan *frag =
		    (crgpu_shader_module_vulkan *)desc->fragment_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		shader_stages[stage_count].module = frag->_shader_module_vk;
		shader_stages[stage_count].pName =
		    frag->_entry_point;
		stage_count++;
	}

	// Optional geometry stage
	if (desc->geometry_module) {
		crgpu_shader_module_vulkan *geom =
		    (crgpu_shader_module_vulkan *)desc->geometry_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage  = VK_SHADER_STAGE_GEOMETRY_BIT;
		shader_stages[stage_count].module = geom->_shader_module_vk;
		shader_stages[stage_count].pName =
		    geom->_entry_point;
		stage_count++;
	}

	// Optional tessellation control stage
	if (desc->tess_control_module) {
		crgpu_shader_module_vulkan *tc =
		    (crgpu_shader_module_vulkan *)desc->tess_control_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage =
		    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		shader_stages[stage_count].module = tc->_shader_module_vk;
		shader_stages[stage_count].pName  = tc->_entry_point;
		stage_count++;
	}

	// Optional tessellation evaluation stage
	if (desc->tess_eval_module) {
		crgpu_shader_module_vulkan *te =
		    (crgpu_shader_module_vulkan *)desc->tess_eval_module;
		shader_stages[stage_count].sType =
		    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[stage_count].stage =
		    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		shader_stages[stage_count].module = te->_shader_module_vk;
		shader_stages[stage_count].pName =
		    te->_entry_point;
		stage_count++;
	}

	// ------------------------------------------------------------------
	// 2. Vertex input state  (binding 0, per-vertex, all attributes)
	// ------------------------------------------------------------------
	// We support up to CRGPU_MAX_VERTEX_ATTRIBUTES attributes described
	// in desc->vertex_attributes / desc->vertex_attribute_count.
	VkVertexInputAttributeDescription
	    attr_descs[CRGPU_MAX_VERTEX_ATTRIBUTES] = {0};
	for (uint32_t i = 0; i < desc->vertex_attribute_count; i++) {
		attr_descs[i].location = desc->vertex_attributes[i].location;
		attr_descs[i].binding  = desc->vertex_attributes[i].binding;
		attr_descs[i].format   = crgpuvulkan___vertex_attr_format_internal(
		    desc->vertex_attributes[i].format);
		attr_descs[i].offset = desc->vertex_attributes[i].offset;
	}

	VkVertexInputBindingDescription binding_descs[CRGPU_MAX_VERTEX_BINDINGS] =
	    {0};
	for (uint32_t i = 0; i < desc->vertex_binding_count; i++) {
		binding_descs[i].binding   = desc->vertex_bindings[i].binding;
		binding_descs[i].stride    = desc->vertex_bindings[i].stride;
		binding_descs[i].inputRate = desc->vertex_bindings[i].per_instance
		                                 ? VK_VERTEX_INPUT_RATE_INSTANCE
		                                 : VK_VERTEX_INPUT_RATE_VERTEX;
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo = {0};
	vertexInputInfo.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = desc->vertex_binding_count;
	vertexInputInfo.pVertexBindingDescriptions    = binding_descs;
	vertexInputInfo.vertexAttributeDescriptionCount =
	    desc->vertex_attribute_count;
	vertexInputInfo.pVertexAttributeDescriptions = attr_descs;

	// ------------------------------------------------------------------
	// 3. Input assembly
	// ------------------------------------------------------------------
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {0};
	inputAssembly.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology =
	    crgpuvulkan___primitive_topology(desc->primitive_topology);
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	// ------------------------------------------------------------------
	// 4. Viewport / scissor  (dynamic; set via command list at draw time)
	// ------------------------------------------------------------------
	VkPipelineViewportStateCreateInfo viewportState = {0};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;
	// pViewports / pScissors intentionally left NULL – declared dynamic below.

	// ------------------------------------------------------------------
	// 5. Rasterizer
	// ------------------------------------------------------------------
	VkPipelineRasterizationStateCreateInfo rasterizer = {0};
	rasterizer.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable        = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode =
	    desc->wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth       = 1.0f;
	rasterizer.cullMode        = (VkCullModeFlags)desc->cull_mode;
	rasterizer.frontFace       = desc->front_face_ccw
	                                 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
	                                 : VK_FRONT_FACE_CLOCKWISE;
	// All three 0 is no bias (crgpu.h). A clamp alone biases nothing.
	rasterizer.depthBiasEnable =
	    (desc->depth_bias_constant != 0 || desc->depth_bias_slope != 0.0f) ? VK_TRUE : VK_FALSE;
	rasterizer.depthBiasConstantFactor = (float)desc->depth_bias_constant;
	rasterizer.depthBiasSlopeFactor    = desc->depth_bias_slope;
	rasterizer.depthBiasClamp          = desc->depth_bias_clamp;

	// ------------------------------------------------------------------
	// 6. Multisampling  (1× — no MSAA for now)
	// ------------------------------------------------------------------
	VkPipelineMultisampleStateCreateInfo multisampling = {0};
	multisampling.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable  = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// ------------------------------------------------------------------
	// 7. Depth / stencil
	// ------------------------------------------------------------------
	VkPipelineDepthStencilStateCreateInfo depthStencil = {0};
	depthStencil.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = desc->depth_test_enable ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable =
	    desc->depth_write_enable ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp        = (VkCompareOp)desc->depth_compare_op;
	// Only on a device created with the feature, which is what switched
	// depthBounds on: without it the pipeline behaves as if the test were off
	// (crgpu.h).
	depthStencil.depthBoundsTestEnable =
	    desc->depth_bounds_test_enable && (device->_features & CRGPU_FEATURE_DEPTH_BOUNDS_TEST) ? VK_TRUE : VK_FALSE;
	depthStencil.stencilTestEnable     = VK_FALSE;

	// ------------------------------------------------------------------
	// 8. Colour blend attachments, one for each colour attachment, from
	// the caller's factors and ops (CRGPU_BLEND_OP is VkBlendOp's
	// numbering; CRGPU_BLEND_FACTOR is not VkBlendFactor's).
	// ------------------------------------------------------------------
	VkPipelineColorBlendAttachmentState colorBlendAttachments[CRGPU_MAX_COLOR_ATTACHMENTS] = {0};
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i) {
		const crgpu_blend_attachment_desc *ba = &desc->blend_attachments[i];
		VkPipelineColorBlendAttachmentState *out = &colorBlendAttachments[i];
		out->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		out->blendEnable = ba->blend_enable ? VK_TRUE : VK_FALSE;
		if (ba->blend_enable) {
			out->srcColorBlendFactor = crgpuvulkan___blend_factor(ba->src_color_blend_factor);
			out->dstColorBlendFactor = crgpuvulkan___blend_factor(ba->dst_color_blend_factor);
			out->colorBlendOp        = (VkBlendOp)ba->color_blend_op;
			out->srcAlphaBlendFactor = crgpuvulkan___blend_factor(ba->src_alpha_blend_factor);
			out->dstAlphaBlendFactor = crgpuvulkan___blend_factor(ba->dst_alpha_blend_factor);
			out->alphaBlendOp        = (VkBlendOp)ba->alpha_blend_op;
		}
	}

	VkPipelineColorBlendStateCreateInfo colorBlending = {0};
	colorBlending.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable   = VK_FALSE;
	colorBlending.attachmentCount = desc->color_attachment_count;
	colorBlending.pAttachments    = colorBlendAttachments;

	// ------------------------------------------------------------------
	// 9. Dynamic state – viewport + scissor changed per frame via commands
	// ------------------------------------------------------------------
	VkDynamicState dynamic_states[3] = {
	    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	uint32_t dynamic_state_count = 2;
	if (desc->depth_bounds_test_enable) {
		dynamic_states[dynamic_state_count++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS;
	}
	VkPipelineDynamicStateCreateInfo dynamicState = {0};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = dynamic_state_count;
	dynamicState.pDynamicStates    = dynamic_states;

	// ------------------------------------------------------------------
	// 10. Pipeline layout  (push constant ranges + descriptor set layouts)
	// ------------------------------------------------------------------
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	VkPushConstantRange push_constant_ranges[CRGPU_MAX_PUSH_CONSTANT_RANGES] =
	    {0};
	for (uint32_t i = 0; i < desc->push_constant_range_count; i++) {
		push_constant_ranges[i].stageFlags = crgpuvulkan___shader_stages(desc->push_constant_ranges[i].stage_flags);
		push_constant_ranges[i].offset = desc->push_constant_ranges[i].offset;
		push_constant_ranges[i].size   = desc->push_constant_ranges[i].size;
	}
	pipelineLayoutInfo.pushConstantRangeCount = desc->push_constant_range_count;
	pipelineLayoutInfo.pPushConstantRanges    = push_constant_ranges;

	VkDescriptorSetLayout vk_layouts[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	for (uint32_t i = 0; i < desc->descriptor_set_layout_count; i++) {
		crgpu_descriptor_set_layout_vulkan *vk_layout =
		    (crgpu_descriptor_set_layout_vulkan *)
		        desc->descriptor_set_layouts[i];
		vk_layouts[i] = vk_layout->_layout_vk;
	}

	pipelineLayoutInfo.setLayoutCount = desc->descriptor_set_layout_count;
	pipelineLayoutInfo.pSetLayouts    = vk_layouts;

	VkDevice vk_device = device->_logical_device_vk;

	if (vkCreatePipelineLayout(
	        vk_device, &pipelineLayoutInfo, NULL,
	        &result._pipeline_layout_vk) != VK_SUCCESS) {
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	// ------------------------------------------------------------------
	// 11. Dynamic rendering attachment formats — no VkRenderPass object.
	// Vulkan requires these to match whatever's actually bound at
	// crgpu_command_list_begin_rendering time (see crgpu.h's rendering section).
	// ------------------------------------------------------------------
	VkFormat color_formats[CRGPU_MAX_COLOR_ATTACHMENTS] = {0};
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i)
		color_formats[i] = convert_cr_to_vulkan_format(desc->color_attachment_formats[i]);
	VkFormat depth_format   = VK_FORMAT_UNDEFINED;
	VkFormat stencil_format = VK_FORMAT_UNDEFINED;
	if (desc->depth_format != CRGPU_FORMAT_UNKNOWN) {
		depth_format = convert_cr_to_vulkan_format((VkFormat)desc->depth_format);
		if (crgpuvulkan___format_has_stencil(depth_format))
			stencil_format = depth_format;
	}

	VkPipelineRenderingCreateInfo renderingInfo = {0};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingInfo.colorAttachmentCount    = desc->color_attachment_count;
	renderingInfo.pColorAttachmentFormats = color_formats;
	renderingInfo.depthAttachmentFormat   = depth_format;
	renderingInfo.stencilAttachmentFormat = stencil_format;

	// ------------------------------------------------------------------
	// 12. Tessellation state (only wired up when both tess stages present)
	// ------------------------------------------------------------------
	VkPipelineTessellationStateCreateInfo tessellationState = {0};
	tessellationState.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	tessellationState.patchControlPoints =
	    desc->patch_control_points ? desc->patch_control_points : 3;

	// ------------------------------------------------------------------
	// 13. Assemble and create the graphics pipeline
	// ------------------------------------------------------------------
	VkGraphicsPipelineCreateInfo pipelineInfo = {0};
	pipelineInfo.sType      = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext      = &renderingInfo;
	pipelineInfo.stageCount = stage_count;
	pipelineInfo.pStages    = shader_stages;
	// A mesh pipeline has no vertex-fetch/input-assembly stage at all - the
	// mesh shader emits geometry directly (see crgpu.h's
	// CRGPU_EXT_MESH_SHADING section) - so these must stay NULL rather
	// than pointing at the (meaningless, all-zero) structs built above.
	pipelineInfo.pVertexInputState   = is_mesh_pipeline ? NULL : &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = is_mesh_pipeline ? NULL : &inputAssembly;
	pipelineInfo.pViewportState      = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState   = &multisampling;
	pipelineInfo.pDepthStencilState  = &depthStencil;
	pipelineInfo.pColorBlendState    = &colorBlending;
	pipelineInfo.pDynamicState       = &dynamicState;
	pipelineInfo.layout              = result._pipeline_layout_vk;
	pipelineInfo.renderPass          = VK_NULL_HANDLE; // dynamic rendering — no render pass object
	pipelineInfo.subpass             = 0;
	pipelineInfo.basePipelineHandle  = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex   = -1;

	// Tessellation state is only meaningful when both tess stages exist
	// (and never alongside a mesh pipeline, which has no fixed-function
	// input-assembly/tessellation stages to configure at all).
	if (!is_mesh_pipeline && desc->tess_control_module && desc->tess_eval_module) {
		pipelineInfo.pTessellationState = &tessellationState;
	}

	if (vkCreateGraphicsPipelines(
	        vk_device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
	        &result._pipeline_vk) != VK_SUCCESS) {
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	// If all successful, return a heap-allocated copy.
	crgpu_shader_pipeline_vulkan *pResult =
	    CR_NEW(&result.head.allocator, crgpu_shader_pipeline_vulkan);
	if (!pResult) {
		vkDestroyPipeline(vk_device, result._pipeline_vk, NULL);
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(crgpu_shader_pipeline_vulkan));
	CRGPU___LIVE_ADD(&pResult->_device._shared->live, CRGPU___OBJECT_SHADER_PIPELINE);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_graphics_pipeline_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pipeline = pResult;
	return CR_SUCCESS;
}

void crgpu_graphics_pipeline_destroy(crgpu_graphics_pipeline pipeline) {
	if (!pipeline)
		return;
	if (CR_STALE(pipeline, "crgpu_graphics_pipeline_destroy", "pipeline"))
		return;
	crgpu_shader_pipeline_vulkan *vkPipeline =
	    (crgpu_shader_pipeline_vulkan *)pipeline;
	VkDevice vk_device = vkPipeline->_device._logical_device_vk;
	vkDestroyPipeline(vk_device, vkPipeline->_pipeline_vk, NULL);
	vkDestroyPipelineLayout(vk_device, vkPipeline->_pipeline_layout_vk, NULL);
	CRGPU___LIVE_REMOVE(&vkPipeline->_device._shared->live, CRGPU___OBJECT_SHADER_PIPELINE);
	CR_KILL(pipeline);
	const CR_ALLOCATOR allocator = vkPipeline->head.allocator;
	cr_string_free(&allocator, vkPipeline->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(vkPipeline), &allocator, crgpu_shader_pipeline_vulkan, vkPipeline);
}

CR_RESULT crgpu_get_shader_pipeline_desc(
    crgpu_graphics_pipeline pipeline, crgpu_graphics_pipeline_desc *out_desc) {
	if (!pipeline)
		return CR_RESULT_GPU_INVALID_GRAPHICS_PIPELINE;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = pipeline->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_GRAPHICS_PIPELINE_DESC);
	return CR_SUCCESS;
}

CR_RESULT crgpu_compute_pipeline_create(
    crgpu_device device,
    const crgpu_compute_pipeline_desc *desc,
    crgpu_compute_pipeline *out_pipeline) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_compute_pipeline_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!out_pipeline)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->compute_module)
		return CR_RESULT_GPU_INVALID_COMPUTE_MODULE;

	crgpu_compute_pipeline_vulkan result = {0};
	result.head.allocator                  = device->head.allocator;
	CR_BIRTH_IN(&result, &device->_shared->quarantine);
	result._device                         = *device;
	result._desc                           = *desc;

	VkDevice vk_device                 = result._device._logical_device_vk;
	crgpu_shader_module_vulkan *comp = desc->compute_module;

	// Pipeline layout
	VkPushConstantRange push_constant_ranges[CRGPU_MAX_PUSH_CONSTANT_RANGES] =
	    {0};
	for (uint32_t i = 0; i < desc->push_constant_range_count; i++) {
		push_constant_ranges[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_constant_ranges[i].offset = desc->push_constant_ranges[i].offset;
		push_constant_ranges[i].size   = desc->push_constant_ranges[i].size;
	}

	VkDescriptorSetLayout vk_layouts[CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	for (uint32_t i = 0; i < desc->descriptor_set_layout_count; i++) {
		crgpu_descriptor_set_layout_vulkan *vk_layout =
		    (crgpu_descriptor_set_layout_vulkan *)
		        desc->descriptor_set_layouts[i];
		vk_layouts[i] = vk_layout->_layout_vk;
	}

	VkPipelineLayoutCreateInfo layoutInfo = {0};
	layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = desc->descriptor_set_layout_count;
	layoutInfo.pSetLayouts    = vk_layouts;
	layoutInfo.pushConstantRangeCount = desc->push_constant_range_count;
	layoutInfo.pPushConstantRanges    = push_constant_ranges;

	if (vkCreatePipelineLayout(
	        vk_device, &layoutInfo, NULL, &result._pipeline_layout_vk) !=
	    VK_SUCCESS) {
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	// Compute pipeline
	VkComputePipelineCreateInfo pipelineInfo = {0};
	pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = result._pipeline_layout_vk;
	pipelineInfo.stage.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineInfo.stage.module = comp->_shader_module_vk;
	pipelineInfo.stage.pName =
	    comp->_entry_point;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
	pipelineInfo.basePipelineIndex  = -1;

	if (vkCreateComputePipelines(
	        vk_device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
	        &result._pipeline_vk) != VK_SUCCESS) {
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	crgpu_compute_pipeline_vulkan *pResult =
	    CR_NEW(&result.head.allocator, crgpu_compute_pipeline_vulkan);
	if (!pResult) {
		vkDestroyPipeline(vk_device, result._pipeline_vk, NULL);
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(crgpu_compute_pipeline_vulkan));
	CRGPU___LIVE_ADD(&pResult->_device._shared->live, CRGPU___OBJECT_COMPUTE_PIPELINE);
	if (cr_debug_name_set(pResult, desc->debug_name) != CR_SUCCESS) {
		crgpu_compute_pipeline_destroy(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pipeline = pResult;
	return CR_SUCCESS;
}

void crgpu_compute_pipeline_destroy(crgpu_compute_pipeline pipeline) {
	if (!pipeline)
		return;
	if (CR_STALE(pipeline, "crgpu_compute_pipeline_destroy", "pipeline"))
		return;
	crgpu_compute_pipeline_vulkan *vkPipeline =
	    (crgpu_compute_pipeline_vulkan *)pipeline;
	VkDevice vk_device = vkPipeline->_device._logical_device_vk;
	vkDestroyPipeline(vk_device, vkPipeline->_pipeline_vk, NULL);
	vkDestroyPipelineLayout(vk_device, vkPipeline->_pipeline_layout_vk, NULL);
	CRGPU___LIVE_REMOVE(&vkPipeline->_device._shared->live, CRGPU___OBJECT_COMPUTE_PIPELINE);
	CR_KILL(pipeline);
	const CR_ALLOCATOR allocator = vkPipeline->head.allocator;
	cr_string_free(&allocator, vkPipeline->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(vkPipeline), &allocator, crgpu_compute_pipeline_vulkan, vkPipeline);
}

CR_RESULT crgpu_compute_pipeline_get_desc(
    crgpu_compute_pipeline pipeline,
    crgpu_compute_pipeline_desc *out_desc) {
    if (!pipeline) return CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE;
    if (CR_STALE(pipeline, "crgpu_compute_pipeline_get_desc", "pipeline")) return CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE;
    if (!out_desc) return CR_RESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = pipeline->_desc;
    out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
    return CR_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // CRGPU_COMPILE_VULKAN_API
