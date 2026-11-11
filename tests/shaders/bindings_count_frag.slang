// Every binding of a set (card S27): the set declared whole but 5 an array
// of 2, where the layout's count is 1. Refused on Metal.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8
// (crgpu_objects_test, test_set_bindings).

struct Unread {
	float4 value;
};

[[vk::binding(0, 0)]] ConstantBuffer<Unread> unread : register(b0, space0);
[[vk::binding(5, 0)]] Texture2D<float4> images[2] : register(t5, space0);
[[vk::binding(8, 0)]] SamplerState image_sampler : register(s8, space0);

[shader("fragment")]
float4 bindings_count_frag() : SV_Target0 {
	return images[1].SampleLevel(image_sampler, float2(0.5, 0.5), 0.0);
}
