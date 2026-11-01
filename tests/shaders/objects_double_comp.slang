// spudgpu_objects_test's compute shader: dst[i] = src[i] * 2 + add.
// Each buffer is uint[], read and written at byte offset 4 * i: a
// byte-address buffer, which is what a storage buffer's raw UAV on D3D12 is.

[[vk::binding(0, 0)]] RWByteAddressBuffer src : register(u0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer dst : register(u1, space0);

struct Push {
	uint add;
	uint count;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("compute")]
[numthreads(64, 1, 1)]
void objects_double_comp(uint3 dispatch_id : SV_DispatchThreadID) {
	uint i = dispatch_id.x;
	if (i < pc.count)
		dst.Store(i * 4u, src.Load(i * 4u) * 2u + pc.add);
}
