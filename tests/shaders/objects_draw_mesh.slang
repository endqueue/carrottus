// spudgpu_objects_test's mesh shader: one cell's quad per workgroup, from
// the cell in the push constants along x. Clip space is +Y up on every
// backend, so y is negated to keep cell (0, 0) at the top left, as the
// vertex shader does.

struct Push {
	float4 color;
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

struct VertexOutput {
	[[vk::location(0)]] float2 uv : TEXCOORD0;
	float4 position : SV_Position;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void objects_draw_mesh(
	uint3 group_id : SV_GroupID,
	OutputVertices<VertexOutput, 4> out_vertices,
	OutputIndices<uint3, 2> out_triangles) {
	SetMeshOutputCounts(4, 2);
	float2 cell = pc.params.zw + float2(float(group_id.x), 0.0);
	for (int i = 0; i < 4; ++i) {
		float2 c   = float2(float(i & 1), float(i >> 1));
		float2 p   = (cell + c) / 8.0;
		float2 ndc = p * 2.0 - 1.0;
		VertexOutput v;
		v.position      = float4(ndc.x, -ndc.y, pc.params.x, 1.0);
		v.uv            = c;
		out_vertices[i] = v;
	}
	out_triangles[0] = uint3(0, 1, 2);
	out_triangles[1] = uint3(2, 1, 3);
}
