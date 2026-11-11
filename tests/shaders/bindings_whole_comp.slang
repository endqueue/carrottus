// Every binding of a set (card S27): the set declared whole, its texel read
// at 5 through the sampler at 8 and the uniform buffer at 0 not read. Slang
// leaves 0 out of the SPIR-V; the build writes it back on Metal, so the
// module's argument buffer is the layout's and the texel is the image's.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8,
// set 1 a storage buffer at 0 (crgpu_objects_test, test_set_bindings).

struct Unread {
	float4 value;
};

[[vk::binding(0, 0)]] ConstantBuffer<Unread> unread : register(b0, space0);
[[vk::binding(5, 0)]] Texture2D<float4> image : register(t5, space0);
[[vk::binding(8, 0)]] SamplerState image_sampler : register(s8, space0);

[[vk::binding(0, 1)]] RWStructuredBuffer<float4> result : register(u0, space1);

[shader("compute")]
[numthreads(1, 1, 1)]
void bindings_whole_comp() {
	result[0] = image.SampleLevel(image_sampler, float2(0.5, 0.5), 0.0);
}
