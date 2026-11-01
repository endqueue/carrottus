// spudgpu_objects_test's fragment shader: the draw's colour, plus a tint
// from a uniform buffer, plus a texel of a 2 x 2 texture as much as the
// draw asks for.

struct FragmentInput {
	[[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct Tint {
	float4 add;
};

[[vk::binding(0, 0)]] ConstantBuffer<Tint> tint : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> tex : register(t1, space0);
[[vk::binding(2, 0)]] SamplerState smp : register(s2, space0);

struct Push {
	float4 color;
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("fragment")]
float4 objects_draw_frag(FragmentInput fin) : SV_Target0 {
	return pc.color + tint.add + tex.Sample(smp, fin.uv) * pc.params.y;
}
