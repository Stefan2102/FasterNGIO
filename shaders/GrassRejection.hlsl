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
//
// With NGIO's grass cliffs or ignored shapes in the world, instance masks carry each instance's
// role: the volume pass traces each role separately, and a second pass (CliffRayGen) casts NGIO's
// cliff rays, whose hits an any-hit shader collects without ending the traversal.

#include "Shared/GrassQueryMath.hlsli"
#include "Shared/RejectionLayout.hlsli"

ConstantBuffer<RootConstants> g_constants : register(b0);

struct Payload
{
	uint hit;
	// The TLAS index of the instance hit.
	uint instance;
};

// A cliff ray gathers its hits in its candidate's scratch words of the output buffer, not in the
// payload: D3D12 on NVIDIA did not hand the any-hit shader the payload the ray-generation shader
// had initialised (Vulkan did). The scratch is the nearest hit's t and instance, the highest hit's
// t (on cliffs only, or on anything) and world normal, and which of the two the ray wants.
struct CliffPayload
{
	uint misses;
};

struct CliffHits
{
	float closestT;
	uint closest;
	float highestT;
	float3 normal;
};

static const uint kScratchClosestT = 0;
static const uint kScratchClosest = 1;
static const uint kScratchHighestT = 2;
static const uint kScratchCliffsOnly = 3;
static const uint kScratchNormal = 4;

uint CliffScratch()
{
	return DispatchRaysIndex().x * kCliffResultWords + kCliffWordScratch;
}

// The surface normal of a cliff-pass hit, in object space (unused by the volume pass).
struct HitAttributes
{
	float3 normal;
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
	attributes.normal = float3(0.0f, 0.0f, 0.0f);
	ReportHit(RayTMin(), 0, attributes);
}

// The cliff pass's ray runs from the segment's start (t = 0) to its end (t = 1), so its object-space
// origin and direction are the segment in the instance's space.
float3 CliffSegmentStart()
{
	return ObjectRayOrigin();
}

float3 CliffSegmentEnd()
{
	return ObjectRayOrigin() + ObjectRayDirection();
}

void ReportCliffHit(SegmentHit hit)
{
	if (hit.hit) {
		HitAttributes attributes;
		attributes.normal = hit.normal;
		ReportHit(hit.t, 0, attributes);
	}
}

// What a TLAS instance is to WorldIndex: its index there, and its role bits (with kInstanceSteep).
uint2 InstanceInfo(uint tlasIndex)
{
	StructuredBuffer<uint2> info = ResourceDescriptorHeap[g_constants.instanceInfo];
	return info[tlasIndex];
}

Payload TraceVolume(RaytracingAccelerationStructure scene, RayDesc ray, uint mask)
{
	// Both outcomes are written by a shader (closest-hit or miss): the payload is not assumed to
	// survive a traversal that runs neither.
	Payload payload;
	payload.hit = 0;
	payload.instance = kNoInstance;
	TraceRay(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_TRIANGLES, mask, 0, 1, 0, ray, payload);
	return payload;
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

	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[g_constants.output];
	if (g_constants.roles == 0) {
		Payload payload = TraceVolume(scene, ray, 0xFF);
		output[index] = payload.hit;
#if DEBUG_QUERIES
		if (IsDebugCandidate(index)) {
			WriteDebug(0, g_constants.tlas, 0, payload.hit, float4(ray.Origin, ray.TMax), float4(ray.Direction, 0.0f), candidate, float4(0, 0, 0, 0), float4(0, 0, 0, 0));
		}
#endif
		return;
	}

	// By role: a cliff is looked for even past an ordinary hit (a blade on a cliff may move onto it),
	// an object with ignored shapes only when nothing ordinary rejects the blade anyway.
	uint roles = TraceVolume(scene, ray, kRoleOrdinary).hit != 0 ? kRoleOrdinary : 0;
	if ((g_constants.roles & kRoleCliff) != 0 && TraceVolume(scene, ray, kRoleCliff).hit != 0) {
		roles |= kRoleCliff;
	}
	uint partIgnored = kNoInstance;
	if ((g_constants.roles & kRolePartIgnored) != 0 && (roles & kRoleOrdinary) == 0) {
		Payload part = TraceVolume(scene, ray, kRolePartIgnored);
		if (part.hit != 0) {
			roles |= kRolePartIgnored;
			partIgnored = InstanceInfo(part.instance).x;
		}
	}
	output[index * kVolumeRoleWords] = roles;
	output[index * kVolumeRoleWords + 1] = partIgnored;
}

CliffHits TraceCliffRay(RaytracingAccelerationStructure scene, float3 origin, float height, bool cliffsOnly)
{
	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[g_constants.output];
	uint scratch = CliffScratch();
	output[scratch + kScratchClosestT] = asuint(2.0f);
	output[scratch + kScratchClosest] = kNoInstance;
	output[scratch + kScratchHighestT] = asuint(-1.0f);
	output[scratch + kScratchCliffsOnly] = cliffsOnly ? 1u : 0u;
	output[scratch + kScratchNormal] = 0;
	output[scratch + kScratchNormal + 1] = 0;
	output[scratch + kScratchNormal + 2] = 0;

	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = float3(0.0f, 0.0f, height);
	ray.TMin = 0.0f;
	ray.TMax = 1.0f;
	CliffPayload payload;
	payload.misses = 0;
	// Every hit is collected and ignored, so the miss shader always ends the traversal.
	TraceRay(scene, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_TRIANGLES, 0xFF, g_constants.cliffHitGroupOffset, 1, 1, ray, payload);

	CliffHits hits;
	hits.closestT = asfloat(output[scratch + kScratchClosestT]);
	hits.closest = output[scratch + kScratchClosest];
	hits.highestT = asfloat(output[scratch + kScratchHighestT]);
	hits.normal = asfloat(uint3(output[scratch + kScratchNormal], output[scratch + kScratchNormal + 1], output[scratch + kScratchNormal + 2]));
	return hits;
}

// NGIO's grass cliff rays for one candidate (Rejection::TraceCliffRays does the same on the CPU):
// up from the blade, then, if that found a cliff, four rays beside the cliff point.
[shader("raygeneration")]
void CliffRayGen()
{
	uint index = DispatchRaysIndex().x;
	if (index >= g_constants.candidateCount) {
		return;
	}
	StructuredBuffer<float4> candidates = ResourceDescriptorHeap[g_constants.candidates];
	float4 candidate = candidates[index];
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g_constants.tlas];
	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[g_constants.output];
	uint base = index * kCliffResultWords;

	CliffHits up = TraceCliffRay(scene, candidate.xyz, kCliffRayLength, true);
	uint2 closest = up.closest != kNoInstance ? InstanceInfo(up.closest) : uint2(kNoInstance, 0);
	output[base + kCliffWordUpClosest] = closest.x;
	output[base + kCliffWordCliffT] = asuint(up.highestT);
	output[base + kCliffWordNormal] = asuint(up.normal.x);
	output[base + kCliffWordNormal + 1] = asuint(up.normal.y);
	output[base + kCliffWordNormal + 2] = asuint(up.normal.z);

	bool cliffPoint = closest.x != kNoInstance && (closest.y & kRoleCliff) != 0 && up.highestT >= 0.0f;
	float cliffZ = candidate.z + kCliffRayLength * up.highestT;
	float window = (closest.y & kInstanceSteep) != 0 ? kCliffSteepWindow : kCliffWindow;
	float offset = candidate.w;
	for (uint i = 0; i < 4; ++i) {
		uint slot = base + kCliffWordNeighbours + i * 2;
		if (!cliffPoint) {
			output[slot] = kNoInstance;
			output[slot + 1] = asuint(-1.0f);
			continue;
		}
		float2 side = float2(0.0f, 0.0f);
		if (i == 0) {
			side.x = offset;
		} else if (i == 1) {
			side.x = -offset;
		} else if (i == 2) {
			side.y = offset;
		} else {
			side.y = -offset;
		}
		CliffHits neighbour = TraceCliffRay(scene, float3(candidate.xy + side, cliffZ - window), 2.0f * window, false);
		output[slot] = neighbour.closest != kNoInstance ? InstanceInfo(neighbour.closest).x : kNoInstance;
		output[slot + 1] = asuint(neighbour.highestT);
	}
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
	payload.instance = InstanceIndex();
}

// Every cliff ray ends here (its hits are all ignored).
[shader("miss")]
void CliffMiss(inout CliffPayload payload)
{
	payload.misses += 1;
}

[shader("anyhit")]
void CliffAnyHit(inout CliffPayload payload, in HitAttributes attributes)
{
	RWStructuredBuffer<uint> output = ResourceDescriptorHeap[g_constants.output];
	uint scratch = CliffScratch();
	float t = RayTCurrent();
	if (t < asfloat(output[scratch + kScratchClosestT])) {
		output[scratch + kScratchClosestT] = asuint(t);
		output[scratch + kScratchClosest] = InstanceIndex();
	}
	bool counts = output[scratch + kScratchCliffsOnly] == 0 || (InstanceInfo(InstanceIndex()).y & kRoleCliff) != 0;
	if (counts && t > asfloat(output[scratch + kScratchHighestT])) {
		float3 normal = normalize(mul((float3x3)ObjectToWorld3x4(), attributes.normal));
		output[scratch + kScratchHighestT] = asuint(t);
		output[scratch + kScratchNormal] = asuint(normal.x);
		output[scratch + kScratchNormal + 1] = asuint(normal.y);
		output[scratch + kScratchNormal + 2] = asuint(normal.z);
	}
	IgnoreHit();
}

[shader("intersection")]
void TriangleIntersection()
{
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelTrianglesOffset) + PrimitiveIndex() * kTriangleStride;
	float3 a = asfloat(model.Load3(offset));
	float3 b = asfloat(model.Load3(offset + 12));
	float3 c = asfloat(model.Load3(offset + 24));
	if (g_constants.pass == kPassCliff) {
		ReportCliffHit(SegmentHitTriangle(CliffSegmentStart(), CliffSegmentEnd(), a, b, c));
		return;
	}
	QuerySegment s = ObjectQuery();
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
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelHullsOffset) + PrimitiveIndex() * kHullStride;
	uint4 record = model.Load4(offset);
	BufferHull hull;
	hull.planeOffset = record.x;
	hull.planeCount = record.y;
	hull.faceOffset = record.z;
	hull.faceCount = record.w;
	hull.radius = asfloat(model.Load(offset + 16));
	if (g_constants.pass == kPassCliff) {
		ReportCliffHit(SegmentHitHull(CliffSegmentStart(), CliffSegmentEnd(), hull));
		return;
	}
	QuerySegment s = ObjectQuery();
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
	ByteAddressBuffer model = ModelBuffer();
	uint offset = model.Load(kModelCapsulesOffset) + PrimitiveIndex() * kCapsuleStride;
	float3 a = asfloat(model.Load3(offset));
	float3 b = asfloat(model.Load3(offset + 12));
	float radius = asfloat(model.Load(offset + 24));
	if (g_constants.pass == kPassCliff) {
		ReportCliffHit(SegmentHitCapsule(CliffSegmentStart(), CliffSegmentEnd(), a, b, radius));
		return;
	}
	QuerySegment s = ObjectQuery();
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
