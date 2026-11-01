// The caller's GPU writer of acceleration structure instances, for a device
// whose layout is SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_ROW_MAJOR_64
// (Vulkan, D3D12; card S14: one writer per layout kind, the caller's). Each
// thread turns one portable instance into the device's 64 bytes, the same
// bytes spudgpu_write_acceleration_structure_instances() writes on the CPU.
//
// An input instance is 20 words: the row-major 3x4 transform (12 floats),
// the user index, the mask, the table offset, the flags, the bottom level's
// reference (low word, high word), and two words of padding. Both buffers
// are byte-address buffers, a storage buffer's raw UAV on D3D12.

[[vk::binding(0, 0)]] RWByteAddressBuffer rq_in : register(u0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer rq_out : register(u1, space0);

struct Push {
	uint count;
	uint out_offset;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("compute")]
[numthreads(64, 1, 1)]
void rq_instances_row64(uint3 id : SV_DispatchThreadID) {
	const uint i = id.x;
	if (i >= pc.count)
		return;
	const uint src = i * 80u;
	const uint dst = pc.out_offset + i * 64u;
	// The 3x4 rows as they are.
	for (uint w = 0; w < 12u; ++w)
		rq_out.Store(dst + w * 4u, rq_in.Load(src + w * 4u));
	const uint user_index   = rq_in.Load(src + 48u);
	const uint mask         = rq_in.Load(src + 52u);
	const uint table_offset = rq_in.Load(src + 56u);
	const uint flags        = rq_in.Load(src + 60u);
	rq_out.Store(dst + 48u, (user_index & 0xFFFFFFu) | (mask << 24));
	rq_out.Store(dst + 52u, (table_offset & 0xFFFFFFu) | (flags << 24));
	rq_out.Store(dst + 56u, rq_in.Load(src + 64u));
	rq_out.Store(dst + 60u, rq_in.Load(src + 68u));
}
