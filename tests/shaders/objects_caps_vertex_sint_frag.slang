// crgpu_objects_test's format capabilities (card S51): the
// vertex attribute objects_caps_vertex_sint_vert fetched, written to a
// four-component sint target.

struct FragmentInput {
	[[vk::location(0)]] nointerpolation int4 value : TEXCOORD0;
};

[shader("fragment")]
int4 objects_caps_vertex_sint_frag(FragmentInput fin) : SV_Target0 {
	return fin.value;
}
