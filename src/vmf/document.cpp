#include "vmf/document.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace vmf {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// keyvalues text -> node tree

class Tokenizer {
public:
    explicit Tokenizer(std::string_view text) : text_(text) {}

    // returns false at end of input. braces come back as "{" / "}" with quoted == false so a quoted "{" string stays distinguishable
    bool next(std::string& token, bool& quoted) {
        skipSpaceAndComments();
        if (pos_ >= text_.size()) return false;
        char c = text_[pos_];
        quoted = false;
        if (c == '{' || c == '}') {
            token.assign(1, c);
            ++pos_;
            return true;
        }
        if (c == '"') {
            size_t end = text_.find('"', pos_ + 1);
            if (end == std::string_view::npos) end = text_.size();
            token.assign(text_.substr(pos_ + 1, end - pos_ - 1));
            pos_ = end + 1;
            quoted = true;
            return true;
        }
        size_t start = pos_;
        while (pos_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[pos_])) && text_[pos_] != '{' &&
               text_[pos_] != '}' && text_[pos_] != '"') {
            ++pos_;
        }
        token.assign(text_.substr(start, pos_ - start));
        return true;
    }

private:
    void skipSpaceAndComments() {
        while (pos_ < text_.size()) {
            if (std::isspace(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            } else if (text_.compare(pos_, 2, "//") == 0) {
                size_t nl = text_.find('\n', pos_);
                pos_ = nl == std::string_view::npos ? text_.size() : nl + 1;
            } else {
                break;
            }
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
};

// parses block contents up to the matching "}" (or end of input at top level)
void parseBlockContents(Tokenizer& tok, Node& into) {
    std::string key, value;
    bool keyQuoted = false, valueQuoted = false;
    while (tok.next(key, keyQuoted)) {
        if (!keyQuoted && key == "}") return;
        if (!keyQuoted && key == "{") continue;  // stray brace
        if (!tok.next(value, valueQuoted)) return;
        if (!valueQuoted && value == "{") {
            Node child;
            child.name = key;
            parseBlockContents(tok, child);
            into.children.push_back(std::move(child));
        } else if (!valueQuoted && value == "}") {
            return;  // a key with no value right before the close
        } else {
            into.values.emplace_back(key, value);
        }
    }
}

// node tree -> typed model

float toFloat(const std::string& s, float fallback = 0.0f) {
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

int toInt(const std::string& s, int fallback = 0) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? fallback : static_cast<int>(v);
}

// "(x y z) (x y z) (x y z)"
bool parsePlane(const std::string& s, std::array<glm::vec3, 3>& out) {
    std::string cleaned = s;
    for (char& c : cleaned) {
        if (c == '(' || c == ')') c = ' ';
    }
    std::istringstream iss(cleaned);
    for (auto& p : out) {
        if (!(iss >> p.x >> p.y >> p.z)) return false;
    }
    return true;
}

// "[x y z shift] scale"
void parseAxis(const std::string& s, glm::vec3& axis, float& shift, float& scale) {
    std::string cleaned = s;
    for (char& c : cleaned) {
        if (c == '[' || c == ']') c = ' ';
    }
    std::istringstream iss(cleaned);
    glm::vec3 a;
    float sh = 0.0f, sc = 0.25f;
    if (iss >> a.x >> a.y >> a.z >> sh) {
        axis = a;
        shift = sh;
        if (iss >> sc) scale = sc;
    }
}

// tracks the highest id seen so new objects get fresh ones
int readId(const std::string& s, int& maxId) {
    int id = toInt(s);
    maxId = std::max(maxId, id);
    return id;
}

Side toSide(const Node& n, int& maxId) {
    Side side;
    for (const auto& [k, v] : n.values) {
        if (iequals(k, "id")) side.id = readId(v, maxId);
        else if (iequals(k, "plane")) parsePlane(v, side.points);
        else if (iequals(k, "material")) side.material = v;
        else if (iequals(k, "uaxis")) parseAxis(v, side.uAxis, side.uShift, side.uScale);
        else if (iequals(k, "vaxis")) parseAxis(v, side.vAxis, side.vShift, side.vScale);
        else if (iequals(k, "rotation")) side.rotation = toFloat(v);
        else if (iequals(k, "lightmapscale")) side.lightmapScale = toInt(v, 16);
        else if (iequals(k, "smoothing_groups")) side.smoothingGroups = v;
    }
    side.extra = n.children;
    return side;
}

Solid toSolid(const Node& n, int& maxId) {
    Solid solid;
    for (const auto& [k, v] : n.values) {
        if (iequals(k, "id")) solid.id = readId(v, maxId);
    }
    for (const auto& c : n.children) {
        if (iequals(c.name, "side")) solid.sides.push_back(toSide(c, maxId));
        else solid.extra.push_back(c);
    }
    return solid;
}

Entity toEntity(const Node& n, int& maxId) {
    Entity e;
    for (const auto& [k, v] : n.values) {
        if (iequals(k, "id")) e.id = readId(v, maxId);
        else e.keyValues.emplace_back(k, v);
    }
    for (const auto& c : n.children) {
        if (iequals(c.name, "solid")) {
            e.solids.push_back(toSolid(c, maxId));
        } else if (iequals(c.name, "connections")) {
            e.connections.insert(e.connections.end(), c.values.begin(), c.values.end());
        } else if (iequals(c.name, "hidden")) {
            // hammer's hide tool wraps objects in "hidden", they're still part of the map so unwrap instead of losing them
            for (const auto& h : c.children) {
                if (iequals(h.name, "solid")) e.solids.push_back(toSolid(h, maxId));
            }
        } else {
            e.extra.push_back(c);
        }
    }
    return e;
}

// typed model -> text

class Writer {
public:
    void open(const std::string& name) {
        line(name);
        line("{");
        ++depth_;
    }
    void close() {
        --depth_;
        line("}");
    }
    void kv(const std::string& k, const std::string& v) { line("\"" + k + "\" \"" + v + "\""); }
    void node(const Node& n) {
        open(n.name);
        for (const auto& [k, v] : n.values) kv(k, v);
        for (const auto& c : n.children) node(c);
        close();
    }
    std::string str() const { return out_.str(); }

private:
    void line(const std::string& s) {
        for (int i = 0; i < depth_; ++i) out_ << '\t';
        out_ << s << "\r\n";  // hammer writes CRLF
    }
    std::ostringstream out_;
    int depth_ = 0;
};

std::string formatAxis(const glm::vec3& axis, float shift, float scale) {
    return "[" + formatVec3(axis) + " " + formatNumber(shift) + "] " + formatNumber(scale);
}

void writeSolid(Writer& w, const Solid& solid) {
    w.open("solid");
    w.kv("id", std::to_string(solid.id));
    for (const auto& side : solid.sides) {
        w.open("side");
        w.kv("id", std::to_string(side.id));
        w.kv("plane", "(" + formatVec3(side.points[0]) + ") (" + formatVec3(side.points[1]) + ") (" +
                          formatVec3(side.points[2]) + ")");
        w.kv("material", side.material);
        w.kv("uaxis", formatAxis(side.uAxis, side.uShift, side.uScale));
        w.kv("vaxis", formatAxis(side.vAxis, side.vShift, side.vScale));
        w.kv("rotation", formatNumber(side.rotation));
        w.kv("lightmapscale", std::to_string(side.lightmapScale));
        w.kv("smoothing_groups", side.smoothingGroups);
        for (const auto& e : side.extra) w.node(e);
        w.close();
    }
    for (const auto& e : solid.extra) w.node(e);
    w.close();
}

void writeEntity(Writer& w, const char* blockName, const Entity& e) {
    w.open(blockName);
    w.kv("id", std::to_string(e.id));
    for (const auto& [k, v] : e.keyValues) w.kv(k, v);
    if (!e.connections.empty()) {
        w.open("connections");
        for (const auto& [k, v] : e.connections) w.kv(k, v);
        w.close();
    }
    for (const auto& s : e.solids) writeSolid(w, s);
    for (const auto& x : e.extra) w.node(x);
    w.close();
}

}  // namespace

std::string Entity::get(std::string_view key, const std::string& fallback) const {
    for (const auto& [k, v] : keyValues) {
        if (iequals(k, key)) return v;
    }
    return fallback;
}

void Entity::set(std::string_view key, const std::string& value) {
    for (auto& [k, v] : keyValues) {
        if (iequals(k, key)) {
            v = value;
            return;
        }
    }
    keyValues.emplace_back(std::string(key), value);
}

std::string formatNumber(float v) {
    if (v == 0.0f) return "0";  // also folds -0
    if (std::abs(v - std::round(v)) < 1e-4f && std::abs(v) < 1e7f) {
        return std::to_string(static_cast<long long>(std::llround(v)));
    }
    // the shortest text that reads back as exactly this float: "0.25" stays short and big coordinates keep every digit (6 digits turned -15709.43 into -15709.4, enough to move a surf ramp's edge by units)
    char buf[32];
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

std::string formatVec3(const glm::vec3& v) {
    return formatNumber(v.x) + " " + formatNumber(v.y) + " " + formatNumber(v.z);
}

namespace {

constexpr const char* kEditorNode = "editor";
constexpr const char* kHiddenKey = "movengine_hidden";
constexpr const char* kGroupKey = "groupid";
constexpr const char* kGroupNode = "group";

// hammer keeps an object's editor metadata in an "editor" child block. both the hidden mark and the group id live in there so they survive save and reopen, and hammer either reads them as its own ("groupid") or passes them through as a key it doesn't know ("movengine_hidden")
const std::string* editorValue(const std::vector<Node>& extra, const char* key) {
    for (const auto& node : extra) {
        if (node.name != kEditorNode) continue;
        for (const auto& [k, v] : node.values) {
            if (k == key) return &v;
        }
    }
    return nullptr;
}

// an empty value takes the key out again: for both of these absent is the default, so an unhidden and ungrouped object carries nothing extra
void setEditorValue(std::vector<Node>& extra, const char* key, const std::string& value) {
    for (auto& node : extra) {
        if (node.name != kEditorNode) continue;
        for (auto it = node.values.begin(); it != node.values.end(); ++it) {
            if (it->first != key) continue;
            if (value.empty()) node.values.erase(it);
            else it->second = value;
            return;
        }
        if (!value.empty()) node.values.emplace_back(key, value);
        return;
    }
    // no editor block at all (a brush this editor made): add one just for this
    if (value.empty()) return;
    Node node;
    node.name = kEditorNode;
    node.values.emplace_back(key, value);
    extra.push_back(std::move(node));
}

bool editorFlag(const std::vector<Node>& extra, const char* key) {
    const std::string* v = editorValue(extra, key);
    return v != nullptr && *v == "1";
}

int editorId(const std::vector<Node>& extra, const char* key) {
    const std::string* v = editorValue(extra, key);
    return v != nullptr ? toInt(*v) : 0;
}

// the id out of a `group { "id" "N" ... }` block
int groupNodeId(const Node& node) {
    for (const auto& [k, v] : node.values) {
        if (iequals(k, "id")) return toInt(v);
    }
    return 0;
}

}  // namespace

bool isHidden(const Solid& solid) { return editorFlag(solid.extra, kHiddenKey); }
bool isHidden(const Entity& entity) { return editorFlag(entity.extra, kHiddenKey); }
void setHidden(Solid& solid, bool hidden) { setEditorValue(solid.extra, kHiddenKey, hidden ? "1" : ""); }
void setHidden(Entity& entity, bool hidden) { setEditorValue(entity.extra, kHiddenKey, hidden ? "1" : ""); }

int groupId(const Solid& solid) { return editorId(solid.extra, kGroupKey); }
int groupId(const Entity& entity) { return editorId(entity.extra, kGroupKey); }

void setGroupId(Solid& solid, int id) {
    setEditorValue(solid.extra, kGroupKey, id > 0 ? std::to_string(id) : "");
}

void setGroupId(Entity& entity, int id) {
    setEditorValue(entity.extra, kGroupKey, id > 0 ? std::to_string(id) : "");
}

void tidyGroups(Document& doc) {
    std::map<int, int> members;
    for (const auto& s : doc.world.solids) {
        if (int g = groupId(s); g > 0) ++members[g];
    }
    for (const auto& e : doc.entities) {
        if (int g = groupId(e); g > 0) ++members[g];
    }
    auto survives = [&](int g) {
        auto it = members.find(g);
        return it != members.end() && it->second >= 2;
    };
    // a group of one isn't a group: deleting all but one member leaves something that still clicks as a group but moves alone, which reads as the editor being broken
    for (auto& s : doc.world.solids) {
        if (int g = groupId(s); g > 0 && !survives(g)) setGroupId(s, 0);
    }
    for (auto& e : doc.entities) {
        if (int g = groupId(e); g > 0 && !survives(g)) setGroupId(e, 0);
    }
    // the `group` blocks themselves. hammer's own carry a colour we keep, so only the ones nothing references go
    std::erase_if(doc.world.extra, [&](const Node& n) {
        return iequals(n.name, kGroupNode) && !survives(groupNodeId(n));
    });
    for (const auto& [g, count] : members) {
        if (count < 2) continue;
        bool have = false;
        for (const auto& n : doc.world.extra) {
            if (iequals(n.name, kGroupNode) && groupNodeId(n) == g) {
                have = true;
                break;
            }
        }
        if (have) continue;
        Node group;
        group.name = kGroupNode;
        group.values.emplace_back("id", std::to_string(g));
        Node meta;
        meta.name = kEditorNode;
        meta.values.emplace_back("color", "0 180 0");
        meta.values.emplace_back("visgroupshown", "1");
        meta.values.emplace_back("visgroupautoshown", "1");
        group.children.push_back(std::move(meta));
        doc.world.extra.push_back(std::move(group));
    }
}

Document emptyDocument() {
    Document doc;
    doc.world.id = doc.allocateId();
    doc.world.keyValues = {
        {"mapversion", "1"},
        {"classname", "worldspawn"},
        {"skyname", "sky_day01_01"},
        {"maxpropscreenwidth", "-1"},
        {"detailvbsp", "detail.vbsp"},
        {"detailmaterial", "detail/detailsprites"},
    };
    doc.extra.push_back({"versioninfo",
                         {{"editorversion", "400"}, {"editorbuild", "0"}, {"mapversion", "1"},
                          {"formatversion", "100"}, {"prefab", "0"}},
                         {}});
    doc.extra.push_back({"visgroups", {}, {}});
    doc.extra.push_back({"viewsettings",
                         {{"bSnapToGrid", "1"}, {"bShowGrid", "1"}, {"bShowLogicalGrid", "0"},
                          {"nGridSpacing", "16"}, {"bShow3DGrid", "0"}},
                         {}});
    return doc;
}

std::optional<Document> parseDocument(std::string_view text) {
    Node root;
    Tokenizer tok(text);
    parseBlockContents(tok, root);

    int maxId = 0;
    Document doc;
    bool sawWorld = false;
    for (const auto& n : root.children) {
        if (iequals(n.name, "world")) {
            doc.world = toEntity(n, maxId);
            sawWorld = true;
        } else if (iequals(n.name, "entity")) {
            doc.entities.push_back(toEntity(n, maxId));
        } else {
            doc.extra.push_back(n);
        }
    }
    if (!sawWorld) {
        return std::nullopt;
    }
    // group blocks sit among the world's extra nodes, unparsed, but their ids come out of the same pot as every other object's, so a new brush must not be handed one of them
    for (const auto& n : doc.world.extra) {
        if (!iequals(n.name, "group")) continue;
        for (const auto& [k, v] : n.values) {
            if (iequals(k, "id")) maxId = std::max(maxId, toInt(v));
        }
    }
    doc.nextId = maxId + 1;
    return doc;
}

std::string writeDocument(const Document& doc) {
    Writer w;
    // hammer's order: versioninfo/visgroups/viewsettings, world, entities, then cameras/cordons
    auto isTrailer = [](const Node& n) { return iequals(n.name, "cameras") || iequals(n.name, "cordon") || iequals(n.name, "cordons"); };
    for (const auto& n : doc.extra) {
        if (!isTrailer(n)) w.node(n);
    }
    writeEntity(w, "world", doc.world);
    for (const auto& e : doc.entities) writeEntity(w, "entity", e);
    for (const auto& n : doc.extra) {
        if (isTrailer(n)) w.node(n);
    }
    return w.str();
}

std::optional<Document> loadDocument(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "failed to open vmf: %s\n", path.c_str());
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto doc = parseDocument(buffer.str());
    if (!doc) {
        std::fprintf(stderr, "not a vmf (no world block): %s\n", path.c_str());
    }
    return doc;
}

bool saveDocument(const std::string& path, const Document& doc) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "failed to write vmf: %s\n", path.c_str());
        return false;
    }
    file << writeDocument(doc);
    return static_cast<bool>(file);
}

}  // namespace vmf
