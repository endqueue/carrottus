// Every binding of a set (card S27): the set declared whole and a binding
// at 9 the layout doesn't hold, read. Refused on Metal.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8,
// set 1 a storage buffer at 0 (spudgpu_objects_test, test_set_bindings).

struct Unread {
	float4 value;
};

[[vk::binding(0, 0)]] ConstantBuffer<Unread> unread : register(b0, space0);
[[vk::binding(5, 0)]] Texture2D<float4> image : register(t5, space0);
[[vk::binding(8, 0)]] SamplerState image_sampler : register(s8, space0);
[[vk::binding(9, 0)]] Texture2D<float4> extra : register(t9, space0);

[[vk::binding(0, 1)]] RWStructuredBuffer<float4> result : register(u0, space1);

[shader("compute")]
[numthreads(1, 1, 1)]
void bindings_extra_comp() {
	result[0] = image.SampleLevel(image_sampler, float2(0.5, 0.5), 0.0) + extra.SampleLevel(image_sampler, float2(0.5, 0.5), 0.0);
}
