// The caller's GPU writer of acceleration structure instances, for a device
// whose layout is SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72
// (Metal's indirect instance descriptor; card S14: one writer per layout
// kind, the caller's). Each thread turns one portable instance into the
// device's 72 bytes, the same bytes
// spudgpu_write_acceleration_structure_instances() writes on the CPU: the
// transform as four columns of three floats, then options (the flags),
// mask, table offset, user index and the 64-bit reference.
//
// The input is rq_instances_row64.slang's: 20 words an instance.

[[vk::binding(0, 0)]] RWByteAddressBuffer rq_in : register(u0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer rq_out : register(u1, space0);

struct Push {
	uint count;
	uint out_offset;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("compute")]
[numthreads(64, 1, 1)]
void rq_instances_col72(uint3 id : SV_DispatchThreadID) {
	const uint i = id.x;
	if (i >= pc.count)
		return;
	const uint src = i * 80u;
	const uint dst = pc.out_offset + i * 72u;
	// Column c, row r: row-major element r * 4 + c.
	for (uint c = 0; c < 4u; ++c)
		for (uint r = 0; r < 3u; ++r)
			rq_out.Store(dst + (c * 3u + r) * 4u, rq_in.Load(src + (r * 4u + c) * 4u));
	rq_out.Store(dst + 48u, rq_in.Load(src + 60u));
	rq_out.Store(dst + 52u, rq_in.Load(src + 52u));
	rq_out.Store(dst + 56u, rq_in.Load(src + 56u));
	rq_out.Store(dst + 60u, rq_in.Load(src + 48u));
	rq_out.Store(dst + 64u, rq_in.Load(src + 64u));
	rq_out.Store(dst + 68u, rq_in.Load(src + 68u));
}
