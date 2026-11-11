//
// Created by nathanmoore on 5/18/26.
//

#if CRGPU_COMPILE_VULKAN_API

#ifndef CARROTTUS_CRGPUVULKAN_H
#define CARROTTUS_CRGPUVULKAN_H

#include "crgpu.h"
#include "../../../crallocator.h"
#include "../../../crthread.h"
#include "../../crgpushared.h"
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <string.h>

typedef struct crgpu_instance_t {
    cr_object_head head;
    VkInstance _instance_vk;
    // The adapters listed when the instance was made, which never change,
    // freed with it. Each surface keeps a copy of this struct (_instance),
    // taken after the list was made.
    crgpu_adapter *_adapters;
    uint32_t _adapter_count;
    // Which of VK_KHR_surface_maintenance1 and its EXT original are enabled
    // (each with VK_KHR_get_surface_capabilities2), where the loader has
    // them: an adapter's VK_KHR/EXT_swapchain_maintenance1 needs its own.
    bool _surface_maintenance1_khr;
    bool _surface_maintenance1_ext;
    // VK_KHR_get_surface_capabilities2's, loaded when either is enabled;
    // NULL otherwise.
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR _vkGetPhysicalDeviceSurfaceCapabilities2KHR;
#if _DEBUG
    // Its destroyed devices' and surfaces' blocks, freed with it
    // (crstale.h, card X27). A surface's copy of this struct never
    // touches its own.
    cr_quarantine _quarantine;
#endif
} crgpu_instance_vulkan;

typedef struct crgpu_adapter_t {
    cr_object_head head;
    VkPhysicalDevice _physical_device_vk;
    // Read when the instance was made and never changed.
    crgpu___adapter_facts _facts;
    VkPhysicalDeviceProperties _properties_vk;
    // The queue family each kind of queue is made from, by the rule in
    // crgpu.h (crgpu_device_desc::direct_queue_count): direct from the
    // first family with graphics; compute from the first with compute and
    // without graphics, else the direct family; copy from the first with
    // transfer and neither graphics nor compute, else the compute family.
    uint32_t _family_direct;
    uint32_t _family_compute;
    uint32_t _family_copy;
    // VK_KHR_swapchain_maintenance1 or VK_EXT_swapchain_maintenance1, the
    // first the adapter lists with swapchainMaintenance1 and the instance
    // has the surface extension for; NULL for neither. A device created
    // with CRGPU_FEATURE_PRESENT enables it, for its present scalings.
    const char *_swapchain_maintenance1_extension;
    // The instance's, copied with _swapchain_maintenance1_extension (NULL
    // without it): a device reads its surfaces' scalings through it.
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR _vkGetPhysicalDeviceSurfaceCapabilities2KHR;
    // Whether a device exists on the adapter: one at a time, claimed with an
    // exchange by crgpu_device_create and given back by
    // crgpu_device_destroy. The adapter is shared, hidden state, so this is
    // Carrottus GPU's to guard.
    atomic_bool _in_use;
#if _DEBUG
    // The instance that listed it, whose quarantine takes its destroyed
    // devices (crstale.h, card X27).
    struct crgpu_instance_t *_debug_instance;
#endif
} crgpu_adapter_vulkan;

// What a device keeps that changes while it lives: its descriptor budget,
// and in a debug build its live objects. Every struct below holds the device
// by value, so this is allocated once, with the device, and every copy of
// the device reaches the same one through its pointer; a mutex in it is
// never copied.
typedef struct crgpu_device_shared_vulkan {
    crgpu___descriptor_budget budget;
#if _DEBUG
    crgpu___live_objects live;
    // The blocks of the objects made from the device and destroyed, freed
    // with it (crstale.h, card X27).
    cr_quarantine quarantine;
#endif
} crgpu_device_shared_vulkan;

#define CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS 4096

// Made once, with the device (crgpu_enumerate_devices); NULL if that failed,
// and then every bindless call reports bindless unsupported. Only ever reached
// through the device's pointer, never copied.
typedef struct crgpu_bindless_state_vulkan {
    crgpu_descriptor_set_layout layout; // Wraps the single global layout.
    VkDescriptorPool pool_vk;
    VkDescriptorSet set_vk;

    // Guards everything below, and every vkUpdateDescriptorSets of set_vk
    // (the set is externally synchronized in Vulkan): registering and
    // unregistering may be done from any thread.
    cr_mutex lock;

    uint32_t sampled_image_next_unused;
    uint32_t storage_image_next_unused;
    uint32_t storage_buffer_next_unused;

    // Stack of freed indices, reused before drawing from *_next_unused.
    uint32_t sampled_image_free_stack[CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS];
    uint32_t sampled_image_free_count;
    uint32_t storage_image_free_stack[CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS];
    uint32_t storage_image_free_count;
    uint32_t storage_buffer_free_stack[CRGPU_BINDLESS_MAX_SLOTS_PER_CLASS];
    uint32_t storage_buffer_free_count;
} crgpu_bindless_state_vulkan;

typedef struct crgpu_device_t {
    cr_object_head head;
    VkDevice _logical_device_vk;
    VkPhysicalDevice _physical_device_vk;
    crgpu_adapter_vulkan *_adapter;
    // crgpu_device_desc::required_features: what every call checks.
    CRGPU_FEATURES _features;
    // Whether the device enabled its adapter's
    // _swapchain_maintenance1_extension: swap chains may then name a
    // VkSwapchainPresentScalingCreateInfoKHR.
    bool _swapchain_maintenance1;
    // The adapter's, copied for the limits the backend reads.
    VkPhysicalDeviceProperties _properties_vk;
    // The family direct queues are made from (the adapter's _family_direct).
    uint32_t _graphics_queue_family_index;

    // NULL without CRGPU_FEATURE_MESH_SHADING. Loaded via
    // vkGetDeviceProcAddr since VK_EXT_mesh_shader's entry points don't come
    // statically linked the way VK_KHR_swapchain's do.
    PFN_vkCmdDrawMeshTasksEXT _vkCmdDrawMeshTasksEXT;
    // NULL when the instance has no VK_EXT_debug_utils
    // (crgpu_command_list_begin_debug_label records nothing then).
    PFN_vkCmdBeginDebugUtilsLabelEXT _vkCmdBeginDebugUtilsLabelEXT;
    PFN_vkCmdEndDebugUtilsLabelEXT _vkCmdEndDebugUtilsLabelEXT;
    // VK_KHR_acceleration_structure's, loaded through vkGetDeviceProcAddr
    // with the device; all NULL without CRGPU_FEATURE_RAY_QUERY, and no
    // call reaches them then (crgpuvulkanraytracing.c refuses first).
    PFN_vkCreateAccelerationStructureKHR _vkCreateAccelerationStructureKHR;
    PFN_vkDestroyAccelerationStructureKHR _vkDestroyAccelerationStructureKHR;
    PFN_vkGetAccelerationStructureBuildSizesKHR _vkGetAccelerationStructureBuildSizesKHR;
    PFN_vkGetAccelerationStructureDeviceAddressKHR _vkGetAccelerationStructureDeviceAddressKHR;
    PFN_vkCmdBuildAccelerationStructuresKHR _vkCmdBuildAccelerationStructuresKHR;
    PFN_vkCmdCopyAccelerationStructureKHR _vkCmdCopyAccelerationStructureKHR;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR _vkCmdWriteAccelerationStructuresPropertiesKHR;

    // Made with the device when CRGPU_FEATURE_BINDLESS is asked for, before
    // it is first copied (every struct below embeds crgpu_device_vulkan by
    // value), and never replaced, so every copy holds the same pointer. NULL
    // exactly when the feature was not asked for.
    crgpu_bindless_state_vulkan *_bindless;
    // The queues the desc asked for, by kind, each array as long as its
    // count (NULL for 0): made with the device and freed with it.
    struct crgpu_command_queue_t **_cmd_queues_direct;
    uint32_t _cmd_queue_count_direct;
    struct crgpu_command_queue_t **_cmd_queues_compute;
    uint32_t _cmd_queue_count_compute;
    struct crgpu_command_queue_t **_cmd_queues_copy;
    uint32_t _cmd_queue_count_copy;
    // See crgpu_device_shared_vulkan.
    crgpu_device_shared_vulkan *_shared;
} crgpu_device_vulkan;

// The largest draw_count one indirect draw on [device] takes:
// CRGPU_ADAPTER_PROPERTIES::max_draw_indirect_count with
// CRGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1 (Vulkan's own rule without
// multiDrawIndirect).
static inline uint32_t crgpuvulkan___max_draw_indirect_count(const crgpu_device_vulkan *device) {
    return (device->_features & CRGPU_FEATURE_MULTI_DRAW_INDIRECT) ? device->_adapter->_facts.properties.max_draw_indirect_count : 1;
}

// One per VkQueue, owned by the device. Never copied: a swap chain keeps a
// pointer. A queue is the caller's to synchronize (crgpu.h,
// crgpu_command_queue_submit_lists), as Vulkan's own vkQueueSubmit,
// vkQueuePresentKHR and vkQueueWaitIdle require; nothing here locks it.
// A block a call carves its temporary arrays from (Vulkan's barriers,
// rectangles, handles, the values of a submission), kept with its capacity
// on the object that owns it, a command list or a queue, from call to call:
// a steady stream of calls allocates only until the largest has been seen.
// Words of 8 bytes, the most any array carved from it needs aligned to.
// The owner is used by one caller at a time (crgpu.h), so one block is
// enough; nothing in it outlives the call.
typedef EQ_ARRAY(uint64_t) crgpuvulkan___scratch;

// Rounds a byte count up to the scratch block's words: where the next array
// carved from it starts.
#define CRGPUVULKAN___SCRATCH_ROUND(bytes) (((bytes) + 7u) & ~(size_t)7u)

// [bytes] of [scratch], zeroed, whatever it held before gone: NULL if it
// can't grow that far.
static inline void *crgpuvulkan___scratch_take(crgpuvulkan___scratch *scratch, size_t bytes) {
    size_t words = CRGPUVULKAN___SCRATCH_ROUND(bytes) / 8u;
    if (words == 0) words = 1;
    if (eq_array_reserve(scratch, words) != EQ_SUCCESS) return NULL;
    memset(scratch->data, 0, words * 8u);
    return scratch->data;
}

typedef struct crgpu_command_queue_t {
    cr_object_head head;
    VkQueue _queue_vk;
    uint32_t _queue_family_index;
    // A submission's arrays (crgpuvulkan___scratch), on _eq_allocator,
    // eqstd's view of head.allocator.
    EQ_ALLOCATOR _eq_allocator;
    crgpuvulkan___scratch _scratch;
#if _DEBUG
    // How many threads are in a call that uses the queue: a debug build
    // asserts it is never more than one (crgpuvulkan___queue_enter).
    atomic_int _debug_users;
#endif
} crgpu_command_queue_vulkan;

// Brackets every Carrottus GPU call that uses a queue (a submit, a present, a
// wait-idle). A debug build asserts that no other thread is in one on the
// same queue: the caller broke the rule crgpu.h states, and on Vulkan that
// is undefined behaviour. Nothing in a release build.
#if _DEBUG
static inline void crgpuvulkan___queue_enter(crgpu_command_queue_vulkan *queue) {
    const int others = atomic_fetch_add(&queue->_debug_users, 1);
    assert(others == 0 && "crgpu: two threads used one command queue at once; the caller synchronizes a queue");
    (void) others;
}
static inline void crgpuvulkan___queue_leave(crgpu_command_queue_vulkan *queue) {
    atomic_fetch_sub(&queue->_debug_users, 1);
}
#else
static inline void crgpuvulkan___queue_enter(crgpu_command_queue_vulkan *queue) { (void) queue; }
static inline void crgpuvulkan___queue_leave(crgpu_command_queue_vulkan *queue) { (void) queue; }
#endif

typedef struct crgpu_command_allocator_t {
    cr_object_head head;
    VkCommandPool _command_pool_vk;
    crgpu_device_vulkan _device;
    uint32_t _queue_family_index;
    // CRGPU_COMMAND_LIST_TYPE_BUNDLE allocators hand out
    // VK_COMMAND_BUFFER_LEVEL_SECONDARY buffers (see crgpu_command_list_create);
    // every other type hands out PRIMARY.
    CRGPU_COMMAND_LIST_TYPE _type;
#if _DEBUG
    // The list of this allocator that is open for recording, or NULL. One
    // at a time on every backend (crgpu_command_list_begin): Vulkan allows
    // more, D3D12 does not, and a debug build asserts so the rule is caught
    // here too. Not atomic: an allocator is used from one thread at a time.
    crgpu_command_list _open_list;
#endif
} crgpu_command_allocator_vulkan;

typedef struct crgpu_command_list_t {
    cr_object_head head;
    VkCommandBuffer _command_buffer_vk;
    crgpu_command_allocator_vulkan _allocator;
    // A recording call's arrays (crgpuvulkan___scratch), on _eq_allocator,
    // eqstd's view of head.allocator.
    EQ_ALLOCATOR _eq_allocator;
    crgpuvulkan___scratch _scratch;
#if _DEBUG
    // _allocator is a copy; this is the allocator itself, whose _open_list
    // the list's begin and end keep.
    crgpu_command_allocator_vulkan *_debug_allocator;
#endif

    // Set by crgpu_command_list_begin_rendering, cleared by crgpu_command_list_end_rendering
    // (crgpuvulkanrenderpass.c). Lets crgpu_command_list_clear_color_attachment/
    // depth_attachment take the cheap vkCmdClearAttachments fast path — valid
    // only against attachments already bound in the currently-active
    // rendering instance, addressed by index, not by view identity — instead
    // of nesting a redundant begin/end pair, which would be an outright
    // Vulkan validation error regardless of cost: vkCmdBeginRendering cannot
    // be called while a rendering instance is already active on this command
    // buffer.
    bool _rendering_active;
    // VkRenderingInfo::layerCount of the pass being recorded.
    uint32_t _bound_layer_count;
    VkImageView _bound_color_attachments[CRGPU_MAX_COLOR_ATTACHMENTS];
    uint32_t _bound_color_attachment_count;
    VkImageView _bound_depth_attachment_view; // VK_NULL_HANDLE if none bound
    // The timestamp the open pass writes at crgpu_command_list_end_rendering
    // (crgpu_rendering_begin_desc::timestamp_set); VK_NULL_HANDLE for none.
    VkQueryPool _end_timestamp_pool_vk;
    uint32_t _end_timestamp_index;
} crgpu_command_list_vulkan;

typedef struct crgpu_timestamp_set_t {
    cr_object_head head;
    VkQueryPool _query_pool_vk;
    VkDevice _logical_device_vk;
    uint32_t _count;
    // VkPhysicalDeviceLimits::timestampPeriod: nanoseconds a tick.
    float _period_ns;
    // The device's, for its live count.
    crgpu_device_shared_vulkan *_shared;
} crgpu_timestamp_set_vulkan;

typedef struct crgpu_buffer_t {
    cr_object_head head;
    VkBuffer _buffer_vk;
    // The buffer's own allocation, or its heap's when _heap is set: the
    // buffer is bound at _memory_offset_vk of it (0 for its own) and covers
    // _memory_size_vk bytes from there, which may be more than _desc.size.
    // Those and the property flags of the memory type are what
    // crgpu_buffer_flush/crgpu_buffer_invalidate need to form a range
    // Vulkan accepts.
    VkDeviceMemory _memory_vk;
    VkDeviceSize _memory_offset_vk;
    VkDeviceSize _memory_size_vk;
    VkMemoryPropertyFlags _memory_properties_vk;
    crgpu_buffer_desc _desc;
    // vkGetBufferDeviceAddress, read once at creation.
    VkDeviceAddress _gpu_address_vk;
    crgpu_device_vulkan _device;
    // NULL for a buffer with its own allocation; the heap it was placed in
    // otherwise (crgpu_buffer_create_placed), whose memory it never frees.
    crgpu_heap _heap;
} crgpu_buffer_vulkan;

typedef struct crgpu_buffer_view_t {
    cr_object_head head;
    VkBufferView _buffer_view_vk;
    crgpu_buffer_view_desc _desc;
    // The buffer's device's, kept here because a view may be destroyed after
    // its buffer (its live count is the device's).
    crgpu_device_shared_vulkan *_shared;
} crgpu_buffer_view_vulkan;

typedef struct crgpu_image_t {
    cr_object_head head;
    VkImage _image_vk;
    // The image's own allocation, or its heap's when _heap is set.
    VkDeviceMemory _memory_vk;
    VkFormat _format_vk;
    crgpu_image_desc _desc;
    crgpu_device_vulkan _device;
    // NULL for an image with its own allocation; the heap it was placed in
    // otherwise (crgpu_image_create_placed), whose memory it never frees.
    crgpu_heap _heap;
} crgpu_image_vulkan;

typedef struct crgpu_heap_t {
    cr_object_head head;
#if _DEBUG
    // Buffers and images placed in the heap and not yet destroyed.
    // crgpu_heap_destroy asserts it is 0. Atomic, because resources in
    // one heap may be placed and destroyed on several threads at once.
    _Atomic uint64_t _placed_count;
#endif
    VkDeviceMemory _memory_vk;
    // The memory type the heap was allocated from (see crgpuvulkanheap.c
    // for how it is chosen), and its property flags.
    uint32_t _memory_type_index_vk;
    VkMemoryPropertyFlags _memory_properties_vk;
    // The whole allocation, mapped from creation to destruction, for a heap
    // of a mappable kind; NULL for CRGPU_MEMORY_KIND_DEVICE. Vulkan allows
    // one mapping of a VkDeviceMemory at a time, so the heap maps it once
    // and its buffers' crgpu_buffer_map hands out pointers into it.
    void *_mapped;
    crgpu_heap_desc _desc;
    crgpu_device_vulkan _device;
} crgpu_heap_vulkan;

typedef struct crgpu_image_view_t {
    cr_object_head head;
    VkImageView _image_view_vk;
    crgpu_image_view_desc _desc;
    // The image's device's, kept here because a view may be destroyed after
    // its image (its live count is the device's). NULL for a swap chain's own
    // views, which the swap chain frees.
    crgpu_device_shared_vulkan *_shared;
} crgpu_image_view_vulkan;

typedef struct crgpu_surface_t {
    cr_object_head head;
    VkSurfaceKHR _surface_vk;
    crgpu_instance_vulkan _instance;
} crgpu_surface_vulkan;

typedef struct crgpu_fence_t {
    cr_object_head head;
    // A timeline semaphore, not a VkFence: crgpu_fence is a counter that
    // only goes up, which is what a timeline semaphore is and a VkFence
    // (one bit, reset by hand) is not.
    VkSemaphore _semaphore_vk;
    VkDevice _device_vk;
    // The device's, for its live count.
    crgpu_device_shared_vulkan *_shared;
} crgpu_fence_vulkan;

typedef struct crgpu_semaphore_t {
    cr_object_head head;
    VkSemaphore _semaphore_vk;
    VkDevice _device_vk;
    // The device's, for its live count. NULL for a swap chain's own
    // semaphores, which the swap chain frees.
    crgpu_device_shared_vulkan *_shared;
} crgpu_semaphore_vulkan;

typedef struct crgpu_swap_chain_t {
    cr_object_head head;
    VkSwapchainKHR _swapchain_vk;
    crgpu_swap_chain_desc _desc;
    crgpu_device_vulkan _device;
    // desc.queue, set at creation - see crgpu_swap_chain_desc::queue.
    crgpu_command_queue_vulkan *_present_queue;
    VkSurfaceKHR _surface_vk;
    VkFormat _format_vk;
    VkExtent2D _extent_vk;
    VkImage *_swapchain_images_vk;
    uint32_t _swapchain_images_count;
    crgpu_image_view_vulkan *_swapchain_image_views_vk;
    uint32_t _swapchain_image_views_count;

    // Made and freed with the counts beside them: _swapchain_images_count
    // images, _swapchain_image_views_count views (each with its own image
    // handle as parent), _max_frames_in_flight image-available semaphores
    // and in-flight fences, _render_finished_semaphores_count
    // render-finished semaphores.
    crgpu_semaphore_vulkan *_image_available_semaphores; // One per frame-in-flight
    crgpu_semaphore_vulkan *_render_finished_semaphores; // One per swap chain image
    uint32_t _render_finished_semaphores_count;
    // One per frame-in-flight. Plain VkFences, internal to the swap chain:
    // signaled by a submission made with it, waited on and reset by acquire.
    VkFence *_in_flight_fences_vk;
    uint32_t _max_frames_in_flight;
    uint32_t _current_frame; // Cycles 0..max_frames_in_flight-1
    uint32_t _current_image_index; // Set by acquire, read by present
} crgpu_swap_chain_vulkan;

typedef struct crgpu_shader_module_t {
    cr_object_head head;
    VkShaderModule _shader_module_vk;
    VkShaderStageFlagBits _stage_vk;
    // The desc's entry point, copied through head.allocator: what a
    // pipeline names as pName.
    const char *_entry_point;
    crgpu_device_vulkan _device;
} crgpu_shader_module_vulkan;


typedef struct crgpu_graphics_pipeline_t {
    cr_object_head head;
    VkPipeline _pipeline_vk;
    VkPipelineLayout _pipeline_layout_vk;
    crgpu_device_vulkan _device;
    crgpu_graphics_pipeline_desc _desc;
} crgpu_shader_pipeline_vulkan;

typedef struct crgpu_compute_pipeline_t {
    cr_object_head head;
    VkPipeline _pipeline_vk;
    VkPipelineLayout _pipeline_layout_vk;
    crgpu_device_vulkan _device;
    crgpu_compute_pipeline_desc _desc;
} crgpu_compute_pipeline_vulkan;

typedef struct crgpu_sampler_t {
    cr_object_head head;
    VkSampler _sampler_vk;
    crgpu_device_vulkan _device;
} crgpu_sampler_vulkan;

typedef struct crgpu_descriptor_set_layout_t {
    cr_object_head head;
    VkDescriptorSetLayout _layout_vk;
    crgpu_device_vulkan _device;
    crgpu_descriptor_set_layout_desc _desc;
} crgpu_descriptor_set_layout_vulkan;

typedef struct crgpu_descriptor_pool_t {
    cr_object_head head;
    VkDescriptorPool _pool_vk;
    crgpu_device_vulkan _device;
    crgpu_descriptor_pool_desc _desc;
    // What the pool took out of its device's descriptor budget, given back
    // when it is destroyed.
    uint64_t _budget_descriptors;
    uint64_t _budget_samplers;
    // Every set handle made from the pool and not yet released, newest
    // first, linked through their _next. crgpu.h has the pool release its
    // sets when it is reset or destroyed, handles and all.
    struct crgpu_descriptor_set_t *_sets;
} crgpu_descriptor_pool_vulkan;

// An acceleration structure (crgpuvulkanraytracing.c): the VkBuffer that
// holds it, bound to memory of its own or to its heap's, and the
// VkAccelerationStructureKHR over the whole buffer. Its first two members
// are crgpu___acceleration_structure_common's (crgpushared.h), which the
// shared checks read. Hot on a build or a bind: _state (the first two cache
// lines, with the head), then _structure_vk and _address_vk on the third.
typedef struct crgpu_acceleration_structure_t {
    cr_object_head head;
    crgpu___acceleration_structure_state _state;
    VkAccelerationStructureKHR _structure_vk;
    // vkGetAccelerationStructureDeviceAddressKHR, read once at creation:
    // what an instance names a bottom level by.
    VkDeviceAddress _address_vk;
    VkBuffer _buffer_vk;
    // Its own allocation; VK_NULL_HANDLE for a placed structure, whose
    // memory is _heap's.
    VkDeviceMemory _memory_vk;
    crgpu_heap _heap;
    // The one query that holds its compacted size, made with it when the
    // desc allows compaction (VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR);
    // VK_NULL_HANDLE otherwise.
    VkQueryPool _compacted_size_query_vk;
    // The device a committed structure was made on, or for a placed one
    // the heap's copy of it (crgpu_heap_t::_device): each outlives
    // the structure, which is destroyed before its device and its heap.
    const crgpu_device_vulkan *_device;
} crgpu_acceleration_structure_vulkan;

typedef struct crgpu_descriptor_set_t {
    cr_object_head head;
    VkDescriptorSet _set_vk;
    // A copy of the pool when the set was made: its _sets is stale and
    // never read.
    crgpu_descriptor_pool_vulkan _pool;
    // The next set in the pool's _sets list.
    struct crgpu_descriptor_set_t *_next;
} crgpu_descriptor_set_vulkan;

VkFormat convert_cr_to_vulkan_format(CRGPU_FORMAT format);
// Defined in crgpuvulkanrenderpass.c; used by barriers and descriptor
// writes alike, since a CRGPU_IMAGE_LAYOUT is never a VkImageLayout.
VkImageLayout cr_layout_to_vk(CRGPU_IMAGE_LAYOUT layout);
// Defined in crgpuvulkanshader.c; push constant ranges and descriptor set
// layout bindings both take CRGPU_SHADER_STAGE bits, never Vulkan's.
VkShaderStageFlags crgpuvulkan___shader_stages(CRGPU_SHADER_STAGE stages);

// Shared by crgpu_buffer_create / crgpu_image_create and the heap calls
// (crgpuvulkanheap.c), so a buffer or image is checked and described the
// same way whether it has its own allocation or is placed in a heap.

// crgpu_buffer_create's desc checks, in its order, after its argument
// checks: size, usage, memory kind, usage against kind, device.
CR_RESULT crgpuvulkan___check_buffer_desc(
    CRGPU_FEATURES features,
    const crgpu_buffer_desc *desc);
// The VkBufferCreateInfo of a buffer of [desc] on a device of [features]:
// CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT is Vulkan's usage
// bit only with CRGPU_FEATURE_RAY_QUERY, whose extension the bit needs.
void crgpuvulkan___buffer_create_info(
    CRGPU_FEATURES features,
    const crgpu_buffer_desc *desc,
    VkBufferCreateInfo *out_info);
// The first memory type in [typeFilter] with the properties [kind]
// documents in crgpu.h, and that type's flags. UINT32_MAX if none.
uint32_t crgpuvulkan___memory_type_for_kind(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    CRGPU_MEMORY_KIND kind,
    VkMemoryPropertyFlags *out_flags);
// crgpu_image_create's desc checks, in its order, after its argument
// checks.
CR_RESULT crgpuvulkan___check_image_desc(const crgpu_image_desc *desc);
// The VkImageCreateInfo of an image of [desc].
void crgpuvulkan___image_create_info(
    const crgpu_image_desc *desc,
    VkImageCreateInfo *out_info);

// Defined in crgpuvulkanraytracing.c: the memory types an acceleration
// structure's buffer of [size] bytes can be bound to, and, where
// [out_requirements] is not NULL, its whole VkMemoryRequirements. Makes a
// probe buffer and destroys it. [device] has CRGPU_FEATURE_RAY_QUERY.
CR_RESULT crgpuvulkan___acceleration_structure_memory_bits(
    const crgpu_device_vulkan *device,
    uint64_t size,
    uint32_t *out_bits,
    VkMemoryRequirements *out_requirements);

// Defined in crgpuvulkandescriptors.c. Called by crgpu_device_create when
// CRGPU_FEATURE_BINDLESS is asked for, before the device's first by-value
// copy (command allocator/buffer/image/pipeline creation, ...), so every copy
// holds the same _bindless. Nothing makes it later; a failure fails the
// device's creation.
CR_RESULT crgpuvulkan___create_bindless_state(crgpu_device device);

// Defined in crgpuvulkancontext.c: the queue family [type]'s queues and
// command pools are made from on [adapter] (a bundle's is the direct one).
uint32_t crgpuvulkan___family_of(const crgpu_adapter_vulkan *adapter, CRGPU_COMMAND_LIST_TYPE type);
void crgpuvulkan___destroy_bindless_state(crgpu_device device);

#if _DEBUG
// Defined in crgpuvulkancommand.c. Keep the allocator's _open_list:
// opened asserts no list of [cmd]'s allocator is open and records [cmd];
// closed forgets [cmd] if it is the one recorded. Called by every call that
// opens or closes a list, bundles included.
void crgpuvulkan___debug_list_opened(crgpu_command_list cmd);
void crgpuvulkan___debug_list_closed(crgpu_command_list cmd);
#endif

#endif //CARROTTUS_CRGPUVULKAN_H

#endif //CRGPU_COMPILE_VULKAN_API
