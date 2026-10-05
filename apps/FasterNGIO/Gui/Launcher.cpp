#include "Gui/Launcher.h"

#include "Generate.h"
#include "Gui/LogSink.h"
#include "Gui/Window.h"
#include "Platform/GameInstall.h"
#include "Platform/UserSettings.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace FasterNGIO::Gui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
		constexpr const char* kExampleGameFolder = "e.g. C:\\Games\\Skyrim Special Edition";
		constexpr const char* kDataGrass = "\\Data\\Grass";
#else
		constexpr const char* kExampleGameFolder = "e.g. /home/you/Games/Skyrim Special Edition";
		constexpr const char* kDataGrass = "/Data/Grass";
#endif

		[[nodiscard]] std::string Utf8(const std::filesystem::path& a_path)
		{
			const auto text = a_path.u8string();
			return std::string(reinterpret_cast<const char*>(text.data()), text.size());
		}

		[[nodiscard]] std::filesystem::path PathFromUtf8(const std::string& a_text)
		{
			return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(a_text.data()), a_text.size()));
		}

		[[nodiscard]] bool IsFile(const std::filesystem::path& a_path)
		{
			std::error_code error;
			return !a_path.empty() && std::filesystem::is_regular_file(a_path, error);
		}

		[[nodiscard]] bool IsDirectory(const std::filesystem::path& a_path)
		{
			std::error_code error;
			return !a_path.empty() && std::filesystem::is_directory(a_path, error);
		}

		// Enabled plugins are the lines plugins.txt marks with '*'; the base game's masters load
		// whether listed or not.
		[[nodiscard]] std::size_t CountEnabledPlugins(const std::filesystem::path& a_pluginsTxt)
		{
			std::ifstream file(a_pluginsTxt);
			std::size_t count = 0;
			for (std::string line; std::getline(file, line);) {
				count += !line.empty() && line.front() == '*' ? 1 : 0;
			}
			return count;
		}

		[[nodiscard]] std::string FormatSeconds(double a_seconds)
		{
			const auto total = static_cast<long long>(a_seconds + 0.5);
			return total >= 60 ? std::format("{}m {:02}s", total / 60, total % 60) : std::format("{}s", total);
		}

		// Work on its own thread whose result the UI thread polls: the result is written before
		// the state is published, so it may be read once Finished() is true.
		template <class Result>
		class BackgroundTask
		{
		public:
			template <class Function>
			explicit BackgroundTask(Function a_function) :
				_thread([this, function = std::move(a_function)](std::stop_token a_stop) {
					try {
						_result.emplace(function(a_stop));
						_state.store(State::Done, std::memory_order_release);
					} catch (const std::exception& e) {
						_error = e.what();
						_state.store(State::Failed, std::memory_order_release);
					}
				})
			{
			}

			BackgroundTask(const BackgroundTask&) = delete;
			BackgroundTask& operator=(const BackgroundTask&) = delete;

			[[nodiscard]] bool Finished() const { return _state.load(std::memory_order_acquire) != State::Running; }
			[[nodiscard]] bool Failed() const { return _state.load(std::memory_order_acquire) == State::Failed; }
			// Only once Finished().
			[[nodiscard]] const std::optional<Result>& Value() const { return _result; }
			[[nodiscard]] const std::string& Error() const { return _error; }
			void RequestStop() { _thread.request_stop(); }

		private:
			enum class State
			{
				Running,
				Done,
				Failed
			};

			std::atomic<State> _state{ State::Running };
			std::optional<Result> _result;
			std::string _error;
			// Last: it starts running once everything above exists, and joins first on destruction.
			std::jthread _thread;
		};

		// The plugins read for the worldspace list, kept for the run when its inputs still match.
		struct Scan
		{
			std::shared_ptr<const App::LoadedWorld> world;
			std::vector<App::WorldSummary> worlds;
		};

		struct ScanKey
		{
			std::filesystem::path data;
			std::filesystem::path pluginsTxt;
			bool operator==(const ScanKey&) const = default;
		};

		enum class Placement
		{
			Smooth,
			Vanilla
		};

		class Launcher
		{
		public:
			Launcher() :
				_sink(std::make_shared<QueueSink>()),
				_settingsPath(Platform::UserSettings::DefaultPath())
			{
				spdlog::default_logger()->sinks().push_back(_sink);
				LoadSettings();
			}

			~Launcher()
			{
				auto& sinks = spdlog::default_logger()->sinks();
				std::erase(sinks, std::static_pointer_cast<spdlog::sinks::sink>(_sink));
			}

			void Setup(float a_dpiScale)
			{
				auto& io = ImGui::GetIO();
				// Nothing written next to wherever the program was started from.
				io.IniFilename = nullptr;
				ImGui::StyleColorsDark();
				auto& style = ImGui::GetStyle();
				style.WindowRounding = 0.0f;
				style.FrameRounding = 4.0f;
				style.GrabRounding = 4.0f;
				style.FramePadding = ImVec2(8.0f, 5.0f);
				style.ItemSpacing = ImVec2(10.0f, 8.0f);
				style.ScaleAllSizes(a_dpiScale);
				style.FontScaleDpi = a_dpiScale;
				if (const auto font = SystemFontPath(); IsFile(font)) {
					io.Fonts->AddFontFromFileTTF(Utf8(font).c_str(), 17.0f);
				} else {
					io.Fonts->AddFontDefault();
				}
			}

			bool Frame()
			{
				DrainLog();
				UpdateInspection();
				UpdateScan();
				UpdateRun();
				Draw();
				return !(_closeAfterCancel && !_run);
			}

			bool CloseRequested()
			{
				SaveSettings();
				if (_run) {
					// Let the run stop at its next cell, then close.
					_run->RequestStop();
					_closeAfterCancel = true;
					return false;
				}
				return true;
			}

			[[nodiscard]] int ExitCode() const { return _lastExitCode; }

		private:
			// --- State derived from the inputs -------------------------------------------------

			[[nodiscard]] std::filesystem::path PluginsTxt() const
			{
				if (!_pluginsOverride.empty()) {
					return PathFromUtf8(_pluginsOverride);
				}
				return _install ? _install->pluginsTxt : std::filesystem::path{};
			}

			[[nodiscard]] std::filesystem::path OutputFolder() const
			{
				if (!_output.empty()) {
					return PathFromUtf8(_output);
				}
				return _install ? _install->data / "Grass" : std::filesystem::path{};
			}

			[[nodiscard]] std::optional<std::filesystem::path> IniFolder() const
			{
				if (!_iniOverride.empty()) {
					return PathFromUtf8(_iniOverride);
				}
				if (_install && IsDirectory(_install->iniDirectory)) {
					return _install->iniDirectory;
				}
				// The usual search (My Games), which finds nothing worse than engine defaults.
				return std::nullopt;
			}

			[[nodiscard]] bool PluginsTxtFound() const { return IsFile(PluginsTxt()); }

			[[nodiscard]] std::optional<ScanKey> CurrentScanKey() const
			{
				if (!_install || !_pluginsFound) {
					return std::nullopt;
				}
				return ScanKey{ _install->data, PluginsTxt() };
			}

			// Re-validates the game folder when an input changed; only filesystem checks.
			void UpdateInspection()
			{
				const auto signature = _game + '\n' + _pluginsOverride;
				if (signature == _inspectedSignature) {
					// plugins.txt appears once the game or a mod manager has run; look again now and then.
					if (_install && !_pluginsFound && Clock::now() - _lastPluginsCheck > std::chrono::seconds(2)) {
						_lastPluginsCheck = Clock::now();
						_pluginsFound = PluginsTxtFound();
						_enabledPlugins = _pluginsFound ? CountEnabledPlugins(PluginsTxt()) : 0;
					}
					return;
				}
				_inspectedSignature = signature;
				_lastEdit = Clock::now();
				_install.reset();
				_installError.clear();
				if (_game.empty()) {
					return;
				}
				auto inspected = Platform::InspectGameFolder(PathFromUtf8(_game));
				if (!inspected) {
					_installError = inspected.error();
					return;
				}
				_install = std::move(*inspected);
				_pluginsFound = PluginsTxtFound();
				_enabledPlugins = _pluginsFound ? CountEnabledPlugins(PluginsTxt()) : 0;
			}

			// Reads the plugins in the background for the worldspace list once the inputs have
			// settled; the run reuses the result.
			void UpdateScan()
			{
				std::erase_if(_retiredScans, [](const auto& a_task) { return a_task->Finished(); });
				if (_scan && _scan->Failed() && !_scanErrorLogged) {
					spdlog::error("could not read the plugins: {}", _scan->Error());
					_scanErrorLogged = true;
				}
				const auto key = CurrentScanKey();
				if (_scan && (!key || *key != _scanKey)) {
					_scan->RequestStop();
					_retiredScans.push_back(std::move(_scan));
				}
				if (_scan || !key || _run || Clock::now() - _lastEdit < std::chrono::milliseconds(400)) {
					return;
				}
				_scanKey = *key;
				_scanErrorLogged = false;
				App::CliOptions options;
				options.dataPath = key->data;
				options.pluginsTxtPath = key->pluginsTxt;
				_scan = std::make_unique<BackgroundTask<Scan>>([options](std::stop_token) {
					auto world = std::make_shared<App::LoadedWorld>(App::LoadStaticSnapshot(options));
					auto worlds = App::ListWorlds(world->snapshot);
					return Scan{ std::move(world), std::move(worlds) };
				});
			}

			[[nodiscard]] const Scan* FinishedScan() const
			{
				if (_scan && _scan->Finished() && !_scan->Failed()) {
					return std::addressof(*_scan->Value());
				}
				return nullptr;
			}

			[[nodiscard]] std::string WorldName(std::uint32_t a_formID) const
			{
				if (const auto* scan = FinishedScan()) {
					for (const auto& world : scan->worlds) {
						if (world.formID.value == a_formID) {
							return world.editorID;
						}
					}
				}
				return std::format("{:08X}", a_formID);
			}

			// --- Running ------------------------------------------------------------------------

			[[nodiscard]] bool CanGenerate() const { return !_run && _install && _pluginsFound && !OutputFolder().empty(); }

			void StartRun()
			{
				SaveSettings();
				App::CliOptions options;
				options.dataPath = _install->data;
				options.pluginsTxtPath = PluginsTxt();
				options.outputDirectory = OutputFolder();
				options.allWorlds = !_world.has_value();
				if (_world) {
					options.worldFormID = GameData::FormID{ *_world };
				}
				options.placement.mode = _placement == Placement::Vanilla ? Grass::PlacementMode::Vanilla : Grass::PlacementMode::Smooth;
				options.rejection = _rejection;
				options.overwrite = _overwrite;
				options.gameIniDirectory = IniFolder();

				std::shared_ptr<const App::LoadedWorld> preloaded;
				if (const auto* scan = FinishedScan(); scan && CurrentScanKey() == _scanKey) {
					preloaded = scan->world;
				}
				_progress = std::make_unique<App::RunProgress>();
				_runStarted = Clock::now();
				_summary.clear();
				_summaryIsError = false;
				_ranOutput = options.outputDirectory;
				spdlog::info("generating into {}", options.outputDirectory.string());
				_run = std::make_unique<BackgroundTask<App::RunResult>>(
					[options, preloaded, progress = _progress.get()](std::stop_token a_stop) {
						return App::Run(options, App::RunControl{ .stop = a_stop, .progress = progress, .preloaded = preloaded.get() });
					});
			}

			void UpdateRun()
			{
				if (!_run || !_run->Finished()) {
					return;
				}
				const auto elapsed = FormatSeconds(std::chrono::duration<double>(Clock::now() - _runStarted).count());
				if (_run->Failed()) {
					_summary = "Failed: " + _run->Error();
					_summaryIsError = true;
					_lastExitCode = 1;
					spdlog::error("{}", _run->Error());
				} else {
					const auto& result = *_run->Value();
					_lastExitCode = result.exitCode;
					if (result.cancelled) {
						_summary = std::format("Cancelled after {}: wrote {} cache file(s).", elapsed, result.cellsWritten);
						_summaryIsError = true;
					} else {
						_summary = std::format("Done in {}: wrote {} cache file(s) for {} worldspace(s){}{}.", elapsed, result.cellsWritten, result.worlds,
							result.cellsSkipped ? std::format(", kept {} existing", result.cellsSkipped) : std::string{},
							result.cellsFailed ? std::format(", {} failed (see the log)", result.cellsFailed) : std::string{});
						_summaryIsError = result.cellsFailed != 0;
					}
				}
				_run.reset();
			}

			// --- Settings -----------------------------------------------------------------------

			void LoadSettings()
			{
				const auto settings = Platform::UserSettings::Load(_settingsPath);
				_game = settings.Get("game_folder").value_or("");
				_output = settings.Get("output_folder").value_or("");
				_pluginsOverride = settings.Get("plugins_txt").value_or("");
				_iniOverride = settings.Get("game_ini_folder").value_or("");
				_placement = settings.Get("placement") == "vanilla" ? Placement::Vanilla : Placement::Smooth;
				const auto rejection = settings.Get("rejection").value_or("auto");
				_rejection = rejection == "cpu" ? App::RejectChoice::Cpu : rejection == "none" ? App::RejectChoice::None : App::RejectChoice::Auto;
#if FASTERNGIO_HAS_GPU
				if (rejection == "gpu") {
					_rejection = App::RejectChoice::Gpu;
				}
#endif
				_overwrite = settings.Get("overwrite") == "1";
				if (const auto world = settings.Get("world"); world && *world != "all") {
					std::uint32_t value = 0;
					if (std::from_chars(world->data(), world->data() + world->size(), value, 16).ec == std::errc{}) {
						_world = value;
					}
				}
				_showAdvanced = !_pluginsOverride.empty() || !_iniOverride.empty();
			}

			void SaveSettings() const
			{
				Platform::UserSettings settings;
				settings.Set("game_folder", _game);
				settings.Set("output_folder", _output);
				settings.Set("plugins_txt", _pluginsOverride);
				settings.Set("game_ini_folder", _iniOverride);
				settings.Set("placement", _placement == Placement::Vanilla ? "vanilla" : "smooth");
				settings.Set("rejection", _rejection == App::RejectChoice::Cpu    ? "cpu"
				                          : _rejection == App::RejectChoice::None ? "none"
				                          : _rejection == App::RejectChoice::Gpu  ? "gpu"
				                                                                  : "auto");
				settings.Set("overwrite", _overwrite ? "1" : "0");
				settings.Set("world", _world ? std::format("{:08X}", *_world) : std::string("all"));
				settings.Save(_settingsPath);
			}

			// --- Log ----------------------------------------------------------------------------

			void DrainLog()
			{
				constexpr std::size_t kMaxLines = 4000;
				while (auto line = _sink->TryPop()) {
					_log.push_back(std::move(*line));
					_scrollToBottom = true;
				}
				while (_log.size() > kMaxLines) {
					_log.pop_front();
				}
			}

			// --- Drawing ------------------------------------------------------------------------

			// A text field with a Browse button that opens the folder chooser.
			bool FolderField(const char* a_id, std::string& a_text, const char* a_hint, const char* a_pickerTitle)
			{
				const auto& style = ImGui::GetStyle();
				const float buttonWidth = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - style.ItemSpacing.x);
				bool changed = ImGui::InputTextWithHint(a_id, a_hint, &a_text);
				ImGui::SameLine();
				ImGui::PushID(a_id);
				ImGui::BeginDisabled(!CanPickFolder());
				if (ImGui::Button("Browse...")) {
					const auto start = a_text.empty() ? std::filesystem::path{} : PathFromUtf8(a_text);
					if (const auto picked = PickFolder(a_pickerTitle, start)) {
						a_text = Utf8(*picked);
						changed = true;
					}
				}
				ImGui::EndDisabled();
				ImGui::PopID();
				return changed;
			}

			static void StatusLine(bool a_ok, const std::string& a_text)
			{
				const ImVec4 color = a_ok ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.55f, 0.4f, 1.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextWrapped("%s %s", a_ok ? "OK:" : "!", a_text.c_str());
				ImGui::PopStyleColor();
			}

			void DrawGame()
			{
				ImGui::SeparatorText("Skyrim Special Edition");
				ImGui::TextUnformatted("Game folder (the one with SkyrimSE.exe):");
				FolderField("##game", _game, kExampleGameFolder, "Choose the Skyrim Special Edition folder");
				if (_game.empty()) {
					ImGui::TextDisabled("Choose the folder Skyrim Special Edition is installed in.");
				} else if (!_install) {
					StatusLine(false, _installError);
				} else if (!_pluginsFound) {
					StatusLine(false, std::format("{} ({}), but its load order was not found at {}. Start the game (or your mod manager) once, "
												  "or choose plugins.txt under Advanced.",
						Utf8(_install->root), Platform::GameStoreName(_install->store), Utf8(PluginsTxt())));
				} else {
					StatusLine(true, std::format("{} version. Load order: {} enabled plugin(s) from {}", Platform::GameStoreName(_install->store), _enabledPlugins,
										 Utf8(PluginsTxt())));
				}
			}

			void DrawOutput()
			{
				ImGui::SeparatorText("Output");
				const auto defaultOutput = _install ? Utf8(_install->data / "Grass") : std::string("<game folder>") + kDataGrass;
				FolderField("##output", _output, defaultOutput.c_str(), "Choose where to write the grass cache");
				ImGui::TextDisabled("Leave empty for %s, where NGIO's own pregeneration writes and where NGIO and DynDOLOD read the cache.", kDataGrass + 1);
			}

			void DrawOptions()
			{
				ImGui::SeparatorText("Options");
				const float labelWidth = ImGui::CalcTextSize("Grass in objects").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
				const float fieldWidth = (std::min)(ImGui::GetContentRegionAvail().x - labelWidth, 480.0f * ImGui::GetStyle().FontScaleDpi);

				// Worldspaces.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Worldspaces");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				const auto* scan = FinishedScan();
				std::string preview = "All worldspaces";
				if (_world) {
					preview = WorldName(*_world);
				}
				if (ImGui::BeginCombo("##world", preview.c_str())) {
					if (ImGui::Selectable("All worldspaces", !_world)) {
						_world.reset();
					}
					if (scan) {
						for (const auto& world : scan->worlds) {
							const auto label = std::format("{}  ({} cells)##{:08X}", world.editorID, world.cells, world.formID.value);
							if (ImGui::Selectable(label.c_str(), _world == world.formID.value)) {
								_world = world.formID.value;
							}
						}
					} else if (_scan && _scan->Failed()) {
						ImGui::TextDisabled("Could not read the plugins: %s", _scan->Error().c_str());
					} else if (_scan) {
						ImGui::TextDisabled("Reading the plugins...");
					} else {
						ImGui::TextDisabled("Choose the game folder to list its worldspaces.");
					}
					ImGui::EndCombo();
				}

				// Placement.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Placement");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				constexpr const char* kPlacements[] = { "Smooth (follows the painted terrain)", "Vanilla (identical to the game's)" };
				int placement = static_cast<int>(_placement);
				if (ImGui::Combo("##placement", &placement, kPlacements, IM_ARRAYSIZE(kPlacements))) {
					_placement = static_cast<Placement>(placement);
				}

				// Rejection.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Grass in objects");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				struct Choice
				{
					App::RejectChoice value;
					const char* label;
				};
				static constexpr Choice kChoices[] = {
#if FASTERNGIO_HAS_GPU
					{ App::RejectChoice::Auto, "Remove: graphics card if supported, else processor" },
					{ App::RejectChoice::Gpu, "Remove: graphics card only" },
#else
					{ App::RejectChoice::Auto, "Remove (processor)" },
#endif
					{ App::RejectChoice::Cpu, "Remove: processor only" },
					{ App::RejectChoice::None, "Keep (no rejection, like vanilla)" },
				};
				const char* current = kChoices[0].label;
				for (const auto& choice : kChoices) {
					if (choice.value == _rejection) {
						current = choice.label;
					}
				}
				if (ImGui::BeginCombo("##rejection", current)) {
					for (const auto& choice : kChoices) {
						if (ImGui::Selectable(choice.label, choice.value == _rejection)) {
							_rejection = choice.value;
						}
					}
					ImGui::EndCombo();
				}

				ImGui::Checkbox("Rebuild cache files that already exist", &_overwrite);

				ImGui::SetNextItemOpen(_showAdvanced, ImGuiCond_Once);
				if (ImGui::TreeNode("Advanced")) {
					ImGui::TextUnformatted("Load order (plugins.txt):");
					const auto pluginsHint = _install ? Utf8(_install->pluginsTxt) : std::string("derived from the game folder");
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputTextWithHint("##plugins", pluginsHint.c_str(), &_pluginsOverride);
					ImGui::TextUnformatted("Game INI folder (Skyrim.ini's [Grass] settings):");
					const auto iniHint = _install ? Utf8(_install->iniDirectory) : std::string("derived from the game folder");
					FolderField("##ini", _iniOverride, iniHint.c_str(), "Choose the folder with Skyrim.ini");
					ImGui::TreePop();
				}
			}

			void DrawRun()
			{
				ImGui::Spacing();
				const auto buttonSize = ImVec2(160.0f * ImGui::GetStyle().FontScaleDpi, 0.0f);
				if (_run) {
					ImGui::BeginDisabled(_run && _closeAfterCancel);
					if (ImGui::Button(_closeAfterCancel ? "Cancelling..." : "Cancel", buttonSize)) {
						_run->RequestStop();
					}
					ImGui::EndDisabled();
				} else {
					ImGui::BeginDisabled(!CanGenerate());
					if (ImGui::Button("Generate", buttonSize)) {
						StartRun();
					}
					ImGui::EndDisabled();
				}
				// Beside the button: the progress, the last run's outcome, or why it is disabled.
				if ((_run && _progress) || !_summary.empty() || !CanGenerate()) {
					ImGui::SameLine();
				}

				if (_run && _progress) {
					const auto stage = _progress->stage.load(std::memory_order_relaxed);
					const auto worldCount = _progress->worldCount.load(std::memory_order_relaxed);
					const auto worldIndex = _progress->worldIndex.load(std::memory_order_relaxed);
					const auto done = _progress->cellsDone.load(std::memory_order_relaxed);
					const auto total = _progress->cellsTotal.load(std::memory_order_relaxed);
					std::string label;
					float fraction = 0.0f;
					switch (stage) {
					case App::RunStage::LoadingPlugins:
						label = "Reading plugins...";
						break;
					case App::RunStage::Preparing:
						label = std::format("{}/{} {}: preparing collision...", worldIndex + 1, worldCount, WorldName(_progress->worldFormID.load()));
						break;
					case App::RunStage::Writing:
						label = "Writing the last files...";
						break;
					case App::RunStage::Generating:
					case App::RunStage::Finished:
						label = std::format("{}/{} {}: {}/{} cells", worldIndex + 1, worldCount, WorldName(_progress->worldFormID.load()), done, total);
						break;
					}
					if (worldCount != 0) {
						const float worldFraction = total != 0 && stage >= App::RunStage::Generating ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
						fraction = (static_cast<float>(worldIndex) + worldFraction) / static_cast<float>(worldCount);
					}
					ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), label.c_str());
				} else if (!_summary.empty()) {
					ImGui::AlignTextToFramePadding();
					StatusLine(!_summaryIsError, _summary);
				} else if (!CanGenerate()) {
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("%s", _install ? "The load order (plugins.txt) is needed to start." : "Choose a valid game folder to start.");
				}
				if (!_run && !_summary.empty() && IsDirectory(_ranOutput)) {
					if (ImGui::Button("Open output folder", buttonSize)) {
						OpenFolder(_ranOutput);
					}
				}
			}

			void DrawLog()
			{
				ImGui::SeparatorText("Log");
				if (ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
					const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(_log.size()));
					while (clipper.Step()) {
						for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
							const auto& line = _log[static_cast<std::size_t>(i)];
							const bool colored = line.level >= spdlog::level::warn;
							if (colored) {
								ImGui::PushStyleColor(ImGuiCol_Text, line.level >= spdlog::level::err ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
							}
							ImGui::TextUnformatted(line.text.c_str(), line.text.c_str() + line.text.size());
							if (colored) {
								ImGui::PopStyleColor();
							}
						}
					}
					if (_scrollToBottom && atBottom) {
						ImGui::SetScrollHereY(1.0f);
					}
					_scrollToBottom = false;
				}
				ImGui::EndChild();
			}

			void Draw()
			{
				const auto* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
				ImGui::Begin("FasterNGIO", nullptr, flags);
				ImGui::TextDisabled("Generates the grass cache No Grass In Objects and DynDOLOD use, without starting the game.");
				ImGui::BeginDisabled(_run != nullptr);
				DrawGame();
				DrawOutput();
				DrawOptions();
				ImGui::EndDisabled();
				DrawRun();
				DrawLog();
				ImGui::End();
			}

			std::shared_ptr<QueueSink> _sink;
			std::filesystem::path _settingsPath;

			// Inputs (UTF-8).
			std::string _game;
			std::string _output;
			std::string _pluginsOverride;
			std::string _iniOverride;
			std::optional<std::uint32_t> _world;
			Placement _placement{ Placement::Smooth };
			App::RejectChoice _rejection{ App::RejectChoice::Auto };
			bool _overwrite{ false };
			bool _showAdvanced{ false };

			// Derived from the inputs.
			std::string _inspectedSignature{ "\x01" };
			Clock::time_point _lastEdit{};
			std::optional<Platform::GameInstall> _install;
			std::string _installError;
			bool _pluginsFound{ false };
			Clock::time_point _lastPluginsCheck{};
			std::size_t _enabledPlugins{ 0 };

			std::unique_ptr<BackgroundTask<Scan>> _scan;
			ScanKey _scanKey;
			bool _scanErrorLogged{ false };
			std::vector<std::unique_ptr<BackgroundTask<Scan>>> _retiredScans;

			std::unique_ptr<BackgroundTask<App::RunResult>> _run;
			std::unique_ptr<App::RunProgress> _progress;
			Clock::time_point _runStarted{};
			std::filesystem::path _ranOutput;
			std::string _summary;
			bool _summaryIsError{ false };
			bool _closeAfterCancel{ false };
			int _lastExitCode{ 0 };

			std::deque<LogLine> _log;
			bool _scrollToBottom{ false };
		};

		// A double-clicked console program gets a console of its own; close it so only the window
		// shows. A console shared with a shell (run from a prompt) stays.
		void ReleaseOwnConsole()
		{
#if defined(_WIN32)
			DWORD processes[2]{};
			if (GetConsoleProcessList(processes, 2) == 1) {
				FreeConsole();
			}
#endif
		}
	}

	int RunLauncher()
	{
		ReleaseOwnConsole();
		try {
			Launcher launcher;
			WindowCallbacks callbacks;
			callbacks.setup = [&](float a_dpiScale) { launcher.Setup(a_dpiScale); };
			callbacks.frame = [&] { return launcher.Frame(); };
			callbacks.closeRequested = [&] { return launcher.CloseRequested(); };
			RunWindow("FasterNGIO", 960, 820, callbacks);
			return launcher.ExitCode();
		} catch (const std::exception& e) {
			spdlog::error("{}", e.what());
			spdlog::info("run FasterNGIO --help for the command-line options");
			return 1;
		}
	}
}
