// crgpu_objects_test's format capabilities (card S51): the
// vertex attribute objects_caps_vertex_uint_vert fetched, written to a
// four-component uint target.

struct FragmentInput {
	[[vk::location(0)]] nointerpolation uint4 value : TEXCOORD0;
};

[shader("fragment")]
uint4 objects_caps_vertex_uint_frag(FragmentInput fin) : SV_Target0 {
	return fin.value;
}
