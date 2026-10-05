#pragma once

#include "Gpu/GpuApi.h"
#include "Rejection/RejectionConfig.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace FasterNGIO::Gpu
{
	// FASTERNGIO_DEBUG_CANDIDATE: the frame-global candidate index whose shader invocations are
	// recorded, when set.
	[[nodiscard]] std::optional<std::uint32_t> DebugCandidate();

	struct ShaderLibraryDesc
	{
		GpuApi api{ DefaultGpuApi() };
		Rejection::QueryMode mode{ Rejection::QueryMode::Capsule };
		// GrassRejection.hlsl and Shared/, or the precompiled .spv files.
		std::filesystem::path shaderDirectory;
		std::filesystem::path cacheDirectory;
		// The variant that records debug invocations (DEBUG_QUERIES).
		bool debugQueries{ false };
	};

	// The ray-tracing library for the API and query mode. Built with the runtime compiler, it is
	// compiled from GrassRejection.hlsl (DXIL or SPIR-V) through a disk cache; otherwise it is the
	// SPIR-V variant compiled at build time. Throws GpuUnsupportedError when this build or machine
	// cannot provide it.
	[[nodiscard]] std::vector<std::byte> LoadShaderLibrary(const ShaderLibraryDesc& a_desc);
}
