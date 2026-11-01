// spudgpu_objects_test's vertex shader: a unit quad's corner (per vertex)
// moved to a cell of an 8 x 8 grid (per instance), so each draw call's
// vertex and instance fetches decide which cells it covers. Clip space is
// +Y up on every backend, so y is negated to keep cell (0, 0) at the top
// left.

struct Push {
	float4 color;
	// x: depth; y: how much of the texture to add; zw: the mesh shader's
	// first cell.
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

struct VertexInput {
	[[vk::location(0)]] float2 corner : TEXCOORD0;
	[[vk::location(1)]] float2 cell : TEXCOORD1;
};

struct VertexOutput {
	[[vk::location(0)]] float2 uv : TEXCOORD0;
	float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput objects_draw_vert(VertexInput vin) {
	VertexOutput vout;
	float2 p      = (vin.cell + vin.corner) / 8.0;
	float2 ndc    = p * 2.0 - 1.0;
	vout.position = float4(ndc.x, -ndc.y, pc.params.x, 1.0);
	vout.uv       = vin.corner;
	return vout;
}
