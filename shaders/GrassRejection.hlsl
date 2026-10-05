// Grass-in-object rejection as a ray-tracing pipeline (DXR, or Vulkan through SPIR-V).
//
// Every candidate blade is one ray launch; one dispatch covers a whole frame's candidates. The ray only drives traversal: it runs vertically
// through the whole query volume (NGIO's capsule, or a bare ray), from r below the segment to r
// above it, so it enters every primitive AABB (grown by the query radius) the volume can touch.
// Procedural intersection shaders cannot read the payload, so they rebuild the exact query from
// the candidate buffer instead.
//
// Collision lives in one ByteAddressBuffer per model, found through InstanceID() (its
// descriptor heap index). Each BLAS geometry is one primitive kind with its own hit group.

#include "Shared/GrassQueryMath.hlsli"
#include "Shared/RejectionLayout.hlsli"

ConstantBuffer<RootConstants> g_constants : register(b0);

struct Payload
{
	uint hit;
};

struct HitAttributes
{
	uint unused;
};

#if DEBUG_QUERIES
// FASTERNGIO_DEBUG_CANDIDATE: one record per shader invocation for that candidate; word 0 of the
// buffer counts them.
struct DebugRecord
{
	uint stage;
	uint instance;
	uint primitive;
	uint result;
	float4 p;
	float4 q;
	float4 a;
	float4 b;
	float4 c;
};

bool IsDebugCandidate(uint candidateIndex)
{
	return g_constants.debugCandidate == candidateIndex;
}

void WriteDebug(uint stage, uint instance, uint primitive, uint result, float4 p, float4 q, float4 a, float4 b, float4 c)
{
	RWByteAddressBuffer debug = ResourceDescriptorHeap[g_constants.debugBuffer];
	uint slot;
	debug.InterlockedAdd(0, 1, slot);
	if (slot >= kDebugMaxRecords) {
		return;
	}
	uint base = kDebugHeaderBytes + slot * kDebugRecordBytes;
	debug.Store4(base, uint4(stage, instance, primitive, result));
	debug.Store4(base + 16, asuint(p));
	debug.Store4(base + 32, asuint(q));
	debug.Store4(base + 48, asuint(a));
	debug.Store4(base + 64, asuint(b));
	debug.Store4(base + 80, asuint(c));
}

uint CurrentCandidate()
{
	return DispatchRaysIndex().x;
}
#endif

struct QuerySegment
{
	float3 p;
	float3 q;
	float r;
};

// Rebuilds this launch's exact world-space query and moves it into the instance's space. Reading
// the candidate again (DispatchRaysIndex is valid in every ray-tracing stage) keeps the query
// independent of how the ray parameterises it.
QuerySegment ObjectQuery()
{
	StructuredBuffer<float4> candidates = ResourceDescriptorHeap[g_constants.candidates];
	float4 candidate = candidates[DispatchRaysIndex().x];
	float3x4 worldToObject = WorldToObject3x4();
	float3 bottom = candidate.xyz;
	float3 top = bottom + float3(0.0f, 0.0f, g_constants.segmentLength);
	QuerySegment s;
	s.p = mul(worldToObject, float4(bottom, 1.0f));
	s.q = mul(worldToObject, float4(top, 1.0f));
	// Uniform scale: any column of the linear part has length 1 / instance scale.
	float scale = length(float3(worldToObject._m00, worldToObject._m10, worldToObject._m20));
#if QUERY_RAY
	s.r = 0.0f;
#else
	s.r = candidate.w * scale;
#endif
	return s;
}

// Lanes of one wave hit different instances, so the descriptor index is not wave-uniform.
ByteAddressBuffer ModelBuffer()
{
	return ResourceDescriptorHeap[NonUniformResourceIndex(InstanceID())];
}

void Accept()
{
	HitAttributes attributes;
	attributes.unused = 0;
	ReportHit(RayTMin(), 0, attributes);
}

[shader("raygeneration")]
void GrassRayGen()
{
	uint index = DispatchRaysIndex().x;
	if (index >= g_constants.candidateCount) {
		return;
	}
	StructuredBuffer<float4> candidates = ResourceDescriptorHeap[g_constants.candidates];
	float4 candidate = candidates[index];
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g_constants.tlas];

	RayDesc ray;
#if QUERY_RAY
	ray.Origin = candidate.xyz;
	ray.Direction = float3(0.0f, 0.0f, 1.0f);
	ray.TMin = 0.0f;
	ray.TMax = g_constants.segmentLength;
#else
	float r = candidate.w;
	ray.Origin = float3(candidate.xy, candidate.z - r);
	ray.Direction = float3(0.0f, 0.0f, r);
	ray.TMin = 0.0f;
	ray.TMax = (g_constants.segmentLength + 2.0f * r) / r;
#endif

	// Both outcomes are written by a shader (closest-hit or miss): the payload is not assumed to
	// survive a traversal that runs neither.
	Payload payload;
	payload.hit = 0;
	TraceRay(scene,
		RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_TRIANGLES,
		0xFF, 0, 1, 0, ray, payload);

	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[g_constants.output];
	output[index] = payload.hit;
#if DEBUG_QUERIES
	if (IsDebugCandidate(index)) {
		WriteDebug(0, g_constants.tlas, 0, payload.hit, float4(ray.Origin, ray.TMax), float4(ray.Direction, 0.0f), candidate, float4(0, 0, 0, 0), float4(0, 0, 0, 0));
	}
#endif
}

[shader("miss")]
void GrassMiss(inout Payload payload)
{
	payload.hit = 0;
}

[shader("closesthit")]
void GrassClosestHit(inout Payload payload, in HitAttributes attributes)
{
	payload.hit = 1;
}

[shader("intersection")]
void TriangleIntersection()
{
	QuerySegment s = ObjectQuery();
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelTrianglesOffset) + PrimitiveIndex() * kTriangleStride;
	float3 a = asfloat(model.Load3(offset));
	float3 b = asfloat(model.Load3(offset + 12));
	float3 c = asfloat(model.Load3(offset + 24));
	float radius = asfloat(model.Load(offset + 36));
	bool overlaps = CapsuleOverlapsTriangle(s.p, s.q, s.r, a, b, c, radius);
#if DEBUG_QUERIES
	if (IsDebugCandidate(CurrentCandidate())) {
		WriteDebug(1, InstanceID(), PrimitiveIndex(), overlaps ? 1u : 0u, float4(s.p, s.r), float4(s.q, radius), float4(a, 0), float4(b, 0), float4(c, 0));
	}
#endif
	if (overlaps) {
		Accept();
	}
}

// A hull record of this instance's model buffer, as the shared hull tests read it.
struct BufferHull
{
	uint planeOffset;
	uint planeCount;
	uint faceOffset;
	uint faceCount;
	float radius;

	uint PlaneCount() { return planeCount; }
	float4 Plane(uint i) { return asfloat(ModelBuffer().Load4(planeOffset + i * kHullPlaneStride)); }
	uint FaceCount() { return faceCount; }
	HullFace Face(uint i)
	{
		ByteAddressBuffer model = ModelBuffer();
		uint offset = faceOffset + i * kHullFaceStride;
		HullFace face;
		face.a = asfloat(model.Load3(offset));
		face.b = asfloat(model.Load3(offset + 12));
		face.c = asfloat(model.Load3(offset + 24));
		return face;
	}
	float Radius() { return radius; }
};

[shader("intersection")]
void HullIntersection()
{
	QuerySegment s = ObjectQuery();
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelHullsOffset) + PrimitiveIndex() * kHullStride;
	uint4 record = model.Load4(offset);
	BufferHull hull;
	hull.planeOffset = record.x;
	hull.planeCount = record.y;
	hull.faceOffset = record.z;
	hull.faceCount = record.w;
	hull.radius = asfloat(model.Load(offset + 16));
#if DEBUG_QUERIES
	if (IsDebugCandidate(CurrentCandidate())) {
		WriteDebug(2, InstanceID(), PrimitiveIndex(), record.w, float4(s.p, s.r), float4(s.q, hull.radius), float4(float3(record.xyz), float(record.w)),
			asfloat(model.Load4(record.x)), float4(asfloat(model.Load3(record.z)), 0));
		for (uint i = 0; i < hull.FaceCount(); ++i) {
			HullFace face = hull.Face(i);
			WriteDebug(4, i, hull.faceOffset + i * kHullFaceStride, CapsuleOverlapsTriangle(s.p, s.q, s.r, face.a, face.b, face.c, hull.radius) ? 1u : 0u,
				float4(s.p, sqrt(SegmentTriangleDistanceSq(s.p, s.q, face.a, face.b, face.c))), float4(s.q, hull.radius), float4(face.a, 0), float4(face.b, 0),
				float4(face.c, 0));
		}
	}
#endif
	if (CapsuleOverlapsHull(s.p, s.q, s.r, hull)) {
		Accept();
	}
}

[shader("intersection")]
void CapsuleIntersection()
{
	QuerySegment s = ObjectQuery();
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelCapsulesOffset) + PrimitiveIndex() * kCapsuleStride;
	float3 a = asfloat(model.Load3(offset));
	float3 b = asfloat(model.Load3(offset + 12));
	float radius = asfloat(model.Load(offset + 24));
	bool overlaps = CapsuleOverlapsCapsule(s.p, s.q, s.r, a, b, radius);
#if DEBUG_QUERIES
	if (IsDebugCandidate(CurrentCandidate())) {
		WriteDebug(3, InstanceID(), PrimitiveIndex(), overlaps ? 1u : 0u, float4(s.p, s.r), float4(s.q, radius), float4(a, 0), float4(b, 0), float4(0, 0, 0, 0));
	}
#endif
	if (overlaps) {
		Accept();
	}
}
