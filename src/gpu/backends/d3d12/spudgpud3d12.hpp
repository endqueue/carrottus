
#if SPUDGPU_COMPILE_D3D12_API

#include "../../../spudallocator.h"
#include "../../spudgpushared.h"
#include "d3dx12.h"
#include "eqstd.hpp"
#include "spudgpu.h"
#include "spudgpu_d3d12_natives.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <dxgi1_6.h>
#include <mutex>
#include <new>
#include <wrl/client.h>

#define D3D12_GPU_VIRTUAL_ADDRESS_NULL ((D3D12_GPU_VIRTUAL_ADDRESS)0)
#define D3D12_GPU_VIRTUAL_ADDRESS_UNKNOWN ((D3D12_GPU_VIRTUAL_ADDRESS) - 1)


inline D3D12_COMMAND_LIST_TYPE
spudgpu_d3d12_get_command_list_type(SPUDGPU_COMMAND_LIST_TYPE t) {
	switch (t) {
	case SPUDGPU_COMMAND_LIST_TYPE_DIRECT:
		return D3D12_COMMAND_LIST_TYPE_DIRECT;
	case SPUDGPU_COMMAND_LIST_TYPE_COPY:
		return D3D12_COMMAND_LIST_TYPE_COPY;
	case SPUDGPU_COMMAND_LIST_TYPE_COMPUTE:
		return D3D12_COMMAND_LIST_TYPE_COMPUTE;
	case SPUDGPU_COMMAND_LIST_TYPE_BUNDLE:
		return D3D12_COMMAND_LIST_TYPE_BUNDLE;
	default:
		return D3D12_COMMAND_LIST_TYPE_NONE;
	}
}

// The heap flags of every buffer and image. spudgpu.h declares a new
// resource's contents undefined until written, so D3D12 is never asked to
// zero one.
inline D3D12_HEAP_FLAGS spudgpu_d3d12_get_heap_flags() {
	return D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
}
inline D3D12_RESOURCE_FLAGS spudgpu_d3d12_get_buffer_resource_flags(
    SPUDGPU_BUFFER_USAGE usage) {
	D3D12_RESOURCE_FLAGS result = D3D12_RESOURCE_FLAG_NONE;
	// SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT needs nothing:
	// a build reads its inputs in NON_PIXEL_SHADER_RESOURCE, or an upload
	// heap's GENERIC_READ.
	if (usage & SPUDGPU_BUFFER_USAGE_STORAGE)
		result |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return result;
}
inline D3D12_RESOURCE_FLAGS spudgpu_d3d12_get_image_resource_flags(
    SPUDGPU_IMAGE_USAGE usage) {
	D3D12_RESOURCE_FLAGS result = D3D12_RESOURCE_FLAG_NONE;
	if (usage & SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT)
		result |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (usage & SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)
		result |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (usage & SPUDGPU_IMAGE_USAGE_STORAGE)
		result |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return result;
}
inline DXGI_FORMAT spudgpu_d3d12_get_dxgi_format(SPUDGPU_FORMAT fmt) {
	return static_cast<DXGI_FORMAT>(fmt);
}

// The heap a buffer of [kind] is created in: what each SPUDGPU_MEMORY_KIND
// documents in spudgpu.h. spudgpu_create_buffer has already refused a value
// that is not a kind.
inline D3D12_HEAP_PROPERTIES
spudgpu_d3d12_get_heap_properties_from_memory_kind(SPUDGPU_MEMORY_KIND kind) {
	D3D12_HEAP_PROPERTIES props = {};
	switch (kind) {
	case SPUDGPU_MEMORY_KIND_UPLOAD:
		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		break;
	case SPUDGPU_MEMORY_KIND_READBACK:
		props.Type = D3D12_HEAP_TYPE_READBACK;
		break;
	case SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE:
		// A custom heap is held to neither the upload heap's rule (always
		// GENERIC_READ, no unordered access) nor the readback heap's (always
		// COPY_DEST). L0 is the only pool a UMA adapter has, and
		// spudgpu_create_buffer only gets here on one.
		props.Type                 = D3D12_HEAP_TYPE_CUSTOM;
		props.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
		break;
	default:
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		break;
	}
	return props;
}
// An upload heap's resources are GENERIC_READ and a readback heap's are
// COPY_DEST for as long as they exist; D3D12 allows no other state for them.
inline D3D12_RESOURCE_STATES
spudgpu_d3d12_get_initial_buffer_state(SPUDGPU_MEMORY_KIND kind) {
	if (kind == SPUDGPU_MEMORY_KIND_UPLOAD)
		return D3D12_RESOURCE_STATE_GENERIC_READ;
	if (kind == SPUDGPU_MEMORY_KIND_READBACK)
		return D3D12_RESOURCE_STATE_COPY_DEST;
	return D3D12_RESOURCE_STATE_COMMON;
}
inline D3D12_RESOURCE_STATES
spudgpu_d3d12_get_initial_image_state(SPUDGPU_IMAGE_USAGE usage) {
	if (usage & SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT)
		return D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if (usage & SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT)
		return D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (usage & SPUDGPU_IMAGE_USAGE_TRANSFER_DST)
		return D3D12_RESOURCE_STATE_COPY_DEST;
	if (usage & SPUDGPU_IMAGE_USAGE_STORAGE)
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return D3D12_RESOURCE_STATE_COMMON;
}

typedef struct spudgpu_instance_t {
	spud_object_head head;
	const char *application_name;
	uint32_t application_version;
	const char *engine_name;
	uint32_t engine_version;
	Microsoft::WRL::ComPtr<IDXGIFactory7> _dxgi_factory;
	// The adapters listed when the instance was made, which never change,
	// freed with it.
	spudgpu_adapter *_adapters;
	uint32_t _adapter_count;
} spudgpu_instance_d3d12;

typedef struct spudgpu_adapter_t {
	spud_object_head head;
	// The instance that listed the adapter, which outlives it and every
	// device made on it; its DXGI factory makes swap chains.
	spudgpu_instance_d3d12 *_instance;
	Microsoft::WRL::ComPtr<IDXGIAdapter4> _dxgi_adapter;
	// Read when the instance was made, through a probe device released at
	// once (spudgpud3d12context.cpp), and never changed.
	spudgpu___adapter_facts _facts;
	// Whether a device exists on the adapter: one at a time, claimed with an
	// exchange by spudgpu_create_device and given back by
	// spudgpu_destroy_device. D3D12 hands back the one live device to a
	// second D3D12CreateDevice, so a second spudgpu_device would be the same
	// native device.
	std::atomic<bool> _in_use{false};
} spudgpu_adapter_d3d12;

#define SPUDGPU_BINDLESS_MAX_SLOTS_PER_CLASS 4096

// Forward declaration -- spudgpu_bindless_state_d3d12 below stores a pointer
// to this before its full definition (with the rest of the descriptor-set
// types) appears further down this file.
typedef struct spudgpu_descriptor_set_layout_t spudgpu_descriptor_set_layout_d3d12;

// One unused range of a spudgpu_d3d12_shader_heap: descriptors
// [offset, offset + count).
typedef struct spudgpu_d3d12_heap_range {
	uint32_t offset;
	uint32_t count;
} spudgpu_d3d12_heap_range;

// One of the device's two shader-visible descriptor heaps, which every
// descriptor pool and the bindless state take a range of. D3D12 lets a
// command list have one shader-visible CBV_SRV_UAV heap and one sampler heap
// set at a time, so sets from different pools, and bindless beside them,
// can only be bound for one draw if they are all in the same two heaps.
// Each is made with the device, as large as the device's descriptor budget
// (spudgpu_device_desc::max_descriptors, ::max_samplers), and not made at all
// for a budget of 0 (spudgpud3d12descriptors.cpp).
typedef EQ_ARRAY(spudgpu_d3d12_heap_range) spudgpu_d3d12_heap_ranges;

typedef struct spudgpu_d3d12_shader_heap {
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
	uint32_t increment = 0;
	// The unused ranges, sorted by offset, with touching neighbours merged,
	// grown through the device's _eq_allocator. The device frees them when
	// it is destroyed (spudgpud3d12context.cpp): a destructor has no
	// allocator to free them through.
	spudgpu_d3d12_heap_ranges free_ranges = {};
} spudgpu_d3d12_shader_heap;

// A root-signature-less command signature describing one DRAW (or
// DRAW_INDEXED) argument every `stride` bytes.
typedef struct spudgpu_d3d12_indirect_signature {
	ID3D12CommandSignature *signature;
	uint32_t stride;
	bool indexed;
} spudgpu_d3d12_indirect_signature;

// The device's indirect draw signatures, grown through the device's
// _eq_allocator. Each signature is released, and the array freed, when the
// device is destroyed (spudgpud3d12context.cpp): a destructor has no
// allocator to free it through.
typedef EQ_ARRAY(spudgpu_d3d12_indirect_signature) spudgpu_d3d12_indirect_signatures;

typedef struct spudgpu_bindless_state_d3d12 {
	// A range of the device's shader-visible CBV_SRV_UAV heap
	// (spudgpu_device_d3d12::_csv_heap) starting at `offset`, in 3
	// fixed-size parts back-to-back: [0, MAX) sampled images, [MAX, 2*MAX)
	// storage images, [2*MAX, 3*MAX) storage buffers.
	// spudgpu_d3d12_build_root_signature (spudgpud3d12shader.cpp) already
	// builds exactly this shape from layout->_desc.bindings[] with no
	// changes needed — see `layout` below.
	uint32_t offset = 0;

	// Sentinel spudgpu_descriptor_set_layout_d3d12 carrying only the 3
	// bindings' shape (binding_count/bindings[]) needed to generate the
	// matching root-signature ranges when a caller includes this handle in
	// their own descriptor_set_layouts[]. Its _slots[]/_cbv_srv_uav_count
	// bookkeeping (used by the classic pool/set allocator) is left zeroed —
	// bindless never goes through spudgpu_create_descriptor_sets.
	spudgpu_descriptor_set_layout_d3d12 *layout = nullptr;

	uint32_t sampled_image_next_unused  = 0;
	uint32_t storage_image_next_unused  = 0;
	uint32_t storage_buffer_next_unused = 0;

	// Stacks of freed indices, reused before drawing from *_next_unused, on
	// the device's _eq_allocator (given when the state is made).
	eq::array<uint32_t> sampled_image_free_stack;
	eq::array<uint32_t> storage_image_free_stack;
	eq::array<uint32_t> storage_buffer_free_stack;

	// Guards the counters and stacks above: registering and unregistering
	// may be done from any thread, and the bindless table is SpudGPU's, not
	// the caller's, to keep. The descriptor writes themselves are made
	// outside it: D3D12's Create*View calls may be made from any thread, and
	// no two threads hold the same slot.
	std::mutex lock;
} spudgpu_bindless_state_d3d12;

typedef struct spudgpu_device_t {
	spud_object_head head;
	// eqstd's view of head.allocator (spud_eq_allocator), which the device's
	// arrays point at: the heaps' free ranges and the indirect signatures.
	EQ_ALLOCATOR _eq_allocator;
	spudgpu_adapter_d3d12 *_adapter;
	Microsoft::WRL::ComPtr<ID3D12Device14> _d3d_device;
	// spudgpu_device_desc::required_features: what every call checks.
	SPUDGPU_FEATURES _features;
	// The queues the desc asked for, by kind, each array as long as its
	// count (NULL for 0).
	spudgpu_command_queue *_cmd_queues_direct;
	uint32_t _cmd_queue_count_direct;
	spudgpu_command_queue *_cmd_queues_compute;
	uint32_t _cmd_queue_count_compute;
	spudgpu_command_queue *_cmd_queues_copy;
	uint32_t _cmd_queue_count_copy;

	// Made with the device when SPUDGPU_FEATURE_BINDLESS is asked for, and
	// NULL exactly when it is not; never replaced. Released with the device.
	spudgpu_bindless_state_d3d12 *_bindless;

	// The shader-visible descriptor heaps every descriptor pool and the
	// bindless state take ranges of (spudgpu_d3d12_shader_heap), made with
	// the device and sized by its descriptor budget: the budget is these
	// heaps. Their free lists are guarded by _shader_heap_mutex, as pools may
	// be made and destroyed on any thread; the heaps themselves are never
	// replaced.
	spudgpu_d3d12_shader_heap _csv_heap;
	spudgpu_d3d12_shader_heap _sampler_heap;
	std::mutex _shader_heap_mutex;

	// The command signatures spudgpu_cmd_draw_indirect / _indexed_indirect
	// use, one for each stride and kind (a signature fixes its ByteStride).
	// The two for tightly packed arguments are made with the device, so the
	// common case takes no lock: _tight_draw_signature and
	// _tight_draw_indexed_signature. A signature for any other stride is
	// keyed by the caller's stride, so it is made by the first draw that
	// uses it, under _indirect_signature_mutex: lists may be recorded on
	// several threads.
	ID3D12CommandSignature *_tight_draw_signature;
	ID3D12CommandSignature *_tight_draw_indexed_signature;
	spudgpu_d3d12_indirect_signatures _indirect_signatures;
	std::mutex _indirect_signature_mutex;

#if _DEBUG
	// The debug layer's message callback (spudgpu_d3d12_debug_message),
	// registered when the device is made and unregistered when it is
	// destroyed. 0 if none was registered: the debug layer is off, or the
	// runtime has no ID3D12InfoQueue1 (before Windows 11).
	DWORD _debug_callback_cookie;
	// Objects made from the device and not yet destroyed, by kind;
	// spudgpu_destroy_device reports and asserts on any left.
	spudgpu___live_objects _live;
#endif
} spudgpu_device_d3d12;

// The largest draw_count one indirect draw on [device] takes:
// SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count with
// SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1.
inline uint32_t spudgpu_d3d12___max_draw_indirect_count(const spudgpu_device_d3d12 *device) {
	return (device->_features & SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT) ? device->_adapter->_facts.properties.max_draw_indirect_count : 1;
}

typedef struct spudgpu_command_queue_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> _d3d_cmd_queue;
	// The lists of a submission, as D3D12 takes them: kept with its
	// capacity from submission to submission, on the device's
	// _eq_allocator, so a steady stream of submissions allocates nothing.
	// A queue is used by one caller at a time (spudgpu.h).
	eq::array<ID3D12CommandList *> _scratch_lists;
#if _DEBUG
	// How many threads are in a call that uses the queue: a debug build
	// asserts it is never more than one (spudgpu_d3d12___queue_enter).
	std::atomic<int> _debug_users{0};
#endif
} spudgpu_command_queue_d3d12;

// Brackets every SpudGPU call that uses a queue (a submit, a present, a
// wait-idle). D3D12 itself would allow two threads at once, but spudgpu.h
// makes a queue the caller's to synchronize on every backend, since Vulkan
// requires it; a debug build asserts here so that code that breaks the rule
// is caught on D3D12 too. Nothing in a release build.
#if _DEBUG
inline void spudgpu_d3d12___queue_enter(spudgpu_command_queue_d3d12 *queue) {
	const int others = queue->_debug_users.fetch_add(1);
	assert(others == 0 && "spudgpu: two threads used one command queue at once; the caller synchronizes a queue");
	(void)others;
}
inline void spudgpu_d3d12___queue_leave(spudgpu_command_queue_d3d12 *queue) { queue->_debug_users.fetch_sub(1); }
#else
inline void spudgpu_d3d12___queue_enter(spudgpu_command_queue_d3d12 *) {}
inline void spudgpu_d3d12___queue_leave(spudgpu_command_queue_d3d12 *) {}
#endif

typedef struct spudgpu_command_allocator_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _d3d_cmd_allocator;
	D3D12_COMMAND_LIST_TYPE _d3d_cmd_list_type;
	// The type it was made with, which the checks read.
	SPUDGPU_COMMAND_LIST_TYPE _type;
#if _DEBUG
	// The list of this allocator that is open for recording, or null. One
	// at a time (spudgpu_begin_command_list); a debug build asserts it
	// before D3D12 refuses the Reset and leaves the list closed. Not atomic:
	// an allocator is used from one thread at a time.
	spudgpu_command_list _open_list;
#endif
} spudgpu_command_allocator_d3d12;

typedef struct spudgpu_command_list_t {
	spud_object_head head;
	spudgpu_command_allocator_d3d12 *_allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> _d3d_cmd_list;

	// Persistent RTV/DSV heaps reused across spudgpu_cmd_begin_rendering
	// calls instead of allocating a new descriptor heap every render pass.
	// _rtv_heap grows (and is recreated) if a pass needs more color
	// attachments than it currently holds; _dsv_heap only ever needs one
	// descriptor and is created once on first use.
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _rtv_heap;
	UINT _rtv_heap_capacity;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _dsv_heap;

	// Set by spudgpu_cmd_begin_rendering, read by spudgpu_cmd_end_rendering:
	// true if BeginRenderPass/EndRenderPass was used for this pass, false if
	// the classic OMSetRenderTargets/Clear*View path was used instead (see
	// spudgpu_cmd_begin_rendering's will_execute_bundles handling) -- the two
	// don't mix, so end_rendering must match whichever begin_rendering chose.
	bool _using_render_pass;
	// Whether a rendering pass is open: set by spudgpu_cmd_begin_rendering,
	// cleared by spudgpu_cmd_end_rendering. Read by the calls a pass can't
	// hold (an acceleration structure build, copy or size write).
	bool _rendering_active;
	// The timestamp the open pass writes at spudgpu_cmd_end_rendering
	// (spudgpu_rendering_begin_desc::timestamp_set); nullptr for none.
	ID3D12QueryHeap *_end_timestamp_heap;
	UINT _end_timestamp_index;

	// What a recording call converts the caller's array into before D3D12
	// takes it: barriers, scissor rectangles, vertex buffer views. Each
	// keeps its capacity from call to call, on the device's _eq_allocator,
	// so recording allocates only until the largest call has been seen.
	// Cleared by each call that uses it; nothing is kept between calls.
	eq::array<D3D12_RESOURCE_BARRIER> _scratch_barriers;
	eq::array<D3D12_RECT> _scratch_rects;
	eq::array<D3D12_VERTEX_BUFFER_VIEW> _scratch_vb_views;
	// An acceleration structure build's geometries and a size write's
	// addresses (spudgpud3d12raytracing.cpp), the same way.
	eq::array<D3D12_RAYTRACING_GEOMETRY_DESC> _scratch_geometries;
	eq::array<D3D12_GPU_VIRTUAL_ADDRESS> _scratch_addresses;
} spudgpu_command_list_d3d12;

typedef struct spudgpu_timestamp_set_t {
	spud_object_head head;
	Microsoft::WRL::ComPtr<ID3D12QueryHeap> _d3d_query_heap;
	// What spudgpu_cmd_resolve_timestamps copies the ticks into, entry i at
	// byte i * 8, and spudgpu_get_timestamps maps.
	Microsoft::WRL::ComPtr<ID3D12Resource> _d3d_readback;
	uint32_t _count;
	// The queue's GetTimestampFrequency: ticks a second.
	uint64_t _frequency;
	spudgpu_device_d3d12 *_device;
} spudgpu_timestamp_set_d3d12;

typedef struct spudgpu_buffer_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_buffer_desc _desc;
	Microsoft::WRL::ComPtr<ID3D12Resource2> _d3d_resource;
	D3D12_RESOURCE_DESC _d3d_resource_desc;
	D3D12_GPU_VIRTUAL_ADDRESS _d3d_gpu_address;
	// nullptr for a committed buffer; the heap it was placed in otherwise
	// (spudgpu_create_placed_buffer).
	spudgpu_heap _heap;
} spudgpu_buffer_d3d12;

typedef struct spudgpu_buffer_view_t {
	spud_object_head head;
	spudgpu_buffer_d3d12 *_buffer;
	spudgpu_buffer_view_desc _desc;
	SPUDGPU_D3D12_BUFFER_VIEW _d3d_view;
	// The buffer's device, kept here because a view may be destroyed after
	// its buffer (its live count is the device's).
	spudgpu_device_d3d12 *_device;
} spudgpu_buffer_view_d3d12;

typedef struct spudgpu_image_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_image_desc _desc;
	Microsoft::WRL::ComPtr<ID3D12Resource2> _d3d_resource;
	D3D12_RESOURCE_DESC _d3d_resource_desc;

	// The resource's actual current D3D12 state, kept in sync by every
	// spudgpu_cmd_image_barrier* call (spudgpud3d12renderpass.cpp). Needed
	// because SPUDGPU_IMAGE_LAYOUT_UNDEFINED (the Vulkan "don't care what it
	// was" convention this API follows) has no real D3D12 equivalent --
	// D3D12's strict before/after state tracking means the barrier's
	// declared "before" state has to match whatever the resource is
	// actually in, which for a freshly created image is its usage-implied
	// initial state (not always D3D12_RESOURCE_STATE_COMMON -- see
	// spudgpu_d3d12_get_initial_image_state) and for a recycled resource
	// (a swap chain back buffer transitioning every frame) is whatever the
	// last real transition left it at, not the creation-time value. Set at
	// creation (spudgpu_create_image / swap chain back buffer wrapping) and
	// updated after every barrier -- never re-derived from a static
	// usage-based guess.
	D3D12_RESOURCE_STATES _current_state;
	// Empty while every subresource is in _current_state, which is nearly
	// always. Once spudgpu_cmd_image_barrier_subresource has moved part of
	// the image somewhere else, it holds the state of each (mip, layer),
	// at index mip + layer * mip_levels, and _current_state is not read
	// until the states agree again and this is emptied. A single state for
	// the whole image can't describe mip N in one layout and mip N + 1 in
	// another, which is what that call exists for. On the device's
	// _eq_allocator, given when the image is made.
	eq::array<D3D12_RESOURCE_STATES> _subresource_states;
	// nullptr for a committed image; the heap it was placed in otherwise
	// (spudgpu_create_placed_image).
	spudgpu_heap _heap;
} spudgpu_image_d3d12;

// An acceleration structure (spudgpud3d12raytracing.cpp): a buffer in
// RAYTRACING_ACCELERATION_STRUCTURE state for its whole life, committed or
// placed. Its first two members are spudgpu___acceleration_structure_common's
// (spudgpushared.h), which the shared checks read. Plain: no constructor
// beyond the ComPtr's, which is made in place.
typedef struct spudgpu_acceleration_structure_t {
	spud_object_head head;
	spudgpu___acceleration_structure_state _state;
	// GetGPUVirtualAddress: what a build, a copy, a descriptor and an
	// instance name it by.
	D3D12_GPU_VIRTUAL_ADDRESS _d3d_gpu_address;
	Microsoft::WRL::ComPtr<ID3D12Resource> _d3d_resource;
	spudgpu_device_d3d12 *_device;
	// nullptr for a committed structure; its heap otherwise.
	spudgpu_heap _heap;
} spudgpu_acceleration_structure_d3d12;

typedef struct spudgpu_heap_t {
	spud_object_head head;
#if _DEBUG
	// Buffers and images placed in the heap and not yet destroyed.
	// spudgpu_destroy_heap asserts it is 0. Atomic, because resources in
	// one heap may be placed and destroyed on several threads at once.
	std::atomic<uint64_t> _placed_count;
#endif
	spudgpu_device_d3d12 *_device;
	spudgpu_heap_desc _desc;
	Microsoft::WRL::ComPtr<ID3D12Heap> _d3d_heap;
} spudgpu_heap_d3d12;

// Shared by spudgpu_create_buffer / spudgpu_create_image and the heap calls
// (spudgpud3d12heap.cpp), so a buffer or image is checked and described the
// same way whether it is committed or placed.

// spudgpu_create_buffer's desc checks, in its order, after its argument
// checks: size, usage, memory kind, usage against kind, device.
SPUDRESULT spudgpu_d3d12_check_buffer_desc(
    SPUDGPU_FEATURES features,
    const spudgpu_buffer_desc *desc);
CD3DX12_RESOURCE_DESC spudgpu_d3d12_create_resource_desc_from_buffer(
    const spudgpu_buffer_desc *desc);
// spudgpu_create_image's desc checks, in its order, after its argument
// checks.
SPUDRESULT spudgpu_d3d12_check_image_desc(const spudgpu_image_desc *desc);
SPUDRESULT spudgpu_d3d12_create_resource_desc_from_image(
    const spudgpu_image_desc *desc,
    D3D12_RESOURCE_DESC *out_d3d_resource_desc);
// The optimized clear value an image of [desc] is created with, or false if
// it has none (it is not an attachment).
bool spudgpu_d3d12_get_image_clear_value(
    const spudgpu_image_desc *desc,
    D3D12_CLEAR_VALUE *out_clear_value);

typedef struct spudgpu_image_view_t {
	spud_object_head head;
	spudgpu_image_d3d12 *_image;
	spudgpu_image_view_desc _desc;
	// What a render pass reads (_rtv, _dsv), and what
	// spudgpu_get_d3d12_image_view hands out: one desc, the first of depth
	// stencil, render target, unordered access and shader resource that the
	// image's usage allows.
	SPUDGPU_D3D12_IMAGE_VIEW _d3d_view_desc;
	// What a descriptor write reads, whatever else the image is used for.
	D3D12_SHADER_RESOURCE_VIEW_DESC _d3d_srv;
	D3D12_UNORDERED_ACCESS_VIEW_DESC _d3d_uav;
	// The image's device, kept here because a view may be destroyed after
	// its image (its live count is the device's). NULL for a swap chain's
	// own views, which the swap chain frees.
	spudgpu_device_d3d12 *_device;
} spudgpu_image_view_d3d12;

// A root parameter index that no table or constant block was given.
#define SPUDGPU_D3D12_NO_ROOT_PARAMETER UINT32_MAX

// Where spudgpu_d3d12_build_root_signature (spudgpud3d12shader.cpp) put each
// descriptor set's tables and the push constants among a root signature's
// parameters. A D3D12 descriptor table reads from one heap, so a set with
// both CBV/SRV/UAV and sampler descriptors takes two tables, one per heap;
// a set's index is therefore not its root parameter index, and every call
// that sets a root parameter looks it up here.
typedef struct spudgpu_d3d12_root_layout {
	// Indexed by set number; SPUDGPU_D3D12_NO_ROOT_PARAMETER where the set
	// has no descriptor of that heap.
	uint32_t cbv_srv_uav_param[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	uint32_t sampler_param[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	// SPUDGPU_D3D12_NO_ROOT_PARAMETER with no push constant range.
	uint32_t push_constant_param;
} spudgpu_d3d12_root_layout;

typedef struct spudgpu_shader_module_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	// The desc, its code and entry point pointing at the module's own
	// copies (allocated through head.allocator; the code at 4-byte
	// alignment), which pipelines take as their bytecode.
	spudgpu_shader_module_desc _desc;
} spudgpu_shader_module_d3d12;

typedef struct spudgpu_shader_pipeline_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_shader_pipeline_desc _desc;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _d3d_pipeline_state;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _d3d_root_signature;
	spudgpu_d3d12_root_layout _root_layout;
	D3D_PRIMITIVE_TOPOLOGY _d3d_primitive_topology;
	// True when built from desc->mesh_module (SPUDGPU_EXT_MESH_SHADING) via
	// the D3D12_PIPELINE_STATE_STREAM_DESC path instead of the classic
	// CreateGraphicsPipelineState path - see spudgpud3d12shader.cpp. Mesh
	// pipelines have no input-assembler stage, so _d3d_primitive_topology
	// is meaningless for them and IASetPrimitiveTopology must be skipped.
	bool _is_mesh_pipeline;
} spudgpu_shader_pipeline_d3d12;

typedef struct spudgpu_compute_pipeline_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_compute_pipeline_desc _desc;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _d3d_pipeline_state;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> _d3d_root_signature;
	spudgpu_d3d12_root_layout _root_layout;
} spudgpu_compute_pipeline_d3d12;

typedef struct spudgpu_fence_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	SPUDGPU_FENCE_FLAGS flags;
	// The fence's value is the ID3D12Fence's own. Nothing is counted here:
	// the value a submission signals is the caller's
	// (spudgpu_submit_desc::signal_fence_value).
	Microsoft::WRL::ComPtr<ID3D12Fence1> _d3d_fence;
} spudgpu_fence_d3d12;

typedef struct spudgpu_semaphore_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	uint64_t _signal_value;
	Microsoft::WRL::ComPtr<ID3D12Fence1> _d3d_fence;
} spudgpu_semaphore_d3d12;

// Per-binding heap offsets pre-computed at layout creation time.
typedef struct spudgpu_d3d12_binding_slot {
	uint32_t cbv_srv_uav_offset; // from set base in the CBV/SRV/UAV heap
	uint32_t sampler_offset;     // from set base in the sampler heap
	bool     is_sampler;         // true if this binding lives in the sampler heap
} spudgpu_d3d12_binding_slot;

// D3D12 samplers are descriptor-heap entries, not standalone device objects
// the way VkSampler/id<MTLSamplerState> are — this just stores the
// translated desc, written into a sampler heap slot on demand by
// spudgpu_update_descriptor_sets (spudgpud3d12descriptors.cpp).
typedef struct spudgpu_sampler_t {
	spud_object_head head;
	D3D12_SAMPLER_DESC _d3d_desc;
	spudgpu_device_d3d12 *_device;
} spudgpu_sampler_d3d12;

typedef struct spudgpu_descriptor_set_layout_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_descriptor_set_layout_desc _desc;
	// Offsets into a set's allocated heap range, indexed by binding desc order.
	spudgpu_d3d12_binding_slot _slots[SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET];
	uint32_t _cbv_srv_uav_count; // CBV/SRV/UAV descriptors needed per set
	uint32_t _sampler_count;     // sampler descriptors needed per set
} spudgpu_descriptor_set_layout_d3d12;

typedef struct spudgpu_descriptor_pool_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_descriptor_pool_desc _desc;
	// The pool's ranges of the device's shader-visible heaps
	// (spudgpu_device_d3d12::_csv_heap / _sampler_heap): descriptors
	// [offset, offset + capacity) of each, returned when the pool is
	// destroyed. A set's slots are indices into these ranges.
	uint32_t _cbv_srv_uav_offset;
	uint32_t _sampler_offset;
	uint32_t _cbv_srv_uav_capacity;
	uint32_t _sampler_capacity;
	uint32_t _cbv_srv_uav_cursor; // next free slot index
	uint32_t _sampler_cursor;
	// Head of the list of every set allocated from this pool, linked through
	// spudgpu_descriptor_set_d3d12::_next. There is no per-set destroy call,
	// so the pool is what releases them, on reset and on destroy.
	struct spudgpu_descriptor_set_t *_sets;
} spudgpu_descriptor_pool_d3d12;

typedef struct spudgpu_descriptor_set_t {
	spud_object_head head;
	spudgpu_descriptor_pool_d3d12 *_pool;
	spudgpu_descriptor_set_layout_d3d12 *_layout;
	uint32_t _cbv_srv_uav_base; // base slot index in pool's CBV/SRV/UAV range
	uint32_t _sampler_base;     // base slot index in pool's sampler range
	struct spudgpu_descriptor_set_t *_next; // next set in _pool's _sets list
} spudgpu_descriptor_set_d3d12;

typedef struct spudgpu_surface_t {
	spud_object_head head;
	HWND _hwnd;
	spudgpu_instance_d3d12 *_instance;
} spudgpu_surface_d3d12;

typedef struct spudgpu_swap_chain_t {
	spud_object_head head;
	spudgpu_device_d3d12 *_device;
	spudgpu_swap_chain_desc _desc;
	Microsoft::WRL::ComPtr<IDXGISwapChain4> _dxgi_swap_chain;
	spudgpu_image_d3d12 *_back_buffer_images;
	spudgpu_image_view_d3d12 *_back_buffer_image_views;

	// Backs spudgpu_submit_command_lists_synced's "signals the in-flight
	// fence" half of the Vulkan/Metal contract. The other two thirds of that
	// contract have no D3D12 equivalent to wire up: flip-model DXGI swap
	// chains hand back a buffer index synchronously (no Vulkan-style
	// image_available acquire wait), and Present() is queued on the same
	// command queue the render work was submitted to, so it's already
	// ordered after that work with no separate render_finished signal
	// needed. One monotonically increasing fence value per submit, rather
	// than Vulkan/Metal's per-back-buffer-index tracking, because every
	// sample built against this backend so far (see HelloTriangle) reuses a
	// single command allocator/list across frames instead of one per
	// back-buffer index -- so the only safe wait is against the
	// immediately preceding submit, not the submit that last touched the
	// current back-buffer index specifically (which can be a stale,
	// already-satisfied value that under-waits relative to the shared
	// allocator's real last use). See spudgpu_swap_chain_acquire_next_image
	// (spudgpud3d12swapchain.cpp) for where this gets waited on.
	Microsoft::WRL::ComPtr<ID3D12Fence1> _frame_fence;
	uint64_t _frame_fence_next_value;
} spudgpu_swap_chain_d3d12;

// Made with the device by spudgpu_create_device (spudgpud3d12context.cpp).
// Defined in spudgpud3d12descriptors.cpp: the shader-visible heaps, sized by
// the descriptor budget (a budget of 0 makes no heap), both or neither; and
// the bindless table, when SPUDGPU_FEATURE_BINDLESS is asked for, after the
// heaps. C linkage, as the definitions sit inside the files' extern "C"
// blocks.
extern "C" {
SPUDRESULT spudgpu_d3d12___create_shader_heaps(
    spudgpu_device_d3d12 *device,
    uint32_t max_descriptors,
    uint32_t max_samplers);
SPUDRESULT spudgpu_d3d12___create_bindless_state(spudgpu_device_d3d12 *device);
// Defined in spudgpud3d12command.cpp: a root-signature-less command
// signature of one DRAW (or DRAW_INDEXED) every [stride] bytes, or null.
ID3D12CommandSignature *spudgpu_d3d12___create_draw_indirect_signature(
    ID3D12Device *d3d_device, bool indexed, uint32_t stride);
}

#endif // SPUDGPU_COMPILE_D3D12_API
