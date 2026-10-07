#pragma once

#include <glm/glm.hpp>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "vmf/document.h"
#include "vmf/geometry.h"

namespace editor {

// what's selected: one world brush, or one entity (with all its brushes)
struct Selection {
    enum class Kind { None, WorldSolid, Entity };
    Kind kind = Kind::None;
    int index = -1;

    bool operator==(const Selection&) const = default;
};

struct RayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    Selection target;
};

enum class Tool { Select, Block, Ramp, Spawn, StartZone, EndZone, Teleport, Booster, Push, PointEntity, Light };
const char* toolName(Tool tool);

// the map being edited and every operation on it. no rendering or input here so the ui and tests can both drive it
class Editor {
public:
    Editor();

    vmf::Document doc;
    std::string path;  // empty until saved
    bool dirty = false;
    int revision = 0;  // bumps on every change, so views know to rebuild
    // the anchor: last thing clicked, and what the inspector edits
    Selection selection;
    // anything else ctrl+click added. things that can act on several (move, rotate, resize, delete, material, hide, clipboard) walk all of them. things that only make sense for one (inspector, tie-to-entity) stay on the anchor
    std::vector<Selection> alsoSelected;
    float grid = 16.0f;
    // hammer's "ignore groups". off (default): a click picks up the whole group the thing under the cursor is in. on: picks just that object out of its group. decompiled maps group each platform into one object so off is for shifting a platform about, on is for nudging one brush of it
    bool ignoreGroups = false;

    // tool options
    vmf::RampDir rampDir = vmf::RampDir::PlusX;
    std::string pointEntityClass = "info_target";
    std::string brushMaterial = vmf::kDefaultBlockMaterial;
    int zoneTrack = 0;  // start/end zone tools place zones for this track (0 = main, n = bonus n)

    void newMap();
    // .vmf, or a .bsp (decompiled; `path` becomes the .vmf beside it, unsaved).
    bool open(const std::string& file);
    bool save(const std::string& file);

    // snapshot the map before changing it, undo/redo walk these
    void checkpoint();
    bool undo();
    bool redo();
    // for edits made straight on `doc` (the inspector), marks it changed
    void touched();

    RayHit raycast(const glm::vec3& origin, const glm::vec3& dir) const;

    // place the tool's object against a surface hit (or in the air if nothing was hit). selects what it made
    void place(Tool tool, const glm::vec3& point, const glm::vec3& normal, float yawDeg);

    bool selectionBounds(glm::vec3& min, glm::vec3& max) const;
    // withCheckpoint = false for continuous edits (inspector drags) that
    // already called checkpoint() once when the drag started.
    void moveSelection(const glm::vec3& delta, bool withCheckpoint = true);
    void resizeSelection(const glm::vec3& newMin, const glm::vec3& newMax, bool withCheckpoint = true);
    // turns the selection about its own centre. point entities have no brushes to turn so this only moves their angles
    void rotateSelection(const glm::vec3& axis, float degrees, bool withCheckpoint = true);
    void deleteSelection();
    void duplicateSelection();
    void setSelectionMaterial(const std::string& material);

    // everything selected, anchor first, empty when nothing is
    std::vector<Selection> allSelected() const;
    int selectionCount() const;
    bool isSelected(const Selection& target) const;
    // ctrl+click adds it to the selection, or takes it back out if it was already in. the anchor follows whatever was added last
    void toggleSelected(const Selection& target);
    void clearSelection();
    // a click in the select tool. picks what was hit, or with groups honoured the whole group it's in. add = ctrl held, which puts it in the selection or takes it (and the rest of its group) back out
    void selectClicked(const Selection& target, bool add);
    // what a click on `target` actually selects: just it, or its whole group
    std::vector<Selection> clickTargets(const Selection& target) const;

    // grouping (ctrl+g / ctrl+u), the way hammer does it. group needs two or more things selected. ungroup breaks every group a selected object is in, not just the selected members, since a half intact group is never what you meant. each returns false when there was nothing to do
    bool groupSelection();
    bool ungroupSelection();
    int groupIdOf(const Selection& target) const;
    // everything in a group, in map order. skips hidden members, they're out of the mouse's way so out of the group's way too
    std::vector<Selection> groupMembers(int groupId) const;
    int groupCount() const;

    // hiding: out of the way without being deleted, and out of the mouse's way too, a hidden brush is neither drawn nor pickable. the mark lives in the map file so it survives saving and reopening
    void hideSelection();
    void unhide(const Selection& target);
    void unhideAll();
    bool isHidden(const Selection& target) const;
    int hiddenCount() const;
    // what's hidden, as something to show in a list plus the handle to put it back. readable labels, in map order
    std::vector<std::pair<std::string, Selection>> hiddenItems() const;

    // ctrl+c / ctrl+x / ctrl+v, over a whole world brush or a whole entity. paste puts the copy back exactly where it was taken from and selects it, like hammer, so the move you were gonna make anyway is the next thing you do and cut then paste puts it back untouched. the clipboard outlives opening another map so it carries between them. each returns false when there was nothing to act on
    bool copySelection();
    bool cutSelection();
    bool pasteClipboard();
    bool hasClipboard() const;

    // hammer's shift-drag: a copy sitting exactly on the original and selected, so dragging it away leaves the original untouched. separate from duplicateSelection (which offsets) and from the clipboard (which it must not disturb). false when nothing is selected
    bool duplicateSelectionInPlace();

    // bulk removal, for clearing out a decompiled map. each takes one checkpoint and returns how many it took out, so one ctrl+z puts the lot back and nothing says "removed 0"
    int removeEntitiesOfClass(const std::string& classname);
    // clip brushes wherever they are. vbsp files them under func_detail so a decompiled map keeps them inside a brush entity instead of the world. a brush entity left with no brushes goes with them
    int removeClipBrushes();

    // what the map actually holds, for the remove-all list: each classname with its count, sorted. clip brushes are counted separately since they're world brushes not entities
    std::vector<std::pair<std::string, int>> entityClassCounts() const;
    int clipBrushCount() const;
    // hammer's "tie to entity" (ctrl+t) and back
    void tieSelectionToEntity(const std::string& classname);
    void moveSelectionToWorld();

    // the map's light_environment (sun + ambient), if it has one
    vmf::Entity* sun();
    void addSun();
    void removeSun();

    // the map's env_fog_controller: distance fog, the one bit of atmosphere that costs nothing and changes how a whole map reads
    vmf::Entity* fog();
    void addFog();
    void removeFog();

    vmf::Entity* selectedEntity();
    vmf::Solid* selectedWorldSolid();

    float snap(float v) const;
    glm::vec3 snap(const glm::vec3& v) const;

    // point entities have no brushes so this is the box they're drawn and picked as (player sized for spawns)
    static bool pointEntityBounds(const vmf::Entity& e, glm::vec3& min, glm::vec3& max);

private:
    struct Clipboard {
        enum class Kind { None, WorldSolid, Entity };
        Kind kind = Kind::None;
        vmf::Solid solid;
        vmf::Entity entity;
    };

    void changed();
    glm::vec3 spawnFeet() const;
    std::string uniqueName(const std::string& prefix);
    void addBrushEntity(const std::string& classname, const glm::vec3& min, const glm::vec3& max,
                        std::vector<std::pair<std::string, std::string>> keyValues,
                        std::vector<std::pair<std::string, std::string>> connections = {});
    int findZone(const std::string& name) const;

    std::deque<vmf::Document> undo_;
    std::deque<vmf::Document> redo_;
    Clipboard clipboard_;
};

}  // namespace editor
