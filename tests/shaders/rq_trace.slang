// S12's step 0 (SPUDGPU_DEVICE_TODO.md, "Hardware ray tracing"): one ray per
// pixel from a compute shader, through an inline ray query, the way the ray
// tracing work ahead traces. Each ray starts on a plane at z = origin.z and
// goes along +z. Triangle candidates are committed as the query reports them
// (non-opaque ones are committed by hand, so both paths run); a procedural
// candidate (an AABB) is a sphere inscribed in its box, intersected here. The
// committed hit's t, instance index, instance id, geometry index, primitive
// index and barycentrics are written for the CPU to check.
//
// Bindings, the same on every target: set 0 binding 0 is the top level, set
// 0 binding 1 the hit records; the push constants are SpudGPU's block (D3D12:
// b0 in space SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS, 4). The hit records are a
// byte-address buffer, 32 bytes each in RqHit's order, since a storage
// buffer is a raw view on D3D12 (as objects_double_comp.slang's are); a
// structured buffer declared over a raw view is outside D3D12's rules.

struct RqHit {
	float t;                  // the committed t, or -1 for a miss
	uint instance_index;      // the instance's index in the top level
	uint instance_id;         // the instance's 24-bit user index
	uint geometry_index;      // the geometry's index in its bottom level
	uint primitive_index;     // the primitive's index in its geometry
	uint kind;                // 0 miss, 1 triangle, 2 procedural
	float2 barycentrics;      // a triangle's (u, v); 0 for the others
};

struct RqParams {
	float4 origin;            // xy: the grid's corner, z: the plane, w: t min
	float4 step_and_tmax;     // xy: one pixel's step, z: t max
	uint width;
	uint height;
	uint cull_mask;           // the instance mask the rays test
	uint ray_flags;           // RAY_FLAG_* given to the query at run time
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure rq_scene : register(t0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer rq_hits : register(u1, space0);
[[vk::push_constant]] ConstantBuffer<RqParams> rq_params : register(b0, space4);

// The sphere inscribed in the AABB [0, 1]^3 of object space, along the ray
// in object space; -1 for none.
float rq_sphere_t(float3 origin, float3 direction) {
	const float3 oc = origin - float3(0.5, 0.5, 0.5);
	const float a = dot(direction, direction);
	const float b = dot(oc, direction);
	const float c = dot(oc, oc) - 0.25;
	const float d = b * b - a * c;
	if (d < 0.0)
		return -1.0;
	return (-b - sqrt(d)) / a;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rq_trace(uint3 id: SV_DispatchThreadID) {
	if (id.x >= rq_params.width || id.y >= rq_params.height)
		return;

	RayDesc ray;
	ray.Origin = float3(rq_params.origin.xy + (float2(id.xy) + 0.5) * rq_params.step_and_tmax.xy, rq_params.origin.z);
	ray.Direction = float3(0.0, 0.0, 1.0);
	ray.TMin = rq_params.origin.w;
	ray.TMax = rq_params.step_and_tmax.z;

	RayQuery<RAY_FLAG_NONE> query;
	query.TraceRayInline(rq_scene, rq_params.ray_flags, rq_params.cull_mask, ray);
	while (query.Proceed()) {
		if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
			const float t = rq_sphere_t(query.CandidateObjectRayOrigin(), query.CandidateObjectRayDirection());
			if (t >= query.RayTMin() && t <= query.CommittedRayT())
				query.CommitProceduralPrimitiveHit(t);
		} else {
			query.CommitNonOpaqueTriangleHit();
		}
	}

	RqHit hit;
	hit.t = -1.0;
	hit.instance_index = 0;
	hit.instance_id = 0;
	hit.geometry_index = 0;
	hit.primitive_index = 0;
	hit.kind = 0;
	hit.barycentrics = float2(0.0, 0.0);
	const uint status = query.CommittedStatus();
	if (status != COMMITTED_NOTHING) {
		hit.t = query.CommittedRayT();
		hit.instance_index = query.CommittedInstanceIndex();
		hit.instance_id = query.CommittedInstanceID();
		hit.geometry_index = query.CommittedGeometryIndex();
		hit.primitive_index = query.CommittedPrimitiveIndex();
		if (status == COMMITTED_TRIANGLE_HIT) {
			hit.kind = 1;
			hit.barycentrics = query.CommittedTriangleBarycentrics();
		} else {
			hit.kind = 2;
		}
	}
	const uint at = (id.y * rq_params.width + id.x) * 32u;
	rq_hits.Store(at + 0u, asuint(hit.t));
	rq_hits.Store(at + 4u, hit.instance_index);
	rq_hits.Store(at + 8u, hit.instance_id);
	rq_hits.Store(at + 12u, hit.geometry_index);
	rq_hits.Store(at + 16u, hit.primitive_index);
	rq_hits.Store(at + 20u, hit.kind);
	rq_hits.Store(at + 24u, asuint(hit.barycentrics.x));
	rq_hits.Store(at + 28u, asuint(hit.barycentrics.y));
}
