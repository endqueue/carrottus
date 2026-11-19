// crgpu_objects_test's argument writer (card A15): one workgroup writes
// the three group counts {4, 1, 1} an indirect dispatch reads next.

[[vk::binding(0, 0)]] RWByteAddressBuffer args : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void objects_dispatch_args_comp(uint3 group : SV_GroupID) {
	args.Store3(0, uint3(4u, 1u, 1u));
}
