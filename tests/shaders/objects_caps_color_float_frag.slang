// crgpu_objects_test's format capabilities (card S51): the push
// block's value, given as raw bits, written to a float colour attachment
// (CRGPU_FORMAT_CAPABILITY_COLOR_ATTACHMENT, and BLEND where blended).

struct Push {
	uint4 value;
	float4 params;
};

[[vk::push_constant]] ConstantBuffer<Push> pc : register(b0, space4);

[shader("fragment")]
float4 objects_caps_color_float_frag(float4 position : SV_Position) : SV_Target0 {
	return asfloat(pc.value);
}
