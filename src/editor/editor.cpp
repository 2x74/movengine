#include <cstdio>
#include "editor/editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <utility>

#include "collision/map_triggers.h"
#include "vmf/bsp_import.h"
#include "vmf/playtest.h"

namespace editor {

namespace {

constexpr size_t kMaxUndo = 64;

bool isSpawnClass(const std::string& cls) {
    std::string c = collision::toLowerAscii(cls);
    return c == "info_player_terrorist" || c == "info_player_counterterrorist" || c == "info_player_start";
}

// ray vs convex volume (intersection of planes). returns the entry distance and the plane it entered through, misses if the ray starts inside
bool rayVsPlanes(const glm::vec3& origin, const glm::vec3& dir, const std::vector<vmf::Plane>& planes, float& tOut,
                 glm::vec3& normalOut) {
    float tEnter = -std::numeric_limits<float>::max();
    float tExit = std::numeric_limits<float>::max();
    glm::vec3 normal(0.0f, 0.0f, 1.0f);
    for (const auto& p : planes) {
        float denom = glm::dot(p.normal, dir);
        float dist = glm::dot(p.normal, origin) - p.dist;
        if (std::abs(denom) < 1e-8f) {
            if (dist > 0.0f) return false;
            continue;
        }
        float t = -dist / denom;
        if (denom < 0.0f) {
            if (t > tEnter) {
                tEnter = t;
                normal = p.normal;
            }
        } else {
            tExit = std::min(tExit, t);
        }
    }
    if (tEnter > tExit || tEnter < 0.0f) return false;
    tOut = tEnter;
    normalOut = normal;
    return true;
}

std::vector<vmf::Plane> boxPlanes(const glm::vec3& min, const glm::vec3& max) {
    return {{{1, 0, 0}, max.x}, {{-1, 0, 0}, -min.x}, {{0, 1, 0}, max.y},
            {{0, -1, 0}, -min.y}, {{0, 0, 1}, max.z}, {{0, 0, -1}, -min.z}};
}

void reassignIds(vmf::Document& doc, vmf::Solid& solid) {
    solid.id = doc.allocateId();
    for (auto& side : solid.sides) side.id = doc.allocateId();
}

}  // namespace

const char* toolName(Tool tool) {
    switch (tool) {
        case Tool::Select: return "select";
        case Tool::Block: return "block";
        case Tool::Ramp: return "ramp";
        case Tool::Spawn: return "spawn";
        case Tool::StartZone: return "start zone";
        case Tool::EndZone: return "end zone";
        case Tool::Teleport: return "teleport";
        case Tool::Booster: return "booster";
        case Tool::Push: return "push";
        case Tool::PointEntity: return "entity";
        case Tool::Light: return "light";
    }
    return "?";
}

Editor::Editor() {
    newMap();
}

void Editor::newMap() {
    doc = vmf::emptyDocument();
    doc.world.solids.push_back(vmf::makeBlock(doc, {-1024, -1024, -16}, {1024, 1024, 0}, vmf::kDefaultBlockMaterial));
    for (const char* cls : {"info_player_terrorist", "info_player_counterterrorist"}) {
        vmf::Entity spawn;
        spawn.id = doc.allocateId();
        spawn.keyValues = {{"classname", cls}, {"origin", "0 0 0"}, {"angles", "0 0 0"}};
        doc.entities.push_back(spawn);
    }
    path.clear();
    dirty = false;
    selection = {};
    undo_.clear();
    redo_.clear();
    revision++;
}

bool Editor::open(const std::string& file) {
    std::filesystem::path p(file);
    bool compiled = collision::toLowerAscii(p.extension().string()) == ".bsp";
    // a .bsp is decompiled, saving writes a .vmf beside it
    auto loaded = compiled ? vmf::importBsp(file) : vmf::loadDocument(file);
    if (!loaded) return false;
    doc = std::move(*loaded);
    path = compiled ? p.replace_extension(".vmf").string() : file;
    dirty = compiled;
    selection = {};
    undo_.clear();
    redo_.clear();
    revision++;
    return true;
}

bool Editor::save(const std::string& file) {
    if (!vmf::saveDocument(file, doc)) return false;
    path = file;
    dirty = false;
    return true;
}

void Editor::checkpoint() {
    undo_.push_back(doc);
    if (undo_.size() > kMaxUndo) undo_.pop_front();
    redo_.clear();
}

bool Editor::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(doc));
    doc = std::move(undo_.back());
    undo_.pop_back();
    selection = {};
    changed();
    return true;
}

bool Editor::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(doc));
    doc = std::move(redo_.back());
    redo_.pop_back();
    selection = {};
    changed();
    return true;
}

void Editor::touched() {
    changed();
}

void Editor::changed() {
    dirty = true;
    revision++;
}

float Editor::snap(float v) const {
    return grid > 0.0f ? std::round(v / grid) * grid : v;
}

glm::vec3 Editor::snap(const glm::vec3& v) const {
    return {snap(v.x), snap(v.y), snap(v.z)};
}

bool Editor::pointEntityBounds(const vmf::Entity& e, glm::vec3& min, glm::vec3& max) {
    glm::vec3 origin = collision::parseKeyValueVec3(e.get("origin"));
    if (isSpawnClass(e.classname())) {
        min = origin + glm::vec3(-16, -16, 0);
        max = origin + glm::vec3(16, 16, 72);
    } else {
        min = origin - glm::vec3(8.0f);
        max = origin + glm::vec3(8.0f);
    }
    return true;
}

RayHit Editor::raycast(const glm::vec3& origin, const glm::vec3& dir) const {
    RayHit best;
    best.distance = std::numeric_limits<float>::max();
    auto consider = [&](const std::vector<vmf::Plane>& planes, Selection target) {
        if (isHidden(target)) return;  // hidden means out of the mouse's way too
        float t;
        glm::vec3 n;
        if (rayVsPlanes(origin, dir, planes, t, n) && t < best.distance) {
            best = {true, t, origin + dir * t, n, target};
        }
    };
    for (size_t i = 0; i < doc.world.solids.size(); ++i) {
        consider(vmf::solidPlanes(doc.world.solids[i]), {Selection::Kind::WorldSolid, static_cast<int>(i)});
    }
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        const auto& e = doc.entities[i];
        Selection target{Selection::Kind::Entity, static_cast<int>(i)};
        if (e.solids.empty()) {
            glm::vec3 mn, mx;
            pointEntityBounds(e, mn, mx);
            consider(boxPlanes(mn, mx), target);
        } else {
            for (const auto& s : e.solids) consider(vmf::solidPlanes(s), target);
        }
    }
    if (!best.hit) best.distance = 0.0f;
    return best;
}

vmf::Entity* Editor::selectedEntity() {
    if (selection.kind != Selection::Kind::Entity) return nullptr;
    if (selection.index < 0 || selection.index >= static_cast<int>(doc.entities.size())) return nullptr;
    return &doc.entities[selection.index];
}

vmf::Solid* Editor::selectedWorldSolid() {
    if (selection.kind != Selection::Kind::WorldSolid) return nullptr;
    if (selection.index < 0 || selection.index >= static_cast<int>(doc.world.solids.size())) return nullptr;
    return &doc.world.solids[selection.index];
}

glm::vec3 Editor::spawnFeet() const {
    for (const auto& e : doc.entities) {
        if (isSpawnClass(e.classname())) return collision::parseKeyValueVec3(e.get("origin"));
    }
    return glm::vec3(0.0f);
}

std::string Editor::uniqueName(const std::string& prefix) {
    for (int n = 1;; ++n) {
        std::string name = prefix + std::to_string(n);
        bool used = std::any_of(doc.entities.begin(), doc.entities.end(), [&](const vmf::Entity& e) {
            return collision::toLowerAscii(e.get("targetname")) == collision::toLowerAscii(name);
        });
        if (!used) return name;
    }
}

int Editor::findZone(const std::string& name) const {
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        const auto& e = doc.entities[i];
        if (collision::toLowerAscii(e.classname()) == "trigger_multiple" &&
            collision::toLowerAscii(e.get("targetname")) == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Editor::addBrushEntity(const std::string& classname, const glm::vec3& min, const glm::vec3& max,
                            std::vector<std::pair<std::string, std::string>> keyValues,
                            std::vector<std::pair<std::string, std::string>> connections) {
    vmf::Entity e;
    e.id = doc.allocateId();
    e.keyValues.emplace_back("classname", classname);
    for (auto& kv : keyValues) e.keyValues.push_back(std::move(kv));
    e.connections = std::move(connections);
    e.solids.push_back(vmf::makeBlock(doc, min, max, vmf::kTriggerMaterial));
    doc.entities.push_back(std::move(e));
    selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
}

void Editor::place(Tool tool, const glm::vec3& point, const glm::vec3& normal, float yawDeg) {
    if (tool == Tool::Select) return;

    glm::vec3 size(128.0f, 128.0f, 64.0f);
    switch (tool) {
        case Tool::Ramp:
            size = (rampDir == vmf::RampDir::PlusX || rampDir == vmf::RampDir::MinusX) ? glm::vec3(256, 128, 128)
                                                                                         : glm::vec3(128, 256, 128);
            break;
        case Tool::StartZone:
        case Tool::EndZone: size = {256, 256, 128}; break;
        case Tool::Teleport: size = {256, 256, 32}; break;
        case Tool::Booster: size = {64, 64, 16}; break;
        case Tool::Push: size = {128, 128, 128}; break;
        default: break;
    }

    // sit the box against the surface along the hit normal's main axis, centered on the (snapped) hit point along the other two
    glm::vec3 at = snap(point);
    glm::vec3 a = glm::abs(normal);
    int axis = (a.z >= a.x && a.z >= a.y) ? 2 : (a.x >= a.y ? 0 : 1);
    glm::vec3 min = at - size * 0.5f;
    if (normal[axis] >= 0.0f) {
        min[axis] = at[axis];
    } else {
        min[axis] = at[axis] - size[axis];
    }
    min = snap(min);
    glm::vec3 max = min + size;
    glm::vec3 feet = at;  // where point entities stand

    checkpoint();
    std::string yaw = vmf::formatNumber(std::round(yawDeg));
    switch (tool) {
        case Tool::Block:
            doc.world.solids.push_back(vmf::makeBlock(doc, min, max, brushMaterial));
            selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
            break;
        case Tool::Ramp:
            // orange by default so ramps stand out, until a texture's been picked
            doc.world.solids.push_back(vmf::makeRamp(
                doc, min, max, rampDir,
                brushMaterial == vmf::kDefaultBlockMaterial ? vmf::kDefaultRampMaterial : brushMaterial));
            selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
            break;
        case Tool::Spawn:
            for (const char* cls : {"info_player_terrorist", "info_player_counterterrorist"}) {
                vmf::Entity e;
                e.id = doc.allocateId();
                e.keyValues = {{"classname", cls}, {"origin", vmf::formatVec3(feet)}, {"angles", "0 " + yaw + " 0"}};
                doc.entities.push_back(std::move(e));
            }
            selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 2};
            break;
        case Tool::StartZone:
        case Tool::EndZone: {
            std::string name = vmf::zoneTargetname(zoneTrack, tool == Tool::StartZone);
            int existing = findZone(name);
            if (existing >= 0) {
                // one start and one end zone per track, placing again moves it
                auto& e = doc.entities[existing];
                e.solids.clear();
                e.solids.push_back(vmf::makeBlock(doc, min, max, vmf::kTriggerMaterial));
                selection = {Selection::Kind::Entity, existing};
            } else {
                addBrushEntity("trigger_multiple", min, max, {{"targetname", name}, {"spawnflags", "1"}, {"wait", "0.1"}});
            }
            break;
        }
        case Tool::Teleport: {
            // the destination starts at the spawn, i.e. a fail teleport back to the start. move it in the inspector or select and drag it
            std::string dest = uniqueName("tp_dest_");
            vmf::Entity d;
            d.id = doc.allocateId();
            d.keyValues = {{"classname", "info_teleport_destination"}, {"targetname", dest},
                           {"origin", vmf::formatVec3(spawnFeet())}, {"angles", "0 " + yaw + " 0"}};
            doc.entities.push_back(std::move(d));
            addBrushEntity("trigger_teleport", min, max, {{"target", dest}, {"spawnflags", "1"}});
            break;
        }
        case Tool::Booster:
            // bhop_bunker's style: launches you as you leave it (jump off it).
            addBrushEntity("trigger_multiple", min, max, {{"spawnflags", "1"}, {"wait", "0.1"}},
                           {{"OnEndTouch", "!activator,AddOutput,basevelocity 0 0 500,0,-1"}});
            break;
        case Tool::Push:
            addBrushEntity("trigger_push", min, max, {{"pushdir", "-90 0 0"}, {"speed", "800"}, {"spawnflags", "1"}});
            break;
        case Tool::PointEntity: {
            vmf::Entity e;
            e.id = doc.allocateId();
            e.keyValues = {{"classname", pointEntityClass}, {"origin", vmf::formatVec3(feet + normal * 8.0f)},
                           {"angles", "0 " + yaw + " 0"}};
            doc.entities.push_back(std::move(e));
            selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
            break;
        }
        case Tool::Light: {
            vmf::Entity e;
            e.id = doc.allocateId();
            // off the surface a bit so it lights the face it was placed on
            e.keyValues = {{"classname", "light"}, {"origin", vmf::formatVec3(snap(point + normal * 32.0f))},
                           {"_light", "255 240 220 200"}, {"_fifty_percent_distance", "128"}};
            doc.entities.push_back(std::move(e));
            selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
            break;
        }
        case Tool::Select: break;
    }
    changed();
}

vmf::Entity* Editor::sun() {
    for (auto& e : doc.entities) {
        if (collision::toLowerAscii(e.classname()) == "light_environment") return &e;
    }
    return nullptr;
}

void Editor::addSun() {
    if (sun()) return;
    checkpoint();
    vmf::Entity e;
    e.id = doc.allocateId();
    e.keyValues = {{"classname", "light_environment"}, {"origin", vmf::formatVec3(spawnFeet() + glm::vec3(0, 0, 128))},
                   {"angles", "0 45 0"},  {"pitch", "-50"},
                   {"_light", "255 244 220 300"}, {"_ambient", "160 180 210 40"}};
    doc.entities.push_back(std::move(e));
    changed();
}

void Editor::removeSun() {
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        if (collision::toLowerAscii(doc.entities[i].classname()) == "light_environment") {
            checkpoint();
            doc.entities.erase(doc.entities.begin() + static_cast<long>(i));
            selection = {};
            changed();
            return;
        }
    }
}

vmf::Entity* Editor::fog() {
    for (auto& e : doc.entities) {
        if (collision::toLowerAscii(e.classname()) == "env_fog_controller") return &e;
    }
    return nullptr;
}

void Editor::addFog() {
    if (fog()) return;
    checkpoint();
    vmf::Entity e;
    e.id = doc.allocateId();
    // a pale distance haze starting well out: visible, but not something you have to immediately dial back to see your own map
    e.keyValues = {{"classname", "env_fog_controller"},
                   {"origin", vmf::formatVec3(spawnFeet() + glm::vec3(0, 0, 64))},
                   {"fogenable", "1"},
                   {"fogcolor", "160 180 200"},
                   {"fogstart", "1500"},
                   {"fogend", "6000"},
                   {"fogmaxdensity", "1"}};
    doc.entities.push_back(std::move(e));
    changed();
}

void Editor::removeFog() {
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        if (collision::toLowerAscii(doc.entities[i].classname()) == "env_fog_controller") {
            checkpoint();
            doc.entities.erase(doc.entities.begin() + static_cast<long>(i));
            selection = {};
            changed();
            return;
        }
    }
}

std::vector<Selection> Editor::allSelected() const {
    std::vector<Selection> out;
    if (selection.kind == Selection::Kind::None) return out;
    out.reserve(alsoSelected.size() + 1);
    out.push_back(selection);
    for (const auto& s : alsoSelected) {
        if (!(s == selection)) out.push_back(s);
    }
    return out;
}

int Editor::selectionCount() const {
    return static_cast<int>(allSelected().size());
}

bool Editor::isSelected(const Selection& target) const {
    if (selection == target) return true;
    for (const auto& s : alsoSelected) {
        if (s == target) return true;
    }
    return false;
}

void Editor::toggleSelected(const Selection& target) {
    if (target.kind == Selection::Kind::None) return;
    if (selection == target) {
        // taking the anchor out: another one has to become the anchor or nothing is selected at all
        if (alsoSelected.empty()) {
            selection = {};
        } else {
            selection = alsoSelected.back();
            alsoSelected.pop_back();
        }
        return;
    }
    for (auto it = alsoSelected.begin(); it != alsoSelected.end(); ++it) {
        if (*it == target) {
            alsoSelected.erase(it);
            return;
        }
    }
    if (selection.kind != Selection::Kind::None) alsoSelected.push_back(selection);
    selection = target;
}

void Editor::clearSelection() {
    selection = {};
    alsoSelected.clear();
}

int Editor::groupIdOf(const Selection& target) const {
    if (target.kind == Selection::Kind::WorldSolid && target.index >= 0 &&
        target.index < static_cast<int>(doc.world.solids.size())) {
        return vmf::groupId(doc.world.solids[target.index]);
    }
    if (target.kind == Selection::Kind::Entity && target.index >= 0 &&
        target.index < static_cast<int>(doc.entities.size())) {
        return vmf::groupId(doc.entities[target.index]);
    }
    return 0;
}

std::vector<Selection> Editor::groupMembers(int groupId) const {
    std::vector<Selection> out;
    if (groupId <= 0) return out;
    for (size_t i = 0; i < doc.world.solids.size(); ++i) {
        if (vmf::groupId(doc.world.solids[i]) != groupId) continue;
        if (vmf::isHidden(doc.world.solids[i])) continue;
        out.push_back({Selection::Kind::WorldSolid, static_cast<int>(i)});
    }
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        if (vmf::groupId(doc.entities[i]) != groupId) continue;
        if (vmf::isHidden(doc.entities[i])) continue;
        out.push_back({Selection::Kind::Entity, static_cast<int>(i)});
    }
    return out;
}

int Editor::groupCount() const {
    std::vector<int> seen;
    auto note = [&](int g) {
        if (g <= 0) return;
        if (std::find(seen.begin(), seen.end(), g) == seen.end()) seen.push_back(g);
    };
    for (const auto& s : doc.world.solids) note(vmf::groupId(s));
    for (const auto& e : doc.entities) note(vmf::groupId(e));
    return static_cast<int>(seen.size());
}

std::vector<Selection> Editor::clickTargets(const Selection& target) const {
    std::vector<Selection> out;
    if (target.kind == Selection::Kind::None) return out;
    const int group = ignoreGroups ? 0 : groupIdOf(target);
    if (group > 0) out = groupMembers(group);
    // ungrouped, groups being ignored, or a group whose only reachable member is this one: just the thing that was clicked
    if (out.empty()) out.push_back(target);
    return out;
}

void Editor::selectClicked(const Selection& target, bool add) {
    std::vector<Selection> picks = clickTargets(target);
    if (picks.empty()) return;
    if (add && isSelected(target)) {
        // ctrl+click on something already in takes it back out, and the rest of its group with it since that's what went in
        for (const Selection& pick : picks) {
            if (selection == pick) selection = {};
            std::erase(alsoSelected, pick);
        }
        if (selection.kind == Selection::Kind::None && !alsoSelected.empty()) {
            selection = alsoSelected.back();
            alsoSelected.pop_back();
        }
        return;
    }
    if (!add) {
        clearSelection();
    } else if (selection.kind != Selection::Kind::None) {
        alsoSelected.push_back(selection);  // the old anchor stays selected
    }
    // the thing actually under the cursor becomes the anchor so the inspector shows what was clicked and not whichever member comes first in the file. the rest of its group rides along
    selection = target;
    for (const Selection& pick : picks) {
        if (!(pick == target) && !isSelected(pick)) alsoSelected.push_back(pick);
    }
}

bool Editor::groupSelection() {
    std::vector<Selection> targets = allSelected();
    if (targets.size() < 2) return false;
    checkpoint();
    // one fresh id for the lot. grouping a group together with a loose brush folds the old group into the new one instead of nesting it, which is what hammer does and all the vmf format can say
    const int group = doc.allocateId();
    for (const Selection& target : targets) {
        if (target.kind == Selection::Kind::WorldSolid && target.index >= 0 &&
            target.index < static_cast<int>(doc.world.solids.size())) {
            vmf::setGroupId(doc.world.solids[target.index], group);
        } else if (target.kind == Selection::Kind::Entity && target.index >= 0 &&
                   target.index < static_cast<int>(doc.entities.size())) {
            vmf::setGroupId(doc.entities[target.index], group);
        }
    }
    vmf::tidyGroups(doc);
    changed();
    return true;
}

bool Editor::ungroupSelection() {
    std::vector<int> groups;
    for (const Selection& target : allSelected()) {
        int group = groupIdOf(target);
        if (group > 0 && std::find(groups.begin(), groups.end(), group) == groups.end()) {
            groups.push_back(group);
        }
    }
    if (groups.empty()) return false;
    checkpoint();
    auto breaks = [&](int group) { return std::find(groups.begin(), groups.end(), group) != groups.end(); };
    // every member, hidden ones included: a hidden brush left behind in a group whose other members are loose would be a group of one in waiting
    for (auto& s : doc.world.solids) {
        if (breaks(vmf::groupId(s))) vmf::setGroupId(s, 0);
    }
    for (auto& e : doc.entities) {
        if (breaks(vmf::groupId(e))) vmf::setGroupId(e, 0);
    }
    vmf::tidyGroups(doc);
    changed();
    return true;
}

bool Editor::isHidden(const Selection& target) const {
    if (target.kind == Selection::Kind::WorldSolid && target.index >= 0 &&
        target.index < static_cast<int>(doc.world.solids.size())) {
        return vmf::isHidden(doc.world.solids[target.index]);
    }
    if (target.kind == Selection::Kind::Entity && target.index >= 0 &&
        target.index < static_cast<int>(doc.entities.size())) {
        return vmf::isHidden(doc.entities[target.index]);
    }
    return false;
}

void Editor::hideSelection() {
    std::vector<Selection> targets = allSelected();
    if (targets.empty()) return;
    checkpoint();
    for (const auto& target : targets) {
        if (target.kind == Selection::Kind::WorldSolid && target.index >= 0 &&
            target.index < static_cast<int>(doc.world.solids.size())) {
            vmf::setHidden(doc.world.solids[target.index], true);
        } else if (target.kind == Selection::Kind::Entity && target.index >= 0 &&
                   target.index < static_cast<int>(doc.entities.size())) {
            vmf::setHidden(doc.entities[target.index], true);
        }
    }
    // nothing may stay selected that can't be clicked
    clearSelection();
    changed();
}

void Editor::unhide(const Selection& target) {
    if (!isHidden(target)) return;
    checkpoint();
    if (target.kind == Selection::Kind::WorldSolid) vmf::setHidden(doc.world.solids[target.index], false);
    else if (target.kind == Selection::Kind::Entity) vmf::setHidden(doc.entities[target.index], false);
    changed();
}

void Editor::unhideAll() {
    if (hiddenCount() == 0) return;
    checkpoint();
    for (auto& s : doc.world.solids) vmf::setHidden(s, false);
    for (auto& e : doc.entities) vmf::setHidden(e, false);
    changed();
}

int Editor::hiddenCount() const {
    int n = 0;
    for (const auto& s : doc.world.solids) {
        if (vmf::isHidden(s)) ++n;
    }
    for (const auto& e : doc.entities) {
        if (vmf::isHidden(e)) ++n;
    }
    return n;
}

std::vector<std::pair<std::string, Selection>> Editor::hiddenItems() const {
    std::vector<std::pair<std::string, Selection>> out;
    for (size_t i = 0; i < doc.world.solids.size(); ++i) {
        if (!vmf::isHidden(doc.world.solids[i])) continue;
        const auto& sides = doc.world.solids[i].sides;
        std::string material = sides.empty() ? std::string("brush") : sides.front().material;
        out.emplace_back("brush  " + material, Selection{Selection::Kind::WorldSolid, static_cast<int>(i)});
    }
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        if (!vmf::isHidden(doc.entities[i])) continue;
        const auto& e = doc.entities[i];
        std::string label = e.classname();
        if (std::string name = e.get("targetname"); !name.empty()) label += "  " + name;
        out.emplace_back(std::move(label), Selection{Selection::Kind::Entity, static_cast<int>(i)});
    }
    return out;
}

bool Editor::selectionBounds(glm::vec3& min, glm::vec3& max) const {
    bool any = false;
    auto grow = [&](const glm::vec3& mn, const glm::vec3& mx) {
        min = any ? glm::min(min, mn) : mn;
        max = any ? glm::max(max, mx) : mx;
        any = true;
    };
    // the box around everything selected so the gizmo wraps the group and not whichever one got clicked last
    for (const Selection& sel : allSelected()) {
        glm::vec3 mn, mx;
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            if (vmf::solidBounds(doc.world.solids[sel.index], mn, mx)) grow(mn, mx);
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            const auto& e = doc.entities[sel.index];
            if (e.solids.empty()) {
                if (pointEntityBounds(e, mn, mx)) grow(mn, mx);
            } else {
                for (const auto& s : e.solids) {
                    if (vmf::solidBounds(s, mn, mx)) grow(mn, mx);
                }
            }
        }
    }
    return any;
}

void Editor::moveSelection(const glm::vec3& delta, bool withCheckpoint) {
    std::vector<Selection> targets = allSelected();
    if (targets.empty()) return;
    if (withCheckpoint) checkpoint();
    for (const Selection& sel : targets) {
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            vmf::translateSolid(doc.world.solids[sel.index], delta);
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            auto& e = doc.entities[sel.index];
            for (auto& s : e.solids) vmf::translateSolid(s, delta);
            std::string origin = e.get("origin");
            if (!origin.empty()) e.set("origin", vmf::formatVec3(collision::parseKeyValueVec3(origin) + delta));
        }
    }
    changed();
}

void Editor::resizeSelection(const glm::vec3& newMinIn, const glm::vec3& newMaxIn, bool withCheckpoint) {
    glm::vec3 oldMin, oldMax;
    if (!selectionBounds(oldMin, oldMax)) return;
    glm::vec3 newMin = glm::min(newMinIn, newMaxIn), newMax = glm::max(newMinIn, newMaxIn);
    // never collapse an axis to nothing
    for (int a = 0; a < 3; ++a) {
        if (newMax[a] - newMin[a] < 1.0f) newMax[a] = newMin[a] + 1.0f;
    }
    if (withCheckpoint) checkpoint();
    // every piece is mapped through the same box so a group keeps its shape and spacing instead of each piece being stretched to the whole
    for (const Selection& sel : allSelected()) {
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            vmf::fitSolidToBounds(doc.world.solids[sel.index], oldMin, oldMax, newMin, newMax);
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            for (auto& s : doc.entities[sel.index].solids) {
                vmf::fitSolidToBounds(s, oldMin, oldMax, newMin, newMax);
            }
        }
    }
    changed();
}

void Editor::rotateSelection(const glm::vec3& axis, float degrees, bool withCheckpoint) {
    glm::vec3 mn, mx;
    if (!selectionBounds(mn, mx)) return;
    std::vector<Selection> targets = allSelected();
    if (targets.empty()) return;
    // one pivot for the lot: the middle of the group, so several brushes turn around each other and not each on its own centre
    const glm::vec3 pivot = (mn + mx) * 0.5f;
    if (withCheckpoint) checkpoint();
    for (const Selection& sel : targets) {
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            vmf::rotateSolid(doc.world.solids[sel.index], pivot, axis, degrees);
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            auto& e = doc.entities[sel.index];
            if (e.solids.empty()) {
                // a point entity has no geometry to turn so it turns by its "angles" key instead. source orders that pitch yaw roll
                float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
                std::sscanf(e.get("angles", "0 0 0").c_str(), "%f %f %f", &pitch, &yaw, &roll);
                if (std::abs(axis.z) > 0.5f) yaw += degrees;
                else if (std::abs(axis.y) > 0.5f) pitch += degrees;
                else roll += degrees;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g %g %g", pitch, yaw, roll);
                e.set("angles", buf);
            } else {
                for (auto& s : e.solids) vmf::rotateSolid(s, pivot, axis, degrees);
            }
        }
    }
    changed();
}

void Editor::deleteSelection() {
    std::vector<Selection> targets = allSelected();
    if (targets.empty()) return;
    // highest index first, within each list: erasing shifts everything after it so going the other way would delete the wrong things
    std::vector<int> solids, entities;
    for (const Selection& sel : targets) {
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            solids.push_back(sel.index);
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            entities.push_back(sel.index);
        }
    }
    if (solids.empty() && entities.empty()) return;
    checkpoint();
    std::sort(solids.rbegin(), solids.rend());
    std::sort(entities.rbegin(), entities.rend());
    for (int i : solids) doc.world.solids.erase(doc.world.solids.begin() + i);
    for (int i : entities) doc.entities.erase(doc.entities.begin() + i);
    // deleting out of a group can leave it with one member, or none
    vmf::tidyGroups(doc);
    clearSelection();
    changed();
}

namespace {

// the tool materials that are collision only: what "remove all clips" means
bool isClipMaterial(const std::string& material) {
    std::string m = collision::toLowerAscii(material);
    return m == "tools/toolsplayerclip" || m == "tools/toolsclip" || m == "tools/toolsnpcclip";
}

bool solidIsClip(const vmf::Solid& s) {
    if (s.sides.empty()) return false;
    for (const auto& side : s.sides) {
        if (!isClipMaterial(side.material)) return false;
    }
    return true;
}

}  // namespace

int Editor::removeEntitiesOfClass(const std::string& classname) {
    std::string want = collision::toLowerAscii(classname);
    int matches = 0;
    for (const auto& e : doc.entities) {
        if (collision::toLowerAscii(e.classname()) == want) ++matches;
    }
    if (matches == 0) return 0;
    checkpoint();
    std::erase_if(doc.entities, [&](const vmf::Entity& e) {
        return collision::toLowerAscii(e.classname()) == want;
    });
    vmf::tidyGroups(doc);
    // indices just moved under it
    selection = {};
    changed();
    return matches;
}

int Editor::removeClipBrushes() {
    int matches = clipBrushCount();
    if (matches == 0) return 0;
    checkpoint();
    std::erase_if(doc.world.solids, solidIsClip);
    // vbsp files clip brushes under func_detail instead of leaving them in the world, so a decompiled map keeps nearly all of them inside a brush entity. clearing them can empty that entity and a brush entity with no brushes is nothing at all, so drop it. point entities never had solids so they're untouched
    std::vector<vmf::Entity> kept;
    kept.reserve(doc.entities.size());
    for (auto& e : doc.entities) {
        const bool wasBrushEntity = !e.solids.empty();
        std::erase_if(e.solids, solidIsClip);
        if (wasBrushEntity && e.solids.empty()) continue;
        kept.push_back(std::move(e));
    }
    doc.entities = std::move(kept);
    vmf::tidyGroups(doc);
    selection = {};
    changed();
    return matches;
}

std::vector<std::pair<std::string, int>> Editor::entityClassCounts() const {
    std::map<std::string, int> counts;
    for (const auto& e : doc.entities) {
        std::string cls = e.classname();
        if (!cls.empty()) ++counts[cls];
    }
    return {counts.begin(), counts.end()};
}

int Editor::clipBrushCount() const {
    int n = 0;
    for (const auto& s : doc.world.solids) {
        if (solidIsClip(s)) ++n;
    }
    for (const auto& e : doc.entities) {
        for (const auto& s : e.solids) {
            if (solidIsClip(s)) ++n;
        }
    }
    return n;
}

bool Editor::duplicateSelectionInPlace() {
    if (auto* s = selectedWorldSolid()) {
        vmf::Solid copy = *s;
        checkpoint();
        reassignIds(doc, copy);
        // a lone copy of one member of a group isn't part of that group, and isn't a group of its own either
        vmf::setGroupId(copy, 0);
        doc.world.solids.push_back(std::move(copy));
        selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
    } else if (auto* e = selectedEntity()) {
        vmf::Entity copy = *e;
        checkpoint();
        copy.id = doc.allocateId();
        vmf::setGroupId(copy, 0);
        for (auto& s : copy.solids) reassignIds(doc, s);
        doc.entities.push_back(std::move(copy));
        selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
    } else {
        return false;
    }
    changed();
    return true;
}

bool Editor::hasClipboard() const {
    return clipboard_.kind != Clipboard::Kind::None;
}

bool Editor::copySelection() {
    if (auto* s = selectedWorldSolid()) {
        clipboard_.kind = Clipboard::Kind::WorldSolid;
        clipboard_.solid = *s;
        clipboard_.entity = {};
        return true;
    }
    if (auto* e = selectedEntity()) {
        clipboard_.kind = Clipboard::Kind::Entity;
        clipboard_.entity = *e;
        clipboard_.solid = {};
        return true;
    }
    return false;
}

bool Editor::cutSelection() {
    // copy first: if there's nothing to copy there's nothing to cut, and the clipboard keeps whatever it had instead of being emptied by a ctrl+x that hit nothing. deleteSelection takes the checkpoint
    if (!copySelection()) return false;
    deleteSelection();
    return true;
}

bool Editor::pasteClipboard() {
    if (clipboard_.kind == Clipboard::Kind::None) return false;
    checkpoint();
    // fresh ids: the copy has to be its own brush and not a second reference to the one it came from, and pasting into a different map mustn't collide with ids already in it
    if (clipboard_.kind == Clipboard::Kind::WorldSolid) {
        vmf::Solid copy = clipboard_.solid;
        reassignIds(doc, copy);
        vmf::setGroupId(copy, 0);  // the copy joins no group of its own accord
        doc.world.solids.push_back(std::move(copy));
        selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
    } else {
        vmf::Entity copy = clipboard_.entity;
        copy.id = doc.allocateId();
        vmf::setGroupId(copy, 0);
        for (auto& s : copy.solids) reassignIds(doc, s);
        doc.entities.push_back(std::move(copy));
        selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
    }
    changed();
    return true;
}

void Editor::duplicateSelection() {
    glm::vec3 mn, mx;
    if (!selectionBounds(mn, mx)) return;
    // right next to the original along +x so the copy is visible
    glm::vec3 offset(std::max(snap(mx.x - mn.x), grid), 0.0f, 0.0f);
    checkpoint();
    if (auto* s = selectedWorldSolid()) {
        vmf::Solid copy = *s;
        reassignIds(doc, copy);
        vmf::setGroupId(copy, 0);
        vmf::translateSolid(copy, offset);
        doc.world.solids.push_back(std::move(copy));
        selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
    } else if (auto* e = selectedEntity()) {
        vmf::Entity copy = *e;
        copy.id = doc.allocateId();
        vmf::setGroupId(copy, 0);
        for (auto& s : copy.solids) {
            reassignIds(doc, s);
            vmf::translateSolid(s, offset);
        }
        std::string origin = copy.get("origin");
        if (!origin.empty()) copy.set("origin", vmf::formatVec3(collision::parseKeyValueVec3(origin) + offset));
        doc.entities.push_back(std::move(copy));
        selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
    }
    changed();
}

void Editor::setSelectionMaterial(const std::string& material) {
    std::vector<Selection> targets = allSelected();
    if (targets.empty()) return;
    checkpoint();
    bool touched = false;
    for (const Selection& sel : targets) {
        if (sel.kind == Selection::Kind::WorldSolid && sel.index >= 0 &&
            sel.index < static_cast<int>(doc.world.solids.size())) {
            for (auto& side : doc.world.solids[sel.index].sides) side.material = material;
            touched = true;
        } else if (sel.kind == Selection::Kind::Entity && sel.index >= 0 &&
                   sel.index < static_cast<int>(doc.entities.size())) {
            for (auto& s : doc.entities[sel.index].solids) {
                for (auto& side : s.sides) side.material = material;
                touched = true;
            }
        }
    }
    if (!touched) return;
    changed();
}

void Editor::tieSelectionToEntity(const std::string& classname) {
    auto* s = selectedWorldSolid();
    if (!s || classname.empty()) return;
    checkpoint();
    vmf::Entity e;
    e.id = doc.allocateId();
    e.keyValues = {{"classname", classname}};
    // the group belongs to the object the editor clicks, which is about to be the entity and not the brush, so the id moves with it
    const int group = vmf::groupId(*s);
    vmf::setGroupId(*s, 0);
    vmf::setGroupId(e, group);
    e.solids.push_back(std::move(*s));
    doc.world.solids.erase(doc.world.solids.begin() + selection.index);
    doc.entities.push_back(std::move(e));
    selection = {Selection::Kind::Entity, static_cast<int>(doc.entities.size()) - 1};
    vmf::tidyGroups(doc);
    changed();
}

void Editor::moveSelectionToWorld() {
    auto* e = selectedEntity();
    if (!e || e->solids.empty()) return;
    checkpoint();
    // and back the other way: each brush coming out keeps the entity's group, so untying a grouped platform leaves it grouped
    const int group = vmf::groupId(*e);
    for (auto& s : e->solids) {
        vmf::setGroupId(s, group);
        doc.world.solids.push_back(std::move(s));
    }
    doc.entities.erase(doc.entities.begin() + selection.index);
    selection = {Selection::Kind::WorldSolid, static_cast<int>(doc.world.solids.size()) - 1};
    vmf::tidyGroups(doc);
    changed();
}

}  // namespace editor
