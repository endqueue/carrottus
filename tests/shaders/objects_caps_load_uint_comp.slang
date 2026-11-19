// crgpu_objects_test's format capabilities (card S51): every texel
// of a 4 x 4 uint image read by Load through a sampled image binding
// (CRGPU_FORMAT_CAPABILITY_SAMPLED), its four components stored as raw
// bits at 16 * (4 y + x) bytes.

[[vk::binding(0, 0)]] Texture2D<uint4> tex : register(t0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer dst : register(u1, space0);

[shader("compute")]
[numthreads(4, 4, 1)]
void objects_caps_load_uint_comp(uint3 id : SV_DispatchThreadID) {
	uint4 v = tex.Load(int3(int2(id.xy), 0));
	dst.Store4((id.y * 4u + id.x) * 16u, (v));
}
