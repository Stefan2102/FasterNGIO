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
	// kPassVolume or kPassCliff.
	uint pass;
	// The roles beyond kRoleOrdinary present in the world (0: plain volume queries).
	uint roles;
	// StructuredBuffer<uint2> per TLAS instance (see kInstanceSteep).
	uint instanceInfo;
	// Where the cliff pass's hit groups start in the hit-group table.
	uint cliffHitGroupOffset;
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

// Instance masks: what an instance is to NGIO's settings (Rejection::InstanceRole).
static const uint kRoleOrdinary = 1;
static const uint kRoleCliff = 2;
static const uint kRolePartIgnored = 4;

static const uint kNoInstance = 0xFFFFFFFFu;

// NGIO's grass cliffs (RaycastHelper::CreateGrassCliff): a ray up from the blade, then four
// vertical rays beside the cliff point, each within a height window around it.
static const float kCliffRayLength = 600.0f;
static const float kCliffWindow = 30.0f;
static const float kCliffSteepWindow = 80.0f;
// The neighbours are this far beyond the grass bounds' half-width, or kCliffNeighbourDefault
// away when the grass has no bounds.
static const float kCliffNeighbourMargin = 40.0f;
static const float kCliffNeighbourDefault = 80.0f;
// A neighbour that is not a cliff must be within this height of the cliff point.
static const float kCliffNeighbourTolerance = 20.0f;

// Which ray-generation shader a dispatch runs (RootConstants::pass).
static const uint kPassVolume = 0;
static const uint kPassCliff = 1;

// A volume query's result when the world has roles (RootConstants::roles != 0): the roles touched
// (bits), then the first part-ignored instance's index in Rejection::WorldIndex, or kNoInstance.
// Otherwise one hit flag.
static const uint kVolumeRoleWords = 2;

// A cliff query's result: the up ray's nearest instance, the t of its highest cliff hit (negative:
// none) and that hit's world normal (facing down the ray); then, for the +x, -x, +y, -y neighbour
// rays, the nearest instance and the t of the highest hit. Instances are WorldIndex indices.
static const uint kCliffWordUpClosest = 0;
static const uint kCliffWordCliffT = 1;
static const uint kCliffWordNormal = 2;
static const uint kCliffWordNeighbours = 6;
// Then scratch the shader uses while tracing (GrassRejection.hlsl).
static const uint kCliffWordScratch = 16;
static const uint kCliffResultWords = 24;

// Per TLAS instance (RootConstants::instanceInfo): its WorldIndex index, and its role bits with
// kInstanceSteep.
static const uint kInstanceSteep = 0x100;

// Debug capture (FASTERNGIO_DEBUG_CANDIDATE): a 16-byte header whose first word counts the
// records, then up to kDebugMaxRecords records of uint4 (stage, instance, primitive, result) and
// five float4s.
static const uint kDebugHeaderBytes = 16;
static const uint kDebugRecordBytes = 96;
static const uint kDebugMaxRecords = 255;
static const uint kDebugBufferBytes = kDebugHeaderBytes + kDebugMaxRecords * kDebugRecordBytes;

#endif
