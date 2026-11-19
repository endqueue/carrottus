// crgpu_objects_test's format capabilities (card S51): a 4 x 4
// image sampled with linear filtering (CRGPU_FORMAT_CAPABILITY_FILTERED)
// at its middle, halfway between the texels of columns 1 and 2, the
// result stored as raw bits at byte 0.

[[vk::binding(0, 0)]] Texture2D<float4> tex : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState smp : register(s1, space0);
[[vk::binding(2, 0)]] RWByteAddressBuffer dst : register(u2, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void objects_caps_sample_comp(uint3 id : SV_DispatchThreadID) {
	dst.Store4(0u, asuint(tex.SampleLevel(smp, float2(0.5, 0.5), 0.0)));
}
