// Exact overlap tests between a grass query volume and collision primitives.
// Shared by the HLSL intersection shaders and the C++ reference (through HlslShim.h), so the
// two can never disagree about what "inside an object" means.
//
// The query is a capsule: segment [p, q] swept by radius r (a ray when r == 0). Every primitive
// carries its own Havok convex radius, so each test is "distance(segment, primitive core) <=
// r + primitiveRadius", or containment for solids.

#ifndef FASTERNGIO_GRASS_QUERY_MATH_HLSLI
#define FASTERNGIO_GRASS_QUERY_MATH_HLSLI

// Squared distance from point p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
float PointTriangleDistanceSq(float3 p, float3 a, float3 b, float3 c)
{
	float3 ab = b - a;
	float3 ac = c - a;
	float3 ap = p - a;
	float d1 = dot(ab, ap);
	float d2 = dot(ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f) {
		return dot(ap, ap);
	}
	float3 bp = p - b;
	float d3 = dot(ab, bp);
	float d4 = dot(ac, bp);
	if (d3 >= 0.0f && d4 <= d3) {
		return dot(bp, bp);
	}
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
		float v = d1 / (d1 - d3);
		float3 d = ap - v * ab;
		return dot(d, d);
	}
	float3 cp = p - c;
	float d5 = dot(ab, cp);
	float d6 = dot(ac, cp);
	if (d6 >= 0.0f && d5 <= d6) {
		return dot(cp, cp);
	}
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
		float w = d2 / (d2 - d6);
		float3 d = ap - w * ac;
		return dot(d, d);
	}
	float va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
		float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		float3 d = bp - w * (c - b);
		return dot(d, d);
	}
	float denom = 1.0f / (va + vb + vc);
	float v = vb * denom;
	float w = vc * denom;
	float3 d = ap - ab * v - ac * w;
	return dot(d, d);
}

// Squared distance between segments [p1, q1] and [p2, q2] (Ericson 5.1.9).
float SegmentSegmentDistanceSq(float3 p1, float3 q1, float3 p2, float3 q2)
{
	const float epsilon = 1.0e-12f;
	float3 d1 = q1 - p1;
	float3 d2 = q2 - p2;
	float3 r = p1 - p2;
	float a = dot(d1, d1);
	float e = dot(d2, d2);
	float f = dot(d2, r);
	float s = 0.0f;
	float t = 0.0f;
	if (a <= epsilon && e <= epsilon) {
		return dot(r, r);
	}
	if (a <= epsilon) {
		t = saturate(f / e);
	} else {
		float c = dot(d1, r);
		if (e <= epsilon) {
			s = saturate(-c / a);
		} else {
			float b = dot(d1, d2);
			float denom = a * e - b * b;
			s = denom != 0.0f ? saturate((b * f - c * e) / denom) : 0.0f;
			t = (b * s + f) / e;
			if (t < 0.0f) {
				t = 0.0f;
				s = saturate(-c / a);
			} else if (t > 1.0f) {
				t = 1.0f;
				s = saturate((b - c) / a);
			}
		}
	}
	float3 c1 = p1 + d1 * s;
	float3 c2 = p2 + d2 * t;
	float3 d = c1 - c2;
	return dot(d, d);
}

// True when segment [p, q] crosses triangle abc (Moller-Trumbore, both sides).
bool SegmentIntersectsTriangle(float3 p, float3 q, float3 a, float3 b, float3 c)
{
	float3 dir = q - p;
	float3 e1 = b - a;
	float3 e2 = c - a;
	float3 h = cross(dir, e2);
	float det = dot(e1, h);
	if (abs(det) < 1.0e-12f) {
		return false;
	}
	float inv = 1.0f / det;
	float3 s = p - a;
	float u = dot(s, h) * inv;
	if (u < 0.0f || u > 1.0f) {
		return false;
	}
	float3 qv = cross(s, e1);
	float v = dot(dir, qv) * inv;
	if (v < 0.0f || u + v > 1.0f) {
		return false;
	}
	float t = dot(e2, qv) * inv;
	return t >= 0.0f && t <= 1.0f;
}

// Squared distance between segment [p, q] and triangle abc.
float SegmentTriangleDistanceSq(float3 p, float3 q, float3 a, float3 b, float3 c)
{
	if (SegmentIntersectsTriangle(p, q, a, b, c)) {
		return 0.0f;
	}
	float best = PointTriangleDistanceSq(p, a, b, c);
	best = min(best, PointTriangleDistanceSq(q, a, b, c));
	best = min(best, SegmentSegmentDistanceSq(p, q, a, b));
	best = min(best, SegmentSegmentDistanceSq(p, q, b, c));
	best = min(best, SegmentSegmentDistanceSq(p, q, c, a));
	return best;
}

// Capsule [p, q] x r against a triangle with convex radius triRadius.
bool CapsuleOverlapsTriangle(float3 p, float3 q, float r, float3 a, float3 b, float3 c, float triRadius)
{
	float reach = r + triRadius;
	return SegmentTriangleDistanceSq(p, q, a, b, c) <= reach * reach;
}

// Capsule [p, q] x r against capsule [a, b] x capsuleRadius.
bool CapsuleOverlapsCapsule(float3 p, float3 q, float r, float3 a, float3 b, float capsuleRadius)
{
	float reach = r + capsuleRadius;
	return SegmentSegmentDistanceSq(p, q, a, b) <= reach * reach;
}

// Point x is outside the half-space of plane (dot(n, x) + w <= 0 is inside).
bool OutsidePlane(float4 plane, float3 x)
{
	return plane.x * x.x + plane.y * x.y + plane.z * x.z + plane.w > 0.0f;
}

// One face of a convex hull.
struct HullFace
{
	float3 a;
	float3 b;
	float3 c;
};

// Convex hulls are stored differently on each side (a model buffer on the GPU, the collision model
// on the CPU), so the tests take the hull as a type with PlaneCount(), Plane(i) (float4),
// FaceCount(), Face(i) (HullFace) and Radius() (its convex radius).

// Point x is inside every plane of the hull.
template <typename Hull>
bool HullContainsPoint(Hull hull, float3 x)
{
	for (uint i = 0; i < hull.PlaneCount(); ++i) {
		if (OutsidePlane(hull.Plane(i), x)) {
			return false;
		}
	}
	return true;
}

// Capsule [p, q] x r against a convex hull: either endpoint inside it, or the capsule within the
// hull's convex radius of a face.
template <typename Hull>
bool CapsuleOverlapsHull(float3 p, float3 q, float r, Hull hull)
{
	if (HullContainsPoint(hull, p) || HullContainsPoint(hull, q)) {
		return true;
	}
	for (uint i = 0; i < hull.FaceCount(); ++i) {
		HullFace face = hull.Face(i);
		if (CapsuleOverlapsTriangle(p, q, r, face.a, face.b, face.c, hull.Radius())) {
			return true;
		}
	}
	return false;
}

#endif
