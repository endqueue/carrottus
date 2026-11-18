// crgpu_objects_test's dual-source fragment shader (card S50): the push
// block's colour as the first output and its params as the second, which
// the blender reads and no attachment receives. Both members carry a
// location (an index alone is an error, and an explicit location on one
// member alone is refused); on Metal it takes the default route, which
// writes the second as [[color(0), index(1)]] (Slang's own route writes
// [[color(1)]], a second attachment).

struct FragmentInput {
	[[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct Push {
	float4 color;
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

struct FragmentOut {
	[[vk::location(0)]] float4 color : SV_Target0;
	[[vk::location(0), vk::index(1)]] float4 blend : SV_Target1;
};

[shader("fragment")]
FragmentOut objects_dual_source_frag(FragmentInput fin) {
	FragmentOut fout;
	fout.color = pc.color;
	fout.blend = pc.params;
	return fout;
}
