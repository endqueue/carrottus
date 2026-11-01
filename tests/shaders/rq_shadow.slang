// S12's step 0 (SPUDGPU_DEVICE_TODO.md, "Hardware ray tracing"): a shadow
// ray from a fragment shader, the way ray-traced shadows trace. The ray
// leaves the fragment's world position toward the light and stops at the
// first hit anywhere along it (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, as a
// flag the query type carries), testing only instances whose mask meets the
// cull mask. The fragment is 0 where something is in the way and 1 where
// nothing is; alpha is 1.
//
// Bindings, the same on every target: set 0 binding 0 is the top level; the
// push constants are SpudGPU's block (D3D12: b0 in space
// SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS, 4). The world position arrives at
// location 0 (TEXCOORD0).

struct RqShadowParams {
	float4 to_light;          // xyz: the direction toward the light, w: t min
	float t_max;              // how far the ray looks
	uint cull_mask;           // the instance mask the ray tests
	uint pad0;
	uint pad1;
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure rq_scene : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<RqShadowParams> rq_params : register(b0, space4);

[shader("fragment")]
float4 rq_shadow([[vk::location(0)]] float3 world: TEXCOORD0) : SV_Target0 {
	RayDesc ray;
	ray.Origin = world;
	ray.Direction = rq_params.to_light.xyz;
	ray.TMin = rq_params.to_light.w;
	ray.TMax = rq_params.t_max;

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
	query.TraceRayInline(rq_scene, RAY_FLAG_NONE, rq_params.cull_mask, ray);
	while (query.Proceed()) {
	}
	const float lit = query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
	return float4(lit, lit, lit, 1.0);
}
