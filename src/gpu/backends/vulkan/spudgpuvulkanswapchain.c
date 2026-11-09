//
// Created by Nathan on 5/13/2026.
//

#if SPUDGPU_COMPILE_VULKAN_API

#include "spudgpuvulkan.h"
#include "spudgpu.h"

#if defined(SPUDLIB_PLATFORM_WIN32)
#include <windows.h>
#include <vulkan/vulkan_win32.h>

#elif defined(SPUDGPU_PLATFORM_WAYLAND)
#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>

#elif defined(SPUDGPU_PLATFORM_XLIB)
#include <vulkan/vulkan_xlib.h>
#include <X11/Xlib.h>
#endif

#include <stdlib.h>
#include <string.h>
#include "../../../spudstructbase.h"


#if __cplusplus
extern "C" {
#endif

// The surface's formats, made through the swap chain's allocator: *ppOutput
// holds *pOutputCount of them in an array of *pOutputCapacity, the count it
// is freed with. False if the array can't be made.
bool spudgpuvulkan___get_available_formats_internal(
	spudgpu_swap_chain_vulkan *pSwapChain,
	VkSurfaceFormatKHR **ppOutput,
	uint32_t *pOutputCount,
	uint32_t *pOutputCapacity) {
	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(pSwapChain->_device._physical_device_vk, pSwapChain->_surface_vk, &formatCount,
	                                     NULL);
	*ppOutput = SPUD_NEW_ARRAY(&pSwapChain->head.allocator, VkSurfaceFormatKHR, formatCount);
	if (!*ppOutput) return false;
	*pOutputCapacity = formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(pSwapChain->_device._physical_device_vk, pSwapChain->_surface_vk, &formatCount,
	                                     *ppOutput);
	*pOutputCount = formatCount;
	return true;
}

// [surface]'s present modes, made through [allocator]: *ppOutput holds
// *pOutputCount of them in an array of *pOutputCapacity, the count it is
// freed with. False if the array can't be made.
static bool spudgpuvulkan___get_available_present_modes_internal(
	const SPUD_ALLOCATOR *allocator,
	VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface,
	VkPresentModeKHR **ppOutput,
	uint32_t *pOutputCount,
	uint32_t *pOutputCapacity) {
	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, NULL);
	*ppOutput = SPUD_NEW_ARRAY(allocator, VkPresentModeKHR, presentModeCount);
	if (!*ppOutput) return false;
	*pOutputCapacity = presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, *ppOutput);
	*pOutputCount = presentModeCount;
	return true;
}

// What [surface] offers for scaling a swap chain of [presentMode]
// (VK_KHR_surface_maintenance1). Only for a device with
// _swapchain_maintenance1, whose adapter has the call loaded.
static SPUDRESULT spudgpuvulkan___scaling_capabilities_internal(
	const spudgpu_device_vulkan *device,
	VkSurfaceKHR surface,
	VkPresentModeKHR presentMode,
	VkSurfacePresentScalingCapabilitiesKHR *pOutput) {
	VkSurfacePresentModeKHR mode = {0};
	mode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
	mode.presentMode = presentMode;
	VkPhysicalDeviceSurfaceInfo2KHR info = {0};
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
	info.pNext = &mode;
	info.surface = surface;
	memset(pOutput, 0, sizeof(*pOutput));
	pOutput->sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR;
	VkSurfaceCapabilities2KHR capabilities = {0};
	capabilities.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
	capabilities.pNext = pOutput;
	if (device->_adapter->_vkGetPhysicalDeviceSurfaceCapabilities2KHR(device->_physical_device_vk, &info, &capabilities) != VK_SUCCESS)
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	return SPUD_SUCCESS;
}

// Whether [capabilities] scale one to one from the top left: the
// ONE_TO_ONE scaling with MIN gravity on both axes.
static bool spudgpuvulkan___one_to_one_offered_internal(const VkSurfacePresentScalingCapabilitiesKHR *capabilities) {
	return (capabilities->supportedPresentScaling & VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR) &&
	       (capabilities->supportedPresentGravityX & VK_PRESENT_GRAVITY_MIN_BIT_KHR) &&
	       (capabilities->supportedPresentGravityY & VK_PRESENT_GRAVITY_MIN_BIT_KHR);
}

// The SPUDGPU_PRESENT_SCALING bits besides ONE_TO_ONE that [capabilities]
// keep the header's rule for. STRETCH takes no gravity; ASPECT_FIT is
// centred on both axes, and never on a Win32 surface: there the NVIDIA
// driver offers ASPECT_RATIO_STRETCH with centred gravity and shows a
// stretch (RTX A2000, 2026-10-23), as DXGI_SCALING_ASPECT_RATIO_STRETCH
// does on a window's swap chain, the reason D3D12 doesn't offer it either.
static uint32_t spudgpuvulkan___scalings_offered_internal(const VkSurfacePresentScalingCapabilitiesKHR *capabilities) {
	uint32_t scalings = 0;
	if (capabilities->supportedPresentScaling & VK_PRESENT_SCALING_STRETCH_BIT_KHR)
		scalings |= SPUDGPU_PRESENT_SCALING_STRETCH;
#if !defined(SPUDLIB_PLATFORM_WIN32)
	if ((capabilities->supportedPresentScaling & VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_KHR) &&
	    (capabilities->supportedPresentGravityX & VK_PRESENT_GRAVITY_CENTERED_BIT_KHR) &&
	    (capabilities->supportedPresentGravityY & VK_PRESENT_GRAVITY_CENTERED_BIT_KHR))
		scalings |= SPUDGPU_PRESENT_SCALING_ASPECT_FIT;
#endif
	return scalings;
}

// spudgpu_get_surface_present_scalings' answer for [surface] on [device],
// which spudgpu_create_swap_chain refuses a scaling outside of.
// ONE_TO_ONE is always in it: where the surface offers no one-to-one
// scaling, or the device has no swapchain_maintenance1, a ONE_TO_ONE swap
// chain names no scaling and takes the surface's own size where the
// surface fixes one, so that nothing is scaled. The rest is what the
// surface offers for every present mode SpudGPU names that it has, so that
// any present_mode a desc names keeps it.
static SPUDRESULT spudgpuvulkan___surface_present_scalings_internal(
	const spudgpu_device_vulkan *device,
	VkSurfaceKHR surface,
	uint32_t *pOutput) {
	*pOutput = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE;
	if (!device->_swapchain_maintenance1) return SPUD_SUCCESS;

	VkPresentModeKHR *pPresentModes = NULL;
	uint32_t presentModeCount = 0;
	uint32_t presentModeCapacity = 0;
	if (!spudgpuvulkan___get_available_present_modes_internal(&device->head.allocator, device->_physical_device_vk, surface,
	                                                          &pPresentModes, &presentModeCount, &presentModeCapacity))
		return SPUDRESULT_OUT_OF_MEMORY;
	uint32_t common = SPUDGPU_PRESENT_SCALING_STRETCH | SPUDGPU_PRESENT_SCALING_ASPECT_FIT;
	bool anyMode = false;
	SPUDRESULT sr = SPUD_SUCCESS;
	for (uint32_t i = 0; i < presentModeCount && sr == SPUD_SUCCESS; i++) {
		const VkPresentModeKHR mode = pPresentModes[i];
		if (mode != VK_PRESENT_MODE_IMMEDIATE_KHR && mode != VK_PRESENT_MODE_FIFO_KHR && mode != VK_PRESENT_MODE_MAILBOX_KHR)
			continue;
		VkSurfacePresentScalingCapabilitiesKHR capabilities;
		sr = spudgpuvulkan___scaling_capabilities_internal(device, surface, mode, &capabilities);
		if (sr == SPUD_SUCCESS) {
			common &= spudgpuvulkan___scalings_offered_internal(&capabilities);
			anyMode = true;
		}
	}
	SPUD_DELETE_ARRAY(&device->head.allocator, VkPresentModeKHR, pPresentModes, presentModeCapacity);
	if (sr != SPUD_SUCCESS) return sr;
	if (anyMode) *pOutput |= common;
	return SPUD_SUCCESS;
}

VkSurfaceFormatKHR spudgpuvulkan___choose_surface_format_internal(
	VkSurfaceFormatKHR *pAvailableFormats,
	uint32_t availableFormatCount,
	VkFormat requestedFormat) {
	// The caller (pSwapChain->_desc.format) already decided which format it
	// wants - e.g. so it matches the format a pipeline was created against
	// (spudgpu_shader_pipeline_desc::color_attachment_formats). Picking a
	// different one here on the caller's behalf is exactly the kind of
	// silent SpudLib-side default this backend must not make; only fall
	// back to SRGB (still colorSpace-matched) if the exact request isn't
	// actually present on this surface.
	for (uint32_t i = 0; i < availableFormatCount; i++)
		if (pAvailableFormats[i].format == requestedFormat &&
		    pAvailableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return pAvailableFormats[i];
	for (uint32_t i = 0; i < availableFormatCount; i++)
		if (pAvailableFormats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    pAvailableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return pAvailableFormats[i];
	return (VkSurfaceFormatKHR){0};
}

VkPresentModeKHR spudgpuvulkan___translate_present_mode_internal(
	SPUDGPU_PRESENT_MODE mode) {
	switch (mode) {
		case SPUDGPU_PRESENT_MODE_IMMEDIATE: return VK_PRESENT_MODE_IMMEDIATE_KHR;
		case SPUDGPU_PRESENT_MODE_MAILBOX: return VK_PRESENT_MODE_MAILBOX_KHR;
		case SPUDGPU_PRESENT_MODE_FIFO:
		default: return VK_PRESENT_MODE_FIFO_KHR;
	}
}

VkPresentModeKHR spudgpuvulkan___choose_present_mode_internal(
	VkPresentModeKHR *pAvailablePresentModes,
	uint32_t availablePresentModeCount,
	SPUDGPU_PRESENT_MODE requested) {
	VkPresentModeKHR wanted = spudgpuvulkan___translate_present_mode_internal(requested);
	for (uint32_t i = 0; i < availablePresentModeCount; i++)
		if (pAvailablePresentModes[i] == wanted)
			return wanted;
	return VK_PRESENT_MODE_FIFO_KHR; // FIFO is guaranteed to be available by the Vulkan spec (Standard V-Sync)
}

VkExtent2D spudgpuvulkan___choose_extent_internal(
	VkSurfaceCapabilitiesKHR capabilities,
	uint32_t width,
	uint32_t height) {
	// If currentExtent is NOT set to the max value of uint32_t,
	// it means the surface size is determined by the window manager.
	if (capabilities.currentExtent.width != SPUD_UINT32_MAX) return capabilities.currentExtent;
	VkExtent2D actualExtent = {width, height};
	if (actualExtent.width < capabilities.minImageExtent.width)
		actualExtent.width = capabilities.minImageExtent.width;
	if (actualExtent.width > capabilities.maxImageExtent.width)
		actualExtent.width = capabilities.maxImageExtent.width;
	if (actualExtent.height < capabilities.minImageExtent.height)
		actualExtent.height = capabilities.minImageExtent.height;
	if (actualExtent.height > capabilities.maxImageExtent.height)
		actualExtent.height = capabilities.maxImageExtent.height;
	return actualExtent;
}


// SPUD_SUCCESS, SPUDRESULT_OUT_OF_MEMORY when an array of SpudGPU's own
// can't be made, SPUDRESULT_API_SPECIFIC_FAILURE when Vulkan fails. What it
// made before failing stays in [pSwapChain], for
// spudgpuvulkan___release_swap_chain_internal.
SPUDRESULT spudgpuvulkan___create_swapchain_internal(
	spudgpu_swap_chain_vulkan *pSwapChain,
	uint32_t width,
	uint32_t height,
	VkSwapchainKHR oldHandle) {
	VkResult result = VK_SUCCESS;
	const SPUD_ALLOCATOR *allocator = &pSwapChain->head.allocator;
	VkPhysicalDevice physicalDevice = pSwapChain->_device._physical_device_vk;
	VkDevice logicalDevice = pSwapChain->_device._logical_device_vk;
	// Query capabilities
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR surfaceFormat;
	VkPresentModeKHR presentMode;
	// Chained only when it names a scaling (scalingBehavior not 0).
	VkSwapchainPresentScalingCreateInfoKHR scalingInfo = {0};
	scalingInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_KHR;
	{
		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pSwapChain->_surface_vk, &capabilities);
		if (result != VK_SUCCESS) return SPUDRESULT_API_SPECIFIC_FAILURE;

		VkSurfaceFormatKHR *pSurfaceFormats = NULL;
		uint32_t surfaceFormatCount = 0;
		uint32_t surfaceFormatCapacity = 0;
		if (!spudgpuvulkan___get_available_formats_internal(pSwapChain, &pSurfaceFormats, &surfaceFormatCount, &surfaceFormatCapacity))
			return SPUDRESULT_OUT_OF_MEMORY;
		surfaceFormat = spudgpuvulkan___choose_surface_format_internal(
			pSurfaceFormats, surfaceFormatCount,
			convert_spud_to_vulkan_format(pSwapChain->_desc.format));
		SPUD_DELETE_ARRAY(allocator, VkSurfaceFormatKHR, pSurfaceFormats, surfaceFormatCapacity);

		VkPresentModeKHR *pPresentModes = NULL;
		uint32_t presentModeCount = 0;
		uint32_t presentModeCapacity = 0;
		if (!spudgpuvulkan___get_available_present_modes_internal(allocator, physicalDevice, pSwapChain->_surface_vk,
		                                                          &pPresentModes, &presentModeCount, &presentModeCapacity))
			return SPUDRESULT_OUT_OF_MEMORY;
		presentMode = spudgpuvulkan___choose_present_mode_internal(pPresentModes, presentModeCount, pSwapChain->_desc.present_mode);
		SPUD_DELETE_ARRAY(allocator, VkPresentModeKHR, pPresentModes, presentModeCapacity);

		// The extents the swap chain's size is chosen within: the surface's,
		// unless a scaling below says otherwise.
		VkSurfaceCapabilitiesKHR extentLimits = capabilities;
		// The present scaling, which spudgpu_create_swap_chain has checked
		// against spudgpu_get_surface_present_scalings' answer: STRETCH and
		// ASPECT_FIT are only in it where the surface offers them for
		// presentMode. ONE_TO_ONE goes through the extension where the
		// surface offers it, and is otherwise no scaling at the surface's
		// size (spudgpuvulkan___surface_present_scalings_internal).
		if (pSwapChain->_device._swapchain_maintenance1) {
			VkSurfacePresentScalingCapabilitiesKHR scalingCapabilities;
			SPUDRESULT sr = spudgpuvulkan___scaling_capabilities_internal(&pSwapChain->_device, pSwapChain->_surface_vk, presentMode,
			                                                              &scalingCapabilities);
			if (sr != SPUD_SUCCESS) return sr;
			switch (pSwapChain->_desc.present_scaling) {
				case SPUDGPU_PRESENT_SCALING_ONE_TO_ONE:
					if (spudgpuvulkan___one_to_one_offered_internal(&scalingCapabilities)) {
						scalingInfo.scalingBehavior = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR;
						scalingInfo.presentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_KHR;
						scalingInfo.presentGravityY = VK_PRESENT_GRAVITY_MIN_BIT_KHR;
					}
					break;
				case SPUDGPU_PRESENT_SCALING_STRETCH:
					scalingInfo.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_KHR;
					break;
				case SPUDGPU_PRESENT_SCALING_ASPECT_FIT:
					scalingInfo.scalingBehavior = VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_KHR;
					scalingInfo.presentGravityX = VK_PRESENT_GRAVITY_CENTERED_BIT_KHR;
					scalingInfo.presentGravityY = VK_PRESENT_GRAVITY_CENTERED_BIT_KHR;
					break;
				default:
					break;
			}
			// A scaled swap chain's extent is the caller's, within the scaled
			// range, rather than the surface's currentExtent
			// (VUID-VkSwapchainCreateInfoKHR-pNext-07782). The special value
			// (0xFFFFFFFF) means the surface takes the swap chain's size, as
			// an unscaled one with no currentExtent does.
			if (scalingInfo.scalingBehavior != 0 && scalingCapabilities.minScaledImageExtent.width != SPUD_UINT32_MAX) {
				extentLimits.currentExtent.width  = SPUD_UINT32_MAX;
				extentLimits.currentExtent.height = SPUD_UINT32_MAX;
				extentLimits.minImageExtent       = scalingCapabilities.minScaledImageExtent;
				extentLimits.maxImageExtent       = scalingCapabilities.maxScaledImageExtent;
			}
		}
		pSwapChain->_extent_vk = spudgpuvulkan___choose_extent_internal(extentLimits, width, height);
		// The surface can dictate the extent (currentExtent), overriding the
		// requested size - report what the back buffers really are.
		pSwapChain->_desc.width  = pSwapChain->_extent_vk.width;
		pSwapChain->_desc.height = pSwapChain->_extent_vk.height;
		pSwapChain->_format_vk = surfaceFormat.format;
	}

	// Determine image count
	uint32_t imageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		imageCount = capabilities.maxImageCount;

	// Pick the best supported composite alpha mode — prefer OPAQUE (the
	// window isn't meant to be blended with what's behind it), falling back
	// to whatever the surface actually supports if OPAQUE isn't available.
	static const VkCompositeAlphaFlagBitsKHR compositeAlphaCandidates[] = {
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
	};
	VkCompositeAlphaFlagBitsKHR compositeAlpha = compositeAlphaCandidates[0];
	for (size_t i = 0; i < sizeof(compositeAlphaCandidates) / sizeof(compositeAlphaCandidates[0]); i++) {
		if (capabilities.supportedCompositeAlpha & compositeAlphaCandidates[i]) {
			compositeAlpha = compositeAlphaCandidates[i];
			break;
		}
	}

	// Populate Creation Info
	VkSwapchainCreateInfoKHR createInfo = {0};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.pNext = scalingInfo.scalingBehavior != 0 ? &scalingInfo : NULL;
	createInfo.surface = pSwapChain->_surface_vk;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = pSwapChain->_extent_vk;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	{
		SPUDGPU_IMAGE_USAGE usage = pSwapChain->_desc.usage;
		if (usage & SPUDGPU_IMAGE_USAGE_TRANSFER_SRC)
			createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (usage & SPUDGPU_IMAGE_USAGE_TRANSFER_DST)
			createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (usage & SPUDGPU_IMAGE_USAGE_SAMPLED)
			createInfo.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (usage & SPUDGPU_IMAGE_USAGE_STORAGE)
			createInfo.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
		// The caller asked for these; fail rather than silently drop one.
		if ((createInfo.imageUsage & capabilities.supportedUsageFlags) != createInfo.imageUsage)
			return SPUDRESULT_API_SPECIFIC_FAILURE;
	}
	createInfo.preTransform = capabilities.currentTransform;
	createInfo.compositeAlpha = compositeAlpha;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;

	// Pass the old swap chain handle when this function is called inside of recreate()
	// This is for Vulkan optimization
	createInfo.oldSwapchain = oldHandle;

	// Handle graphics vs presentation queue sharing here (Concurrent vs Exclusive)
	// ...

	result = vkCreateSwapchainKHR(logicalDevice, &createInfo, NULL, &pSwapChain->_swapchain_vk);
	if (result != VK_SUCCESS) {
		//throw std::runtime_error("failed to create swap chain!");
		pSwapChain->_swapchain_vk = VK_NULL_HANDLE;
		return SPUDRESULT_API_SPECIFIC_FAILURE;
	}

	// Retrieve handles to the images
	result = vkGetSwapchainImagesKHR(logicalDevice, pSwapChain->_swapchain_vk, &imageCount, NULL);
	if (result != VK_SUCCESS) return SPUDRESULT_API_SPECIFIC_FAILURE;

	// The count the array is made and freed with; the images past what the
	// second call fills, if any, are never read.
	pSwapChain->_swapchain_images_vk = SPUD_NEW_ARRAY_ZEROED(allocator, VkImage, imageCount);
	if (!pSwapChain->_swapchain_images_vk) return SPUDRESULT_OUT_OF_MEMORY;
	pSwapChain->_swapchain_images_count = imageCount; // Set once the array exists
	result = vkGetSwapchainImagesKHR(
		logicalDevice, pSwapChain->_swapchain_vk, &imageCount,
		pSwapChain->_swapchain_images_vk);
	if (result != VK_SUCCESS) return SPUDRESULT_API_SPECIFIC_FAILURE;
	// The images there really are, which an acquire may name any of: the
	// count spudgpu_get_swap_chain_desc() reports, as spudgpu.h says.
	pSwapChain->_desc.buffer_count = imageCount;

	return SPUD_SUCCESS;
}

// SPUD_SUCCESS, or SPUDRESULT_OUT_OF_MEMORY with nothing of its own left made.
SPUDRESULT spudgpuvulkan___fences_semaphores_swapchain_creation_internal(
	spudgpu_swap_chain_vulkan *pSwapChain) {
	// This is the CPU-GPU pipelining depth, not the swapchain image count —
	// deliberately independent of desc.buffer_count. The caller records into
	// a single shared command buffer/allocator per frame (not one per frame
	// in flight), so _in_flight_fences_vk[_current_frame] only actually
	// tracks that command buffer's previous submission if there is exactly
	// one slot: with buffer_count (e.g. 2) slots instead, alternate frames
	// wait on a fence that was never signaled by a real submission (still in
	// its initial signaled state), so the wait is a no-op and the shared
	// command buffer gets reset/re-recorded while the GPU may still be
	// executing it from the previous frame — see app_render (main.cpp),
	// which resets app.cmd_allocator/app.cmd_list right after acquire.
	uint32_t max_frames = 1;
	pSwapChain->_current_frame = 0;

	// render_finished_semaphores is sized/indexed by swapchain IMAGE index
	// (_current_image_index), not by _current_frame — it's waited on by
	// vkQueuePresentKHR, and the presentation engine can hold a semaphore in
	// use past the point where our fence signals GPU completion. With only
	// max_frames_in_flight (1) of them shared across desc.buffer_count (e.g.
	// 2) images, present("image 0", sem) followed by acquire+submit+present
	// for image 1 re-signals that same semaphore while the engine may still
	// be consuming it for image 0 (VUID-vkQueueSubmit-pSignalSemaphores-00067).
	uint32_t image_count = pSwapChain->_swapchain_images_count > 0 ? pSwapChain->_swapchain_images_count : max_frames;

	// All three arrays or none; the counts are set once all three exist, and
	// are what they are freed with.
	const SPUD_ALLOCATOR *allocator = &pSwapChain->head.allocator;
	spudgpu_semaphore_vulkan *image_available = SPUD_NEW_ARRAY_ZEROED(allocator, spudgpu_semaphore_vulkan, max_frames);
	spudgpu_semaphore_vulkan *render_finished = SPUD_NEW_ARRAY_ZEROED(allocator, spudgpu_semaphore_vulkan, image_count);
	VkFence *in_flight                        = SPUD_NEW_ARRAY_ZEROED(allocator, VkFence, max_frames);
	if (!image_available || !render_finished || !in_flight) {
		SPUD_DELETE_ARRAY(allocator, spudgpu_semaphore_vulkan, image_available, max_frames);
		SPUD_DELETE_ARRAY(allocator, spudgpu_semaphore_vulkan, render_finished, image_count);
		SPUD_DELETE_ARRAY(allocator, VkFence, in_flight, max_frames);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	pSwapChain->_image_available_semaphores       = image_available;
	pSwapChain->_render_finished_semaphores       = render_finished;
	pSwapChain->_in_flight_fences_vk              = in_flight;
	pSwapChain->_max_frames_in_flight             = max_frames;
	pSwapChain->_render_finished_semaphores_count = image_count;

	VkDevice vk_device = pSwapChain->_device._logical_device_vk;

	VkSemaphoreCreateInfo semInfo = {0};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo = {0};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // <-- CRITICAL: first frame must not block

	// The semaphores are handles the caller can name (the swap chain's
	// semaphore getters), so each has the swap chain's allocator.
	for (uint32_t i = 0; i < max_frames; i++) {
		pSwapChain->_image_available_semaphores[i].head.allocator = pSwapChain->head.allocator;
		SPUD_BIRTH(&pSwapChain->_image_available_semaphores[i]);
		pSwapChain->_image_available_semaphores[i]._device_vk = vk_device;
		vkCreateSemaphore(vk_device, &semInfo, NULL, &pSwapChain->_image_available_semaphores[i]._semaphore_vk);
		vkCreateFence(vk_device, &fenceInfo, NULL, &pSwapChain->_in_flight_fences_vk[i]);
	}

	for (uint32_t i = 0; i < image_count; i++) {
		pSwapChain->_render_finished_semaphores[i].head.allocator = pSwapChain->head.allocator;
		SPUD_BIRTH(&pSwapChain->_render_finished_semaphores[i]);
		pSwapChain->_render_finished_semaphores[i]._device_vk = vk_device;
		vkCreateSemaphore(vk_device, &semInfo, NULL, &pSwapChain->_render_finished_semaphores[i]._semaphore_vk);
	}
	return SPUD_SUCCESS;
}

/*
VkExtent2D swap_chain_vulkan::choose_extent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width,
                                            uint32_t height) {
	// If currentExtent is NOT set to the max value of uint32_t,
	// it means the surface size is determined by the window manager.
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		return capabilities.currentExtent;
	} else {
		// Otherwise, we manually clamp our window dimensions to the
		// min/max supported by the GPU surface.
		VkExtent2D actualExtent = {width, height};

		actualExtent.width = std::clamp(actualExtent.width,
		                                capabilities.minImageExtent.width,
		                                capabilities.maxImageExtent.width);

		actualExtent.height = std::clamp(actualExtent.height,
		                                 capabilities.minImageExtent.height,
		                                 capabilities.maxImageExtent.height);

		return actualExtent;
	}
}*/

// As spudgpuvulkan___create_swapchain_internal: what it made before failing
// stays in [pSwapChain].
SPUDRESULT spudgpuvulkan_create_image_views_internal(
	spudgpu_swap_chain_vulkan *pSwapChain,
	uint32_t swap_chain_image_view_count) {
	const SPUD_ALLOCATOR *allocator = &pSwapChain->head.allocator;
	pSwapChain->_swapchain_image_views_vk = SPUD_NEW_ARRAY_ZEROED(allocator, spudgpu_image_view_vulkan,
	                                                              swap_chain_image_view_count);
	if (!pSwapChain->_swapchain_image_views_vk) return SPUDRESULT_OUT_OF_MEMORY;
	pSwapChain->_swapchain_image_views_count = swap_chain_image_view_count; // Set once the array exists

	VkResult result = VK_SUCCESS;
	for (size_t i = 0; i < pSwapChain->_swapchain_images_count; i++) {
		VkImageViewCreateInfo createInfo = {0};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = pSwapChain->_swapchain_images_vk[i];

		// Treat the image as a standard 2D texture
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = pSwapChain->_format_vk; // Use the format chosen in chooseSurfaceFormat

		// Components allow you to remap color channels (swizzling)
		// VK_COMPONENT_SWIZZLE_IDENTITY means use the default mapping
		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		// SubresourceRange describes what the image's purpose is
		// and which parts of the image should be accessed
		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;

		spudgpu_image_view_vulkan *view = &pSwapChain->_swapchain_image_views_vk[i];
		// The view and its image are handles the caller can name, so each
		// has the swap chain's allocator.
		view->head.allocator = pSwapChain->head.allocator;
		SPUD_BIRTH(view);
		spudgpu_image_vulkan *img = SPUD_NEW_ZEROED(allocator, spudgpu_image_vulkan);
		if (!img) return SPUDRESULT_OUT_OF_MEMORY;
		img->head.allocator = pSwapChain->head.allocator;
		SPUD_BIRTH(img);
		img->_device = pSwapChain->_device;
		img->_image_vk = pSwapChain->_swapchain_images_vk[i];
		img->_format_vk = pSwapChain->_format_vk;
		view->_desc.parent_image = (spudgpu_image) img;
		result = vkCreateImageView(pSwapChain->_device._logical_device_vk, &createInfo, NULL,
		                           &pSwapChain->_swapchain_image_views_vk[i]._image_view_vk);
		if (result != VK_SUCCESS) {
			//throw std::runtime_error("failed to create image views!");
			pSwapChain->_swapchain_image_views_vk[i]._image_view_vk = VK_NULL_HANDLE;
			return SPUDRESULT_API_SPECIFIC_FAILURE;
		}


	}
	return SPUD_SUCCESS;
}


// Releases everything [swap_chain] holds but itself and its name: what a
// failed creation made so far, or all of it. Arrays not yet made are NULL
// with a count of 0, and Vulkan objects not made are VK_NULL_HANDLE, which
// Vulkan's destroy calls take.
static void spudgpuvulkan___release_swap_chain_internal(spudgpu_swap_chain_vulkan *swap_chain) {
	const SPUD_ALLOCATOR *allocator = &swap_chain->head.allocator;
	VkDevice dev = swap_chain->_device._logical_device_vk;
	// A destroyed swap chain's semaphores, images and views go to its
	// device's quarantine with it (spudstale.h); NULL on a create's failure
	// path.
	SPUD_QUARANTINE_KEEP(quarantine, swap_chain);

	for (uint32_t i = 0; i < swap_chain->_max_frames_in_flight; i++) {
		vkDestroySemaphore(dev, swap_chain->_image_available_semaphores[i]._semaphore_vk, NULL);
		vkDestroyFence(dev, swap_chain->_in_flight_fences_vk[i], NULL);
		SPUD_KILL(&swap_chain->_image_available_semaphores[i]);
		spud_string_free(allocator, swap_chain->_image_available_semaphores[i].head.debug_name);
	}
	// render_finished_semaphores is sized/indexed by swapchain image count, not
	// _max_frames_in_flight — see spudgpuvulkan___fences_semaphores_swapchain_creation_internal.
	for (uint32_t i = 0; i < swap_chain->_render_finished_semaphores_count; i++) {
		vkDestroySemaphore(dev, swap_chain->_render_finished_semaphores[i]._semaphore_vk, NULL);
		SPUD_KILL(&swap_chain->_render_finished_semaphores[i]);
		spud_string_free(allocator, swap_chain->_render_finished_semaphores[i].head.debug_name);
	}
	SPUD_RETIRE_ARRAY(quarantine, allocator, spudgpu_semaphore_vulkan, swap_chain->_image_available_semaphores, swap_chain->_max_frames_in_flight);
	SPUD_RETIRE_ARRAY(quarantine, allocator, spudgpu_semaphore_vulkan, swap_chain->_render_finished_semaphores, swap_chain->_render_finished_semaphores_count);
	SPUD_DELETE_ARRAY(allocator, VkFence, swap_chain->_in_flight_fences_vk, swap_chain->_max_frames_in_flight);

	for (uint32_t i = 0; i < swap_chain->_swapchain_image_views_count; i++) {
		spudgpu_image_vulkan *image = swap_chain->_swapchain_image_views_vk[i]._desc.parent_image;
		if (image) {
			SPUD_KILL(image);
			spud_string_free(allocator, image->head.debug_name);
			SPUD_RETIRE(quarantine, allocator, spudgpu_image_vulkan, image);
		}
		SPUD_KILL(&swap_chain->_swapchain_image_views_vk[i]);
		spud_string_free(allocator, swap_chain->_swapchain_image_views_vk[i].head.debug_name);
		vkDestroyImageView(dev, swap_chain->_swapchain_image_views_vk[i]._image_view_vk, NULL);
	}
	SPUD_RETIRE_ARRAY(quarantine, allocator, spudgpu_image_view_vulkan, swap_chain->_swapchain_image_views_vk, swap_chain->_swapchain_image_views_count);
	SPUD_DELETE_ARRAY(allocator, VkImage, swap_chain->_swapchain_images_vk, swap_chain->_swapchain_images_count);

	// The surface is owned by the caller — do not destroy it here.
	// The caller is responsible for calling spudgpu_destroy_surface separately.
	if (swap_chain->_swapchain_vk != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(dev, swap_chain->_swapchain_vk, NULL);
}


SPUDRESULT spudgpu_create_surface(
	spudgpu_instance instance,
	void *window_handle,
	void *display_handle,
	spudgpu_surface *out_surface) {
	if (!instance) return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (SPUD_STALE(instance, "spudgpu_create_surface", "instance")) return SPUDRESULT_GPU_INVALID_INSTANCE;
	if (!window_handle) return SPUDRESULT_GPU_INVALID_WINDOW_HANDLE;
#if !defined(SPUDLIB_PLATFORM_WIN32)
	// Only Wayland/X11 surfaces need the display connection; Win32 has none
	// (spudgpu.h: "Windows: unused (set to NULL)").
	if (!display_handle) return SPUDRESULT_GPU_INVALID_DISPLAY_HANDLE;
#endif
	if (!out_surface) return SPUDRESULT_NULL_OUTPUT_PARAMETER;

	spudgpu_surface_vulkan result = {0};
	result.head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(&result, &instance->_quarantine);
	result._instance = *instance;

	VkResult r = VK_SUCCESS;

#if defined(SPUDLIB_PLATFORM_WIN32)
	VkWin32SurfaceCreateInfoKHR ci = {0};
	ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	ci.hwnd = (HWND) window_handle;
	ci.hinstance = GetModuleHandle(NULL);
	r = vkCreateWin32SurfaceKHR(instance->_instance_vk, &ci, NULL, &result._surface_vk);

#elif defined(SPUDGPU_PLATFORM_WAYLAND)
	VkWaylandSurfaceCreateInfoKHR ci = {0};
	ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	ci.display = (struct wl_display *) display_handle;
	ci.surface = (struct wl_surface *) window_handle;
	r = vkCreateWaylandSurfaceKHR(instance->_instance_vk, &ci, NULL, &result._surface_vk);

#elif defined(SPUDGPU_PLATFORM_XLIB)
	VkXlibSurfaceCreateInfoKHR ci = {0};
	ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
	ci.dpy = (Display *) display_handle;
	ci.window = (Window)(uintptr_t) window_handle;
	r = vkCreateXlibSurfaceKHR(instance->_instance_vk, &ci, NULL, &result._surface_vk);

#else
#error "No platform defined."
#endif

	if (r != VK_SUCCESS) return SPUDRESULT_API_SPECIFIC_FAILURE;

	spudgpu_surface_vulkan *pResult = SPUD_NEW(&result.head.allocator, spudgpu_surface_vulkan);
	if (!pResult) {
		vkDestroySurfaceKHR(instance->_instance_vk, result._surface_vk, NULL);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(spudgpu_surface_vulkan));
	*out_surface = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_surface(spudgpu_surface surface) {
	if (!surface) return;
	if (SPUD_STALE(surface, "spudgpu_destroy_surface", "surface")) return;
	vkDestroySurfaceKHR(
		surface->_instance._instance_vk,
		surface->_surface_vk, NULL);
	SPUD_KILL(surface);
	const SPUD_ALLOCATOR allocator = surface->head.allocator;
	spud_string_free(&allocator, surface->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(surface), &allocator, spudgpu_surface_vulkan, surface);
}

spudgpu_surface spudgpu_create_surface_from_callback(
	spudgpu_instance instance,
	void *user_data,
	spudgpu_surface_create_fn create_fn) {
	if (!instance || !create_fn) return NULL;
	if (SPUD_STALE(instance, "spudgpu_create_surface_from_callback", "instance")) return NULL;

	spudgpu_surface_vulkan *result = SPUD_NEW_ZEROED(&instance->head.allocator, spudgpu_surface_vulkan);
	if (!result) return NULL;

	result->head.allocator = instance->head.allocator;
	SPUD_BIRTH_IN(result, &instance->_quarantine);
	result->_instance = *instance;

	bool ok = create_fn(
		(void *) instance->_instance_vk,
		user_data,
		(void *) &result->_surface_vk);

	if (!ok) {
		SPUD_DELETE(&instance->head.allocator, spudgpu_surface_vulkan, result);
		return NULL;
	}

	return result;
}


SPUDRESULT spudgpu_get_surface_present_scalings(
	spudgpu_device device,
	spudgpu_surface surface,
	uint32_t *out_scalings) {
	if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_get_surface_present_scalings", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
	const SPUDRESULT sr = spudgpu___internal_check_surface_present_scalings(device->_features, surface, out_scalings);
	if (sr != SPUD_SUCCESS) return sr;
	return spudgpuvulkan___surface_present_scalings_internal(device, surface->_surface_vk, out_scalings);
}

SPUDRESULT spudgpu_create_swap_chain(
	spudgpu_device device,
	const spudgpu_swap_chain_desc *desc,
	spudgpu_swap_chain *out_swap_chain) {
	if (!device) return SPUDRESULT_GPU_INVALID_DEVICE;
	if (SPUD_STALE(device, "spudgpu_create_swap_chain", "device")) return SPUDRESULT_GPU_INVALID_DEVICE;
	const SPUDRESULT desc_sr = spudgpu___internal_check_swap_chain_desc(device->_features, desc, out_swap_chain);
	if (desc_sr != SPUD_SUCCESS) return desc_sr;
	uint32_t scalings = 0;
	const SPUDRESULT scalings_sr = spudgpuvulkan___surface_present_scalings_internal(device, desc->surface->_surface_vk, &scalings);
	if (scalings_sr != SPUD_SUCCESS) return scalings_sr;
	if (!(scalings & desc->present_scaling)) return SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED;

	spudgpu_swap_chain_vulkan result = {0};
	result.head.allocator = device->head.allocator;
	SPUD_BIRTH_IN(&result, &device->_shared->quarantine);
	result._device = *((spudgpu_device_vulkan *) device);
	memcpy(&result._desc, desc, sizeof(spudgpu_swap_chain_desc));
	result._present_queue = (spudgpu_command_queue_vulkan *) desc->queue;

	if (desc->surface) {
		result._surface_vk = desc->surface->_surface_vk;
	}

	VkResult r = VK_SUCCESS;
	if (r != VK_SUCCESS) return SPUDRESULT_API_SPECIFIC_FAILURE;

	//result._desc = desc; Can't do this because of 'const'
	SPUDRESULT sr = spudgpuvulkan___create_swapchain_internal(&result, desc->width, desc->height, NULL);
	if (sr == SPUD_SUCCESS)
		sr = spudgpuvulkan_create_image_views_internal(&result, result._swapchain_images_count);
	if (sr == SPUD_SUCCESS)
		sr = spudgpuvulkan___fences_semaphores_swapchain_creation_internal(&result);
	if (sr != SPUD_SUCCESS) {
		spudgpuvulkan___release_swap_chain_internal(&result);
		return sr;
	}

	spudgpu_swap_chain_vulkan *pResult = SPUD_NEW(&result.head.allocator, spudgpu_swap_chain_vulkan);
	if (!pResult) {
		spudgpuvulkan___release_swap_chain_internal(&result);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	memcpy(pResult, &result, sizeof(spudgpu_swap_chain_vulkan));
	SPUDGPU___LIVE_ADD(&pResult->_device._shared->live, SPUDGPU___OBJECT_SWAP_CHAIN);
	*out_swap_chain = pResult;
	return SPUD_SUCCESS;
}

void spudgpu_destroy_swap_chain(spudgpu_swap_chain swap_chain) {
	if (!swap_chain) return;
	if (SPUD_STALE(swap_chain, "spudgpu_destroy_swap_chain", "swap_chain")) return;
	SPUDGPU___LIVE_REMOVE(&swap_chain->_device._shared->live, SPUDGPU___OBJECT_SWAP_CHAIN);
	// Dead first: what it releases goes to the quarantine with it.
	SPUD_KILL(swap_chain);
	spudgpuvulkan___release_swap_chain_internal(swap_chain);
	const SPUD_ALLOCATOR allocator = swap_chain->head.allocator;
	spud_string_free(&allocator, swap_chain->head.debug_name);
	SPUD_RETIRE(SPUD_QUARANTINE_OF(swap_chain), &allocator, spudgpu_swap_chain_vulkan, swap_chain);
}


SPUDRESULT spudgpu_get_swap_chain_desc(
	spudgpu_swap_chain swap_chain,
	spudgpu_swap_chain_desc *out_desc) {
	if (!swap_chain) return SPUDRESULT_GPU_INVALID_SWAP_CHAIN;
	if (SPUD_STALE(swap_chain, "spudgpu_get_swap_chain_desc", "swap_chain")) return SPUDRESULT_GPU_INVALID_SWAP_CHAIN;
	if (!out_desc) return SPUDRESULT_NULL_OUTPUT_PARAMETER;
	*out_desc = ((spudgpu_swap_chain_vulkan *) swap_chain)->_desc;
	out_desc->base = spud_struct_base(SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC);
	return SPUD_SUCCESS;
}

uint32_t spudgpu_swap_chain_acquire_next_image(spudgpu_swap_chain swap_chain) {
	if (!swap_chain) return SPUD_UINT32_MAX;
	if (SPUD_STALE(swap_chain, "spudgpu_swap_chain_acquire_next_image", "swap_chain")) return SPUD_UINT32_MAX;
	VkDevice device = swap_chain->_device._logical_device_vk;

	// Wait for the fence of the current frame slot, so we don't overwrite
	// GPU resources that are still in flight.
	vkWaitForFences(device, 1, &swap_chain->_in_flight_fences_vk[swap_chain->_current_frame],
	                VK_TRUE, SPUD_UINT64_MAX);

	// Ask the driver for the next available swapchain image.
	// Signals _image_available_semaphores[_current_frame] when the image is ready.
	VkResult result = vkAcquireNextImageKHR(
		device,
		swap_chain->_swapchain_vk,
		SPUD_UINT64_MAX, // No timeout — block until an image is available
		swap_chain->_image_available_semaphores[swap_chain->_current_frame]._semaphore_vk,
		VK_NULL_HANDLE,
		&swap_chain->_current_image_index);

	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		// Caller should handle swap chain recreation on VK_ERROR_OUT_OF_DATE_KHR.
		// Do NOT reset the fence here: _current_frame only advances on a
		// successful present (see below), so a caller that bails out on a
		// failed acquire submits nothing that
		// would ever re-signal this fence. Resetting it unconditionally,
		// before knowing the acquire would succeed, left it permanently
		// unsignaled the moment a resize made the swap chain go out of
		// date — and the very next acquire's wait above would then block
		// forever on that same fence, freezing the render thread.
		return SPUD_UINT32_MAX;
	}

	// Only reset now that we know real work is about to be submitted for
	// this frame, which will eventually re-signal the fence.
	vkResetFences(device, 1, &swap_chain->_in_flight_fences_vk[swap_chain->_current_frame]);

	return swap_chain->_current_image_index;
}

void spudgpu_swap_chain_present(spudgpu_swap_chain swap_chain) {
	if (!swap_chain) return;
	if (SPUD_STALE(swap_chain, "spudgpu_swap_chain_present", "swap_chain")) return;

	// Tell the presentation engine to display _current_image_index.
	// Wait on _render_finished_semaphores[_current_image_index] — indexed by
	// swapchain image, not frame-in-flight slot, since this semaphore is
	// specifically what the present engine consumes for this image.
	VkSemaphore waitSemaphores[] = {
		swap_chain->_render_finished_semaphores[swap_chain->_current_image_index]._semaphore_vk
	};

	VkPresentInfoKHR presentInfo = {0};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = waitSemaphores;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swap_chain->_swapchain_vk;
	presentInfo.pImageIndices = &swap_chain->_current_image_index;
	presentInfo.pResults = NULL; // Optional per-swapchain result

	// Present against the queue given as spudgpu_swap_chain_desc::queue at
	// creation, not a freshly re-derived "the graphics queue" guess - the
	// caller is required to have submitted this frame's rendering there too.
	// A use of the queue like a submit: the caller's to synchronize.
	spudgpuvulkan___queue_enter(swap_chain->_present_queue);
	vkQueuePresentKHR(swap_chain->_present_queue->_queue_vk, &presentInfo);
	spudgpuvulkan___queue_leave(swap_chain->_present_queue);

	// Advance the frame-in-flight cursor
	swap_chain->_current_frame = (swap_chain->_current_frame + 1) % swap_chain->_max_frames_in_flight;
}

spudgpu_image_view spudgpu_get_swap_chain_image_view(
	spudgpu_swap_chain swap_chain,
	uint32_t image_index) {
	if (!swap_chain) return NULL;
	if (SPUD_STALE(swap_chain, "spudgpu_get_swap_chain_image_view", "swap_chain")) return NULL;
	if (image_index >= swap_chain->_swapchain_image_views_count) return NULL;
	return &swap_chain->_swapchain_image_views_vk[image_index];
}

spudgpu_semaphore spudgpu_swap_chain_get_image_available_semaphore(spudgpu_swap_chain swap_chain) {
	if (!swap_chain) return NULL;
	if (SPUD_STALE(swap_chain, "spudgpu_swap_chain_get_image_available_semaphore", "swap_chain")) return NULL;
	return &swap_chain->_image_available_semaphores[swap_chain->_current_frame];
}

spudgpu_semaphore spudgpu_swap_chain_get_render_finished_semaphore(spudgpu_swap_chain swap_chain) {
	if (!swap_chain) return NULL;
	if (SPUD_STALE(swap_chain, "spudgpu_swap_chain_get_render_finished_semaphore", "swap_chain")) return NULL;
	// Indexed by swapchain image, not frame-in-flight slot — see spudgpu_swap_chain_present.
	return &swap_chain->_render_finished_semaphores[swap_chain->_current_image_index];
}


#if __cplusplus
}
#endif

#endif // SPUDGPU_COMPILE_VULKAN_API
