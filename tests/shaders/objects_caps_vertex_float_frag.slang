// crgpu_objects_test's format capabilities (card S51): the
// vertex attribute objects_caps_vertex_float_vert fetched, written to a
// four-component float target.

struct FragmentInput {
	[[vk::location(0)]] nointerpolation float4 value : TEXCOORD0;
};

[shader("fragment")]
float4 objects_caps_vertex_float_frag(FragmentInput fin) : SV_Target0 {
	return fin.value;
}
