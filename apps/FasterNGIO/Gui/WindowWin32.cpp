#include "Gui/Window.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <Windows.h>
#include <d3d11.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam);

namespace FasterNGIO::Gui
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		struct D3D
		{
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11DeviceContext> context;
			ComPtr<IDXGISwapChain> swapChain;
			ComPtr<ID3D11RenderTargetView> target;
			UINT resizeWidth{ 0 };
			UINT resizeHeight{ 0 };
			bool closeRequested{ false };
		};

		D3D* g_d3d = nullptr;

		// Runs a function when the scope ends, however it ends.
		template <class Function>
		class ScopeExit
		{
		public:
			explicit ScopeExit(Function a_function) :
				_function(std::move(a_function)) {}
			ScopeExit(const ScopeExit&) = delete;
			ScopeExit& operator=(const ScopeExit&) = delete;
			~ScopeExit() { _function(); }

		private:
			Function _function;
		};

		// A double-clicked console program gets a console of its own; close it so only the window
		// shows. A console shared with a shell (run from a prompt) stays.
		void ReleaseOwnConsole()
		{
			DWORD processes[2]{};
			if (GetConsoleProcessList(processes, 2) == 1) {
				FreeConsole();
			}
		}

		void CreateTarget(D3D& a_d3d)
		{
			ComPtr<ID3D11Texture2D> backBuffer;
			a_d3d.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
			a_d3d.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &a_d3d.target);
		}

		void CreateDevice(HWND a_window, D3D& a_d3d)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			desc.BufferCount = 2;
			desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BufferDesc.RefreshRate.Numerator = 60;
			desc.BufferDesc.RefreshRate.Denominator = 1;
			desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.OutputWindow = a_window;
			desc.SampleDesc.Count = 1;
			desc.Windowed = TRUE;
			desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

			constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
			D3D_FEATURE_LEVEL level{};
			auto result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &a_d3d.swapChain,
				&a_d3d.device, &level, &a_d3d.context);
			if (result == DXGI_ERROR_UNSUPPORTED) {
				// No hardware device (remote desktop, basic display driver): the software rasterizer.
				result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &a_d3d.swapChain,
					&a_d3d.device, &level, &a_d3d.context);
			}
			if (FAILED(result)) {
				throw std::runtime_error("could not create a Direct3D 11 device for the window");
			}
			CreateTarget(a_d3d);
		}

		LRESULT WINAPI WindowProc(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam)
		{
			if (ImGui_ImplWin32_WndProcHandler(a_window, a_message, a_wParam, a_lParam)) {
				return TRUE;
			}
			switch (a_message) {
			case WM_SIZE:
				if (g_d3d && a_wParam != SIZE_MINIMIZED) {
					g_d3d->resizeWidth = LOWORD(a_lParam);
					g_d3d->resizeHeight = HIWORD(a_lParam);
				}
				return 0;
			case WM_SYSCOMMAND:
				// No ALT application menu.
				if ((a_wParam & 0xfff0) == SC_KEYMENU) {
					return 0;
				}
				break;
			case WM_CLOSE:
				if (g_d3d) {
					g_d3d->closeRequested = true;
				}
				return 0;
			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;
			default:
				break;
			}
			return DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
		}

		[[nodiscard]] std::wstring Wide(const char* a_text)
		{
			const int length = MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
			std::wstring result(static_cast<std::size_t>((std::max)(length, 1)), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, a_text, -1, result.data(), length);
			result.resize(static_cast<std::size_t>((std::max)(length - 1, 0)));
			return result;
		}
	}

	void RunWindow(const char* a_title, int a_width, int a_height, const WindowCallbacks& a_callbacks)
	{
		ImGui_ImplWin32_EnableDpiAwareness();
		const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const ScopeExit comScope([com] {
			if (SUCCEEDED(com)) {
				CoUninitialize();
			}
		});

		const auto instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW windowClass{ sizeof(windowClass), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
			L"FasterNGIO", nullptr };
		windowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
		RegisterClassExW(&windowClass);
		const ScopeExit classScope([&] { UnregisterClassW(windowClass.lpszClassName, instance); });

		const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
		const auto title = Wide(a_title);
		HWND window = CreateWindowW(windowClass.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(a_width * scale),
			static_cast<int>(a_height * scale), nullptr, nullptr, instance, nullptr);
		if (!window) {
			throw std::runtime_error("could not create the launcher window");
		}
		const ScopeExit windowScope([window] { DestroyWindow(window); });

		D3D d3d;
		CreateDevice(window, d3d);
		g_d3d = &d3d;
		const ScopeExit d3dScope([] { g_d3d = nullptr; });
		ShowWindow(window, SW_SHOWDEFAULT);
		UpdateWindow(window);
		// The window is up: errors from here on show in it, so a console of its own can go.
		ReleaseOwnConsole();

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui_ImplWin32_Init(window);
		ImGui_ImplDX11_Init(d3d.device.Get(), d3d.context.Get());
		const ScopeExit imguiScope([] {
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		});
		a_callbacks.setup(ImGui_ImplWin32_GetDpiScaleForHwnd(window));

		bool running = true;
		while (running) {
			MSG message;
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&message);
				DispatchMessageW(&message);
				if (message.message == WM_QUIT) {
					running = false;
				}
			}
			if (!running) {
				break;
			}
			if (d3d.closeRequested) {
				d3d.closeRequested = false;
				if (a_callbacks.closeRequested()) {
					break;
				}
			}
			if (d3d.resizeWidth != 0 && d3d.resizeHeight != 0) {
				d3d.target.Reset();
				d3d.swapChain->ResizeBuffers(0, d3d.resizeWidth, d3d.resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
				d3d.resizeWidth = d3d.resizeHeight = 0;
				CreateTarget(d3d);
			}

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			const bool keepOpen = a_callbacks.frame();
			ImGui::Render();
			constexpr float clear[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
			d3d.context->OMSetRenderTargets(1, d3d.target.GetAddressOf(), nullptr);
			d3d.context->ClearRenderTargetView(d3d.target.Get(), clear);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			// Vsync: a launcher has no reason to spin faster than the display.
			d3d.swapChain->Present(1, 0);
			if (!keepOpen) {
				break;
			}
		}

	}

	bool CanPickFolder()
	{
		return true;
	}

	std::optional<std::filesystem::path> PickFolder(const char* a_title, const std::filesystem::path& a_start)
	{
		ComPtr<IFileOpenDialog> dialog;
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
			return std::nullopt;
		}
		DWORD options = 0;
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		const auto title = Wide(a_title);
		dialog->SetTitle(title.c_str());
		std::error_code error;
		if (!a_start.empty() && std::filesystem::is_directory(a_start, error)) {
			ComPtr<IShellItem> start;
			if (SUCCEEDED(SHCreateItemFromParsingName(a_start.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
				dialog->SetFolder(start.Get());
			}
		}
		if (FAILED(dialog->Show(GetActiveWindow()))) {
			return std::nullopt;
		}
		ComPtr<IShellItem> item;
		PWSTR path = nullptr;
		if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
			return std::nullopt;
		}
		std::filesystem::path result(path);
		CoTaskMemFree(path);
		return result;
	}

	void OpenFolder(const std::filesystem::path& a_folder)
	{
		ShellExecuteW(nullptr, L"open", a_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	std::filesystem::path SystemFontPath()
	{
		PWSTR fonts = nullptr;
		std::filesystem::path result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &fonts)) && fonts) {
			result = std::filesystem::path(fonts) / "segoeui.ttf";
		}
		CoTaskMemFree(fonts);
		return result;
	}
}
