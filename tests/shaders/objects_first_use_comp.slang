// spudgpu_objects_test's attachment image written by compute after its
// first use, a clear (spudgpu_create_image(), the first use of an attachment
// image, card S38): texel (x, y) of the left half becomes x + 8 y + 1; the
// right half keeps the clear.

[[vk::binding(0, 0)]] RWTexture2D<float> target : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void objects_first_use_comp(uint3 dispatch_id : SV_DispatchThreadID) {
	if (dispatch_id.x < 4u && dispatch_id.y < 8u)
		target[dispatch_id.xy] = float(dispatch_id.x + dispatch_id.y * 8u + 1u);
}
