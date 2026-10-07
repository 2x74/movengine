#pragma once

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vmf {

// a raw vmf/keyvalues block, for everything the editor doesn't model (displacements, visgroups, hammer's camera/view settings, editor metadata). kept so open -> save round trips hammer files without loss
struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<Node> children;
};

struct Side {
    int id = 0;
    // hammer's plane: three points on the face, ordered so that (p0 - p1) x (p2 - p1) points out of the solid (vbsp's PlaneFromPoints)
    std::array<glm::vec3, 3> points{};
    std::string material = "DEV/DEV_MEASUREGENERIC01B";
    // texture projection: texel u = dot(p, uAxis) / uScale + uShift
    glm::vec3 uAxis{1.0f, 0.0f, 0.0f};
    float uShift = 0.0f;
    float uScale = 0.25f;
    glm::vec3 vAxis{0.0f, -1.0f, 0.0f};
    float vShift = 0.0f;
    float vScale = 0.25f;
    float rotation = 0.0f;
    int lightmapScale = 16;
    std::string smoothingGroups = "0";
    std::vector<Node> extra;  // dispinfo etc.
};

struct Solid {
    int id = 0;
    std::vector<Side> sides;
    std::vector<Node> extra;  // editor block etc.
};

struct Entity {
    int id = 0;
    // file order, original key case. classname, targetname, origin, angles, spawnflags... anything the fgd allows
    std::vector<std::pair<std::string, std::string>> keyValues;
    // outputs: name (OnStartTouch, ...) -> "target,input,param,delay,times"
    std::vector<std::pair<std::string, std::string>> connections;
    std::vector<Solid> solids;  // world space, empty for point entities
    std::vector<Node> extra;

    std::string get(std::string_view key, const std::string& fallback = "") const;  // case-insensitive
    void set(std::string_view key, const std::string& value);
    std::string classname() const { return get("classname"); }
};

struct Document {
    Entity world;  // classname "worldspawn": skyname etc., solids = world brushes
    std::vector<Entity> entities;
    std::vector<Node> extra;  // versioninfo, visgroups, viewsettings, cameras, cordons
    int nextId = 1;

    int allocateId() { return nextId++; }
};

// the editor's "hidden" mark: a brush put out of the way without deleting it, neither drawn nor pickable. kept inside the solid's own editor block, which round trips through save and reopen like the rest of `extra`, and which hammer ignores as an unknown key instead of choking on
bool isHidden(const Solid& solid);
bool isHidden(const Entity& entity);
void setHidden(Solid& solid, bool hidden);
void setHidden(Entity& entity, bool hidden);

// grouping, hammer's: a handful of objects that click, move and turn as one thing. each member carries the group's id in that same editor block, under the key hammer itself uses ("groupid"), and the group has a `group` block of its own inside `world`, so a group made here opens as a group in hammer and a map grouped in hammer arrives grouped here. 0 means ungrouped
int groupId(const Solid& solid);
int groupId(const Entity& entity);
void setGroupId(Solid& solid, int id);
void setGroupId(Entity& entity, int id);
// brings the `group` blocks back in line with the ids actually in use: drops the ones nothing references any more, adds any that are missing, and ungroups a group left holding a single member since one object on its own isn't a group. called after anything that groups, ungroups or deletes
void tidyGroups(Document& doc);

// css defaults for a new map: worldspawn with a sky, nothing else
Document emptyDocument();

std::optional<Document> loadDocument(const std::string& path);
bool saveDocument(const std::string& path, const Document& doc);

// parse/emit straight from text (used by load/save, exposed for tests)
std::optional<Document> parseDocument(std::string_view text);
std::string writeDocument(const Document& doc);

// "1.5" / "-3" style, no trailing zeros, the way Hammer writes numbers.
std::string formatNumber(float v);
std::string formatVec3(const glm::vec3& v);  // "x y z"

}  // namespace vmf
