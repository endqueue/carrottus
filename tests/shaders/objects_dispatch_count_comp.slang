// crgpu_objects_test's group counter (card A15): each workgroup (1 x 1 x 1)
// adds 1 to word 0, and writes nothing else, so any number of groups fits.

[[vk::binding(0, 0)]] RWByteAddressBuffer words : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void objects_dispatch_count_comp(uint3 group : SV_GroupID) {
	uint before;
	words.InterlockedAdd(0, 1u, before);
}
