// crgpu_objects_test's indirect dispatch shader (card A15): each workgroup
// (1 x 1 x 1) adds 1 to word 0 and writes its own value,
// (x | y << 8 | z << 16) + 1, at word 1 + (z * Y + y) * X + x, X and Y
// pushed. A group run twice counts twice; a group never run leaves its
// slot as it was.

[[vk::binding(0, 0)]] RWByteAddressBuffer words : register(u0, space0);

struct Push {
	uint groups_x;
	uint groups_y;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("compute")]
[numthreads(1, 1, 1)]
void objects_dispatch_ids_comp(uint3 group : SV_GroupID) {
	uint before;
	words.InterlockedAdd(0, 1u, before);
	uint slot = 1u + (group.z * pc.groups_y + group.y) * pc.groups_x + group.x;
	words.Store(slot * 4u, (group.x | (group.y << 8) | (group.z << 16)) + 1u);
}
