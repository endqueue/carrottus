// The vertex shader under rq_shadow.slang: a quad covering the whole
// target, from SV_VertexID (six vertices, two triangles), each corner at a
// point of a ground plane in world space that the fragment shader traces
// shadow rays from. Corner (u, v) is at world (u * extent.x, v * extent.y,
// height), and v = 0 is the top row (clip space is +Y up on every backend).

// One push constant block for both stages: rq_shadow.slang's 32 bytes
// first, this stage's after them.
struct Push {
	float4 fragment_params[2];
	float4 extent_and_height; // xy: the plane's extent from the origin, z: its height
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

struct VertexOutput {
	[[vk::location(0)]] float3 world : TEXCOORD0;
	float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput rq_shadow_vert(uint vertex_id : SV_VertexID) {
	const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
	const float2 c = corners[vertex_id];
	VertexOutput vout;
	vout.position = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
	vout.world    = float3(c.x * pc.extent_and_height.x, c.y * pc.extent_and_height.y, pc.extent_and_height.z);
	return vout;
}
