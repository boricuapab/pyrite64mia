// In-app companion MCP server controls. MIT licensed.
#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_dialog.h>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "../context.h"
#include "../pyrite64_mcp_bridge.h"
#include "../utils/proc.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Win32's ERROR macro collides with Pyrite64's CLI and notification enums.
#undef ERROR
#else
#include <unistd.h>
#endif

namespace Editor::MCP {
namespace fs = std::filesystem;
inline SDL_Process* process = nullptr;
inline std::string projectPath, output, message, draftDirectory;
inline int draftPort = 8974, runningPort = 8974;
inline bool ready = false, wantSettings = false, wantLog = false;
inline std::string utf8Path(const fs::path& path) {
  auto bytes = path.u8string();
  return std::string(bytes.begin(), bytes.end());
}
inline std::mutex browseMutex;
inline std::string chosenDirectory;
#ifdef _WIN32
inline HANDLE processJob = nullptr;
#endif

inline std::string endpoint() { return "http://127.0.0.1:" + std::to_string(process ? runningPort : ctx.prefs.mcpServerPort) + "/mcp"; }

inline void stop() {
  if (process) {
#ifdef _WIN32
    if (processJob) { CloseHandle(processJob); processJob = nullptr; }
#endif
    SDL_KillProcess(process, true);
    SDL_WaitProcess(process, true, nullptr);
    SDL_DestroyProcess(process);
    process = nullptr;
    message = "Stopped";
  }
  ready = false;
  projectPath.clear();
  Pyrite64MCP::directoryOverride.clear();
}

inline void tick() {
  if (!process) return;
  auto stream = SDL_GetProcessOutput(process);
  char buffer[1024];
  if (stream) for (int i = 0; i < 32; ++i) {
    auto count = SDL_ReadIO(stream, buffer, sizeof(buffer));
    if (!count) break;
    output.append(buffer, count);
    if (output.find("Uvicorn running on") != std::string::npos) { ready = true; message = "Server running at " + endpoint(); }
    if (output.size() > 16384) output.erase(0, output.size() - 16384);
  }
  int exitCode = 0;
  if (SDL_WaitProcess(process, false, &exitCode)) {
    stop();
    message = "Server exited. Open Server Log for details; check setup and port availability.";
  } else if (!ctx.project || ctx.project->getPath() != projectPath) {
    stop();
    message = "Stopped because the project was closed or changed.";
  }
}

inline void start() {
  if (process) return;
  output.clear();
  try {
    if (!ctx.project) throw std::runtime_error("Open a game project first.");
    if (ctx.prefs.mcpServerDirectory.empty()) throw std::runtime_error("Select the companion server folder in MCP > Server Settings.");
    auto& selectedDirectory = ctx.prefs.mcpServerDirectory;
    auto repository = fs::absolute(fs::path(std::u8string(selectedDirectory.begin(), selectedDirectory.end())));
#ifdef _WIN32
    auto python = repository / ".venv/Scripts/python.exe";
#else
    auto python = repository / ".venv/bin/python";
#endif
    auto source = repository / "src";
    if (!fs::is_regular_file(source / "pyrite64_mcp/server.py"))
      throw std::runtime_error("Choose the pyrata64-mcp repository folder.");
    if (!fs::is_regular_file(python))
      throw std::runtime_error("Set up the companion server's .venv first. See MCP > Setup Instructions.");
    if (ctx.prefs.mcpServerPort < 1 || ctx.prefs.mcpServerPort > 65535)
      throw std::runtime_error("The port must be between 1 and 65535.");
#ifdef _WIN32
    auto editorPid = GetCurrentProcessId();
#else
    auto editorPid = getpid();
#endif
    auto bridge = Utils::Proc::getAppDataPath() / "mcp" / ("bridge-" + std::to_string(editorPid));
    fs::create_directories(bridge);
    projectPath = ctx.project->getPath();
    runningPort = ctx.prefs.mcpServerPort;
    std::vector<std::string> values{utf8Path(python), "-m", "pyrite64_mcp.server", "--mode", "live",
      "--project", projectPath, "--executable", utf8Path(Utils::Proc::getSelfPath()),
      "--bridge-dir", utf8Path(bridge), "--port", std::to_string(runningPort)};
    std::vector<const char*> args;
    for (auto& value : values) args.push_back(value.c_str());
    args.push_back(nullptr);
    auto env = SDL_CreateEnvironment(true);
    auto props = SDL_CreateProperties();
    if (!env || !props) {
      if (env) SDL_DestroyEnvironment(env);
      if (props) SDL_DestroyProperties(props);
      throw std::runtime_error(SDL_GetError());
    }
    SDL_SetEnvironmentVariable(env, "PYTHONPATH", utf8Path(source).c_str(), true);
    SDL_SetEnvironmentVariable(env, "PYTHONUNBUFFERED", "1", true);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, utf8Path(repository).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
#ifdef _WIN32
    // SDL's Windows background flag hides the console while retaining process tracking.
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (!process) throw std::runtime_error(SDL_GetError());
#ifdef _WIN32
    processJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    auto pid = SDL_GetNumberProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0);
    auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    bool assigned = processJob && handle && SetInformationJobObject(processJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(processJob, handle);
    if (handle) CloseHandle(handle);
    if (!assigned) throw std::runtime_error("Could not manage the server process. Launch the companion server externally instead.");
#endif
    Pyrite64MCP::directoryOverride = utf8Path(bridge);
    ready = false;
    message = "Starting server...";
  } catch (const std::exception& error) {
    stop();
    message = error.what();
  }
}

inline void browseCallback(void*, const char* const* files, int) {
  if (!files || !files[0]) return;
  std::lock_guard lock(browseMutex);
  chosenDirectory = files[0];
}

inline void drawMenu() {
  if (ImGui::BeginMenu("MCP")) {
    ImGui::TextDisabled("%s", process ? (ready ? "Running" : "Starting...") : "Stopped");
    if (ImGui::MenuItem("Start Server", nullptr, false, !process && ctx.project && !ctx.isBuildOrRunning())) {
      if (ctx.prefs.mcpServerDirectory.empty()) wantSettings = true;
      else start();
    }
    if (ImGui::MenuItem("Stop Server", nullptr, false, process != nullptr)) stop();
    ImGui::Separator();
    if (ImGui::MenuItem("Server Settings...", nullptr, false, process == nullptr)) wantSettings = true;
    if (ImGui::MenuItem("Server Log...")) wantLog = true;
    if (ImGui::MenuItem("Copy MCP URL")) ImGui::SetClipboardText(endpoint().c_str());
    if (ImGui::BeginMenu("Copy Client Command")) {
      if (ImGui::MenuItem("Codex")) ImGui::SetClipboardText(("codex mcp add pyrite64 --url " + endpoint()).c_str());
      if (ImGui::MenuItem("Claude Code")) ImGui::SetClipboardText(("claude mcp add --transport http pyrite64 " + endpoint()).c_str());
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Setup Instructions")) Utils::Proc::openURL("https://github.com/boricuapab/pyrata64-mcp#easy-windows-launcher");
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::EndMenu();
  }
}

inline void drawDialogs() {
  if (wantSettings) {
    draftDirectory = ctx.prefs.mcpServerDirectory;
    draftPort = ctx.prefs.mcpServerPort;
    wantSettings = false;
    ImGui::OpenPopup("MCP Server Settings");
  }
  {
    std::lock_guard lock(browseMutex);
    if (!chosenDirectory.empty()) { draftDirectory = std::move(chosenDirectory); chosenDirectory.clear(); }
  }
  ImGui::SetNextWindowSize({640, 0}, ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("MCP Server Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Select the companion pyrata64-mcp folder. Set up its Python environment first; the editor will use its .venv.");
    ImGui::SetNextItemWidth(490);
    ImGui::InputText("##serverDirectory", &draftDirectory);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) SDL_ShowOpenFolderDialog(browseCallback, nullptr, ctx.window, draftDirectory.empty() ? nullptr : draftDirectory.c_str(), false);
    ImGui::InputInt("Port", &draftPort);
    ImGui::TextWrapped("Settings are stored locally. The server uses the open project and stops when it closes or the editor exits.");
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    bool valid = !draftDirectory.empty() && draftPort > 0 && draftPort <= 65535;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Save and Start")) {
      ctx.prefs.mcpServerDirectory = draftDirectory;
      ctx.prefs.mcpServerPort = draftPort;
      ctx.prefs.save();
      start();
      if (process) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      ctx.prefs.mcpServerDirectory = draftDirectory;
      ctx.prefs.mcpServerPort = draftPort;
      ctx.prefs.save();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (wantLog) { wantLog = false; ImGui::OpenPopup("MCP Server Log"); }
  ImGui::SetNextWindowSize({720, 360}, ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("MCP Server Log", nullptr)) {
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::BeginChild("output", {0, -40}, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(output.empty() ? "No server output yet." : output.c_str());
    ImGui::EndChild();
    if (ImGui::Button("Copy Log")) ImGui::SetClipboardText(output.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}
} // namespace Editor::MCP
