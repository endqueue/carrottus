// Every binding of a set (card S27): the set declared whole but 5, an image
// in the layout, declared a buffer. Refused on Metal.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8
// (spudgpu_objects_test, test_set_bindings).

struct Unread {
	float4 value;
};

[[vk::binding(0, 0)]] ConstantBuffer<Unread> unread : register(b0, space0);
[[vk::binding(5, 0)]] StructuredBuffer<float4> not_an_image : register(t5, space0);
[[vk::binding(8, 0)]] SamplerState image_sampler : register(s8, space0);

[shader("fragment")]
float4 bindings_type_frag() : SV_Target0 {
	return not_an_image[0];
}
