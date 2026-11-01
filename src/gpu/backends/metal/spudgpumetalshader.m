//
// SpudGPU Metal backend - shader modules and pipeline state. A module is a
// .metallib the caller built (spudgpu.h, spudgpu_shader_module_desc):
// nothing is compiled here.
//

#if SPUDGPU_COMPILE_METAL_API

#include "spudgpu.h"
#include "spudcore.h"
#include "spudgpumetal.h"
#include "../../spudgpushared.h"
#include <Metal/Metal.h>
#include "../../../spudstructbase.h"

// Whether [function]'s type is [stage]'s: Metal's vertex, fragment, kernel,
// mesh and object functions. A tessellation control stage is a kernel on
// Metal and an evaluation stage a post-tessellation vertex function; Metal
// has no geometry stage, so no function is one.
static bool spudgpumetal___internal_function_is_stage(id<MTLFunction> function, SPUDGPU_SHADER_STAGE stage) {
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
	case SPUDGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		return function.functionType == MTLFunctionTypeVertex;
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return function.functionType == MTLFunctionTypeFragment;
	case SPUDGPU_SHADER_STAGE_COMPUTE:
	case SPUDGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		return function.functionType == MTLFunctionTypeKernel;
	case SPUDGPU_SHADER_STAGE_MESH:
		return function.functionType == MTLFunctionTypeMesh;
	case SPUDGPU_SHADER_STAGE_TASK:
		return function.functionType == MTLFunctionTypeObject;
	default:
		return false;
	}
}

// The descriptor sets [bindings] - a function's bindings in a pipeline's
// reflection - read: bit n for each buffer argument it uses at set n's
// argument buffer index, and SPUDGPU_METAL_PUSH_CONSTANTS_MASK_BIT when it
// uses the push constants'. All bits when [bindings] is nil.
static uint32_t spudgpumetal___internal_set_mask(NSArray<id<MTLBinding>> *bindings) {
	if (!bindings)
		return 0xffffffffu;
	uint32_t mask = 0;
	for (id<MTLBinding> binding in bindings) {
		if (binding.type != MTLBindingTypeBuffer || !binding.used)
			continue;
		NSUInteger index = binding.index;
		if (index >= SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE &&
		    index < SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
			mask |= 1u << (uint32_t)(index - SPUDGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE);
		else if (index == SPUDGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX)
			mask |= SPUDGPU_METAL_PUSH_CONSTANTS_MASK_BIT;
	}
	return mask;
}

// -----------------------------------------------------------------------
// Shader modules
// -----------------------------------------------------------------------

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
	if (desc->code_size > SIZE_MAX)
		return SPUDRESULT_OUT_OF_MEMORY;

	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	SPUDRESULT sr                      = SPUD_SUCCESS;

	spudgpu_shader_module_metal *module_metal = SPUD_NEW_ZEROED(&device_metal->head.allocator, spudgpu_shader_module_metal);
	if (!module_metal)
		return SPUDRESULT_OUT_OF_MEMORY;
	module_metal->head.allocator = device_metal->head.allocator;
	SPUD_BIRTH(module_metal);
	module_metal->_parent_device = device_metal;
	module_metal->_desc          = *desc;
	// The caller's pointers are borrowed for the call: neither is kept.
	module_metal->_desc.code        = NULL;
	module_metal->_desc.entry_point = NULL;
	module_metal->_workgroup_size_x = desc->workgroup_size[0];
	module_metal->_workgroup_size_y = desc->workgroup_size[1];
	module_metal->_workgroup_size_z = desc->workgroup_size[2];

	// The pool's block ends before any jump to the cleanup: leaving it by a
	// goto would leave the pool unbalanced.
	@autoreleasepool {
		// The default destructor makes dispatch_data copy the bytes, so the
		// library never reads the caller's memory after this call.
		dispatch_data_t data = dispatch_data_create(desc->code, (size_t)desc->code_size, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		if (!data) {
			sr = SPUDRESULT_OUT_OF_MEMORY;
		} else {
			NSError *error             = nil;
			module_metal->_library_mtl = [device_metal->_device_mtl newLibraryWithData:data error:&error];
			dispatch_release(data);
			if (!module_metal->_library_mtl)
				sr = SPUDRESULT_GPU_INVALID_SHADER_CODE;
		}
		if (sr == SPUD_SUCCESS) {
			NSString *name = [NSString stringWithUTF8String:desc->entry_point];
			if (name)
				module_metal->_function_mtl = [module_metal->_library_mtl newFunctionWithName:name];
			if (!module_metal->_function_mtl ||
			    !spudgpumetal___internal_function_is_stage(module_metal->_function_mtl, desc->stage))
				sr = SPUDRESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND;
		}
	}
	if (sr != SPUD_SUCCESS)
		goto failedattempt;

	// From here spudgpu_destroy_shader_module frees it, and takes the count
	// back.
	SPUDGPU___LIVE_ADD(&device_metal->_live, SPUDGPU___OBJECT_SHADER_MODULE);
	if (spud_debug_name_set(module_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_shader_module((spudgpu_shader_module)module_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	module_metal->_desc.debug_name = module_metal->head.debug_name;

	*out_module = (spudgpu_shader_module)module_metal;

	return sr;

failedattempt:
	{
		if (module_metal->_function_mtl) {
			[module_metal->_function_mtl release];
		}
		if (module_metal->_library_mtl) {
			[module_metal->_library_mtl release];
		}
		SPUD_DELETE(&device_metal->head.allocator, spudgpu_shader_module_metal, module_metal);
	}
	return sr;
}

void spudgpu_destroy_shader_module(spudgpu_shader_module shader_module) {
	spudgpu_shader_module_metal *module_metal = (spudgpu_shader_module_metal *)shader_module;
	if (module_metal) {
		if (SPUD_STALE(shader_module, "spudgpu_destroy_shader_module", "shader_module"))
			return;
		if (module_metal->_function_mtl) {
			[module_metal->_function_mtl release];
		}
		if (module_metal->_library_mtl) {
			[module_metal->_library_mtl release];
		}
		SPUDGPU___LIVE_REMOVE(&module_metal->_parent_device->_live, SPUDGPU___OBJECT_SHADER_MODULE);
		SPUD_KILL(shader_module);
		const SPUD_ALLOCATOR allocator = module_metal->head.allocator;
		spud_string_free(&allocator, module_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_shader_module_metal, module_metal);
	}
}

// -----------------------------------------------------------------------
// Render (shader) pipelines
// -----------------------------------------------------------------------

// The class of primitive a topology draws. Patches have none here, for the
// reason spudgpumetal___internal_primitive_type gives, and fall back the
// same way.
static MTLPrimitiveTopologyClass spudgpumetal___internal_topology_class(SPUDGPU_PRIMITIVE_TOPOLOGY topology) {
	switch (topology) {
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return MTLPrimitiveTopologyClassTriangle;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTopologyClassTriangle;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_LIST:      return MTLPrimitiveTopologyClassLine;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTopologyClassLine;
	case SPUDGPU_PRIMITIVE_TOPOLOGY_POINT_LIST:     return MTLPrimitiveTopologyClassPoint;
	default:                                        return MTLPrimitiveTopologyClassTriangle;
	}
}

static MTLCompareFunction spudgpumetal___internal_compare_function(SPUDGPU_COMPARE_OP op) {
	switch (op) {
	case SPUDGPU_COMPARE_OP_NEVER:            return MTLCompareFunctionNever;
	case SPUDGPU_COMPARE_OP_LESS:             return MTLCompareFunctionLess;
	case SPUDGPU_COMPARE_OP_EQUAL:            return MTLCompareFunctionEqual;
	case SPUDGPU_COMPARE_OP_LESS_OR_EQUAL:    return MTLCompareFunctionLessEqual;
	case SPUDGPU_COMPARE_OP_GREATER:          return MTLCompareFunctionGreater;
	case SPUDGPU_COMPARE_OP_NOT_EQUAL:        return MTLCompareFunctionNotEqual;
	case SPUDGPU_COMPARE_OP_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
	default:                                  return MTLCompareFunctionAlways;
	}
}

static MTLBlendFactor spudgpumetal___internal_blend_factor(SPUDGPU_BLEND_FACTOR factor) {
	switch (factor) {
	case SPUDGPU_BLEND_FACTOR_ONE:                 return MTLBlendFactorOne;
	case SPUDGPU_BLEND_FACTOR_SRC_ALPHA:           return MTLBlendFactorSourceAlpha;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
	case SPUDGPU_BLEND_FACTOR_DST_ALPHA:           return MTLBlendFactorDestinationAlpha;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
	case SPUDGPU_BLEND_FACTOR_SRC_COLOR:           return MTLBlendFactorSourceColor;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
	case SPUDGPU_BLEND_FACTOR_DST_COLOR:           return MTLBlendFactorDestinationColor;
	case SPUDGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
	default:                                       return MTLBlendFactorZero;
	}
}

static MTLBlendOperation spudgpumetal___internal_blend_operation(SPUDGPU_BLEND_OP op) {
	switch (op) {
	case SPUDGPU_BLEND_OP_SUBTRACT:         return MTLBlendOperationSubtract;
	case SPUDGPU_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
	case SPUDGPU_BLEND_OP_MIN:              return MTLBlendOperationMin;
	case SPUDGPU_BLEND_OP_MAX:              return MTLBlendOperationMax;
	default:                                return MTLBlendOperationAdd;
	}
}

// The pipeline's colour attachments, each with its format and blend state
// (spudgpu_shader_pipeline_desc::color_attachment_formats and
// blend_attachments). Every format has been checked.
static void spudgpumetal___internal_set_color_attachments(
    MTLRenderPipelineColorAttachmentDescriptorArray *attachments,
    const spudgpu_shader_pipeline_desc *desc) {
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment = attachments[i];
		const spudgpu_blend_attachment_desc *blend             = &desc->blend_attachments[i];
		attachment.pixelFormat     = spudgpumetal___format_info(desc->color_attachment_formats[i])->pixel;
		attachment.blendingEnabled = blend->blend_enable;
		if (blend->blend_enable) {
			attachment.sourceRGBBlendFactor        = spudgpumetal___internal_blend_factor(blend->src_color_blend_factor);
			attachment.destinationRGBBlendFactor   = spudgpumetal___internal_blend_factor(blend->dst_color_blend_factor);
			attachment.rgbBlendOperation           = spudgpumetal___internal_blend_operation(blend->color_blend_op);
			attachment.sourceAlphaBlendFactor      = spudgpumetal___internal_blend_factor(blend->src_alpha_blend_factor);
			attachment.destinationAlphaBlendFactor = spudgpumetal___internal_blend_factor(blend->dst_alpha_blend_factor);
			attachment.alphaBlendOperation         = spudgpumetal___internal_blend_operation(blend->alpha_blend_op);
		}
	}
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
	if ((!desc->vertex_module && !desc->mesh_module) || !desc->fragment_module)
		return SPUDRESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED;
	if (desc->vertex_module && desc->mesh_module)
		return SPUDRESULT_GPU_INVALID_SHADER_STAGE; // Mutually exclusive - see spudgpu.h.
	if (desc->mesh_module && !(device->_features & SPUDGPU_FEATURE_MESH_SHADING))
		return SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	SPUDRESULT outputs = spudgpu___internal_check_pipeline_outputs(desc, device->_features);
	if (outputs != SPUD_SUCCESS)
		return outputs;
	// Geometry and tessellation stages were refused just above
	// (SPUDGPU_EXT_GEOMETRY_SHADER and SPUDGPU_EXT_TESSELLATION are 0 here,
	// so no Metal device has their bits): Metal has no geometry stage, and
	// its tessellation is post-tessellation vertex functions and
	// -drawPatches:, not a control and evaluation stage pair.

	spudgpu_shader_module_metal *vertex_module_metal   = (spudgpu_shader_module_metal *)desc->vertex_module;
	spudgpu_shader_module_metal *mesh_module_metal      = (spudgpu_shader_module_metal *)desc->mesh_module;
	spudgpu_shader_module_metal *fragment_module_metal = (spudgpu_shader_module_metal *)desc->fragment_module;
	if (vertex_module_metal && !vertex_module_metal->_function_mtl)
		return SPUDRESULT_GPU_INVALID_SHADER_MODULE;
	if (mesh_module_metal && !mesh_module_metal->_function_mtl)
		return SPUDRESULT_GPU_INVALID_SHADER_MODULE;
	if (!fragment_module_metal->_function_mtl)
		return SPUDRESULT_GPU_INVALID_SHADER_MODULE;

	// Attachment formats from the format table, as the images rendered to
	// were made: a pipeline and the textures of its pass always agree.
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i)
		if (spudgpumetal___format_info(desc->color_attachment_formats[i])->pixel == MTLPixelFormatInvalid)
			return SPUDRESULT_GPU_INVALID_FORMAT;
	MTLPixelFormat depth_format = MTLPixelFormatInvalid;
	// A depth format with a stencil aspect is the stencil attachment too.
	bool depth_has_stencil = false;
	if (desc->depth_format != SPUDGPU_FORMAT_UNKNOWN) {
		const spudgpumetal_format_info *depth_info = spudgpumetal___format_info(desc->depth_format);
		depth_format                               = depth_info->pixel;
		if (depth_format == MTLPixelFormatInvalid)
			return SPUDRESULT_GPU_INVALID_FORMAT;
		depth_has_stencil = (depth_info->aspects & SPUDGPU_IMAGE_ASPECT_STENCIL) != 0;
	}

	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	SPUDRESULT sr                      = SPUD_SUCCESS;

	spudgpu_shader_pipeline_metal *pipeline_metal = SPUD_NEW_ZEROED(&device_metal->head.allocator, spudgpu_shader_pipeline_metal);
	if (!pipeline_metal) {
		sr = SPUDRESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	pipeline_metal->head.allocator = device_metal->head.allocator;
	SPUD_BIRTH(pipeline_metal);
	pipeline_metal->_parent_device = device_metal;
	pipeline_metal->_desc          = *desc;

	if (mesh_module_metal) {
		// Mesh-shader pipeline: no vertex input state/input-assembler at
		// all - the mesh function supplies its own geometry. See
		// spudgpu_shader_pipeline_desc::mesh_module in spudgpu.h.
		MTLMeshRenderPipelineDescriptor *pipeline_desc = [[MTLMeshRenderPipelineDescriptor alloc] init];
		pipeline_desc.meshFunction                     = mesh_module_metal->_function_mtl;
		pipeline_desc.fragmentFunction                 = fragment_module_metal->_function_mtl;
		spudgpumetal___internal_set_color_attachments(pipeline_desc.colorAttachments, desc);

		if (depth_format != MTLPixelFormatInvalid) {
			pipeline_desc.depthAttachmentPixelFormat = depth_format;
			if (depth_has_stencil) {
				pipeline_desc.stencilAttachmentPixelFormat = depth_format;
			}
		}

		NSError *pipeline_error                 = nil;
		MTLRenderPipelineReflection *reflection = nil;
		pipeline_metal->_render_pipeline_state_mtl = [device_metal->_device_mtl
		    newRenderPipelineStateWithMeshDescriptor:pipeline_desc
		                                      options:MTLPipelineOptionBindingInfo
		                                   reflection:&reflection
		                                        error:&pipeline_error];
		// Autoreleased: read now, before anything can release it.
		pipeline_metal->_non_fragment_set_mask =
		    reflection ? (spudgpumetal___internal_set_mask(reflection.meshBindings) | spudgpumetal___internal_set_mask(reflection.objectBindings))
		               : 0xffffffffu;
		pipeline_metal->_fragment_set_mask = reflection ? spudgpumetal___internal_set_mask(reflection.fragmentBindings) : 0xffffffffu;
		[pipeline_desc release];
		if (!pipeline_metal->_render_pipeline_state_mtl) {
			printf(
			    "spudgpu: MTLRenderPipelineState (mesh) creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = SPUDRESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}

		pipeline_metal->_is_mesh_pipeline    = true;
		pipeline_metal->_mesh_local_size_x   = mesh_module_metal->_workgroup_size_x;
		pipeline_metal->_mesh_local_size_y   = mesh_module_metal->_workgroup_size_y;
		pipeline_metal->_mesh_local_size_z   = mesh_module_metal->_workgroup_size_z;
	} else {
		// descriptor_set_layouts is intentionally unconsumed here: unlike
		// Vulkan's VkPipelineLayout or D3D12's ID3D12RootSignature, building
		// an MTLRenderPipelineState needs no resource-layout object at all -
		// the (set, binding) -> flat-argument-index scheme lives entirely in
		// the already-cross-compiled MSL source (SPIRV-Cross's own default
		// assignment, for now - see spudgpumetaldescriptors.m, which will
		// need to agree with whatever scheme is used once it's implemented).
		MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
		for (uint32_t i = 0; i < desc->vertex_attribute_count; i++) {
			const spudgpu_vertex_attribute_desc *a = &desc->vertex_attributes[i];
			MTLVertexAttributeDescriptor *attr     = vertex_desc.attributes[a->location];
			attr.format                            = spudgpumetal___format_info(a->format)->vertex;
			attr.offset                            = a->offset;
			attr.bufferIndex                       = a->binding;
		}
		for (uint32_t i = 0; i < desc->vertex_binding_count; i++) {
			const spudgpu_vertex_binding_desc *b   = &desc->vertex_bindings[i];
			MTLVertexBufferLayoutDescriptor *layout = vertex_desc.layouts[b->binding];
			layout.stride                           = b->stride;
			layout.stepFunction = b->per_instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
			layout.stepRate     = 1;
		}

		MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
		pipeline_desc.vertexFunction               = vertex_module_metal->_function_mtl;
		pipeline_desc.fragmentFunction              = fragment_module_metal->_function_mtl;
		pipeline_desc.vertexDescriptor              = vertex_desc;
		// Metal refuses a pipeline in a layered pass
		// (spudgpu_rendering_begin_desc::layer_count) unless this is given,
		// and a pipeline isn't created against a pass, so it is always given.
		pipeline_desc.inputPrimitiveTopology = spudgpumetal___internal_topology_class(desc->primitive_topology);
		spudgpumetal___internal_set_color_attachments(pipeline_desc.colorAttachments, desc);

		if (depth_format != MTLPixelFormatInvalid) {
			pipeline_desc.depthAttachmentPixelFormat = depth_format;
			// A depth and stencil texture (Depth32Float_Stencil8, the only
			// one the format table has) is the stencil attachment too.
			if (depth_has_stencil) {
				pipeline_desc.stencilAttachmentPixelFormat = depth_format;
			}
		}

		NSError *pipeline_error                     = nil;
		MTLRenderPipelineReflection *reflection     = nil;
		pipeline_metal->_render_pipeline_state_mtl  = [device_metal->_device_mtl
		    newRenderPipelineStateWithDescriptor:pipeline_desc
		                                 options:MTLPipelineOptionBindingInfo
		                              reflection:&reflection
		                                    error:&pipeline_error];
		pipeline_metal->_non_fragment_set_mask = reflection ? spudgpumetal___internal_set_mask(reflection.vertexBindings) : 0xffffffffu;
		pipeline_metal->_fragment_set_mask     = reflection ? spudgpumetal___internal_set_mask(reflection.fragmentBindings) : 0xffffffffu;
		[pipeline_desc release];
		if (!pipeline_metal->_render_pipeline_state_mtl) {
			printf(
			    "spudgpu: MTLRenderPipelineState creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = SPUDRESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
	}

	{
		// Vulkan/D3D12 bake depth/stencil test config into the same
		// monolithic pipeline object as everything above; Metal keeps it as
		// a wholly separate native object - see spudgpu_cmd_bind_pipeline in
		// spudgpumetalcommand.m, which binds both together.
		MTLDepthStencilDescriptor *depth_stencil_desc = [[MTLDepthStencilDescriptor alloc] init];
		depth_stencil_desc.depthCompareFunction =
		    desc->depth_test_enable ? spudgpumetal___internal_compare_function(desc->depth_compare_op) : MTLCompareFunctionAlways;
		depth_stencil_desc.depthWriteEnabled = desc->depth_test_enable && desc->depth_write_enable;
		pipeline_metal->_depth_stencil_state_mtl =
		    [device_metal->_device_mtl newDepthStencilStateWithDescriptor:depth_stencil_desc];
		[depth_stencil_desc release];
		if (!pipeline_metal->_depth_stencil_state_mtl) {
			sr = SPUDRESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
	}

	SPUDGPU___LIVE_ADD(&device_metal->_live, SPUDGPU___OBJECT_SHADER_PIPELINE);
	if (spud_debug_name_set(pipeline_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_shader_pipeline((spudgpu_shader_pipeline)pipeline_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pipeline_metal->_desc.debug_name = pipeline_metal->head.debug_name;

	*out_pipeline = (spudgpu_shader_pipeline)pipeline_metal;

	return sr;

failedattempt:
	if (pipeline_metal) {
		if (pipeline_metal->_depth_stencil_state_mtl) {
			[pipeline_metal->_depth_stencil_state_mtl release];
		}
		if (pipeline_metal->_render_pipeline_state_mtl) {
			[pipeline_metal->_render_pipeline_state_mtl release];
		}
		SPUD_DELETE(&device_metal->head.allocator, spudgpu_shader_pipeline_metal, pipeline_metal);
	}
	return sr;
}

void spudgpu_destroy_shader_pipeline(spudgpu_shader_pipeline pipeline) {
	spudgpu_shader_pipeline_metal *pipeline_metal = (spudgpu_shader_pipeline_metal *)pipeline;
	if (pipeline_metal) {
		if (SPUD_STALE(pipeline, "spudgpu_destroy_shader_pipeline", "pipeline"))
			return;
		if (pipeline_metal->_depth_stencil_state_mtl) {
			[pipeline_metal->_depth_stencil_state_mtl release];
		}
		if (pipeline_metal->_render_pipeline_state_mtl) {
			[pipeline_metal->_render_pipeline_state_mtl release];
		}
		SPUDGPU___LIVE_REMOVE(&pipeline_metal->_parent_device->_live, SPUDGPU___OBJECT_SHADER_PIPELINE);
		SPUD_KILL(pipeline);
		const SPUD_ALLOCATOR allocator = pipeline_metal->head.allocator;
		spud_string_free(&allocator, pipeline_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_shader_pipeline_metal, pipeline_metal);
	}
}

// -----------------------------------------------------------------------
// Compute pipelines
// -----------------------------------------------------------------------

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

	spudgpu_shader_module_metal *compute_module_metal = (spudgpu_shader_module_metal *)desc->compute_module;
	if (!compute_module_metal->_function_mtl)
		return SPUDRESULT_GPU_INVALID_SHADER_MODULE;

	spudgpu_device_metal *device_metal = (spudgpu_device_metal *)device;
	SPUDRESULT sr                      = SPUD_SUCCESS;

	spudgpu_compute_pipeline_metal *pipeline_metal = SPUD_NEW_ZEROED(&device_metal->head.allocator, spudgpu_compute_pipeline_metal);
	if (!pipeline_metal) {
		sr = SPUDRESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	pipeline_metal->head.allocator = device_metal->head.allocator;
	SPUD_BIRTH(pipeline_metal);
	pipeline_metal->_parent_device = device_metal;
	pipeline_metal->_desc          = *desc;
	pipeline_metal->_local_size_x  = compute_module_metal->_workgroup_size_x;
	pipeline_metal->_local_size_y  = compute_module_metal->_workgroup_size_y;
	pipeline_metal->_local_size_z  = compute_module_metal->_workgroup_size_z;

	{
		// Same reasoning as spudgpu_create_shader_pipeline above -
		// descriptor_set_layouts needs no consumption here either; a
		// MTLComputePipelineState needs no resource-layout object.
		NSError *pipeline_error = nil;
		pipeline_metal->_compute_pipeline_state_mtl =
		    [device_metal->_device_mtl newComputePipelineStateWithFunction:compute_module_metal->_function_mtl
		                                                              error:&pipeline_error];
		if (!pipeline_metal->_compute_pipeline_state_mtl) {
			printf(
			    "spudgpu: MTLComputePipelineState creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = SPUDRESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
	}

	SPUDGPU___LIVE_ADD(&device_metal->_live, SPUDGPU___OBJECT_COMPUTE_PIPELINE);
	if (spud_debug_name_set(pipeline_metal, desc->debug_name) != SPUD_SUCCESS) {
		spudgpu_destroy_compute_pipeline((spudgpu_compute_pipeline)pipeline_metal);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pipeline_metal->_desc.debug_name = pipeline_metal->head.debug_name;

	*out_pipeline = (spudgpu_compute_pipeline)pipeline_metal;

	return sr;

failedattempt:
	if (pipeline_metal) {
		if (pipeline_metal->_compute_pipeline_state_mtl) {
			[pipeline_metal->_compute_pipeline_state_mtl release];
		}
		SPUD_DELETE(&device_metal->head.allocator, spudgpu_compute_pipeline_metal, pipeline_metal);
	}
	return sr;
}

void spudgpu_destroy_compute_pipeline(spudgpu_compute_pipeline pipeline) {
	spudgpu_compute_pipeline_metal *pipeline_metal = (spudgpu_compute_pipeline_metal *)pipeline;
	if (pipeline_metal) {
		if (SPUD_STALE(pipeline, "spudgpu_destroy_compute_pipeline", "pipeline"))
			return;
		if (pipeline_metal->_compute_pipeline_state_mtl) {
			[pipeline_metal->_compute_pipeline_state_mtl release];
		}
		SPUDGPU___LIVE_REMOVE(&pipeline_metal->_parent_device->_live, SPUDGPU___OBJECT_COMPUTE_PIPELINE);
		SPUD_KILL(pipeline);
		const SPUD_ALLOCATOR allocator = pipeline_metal->head.allocator;
		spud_string_free(&allocator, pipeline_metal->head.debug_name);
		SPUD_DELETE(&allocator, spudgpu_compute_pipeline_metal, pipeline_metal);
	}
}

SPUDRESULT spudgpu_get_compute_pipeline_desc(
    spudgpu_compute_pipeline pipeline,
    spudgpu_compute_pipeline_desc *out_desc) {
	spudgpu_compute_pipeline_metal *pipeline_metal = (spudgpu_compute_pipeline_metal *)pipeline;
	if (!pipeline_metal)
		return SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE;
	if (SPUD_STALE(pipeline, "spudgpu_get_compute_pipeline_desc", "pipeline"))
		return SPUDRESULT_GPU_INVALID_COMPUTE_PIPELINE;
	if (!out_desc)
		return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = pipeline_metal->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
	return SPUD_SUCCESS;
}

#endif // SPUDGPU_COMPILE_METAL_API
