#include <directx/d3d12.h>

// The vcpkg baseline pins matching directx-headers and directx12-agility packages. Use the
// header's SDK version so an upgrade cannot silently leave an old literal in the executable.
extern "C"
{
	__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
	__declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";
}
