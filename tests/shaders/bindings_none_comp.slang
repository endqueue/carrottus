// Every binding of a set (card S27) is a rule for each set a module reads:
// this one reads set 1 alone, though the pipeline's layouts name set 0
// too, and is created on every backend.
// Set 0 is a uniform buffer at 0, a sampled image at 5 and a sampler at 8,
// set 1 a storage buffer at 0 (crgpu_objects_test, test_set_bindings).

[[vk::binding(0, 1)]] RWStructuredBuffer<float4> result : register(u0, space1);

[shader("compute")]
[numthreads(1, 1, 1)]
void bindings_none_comp() {
	result[0] = float4(1.0, 2.0, 3.0, 4.0);
}
