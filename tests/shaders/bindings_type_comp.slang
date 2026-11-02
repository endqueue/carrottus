// Every binding of a set (card S27): the set declared whole but 5, an image
// in the layout, declared a buffer. Refused on Metal.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8,
// set 1 a storage buffer at 0 (spudgpu_objects_test, test_set_bindings).

struct Unread {
	float4 value;
};

[[vk::binding(0, 0)]] ConstantBuffer<Unread> unread : register(b0, space0);
[[vk::binding(5, 0)]] StructuredBuffer<float4> not_an_image : register(t5, space0);
[[vk::binding(8, 0)]] SamplerState image_sampler : register(s8, space0);

[[vk::binding(0, 1)]] RWStructuredBuffer<float4> result : register(u0, space1);

[shader("compute")]
[numthreads(1, 1, 1)]
void bindings_type_comp() {
	result[0] = not_an_image[0];
}
