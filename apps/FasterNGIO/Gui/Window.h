#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// What the launcher needs from the platform: a window running Dear ImGui, a folder picker and a
// way to show a folder. Win32 + D3D11 on Windows (WindowWin32.cpp), GLFW + OpenGL 3 elsewhere
// (WindowGlfw.cpp).
namespace FasterNGIO::Gui
{
	struct WindowCallbacks
	{
		// Called once the ImGui context and backends exist, with the display's scale factor
		// (1 at 96 DPI), to load fonts and size the style.
		std::function<void(float a_dpiScale)> setup;
		// Called every frame between ImGui::NewFrame and ImGui::Render; return false to close.
		std::function<bool()> frame;
		// The user asked to close the window; return false to keep it open.
		std::function<bool()> closeRequested;
	};

	// Opens the window and runs it until it closes. Throws when no window can be created (no
	// display); the message says why.
	void RunWindow(const char* a_title, int a_width, int a_height, const WindowCallbacks& a_callbacks);

	// The system's folder chooser, starting at a_start when it exists. nullopt when cancelled or
	// when no chooser is available.
	[[nodiscard]] std::optional<std::filesystem::path> PickFolder(const char* a_title, const std::filesystem::path& a_start);

	// Whether PickFolder can show anything (off Windows it needs zenity or kdialog).
	[[nodiscard]] bool CanPickFolder();

	// Shows a_folder in the file manager.
	void OpenFolder(const std::filesystem::path& a_folder);

	// A UI font from the system, or empty to use ImGui's built-in one.
	[[nodiscard]] std::filesystem::path SystemFontPath();
}
