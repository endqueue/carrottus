// crgpu_objects_test's format capabilities (card S51): a triangle
// covering the target, from the vertex index, at the push block's depth
// (params.x): the colour and depth attachments' draws.

struct Push {
	uint4 value;
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("vertex")]
float4 objects_caps_vert(uint vertex_id : SV_VertexID) : SV_Position {
	float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
	return float4(corner * 2.0 - 1.0, pc.params.x, 1.0);
}
