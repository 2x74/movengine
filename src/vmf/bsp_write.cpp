#include "vmf/bsp_write.h"

#include <bsppp/BSP.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <unordered_map>
#include <utility>

#include "collision/map_triggers.h"
#include "vmf/geometry.h"

namespace vmf {

namespace {

// on disk lump structures: the source bsp v20 layouts, which is what css writes and what src/bsp and src/collision already read back. every size below was checked against a real css map by dividing its lump length by the struct size (BRUSHES/12 giving exactly the brush count the game reports for that map, and so on). see docs/bsp-compile.md
#pragma pack(push, 1)

struct DPlane {
    float normal[3];
    float dist;
    int32_t type;  // PLANE_ANYZ etc; the engine recomputes what it needs
};

struct DVertex {
    float point[3];
};

struct DEdge {
    uint16_t v[2];
};

struct DFace {
    uint16_t planeNum;
    uint8_t side;    // 1 when the face looks down the plane's back
    uint8_t onNode;  // 0: this face is in a leaf, not split across a node
    int32_t firstEdge;
    int16_t numEdges;
    int16_t texInfo;
    int16_t dispInfo;
    int16_t surfaceFogVolumeID;
    uint8_t styles[4];
    int32_t lightOffset;
    float area;
    int32_t lightmapMins[2];
    int32_t lightmapSize[2];  // extent, so luxel count is this + 1
    int32_t originalFace;
    uint16_t numPrims;
    uint16_t firstPrimID;
    uint32_t smoothingGroups;
};

struct DTexInfo {
    float textureVecs[2][4];   // texel = dot(point, vec.xyz) + vec.w
    float lightmapVecs[2][4];  // luxel, same form
    int32_t flags;
    int32_t texData;
};

struct DTexData {
    float reflectivity[3];
    int32_t nameStringTableID;
    int32_t width, height;
    int32_t viewWidth, viewHeight;
};

struct DNode {
    int32_t planeNum;
    int32_t children[2];  // >= 0 a node, else leaf -(child + 1)
    int16_t mins[3];
    int16_t maxs[3];
    uint16_t firstFace;
    uint16_t numFaces;
    int16_t area;
    int16_t padding;
};

struct DLeaf {
    int32_t contents;
    int16_t cluster;
    int16_t areaFlags;  // area:9, flags:7
    int16_t mins[3];
    int16_t maxs[3];
    uint16_t firstLeafFace, numLeafFaces;
    uint16_t firstLeafBrush, numLeafBrushes;
    int16_t leafWaterDataID;
    int16_t padding;
};

struct DBrush {
    int32_t firstSide;
    int32_t numSides;
    int32_t contents;
};

struct DBrushSide {
    uint16_t planeNum;
    int16_t texInfo;
    int16_t dispInfo;
    int16_t bevel;
};

struct DModel {
    float mins[3], maxs[3];
    float origin[3];
    int32_t headNode;
    int32_t firstFace, numFaces;
};

struct DArea {
    int32_t numAreaPortals;
    int32_t firstAreaPortal;
};

struct DLeafAmbientIndex {
    uint16_t ambientSampleCount;
    uint16_t firstAmbientSample;
};

struct DLeafAmbientLighting {
    uint8_t cube[6][4];  // ColorRGBExp32 per cube face
    uint8_t x, y, z, pad;
};

#pragma pack(pop)

static_assert(sizeof(DPlane) == 20);
static_assert(sizeof(DVertex) == 12);
static_assert(sizeof(DEdge) == 4);
static_assert(sizeof(DFace) == 56);
static_assert(sizeof(DTexInfo) == 72);
static_assert(sizeof(DTexData) == 32);
static_assert(sizeof(DNode) == 32);
static_assert(sizeof(DLeaf) == 32);
static_assert(sizeof(DBrush) == 12);
static_assert(sizeof(DBrushSide) == 8);
static_assert(sizeof(DModel) == 48);
static_assert(sizeof(DArea) == 8);
static_assert(sizeof(DLeafAmbientIndex) == 4);
static_assert(sizeof(DLeafAmbientLighting) == 28);

// bspflags.h, the handful we set.
constexpr int32_t kContentsEmpty = 0;
constexpr int32_t kContentsSolid = 0x1;
constexpr int32_t kContentsPlayerClip = 0x10000;

constexpr int32_t kSurfSky = 0x4;
constexpr int32_t kSurfNoDraw = 0x80;
constexpr int32_t kSurfSkip = 0x200;
constexpr int32_t kSurfNoLight = 0x400;

// vbsp clamps a face's lightmap to 32x32 luxels before it starts subdividing the face. we don't subdivide so clamping is all we can do: a face bigger than its lightmap just gets a stretched one, which for a flat fullbright lightmap makes no visible difference anyway
constexpr int kMaxLuxels = 32;

// the bsp map version and per lump versions a css map carries. mismatched lump versions aren't cosmetic: bsppp (and the engine) pick which struct layout to parse from them and a wrong one is silently misread
constexpr uint32_t kMapVersion = 20;
constexpr uint32_t kFacesLumpVersion = 1;
constexpr uint32_t kLeafsLumpVersion = 1;
constexpr uint32_t kLightingLumpVersion = 1;
constexpr uint32_t kLeafAmbientLumpVersion = 1;

// "VBSP", the map version, 64 lump directory entries, then the map revision.
constexpr size_t kLumpCount = 64;
constexpr size_t kHeaderSize = 4 + 4 + kLumpCount * 16 + 4;

struct LumpEntry {
    int32_t offset = 0;
    int32_t length = 0;
    int32_t version = 0;
};

template <typename T>
std::span<const std::byte> asBytes(const std::vector<T>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size() * sizeof(T)};
}

int16_t toShortClamped(float v) {
    return static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
}

std::string upperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// what a tool material means for the brush it's on, and for its faces
struct MaterialTraits {
    bool skipEntirely = false;  // hint/skip: compile-time only, never shipped
    bool noDraw = false;
    bool sky = false;
    int32_t contents = kContentsSolid;
};

MaterialTraits materialTraits(const std::string& material) {
    MaterialTraits t;
    std::string m = upperAscii(material);
    auto is = [&](const char* name) { return m == name; };
    if (is("TOOLS/TOOLSHINT") || is("TOOLS/TOOLSSKIP")) {
        t.skipEntirely = true;
    } else if (is("TOOLS/TOOLSNODRAW")) {
        t.noDraw = true;
    } else if (is("TOOLS/TOOLSSKYBOX") || is("TOOLS/TOOLSSKYBOX2D")) {
        t.sky = true;
    } else if (is("TOOLS/TOOLSPLAYERCLIP")) {
        t.noDraw = true;
        t.contents = kContentsPlayerClip;
    } else if (is("TOOLS/TOOLSCLIP") || is("TOOLS/TOOLSNPCCLIP")) {
        t.noDraw = true;
    } else if (is("TOOLS/TOOLSTRIGGER") || is("TOOLS/TOOLSINVISIBLE") || is("TOOLS/TOOLSBLACK")) {
        // trigger volumes keep solid contents in their own brush model, it's the entity around them that makes them a trigger instead of a wall
        t.noDraw = true;
    }
    return t;
}

// the lump builder. everything is accumulated here, then handed to bsppp
class BspBuilder {
public:
    explicit BspBuilder(const TextureSizeLookup& textureSize) : textureSize_(textureSize) {}

    // planes come in (even, odd) pairs sharing a surface with opposite normals, the way vbsp emits them, so flipping a plane is index ^ 1
    uint16_t addPlane(const glm::vec3& normal, float dist) {
        glm::vec3 n = normal;
        float d = dist;
        bool flipped = false;
        // canonical orientation: first significant component positive
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(n[i]) > 1e-5f) {
                if (n[i] < 0.0f) flipped = true;
                break;
            }
        }
        if (flipped) {
            n = -n;
            d = -d;
        }
        Key key{quantize(n.x), quantize(n.y), quantize(n.z), quantize(d)};
        auto it = planeIndex_.find(key);
        size_t base;
        if (it != planeIndex_.end()) {
            base = it->second;
        } else {
            base = planes_.size();
            planeIndex_.emplace(key, base);
            planes_.push_back(makePlane(n, d));
            planes_.push_back(makePlane(-n, -d));
        }
        return static_cast<uint16_t>(flipped ? base + 1 : base);
    }

    uint16_t addVertex(const glm::vec3& p) {
        Key key{quantize(p.x), quantize(p.y), quantize(p.z), 0};
        auto it = vertexIndex_.find(key);
        if (it != vertexIndex_.end()) return it->second;
        auto index = static_cast<uint16_t>(vertices_.size());
        vertexIndex_.emplace(key, index);
        vertices_.push_back(DVertex{{p.x, p.y, p.z}});
        return index;
    }

    // an edge is shared by the two faces either side of it, the second one walking it backwards, that's what a negative surfedge means
    int32_t addSurfEdge(uint16_t a, uint16_t b) {
        auto forward = edgeIndex_.find({a, b});
        if (forward != edgeIndex_.end()) return static_cast<int32_t>(forward->second);
        auto backward = edgeIndex_.find({b, a});
        if (backward != edgeIndex_.end()) return -static_cast<int32_t>(backward->second);
        auto index = static_cast<uint32_t>(edges_.size());
        edges_.push_back(DEdge{{a, b}});
        edgeIndex_.emplace(std::pair<uint16_t, uint16_t>{a, b}, index);
        return static_cast<int32_t>(index);
    }

    int32_t addTexData(const std::string& material) {
        auto it = texDataIndex_.find(material);
        if (it != texDataIndex_.end()) return it->second;
        glm::ivec2 size = textureSize_ ? textureSize_(material) : glm::ivec2(0);
        if (size.x <= 0 || size.y <= 0) size = glm::ivec2(512, 512);

        auto nameId = static_cast<int32_t>(texDataStringTable_.size());
        texDataStringTable_.push_back(static_cast<int32_t>(texDataStringData_.size()));
        // the engine looks materials up under materials/<name>.vmt so the stored name carries neither that prefix nor the extension
        texDataStringData_.insert(texDataStringData_.end(), material.begin(), material.end());
        texDataStringData_.push_back('\0');

        auto index = static_cast<int32_t>(texDatas_.size());
        texDatas_.push_back(DTexData{{0.5f, 0.5f, 0.5f}, nameId, size.x, size.y, size.x, size.y});
        texDataIndex_.emplace(material, index);
        return index;
    }

    int16_t addTexInfo(const Side& side, int32_t flags) {
        DTexInfo info{};
        const float uScale = side.uScale != 0.0f ? side.uScale : 0.25f;
        const float vScale = side.vScale != 0.0f ? side.vScale : 0.25f;
        // texel u = dot(point, uAxis) / uScale + uShift
        for (int i = 0; i < 3; ++i) {
            info.textureVecs[0][i] = side.uAxis[i] / uScale;
            info.textureVecs[1][i] = side.vAxis[i] / vScale;
        }
        info.textureVecs[0][3] = side.uShift;
        info.textureVecs[1][3] = side.vShift;

        // luxels run along the same axes, one per lightmapScale world units. texel 0 sits at dot(point, axis) == -uShift * uScale, so that same world offset becomes the luxel shift once divided through
        const float lmScale = side.lightmapScale > 0 ? static_cast<float>(side.lightmapScale) : 16.0f;
        for (int i = 0; i < 3; ++i) {
            info.lightmapVecs[0][i] = side.uAxis[i] / lmScale;
            info.lightmapVecs[1][i] = side.vAxis[i] / lmScale;
        }
        info.lightmapVecs[0][3] = side.uShift * uScale / lmScale;
        info.lightmapVecs[1][3] = side.vShift * vScale / lmScale;

        info.flags = flags;
        info.texData = addTexData(side.material);

        // texinfos repeat constantly, every face of a block that shares a material and projection is the same one
        std::string key(reinterpret_cast<const char*>(&info), sizeof(info));
        auto it = texInfoIndex_.find(key);
        if (it != texInfoIndex_.end()) return it->second;
        auto index = static_cast<int16_t>(texInfos_.size());
        texInfos_.push_back(info);
        texInfoIndex_.emplace(std::move(key), index);
        return index;
    }

    // emits one face, returning false when the polygon was unusable
    bool addFace(const std::vector<glm::vec3>& polygon, const glm::vec3& normal, float dist, const Side& side,
                 const MaterialTraits& traits) {
        if (polygon.size() < 3) return false;

        int32_t flags = 0;
        if (traits.noDraw) flags |= kSurfNoDraw | kSurfNoLight;
        if (traits.sky) flags |= kSurfSky | kSurfNoLight;

        DFace face{};
        face.planeNum = addPlane(normal, dist);
        // addPlane hands back the index whose normal matches, so the face
        // always looks along its plane's front.
        face.side = 0;
        face.onNode = 0;
        face.texInfo = addTexInfo(side, flags);
        face.dispInfo = -1;
        face.surfaceFogVolumeID = -1;
        face.smoothingGroups = 0;
        face.numPrims = 0;
        face.firstPrimID = 0;

        // real maps store the loop running clockwise seen from the front so its own winding normal is the negation of the plane's. verified across every non displacement face of a shipped css map, emitting the other handedness makes the engine cull the world inside out
        std::vector<glm::vec3> loop = polygon;
        std::reverse(loop.begin(), loop.end());

        face.firstEdge = static_cast<int32_t>(surfEdges_.size());
        for (size_t i = 0; i < loop.size(); ++i) {
            uint16_t a = addVertex(loop[i]);
            uint16_t b = addVertex(loop[(i + 1) % loop.size()]);
            if (a == b) continue;  // a duplicated point, not an edge
            surfEdges_.push_back(addSurfEdge(a, b));
        }
        face.numEdges = static_cast<int16_t>(surfEdges_.size() - face.firstEdge);
        if (face.numEdges < 3) {
            surfEdges_.resize(static_cast<size_t>(face.firstEdge));
            return false;
        }

        face.area = polygonArea(loop);
        addLightmap(face, loop, texInfos_[face.texInfo], traits);

        faces_.push_back(face);
        return true;
    }

    // one "everything is in front of me" plane, so each model's headnode is a real node: its front child is that model's leaf, its back child the shared solid leaf. see the note in bsp_write.h about there being no visibility tree here
    void beginTree(const glm::vec3& worldMin) {
        // leaf 0 is the solid leaf every source map starts with
        DLeaf solid{};
        solid.contents = kContentsSolid;
        solid.cluster = -1;
        solid.leafWaterDataID = -1;
        leaves_.push_back(solid);
        outsidePlane_ = addPlane(glm::vec3(0.0f, 0.0f, 1.0f), worldMin.z - 4096.0f);
    }

    // wraps the faces and brushes already added for one model into a node and a leaf, and returns the node index for DModel::headNode
    int32_t addModelTree(int32_t firstFace, int32_t numFaces, int32_t firstBrush, int32_t numBrushes,
                         const glm::vec3& mins, const glm::vec3& maxs, bool worldspawn) {
        DLeaf leaf{};
        leaf.contents = kContentsEmpty;
        // only the world leaf is in the vis tree, brush model leaves aren't, same as a real compile
        leaf.cluster = worldspawn ? 0 : -1;
        leaf.areaFlags = 0;
        leaf.leafWaterDataID = -1;
        for (int i = 0; i < 3; ++i) {
            leaf.mins[i] = toShortClamped(std::floor(mins[i]));
            leaf.maxs[i] = toShortClamped(std::ceil(maxs[i]));
        }
        leaf.firstLeafFace = static_cast<uint16_t>(leafFaces_.size());
        for (int32_t i = 0; i < numFaces; ++i) leafFaces_.push_back(static_cast<uint16_t>(firstFace + i));
        leaf.numLeafFaces = static_cast<uint16_t>(numFaces);
        leaf.firstLeafBrush = static_cast<uint16_t>(leafBrushes_.size());
        for (int32_t i = 0; i < numBrushes; ++i) leafBrushes_.push_back(static_cast<uint16_t>(firstBrush + i));
        leaf.numLeafBrushes = static_cast<uint16_t>(numBrushes);

        auto leafIndex = static_cast<int32_t>(leaves_.size());
        leaves_.push_back(leaf);

        DNode node{};
        node.planeNum = outsidePlane_;
        node.children[0] = -(leafIndex + 1);  // front: the whole model
        node.children[1] = -1;                // back: the solid leaf
        for (int i = 0; i < 3; ++i) {
            node.mins[i] = leaf.mins[i];
            node.maxs[i] = leaf.maxs[i];
        }
        node.firstFace = static_cast<uint16_t>(firstFace);
        node.numFaces = static_cast<uint16_t>(numFaces);
        node.area = 0;
        auto nodeIndex = static_cast<int32_t>(nodes_.size());
        nodes_.push_back(node);
        return nodeIndex;
    }

    int32_t addBrush(const std::vector<Plane>& planes, const std::vector<Side>& sides, int32_t contents) {
        DBrush brush{};
        brush.contents = contents;
        brush.firstSide = static_cast<int32_t>(brushSides_.size());
        for (size_t i = 0; i < planes.size(); ++i) {
            DBrushSide bs{};
            bs.planeNum = addPlane(planes[i].normal, planes[i].dist);
            MaterialTraits traits = materialTraits(sides[i].material);
            int32_t flags = traits.noDraw ? (kSurfNoDraw | kSurfNoLight) : 0;
            if (traits.sky) flags |= kSurfSky | kSurfNoLight;
            bs.texInfo = addTexInfo(sides[i], flags);
            bs.dispInfo = -1;
            bs.bevel = 0;
            brushSides_.push_back(bs);
        }
        brush.numSides = static_cast<int32_t>(brushSides_.size() - brush.firstSide);
        auto index = static_cast<int32_t>(brushes_.size());
        brushes_.push_back(brush);
        return index;
    }

    void addModel(const glm::vec3& mins, const glm::vec3& maxs, const glm::vec3& origin, int32_t headNode,
                  int32_t firstFace, int32_t numFaces) {
        DModel m{};
        for (int i = 0; i < 3; ++i) {
            m.mins[i] = mins[i];
            m.maxs[i] = maxs[i];
            m.origin[i] = origin[i];
        }
        m.headNode = headNode;
        m.firstFace = firstFace;
        m.numFaces = numFaces;
        models_.push_back(m);
    }

    int32_t faceCount() const { return static_cast<int32_t>(faces_.size()); }
    int32_t brushCount() const { return static_cast<int32_t>(brushes_.size()); }
    int32_t modelCount() const { return static_cast<int32_t>(models_.size()); }

    bool write(const std::string& path, const std::string& entityText, BspWriteStats& stats, std::string& error);

private:
    using Key = std::array<int64_t, 4>;
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = 1469598103934665603ull;
            for (int64_t v : k) h = (h ^ static_cast<size_t>(v)) * 1099511628211ull;
            return h;
        }
    };
    struct PairHash {
        size_t operator()(const std::pair<uint16_t, uint16_t>& p) const {
            return (static_cast<size_t>(p.first) << 16) ^ p.second;
        }
    };

    // snap to 1/16 of a unit so the same corner shared by two brushes collapses to one vertex instead of two almost equal ones
    static int64_t quantize(float v) { return static_cast<int64_t>(std::llround(v * 16.0f)); }

    static DPlane makePlane(const glm::vec3& n, float d) {
        DPlane p{{n.x, n.y, n.z}, d, 3};
        // PLANE_X/Y/Z for the axial ones, which is what vbsp records, the engine takes axial planes down a cheaper path
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(n[i]) > 0.9999f) p.type = i;
        }
        return p;
    }

    static float polygonArea(const std::vector<glm::vec3>& loop) {
        glm::vec3 sum(0.0f);
        for (size_t i = 0; i < loop.size(); ++i) {
            sum += glm::cross(loop[i], loop[(i + 1) % loop.size()]);
        }
        return glm::length(sum) * 0.5f;
    }

    void addLightmap(DFace& face, const std::vector<glm::vec3>& loop, const DTexInfo& info,
                     const MaterialTraits& traits) {
        if (traits.noDraw || traits.sky) {
            // nothing draws it so it needs no luxels. styles[0] = 255 is how a face says "no lightmap", our own loader checks exactly that
            face.styles[0] = face.styles[1] = face.styles[2] = face.styles[3] = 255;
            face.lightOffset = -1;
            face.lightmapMins[0] = face.lightmapMins[1] = 0;
            face.lightmapSize[0] = face.lightmapSize[1] = 0;
            return;
        }

        float minS = 1e30f, minT = 1e30f, maxS = -1e30f, maxT = -1e30f;
        for (const glm::vec3& p : loop) {
            float s = p.x * info.lightmapVecs[0][0] + p.y * info.lightmapVecs[0][1] + p.z * info.lightmapVecs[0][2] +
                      info.lightmapVecs[0][3];
            float t = p.x * info.lightmapVecs[1][0] + p.y * info.lightmapVecs[1][1] + p.z * info.lightmapVecs[1][2] +
                      info.lightmapVecs[1][3];
            minS = std::min(minS, s);
            maxS = std::max(maxS, s);
            minT = std::min(minT, t);
            maxT = std::max(maxT, t);
        }

        int mins0 = static_cast<int>(std::floor(minS));
        int mins1 = static_cast<int>(std::floor(minT));
        int size0 = std::clamp(static_cast<int>(std::ceil(maxS)) - mins0, 0, kMaxLuxels);
        int size1 = std::clamp(static_cast<int>(std::ceil(maxT)) - mins1, 0, kMaxLuxels);

        face.styles[0] = 0;  // lightstyle 0, the one every static light uses
        face.styles[1] = face.styles[2] = face.styles[3] = 255;
        face.lightmapMins[0] = mins0;
        face.lightmapMins[1] = mins1;
        face.lightmapSize[0] = size0;
        face.lightmapSize[1] = size1;
        face.lightOffset = static_cast<int32_t>(lighting_.size());

        // flat fullbright: no vrad so every luxel is the same. ColorRGBExp32 is mantissa + a shared exponent so 255 at exponent 0 is white
        const int luxels = (size0 + 1) * (size1 + 1);
        for (int i = 0; i < luxels; ++i) {
            lighting_.push_back(255);
            lighting_.push_back(255);
            lighting_.push_back(255);
            lighting_.push_back(0);
        }
    }

    TextureSizeLookup textureSize_;

    std::vector<DPlane> planes_;
    std::vector<DVertex> vertices_;
    std::vector<DEdge> edges_;
    std::vector<int32_t> surfEdges_;
    std::vector<DFace> faces_;
    std::vector<DTexInfo> texInfos_;
    std::vector<DTexData> texDatas_;
    std::vector<DNode> nodes_;
    std::vector<DLeaf> leaves_;
    std::vector<uint16_t> leafFaces_;
    std::vector<uint16_t> leafBrushes_;
    std::vector<DBrush> brushes_;
    std::vector<DBrushSide> brushSides_;
    std::vector<DModel> models_;
    std::vector<uint8_t> lighting_;
    std::vector<char> texDataStringData_;
    std::vector<int32_t> texDataStringTable_;

    std::unordered_map<Key, size_t, KeyHash> planeIndex_;
    std::unordered_map<Key, uint16_t, KeyHash> vertexIndex_;
    std::unordered_map<std::pair<uint16_t, uint16_t>, uint32_t, PairHash> edgeIndex_;
    std::unordered_map<std::string, int16_t> texInfoIndex_;
    std::unordered_map<std::string, int32_t> texDataIndex_;

    uint16_t outsidePlane_ = 0;
};

bool BspBuilder::write(const std::string& path, const std::string& entityText, BspWriteStats& stats,
                       std::string& error) {
    if (faces_.empty()) {
        error = "nothing to compile: the map has no visible faces";
        return false;
    }
    // several lump fields are 16 bit so a map past these limits can't be expressed at all. say which one ran out instead of writing a file that silently wraps around and loads as nonsense. vbsp answers these by splitting faces and brushes up, we don't, so this is where we stop
    struct Limit {
        const char* what;
        size_t count, max;
    };
    for (const Limit& l : {
             Limit{"vertices", vertices_.size(), 65535},
             Limit{"planes", planes_.size(), 65535},
             Limit{"faces", faces_.size(), 65535},
             Limit{"brushes", brushes_.size(), 65535},
             Limit{"leaf faces", leafFaces_.size(), 65535},
             Limit{"leaf brushes", leafBrushes_.size(), 65535},
             Limit{"brush sides", brushSides_.size(), 65535},
             Limit{"texinfos", texInfos_.size(), 32767},
         }) {
        if (l.count > l.max) {
            error = std::string("too many ") + l.what + " for a v20 bsp (" + std::to_string(l.count) + " > " +
                    std::to_string(l.max) + "): compile this one with vbsp instead";
            return false;
        }
    }

    // visibility: one cluster that sees itself. a byte of 0x01 is a literal run in source's vis encoding (a 0 byte would start a zero run) so this reads back as "cluster 0 can see cluster 0"
    std::vector<std::byte> vis;
    auto pushVisInt = [&vis](int32_t v) {
        for (int i = 0; i < 4; ++i) vis.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
    };
    pushVisInt(1);   // numclusters
    pushVisInt(12);  // byteofs[0][0], the PVS
    pushVisInt(13);  // byteofs[0][1], the PAS
    vis.push_back(static_cast<std::byte>(0x01));
    vis.push_back(static_cast<std::byte>(0x01));

    // one area and no portals: func_areaportal would need them and we emit none, but every leaf still has to sit in an area that exists
    std::vector<DArea> areas{DArea{0, 0}};

    // an ambient sample per leaf, so models in the map aren't lit pitch black by a lump the engine expects vrad to have filled in
    std::vector<DLeafAmbientIndex> ambientIndex;
    std::vector<DLeafAmbientLighting> ambientLighting;
    for (size_t i = 0; i < leaves_.size(); ++i) {
        ambientIndex.push_back({1, static_cast<uint16_t>(ambientLighting.size())});
        DLeafAmbientLighting sample{};
        for (auto& cubeFace : sample.cube) {
            cubeFace[0] = cubeFace[1] = cubeFace[2] = 255;
            cubeFace[3] = 0;
        }
        sample.x = sample.y = sample.z = 128;
        ambientLighting.push_back(sample);
    }

    // ORIGINALFACES mirrors FACES: we never split a face so each one is its own original
    for (size_t i = 0; i < faces_.size(); ++i) faces_[i].originalFace = static_cast<int32_t>(i);
    std::vector<DFace> originalFaces = faces_;

    std::vector<int32_t> mapFlags{0};

    // the entity lump is plain text, NUL terminated
    std::vector<std::byte> entityBytes;
    entityBytes.reserve(entityText.size() + 1);
    for (char c : entityText) entityBytes.push_back(static_cast<std::byte>(c));
    entityBytes.push_back(std::byte{0});

    using L = bsppp::BSPLump;
    struct LumpWrite {
        L lump;
        uint32_t version;
        std::span<const std::byte> data;
    };
    const std::vector<LumpWrite> lumpWrites = {
        {L::ENTITIES, 0, {entityBytes.data(), entityBytes.size()}},
        {L::PLANES, 0, asBytes(planes_)},
        {L::TEXDATA, 0, asBytes(texDatas_)},
        {L::VERTEXES, 0, asBytes(vertices_)},
        {L::VISIBILITY, 0, {vis.data(), vis.size()}},
        {L::NODES, 0, asBytes(nodes_)},
        {L::TEXINFO, 0, asBytes(texInfos_)},
        {L::FACES, kFacesLumpVersion, asBytes(faces_)},
        {L::ORIGINALFACES, 0, asBytes(originalFaces)},
        {L::LIGHTING, kLightingLumpVersion, asBytes(lighting_)},
        {L::LEAFS, kLeafsLumpVersion, asBytes(leaves_)},
        {L::EDGES, 0, asBytes(edges_)},
        {L::SURFEDGES, 0, asBytes(surfEdges_)},
        {L::MODELS, 0, asBytes(models_)},
        {L::LEAFFACES, 0, asBytes(leafFaces_)},
        {L::LEAFBRUSHES, 0, asBytes(leafBrushes_)},
        {L::BRUSHES, 0, asBytes(brushes_)},
        {L::BRUSHSIDES, 0, asBytes(brushSides_)},
        {L::AREAS, 0, asBytes(areas)},
        {L::TEXDATA_STRING_DATA, 0, asBytes(texDataStringData_)},
        {L::TEXDATA_STRING_TABLE, 0, asBytes(texDataStringTable_)},
        {L::LEAF_AMBIENT_INDEX, 0, asBytes(ambientIndex)},
        {L::LEAF_AMBIENT_LIGHTING, kLeafAmbientLumpVersion, asBytes(ambientLighting)},
        {L::MAP_FLAGS, 0, asBytes(mapFlags)},
    };

    // written straight out instead of through bsppp's own writer: on a freshly created file bsppp::BSP::setLump stages a lump with its offset and length left at zero, and bake() skips any lump its hasLump() calls absent, which is exactly those. the staged lump branch of bake() is unreachable for a new map so every lump comes back empty. the format is small enough to emit directly, and that also keeps the per lump versions above entirely in our hands. we still read bsps with bsppp
    std::vector<std::byte> payload;
    std::array<LumpEntry, kLumpCount> header{};
    for (const auto& w : lumpWrites) {
        auto index = static_cast<size_t>(w.lump);
        if (index >= kLumpCount) {
            error = "lump index out of range";
            return false;
        }
        // lumps are 4 byte aligned
        while (payload.size() % 4 != 0) payload.push_back(std::byte{0});
        LumpEntry& entry = header[index];
        entry.version = static_cast<int32_t>(w.version);
        if (w.data.empty()) continue;  // absent: offset and length stay zero
        entry.offset = static_cast<int32_t>(kHeaderSize + payload.size());
        entry.length = static_cast<int32_t>(w.data.size());
        payload.insert(payload.end(), w.data.begin(), w.data.end());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "couldn't open " + path + " for writing";
        return false;
    }
    auto writeInt = [&file](int32_t v) {
        char bytes[4];
        for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((v >> (i * 8)) & 0xFF);
        file.write(bytes, 4);
    };
    file.write("VBSP", 4);
    writeInt(static_cast<int32_t>(kMapVersion));
    for (const LumpEntry& entry : header) {
        writeInt(entry.offset);
        writeInt(entry.length);
        writeInt(entry.version);
        writeInt(0);  // fourCC: the uncompressed length, and we never compress
    }
    writeInt(0);  // map revision
    file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    file.flush();
    if (!file) {
        error = "failed writing " + path;
        return false;
    }
    file.close();

    stats.planes = static_cast<int>(planes_.size());
    stats.vertices = static_cast<int>(vertices_.size());
    stats.edges = static_cast<int>(edges_.size());
    stats.faces = static_cast<int>(faces_.size());
    stats.texInfos = static_cast<int>(texInfos_.size());
    stats.materials = static_cast<int>(texDatas_.size());
    stats.brushes = static_cast<int>(brushes_.size());
    stats.brushSides = static_cast<int>(brushSides_.size());
    stats.models = static_cast<int>(models_.size());
    stats.lightmapLuxels = static_cast<int>(lighting_.size() / 4);
    return true;
}

// walking the document

// adds one solid's faces and its brush. returns false when there was nothing usable in it
bool addSolid(BspBuilder& b, const Solid& solid, int32_t& brushesAdded, int32_t& facesAdded) {
    std::vector<Plane> planes = solidPlanes(solid);
    std::vector<std::vector<glm::vec3>> polygons = solidPolygons(solid);
    if (planes.size() != solid.sides.size() || polygons.size() != solid.sides.size()) return false;

    // a brush made only of hint/skip never reaches the engine
    bool anyReal = false;
    for (const auto& side : solid.sides) {
        if (!materialTraits(side.material).skipEntirely) anyReal = true;
    }
    if (!anyReal) return false;

    bool any = false;
    for (size_t i = 0; i < solid.sides.size(); ++i) {
        MaterialTraits traits = materialTraits(solid.sides[i].material);
        if (traits.skipEntirely) continue;
        if (b.addFace(polygons[i], planes[i].normal, planes[i].dist, solid.sides[i], traits)) {
            ++facesAdded;
            any = true;
        }
    }

    // contents come from the brush as a whole: a playerclip brush is one whose sides are playerclip
    int32_t contents = kContentsSolid;
    for (const auto& side : solid.sides) {
        MaterialTraits traits = materialTraits(side.material);
        if (traits.contents != kContentsSolid) {
            contents = traits.contents;
            break;
        }
    }
    b.addBrush(planes, solid.sides, contents);
    ++brushesAdded;
    return any;
}

bool solidsBounds(const std::vector<Solid>& solids, glm::vec3& mins, glm::vec3& maxs) {
    bool any = false;
    for (const auto& s : solids) {
        glm::vec3 mn, mx;
        if (!solidBounds(s, mn, mx)) continue;
        if (!any) {
            mins = mn;
            maxs = mx;
            any = true;
        } else {
            mins = glm::min(mins, mn);
            maxs = glm::max(maxs, mx);
        }
    }
    return any;
}

// the entity lump, in the same "key" "value" block form a vmf uses
std::string buildEntityText(const Document& doc, const std::vector<std::string>& brushEntityModels) {
    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c != '"') out.push_back(c);
        }
        return out;
    };
    auto writeEntity = [&](std::string& out, const Entity& e, const std::string& modelOverride) {
        out += "{\n";
        bool wroteModel = false;
        for (const auto& [k, v] : e.keyValues) {
            if (!modelOverride.empty() && collision::toLowerAscii(k) == "model") {
                out += "\"model\" \"" + modelOverride + "\"\n";
                wroteModel = true;
                continue;
            }
            out += "\"" + escape(k) + "\" \"" + escape(v) + "\"\n";
        }
        if (!modelOverride.empty() && !wroteModel) {
            out += "\"model\" \"" + modelOverride + "\"\n";
        }
        for (const auto& [k, v] : e.connections) {
            out += "\"" + escape(k) + "\" \"" + escape(v) + "\"\n";
        }
        out += "}\n";
    };

    std::string out;
    writeEntity(out, doc.world, "");
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        writeEntity(out, doc.entities[i], i < brushEntityModels.size() ? brushEntityModels[i] : std::string());
    }
    return out;
}

}  // namespace

BspWriteResult writeBsp(const std::string& path, const Document& doc, const TextureSizeLookup& textureSize) {
    BspWriteResult result;
    BspBuilder b(textureSize);

    glm::vec3 worldMin(0.0f), worldMax(0.0f);
    if (!solidsBounds(doc.world.solids, worldMin, worldMax)) {
        // an empty world still needs somewhere to put the tree
        worldMin = glm::vec3(-64.0f);
        worldMax = glm::vec3(64.0f);
    }
    b.beginTree(worldMin);

    // model 0 is worldspawn: every world brush and its faces
    int32_t worldFirstFace = b.faceCount();
    int32_t worldFirstBrush = b.brushCount();
    int32_t worldFaces = 0, worldBrushes = 0;
    for (const auto& solid : doc.world.solids) {
        if (!addSolid(b, solid, worldBrushes, worldFaces)) ++result.stats.skippedSolids;
    }
    int32_t worldHeadNode =
        b.addModelTree(worldFirstFace, worldFaces, worldFirstBrush, worldBrushes, worldMin, worldMax, true);
    b.addModel(worldMin, worldMax, glm::vec3(0.0f), worldHeadNode, worldFirstFace, worldFaces);

    // then one model per brush entity, which is what "model" "*N" points at
    std::vector<std::string> brushEntityModels(doc.entities.size());
    for (size_t i = 0; i < doc.entities.size(); ++i) {
        const Entity& e = doc.entities[i];
        if (e.solids.empty()) continue;

        glm::vec3 mins, maxs;
        if (!solidsBounds(e.solids, mins, maxs)) continue;

        int32_t firstFace = b.faceCount();
        int32_t firstBrush = b.brushCount();
        int32_t faceCount = 0, brushCount = 0;
        for (const auto& solid : e.solids) {
            if (!addSolid(b, solid, brushCount, faceCount)) ++result.stats.skippedSolids;
        }
        if (brushCount == 0) continue;

        int32_t headNode = b.addModelTree(firstFace, faceCount, firstBrush, brushCount, mins, maxs, false);
        // a brush entity's geometry is stored in world space here so its model origin stays at zero instead of at the entity's origin
        b.addModel(mins, maxs, glm::vec3(0.0f), headNode, firstFace, faceCount);
        brushEntityModels[i] = "*" + std::to_string(b.modelCount() - 1);
    }

    std::string entityText = buildEntityText(doc, brushEntityModels);
    result.stats.entities = static_cast<int>(doc.entities.size()) + 1;

    if (!b.write(path, entityText, result.stats, result.error)) return result;
    result.ok = true;
    return result;
}

}  // namespace vmf
