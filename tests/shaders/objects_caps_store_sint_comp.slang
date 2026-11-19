// crgpu_objects_test's format capabilities (card S51): a 4 x 4
// sint image written through a storage image binding of unknown format
// (CRGPU_FORMAT_CAPABILITY_STORAGE): columns 0 and 1 the push block's
// left value, columns 2 and 3 its right, each given as raw bits.

[[vk::binding(0, 0)]] [format("unknown")] RWTexture2D<int4> img : register(u0, space0);

struct Push {
	uint4 left;
	uint4 right;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("compute")]
[numthreads(4, 4, 1)]
void objects_caps_store_sint_comp(uint3 id : SV_DispatchThreadID) {
	uint4 bits = id.x < 2u ? pc.left : pc.right;
	img[id.xy] = asint(bits);
}
