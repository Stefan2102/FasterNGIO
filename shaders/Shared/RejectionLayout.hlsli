// The data layouts GrassRejection.hlsl and GpuRejector.cpp exchange. Included by the shader and,
// through HlslShim.h, by the C++ that fills the buffers, so the two cannot drift apart.

#ifndef FASTERNGIO_REJECTION_LAYOUT_HLSLI
#define FASTERNGIO_REJECTION_LAYOUT_HLSLI

// Pushed as root constants (Vulkan push constants), one 32-bit value per member.
struct RootConstants
{
	uint tlas;
	uint candidates;
	uint output;
	uint candidateCount;
	float segmentLength;
	// Diagnostics: the frame-global candidate index to record, or kNoDebugCandidate.
	uint debugCandidate;
	uint debugBuffer;
};

static const uint kNoDebugCandidate = 0xFFFFFFFFu;

// A model buffer starts with byte offsets and counts of each primitive kind's records (six words,
// padded to 32 bytes), followed by the records.
static const uint kModelHeaderBytes = 32;
static const uint kModelTrianglesOffset = 0;
static const uint kModelTriangleCount = 4;
static const uint kModelHullsOffset = 8;
static const uint kModelHullCount = 12;
static const uint kModelCapsulesOffset = 16;
static const uint kModelCapsuleCount = 20;

// float3 a, b, c; radius.
static const uint kTriangleStride = 40;
// planeOffset, planeCount, faceOffset, faceCount, radius, three words of padding. The planes
// (float4 each, 16-byte aligned) and faces (float3 a, b, c) follow all the records.
static const uint kHullStride = 32;
static const uint kHullPlaneStride = 16;
static const uint kHullFaceStride = 36;
// float3 p0, p1; radius; padding.
static const uint kCapsuleStride = 32;

// Debug capture (FASTERNGIO_DEBUG_CANDIDATE): a 16-byte header whose first word counts the
// records, then up to kDebugMaxRecords records of uint4 (stage, instance, primitive, result) and
// five float4s.
static const uint kDebugHeaderBytes = 16;
static const uint kDebugRecordBytes = 96;
static const uint kDebugMaxRecords = 255;
static const uint kDebugBufferBytes = kDebugHeaderBytes + kDebugMaxRecords * kDebugRecordBytes;

#endif
