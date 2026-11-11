
#if CRGPU_COMPILE_D3D12_API

#include "crgpud3d12.hpp"
#include "../../../crstructbase.h"

// Releases a swap chain and its back buffer arrays, each of
// _desc.buffer_count elements. Either array may still be null, which is how
// a swap chain that failed part-way through creation arrives here.
static void crgpu_d3d12___release_swap_chain(crgpu_swap_chain_d3d12 *swap_chain) {
	const CR_ALLOCATOR allocator = swap_chain->head.allocator;
	const uint32_t count           = swap_chain->_desc.buffer_count;
	// A destroyed swap chain's block and its back buffers' arrays go to its
	// device's quarantine (crstale.h); NULL on a create's failure path.
	CR_QUARANTINE_KEEP(quarantine, swap_chain);
	if (swap_chain->_back_buffer_images) {
		for (uint32_t i = 0; i < count; ++i) {
			CR_KILL(&swap_chain->_back_buffer_images[i]);
			cr_string_free(&allocator, swap_chain->_back_buffer_images[i].head.debug_name);
			swap_chain->_back_buffer_images[i].~crgpu_image_d3d12();
		}
		CR_RETIRE_ARRAY(quarantine, &allocator, crgpu_image_d3d12, swap_chain->_back_buffer_images, count);
	}
	if (swap_chain->_back_buffer_image_views) {
		for (uint32_t i = 0; i < count; ++i) {
			CR_KILL(&swap_chain->_back_buffer_image_views[i]);
			cr_string_free(&allocator, swap_chain->_back_buffer_image_views[i].head.debug_name);
		}
	}
	CR_RETIRE_ARRAY(quarantine, &allocator, crgpu_image_view_d3d12, swap_chain->_back_buffer_image_views, count);
	cr_string_free(&allocator, swap_chain->head.debug_name);
	swap_chain->~crgpu_swap_chain_d3d12();
	CR_RETIRE(quarantine, &allocator, crgpu_swap_chain_d3d12, swap_chain);
}

// The present scalings a flip model swap chain on [hwnd] keeps the header's
// rule for. DXGI_SCALING_ASPECT_RATIO_STRETCH is never one: on a window's
// swap chain it acts as DXGI_SCALING_STRETCH (DXGI_SCALING's docs). Nor is
// DXGI_SCALING_NONE on a WS_EX_LAYOUTRTL window, which it aligns by the
// right edges, not the left.
static uint32_t crgpu_d3d12___present_scalings(HWND hwnd) {
	uint32_t scalings = CRGPU_PRESENT_SCALING_STRETCH;
	if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL))
		scalings |= CRGPU_PRESENT_SCALING_ONE_TO_ONE;
	return scalings;
}

extern "C" {

CR_RESULT crgpu_surface_get_present_scalings(
    crgpu_device device,
    crgpu_surface surface,
    uint32_t *out_scalings) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_surface_get_present_scalings", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	const CR_RESULT sr = crgpu___internal_check_surface_present_scalings(device->_features, surface, out_scalings);
	if (sr != CR_SUCCESS)
		return sr;
	*out_scalings = crgpu_d3d12___present_scalings(surface->_hwnd);
	return CR_SUCCESS;
}

CR_RESULT crgpu_swap_chain_create(
    crgpu_device device,
    const crgpu_swap_chain_desc *desc,
    crgpu_swap_chain *out_swap_chain) {
	if (!device)
		return CR_RESULT_GPU_INVALID_DEVICE;
	if (CR_STALE(device, "crgpu_swap_chain_create", "device"))
		return CR_RESULT_GPU_INVALID_DEVICE;
	const CR_RESULT desc_sr = crgpu___internal_check_swap_chain_desc(device->_features, desc, out_swap_chain);
	if (desc_sr != CR_SUCCESS)
		return desc_sr;
	if (!(crgpu_d3d12___present_scalings(desc->surface->_hwnd) & desc->present_scaling))
		return CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED;

	crgpu_swap_chain_d3d12 *pResult =
	    CR_NEW(&device->head.allocator, crgpu_swap_chain_d3d12);
	if (!pResult)
		return CR_RESULT_OUT_OF_MEMORY;
	pResult                 = new (pResult) crgpu_swap_chain_d3d12();
	pResult->head.allocator = device->head.allocator;
	CR_BIRTH_IN(pResult, &device->_quarantine);
	pResult->_device        = device;
	pResult->_desc          = *desc;

	HWND hwnd = desc->surface->_hwnd;
	// The DXGI swap chain is bound to this specific queue for its lifetime -
	// crgpu_swap_chain_desc::queue, not a hardcoded "the graphics queue"
	// guess, since DXGI can't rebind a swap chain to a different queue later.
	ID3D12CommandQueue *cmdQueue = desc->queue->_d3d_cmd_queue.Get();

	DXGI_SWAP_CHAIN_DESC1 scDesc = {};
	scDesc.Width                 = desc->width;
	scDesc.Height                = desc->height;
	scDesc.Format                = crgpu_d3d12_get_dxgi_format(desc->format);
	scDesc.Stereo                = FALSE;
	scDesc.SampleDesc            = {1, 0};
	scDesc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// Copy source/destination needs no DXGI flag; D3D12 back buffers allow
	// CopyTextureRegion either way.
	if (desc->usage & CRGPU_IMAGE_USAGE_SAMPLED)
		scDesc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
	if (desc->usage & CRGPU_IMAGE_USAGE_STORAGE)
		scDesc.BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS;
	scDesc.BufferCount           = desc->buffer_count;
	// ONE_TO_ONE or STRETCH, the two crgpu_d3d12___present_scalings offers.
	scDesc.Scaling = desc->present_scaling == CRGPU_PRESENT_SCALING_ONE_TO_ONE ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
	scDesc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scDesc.AlphaMode             = DXGI_ALPHA_MODE_UNSPECIFIED;
	scDesc.Flags                 = 0;

	bool isExclusiveFullscreen =
	    (desc->fullscreen_mode == CRGPU_FULLSCREEN_MODE_FULLSCREEN);
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
	fsDesc.RefreshRate                     = {0, 1};
	fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	fsDesc.Scaling          = DXGI_MODE_SCALING_UNSPECIFIED;
	fsDesc.Windowed         = FALSE;

	Microsoft::WRL::ComPtr<IDXGISwapChain1> dxgiSwapChain1;
	HRESULT hr = device->_adapter->_instance->_dxgi_factory->CreateSwapChainForHwnd(
	    cmdQueue, hwnd, &scDesc, isExclusiveFullscreen ? &fsDesc : nullptr,
	    nullptr, &dxgiSwapChain1);
	if (FAILED(hr)) {
		crgpu_d3d12___release_swap_chain(pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	hr = dxgiSwapChain1.As(&pResult->_dxgi_swap_chain);
	if (FAILED(hr)) {
		crgpu_d3d12___release_swap_chain(pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}

	// The image holds a ComPtr, so each element is constructed in place; the
	// image view is plain and zeroed memory is its initial state. Every
	// image is constructed before the first GetBuffer so that a failure
	// below always finds buffer_count constructed images to release. Both
	// arrays are of buffer_count, which is what they are freed with; each
	// element's allocator is the swap chain's, which frees the names a
	// caller gives them.
	crgpu_image_d3d12 *images = CR_NEW_ARRAY(
	    &pResult->head.allocator, crgpu_image_d3d12, desc->buffer_count);
	if (!images) {
		crgpu_d3d12___release_swap_chain(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	for (uint32_t i = 0; i < desc->buffer_count; ++i) {
		new (&images[i]) crgpu_image_d3d12();
		images[i].head.allocator      = pResult->head.allocator;
		CR_BIRTH(&images[i]);
		images[i]._subresource_states = eq::array<D3D12_RESOURCE_STATES>(&device->_eq_allocator);
	}
	pResult->_back_buffer_images = images;

	pResult->_back_buffer_image_views = CR_NEW_ARRAY_ZEROED(
	    &pResult->head.allocator, crgpu_image_view_d3d12, desc->buffer_count);
	if (!pResult->_back_buffer_image_views) {
		crgpu_d3d12___release_swap_chain(pResult);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	for (uint32_t i = 0; i < desc->buffer_count; ++i) {
		pResult->_back_buffer_image_views[i].head.allocator = pResult->head.allocator;
		CR_BIRTH(&pResult->_back_buffer_image_views[i]);
	}

	DXGI_FORMAT fmt = crgpu_d3d12_get_dxgi_format(desc->format);

	for (uint32_t i = 0; i < desc->buffer_count; ++i) {
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		hr = pResult->_dxgi_swap_chain->GetBuffer(i, IID_PPV_ARGS(&resource));
		if (FAILED(hr)) {
			crgpu_d3d12___release_swap_chain(pResult);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}

		crgpu_image_d3d12 &img = pResult->_back_buffer_images[i];
		hr = resource.As(&img._d3d_resource);
		if (FAILED(hr)) {
			crgpu_d3d12___release_swap_chain(pResult);
			return CR_RESULT_API_SPECIFIC_FAILURE;
		}
		img._device            = device;
		img._desc.format       = desc->format;
		img._desc.width        = desc->width;
		img._desc.height       = desc->height;
		img._desc.depth        = 1;
		img._desc.mip_levels   = 1;
		img._desc.array_layers = 1;
		img._desc.type         = CRGPU_IMAGE_TYPE_2D;
		img._desc.usage        = CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT | desc->usage;
		img._d3d_resource_desc = img._d3d_resource->GetDesc();
		// Swap chain back buffers start life in PRESENT (== COMMON, 0x0), not
		// the RENDER_TARGET state CRGPU_IMAGE_USAGE_COLOR_ATTACHMENT would
		// otherwise imply for a freshly crgpu_image_create'd resource --
		// they're DXGI-provided, not created via CreateCommittedResource1, so
		// crgpu_d3d12_get_initial_image_state's usage-based guess doesn't
		// apply here.
		img._current_state     = D3D12_RESOURCE_STATE_PRESENT;

		crgpu_image_view_d3d12 &view = pResult->_back_buffer_image_views[i];
		view._image                    = &img;
		view._desc.type                = CRGPU_IMAGE_VIEW_TYPE_2D;
		view._desc.parent_image        = &img;
		// view._desc.format                             = desc->format;
		view._d3d_view_desc._rtv.Format        = fmt;
		view._d3d_view_desc._rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		view._d3d_view_desc._rtv.Texture2D.MipSlice   = 0;
		view._d3d_view_desc._rtv.Texture2D.PlaneSlice = 0;
	}

	if (FAILED(device->_d3d_device->CreateFence(
	        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pResult->_frame_fence)))) {
		crgpu_d3d12___release_swap_chain(pResult);
		return CR_RESULT_API_SPECIFIC_FAILURE;
	}
	pResult->_frame_fence_next_value = 0;

	CRGPU___LIVE_ADD(&device->_live, CRGPU___OBJECT_SWAP_CHAIN);
	*out_swap_chain = pResult;
	return CR_SUCCESS;
}

void crgpu_swap_chain_destroy(crgpu_swap_chain swap_chain) {
	if (!swap_chain)
		return;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_destroy", "swap_chain"))
		return;
	CRGPU___LIVE_REMOVE(&swap_chain->_device->_live, CRGPU___OBJECT_SWAP_CHAIN);
	CR_KILL(swap_chain);
	crgpu_d3d12___release_swap_chain(swap_chain);
}

CR_RESULT crgpu_swap_chain_get_desc(
    crgpu_swap_chain swap_chain, crgpu_swap_chain_desc *out_desc) {
	if (!swap_chain)
		return CR_RESULT_GPU_INVALID_SWAP_CHAIN;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_get_desc", "swap_chain"))
		return CR_RESULT_GPU_INVALID_SWAP_CHAIN;
	if (!out_desc)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = swap_chain->_desc;
	out_desc->base = cr_struct_base(CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC);
	return CR_SUCCESS;
}

uint32_t crgpu_swap_chain_acquire_next_image(crgpu_swap_chain swap_chain) {
	if (!swap_chain)
		return CR_UINT32_MAX;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_acquire_next_image", "swap_chain"))
		return CR_UINT32_MAX;

	// The in-flight-fence half of crgpu_command_queue_submit_lists_synced's
	// contract (see crgpu_swap_chain_d3d12 in crgpud3d12.hpp) -- block
	// until the previously submitted frame's GPU work has completed before
	// handing back a buffer index the caller is about to reuse a shared
	// command allocator/list against.
	uint64_t pending = swap_chain->_frame_fence_next_value;
	if (pending != 0 && swap_chain->_frame_fence->GetCompletedValue() < pending) {
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (event) {
			if (SUCCEEDED(swap_chain->_frame_fence->SetEventOnCompletion(pending, event)))
				WaitForSingleObject(event, INFINITE);
			CloseHandle(event);
		}
	}

	return swap_chain->_dxgi_swap_chain->GetCurrentBackBufferIndex();
}

void crgpu_swap_chain_present(crgpu_swap_chain swap_chain) {
	if (!swap_chain)
		return;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_present", "swap_chain"))
		return;
	UINT syncInterval =
	    (swap_chain->_desc.present_mode == CRGPU_PRESENT_MODE_IMMEDIATE) ? 0
	                                                                       : 1;
	// Present queues work on the swap chain's queue: a use of it (crgpu.h).
	crgpu_d3d12___queue_enter(swap_chain->_desc.queue);
	swap_chain->_dxgi_swap_chain->Present(syncInterval, 0);
	crgpu_d3d12___queue_leave(swap_chain->_desc.queue);
}

crgpu_image_view crgpu_swap_chain_get_image_view(
    crgpu_swap_chain swap_chain, uint32_t image_index) {
	if (!swap_chain)
		return nullptr;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_get_image_view", "swap_chain"))
		return nullptr;
	if (image_index >= swap_chain->_desc.buffer_count)
		return nullptr;
	return &swap_chain->_back_buffer_image_views[image_index];
}

// The three calls below are declared for every backend and not implemented
// by this one (crgpu.h says so on each). They are defined, refusing as
// their header says, so that a caller naming one links on D3D12 as on the
// others. A D3D12 swap chain synchronizes through its own fence per present
// (crgpu_swap_chain_d3d12), not through semaphores a caller passes on.

crgpu_surface crgpu_surface_create_from_callback(
    crgpu_instance instance,
    void *user_data,
    crgpu_surface_create_fn create_fn) {
	(void)user_data;
	if (!instance || !create_fn)
		return nullptr;
	if (CR_STALE(instance, "crgpu_surface_create_from_callback", "instance"))
		return nullptr;
	return nullptr;
}

crgpu_semaphore crgpu_swap_chain_get_image_available_semaphore(crgpu_swap_chain swap_chain) {
	// D3D12's swap chain has no semaphores: null for every swap chain.
	if (!swap_chain)
		return nullptr;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_get_image_available_semaphore", "swap_chain"))
		return nullptr;
	return nullptr;
}

crgpu_semaphore crgpu_swap_chain_get_render_finished_semaphore(crgpu_swap_chain swap_chain) {
	// D3D12's swap chain has no semaphores: null for every swap chain.
	if (!swap_chain)
		return nullptr;
	if (CR_STALE(swap_chain, "crgpu_swap_chain_get_render_finished_semaphore", "swap_chain"))
		return nullptr;
	return nullptr;
}
}

#endif // CRGPU_COMPILE_D3D12_API
