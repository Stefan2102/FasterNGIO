#include "Gpu/GpuShaders.h"

#include "Gpu/GpuRejector.h"

#if FASTERNGIO_RUNTIME_SHADER_COMPILER
#include <ORGModuleServices/ShaderCompiler.h>
#endif

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>

namespace FasterNGIO::Gpu
{
	namespace
	{
		// The whole file, or nullopt when it cannot be opened.
		[[nodiscard]] std::optional<std::vector<char>> ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}
			return std::vector<char>{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		}
	}

	std::optional<std::uint32_t> DebugCandidate()
	{
		const char* value = std::getenv("FASTERNGIO_DEBUG_CANDIDATE");
		if (!value) {
			return std::nullopt;
		}
		return static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
	}

	std::vector<std::byte> LoadShaderLibrary(const ShaderLibraryDesc& a_desc)
	{
#if FASTERNGIO_RUNTIME_SHADER_COMPILER
		const auto shaderPath = a_desc.shaderDirectory / "GrassRejection.hlsl";
		const auto source = ReadFile(shaderPath);
		if (!source) {
			throw std::runtime_error("GPU: cannot read " + shaderPath.string());
		}
		org::services::ShaderCompiler compiler(a_desc.cacheDirectory);
		if (!compiler.Available()) {
			throw GpuUnsupportedError("the DXC shader compiler (dxcompiler) is not available");
		}
		org::services::ShaderCompileRequest request{};
		request.sourceName = shaderPath.string();
		request.source = std::as_bytes(std::span(*source));
		request.target = L"lib_6_6";
		request.format = a_desc.api == GpuApi::Vulkan ? org::services::ShaderBinaryFormat::Spirv : org::services::ShaderBinaryFormat::Dxil;
		request.includeDirectories = { a_desc.shaderDirectory };
		// Everything GrassRejection.hlsl includes, so the cache notices when one changes.
		request.dependencyFiles = { a_desc.shaderDirectory / "Shared" / "GrassQueryMath.hlsli", a_desc.shaderDirectory / "Shared" / "RejectionLayout.hlsli" };
		request.defines = {
			{ L"QUERY_RAY", a_desc.mode == Rejection::QueryMode::Ray ? L"1" : L"0" },
			{ L"DEBUG_QUERIES", a_desc.debugQueries ? L"1" : L"0" },
		};
		const auto artifact = compiler.Compile(std::move(request));
		if (!artifact) {
			throw std::runtime_error("GPU: shader compilation failed:\n" + artifact.diagnostics);
		}
		return artifact.binary;
#else
		// One variant per QUERY_RAY and DEBUG_QUERIES value (apps/FasterNGIO/CMakeLists.txt).
		if (a_desc.api != GpuApi::Vulkan) {
			throw GpuUnsupportedError("this build has only precompiled SPIR-V shaders, which need Vulkan");
		}
		std::string name = a_desc.mode == Rejection::QueryMode::Ray ? "GrassRejection.ray" : "GrassRejection.shape";
		if (a_desc.debugQueries) {
			name += ".debug";
		}
		const auto shaderPath = a_desc.shaderDirectory / (name + ".spv");
		const auto binary = ReadFile(shaderPath);
		if (!binary) {
			throw GpuUnsupportedError("missing precompiled shader " + shaderPath.string());
		}
		if (binary->empty() || binary->size() % 4 != 0) {
			throw std::runtime_error("GPU: " + shaderPath.string() + " is not SPIR-V");
		}
		const auto bytes = std::as_bytes(std::span(*binary));
		return { bytes.begin(), bytes.end() };
#endif
	}
}
