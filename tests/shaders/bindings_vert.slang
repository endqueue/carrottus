// crgpu_objects_test's set bindings draw (card S27): one triangle over the
// whole target from the vertex index, with no vertex input and no set.

struct VertexOutput {
	float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput bindings_vert(uint vertex_id : SV_VertexID) {
	VertexOutput o;
	const float2 corner = float2(float((vertex_id << 1u) & 2u), float(vertex_id & 2u));
	o.position          = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	return o;
}
