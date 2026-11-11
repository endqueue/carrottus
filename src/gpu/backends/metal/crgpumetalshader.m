//
// Carrottus GPU Metal backend - shader modules and pipeline state. A module is a
// .metallib the caller built (crgpu.h, crgpu_shader_module_desc):
// nothing is compiled here.
//

#if CRGPU_COMPILE_METAL_API

#include "crgpu.h"
#include "crcore.h"
#include "crgpumetal.h"
#include "../../crgpushared.h"
#include <Metal/Metal.h>
#include "../../../crstructbase.h"

// Whether [function]'s type is [stage]'s: Metal's vertex, fragment, kernel,
// mesh and object functions. A tessellation control stage is a kernel on
// Metal and an evaluation stage a post-tessellation vertex function; Metal
// has no geometry stage, so no function is one.
static bool crgpumetal___internal_function_is_stage(id<MTLFunction> function, CRGPU_SHADER_STAGE stage) {
	switch (stage) {
	case CRGPU_SHADER_STAGE_VERTEX:
	case CRGPU_SHADER_STAGE_TESSELLATION_EVALUATION:
		return function.functionType == MTLFunctionTypeVertex;
	case CRGPU_SHADER_STAGE_FRAGMENT:
		return function.functionType == MTLFunctionTypeFragment;
	case CRGPU_SHADER_STAGE_COMPUTE:
	case CRGPU_SHADER_STAGE_TESSELLATION_CONTROL:
		return function.functionType == MTLFunctionTypeKernel;
	case CRGPU_SHADER_STAGE_MESH:
		return function.functionType == MTLFunctionTypeMesh;
	case CRGPU_SHADER_STAGE_TASK:
		return function.functionType == MTLFunctionTypeObject;
	default:
		return false;
	}
}

// The descriptor sets [bindings] - a function's bindings in a pipeline's
// reflection - read: bit n for each buffer argument it uses at set n's
// argument buffer index, and CRGPU_METAL_PUSH_CONSTANTS_MASK_BIT when it
// uses the push constants'. All bits when [bindings] is nil.
static uint32_t crgpumetal___internal_set_mask(NSArray<id<MTLBinding>> *bindings) {
	if (!bindings)
		return 0xffffffffu;
	uint32_t mask = 0;
	for (id<MTLBinding> binding in bindings) {
		if (binding.type != MTLBindingTypeBuffer || !binding.used)
			continue;
		NSUInteger index = binding.index;
		if (index >= CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE &&
		    index < CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
			mask |= 1u << (uint32_t)(index - CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE);
		else if (index == CRGPU_METAL_PUSH_CONSTANTS_BUFFER_INDEX)
			mask |= CRGPU_METAL_PUSH_CONSTANTS_MASK_BIT;
	}
	return mask;
}

// The runs of a function's argument buffer for one set, [type] from a
// pipeline's reflection, into [ranges] (CRGPU_METAL_MAX_SET_RANGES of
// them), normalized. Returns the count, or UINT32_MAX for one with more
// members than a set holds or a member no layout makes (a struct, a
// scalar), which no layout matches.
static uint32_t crgpumetal___internal_module_ranges(MTLStructType *type, crgpumetal_set_range *ranges) {
	uint32_t count = 0;
	for (MTLStructMember *member in type.members) {
		if (count == CRGPU_METAL_MAX_SET_RANGES)
			return UINT32_MAX;
		MTLDataType data_type = member.dataType;
		uint64_t length       = 1;
		if (data_type == MTLDataTypeArray) {
			data_type = member.arrayType.elementType;
			length    = member.arrayType.arrayLength;
		}
		switch (data_type) {
		case MTLDataTypeTexture:
		case MTLDataTypeSampler:
		case MTLDataTypePointer:
		case MTLDataTypeInstanceAccelerationStructure:
			break;
		default:
			return UINT32_MAX;
		}
		if (length == 0 || length > UINT32_MAX || member.argumentIndex > UINT32_MAX)
			return UINT32_MAX;
		ranges[count].first_id  = (uint32_t)member.argumentIndex;
		ranges[count].count     = (uint32_t)length;
		ranges[count].data_type = (uint32_t)data_type;
		++count;
	}
	return crgpumetal___set_ranges_normalize(ranges, count);
}

// Whether every set a function reads - [bindings], its bindings in a
// pipeline's reflection - has the argument buffer of the pipeline's layout
// at that set ([layouts], [layout_count] of them): every binding of the
// layout, of its kind and count, and no other (crgpu_shader_module_desc,
// "Every binding of a set"). A set the function reads with no layout there
// matches nothing.
static bool crgpumetal___internal_sets_match(NSArray<id<MTLBinding>> *bindings, void *const *layouts, uint32_t layout_count) {
	for (id<MTLBinding> binding in bindings) {
		if (binding.type != MTLBindingTypeBuffer || !binding.used)
			continue;
		const NSUInteger index = binding.index;
		if (index < CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE ||
		    index >= CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE + CRGPU_MAX_DESCRIPTOR_SET_LAYOUTS)
			continue;
		const uint32_t set                                 = (uint32_t)(index - CRGPU_METAL_DESCRIPTOR_SET_BUFFER_INDEX_BASE);
		const crgpu_descriptor_set_layout_metal *layout = set < layout_count ? (const crgpu_descriptor_set_layout_metal *)layouts[set] : NULL;
		if (!layout)
			return false;
		id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
		if (buffer.bufferDataType != MTLDataTypeStruct || !buffer.bufferStructType)
			return false;
		crgpumetal_set_range ranges[CRGPU_METAL_MAX_SET_RANGES];
		const uint32_t count = crgpumetal___internal_module_ranges(buffer.bufferStructType, ranges);
		if (count != layout->_range_count)
			return false;
		for (uint32_t i = 0; i < count; ++i)
			if (ranges[i].first_id != layout->_ranges[i].first_id || ranges[i].count != layout->_ranges[i].count ||
			    ranges[i].data_type != layout->_ranges[i].data_type)
				return false;
	}
	return true;
}

// -----------------------------------------------------------------------
// Shader modules
// -----------------------------------------------------------------------

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
	if (desc->code_size > SIZE_MAX)
		return CR_RESULT_OUT_OF_MEMORY;

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	CR_RESULT sr                      = CR_SUCCESS;

	crgpu_shader_module_metal *module_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_shader_module_metal);
	if (!module_metal)
		return CR_RESULT_OUT_OF_MEMORY;
	module_metal->head.allocator = device_metal->head.allocator;
	CR_BIRTH(module_metal);
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
			sr = CR_RESULT_OUT_OF_MEMORY;
		} else {
			NSError *error             = nil;
			module_metal->_library_mtl = [device_metal->_device_mtl newLibraryWithData:data error:&error];
			dispatch_release(data);
			if (!module_metal->_library_mtl)
				sr = CR_RESULT_GPU_INVALID_SHADER_CODE;
		}
		if (sr == CR_SUCCESS) {
			NSString *name = [NSString stringWithUTF8String:desc->entry_point];
			if (name)
				module_metal->_function_mtl = [module_metal->_library_mtl newFunctionWithName:name];
			if (!module_metal->_function_mtl ||
			    !crgpumetal___internal_function_is_stage(module_metal->_function_mtl, desc->stage))
				sr = CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND;
		}
	}
	if (sr != CR_SUCCESS)
		goto failedattempt;

	// From here crgpu_shader_module_destroy frees it, and takes the count
	// back.
	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_SHADER_MODULE);
	if (cr_debug_name_set(module_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_shader_module_destroy((crgpu_shader_module)module_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	module_metal->_desc.debug_name = module_metal->head.debug_name;

	*out_module = (crgpu_shader_module)module_metal;

	return sr;

failedattempt:
	{
		if (module_metal->_function_mtl) {
			[module_metal->_function_mtl release];
		}
		if (module_metal->_library_mtl) {
			[module_metal->_library_mtl release];
		}
		CR_DELETE(&device_metal->head.allocator, crgpu_shader_module_metal, module_metal);
	}
	return sr;
}

void crgpu_shader_module_destroy(crgpu_shader_module shader_module) {
	crgpu_shader_module_metal *module_metal = (crgpu_shader_module_metal *)shader_module;
	if (module_metal) {
		if (CR_STALE(shader_module, "crgpu_shader_module_destroy", "shader_module"))
			return;
		if (module_metal->_function_mtl) {
			[module_metal->_function_mtl release];
		}
		if (module_metal->_library_mtl) {
			[module_metal->_library_mtl release];
		}
		CRGPU___LIVE_REMOVE(&module_metal->_parent_device->_live, CRGPU___OBJECT_SHADER_MODULE);
		CR_KILL(shader_module);
		const CR_ALLOCATOR allocator = module_metal->head.allocator;
		cr_string_free(&allocator, module_metal->head.debug_name);
		CR_DELETE(&allocator, crgpu_shader_module_metal, module_metal);
	}
}

// -----------------------------------------------------------------------
// Render (shader) pipelines
// -----------------------------------------------------------------------

// The class of primitive a topology draws. Patches have none here, for the
// reason crgpumetal___internal_primitive_type gives, and fall back the
// same way.
static MTLPrimitiveTopologyClass crgpumetal___internal_topology_class(CRGPU_PRIMITIVE_TOPOLOGY topology) {
	switch (topology) {
	case CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return MTLPrimitiveTopologyClassTriangle;
	case CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTopologyClassTriangle;
	case CRGPU_PRIMITIVE_TOPOLOGY_LINE_LIST:      return MTLPrimitiveTopologyClassLine;
	case CRGPU_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTopologyClassLine;
	case CRGPU_PRIMITIVE_TOPOLOGY_POINT_LIST:     return MTLPrimitiveTopologyClassPoint;
	default:                                        return MTLPrimitiveTopologyClassTriangle;
	}
}

static MTLCompareFunction crgpumetal___internal_compare_function(CRGPU_COMPARE_OP op) {
	switch (op) {
	case CRGPU_COMPARE_OP_NEVER:            return MTLCompareFunctionNever;
	case CRGPU_COMPARE_OP_LESS:             return MTLCompareFunctionLess;
	case CRGPU_COMPARE_OP_EQUAL:            return MTLCompareFunctionEqual;
	case CRGPU_COMPARE_OP_LESS_OR_EQUAL:    return MTLCompareFunctionLessEqual;
	case CRGPU_COMPARE_OP_GREATER:          return MTLCompareFunctionGreater;
	case CRGPU_COMPARE_OP_NOT_EQUAL:        return MTLCompareFunctionNotEqual;
	case CRGPU_COMPARE_OP_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
	default:                                  return MTLCompareFunctionAlways;
	}
}

static MTLBlendFactor crgpumetal___internal_blend_factor(CRGPU_BLEND_FACTOR factor) {
	switch (factor) {
	case CRGPU_BLEND_FACTOR_ONE:                 return MTLBlendFactorOne;
	case CRGPU_BLEND_FACTOR_SRC_ALPHA:           return MTLBlendFactorSourceAlpha;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
	case CRGPU_BLEND_FACTOR_DST_ALPHA:           return MTLBlendFactorDestinationAlpha;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
	case CRGPU_BLEND_FACTOR_SRC_COLOR:           return MTLBlendFactorSourceColor;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
	case CRGPU_BLEND_FACTOR_DST_COLOR:           return MTLBlendFactorDestinationColor;
	case CRGPU_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
	default:                                       return MTLBlendFactorZero;
	}
}

static MTLBlendOperation crgpumetal___internal_blend_operation(CRGPU_BLEND_OP op) {
	switch (op) {
	case CRGPU_BLEND_OP_SUBTRACT:         return MTLBlendOperationSubtract;
	case CRGPU_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
	case CRGPU_BLEND_OP_MIN:              return MTLBlendOperationMin;
	case CRGPU_BLEND_OP_MAX:              return MTLBlendOperationMax;
	default:                                return MTLBlendOperationAdd;
	}
}

// The pipeline's colour attachments, each with its format and blend state
// (crgpu_graphics_pipeline_desc::color_attachment_formats and
// blend_attachments). Every format has been checked.
static void crgpumetal___internal_set_color_attachments(
    MTLRenderPipelineColorAttachmentDescriptorArray *attachments,
    const crgpu_graphics_pipeline_desc *desc) {
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i) {
		MTLRenderPipelineColorAttachmentDescriptor *attachment = attachments[i];
		const crgpu_blend_attachment_desc *blend             = &desc->blend_attachments[i];
		attachment.pixelFormat     = crgpumetal___format_info(desc->color_attachment_formats[i])->pixel;
		attachment.blendingEnabled = blend->blend_enable;
		if (blend->blend_enable) {
			attachment.sourceRGBBlendFactor        = crgpumetal___internal_blend_factor(blend->src_color_blend_factor);
			attachment.destinationRGBBlendFactor   = crgpumetal___internal_blend_factor(blend->dst_color_blend_factor);
			attachment.rgbBlendOperation           = crgpumetal___internal_blend_operation(blend->color_blend_op);
			attachment.sourceAlphaBlendFactor      = crgpumetal___internal_blend_factor(blend->src_alpha_blend_factor);
			attachment.destinationAlphaBlendFactor = crgpumetal___internal_blend_factor(blend->dst_alpha_blend_factor);
			attachment.alphaBlendOperation         = crgpumetal___internal_blend_operation(blend->alpha_blend_op);
		}
	}
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
	if ((!desc->vertex_module && !desc->mesh_module) || !desc->fragment_module)
		return CR_RESULT_GPU_VERTEX_AND_FRAGMENT_SHADER_REQUIRED;
	if (desc->vertex_module && desc->mesh_module)
		return CR_RESULT_GPU_INVALID_SHADER_STAGE; // Mutually exclusive - see crgpu.h.
	if (desc->mesh_module && !(device->_features & CRGPU_FEATURE_MESH_SHADING))
		return CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED;
	CR_RESULT outputs = crgpu___internal_check_pipeline_outputs(desc, device->_features);
	if (outputs != CR_SUCCESS)
		return outputs;
	// Geometry and tessellation stages were refused just above
	// (CRGPU_EXT_GEOMETRY_SHADER and CRGPU_EXT_TESSELLATION are 0 here,
	// so no Metal device has their bits): Metal has no geometry stage, and
	// its tessellation is post-tessellation vertex functions and
	// -drawPatches:, not a control and evaluation stage pair.

	crgpu_shader_module_metal *vertex_module_metal   = (crgpu_shader_module_metal *)desc->vertex_module;
	crgpu_shader_module_metal *mesh_module_metal      = (crgpu_shader_module_metal *)desc->mesh_module;
	crgpu_shader_module_metal *fragment_module_metal = (crgpu_shader_module_metal *)desc->fragment_module;
	if (vertex_module_metal && !vertex_module_metal->_function_mtl)
		return CR_RESULT_GPU_INVALID_SHADER_MODULE;
	if (mesh_module_metal && !mesh_module_metal->_function_mtl)
		return CR_RESULT_GPU_INVALID_SHADER_MODULE;
	if (!fragment_module_metal->_function_mtl)
		return CR_RESULT_GPU_INVALID_SHADER_MODULE;

	// Attachment formats from the format table, as the images rendered to
	// were made: a pipeline and the textures of its pass always agree.
	for (uint32_t i = 0; i < desc->color_attachment_count; ++i)
		if (crgpumetal___format_info(desc->color_attachment_formats[i])->pixel == MTLPixelFormatInvalid)
			return CR_RESULT_GPU_INVALID_FORMAT;
	MTLPixelFormat depth_format = MTLPixelFormatInvalid;
	// A depth format with a stencil aspect is the stencil attachment too.
	bool depth_has_stencil = false;
	if (desc->depth_format != CRGPU_FORMAT_UNKNOWN) {
		const crgpumetal_format_info *depth_info = crgpumetal___format_info(desc->depth_format);
		depth_format                               = depth_info->pixel;
		if (depth_format == MTLPixelFormatInvalid)
			return CR_RESULT_GPU_INVALID_FORMAT;
		depth_has_stencil = (depth_info->aspects & CRGPU_IMAGE_ASPECT_STENCIL) != 0;
	}

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	CR_RESULT sr                      = CR_SUCCESS;

	crgpu_shader_pipeline_metal *pipeline_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_shader_pipeline_metal);
	if (!pipeline_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	pipeline_metal->head.allocator = device_metal->head.allocator;
	CR_BIRTH(pipeline_metal);
	pipeline_metal->_parent_device = device_metal;
	pipeline_metal->_desc          = *desc;

	if (mesh_module_metal) {
		// Mesh-shader pipeline: no vertex input state/input-assembler at
		// all - the mesh function supplies its own geometry. See
		// crgpu_graphics_pipeline_desc::mesh_module in crgpu.h.
		MTLMeshRenderPipelineDescriptor *pipeline_desc = [[MTLMeshRenderPipelineDescriptor alloc] init];
		pipeline_desc.meshFunction                     = mesh_module_metal->_function_mtl;
		pipeline_desc.fragmentFunction                 = fragment_module_metal->_function_mtl;
		crgpumetal___internal_set_color_attachments(pipeline_desc.colorAttachments, desc);

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
		                                      options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
		                                   reflection:&reflection
		                                        error:&pipeline_error];
		// Autoreleased: read now, before anything can release it.
		pipeline_metal->_non_fragment_set_mask =
		    reflection ? (crgpumetal___internal_set_mask(reflection.meshBindings) | crgpumetal___internal_set_mask(reflection.objectBindings))
		               : 0xffffffffu;
		pipeline_metal->_fragment_set_mask = reflection ? crgpumetal___internal_set_mask(reflection.fragmentBindings) : 0xffffffffu;
		[pipeline_desc release];
		if (!pipeline_metal->_render_pipeline_state_mtl) {
			printf(
			    "crgpu: MTLRenderPipelineState (mesh) creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = CR_RESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
		if (!reflection) {
			sr = CR_RESULT_API_SPECIFIC_FAILURE;
			goto failedattempt;
		}
		if (!crgpumetal___internal_sets_match(reflection.objectBindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count) ||
		    !crgpumetal___internal_sets_match(reflection.meshBindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count) ||
		    !crgpumetal___internal_sets_match(reflection.fragmentBindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count)) {
			sr = CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH;
			goto failedattempt;
		}

		pipeline_metal->_is_mesh_pipeline    = true;
		pipeline_metal->_mesh_local_size_x   = mesh_module_metal->_workgroup_size_x;
		pipeline_metal->_mesh_local_size_y   = mesh_module_metal->_workgroup_size_y;
		pipeline_metal->_mesh_local_size_z   = mesh_module_metal->_workgroup_size_z;
	} else {
		// Unlike Vulkan's VkPipelineLayout or D3D12's ID3D12RootSignature,
		// an MTLRenderPipelineState needs no resource-layout object: each
		// set's argument buffer layout is the module's, in the binding layout
		// crgpu.h states. descriptor_set_layouts are read only to check
		// the modules' argument buffers against them, once the state exists.
		MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
		for (uint32_t i = 0; i < desc->vertex_attribute_count; i++) {
			const crgpu_vertex_attribute_desc *a = &desc->vertex_attributes[i];
			MTLVertexAttributeDescriptor *attr     = vertex_desc.attributes[a->location];
			attr.format                            = crgpumetal___format_info(a->format)->vertex;
			attr.offset                            = a->offset;
			attr.bufferIndex                       = a->binding;
		}
		for (uint32_t i = 0; i < desc->vertex_binding_count; i++) {
			const crgpu_vertex_binding_desc *b   = &desc->vertex_bindings[i];
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
		// (crgpu_rendering_begin_desc::layer_count) unless this is given,
		// and a pipeline isn't created against a pass, so it is always given.
		pipeline_desc.inputPrimitiveTopology = crgpumetal___internal_topology_class(desc->primitive_topology);
		crgpumetal___internal_set_color_attachments(pipeline_desc.colorAttachments, desc);

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
		                                 options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
		                              reflection:&reflection
		                                    error:&pipeline_error];
		pipeline_metal->_non_fragment_set_mask = reflection ? crgpumetal___internal_set_mask(reflection.vertexBindings) : 0xffffffffu;
		pipeline_metal->_fragment_set_mask     = reflection ? crgpumetal___internal_set_mask(reflection.fragmentBindings) : 0xffffffffu;
		[pipeline_desc release];
		if (!pipeline_metal->_render_pipeline_state_mtl) {
			printf(
			    "crgpu: MTLRenderPipelineState creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = CR_RESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
		if (!reflection) {
			sr = CR_RESULT_API_SPECIFIC_FAILURE;
			goto failedattempt;
		}
		if (!crgpumetal___internal_sets_match(reflection.vertexBindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count) ||
		    !crgpumetal___internal_sets_match(reflection.fragmentBindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count)) {
			sr = CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH;
			goto failedattempt;
		}
	}

	{
		// Vulkan/D3D12 bake depth/stencil test config into the same
		// monolithic pipeline object as everything above; Metal keeps it as
		// a wholly separate native object - see crgpu_command_list_bind_graphics_pipeline in
		// crgpumetalcommand.m, which binds both together.
		MTLDepthStencilDescriptor *depth_stencil_desc = [[MTLDepthStencilDescriptor alloc] init];
		depth_stencil_desc.depthCompareFunction =
		    desc->depth_test_enable ? crgpumetal___internal_compare_function(desc->depth_compare_op) : MTLCompareFunctionAlways;
		depth_stencil_desc.depthWriteEnabled = desc->depth_test_enable && desc->depth_write_enable;
		pipeline_metal->_depth_stencil_state_mtl =
		    [device_metal->_device_mtl newDepthStencilStateWithDescriptor:depth_stencil_desc];
		[depth_stencil_desc release];
		if (!pipeline_metal->_depth_stencil_state_mtl) {
			sr = CR_RESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
	}

	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_SHADER_PIPELINE);
	if (cr_debug_name_set(pipeline_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_graphics_pipeline_destroy((crgpu_graphics_pipeline)pipeline_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pipeline_metal->_desc.debug_name = pipeline_metal->head.debug_name;

	*out_pipeline = (crgpu_graphics_pipeline)pipeline_metal;

	return sr;

failedattempt:
	if (pipeline_metal) {
		if (pipeline_metal->_depth_stencil_state_mtl) {
			[pipeline_metal->_depth_stencil_state_mtl release];
		}
		if (pipeline_metal->_render_pipeline_state_mtl) {
			[pipeline_metal->_render_pipeline_state_mtl release];
		}
		CR_DELETE(&device_metal->head.allocator, crgpu_shader_pipeline_metal, pipeline_metal);
	}
	return sr;
}

void crgpu_graphics_pipeline_destroy(crgpu_graphics_pipeline pipeline) {
	crgpu_shader_pipeline_metal *pipeline_metal = (crgpu_shader_pipeline_metal *)pipeline;
	if (pipeline_metal) {
		if (CR_STALE(pipeline, "crgpu_graphics_pipeline_destroy", "pipeline"))
			return;
		if (pipeline_metal->_depth_stencil_state_mtl) {
			[pipeline_metal->_depth_stencil_state_mtl release];
		}
		if (pipeline_metal->_render_pipeline_state_mtl) {
			[pipeline_metal->_render_pipeline_state_mtl release];
		}
		CRGPU___LIVE_REMOVE(&pipeline_metal->_parent_device->_live, CRGPU___OBJECT_SHADER_PIPELINE);
		CR_KILL(pipeline);
		const CR_ALLOCATOR allocator = pipeline_metal->head.allocator;
		cr_string_free(&allocator, pipeline_metal->head.debug_name);
		CR_DELETE(&allocator, crgpu_shader_pipeline_metal, pipeline_metal);
	}
}

// -----------------------------------------------------------------------
// Compute pipelines
// -----------------------------------------------------------------------

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

	crgpu_shader_module_metal *compute_module_metal = (crgpu_shader_module_metal *)desc->compute_module;
	if (!compute_module_metal->_function_mtl)
		return CR_RESULT_GPU_INVALID_SHADER_MODULE;

	crgpu_device_metal *device_metal = (crgpu_device_metal *)device;
	CR_RESULT sr                      = CR_SUCCESS;

	crgpu_compute_pipeline_metal *pipeline_metal = CR_NEW_ZEROED(&device_metal->head.allocator, crgpu_compute_pipeline_metal);
	if (!pipeline_metal) {
		sr = CR_RESULT_GENERAL_FAILURE;
		goto failedattempt;
	}
	pipeline_metal->head.allocator = device_metal->head.allocator;
	CR_BIRTH(pipeline_metal);
	pipeline_metal->_parent_device = device_metal;
	pipeline_metal->_desc          = *desc;
	pipeline_metal->_local_size_x  = compute_module_metal->_workgroup_size_x;
	pipeline_metal->_local_size_y  = compute_module_metal->_workgroup_size_y;
	pipeline_metal->_local_size_z  = compute_module_metal->_workgroup_size_z;

	{
		// Same reasoning as crgpu_graphics_pipeline_create above - a
		// MTLComputePipelineState needs no resource-layout object; the
		// descriptor_set_layouts are read only to check the module's
		// argument buffers against them.
		NSError *pipeline_error                  = nil;
		MTLComputePipelineReflection *reflection = nil;
		pipeline_metal->_compute_pipeline_state_mtl =
		    [device_metal->_device_mtl newComputePipelineStateWithFunction:compute_module_metal->_function_mtl
		                                                            options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
		                                                         reflection:&reflection
		                                                              error:&pipeline_error];
		if (!pipeline_metal->_compute_pipeline_state_mtl) {
			printf(
			    "crgpu: MTLComputePipelineState creation failed: %s\n",
			    pipeline_error ? pipeline_error.localizedDescription.UTF8String : "(unknown)");
			sr = CR_RESULT_GPU_INVALID_SHADER_CODE;
			goto failedattempt;
		}
		if (!reflection) {
			sr = CR_RESULT_API_SPECIFIC_FAILURE;
			goto failedattempt;
		}
		if (!crgpumetal___internal_sets_match(reflection.bindings, desc->descriptor_set_layouts, desc->descriptor_set_layout_count)) {
			sr = CR_RESULT_GPU_SHADER_LAYOUT_MISMATCH;
			goto failedattempt;
		}
	}

	CRGPU___LIVE_ADD(&device_metal->_live, CRGPU___OBJECT_COMPUTE_PIPELINE);
	if (cr_debug_name_set(pipeline_metal, desc->debug_name) != CR_SUCCESS) {
		crgpu_compute_pipeline_destroy((crgpu_compute_pipeline)pipeline_metal);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	pipeline_metal->_desc.debug_name = pipeline_metal->head.debug_name;

	*out_pipeline = (crgpu_compute_pipeline)pipeline_metal;

	return sr;

failedattempt:
	if (pipeline_metal) {
		if (pipeline_metal->_compute_pipeline_state_mtl) {
			[pipeline_metal->_compute_pipeline_state_mtl release];
		}
		CR_DELETE(&device_metal->head.allocator, crgpu_compute_pipeline_metal, pipeline_metal);
	}
	return sr;
}

void crgpu_compute_pipeline_destroy(crgpu_compute_pipeline pipeline) {
	crgpu_compute_pipeline_metal *pipeline_metal = (crgpu_compute_pipeline_metal *)pipeline;
	if (pipeline_metal) {
		if (CR_STALE(pipeline, "crgpu_compute_pipeline_destroy", "pipeline"))
			return;
		if (pipeline_metal->_compute_pipeline_state_mtl) {
			[pipeline_metal->_compute_pipeline_state_mtl release];
		}
		CRGPU___LIVE_REMOVE(&pipeline_metal->_parent_device->_live, CRGPU___OBJECT_COMPUTE_PIPELINE);
		CR_KILL(pipeline);
		const CR_ALLOCATOR allocator = pipeline_metal->head.allocator;
		cr_string_free(&allocator, pipeline_metal->head.debug_name);
		CR_DELETE(&allocator, crgpu_compute_pipeline_metal, pipeline_metal);
	}
}

CR_RESULT crgpu_compute_pipeline_get_desc(
    crgpu_compute_pipeline pipeline,
    crgpu_compute_pipeline_desc *out_desc) {
	crgpu_compute_pipeline_metal *pipeline_metal = (crgpu_compute_pipeline_metal *)pipeline;
	if (!pipeline_metal)
		return CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE;
	if (CR_STALE(pipeline, "crgpu_compute_pipeline_get_desc", "pipeline"))
		return CR_RESULT_GPU_INVALID_COMPUTE_PIPELINE;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = pipeline_metal->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC);
	return CR_SUCCESS;
}

#endif // CRGPU_COMPILE_METAL_API
