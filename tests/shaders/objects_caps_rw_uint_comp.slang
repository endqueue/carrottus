// crgpu_objects_test's format capabilities (card S51): a 4 x 4
// uint image read, and written back unchanged, through a storage image
// binding of unknown format (CRGPU_FORMAT_CAPABILITY_STORAGE_READ, a
// read-write texture on Metal), each texel's raw bits stored at
// 16 * (4 y + x) bytes.

[[vk::binding(0, 0)]] [format("unknown")] RWTexture2D<uint4> img : register(u0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer dst : register(u1, space0);

[shader("compute")]
[numthreads(4, 4, 1)]
void objects_caps_rw_uint_comp(uint3 id : SV_DispatchThreadID) {
	uint4 v = img[id.xy];
	dst.Store4((id.y * 4u + id.x) * 16u, (v));
	img[id.xy] = v;
}
