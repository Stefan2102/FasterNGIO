#pragma once

// The minimal HLSL vocabulary the shared shader headers use, so the C++ compiles the exact same
// source as the GPU: the overlap tests (GrassQueryMath.hlsli) and the buffer layouts
// (RejectionLayout.hlsli).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FasterNGIO::Rejection::Hlsl
{
	using uint = std::uint32_t;

	struct float3
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };

		constexpr float3() = default;
		constexpr float3(float a_x, float a_y, float a_z) :
			x(a_x), y(a_y), z(a_z) {}

		friend constexpr float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		friend constexpr float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		friend constexpr float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		friend constexpr float3 operator*(float s, float3 a) { return { a.x * s, a.y * s, a.z * s }; }
	};

	struct float4
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float w{ 0.0f };
	};

	[[nodiscard]] inline constexpr float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	[[nodiscard]] inline constexpr float3 cross(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	[[nodiscard]] inline constexpr float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
	[[nodiscard]] inline float abs(float v) { return std::fabs(v); }
	[[nodiscard]] inline constexpr float min(float a, float b) { return a < b ? a : b; }
	[[nodiscard]] inline constexpr float max(float a, float b) { return a > b ? a : b; }

	// Internal linkage: the .hlsli's functions are not marked inline (HLSL has no ODR), and several
	// translation units include them.
	namespace
	{
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../shaders/Shared/GrassQueryMath.hlsli"
#include "../../shaders/Shared/RejectionLayout.hlsli"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
	}
}
