// crgpu_objects_test's format capabilities (card S51): a vertex
// attribute of the format under test fetched as float4 at location 0
// (CRGPU_FORMAT_CAPABILITY_VERTEX), passed on unchanged; the position is a
// triangle covering the target, from the vertex index.

struct VertexOutput {
	[[vk::location(0)]] nointerpolation float4 value : TEXCOORD0;
	float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput objects_caps_vertex_float_vert([[vk::location(0)]] float4 attribute : TEXCOORD0, uint vertex_id : SV_VertexID) {
	VertexOutput vout;
	float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
	vout.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	vout.value    = attribute;
	return vout;
}
