
#if SPUDGPU_COMPILE_VULKAN_API

#include "spudgpu.h"
#include "spudgpuvulkan.h"
#include "../../spudgpushared.h"
#include <stdlib.h>
#include <string.h>
#include "../../../spudstructbase.h"

VkPrimitiveTopology
spudgpuvulkan___primitive_topology(SPUDGPU_PRIMITIVE_TOPOLOGY topology) {
	switch (topology) {
	case SPUDGPU_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	default:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

VkShaderStageFlagBits
spudgpuvulkan___shader_stage_flag_internal(SPUDGPU_SHADER_STAGE stage) {
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
		return VK_SHADER_STAGE_VERTEX_BIT;
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return VK_SHADER_STAGE_FRAGMENT_BIT;
	case SPUDGPU_SHADER_STAGE_COMPUTE:
		return VK_SHADER_STAGE_COMPUTE_BIT;
	case SPUDGPU_SHADER_STAGE_GEOMETRY:
		return VK_SHADER_STAGE_GEOMETRY_BIT;
	case SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	case SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	case SPUDGPU_SHADER_STAGE_MESH:
		return VK_SHADER_STAGE_MESH_BIT_EXT;
	case SPUDGPU_SHADER_STAGE_TASK:
		return VK_SHADER_STAGE_TASK_BIT_EXT;
	default:
		return (VkShaderStageFlagBits)0;
	}
}

VkFormat spudgpuvulkan___vertex_attr_format_internal(SPUDGPU_FORMAT fmt) {
	return convert_spud_to_vulkan_format((VkFormat)fmt);
}

VkResult spudgpuvulkan___create_shader_module_internal(
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
// SPUDGPU_BLEND_FACTOR as VkBlendFactor, whose numbering differs.
static VkBlendFactor spudgpuvulkan___blend_factor(SPUDGPU_BLEND_FACTOR factor) {
	switch (factor) {
	case SPUDGPU_BLEND_FACTOR_ZERO:
		return VK_BLEND_FACTOR_ZERO;
	case SPUDGPU_BLEND_FACTOR_ONE:
		return VK_BLEND_FACTOR_ONE;
	case SPUDGPU_BLEND_FACTOR_SRC_ALPHA:
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case SPUDGPU_BLEND_FACTOR_DST_ALPHA:
		return VK_BLEND_FACTOR_DST_ALPHA;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case SPUDGPU_BLEND_FACTOR_SRC_COLOR:
		return VK_BLEND_FACTOR_SRC_COLOR;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case SPUDGPU_BLEND_FACTOR_DST_COLOR:
		return VK_BLEND_FACTOR_DST_COLOR;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	default:
		return VK_BLEND_FACTOR_ZERO;
	}
}

static int spudgpuvulkan___format_has_stencil(VkFormat fmt) {
	switch (fmt) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return 1;
	default:
		return 0;
	}
}

VkShaderStageFlags spudgpuvulkan___shader_stages(SPUDGPU_SHADER_STAGE stages) {
	VkShaderStageFlags sf = 0;
	if (stages & SPUDGPU_SHADER_STAGE_VERTEX)
		sf |= VK_SHADER_STAGE_VERTEX_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_FRAGMENT)
		sf |= VK_SHADER_STAGE_FRAGMENT_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_COMPUTE)
		sf |= VK_SHADER_STAGE_COMPUTE_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_GEOMETRY)
		sf |= VK_SHADER_STAGE_GEOMETRY_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL)
		sf |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION)
		sf |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	if (stages & SPUDGPU_SHADER_STAGE_MESH)
		sf |= VK_SHADER_STAGE_MESH_BIT_EXT;
	if (stages & SPUDGPU_SHADER_STAGE_TASK)
		sf |= VK_SHADER_STAGE_TASK_BIT_EXT;
	return sf;
}

// Whether [words] (word_count of them) is SPIR-V with an OpEntryPoint named
// [name] for [stage]: SPUD_SUCCESS; SPUDRESULT_GPU_INVALID_SHADER_CODE for
// words that aren't SPIR-V (the magic word, or an instruction running past
// the end); SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND for none. Reads the
// header and walks the instructions as far as the entry points, which
// come first in a module's layout.
static SPUDRESULT spudgpuvulkan___find_entry_point(
    const uint32_t *words,
    size_t word_count,
    SPUDGPU_SHADER_STAGE stage,
    const char *name) {
	const uint32_t magic          = 0x07230203u;
	const uint32_t op_entry_point = 15;
	const uint32_t op_function    = 54;
	uint32_t model;
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
		model = 0;
		break;
	case SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		model = 1;
		break;
	case SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		model = 2;
		break;
	case SPUDGPU_SHADER_STAGE_GEOMETRY:
		model = 3;
		break;
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		model = 4;
		break;
	case SPUDGPU_SHADER_STAGE_COMPUTE:
		model = 5;
		break;
	case SPUDGPU_SHADER_STAGE_TASK:
		model = 5364; // TaskEXT
		break;
	default:
		model = 5365; // MeshEXT
		break;
	}
	if (word_count < 5 || words[0] != magic)
		return SPUDRESULT_GPU_INVALID_SHADER_CODE;
	const size_t name_length = strlen(name);
	size_t i                 = 5;
	while (i < word_count) {
		const uint32_t count  = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFFu;
		if (count == 0 || count > word_count - i)
			return SPUDRESULT_GPU_INVALID_SHADER_CODE;
		if (opcode == op_function)
			break; // Past the entry points.
		// OpEntryPoint: model, id, then the name, a nul-terminated string
		// packed into the words that follow.
		if (opcode == op_entry_point && count >= 4 && words[i + 1] == model) {
			const char *literal       = (const char *)&words[i + 3];
			const size_t literal_room = (size_t)(count - 3) * 4;
			if (name_length < literal_room && memcmp(literal, name, name_length + 1) == 0)
				return SPUD_SUCCESS;
		}
		i += count;
	}
	return SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND;
}

#if __cplusplus
extern "C" {

#endif

SPUDRESULT spudgpu_create_shader_module(
    spudgpu_device device,
    const spudgpu_shader_module_desc *desc,
    spudgpu_shader_module *out_module) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_shader_module", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_module)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	const SPUDRESULT desc_sr = spudgpu___internal_check_shader_module_desc(desc, device->_features);
	if (desc_sr != SPUD_SUCCESS)
		return desc_sr;
	const SPUDRESULT entry_sr = spudgpuvulkan___find_entry_point(
	    (const uint32_t *)desc->code, (size_t)(desc->code_size / 4), desc->stage, desc->entry_point);
	if (entry_sr != SPUD_SUCCESS)
		return entry_sr;

	spudgpu_shader_module_vulkan result = {0};
	result.head.allocator               = device->head.allocator;
	SPUD_BIRTH(&result);
	result._device                      = *((spudgpu_device_vulkan *)device);
	result._stage_vk = spudgpuvulkan___shader_stage_flag_internal(desc->stage);
	result._entry_point = spud_string_copy(&result.head.allocator, desc->entry_point);
	if (!result._entry_point)
		return SPUDRESULT_OUT_OF_MEMORY;

	// vkCreateShaderModule copies the words.
	if (spudgpuvulkan___create_shader_module_internal(
	        result._device._logical_device_vk,
	        (const uint32_t *)desc->code, desc->code_size,
	        &result._shader_module_vk) != VK_SUCCESS) {
		spud_string_free(&result.head.allocator, result._entry_point);
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	spudgpu_shader_module_vulkan *pResult =
	    SPUD_NEW(&result.head.allocator, spudgpu_shader_module_vulkan);
	if (!pResult) {
		vkDestroyShaderModule(result._device._logical_device_vk, result._shader_module_vk, NULL);
		spud_string_free(&result.head.allocator, result._entry_point);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(spudgpu_shader_module_vulkan));
	// From here spudgpu_destroy_shader_module frees it, and takes the count
	// back.
	SPUDGPU___LIVE_ADD(&pResult->_device._shared->live, SPUDGPU___OBJECT_SHADER_MODULE);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_shader_module(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}

	*out_module = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_shader_module(spudgpu_shader_module shader_module) {
	if (!shader_module)
		return;
	if (SPUD_STALE(shader_module, "spudgpu_destroy_shader_module", "shader_module"))
		return;
	vkDestroyShaderModule(
	    shader_module->_device._logical_device_vk,
	    shader_module->_shader_module_vk, NULL);
	SPUDGPU___LIVE_REMOVE(&shader_module->_device._shared->live, SPUDGPU___OBJECT_SHADER_MODULE);
	SPUD_KILL(shader_module);
	const SPUD_ALLOCATOR allocator = shader_module->head.allocator;
	spud_string_free(&allocator, shader_module->_entry_point);
	spud_string_free(&allocator, shader_module->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_shader_module_vulkan, shader_module);
}

SPUDRESULT spudgpu_create_shader_pipeline(
    spudgpu_device device,
    const spudgpu_shader_pipeline_desc *desc,
    spudgpu_shader_pipeline *out_pipeline) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_shader_pipeline", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_pipeline)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	// Validate: at minimum a fragment shader plus either a vertex module
	// (classic pipeline) or a mesh module (SPUDGPU_EXT_MESH_SHADING - mesh
	// pipelines have no vertex-fetch/input-assembly stage at all, see
	// spudgpu.h) is required. The two are mutually exclusive.
	if ((!desc->vertex_module && !desc->mesh_module) || !desc->fragment_module)
		return SPUDRESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED;
	if (desc->vertex_module && desc->mesh_module)
		return SPUDRESULT_GPU_INVALID_SHADER_STAGE;
	if (desc->mesh_module && !(device->_features & SPUDGPU_FEATURE_MESH_SHADING))
		return SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	SPUDRESULT outputs = spudgpu___internal_check_pipeline_outputs(desc, device->_features);
	if (outputs != SPUD_SUCCESS)
		return outputs;

	spudgpu_shader_pipeline_vulkan result = {0};
	result.head.allocator                 = device->head.allocator;
	SPUD_BIRTH(&result);
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
		spudgpu_shader_module_vulkan *vert =
		    (spudgpu_shader_module_vulkan *)desc->vertex_module;
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
		spudgpu_shader_module_vulkan *task =
		    (spudgpu_shader_module_vulkan *)desc->task_module;
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
		spudgpu_shader_module_vulkan *mesh =
		    (spudgpu_shader_module_vulkan *)desc->mesh_module;
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
		spudgpu_shader_module_vulkan *frag =
		    (spudgpu_shader_module_vulkan *)desc->fragment_module;
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
		spudgpu_shader_module_vulkan *geom =
		    (spudgpu_shader_module_vulkan *)desc->geometry_module;
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
		spudgpu_shader_module_vulkan *tc =
		    (spudgpu_shader_module_vulkan *)desc->tess_control_module;
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
		spudgpu_shader_module_vulkan *te =
		    (spudgpu_shader_module_vulkan *)desc->tess_eval_module;
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
	// We support up to SPUDGPU_MAX_VERTEX_ATTRIBUTES attributes described
	// in desc->vertex_attributes / desc->vertex_attribute_count.
	VkVertexInputAttributeDescription
	    attr_descs[SPUDGPU_MAX_VERTEX_ATTRIBUTES] = {0};
	for (uint32_t i = 0; i < desc->vertex_attribute_count; i++) {
		attr_descs[i].location = desc->vertex_attributes[i].location;
		attr_descs[i].binding  = desc->vertex_attributes[i].binding;
		attr_descs[i].format   = spudgpuvulkan___vertex_attr_format_internal(
		    desc->vertex_attributes[i].format);
		attr_descs[i].offset = desc->vertex_attributes[i].offset;
	}

	VkVertexInputBindingDescription binding_descs[SPUDGPU_MAX_VERTEX_BINDINGS] =
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
	    spudgpuvulkan___primitive_topology(desc->primitive_topology);
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
	// All three 0 is no bias (spudgpu.h). A clamp alone biases nothing.
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
	// (spudgpu.h).
	depthStencil.depthBoundsTestEnable =
	    desc->depth_bounds_test_enable && (device->_features & SPUDGPU_FEATURE_DEPTH_BOUNDS_TEST) ? VK_TRUE : VK_FALSE;
	depthStencil.stencilTestEnable     = VK_FALSE;

	// ------------------------------------------------------------------
	// 8. Colour blend attachments, one for each colour attachment, from
	// the caller's factors and ops (SPUDGPU_BLEND_OP is VkBlendOp's
	// numbering; SPUDGPU_BLEND_FACTOR is not VkBlendFactor's).
	// ------------------------------------------------------------------
	VkPipelineColorBlendAttachmentState colorBlendAttachments[SPUDGPU_MAX_COLOR_ATTACHMENTS] = {0};
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i) {
		const spudgpu_blend_attachment_desc *ba = &desc->blend_attachments[i];
		VkPipelineColorBlendAttachmentState *out = &colorBlendAttachments[i];
		out->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		out->blendEnable = ba->blend_enable ? VK_TRUE : VK_FALSE;
		if (ba->blend_enable) {
			out->srcColorBlendFactor = spudgpuvulkan___blend_factor(ba->src_color_blend_factor);
			out->dstColorBlendFactor = spudgpuvulkan___blend_factor(ba->dst_color_blend_factor);
			out->colorBlendOp        = (VkBlendOp)ba->color_blend_op;
			out->srcAlphaBlendFactor = spudgpuvulkan___blend_factor(ba->src_alpha_blend_factor);
			out->dstAlphaBlendFactor = spudgpuvulkan___blend_factor(ba->dst_alpha_blend_factor);
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

	VkPushConstantRange push_constant_ranges[SPUDGPU_MAX_PUSH_CONSTANT_RANGES] =
	    {0};
	for (uint32_t i = 0; i < desc->push_constant_range_count; i++) {
		push_constant_ranges[i].stageFlags = spudgpuvulkan___shader_stages(desc->push_constant_ranges[i].stage_flags);
		push_constant_ranges[i].offset = desc->push_constant_ranges[i].offset;
		push_constant_ranges[i].size   = desc->push_constant_ranges[i].size;
	}
	pipelineLayoutInfo.pushConstantRangeCount = desc->push_constant_range_count;
	pipelineLayoutInfo.pPushConstantRanges    = push_constant_ranges;

	VkDescriptorSetLayout vk_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	for (uint32_t i = 0; i < desc->descriptor_set_layout_count; i++) {
		spudgpu_descriptor_set_layout_vulkan *vk_layout =
		    (spudgpu_descriptor_set_layout_vulkan *)
		        desc->descriptor_set_layouts[i];
		vk_layouts[i] = vk_layout->_layout_vk;
	}

	pipelineLayoutInfo.setLayoutCount = desc->descriptor_set_layout_count;
	pipelineLayoutInfo.pSetLayouts    = vk_layouts;

	VkDevice vk_device = device->_logical_device_vk;

	if (vkCreatePipelineLayout(
	        vk_device, &pipelineLayoutInfo, NULL,
	        &result._pipeline_layout_vk) != VK_SUCCESS) {
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	// ------------------------------------------------------------------
	// 11. Dynamic rendering attachment formats — no VkRenderPass object.
	// Vulkan requires these to match whatever's actually bound at
	// spudgpu_cmd_begin_rendering time (see spudgpu.h's rendering section).
	// ------------------------------------------------------------------
	VkFormat color_formats[SPUDGPU_MAX_COLOR_ATTACHMENTS] = {0};
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i)
		color_formats[i] = convert_spud_to_vulkan_format(desc->color_attachment_formats[i]);
	VkFormat depth_format   = VK_FORMAT_UNDEFINED;
	VkFormat stencil_format = VK_FORMAT_UNDEFINED;
	if (desc->depth_format != SPUDGPU_FORMAT_UNKNOWN) {
		depth_format = convert_spud_to_vulkan_format((VkFormat)desc->depth_format);
		if (spudgpuvulkan___format_has_stencil(depth_format))
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
	// mesh shader emits geometry directly (see spudgpu.h's
	// SPUDGPU_EXT_MESH_SHADING section) - so these must stay NULL rather
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
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	// If all successful, return a heap-allocated copy.
	spudgpu_shader_pipeline_vulkan *pResult =
	    SPUD_NEW(&result.head.allocator, spudgpu_shader_pipeline_vulkan);
	if (!pResult) {
		vkDestroyPipeline(vk_device, result._pipeline_vk, NULL);
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(spudgpu_shader_pipeline_vulkan));
	SPUDGPU___LIVE_ADD(&pResult->_device._shared->live, SPUDGPU___OBJECT_SHADER_PIPELINE);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_shader_pipeline(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pipeline = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_shader_pipeline(spudgpu_shader_pipeline pipeline) {
	if (!pipeline)
		return;
	if (SPUD_STALE(pipeline, "spudgpu_destroy_shader_pipeline", "pipeline"))
		return;
	spudgpu_shader_pipeline_vulkan *vkPipeline =
	    (spudgpu_shader_pipeline_vulkan *)pipeline;
	VkDevice vk_device = vkPipeline->_device._logical_device_vk;
	vkDestroyPipeline(vk_device, vkPipeline->_pipeline_vk, NULL);
	vkDestroyPipelineLayout(vk_device, vkPipeline->_pipeline_layout_vk, NULL);
	SPUDGPU___LIVE_REMOVE(&vkPipeline->_device._shared->live, SPUDGPU___OBJECT_SHADER_PIPELINE);
	SPUD_KILL(pipeline);
	const SPUD_ALLOCATOR allocator = vkPipeline->head.allocator;
	spud_string_free(&allocator, vkPipeline->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_shader_pipeline_vulkan, vkPipeline);
}

SPUDRESULT spudgpu_get_shader_pipeline_desc(
    spudgpu_shader_pipeline pipeline, spudgpu_shader_pipeline_desc *out_desc) {
	if (!pipeline)
		return SPUDRESULT_GPU_INVALID_SHADER_PIPELINE;
	if (!out_desc)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = pipeline->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC);
	return SPUD_SUCCESS;
}

SPUDRESULT spudgpu_create_compute_pipeline(
    spudgpu_device device,
    const spudgpu_compute_pipeline_desc *desc,
    spudgpu_compute_pipeline *out_pipeline) {
	if (!device)
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_compute_pipeline", "device"))
		return SPUDRESULT_GPU_INVALID_DEVICE;
	if (!desc)
		return SPUDRESULT_NULL_DESC;
	const SPUDRESULT base_sr = spud_struct_check(&desc->base, SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
	if (base_sr != SPUD_SUCCESS)
		return base_sr;
	if (!out_pipeline)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	if (!desc->compute_module)
		return SPUDRESULT_GPU_INVALID_COMPUTE_MODULE;

	spudgpu_compute_pipeline_vulkan result = {0};
	result.head.allocator                  = device->head.allocator;
	SPUD_BIRTH(&result);
	result._device                         = *device;
	result._desc                           = *desc;

	VkDevice vk_device                 = result._device._logical_device_vk;
	spudgpu_shader_module_vulkan *comp = desc->compute_module;

	// Pipeline layout
	VkPushConstantRange push_constant_ranges[SPUDGPU_MAX_PUSH_CONSTANT_RANGES] =
	    {0};
	for (uint32_t i = 0; i < desc->push_constant_range_count; i++) {
		push_constant_ranges[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		push_constant_ranges[i].offset = desc->push_constant_ranges[i].offset;
		push_constant_ranges[i].size   = desc->push_constant_ranges[i].size;
	}

	VkDescriptorSetLayout vk_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	for (uint32_t i = 0; i < desc->descriptor_set_layout_count; i++) {
		spudgpu_descriptor_set_layout_vulkan *vk_layout =
		    (spudgpu_descriptor_set_layout_vulkan *)
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
		return SPUDRESULT_API_SPECIFIC_FAILURE;
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
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	spudgpu_compute_pipeline_vulkan *pResult =
	    SPUD_NEW(&result.head.allocator, spudgpu_compute_pipeline_vulkan);
	if (!pResult) {
		vkDestroyPipeline(vk_device, result._pipeline_vk, NULL);
		vkDestroyPipelineLayout(vk_device, result._pipeline_layout_vk, NULL);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(spudgpu_compute_pipeline_vulkan));
	SPUDGPU___LIVE_ADD(&pResult->_device._shared->live, SPUDGPU___OBJECT_COMPUTE_PIPELINE);
	if (spud_debug_name_set(pResult, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_compute_pipeline(pResult);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pResult->_desc.debug_name = pResult->head.debug_name;

	*out_pipeline = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_compute_pipeline(spudgpu_compute_pipeline pipeline) {
	if (!pipeline)
		return;
	if (SPUD_STALE(pipeline, "spudgpu_destroy_compute_pipeline", "pipeline"))
		return;
	spudgpu_compute_pipeline_vulkan *vkPipeline =
	    (spudgpu_compute_pipeline_vulkan *)pipeline;
	VkDevice vk_device = vkPipeline->_device._logical_device_vk;
	vkDestroyPipeline(vk_device, vkPipeline->_pipeline_vk, NULL);
	vkDestroyPipelineLayout(vk_device, vkPipeline->_pipeline_layout_vk, NULL);
	SPUDGPU___LIVE_REMOVE(&vkPipeline->_device._shared->live, SPUDGPU___OBJECT_COMPUTE_PIPELINE);
	SPUD_KILL(pipeline);
	const SPUD_ALLOCATOR allocator = vkPipeline->head.allocator;
	spud_string_free(&allocator, vkPipeline->head.debug_name);
	SPUD_DELETE(&allocator, spudgpu_compute_pipeline_vulkan, vkPipeline);
}

SPUDRESULT spudgpu_get_compute_pipeline_desc(
    spudgpu_compute_pipeline pipeline,
    spudgpu_compute_pipeline_desc *out_desc) {
    if (!pipeline) return SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE;
    if (SPUD_STALE(pipeline, "spudgpu_get_compute_pipeline_desc", "pipeline")) return SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE;
    if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
    *out_desc = pipeline->_desc;
    out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
    return SPUD_SUCCESS;
}

#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
