#pragma once

namespace FasterNGIO::Gpu
{
	enum class GpuApi
	{
		D3D12,
		Vulkan
	};

	// D3D12 on Windows; Vulkan everywhere else (native Linux, no Proton).
	[[nodiscard]] constexpr GpuApi DefaultGpuApi()
	{
#if defined(_WIN32)
		return GpuApi::D3D12;
#else
		return GpuApi::Vulkan;
#endif
	}

	[[nodiscard]] constexpr const char* GpuApiName(GpuApi a_api)
	{
		return a_api == GpuApi::D3D12 ? "D3D12" : "Vulkan";
	}
}
