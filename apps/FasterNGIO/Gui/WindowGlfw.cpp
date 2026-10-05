#include "Gui/Window.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// The OpenGL entry points come from ImGui's own loader; GLFW only needs to leave them alone.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern char** environ;

namespace FasterNGIO::Gui
{
	namespace
	{
		[[nodiscard]] bool OnPath(const char* a_program)
		{
			const char* path = std::getenv("PATH");
			if (!path) {
				return false;
			}
			std::string_view remaining(path);
			while (!remaining.empty()) {
				const auto colon = remaining.find(':');
				const auto directory = remaining.substr(0, colon);
				remaining = colon == std::string_view::npos ? std::string_view{} : remaining.substr(colon + 1);
				if (!directory.empty() && access((std::string(directory) + "/" + a_program).c_str(), X_OK) == 0) {
					return true;
				}
			}
			return false;
		}

		// Runs a program and returns what it printed, or nullopt when it failed (cancelled).
		[[nodiscard]] std::optional<std::string> Capture(const std::vector<std::string>& a_arguments)
		{
			int pipeFds[2];
			if (pipe(pipeFds) != 0) {
				return std::nullopt;
			}
			posix_spawn_file_actions_t actions;
			posix_spawn_file_actions_init(&actions);
			posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
			posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
			std::vector<char*> argv;
			for (const auto& argument : a_arguments) {
				argv.push_back(const_cast<char*>(argument.c_str()));
			}
			argv.push_back(nullptr);
			pid_t child = 0;
			const int spawned = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
			posix_spawn_file_actions_destroy(&actions);
			close(pipeFds[1]);
			if (spawned != 0) {
				close(pipeFds[0]);
				return std::nullopt;
			}
			std::string output;
			std::array<char, 4096> buffer{};
			for (ssize_t read; (read = ::read(pipeFds[0], buffer.data(), buffer.size())) > 0;) {
				output.append(buffer.data(), static_cast<std::size_t>(read));
			}
			close(pipeFds[0]);
			int status = 0;
			waitpid(child, &status, 0);
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				return std::nullopt;
			}
			while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
				output.pop_back();
			}
			return output;
		}

		void ErrorCallback(int, const char* a_description)
		{
			std::fprintf(stderr, "GLFW: %s\n", a_description);
		}
	}

	void RunWindow(const char* a_title, int a_width, int a_height, const WindowCallbacks& a_callbacks)
	{
		glfwSetErrorCallback(ErrorCallback);
		if (!glfwInit()) {
			throw std::runtime_error("no display to open the launcher window on (is DISPLAY or WAYLAND_DISPLAY set?)");
		}
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
		GLFWwindow* window = glfwCreateWindow(a_width, a_height, a_title, nullptr, nullptr);
		if (!window) {
			glfwTerminate();
			throw std::runtime_error("could not create the launcher window (OpenGL 3.0 is required)");
		}
		glfwMakeContextCurrent(window);
		glfwSwapInterval(1);

		float scaleX = 1.0f;
		float scaleY = 1.0f;
		glfwGetWindowContentScale(window, &scaleX, &scaleY);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui_ImplGlfw_InitForOpenGL(window, true);
		ImGui_ImplOpenGL3_Init("#version 130");
		a_callbacks.setup(scaleY);

		while (true) {
			glfwPollEvents();
			if (glfwWindowShouldClose(window)) {
				glfwSetWindowShouldClose(window, GLFW_FALSE);
				if (a_callbacks.closeRequested()) {
					break;
				}
			}
			if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
				glfwWaitEventsTimeout(0.1);
				continue;
			}
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			const bool keepOpen = a_callbacks.frame();
			ImGui::Render();
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			glfwSwapBuffers(window);
			if (!keepOpen) {
				break;
			}
		}

		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(window);
		glfwTerminate();
	}

	bool CanPickFolder()
	{
		static const bool available = OnPath("zenity") || OnPath("kdialog");
		return available;
	}

	std::optional<std::filesystem::path> PickFolder(const char* a_title, const std::filesystem::path& a_start)
	{
		std::error_code error;
		const bool hasStart = !a_start.empty() && std::filesystem::is_directory(a_start, error);
		std::optional<std::string> picked;
		if (OnPath("zenity")) {
			std::vector<std::string> arguments{ "zenity", "--file-selection", "--directory", std::string("--title=") + a_title };
			if (hasStart) {
				arguments.push_back("--filename=" + (a_start / "").string());
			}
			picked = Capture(arguments);
		} else if (OnPath("kdialog")) {
			picked = Capture({ "kdialog", "--title", a_title, "--getexistingdirectory", hasStart ? a_start.string() : std::string(".") });
		}
		if (!picked || picked->empty()) {
			return std::nullopt;
		}
		return std::filesystem::path(*picked);
	}

	void OpenFolder(const std::filesystem::path& a_folder)
	{
		const std::string folder = a_folder.string();
		char* argv[] = { const_cast<char*>("xdg-open"), const_cast<char*>(folder.c_str()), nullptr };
		pid_t child = 0;
		if (posix_spawnp(&child, "xdg-open", nullptr, nullptr, argv, environ) == 0) {
			// xdg-open hands off to the file manager and exits.
			int status = 0;
			waitpid(child, &status, 0);
		}
	}

	std::filesystem::path SystemFontPath()
	{
		for (const char* candidate : {
				 "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
				 "/usr/share/fonts/noto/NotoSans-Regular.ttf",
				 "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
				 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
				 "/usr/share/fonts/dejavu/DejaVuSans.ttf",
				 "/usr/share/fonts/TTF/DejaVuSans.ttf",
			 }) {
			std::error_code error;
			if (std::filesystem::is_regular_file(candidate, error)) {
				return candidate;
			}
		}
		return {};
	}
}
