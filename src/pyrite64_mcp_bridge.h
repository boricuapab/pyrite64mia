// Optional main-thread bridge for boricuapab/pyrite64mia. MIT licensed.
#pragma once
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "context.h"
#include "project/scene/scene.h"
#include "editor/undoRedo.h"

namespace Pyrite64MCP {
using Json = nlohmann::json;
namespace fs = std::filesystem;

inline void deserializeScene(Project::Scene& scene, Json document) {
  // The pinned fork defines GLM_FORCE_QUAT_DATA_XYZW in CMakeLists.txt.
  // Only adapt the loader if a custom build omitted that native configuration.
#ifndef GLM_FORCE_QUAT_DATA_XYZW
  auto fix = [](Json& node, auto& self) -> void {
    if (node.contains("rot")) {
      auto rotation = node.at("rot");
      if (!rotation.is_array() || rotation.size() != 4) throw std::runtime_error("Invalid rotation");
      node["rot"] = Json::array({rotation[3], rotation[0], rotation[1], rotation[2]});
    }
    for (auto& child : node.at("children")) self(child, self);
  };
  fix(document.at("graph"), fix);
#endif
  scene.deserialize(document.dump());
}

inline Project::Scene& sceneFor(const Json& args) {
  auto scene = ctx.project->getScenes().getLoadedScene();
  if (!scene) throw std::runtime_error("No scene is open");
  if (args.contains("scene_id") && args.at("scene_id").get<int>() != scene->getId())
    throw std::runtime_error("Open the requested scene in the editor first");
  return *scene;
}

inline Json dispatch(const std::string& action, const Json& args) {
  if (!ctx.project) throw std::runtime_error("Open a project in Pyrite64 first");
  auto actual = fs::weakly_canonical(ctx.project->getPath());
  auto expected = fs::weakly_canonical(args.at("project_path").get<std::string>());
  if (!fs::equivalent(actual, expected)) throw std::runtime_error("Editor has a different project open");
  if (ctx.isBuildOrRunning()) throw std::runtime_error("Editor is building/running; try again after it finishes");
  if (action == "status") {
    auto scene = ctx.project->getScenes().getLoadedScene();
    return {{"project_path", actual.string()}, {"scene_id", scene ? scene->getId() : 0},
            {"bridge_version", 1}, {"selected_object", ctx.selObjectUUID}};
  }
  if (action == "get_project") return Json::parse(ctx.project->conf.serialize());
  if (action == "save_project") {
    ctx.project->save();
    return {{"saved", true}};
  }
  if (action == "reload_assets") {
    ctx.project->getAssets().save();
    ctx.project->getAssets().reload();
    return {{"reloaded", true}};
  }
  auto& scene = sceneFor(args);
  if (action == "get_scene") return {{"scene", Json::parse(scene.serialize())}};
  if (action == "replace_scene") {
    auto old = Json::parse(scene.serialize());
    if (old != args.at("expected")) throw std::runtime_error("Scene changed in editor; read again before editing");
    auto updated = args.at("scene");
    if (!updated.is_object() || !updated.contains("conf") || !updated.contains("graph") || updated.value("version", 0) != 2)
      throw std::runtime_error("Invalid scene structure/version");
    // The MCP validates the object tree before enqueueing. Roll back native parse failures.
    auto& history = Editor::UndoRedo::getHistory();
    history.begin();
    try { deserializeScene(scene, updated); }
    catch (...) { deserializeScene(scene, old); throw; }
    ctx.clearObjectSelection();
    history.markChanged("MCP scene edit");
    history.end();
    ctx.project->markDirty();
    return {{"updated", true}, {"saved", false}};
  }
  auto object = scene.getObjectByUUID(args.at("object_uuid").get<uint32_t>());
  if (!object) throw std::runtime_error("Object does not exist (root selection unsupported)");
  if (action == "select_object") {
    ctx.setObjectSelection(object->uuid);
    return {{"selected", object->uuid}};
  }
  if (action == "add_component") {
    if (object->isPrefabInstance()) throw std::runtime_error("Unpack the prefab before adding components");
    int id = args.at("component_id").get<int>();
    if (id < 0 || id >= static_cast<int>(Project::Component::TABLE.size()))
      throw std::runtime_error("Invalid component id");
    auto count = object->components.size();
    auto& history = Editor::UndoRedo::getHistory();
    history.begin();
    object->addComponent(id);
    if (object->components.size() == count) throw std::runtime_error("Native component rules rejected this addition");
    history.markChanged("MCP add component");
    history.end();
    ctx.project->markDirty();
    return {{"object", object->serialize()}, {"saved", false}};
  }
  throw std::runtime_error("Unsupported bridge action");
}

inline void poll() {
  const char* configured = std::getenv("PYRITE64_MCP_BRIDGE_DIR");
  if (!configured || !*configured) return; // Explicit opt-in only.
  static auto last = std::chrono::steady_clock::time_point{};
  auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(50)) return;
  last = now;
  try {
    fs::path directory{configured};
    fs::create_directories(directory);
    int processed = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
      auto filename = entry.path().filename().string();
      constexpr auto suffix = ".request.json";
      if (!filename.ends_with(suffix) || filename.size() != 45) continue;
      auto id = filename.substr(0, 32);
      if (id.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
      auto responsePath = directory / (id + ".response.json");
      auto tempPath = directory / (id + ".response.tmp");
      Json response{{"id", id}, {"ok", false}};
      try {
        if (entry.file_size() > 8 * 1024 * 1024) throw std::runtime_error("Request exceeds 8 MiB");
        std::ifstream input(entry.path());
        auto request = Json::parse(input);
        double clock = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (request.at("id") != id || request.at("expires").get<double>() < clock)
          throw std::runtime_error("Expired/invalid request; no action performed");
        response["result"] = dispatch(request.at("action").get<std::string>(), request.at("args"));
        response["ok"] = true;
      } catch (const std::exception& ex) { response["error"] = ex.what(); }
      { std::ofstream output(tempPath); output << response.dump(); }
      fs::rename(tempPath, responsePath);
      fs::remove(entry.path());
      if (++processed >= 4) break;
    }
  } catch (const std::exception& ex) {
    // Bridge I/O failure must never terminate the editor's frame loop.
    fprintf(stderr, "Pyrite64 MCP bridge: %s\n", ex.what());
  }
}
} // namespace Pyrite64MCP
