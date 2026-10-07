#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bsp/bsp_loader.h"
#include "bsp/material_loader.h"
#include "bsp/spawn_point.h"
#include "collision/brush_polygons.h"
#include "collision/bsp_brush_loader.h"
#include "collision/clip.h"
#include "audio/sound.h"
#include "content/game_content.h"
#include "content/materials.h"
#include "console/cfg_exec.h"
#include "console/cvar.h"
#include "movement/noclip.h"
#include "movement/personal_bests.h"
#include "movement/replay.h"
#include "movement/player_move.h"
#include "movement/timer.h"
#include "movement/triggers.h"
#include "movement/zones.h"
#include "platform/file_dialog.h"
#include "platform/process.h"
#include "platform/discord.h"
#include "platform/user_files.h"
#include "platform/video_recorder.h"
#include "platform/window.h"
#include "render/camera.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/shaders.h"
#include "render/texture.h"
#include "render/debug_box.h"
#include "render/lighting.h"
#include "ui/imgui_layer.h"
#include "ui/jhud.h"
#include "ui/keybind_editor.h"
#include "ui/keybinds.h"
#include "ui/menu_bar.h"
#include "ui/run_hud.h"
#include "ui/strafe_trainer.h"
#include "vmf/document.h"
#include "vmf/geometry.h"
#include "vmf/playtest.h"
#include "ui/settings_panel.h"
#include "ui/sounds_window.h"
#include "ui/zones_window.h"

enum class MoveMode { Play, InstantNoclip, SourceNoclip };
const char* moveModeLabel(MoveMode m) {
    switch (m) {
        case MoveMode::Play: return "play";
        case MoveMode::InstantNoclip: return "noclip (instant)";
        case MoveMode::SourceNoclip: return "noclip (source)";
    }
    return "?";
}

namespace {

void runDropTest(const bsp::WorldGeometry& geometry, const collision::WorldBrushes& brushes) {
    glm::vec3 mapMin(std::numeric_limits<float>::max());
    glm::vec3 mapMax(std::numeric_limits<float>::lowest());
    for (const auto& group : geometry.groups) {
        for (const auto& v : group.triangles) {
            mapMin = glm::min(mapMin, v.position);
            mapMax = glm::max(mapMax, v.position);
        }
    }

    glm::vec3 halfExtents(16.0f, 16.0f, 36.0f);
    glm::vec3 pos((mapMin.x + mapMax.x) * 0.5f, (mapMin.y + mapMax.y) * 0.5f, mapMax.z - 10.0f);
    glm::vec3 velocity(0.0f);

    const float dt = 1.0f / 100.0f;  // 100t
    const float gravity = 800.0f;    // sv_gravity shit
    const int totalTicks = 500;

    float lastLoggedZ = pos.z;
    bool everGrounded = false;

    for (int tick = 0; tick < totalTicks; ++tick) {
        velocity.z -= gravity * dt;
        pos += velocity * dt;

        glm::vec3 hitNormal(0.0f);
        if (collision::resolveAabbVsBrushes(brushes, pos, halfExtents, &hitNormal)) {
            if (hitNormal.z > 0.7f) {
                velocity.z = 0.0f;
                everGrounded = true;
            }
        }
        lastLoggedZ = pos.z;
    }

    if (everGrounded && std::abs(velocity.z) < 0.01f) {
        std::printf("drop test: settled at z=%.2f\n", lastLoggedZ);
    } else {
        std::printf("drop test: did not settle cleanly (final z=%.2f, ever grounded=%d)\n",
                     lastLoggedZ, everGrounded);
    }
}

static_assert(vmf::kZoneTracks == movement::kMaxTracks);

struct LoadedMap {
    bsp::WorldGeometry geometry;
    collision::WorldBrushes brushes;
    collision::MapTriggers triggers;
    glm::vec3 spawnPosition;
    float spawnYawDeg;
    // .vmf maps are played straight from the editor's file (no compile), their start/end zones can come from the map itself.
    bool isVmf = false;
    std::array<vmf::TrackZoneBoxes, vmf::kZoneTracks> mapZones;
    bsp::SceneLighting lighting;  // .vmf light entities
    std::string pakSource;  // packed textures from the bsp (if provided by the bsp itself)
};

// 1 = baked lightmaps 2 = live light entities 0 = neither (plain shading)
int lightingModeFor(const LoadedMap& map) {
    if (!map.geometry.lightmapAtlas.rgba8888.empty()) return 1;
    if (map.lighting.hasLights) return 2;
    return 0;
}

bool hasExtension(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(path[path.size() - ext.size() + i])) != ext[i]) return false;
    }
    return true;
}

std::optional<LoadedMap> loadVmfMap(const std::string& path) {
    auto doc = vmf::loadDocument(path);
    if (!doc) {
        return std::nullopt;
    }
    vmf::PlaytestMap map = vmf::buildPlaytestMap(*doc);
    std::printf("vmf: %zu solid brushes, %zu triggers\n", map.brushes.brushes.size(), map.triggers.triggers.size());
    glm::vec3 spawnFeet = map.hasSpawn ? map.spawnFeet : glm::vec3(0.0f);
    LoadedMap result{std::move(map.geometry), std::move(map.brushes), std::move(map.triggers),
                     spawnFeet + glm::vec3(0.0f, 0.0f, 36.0f), map.spawnYawDeg, true, map.zones,
                     std::move(map.lighting), map.pakfile};
    return result;
}

std::optional<LoadedMap> loadMap(const std::string& path) {
    if (hasExtension(path, ".vmf")) {
        return loadVmfMap(path);
    }
    auto geometry = bsp::loadWorldGeometry(path);
    if (!geometry) {
        std::fprintf(stderr, "failed to load map: %s\n", path.c_str());
        return std::nullopt;
    }
    auto brushes = collision::loadWorldBrushes(path);
    if (!brushes) {
        std::fprintf(stderr, "failed to load collision: %s\n", path.c_str());
        return std::nullopt;
    }
    runDropTest(*geometry, *brushes);
    auto triggers = collision::loadMapTriggers(path);

    glm::vec3 spawnPosition(0.0f, 0.0f, 64.0f);  // no spawn entity?? (fix your fucking maps people)
    float spawnYawDeg = 0.0f;
    if (auto spawn = bsp::loadSpawnPoint(path)) {
        spawnPosition = spawn->position + glm::vec3(0.0f, 0.0f, 36.0f);  // entity origin is feet-level; 36 = playerHalfHeight
        spawnYawDeg = spawn->yawDeg;
    }

    LoadedMap result{std::move(*geometry), std::move(*brushes),
                     triggers ? std::move(*triggers) : collision::MapTriggers{}, spawnPosition, spawnYawDeg, false,
                     {}, {}, path};
    return result;
}

struct RenderGroup {
    render::Mesh mesh;
    render::Texture texture;
    bool alphaTest = false;
    float alphaTestRef = 0.5f;
    bool translucent = false;
    glm::vec3 center{0.0f};  // for sorting the translucent ones back to front
};

// textures come from the map's own pakfile (pakSource), then the mounted cs:s install, then the built-ins. anything else gets the MT (missing texture) texture.
std::vector<RenderGroup> buildRenderGroups(const LoadedMap& map, const content::GameContent& game) {
    const bsp::WorldGeometry& geometry = map.geometry;
    const bool vmfMap = map.isVmf;
    content::FileReader pak = map.pakSource.empty() ? nullptr : content::bspPakReader(map.pakSource);
    std::vector<RenderGroup> groups;
    groups.reserve(geometry.groups.size());
    int counts[4] = {0, 0, 0, 0};
    for (const auto& matGroup : geometry.groups) {
        content::ResolvedMaterial material = content::resolveMaterial(matGroup.materialName, &game, pak);
        counts[static_cast<int>(material.source)]++;
        RenderGroup rg;
        if (vmfMap) {
            std::vector<bsp::Vertex> tris = matGroup.triangles;
            for (auto& v : tris) v.uv /= material.uvSize;
            rg.mesh.upload(tris);
        } else {
            rg.mesh.upload(matGroup.triangles);
        }
        rg.texture.upload(material.pixels);
        rg.alphaTest = material.alphaTest;
        rg.alphaTestRef = material.alphaTestRef;
        rg.translucent = material.translucent;
        if (!matGroup.triangles.empty()) {
            glm::vec3 sum(0.0f);
            for (const auto& v : matGroup.triangles) sum += v.position;
            rg.center = sum / static_cast<float>(matGroup.triangles.size());
        }
        groups.push_back(std::move(rg));
    }
    std::printf("textures: %d from map, %d from CS:S, %d built-in, %d missing (MT)\n", counts[0], counts[1],
                counts[2], counts[3]);
    return groups;
}

// sc & st: brush volumes that are invisible, drawn with source's tool textures so which is which reads off the surface one tile per 128 units
bsp::MaterialGroup toolBrushGeometry(const std::vector<collision::Brush>& brushes, const char* material) {
    constexpr float kUnitsPerTile = 128.0f;
    bsp::MaterialGroup group;
    group.materialName = material;
    for (const auto& brush : brushes) {
        for (const auto& face : collision::brushPolygons(brush)) {
            const std::vector<glm::vec3>& poly = face.points;
            const glm::vec3 normal = face.normal;
            const glm::vec3 up =
                std::fabs(normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 uAxis = glm::normalize(glm::cross(up, normal));
            const glm::vec3 vAxis = glm::cross(normal, uAxis);
            auto uvOf = [&](const glm::vec3& p) {
                return glm::vec2(glm::dot(p, uAxis), -glm::dot(p, vAxis)) / kUnitsPerTile;
            };
            for (size_t i = 1; i + 1 < poly.size(); ++i) {
                for (size_t idx : {size_t(0), i, i + 1}) {
                    bsp::Vertex v;
                    v.position = poly[idx];
                    v.normal = normal;
                    v.uv = uvOf(poly[idx]);
                    group.triangles.push_back(v);
                }
            }
        }
    }
    return group;
}

std::vector<collision::Brush> loadClipBrushes(const std::string& path) {
    constexpr int32_t kContentsPlayerClip = 0x10000;
    std::vector<collision::Brush> out;
    auto all = collision::loadBspBrushes(path);
    if (!all) return out;
    for (const auto& bb : *all) {
        if ((bb.contents & kContentsPlayerClip) == 0 || bb.model > 0) continue;
        collision::Brush brush;
        brush.sides.reserve(bb.sides.size());
        for (const auto& side : bb.sides) brush.sides.push_back(side.plane);
        if (brush.sides.size() >= 4) out.push_back(std::move(brush));
    }
    return out;
}

void destroyRenderGroups(std::vector<RenderGroup>& groups) {
    for (auto& rg : groups) {
        rg.mesh.destroy();
        rg.texture.destroy();
    }
    groups.clear();
}

// rpc
inline constexpr const char* kDiscordAppId = "1557069026407227412";
inline constexpr const char* kDiscordDetails = "movengine - https://mve.1998.lol";
// the art asset in the dev portal, shown as the big image
inline constexpr const char* kDiscordLargeImage = "448004ebee2ca1e66b499429785f8e27";

std::string prefFilePath(const std::string& fileName) {
    return platform::prefFile(fileName);
}

std::string keybindsConfigPath() {
    return prefFilePath("keybinds.cfg");
}

std::string contentConfigPath() {
    return prefFilePath("content.cfg");
}

std::string settingsConfigPath() {
    return prefFilePath("settings.cfg");
}

std::vector<float> cvarValues(const console::CvarRegistry& cvars) {
    std::vector<float> values;
    for (const auto& cvar : cvars.all()) {
        values.push_back(cvar.value);
    }
    return values;
}

std::string zonesConfigPath(const std::string& mapPath) {
    return prefFilePath("zones_" + std::filesystem::path(mapPath).stem().string() + ".cfg");
}

std::string settingsKey(const console::CvarRegistry& cvars) {
    std::vector<std::string> parts;
    char buf[96];
    for (const auto& cvar : cvars.all()) {
        if (cvar.name.rfind("sv_", 0) != 0) continue;  // 100t (fuck you 66t surfers 100t reigns supreme 4ever)
        std::snprintf(buf, sizeof(buf), "%s=%g", cvar.name.c_str(), cvar.value);
        parts.push_back(buf);
    }
    std::sort(parts.begin(), parts.end());
    std::string key;
    for (const auto& p : parts) key += (key.empty() ? "" : " ") + p;
    return key;
}

std::string describeSettings(const std::string& key, const console::CvarRegistry& cvars) {
    if (key.empty()) return "settings weren't recorded for this time";
    std::istringstream iss(key);
    std::string part, out;
    while (iss >> part) {
        size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string name = part.substr(0, eq);
        char def[64];
        for (const auto& cvar : cvars.all()) {
            std::snprintf(def, sizeof(def), "%g", cvar.defaultValue);
            if (cvar.name == name && part.substr(eq + 1) != def) out += (out.empty() ? "" : ", ") + part;
        }
    }
    return out.empty() ? "default settings" : out;
}

// videos are put in the videos folder :scream:
std::filesystem::path videoDirectory() {
    const char* base = SDL_GetBasePath();
    std::filesystem::path dir = std::filesystem::path(base ? base : "") / "video";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// map_track_style_time.mp4, with anything path-hostile removed
std::string videoFileName(const std::string& mapPath, const std::string& suffix) {
    std::string stem = std::filesystem::path(mapPath).stem().string();
    std::string safe;
    for (char c : stem + "_" + suffix) {
        safe += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.')
                     ? c
                     : '_';
    }
    return safe + ".mp4";
}

std::string timesConfigPath(const std::string& mapPath) {
    return prefFilePath("times_" + std::filesystem::path(mapPath).stem().string() + ".cfg");
}

// one saved replay per map, track and style the run that set the pb. Style names are shown to people ("Normal"), so anything that would fuck up the path becomes an underscore
std::string replayFilePath(const std::string& mapPath, int track, const std::string& style) {
    std::string safe;
    for (char c : style) {
        safe += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? static_cast<char>(std::tolower(c)) : '_';
    }
    return prefFilePath("replay_" + std::filesystem::path(mapPath).stem().string() + "_t" + std::to_string(track) +
                         "_" + safe + ".rpl");
}

// falling so far respawns you
float killHeight(const collision::WorldBrushes& brushes) {
    float lowest = std::numeric_limits<float>::max();
    for (const auto& b : brushes.brushes) {
        glm::vec3 mn, mx;
        if (collision::brushBounds(b, mn, mx)) lowest = std::min(lowest, mn.z);
    }
    return lowest == std::numeric_limits<float>::max() ? -16384.0f : lowest - 1024.0f;
}

}

int main(int argc, char** argv) {
    std::printf("movengine build %s\n", BHOP_GIT_COMMIT);
    std::optional<std::string> mapPath;
    if (argc > 1) {
        mapPath = std::string(argv[1]);
    } else {
        mapPath = platform::chooseMapFile();
    }
    if (!mapPath) {
        std::printf("no map selected, exiting\n");
        return 0;
    }
    std::printf("selected map: %s\n", mapPath->c_str());

    platform::Window window;
    platform::DiscordPresence discord;
    discord.start(kDiscordAppId);
    discord.setLargeImage(kDiscordLargeImage, "movengine");

    if (!window.init("movengine", 1280, 720)) {
        return 1;
    }

    GLuint program = render::createShaderProgram(render::kTexturedVertexSrc, render::kTexturedFragmentSrc);
    if (!program) {
        return 1;
    }
    GLint uViewProjLoc = glGetUniformLocation(program, "uViewProj");
    GLint uTextureLoc = glGetUniformLocation(program, "uTexture");
    render::LightingUniforms lightingUniforms;
    lightingUniforms.locate(program);
    GLint uFogEndLoc = glGetUniformLocation(program, "uFogEnd");
    GLint uFogColorLoc = glGetUniformLocation(program, "uFogColor");
    GLint uExposureLoc = glGetUniformLocation(program, "uExposure");
    GLint uAlphaTestRefLoc = glGetUniformLocation(program, "uAlphaTestRef");
    GLint uViewPosLoc = glGetUniformLocation(program, "uViewPos");
    const glm::vec3 kSkyColor(0.10f, 0.11f, 0.14f);
    float drawDistance = 0.0f;  // r_drawdist 0 = everything (32768 does the exact same shit)
    float mousePerTick = 1.0f;  // m_pertick spreads a frame's mouse over its ticks (ri2 implementation ik this is bad FORGIVE ME)
    float fpsMax = 0.0f;        // fps_max 0 = uncapped
    // cl_yawspeed is 1070/9 degrees a second (118) which makes a perfect 290 pre
    float yawSpeed = 1070.0f / 9.0f;
    float brightness = 1.0f;  // mat_brightness
    std::vector<platform::Window::MouseSample> mouseSamples;
    float showFps = 0.0f;       // cl_showfps
    float fpsSmoothed = 0.0f;   // what cl_showfps prints

    GLuint debugBoxProgram = render::createShaderProgram(render::kDebugBoxVertexSrc, render::kDebugBoxFragmentSrc);
    if (!debugBoxProgram) {
        return 1;
    }
    GLint uDebugViewProjLoc = glGetUniformLocation(debugBoxProgram, "uViewProj");
    GLint uDebugModelLoc = glGetUniformLocation(debugBoxProgram, "uModel");
    GLint uDebugColorLoc = glGetUniformLocation(debugBoxProgram, "uColor");
    render::DebugBox debugBox;
    debugBox.init();

    auto loaded = loadMap(*mapPath);
    if (!loaded) {
        return 1;
    }
    // cs:s textures when it's installed (or a folder was picked)*, else a
    // texture pack, else the built-ins.

    // VALVE I'M NOT RESPONSIBLE FOR ANYONE WHO SHARES TEXTURES FROM CS:S
    content::GameContent gameContent;
    {
        const char* base = SDL_GetBasePath();
        content::mountConfiguredContent(gameContent, contentConfigPath(),
                                        std::filesystem::path(reinterpret_cast<const char8_t*>(base ? base : "")));
    }
    auto renderGroups = buildRenderGroups(*loaded, gameContent);
    float showClips = 0.0f, showTriggers = 0.0f;  // r_showclips / r_showtriggers
    RenderGroup clipGroup, triggerGroup;
    int clipBrushCount = 0, triggerBrushCount = 0;
    auto buildToolBrushes = [&]() {
        clipGroup.mesh.destroy();
        clipGroup.texture.destroy();
        triggerGroup.mesh.destroy();
        triggerGroup.texture.destroy();
        content::FileReader pak =
            loaded->pakSource.empty() ? nullptr : content::bspPakReader(loaded->pakSource);
        auto upload = [&](const bsp::MaterialGroup& g, RenderGroup& rg) {
            content::ResolvedMaterial mat = content::resolveMaterial(g.materialName, &gameContent, pak);
            std::vector<bsp::Vertex> tris = g.triangles;
            for (auto& v : tris) v.uv *= 256.0f / mat.uvSize;  // keep a tile 128 units whatever size it is
            rg.mesh.upload(tris);
            rg.texture.upload(mat.pixels);
        };
        // .vmf maps are played straight from the editor and have no compiled brush lump to read clips out of.
        std::vector<collision::Brush> clips = loaded->isVmf ? std::vector<collision::Brush>{}
                                                            : loadClipBrushes(loaded->pakSource);
        clipBrushCount = static_cast<int>(clips.size());
        upload(toolBrushGeometry(clips, "TOOLS/TOOLSPLAYERCLIP"), clipGroup);

        std::vector<collision::Brush> trigs;
        for (const auto& t : loaded->triggers.triggers)
            trigs.insert(trigs.end(), t.brushes.begin(), t.brushes.end());
        triggerBrushCount = static_cast<int>(trigs.size());
        upload(toolBrushGeometry(trigs, "TOOLS/TOOLSTRIGGER"), triggerGroup);
        std::printf("tool brushes: %d player clips, %d trigger volumes\n", clipBrushCount, triggerBrushCount);
    };
    buildToolBrushes();
    render::Texture lightmapTexture;
    auto uploadLightmap = [&]() {
        lightmapTexture.destroy();
        if (!loaded->geometry.lightmapAtlas.rgba8888.empty()) {
            lightmapTexture.upload(loaded->geometry.lightmapAtlas, false);
        }
    };
    uploadLightmap();

    ui::ImGuiLayer imgui;
    // for non-admin purposes, imgui shit is written to the same folder that movengine is in
    static std::string imguiIniPath = prefFilePath("imgui.ini");
    if (!imgui.init(window)) {
        std::fprintf(stderr, "failed to init imgui\n");
        return 1;
    }
    ImGui::GetIO().IniFilename = imguiIniPath.c_str();

    render::FreeFlyCamera camera;
    glm::vec3 spawnPosition = loaded->spawnPosition;
    float spawnYawDeg = loaded->spawnYawDeg;
    camera.position = spawnPosition;
    camera.setYawDegrees(spawnYawDeg);

    MoveMode moveMode = MoveMode::Play;
    movement::MoveConstants moveConstants;
    movement::PlayerState playerState;
    playerState.position = spawnPosition;
    movement::NoclipState sourceNoclipState;
    sourceNoclipState.position = spawnPosition;
    // hardcoded 3500u nc for infinite
    constexpr float kSourceNoclipSpeed = 3500.0f;
    float instantNoclipSpeed = 3000.0f;              // noclip_speed
    constexpr float kInstantNoclipShiftBoost = 500.0f;  // shift = go fast <(O_O)>
    glm::vec3 instantNoclipVelocity(0.0f);  // the freefly camera has no velocity of its own

    movement::MapZones zones;
    int currentTrack = 0;        // the track that restart takes you to (0 = main n = bonus n)
    movement::TimerState timer;
    movement::PersonalBests personalBests;
    movement::Replay recordingReplay;  // the run in progress, one frame per tick
    movement::Replay bestReplay;       // the run that set the pb for this map/track/style
    bool replayPlaying = false;
    bool replayPaused = false;
    float replayTime = 0.0f;
    float replaySpeed = 1.0f;
    platform::VideoRecorder videoRecorder;
    float videoQuality = 4.0f;   // video_quality, 1 = lossless , 10 twitter compression (fuck elon)
    float videoFps = 60.0f;      // video_fps
    float videoAccum = 0.0f;     // i copied the functionality of nulls because they all need admin rights, and i don't fw that. this is a bhop engine why would you need admin for a bhop engine lmao
    float nullAD = 1.0f;          // null_ad
    float releaseWOnJump = 1.0f;  // w_release_on_jump
    float recorrectTeleportMomentum = 1.0f;  // tp_recorrect
    int lastAdPressed = 0;        // 1 = A was pressed more recently, 2 = D (socd
    bool prevAHeld = false, prevDHeld = false;
    std::string loadedReplayKey;
    std::string timesPath;
    float lastRunDelta = std::numeric_limits<float>::quiet_NaN();  // compare this run to the pb
    float killZ = 0.0f;

    std::filesystem::path appDir;
    if (const char* base = SDL_GetBasePath()) appDir = std::filesystem::path(reinterpret_cast<const char8_t*>(base));
    std::thread exportThread;
    content::ExportProgress exportProgress;
    std::string exportPath;
    audio::SoundPlayer sounds;
    sounds.init();
    std::string soundsPath = prefFilePath("sounds.cfg");
    audio::SoundConfig soundConfig = audio::defaultSoundConfig();
    audio::loadSoundConfig(soundsPath, soundConfig);
    auto playSound = [&](audio::SoundEvent event) {
        sounds.play(audio::resolveSoundPath(soundConfig.path(event), appDir), soundConfig.volume);
    };
    int creatingTrack = -1;
    bool creatingStart = true;
    bool zoneHasFirstCorner = false;
    glm::vec3 zoneFirstCorner(0.0f);
    ui::StrafeTrainerState strafeTrainerState;
    ui::JhudState jhud;
    float lastHorizontalSpeed = 0.0f;

    std::string zonesPath = zonesConfigPath(*mapPath);
    // zones put in the map by a map creator beforehand are used over ones you place
    auto loadZonesForMap = [&]() {
        zonesPath = zonesConfigPath(*mapPath);
        movement::loadZones(zonesPath, zones);
        auto fromMap = [](const vmf::ZoneBox& box, movement::Zone& zone) {
            if (zone.defined || zone.removed || !box.defined) return;
            zone.defined = true;
            zone.min = box.min;
            zone.max = box.max;
            zone.anchorPoint = glm::vec3((box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, box.min.z);
        };
        for (int t = 0; t < movement::kMaxTracks; ++t) {
            fromMap(loaded->mapZones[t].start, zones.tracks[t].start);
            fromMap(loaded->mapZones[t].end, zones.tracks[t].end);
        }
        if (!zones.tracks[currentTrack].used()) currentTrack = 0;
        movement::switchTimerTrack(timer, currentTrack);
        // ...and the map's saved times and kill height along with them.
        timesPath = timesConfigPath(*mapPath);
        personalBests.load(timesPath);
        killZ = killHeight(loaded->brushes);
    };
    loadZonesForMap();

    std::error_code mapTimeError;
    std::filesystem::file_time_type mapWriteTime = std::filesystem::last_write_time(*mapPath, mapTimeError);
    std::optional<std::filesystem::file_time_type> pendingWriteTime;
    float reloadCheckTimer = 0.5f;

    // printf is invisible
    std::string toastText;
    float toastTimer = 0.0f;
    auto showToast = [&](const std::string& text) {
        toastText = text;
        toastTimer = 2.5f;
        std::printf("%s\n", text.c_str());
    };
    movement::TriggerRuntime triggerRuntime;
    movement::resetTriggerRuntime(triggerRuntime, loaded->triggers);

    float modeOverlayTimer = 3.0f;  // seconds left to show the mode overlay

    auto switchMoveMode = [&](MoveMode newMode) {
        glm::vec3 currentPos = camera.position;
        MoveMode oldMode = moveMode;
        moveMode = newMode;
        // fuck you your timer is disabled now
        if (newMode != MoveMode::Play) {
            movement::resetTimerRun(timer);
            timer.wasInStartZone = false;
        }
        if (newMode == MoveMode::Play) {
            playerState.position = currentPos;
            movement::resetRunState(playerState);
            // don't kill the nc speed! pls...
            if (oldMode == MoveMode::SourceNoclip) {
                playerState.velocity = sourceNoclipState.velocity;
            } else if (oldMode == MoveMode::InstantNoclip) {
                playerState.velocity = instantNoclipVelocity;
            } else {
                playerState.velocity = glm::vec3(0.0f);
            }
        } else if (newMode == MoveMode::SourceNoclip) {
            sourceNoclipState.position = currentPos;
            sourceNoclipState.velocity = glm::vec3(0.0f);
        }
        modeOverlayTimer = 3.0f;
        std::printf("move mode: %s\n", moveModeLabel(moveMode));
    };

    console::CvarRegistry cvars;
    // i <3 100t
    constexpr float tickInterval = 0.01f;
    cvars.add("sensitivity", "mouse sensitivity, on CS:S's scale (degrees = counts * sensitivity * m_yaw)",
              camera.mouseSensitivity(), 0.1f, 20.0f, [&](float v) { camera.setMouseSensitivity(v); });
    cvars.add("m_yaw", "degrees of yaw per count per unit of sensitivity (CS:S: 0.022)", camera.mouseYaw(),
              0.0001f, 1.0f, [&](float v) { camera.setMouseYaw(v); });
    cvars.add("m_pitch", "degrees of pitch per count per unit of sensitivity (CS:S: 0.022)", 0.022f, 0.0001f, 1.0f,
              [&](float v) { camera.setMousePitch(v); });
    cvars.add("cl_yawspeed", "degrees per second the +left/+right turnbinds turn at",
              yawSpeed, 0.0f, 3600.0f, [&](float v) { yawSpeed = v; });
    cvars.add("fov", "field of view (degrees)", camera.fovDegrees(), 60.0f, 130.0f,
              [&](float v) { camera.setFovDegrees(v); });
    cvars.add("noclip_speed", "instant noclip's speed in units/sec (hold shift for +500); source noclip is a fixed 3500",
              instantNoclipSpeed, 100.0f, 20000.0f, [&](float v) { instantNoclipSpeed = v; });
    cvars.add("r_showclips", "draw player-clip brushes, which are invisible in play", showClips, 0.0f, 1.0f,
              [&](float v) { showClips = v; });
    cvars.add("r_showtriggers", "draw trigger volumes, which are invisible in play", showTriggers, 0.0f, 1.0f,
              [&](float v) { showTriggers = v; });
    cvars.add("tp_recorrect",
              "leaving a teleport, turn your speed to face the way the destination does instead of keeping the "
              "direction you went in with",
              recorrectTeleportMomentum, 0.0f, 1.0f, [&](float v) { recorrectTeleportMomentum = v; });
    cvars.add("null_ad", "holding A and D moves you the way you pressed last, instead of cancelling out",
              nullAD, 0.0f, 1.0f, [&](float v) { nullAD = v; });
    cvars.add("w_release_on_jump", "W stops counting while jump is held", releaseWOnJump, 0.0f, 1.0f,
              [&](float v) { releaseWOnJump = v; });
    cvars.add("video_quality", "1 lossless to 10 twitter compression", videoQuality, 1.0f, 10.0f,
              [&](float v) { videoQuality = std::round(v); });
    cvars.add("video_fps", "frames per second to record at", videoFps, 15.0f, 240.0f,
              [&](float v) { videoFps = v; });
    cvars.add("mat_brightness", "scales the lit result; 1 is the map's own lighting", brightness, 0.1f, 4.0f,
              [&](float v) { brightness = v; });
    cvars.add("vsync", "sync to the display's refresh (0 off, 1 on); on caps the frame rate at the "
                       "refresh rate whatever fps_max says, and adds latency",
              0.0f, 0.0f, 1.0f, [&](float v) { window.setVsync(v != 0.0f ? 1 : 0); });
    cvars.add("fps_max", "frame rate cap (0 = uncapped); the physics tickrate is fixed at 100 either way",
              fpsMax, 0.0f, 1000.0f, [&](float v) { fpsMax = v; });
    cvars.add("cl_showfps", "show the frame rate in the corner", showFps, 0.0f, 1.0f,
              [&](float v) { showFps = v; });
    cvars.add("m_pertick",
              "spread each frame's mouse movement over the ticks it covers, so strafe gain "
              "does not fall off below the tickrate (0 = one sample per frame)",
              mousePerTick, 0.0f, 1.0f, [&](float v) { mousePerTick = v; });
    cvars.add("r_drawdist", "render distance in units (0 = unlimited); the world fades out before it",
              drawDistance, 0.0f, 32768.0f, [&](float v) { drawDistance = v > 0.0f ? std::max(v, 256.0f) : 0.0f; });
    cvars.add("sv_maxspeed", "max ground speed", moveConstants.maxSpeed, 100.0f, 1000.0f,
              [&](float v) { moveConstants.maxSpeed = v; });
    cvars.add("sv_accelerate", "ground acceleration", moveConstants.accelerate, 1.0f, 50.0f,
              [&](float v) { moveConstants.accelerate = v; });
    cvars.add("sv_airaccelerate", "air acceleration", moveConstants.airAccelerate, 1.0f, 2000.0f,
              [&](float v) { moveConstants.airAccelerate = v; });
    cvars.add("sv_friction", "ground friction", moveConstants.friction, 0.0f, 10.0f,
              [&](float v) { moveConstants.friction = v; });
    cvars.add("sv_gravity", "gravity", moveConstants.gravity, 0.0f, 3000.0f,
              [&](float v) { moveConstants.gravity = v; });
    cvars.add("sv_stopspeed", "ground stop-speed threshold", moveConstants.stopSpeed, 0.0f, 400.0f,
              [&](float v) { moveConstants.stopSpeed = v; });
    cvars.add("sv_jumpvelocity", "jump impulse (CS:S: 301.99, a 57-unit jump)", moveConstants.jumpVelocity,
              0.0f, 600.0f, [&](float v) { moveConstants.jumpVelocity = v; });

    std::string settingsPath = settingsConfigPath();
    if (std::filesystem::exists(settingsPath)) {
        auto stale = [](const std::string& line) {
            return line == "sv_maxspeed 320" || line == "sv_jumpvelocity 300";
        };
        std::ifstream old(settingsPath);
        std::string first;
        std::getline(old, first);
        if (first != console::kSettingsHeader) {
            std::string kept = stale(first) ? "" : first + "\n", line;
            while (std::getline(old, line)) {
                if (!stale(line)) kept += line + "\n";
            }
            old.close();
            std::ofstream(settingsPath, std::ios::trunc) << console::kSettingsHeader << '\n' << kept;
        }
        console::execConfigFile(settingsPath, cvars);
    }
    std::vector<float> savedCvarValues = cvarValues(cvars);

    ui::MenuBarState menuState;
    for (const auto& style : movement::styleList()) {
        menuState.styleNames.push_back(style.name);
    }

    bool uiMode = false;
    window.setRelativeMouseMode(true);

    std::string keybindsPath = keybindsConfigPath();

    ui::KeybindRegistry keybinds;
    auto toggleRecording = [&](const std::string& suffix) {
        if (videoRecorder.recording()) {
            std::string done = videoRecorder.outputPath();
            int frames = videoRecorder.frameCount();
            videoRecorder.stop();
            showToast("saved " + std::filesystem::path(done).filename().string() + " (" + std::to_string(frames) +
                      " frames)");
            return;
        }
        std::filesystem::path out = videoDirectory() / videoFileName(*mapPath, suffix);
        int capW = window.width(), capH = window.height();
        window.drawableSize(capW, capH);  // pixels, which is what gets read back
        if (!videoRecorder.start(out.string(), capW, capH, static_cast<int>(videoFps),
                                  static_cast<int>(videoQuality))) {
            showToast(videoRecorder.error());  // says ffmpeg is missing rather than doing nothing
            return;
        }
        videoAccum = 0.0f;
        showToast("recording to video/" + out.filename().string() + " (" +
                  platform::VideoRecorder::qualityLabel(static_cast<int>(videoQuality)) + ")");
    };
    keybinds.add("record", "start/stop recording to video/ (f9)", SDL_SCANCODE_F9,
                 [&]() { toggleRecording("clip"); });
    keybinds.add("replay", "watch your best run on this track (p)", SDL_SCANCODE_P, [&]() {
        if (bestReplay.empty()) {
            showToast("no saved run for " + movement::trackName(currentTrack) + " (" +
                      movement::styleList()[menuState.selectedStyle].name + ") yet");
            return;
        }
        replayPlaying = !replayPlaying;
        replayPaused = false;
        replayTime = 0.0f;
        if (!replayPlaying) {
            // after a replay, the player will be back where they were upon playing the replay (source shiz)
            playerState.position = camera.position;
            playerState.velocity = glm::vec3(0.0f);
            movement::resetRunState(playerState);
            movement::resetTimerRun(timer);
        }
    });
    keybinds.add("turn_left", "turn left while held (+left)", SDL_SCANCODE_E, []() {});
    keybinds.add("turn_right", "turn right while held (+right)", SDL_SCANCODE_Q, []() {});
    keybinds.add("fullscreen", "toggle fullscreen (f11)", SDL_SCANCODE_F11, [&]() {
        window.setFullscreen(!window.fullscreen());
    });
    keybinds.add("toggle_ui", "toggle menu/mouse (tab)", SDL_SCANCODE_TAB, [&]() {
        uiMode = !uiMode;
        window.setRelativeMouseMode(!uiMode);
    });
    // middle of the zone at its lowest corner's floor height. uneven floor can put that inside the ground so it steps up till the hull is clear, else falls back to the corner you stood on
    auto zoneMiddle = [&](const movement::Zone& zone) {
        glm::vec3 half(moveConstants.playerHalfWidth, moveConstants.playerHalfWidth, moveConstants.playerHalfHeight);
        glm::vec3 pos((zone.min.x + zone.max.x) * 0.5f, (zone.min.y + zone.max.y) * 0.5f, zone.min.z + half.z + 2.0f);
        for (; pos.z + half.z <= zone.max.z; pos.z += 8.0f) {
            glm::vec3 probe = pos;
            if (!collision::resolveAabbVsBrushes(loaded->brushes, probe, half)) {
                return pos;
            }
        }
        return zone.anchorPoint + glm::vec3(0.0f, 0.0f, half.z + 2.0f);
    };

    // restart goes to your set start, else the middle of the start zone, else the map spawn if there's no zones yet
    auto restart = [&](bool quiet = false) {
        if (!quiet) playSound(audio::SoundEvent::Restart);
        const movement::TrackZones& track = zones.tracks[currentTrack];
        glm::vec3 target = spawnPosition;
        float yaw = spawnYawDeg;
        float pitch = 0.0f;
        if (track.startPoint.defined) {
            target = track.startPoint.feetPos + glm::vec3(0.0f, 0.0f, moveConstants.playerHalfHeight + 2.0f);
            yaw = track.startPoint.yawDeg;
            pitch = track.startPoint.pitchDeg;
        } else if (track.start.defined) {
            target = zoneMiddle(track.start);
        }
        camera.position = target;
        camera.setYawDegrees(yaw);
        camera.setPitchDegrees(pitch);
        if (moveMode == MoveMode::Play) {
            playerState.position = target;
            playerState.velocity = glm::vec3(0.0f);
            movement::resetRunState(playerState);
        } else if (moveMode == MoveMode::SourceNoclip) {
            sourceNoclipState.position = target;
            sourceNoclipState.velocity = glm::vec3(0.0f);
        }
        movement::switchTimerTrack(timer, currentTrack);
        movement::resetTimerRun(timer);
        lastRunDelta = std::numeric_limits<float>::quiet_NaN();
        movement::resetTriggerRuntime(triggerRuntime, loaded->triggers);
        ui::resetJhud(jhud);
    };

    // starts the corner tool for a track's start or end zone
    auto beginZonePlacement = [&](int track, bool start) {
        creatingTrack = track;
        creatingStart = start;
        zoneHasFirstCorner = false;  // the old zone stays live until the new one's second corner
    };

    keybinds.add("restart", "restart (to your set start, the start zone, or spawn; ctrl+r: back to main)", SDL_SCANCODE_R, [&]() {
        restart();
    });
    keybinds.add("next_track", "switch track: main -> bonus 1 -> bonus 2 ... (restarts onto it)", SDL_SCANCODE_N,
                 [&]() {
                     for (int step = 1; step <= movement::kMaxTracks; ++step) {
                         int t = (currentTrack + step) % movement::kMaxTracks;
                         if (t == 0 || zones.tracks[t].start.defined) {
                             currentTrack = t;
                             break;
                         }
                     }
                     restart();
                     showToast("track: " + movement::trackName(currentTrack));
                 });
    keybinds.add("set_start", "set your restart position + view (inside the start zone)", SDL_SCANCODE_B, [&]() {
        float halfHeight = moveConstants.playerHalfHeight;
        glm::vec3 center = camera.position;
        if (moveMode == MoveMode::Play) {
            halfHeight = playerState.ducking ? moveConstants.duckHalfHeight : moveConstants.playerHalfHeight;
            center = playerState.position;
        }
        glm::vec3 half(moveConstants.playerHalfWidth, moveConstants.playerHalfWidth, halfHeight);
        movement::TrackZones& track = zones.tracks[currentTrack];
        if (track.start.defined && !movement::hullInZone(track.start, center, half)) {
            showToast("set start only works inside the " + movement::trackName(currentTrack) + " start zone");
            return;
        }
        track.startPoint.defined = true;
        track.startPoint.feetPos = center - glm::vec3(0.0f, 0.0f, halfHeight);
        track.startPoint.yawDeg = camera.yawDegrees();
        track.startPoint.pitchDeg = camera.pitchDegrees();
        movement::saveZones(zonesPath, zones);
        showToast(movement::trackName(currentTrack) + " start position set");
    });
    keybinds.add("zone_start", "create start zone for the current track (walk to corner, press e twice)",
                 SDL_SCANCODE_Z, [&]() { beginZonePlacement(currentTrack, true); });
    keybinds.add("zone_end", "create end zone for the current track (walk to corner, press e twice)", SDL_SCANCODE_X,
                 [&]() { beginZonePlacement(currentTrack, false); });
    keybinds.add("zone_corner", "set zone corner (use, while creating a zone)", SDL_SCANCODE_E, [&]() {
        if (creatingTrack == -1) {
            return;
        }
        movement::TrackZones& track = zones.tracks[creatingTrack];
        movement::Zone& zone = creatingStart ? track.start : track.end;
        bool wasSettingFirstCorner = !zoneHasFirstCorner;
        glm::vec3 feet = camera.position - glm::vec3(0.0f, 0.0f, moveConstants.playerHalfHeight);
        if (moveMode == MoveMode::Play) {
            float halfHeight = playerState.ducking ? moveConstants.duckHalfHeight : moveConstants.playerHalfHeight;
            feet = playerState.position - glm::vec3(0.0f, 0.0f, halfHeight);
        }
        movement::setZoneCorner(zone, zoneHasFirstCorner, zoneFirstCorner, feet);
        if (!wasSettingFirstCorner) {
            if (creatingStart && track.startPoint.defined) {
                glm::vec3 half(moveConstants.playerHalfWidth, moveConstants.playerHalfWidth, moveConstants.playerHalfHeight);
                glm::vec3 center = track.startPoint.feetPos + glm::vec3(0.0f, 0.0f, half.z);
                if (!movement::hullInZone(track.start, center, half)) {
                    track.startPoint.defined = false;  // the old spot isn't in the new zone anymore
                }
            }
            showToast(movement::trackName(creatingTrack) + (creatingStart ? " start" : " end") + " zone placed");
            creatingTrack = -1;
            movement::saveZones(zonesPath, zones);
        }
    });
    keybinds.load(keybindsPath);

    glEnable(GL_DEPTH_TEST);

    int lastSelectedStyle = menuState.selectedStyle;
    std::string lastSettings = settingsKey(cvars);

    Uint64 lastTicks = SDL_GetTicksNS();
    float tickAccumulator = 0.0f;
    glm::vec3 previousPlayerPos = playerState.position;
    bool quitRequested = false;
    while (!quitRequested) {
        // a run is timed with one style and one set of movement settings, start to finish. changing either ends it
        if (std::string now = settingsKey(cvars); now != lastSettings) {
            lastSettings = now;
            lastSelectedStyle = -1;
        }
        if (menuState.selectedStyle != lastSelectedStyle) {
            lastSelectedStyle = menuState.selectedStyle;
            movement::resetRunState(playerState);
            movement::resetTimerRun(timer);  // a run is timed in one style, start to finish
            timer.wasInStartZone = false;
            timer.lastRunTimeSeconds = -1.0f;
        }

        {
            const std::string& replayStyle = movement::styleList()[menuState.selectedStyle].name;
            std::string key = *mapPath + "\n" + std::to_string(currentTrack) + "\n" + replayStyle;
            if (key != loadedReplayKey) {
                loadedReplayKey = key;
                bestReplay = {};
                replayPlaying = false;
                replayTime = 0.0f;
                movement::loadReplay(replayFilePath(*mapPath, currentTrack, replayStyle), bestReplay);
            }
        }

        bool wasCapturing = keybinds.isCapturing();
        std::optional<std::string> droppedMapPath;  // a map dragged onto the window this frame

        window.pollEvents(quitRequested, [&](const SDL_Event& event) {
            imgui.processEvent(event);
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                // dropping a map = file > open map, same reload below. SDL owns event.drop.data so copy it
                std::string dropped(event.drop.data);
                std::string lower = dropped;
                for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower.size() > 4 &&
                    (lower.compare(lower.size() - 4, 4, ".bsp") == 0 ||
                     lower.compare(lower.size() - 4, 4, ".vmf") == 0)) {
                    droppedMapPath = dropped;
                } else {
                    showToast("not a map: drop a .bsp or .vmf");
                }
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                // esc just backs out of the menu, not rebindable. it never quits the app, only file > exit or the X does
                if (event.key.key == SDLK_ESCAPE && !keybinds.isCapturing()) {
                    if (uiMode) {
                        uiMode = false;
                        window.setRelativeMouseMode(true);
                    }
                } else if ((event.key.key == SDLK_LEFTBRACKET || event.key.key == SDLK_RIGHTBRACKET) &&
                           !keybinds.isCapturing() && !uiMode) {
                    // [ and ] change sens without the menu. same range as the cvar, the old 0.01-2.0 clamp would drag a css sens down to 2 lol
                    float step = (event.key.key == SDLK_RIGHTBRACKET) ? 0.1f : -0.1f;
                    float newSens = std::clamp(cvars.get("sensitivity") + step, 0.1f, 20.0f);
                    cvars.set("sensitivity", newSens);
                    std::printf("sensitivity: %.3f\n", newSens);
                } else if ((event.key.key == SDLK_MINUS || event.key.key == SDLK_EQUALS) &&
                           !keybinds.isCapturing() && !uiMode) {
                    // - and = change fov, same idea as the sens brackets
                    float step = (event.key.key == SDLK_EQUALS) ? 5.0f : -5.0f;
                    float newFov = std::clamp(cvars.get("fov") + step, 60.0f, 130.0f);
                    cvars.set("fov", newFov);
                    std::printf("fov: %.1f\n", newFov);
                } else if (event.key.key == SDLK_R && (event.key.mod & SDL_KMOD_CTRL) && !keybinds.isCapturing() &&
                           !uiMode) {
                    // ctrl+r: back to the main course from any bonus.
                    currentTrack = 0;
                    restart();
                    showToast("track: main");
                } else if (event.key.key == SDLK_F && !keybinds.isCapturing() && !uiMode) {
                    // not in the keybind registry cause it only does one bare scancode per action and this is a ctrl pair. debug tools i wanted to keep
                    // f or ctrl+f always goes back to play from either noclip (used to jump from source nc into instant nc instead)
                    bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
                    bool inAnyNoclip = moveMode == MoveMode::InstantNoclip || moveMode == MoveMode::SourceNoclip;
                    if (ctrl) {
                        switchMoveMode(inAnyNoclip ? MoveMode::Play : MoveMode::InstantNoclip);
                    } else {
                        switchMoveMode(inAnyNoclip ? MoveMode::Play : MoveMode::SourceNoclip);
                    }
                } else {
                    keybinds.handleKeyDown(event.key.scancode);
                }
            }
        });

        if (wasCapturing && !keybinds.isCapturing()) {
            keybinds.save(keybindsPath);
        }

        // save settings when anything changes (sliders, [ ] - =, exec) but not mid drag or it rewrites the file every frame
        if (!ImGui::IsAnyItemActive()) {
            std::vector<float> current = cvarValues(cvars);
            if (current != savedCvarValues) {
                console::saveConfigFile(settingsPath, cvars);
                savedCvarValues = std::move(current);
            }
        }

        Uint64 nowTicks = SDL_GetTicksNS();
        const Uint64 frameStartNs = lastTicks;  // mouse samples are binned across this span
        float dt = std::min(static_cast<float>(nowTicks - lastTicks) * 1e-9f, 0.25f);
        lastTicks = nowTicks;
        const float frameDt = dt;

        if (replayPlaying && !bestReplay.empty()) {
            // playback owns the camera, no physics or movement keys in here
            if (!replayPaused) replayTime += dt * replaySpeed;
            const float end = bestReplay.duration();
            if (replayTime >= end) {  // loop, so you can watch a section over
                replayTime = 0.0f;
            }
            movement::ReplayFrame f = bestReplay.sample(replayTime);
            camera.position = f.position;
            camera.setYawDegrees(f.yawDeg);
            camera.setPitchDegrees(f.pitchDeg);
            // keep the player on the replay so the hud speed and the world match the screen
            playerState.position = f.position;
        } else if (!uiMode) {
            float mouseDx = 0.0f, mouseDy = 0.0f;
            window.consumeMouseDelta(mouseDx, mouseDy);
            window.consumeMouseSamples(mouseSamples);
            // play mode feeds the mouse to the tick loop a slice at a time, noclips have no ticks so they just turn
            if (moveMode != MoveMode::Play) camera.look(mouseDx, mouseDy);

            const bool* keys = SDL_GetKeyboardState(nullptr);
            // which of a/d went down last. polled once a frame not per tick (a frame can be several ticks)
            const bool aHeld = keys[SDL_SCANCODE_A], dHeld = keys[SDL_SCANCODE_D];
            if (aHeld && !prevAHeld) lastAdPressed = 1;
            if (dHeld && !prevDHeld) lastAdPressed = 2;
            prevAHeld = aHeld;
            prevDHeld = dHeld;

            const SDL_Scancode turnLeftKey = keybinds.keyFor("turn_left");
            const SDL_Scancode turnRightKey = keybinds.keyFor("turn_right");

            switch (moveMode) {
                case MoveMode::Play: {
                    // fixed rate ticks like a server so jumps and strafes don't depend on fps. camera is drawn between the last two ticks
                    tickAccumulator += frameDt;
                    // mouse is read once per frame but a frame can be several ticks. giving every tick the same yaw means they all strafe at the same angle, and airaccel only pays when you turn WHILE airborne, so gain tanks under the tickrate (2 ticks per sample costs ~148 u/s, 3 ~216 u/s over six jumps). so each tick gets its share of the frame's mouse instead, same gain at 60fps as 300. m_pertick 0 = old behaviour
                    int pendingTicks = static_cast<int>(tickAccumulator / tickInterval);
                    if (pendingTicks > 10) pendingTicks = 10;  // a hitch; the backlog is dropped below
                    if (pendingTicks <= 0 || mousePerTick == 0.0f) {
                        camera.look(mouseDx, mouseDy);
                        mouseDx = mouseDy = 0.0f;
                        pendingTicks = 0;
                    }
                    int ticksThisFrame = 0;
                    while (tickAccumulator >= tickInterval) {
                        if (++ticksThisFrame > 10) {  // a long hitch: drop the backlog rather than spiral
                            tickAccumulator = 0.0f;
                            break;
                        }
                        if (pendingTicks > 0) {
                            // each mouse report has the time the device sent it so it goes in the tick it actually landed in, not an even split. a flick in the last 2ms of a frame belongs to the last tick. falls back to even split for turnbind only / sampleless frames
                            if (!mouseSamples.empty()) {
                                const Uint64 span = nowTicks > frameStartNs ? nowTicks - frameStartNs : 1;
                                const int k = ticksThisFrame - 1;  // 0-based index of this tick
                                const Uint64 lo = frameStartNs + span * k / pendingTicks;
                                const Uint64 hi = frameStartNs + span * (k + 1) / pendingTicks;
                                const bool last = (k + 1) >= pendingTicks;
                                float dx = 0.0f, dy = 0.0f;
                                for (const auto& s : mouseSamples) {
                                    // first tick has no lower bound and last has no upper so stamps outside the frame aren't dropped. with one tick it's both and takes everything (getting that wrong silently ate input)
                                    const bool inWindow =
                                        (k == 0 || s.timestampNs >= lo) && (last || s.timestampNs < hi);
                                    if (inWindow) {
                                        dx += s.dx;
                                        dy += s.dy;
                                    }
                                }
                                camera.look(dx, dy);
                            } else {
                                camera.look(mouseDx / pendingTicks, mouseDy / pendingTicks);
                            }
                        }
                        // turnbinds are degrees per second applied per tick like the mouse so same speed at any fps. both held cancels
                        if (turnLeftKey != SDL_SCANCODE_UNKNOWN || turnRightKey != SDL_SCANCODE_UNKNOWN) {
                            float turn = 0.0f;
                            if (turnLeftKey != SDL_SCANCODE_UNKNOWN && keys[turnLeftKey]) turn += yawSpeed;
                            if (turnRightKey != SDL_SCANCODE_UNKNOWN && keys[turnRightKey]) turn -= yawSpeed;
                            if (turn != 0.0f) camera.turn(turn * tickInterval);
                        }
                        tickAccumulator -= tickInterval;
                        const float dt = tickInterval;
                        previousPlayerPos = playerState.position;
                        movement::MoveInput input;
                        input.forward = keys[SDL_SCANCODE_W];
                        input.back = keys[SDL_SCANCODE_S];
                        input.left = keys[SDL_SCANCODE_A];
                        input.right = keys[SDL_SCANCODE_D];
                        input.jump = keys[SDL_SCANCODE_SPACE];
                        input.duck = keys[SDL_SCANCODE_LCTRL];
                        // null binds, same as the standalone tool. a+d both down = whichever was pressed last, and w stops counting the whole time jump is held, not just on the jump
                        if (nullAD != 0.0f && input.left && input.right) {
                            input.left = lastAdPressed == 1;
                            input.right = lastAdPressed == 2;
                        }
                        if (releaseWOnJump != 0.0f && input.forward && input.jump) {
                            input.forward = false;
                        }
                        input.attack = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
                        input.yawDeg = camera.yawDegrees();
                        const movement::StyleDef& activeStyle = movement::styleList()[menuState.selectedStyle];
                        movement::tickPlayerMove(playerState, input, moveConstants, activeStyle, loaded->brushes, dt);
                        if (playerState.justJumped) playSound(audio::SoundEvent::Jump);
                        auto triggerResult =
                            movement::tickTriggers(triggerRuntime, loaded->triggers, playerState, moveConstants, dt,
                                                    recorrectTeleportMomentum != 0.0f);
                        camera.position = playerState.position;
                        if (playerState.hasYawOverride) {
                            camera.setYawDegrees(playerState.yawOverrideDeg);
                        }
                        if (triggerResult.setView) {
                            camera.setYawDegrees(triggerResult.yawDeg);
                            camera.setSourcePitchDegrees(triggerResult.pitchDeg);
                        }
                        if (triggerResult.teleported) playSound(audio::SoundEvent::Teleport);
                        if (triggerResult.killed || playerState.position.z < killZ) {
                            // like respawning but back at your track's start
                            playSound(audio::SoundEvent::Death);
                            restart(true);
                            continue;
                        }
                        glm::vec3 hullHalfExtents(moveConstants.playerHalfWidth, moveConstants.playerHalfWidth,
                                                  playerState.ducking ? moveConstants.duckHalfHeight
                                                                      : moveConstants.playerHalfHeight);
                        movement::tickTimer(timer, zones, playerState.position, hullHalfExtents,
                                            playerState.grounded, playerState.justJumped, playerState.speedGainedInAir,
                                            dt);
                        currentTrack = timer.track;  // walking into a bonus's start puts you on that bonus
                        if (timer.justStarted) {
                            playSound(audio::SoundEvent::RunStart);
                            recordingReplay = {};
                            recordingReplay.tickInterval = tickInterval;
                        }
                        // one frame per tick while the clock runs so playback is the same physics steps
                        if (timer.running) {
                            recordingReplay.frames.push_back({playerState.position, camera.yawDegrees(),
                                                              camera.pitchDegrees(), playerState.ducking});
                        }
                        if (timer.justFinished) {
                            const std::string& style = movement::styleList()[menuState.selectedStyle].name;
                            std::string settings = settingsKey(cvars);
                            float previous = personalBests.get(timer.track, style, settings);
                            float time = timer.lastRunTimeSeconds;
                            lastRunDelta = previous >= 0.0f ? time - previous
                                                            : std::numeric_limits<float>::quiet_NaN();
                            std::string what = movement::trackName(timer.track) + " (" + style + ")";
                            if (personalBests.submit(timer.track, style, settings, time)) {
                                personalBests.save(timesPath);
                                // the run that set the time is the one worth keeping so it gets saved with it
                                recordingReplay.seconds = time;
                                recordingReplay.track = timer.track;
                                recordingReplay.style = style;
                                recordingReplay.settings = settings;
                                if (!recordingReplay.empty()) {
                                    movement::saveReplay(replayFilePath(*mapPath, timer.track, style),
                                                          recordingReplay);
                                    bestReplay = recordingReplay;
                                }
                                playSound(audio::SoundEvent::PersonalBest);
                                if (previous >= 0.0f) {
                                    char delta[32];
                                    std::snprintf(delta, sizeof(delta), "%+.2f", time - previous);
                                    showToast("new personal best on " + what + ": " + ui::formatRunTime(time) + " (" +
                                              delta + ")");
                                } else {
                                    showToast("first finish on " + what + ": " + ui::formatRunTime(time));
                                }
                            } else {
                                playSound(audio::SoundEvent::Finish);
                                showToast("finished " + what + " in " + ui::formatRunTime(time));
                            }
                        }

                        lastHorizontalSpeed = glm::length(glm::vec2(playerState.velocity.x, playerState.velocity.y));
                        ui::tickJhud(jhud, playerState.justJumped, playerState.grounded, lastHorizontalSpeed,
                                     playerState.airGain, playerState.airGainMax, playerState.speedGainedInAir, dt);
                        bool strafing = input.left != input.right;
                        ui::tickStrafeTrainer(strafeTrainerState, camera.yawDegrees(), lastHorizontalSpeed,
                                               moveConstants.maxSpeed, moveConstants.airSpeedCap, dt,
                                               !playerState.grounded, strafing);
                    }
                    {
                        // teleports and restarts snap instead of sliding
                        float alpha = tickAccumulator / tickInterval;
                        bool jumped = glm::length(playerState.position - previousPlayerPos) > 128.0f;
                        camera.position = jumped ? playerState.position
                                                 : glm::mix(previousPlayerPos, playerState.position, alpha);
                    }
                    break;
                }
                case MoveMode::InstantNoclip: {
                    render::FreeFlyCamera::MoveInput move;
                    move.forward = keys[SDL_SCANCODE_W];
                    move.back = keys[SDL_SCANCODE_S];
                    move.left = keys[SDL_SCANCODE_A];
                    move.right = keys[SDL_SCANCODE_D];
                    move.up = keys[SDL_SCANCODE_SPACE];
                    move.down = keys[SDL_SCANCODE_LCTRL];
                    camera.setMoveSpeed(keys[SDL_SCANCODE_LSHIFT] ? instantNoclipSpeed + kInstantNoclipShiftBoost
                                                                   : instantNoclipSpeed);
                    glm::vec3 before = camera.position;
                    camera.update(move, dt);
                    instantNoclipVelocity = dt > 0.0f ? (camera.position - before) / dt : glm::vec3(0.0f);
                    break;
                }
                case MoveMode::SourceNoclip: {
                    movement::NoclipInput input;
                    input.forward = keys[SDL_SCANCODE_W];
                    input.back = keys[SDL_SCANCODE_S];
                    input.left = keys[SDL_SCANCODE_A];
                    input.right = keys[SDL_SCANCODE_D];
                    input.up = keys[SDL_SCANCODE_SPACE];
                    input.down = keys[SDL_SCANCODE_LCTRL];
                    input.yawDeg = camera.yawDegrees();
                    input.pitchDeg = camera.pitchDegrees();
                    movement::tickNoclip(sourceNoclipState, input, kSourceNoclipSpeed,
                                          moveConstants.accelerate, moveConstants.friction, dt);
                    camera.position = sourceNoclipState.position;
                    break;
                }
            }
        } else {
            float consumedDx = 0.0f, consumedDy = 0.0f;
            window.consumeMouseDelta(consumedDx, consumedDy);  // drop camera-look while in the UI
        }

        imgui.newFrame();
        // times menu: this track's times, fastest first
        std::vector<movement::PersonalBests::Entry> trackTimes = personalBests.list(timer.track);
        {
            std::string settingsNow = settingsKey(cvars);
            const std::string& styleNow = movement::styleList()[menuState.selectedStyle].name;
            menuState.timesHeader = std::filesystem::path(*mapPath).stem().string() + " - " +
                                    movement::trackName(timer.track) + "  (green: your style and settings now)";
            menuState.times.clear();
            for (const auto& t : trackTimes) {
                ui::MenuBarState::TimeRow row;
                row.label = ui::formatRunTime(t.seconds) + "   " + t.style;
                row.details = describeSettings(t.settings, cvars);
                if (t.settings.empty()) row.label += "   (settings not recorded)";
                else if (row.details != "default settings") row.label += "   (" + row.details + ")";
                row.current = t.style == styleNow && t.settings == settingsNow;
                row.canApply = !t.settings.empty();
                menuState.times.push_back(std::move(row));
            }
        }
        menuState.nullAD = nullAD != 0.0f;
        menuState.releaseWOnJump = releaseWOnJump != 0.0f;
        menuState.recorrectTeleportMomentum = recorrectTeleportMomentum != 0.0f;
        menuState.hasReplay = !bestReplay.empty();
        menuState.showClips = showClips != 0.0f;
        menuState.showTriggers = showTriggers != 0.0f;
        drawMenuBar(menuState);  // always visible; only clickable once uiMode frees the cursor
        // back through the cvars so the menu and console agree and it saves like any other setting
        if (menuState.nullAD != (nullAD != 0.0f)) cvars.set("null_ad", menuState.nullAD ? 1.0f : 0.0f);
        if (menuState.releaseWOnJump != (releaseWOnJump != 0.0f))
            cvars.set("w_release_on_jump", menuState.releaseWOnJump ? 1.0f : 0.0f);
        if (menuState.recorrectTeleportMomentum != (recorrectTeleportMomentum != 0.0f))
            cvars.set("tp_recorrect", menuState.recorrectTeleportMomentum ? 1.0f : 0.0f);
        if (menuState.showClips != (showClips != 0.0f)) cvars.set("r_showclips", menuState.showClips ? 1.0f : 0.0f);
        if (replayPlaying && !bestReplay.empty()) {
            ImGuiIO& rio = ImGui::GetIO();
            const float barWidth = std::min(rio.DisplaySize.x - 32.0f, 900.0f);
            ImGui::SetNextWindowPos(ImVec2(rio.DisplaySize.x * 0.5f, 34.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(barWidth, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##replay_bar", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            const float end = bestReplay.duration();
            ImGui::Text("replay  %s  %s", bestReplay.style.c_str(), ui::formatRunTime(bestReplay.seconds).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(replayPaused ? "play" : "pause")) replayPaused = !replayPaused;
            ImGui::SameLine();
            if (ImGui::SmallButton("restart")) replayTime = 0.0f;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::SliderFloat("##speed", &replaySpeed, 0.1f, 4.0f, "%.2fx");
            ImGui::SameLine();
            if (ImGui::SmallButton(videoRecorder.recording() ? "stop rec" : "record")) {
                // start from the top so the clip is the whole run
                if (!videoRecorder.recording()) {
                    replayTime = 0.0f;
                    replayPaused = false;
                }
                char suffix[96];
                std::snprintf(suffix, sizeof(suffix), "t%d_%s_%.2f", bestReplay.track, bestReplay.style.c_str(),
                              bestReplay.seconds);
                toggleRecording(suffix);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("exit")) replayPlaying = false;

            // scrubbing pauses or playback fights the drag
            ImGui::SetNextItemWidth(-1.0f);
            float scrub = replayTime;
            if (ImGui::SliderFloat("##scrub", &scrub, 0.0f, std::max(end, 0.001f),
                                   (ui::formatRunTime(replayTime) + " / " + ui::formatRunTime(end)).c_str())) {
                replayTime = scrub;
                replayPaused = true;
            }
            ImGui::End();
        }
        if (menuState.showTriggers != (showTriggers != 0.0f))
            cvars.set("r_showtriggers", menuState.showTriggers ? 1.0f : 0.0f);
        if (menuState.applyTimeClicked >= 0) {
            const auto& t = trackTimes[static_cast<size_t>(menuState.applyTimeClicked)];
            menuState.applyTimeClicked = -1;
            std::istringstream iss(t.settings);
            std::string part;
            while (iss >> part) {
                size_t eq = part.find('=');
                if (eq == std::string::npos) continue;
                std::string name = part.substr(0, eq), value = part.substr(eq + 1);
                // times store values as written (%g), skip ones that already read the same (301.993377 stays, not 301.993)
                char now[64];
                std::snprintf(now, sizeof(now), "%g", cvars.get(name));
                if (value != now) cvars.set(name, std::strtof(value.c_str(), nullptr));
            }
            for (size_t i = 0; i < movement::styleList().size(); ++i) {
                if (movement::styleList()[i].name == t.style) menuState.selectedStyle = static_cast<int>(i);
            }
            showToast("style and settings set to the ones from your " + ui::formatRunTime(t.seconds));
        }
        ui::drawKeybindEditor(keybinds, menuState.showKeybindsWindow);
        ui::drawSettingsPanel(cvars, menuState.showSettingsWindow);
        {
            ui::SoundsWindowResult sr = ui::drawSoundsWindow(soundConfig, appDir, menuState.showSoundsWindow);
            if (sr.changed) audio::saveSoundConfig(soundsPath, soundConfig);
            if (sr.testEvent >= 0) {
                auto event = static_cast<audio::SoundEvent>(sr.testEvent);
                std::filesystem::path file = audio::resolveSoundPath(soundConfig.path(event), appDir);
                sounds.forget(file);  // pick up a file that was swapped on disk
                sounds.play(file, soundConfig.volume);
            }
        }
        {
            ui::ZonesWindowResult zr = ui::drawZonesWindow(zones, currentTrack, creatingTrack, creatingStart,
                                                           menuState.showZonesWindow);
            if (zr.changed) {
                movement::saveZones(zonesPath, zones);
                if (!zones.tracks[currentTrack].used()) currentTrack = 0;
                movement::switchTimerTrack(timer, currentTrack);
            }
            if (zr.placeTrack >= 0) {
                beginZonePlacement(zr.placeTrack, zr.placeStart);
                if (zr.placeStart || zones.tracks[zr.placeTrack].start.defined) {
                    currentTrack = zr.placeTrack;
                    movement::switchTimerTrack(timer, currentTrack);
                }
                showToast("placing " + movement::trackName(zr.placeTrack) + (zr.placeStart ? " start" : " end") +
                          " zone: press e at two corners");
            }
            if (zr.goTrack >= 0) {
                currentTrack = zr.goTrack;
                restart();
            }
        }

        if (moveMode == MoveMode::Play && !uiMode) {
            ui::drawStrafeTrainerBar(strafeTrainerState, !playerState.grounded);
            std::string hudLabel = movement::styleList()[menuState.selectedStyle].name;
            if (timer.track != 0) hudLabel += "  |  " + movement::trackName(timer.track);
            ui::drawRunHud(timer, hudLabel, lastHorizontalSpeed,
                           personalBests.get(timer.track, movement::styleList()[menuState.selectedStyle].name,
                                             settingsKey(cvars)),
                           lastRunDelta);
            if (showFps != 0.0f) {
                // eased or it flickers through every value each frame
                float instant = frameDt > 0.0f ? 1.0f / frameDt : 0.0f;
                fpsSmoothed = fpsSmoothed <= 0.0f ? instant : fpsSmoothed + (instant - fpsSmoothed) * 0.1f;
                ImGuiIO& fpsIo = ImGui::GetIO();
                ImGui::SetNextWindowPos(ImVec2(fpsIo.DisplaySize.x - 8.0f, 28.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGui::SetNextWindowBgAlpha(0.0f);
                ImGui::Begin("##fps", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoMove);
                ImGui::Text("%.0f fps", fpsSmoothed);
                ImGui::End();
            }
            if (menuState.showJhud) {
                ui::drawJhud(jhud);
            }
        }

        // zone creation only printed to the console before, which you can't see while playing. that's why it felt broken, not setZoneCorner
        if (creatingTrack != -1) {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.3f), ImGuiCond_Always,
                                     ImVec2(0.5f, 0.5f));
            ImGui::Begin("##zone_creation", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoMove);
            ImGui::Text("creating %s %s zone", movement::trackName(creatingTrack).c_str(),
                        creatingStart ? "start" : "end");
            if (!zoneHasFirstCorner) {
                ImGui::Text("press e to set the first corner");
            } else {
                ImGui::Text("corner 1 set -- walk to the opposite corner and press e again");
            }
            ImGui::End();
        } else if (moveMode == MoveMode::Play && !uiMode &&
                   (!zones.tracks[currentTrack].start.defined || !zones.tracks[currentTrack].end.defined)) {
            auto keyName = [&](const char* action) -> const char* {
                for (const auto& bind : keybinds.all()) {
                    if (bind.action == action) return SDL_GetScancodeName(bind.key);
                }
                return "?";
            };
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##zone_hint", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoMove);
            std::string trackLabel = movement::trackName(currentTrack);
            const char* track = trackLabel.c_str();
            if (!zones.tracks[currentTrack].start.defined) {
                ImGui::Text("no %s start zone - press %s to place one", track, keyName("zone_start"));
            }
            if (!zones.tracks[currentTrack].end.defined) {
                ImGui::Text("no %s end zone - press %s to place one", track, keyName("zone_end"));
            }
            ImGui::End();
        }

        if (toastTimer > 0.0f) {
            toastTimer -= dt;
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always,
                                     ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##toast", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoMove);
            ImGui::TextUnformatted(toastText.c_str());
            ImGui::End();
        }

        if (modeOverlayTimer > 0.0f) {
            modeOverlayTimer -= dt;
            float fadeStart = 0.5f;  // last half-second ramps alpha down instead of popping off
            float alpha = (modeOverlayTimer < fadeStart) ? std::max(modeOverlayTimer / fadeStart, 0.0f) : 1.0f;

            ImGui::SetNextWindowPos(ImVec2(8.0f, 28.0f));
            ImGui::SetNextWindowBgAlpha(0.35f * alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::Begin("##mode_overlay", nullptr,
                          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
            ImGui::Text("mode: %s  (f: source noclip, ctrl+f: instant noclip, r: restart)", moveModeLabel(moveMode));
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (menuState.replayClicked) {
            menuState.replayClicked = false;
            keybinds.handleKeyDown(keybinds.keyFor("replay"));  // one implementation, not two
        }

        if (menuState.openMapClicked || droppedMapPath) {
            const bool fromDrop = droppedMapPath.has_value();
            menuState.openMapClicked = false;
            // a drag already named the file, only the menu needs to ask
            auto newPath = fromDrop ? droppedMapPath : platform::chooseMapFile();
            droppedMapPath.reset();
            if (newPath) {
                if (auto reloaded = loadMap(*newPath)) {
                    loaded = std::move(reloaded);
                    mapPath = newPath;
                    mapWriteTime = std::filesystem::last_write_time(*mapPath, mapTimeError);
                    pendingWriteTime.reset();
                    destroyRenderGroups(renderGroups);
                    renderGroups = buildRenderGroups(*loaded, gameContent);
                    buildToolBrushes();
                    uploadLightmap();
                    spawnPosition = loaded->spawnPosition;
                    spawnYawDeg = loaded->spawnYawDeg;
                    camera.position = spawnPosition;
                    camera.setYawDegrees(spawnYawDeg);
                    playerState.position = spawnPosition;
                    playerState.velocity = glm::vec3(0.0f);
                    movement::resetRunState(playerState);
                    sourceNoclipState.position = spawnPosition;
                    sourceNoclipState.velocity = glm::vec3(0.0f);
                    timer = movement::TimerState{};
                    loadZonesForMap();
                    creatingTrack = -1;
                    zoneHasFirstCorner = false;
                    movement::resetTriggerRuntime(triggerRuntime, loaded->triggers);
                    if (fromDrop) {
                        showToast("loaded " + std::filesystem::path(*newPath).filename().string());
                    }
                } else {
                    // getting nothing from the dialog is confusing enough, getting nothing from a drop gives no clue it registered
                    showToast("couldn't load " + std::filesystem::path(*newPath).filename().string());
                }
            }
        }
        if (menuState.mountCssClicked) {
            menuState.mountCssClicked = false;
            if (auto folder = platform::chooseFolder("pick your Counter-Strike Source folder")) {
                if (content::isCssInstall(*folder)) {
                    content::saveCssPath(contentConfigPath(), *folder);
                    gameContent.mount(*folder);
                    destroyRenderGroups(renderGroups);
                    renderGroups = buildRenderGroups(*loaded, gameContent);
                    buildToolBrushes();
                    showToast("CS:S content mounted");
                } else {
                    showToast("that folder has no cstrike/cstrike_pak_dir.vpk");
                }
            }
        }

        if (menuState.loadPackClicked) {
            menuState.loadPackClicked = false;
            if (auto file = platform::chooseZipToOpen("pick a CS:S texture pack")) {
                if (gameContent.mountPack(*file)) {
                    content::saveTexturePackPath(contentConfigPath(), *file);
                    content::saveCssPath(contentConfigPath(), {});  // the pack is the choice now
                    destroyRenderGroups(renderGroups);
                    renderGroups = buildRenderGroups(*loaded, gameContent);
                    buildToolBrushes();
                    showToast("texture pack loaded");
                } else {
                    showToast("couldn't open that texture pack");
                }
            }
        }
        if (menuState.exportPackClicked) {
            menuState.exportPackClicked = false;
            if (gameContent.root().empty()) {
                showToast("exporting needs CS:S itself: file > cs:s folder... first");
            } else if (exportThread.joinable()) {
                showToast("already exporting");
            } else if (auto file = platform::chooseZipToSave("save the texture pack as", (appDir / content::kDefaultPackName).string())) {
                exportProgress.done = 0;
                exportProgress.total = 0;
                exportProgress.finished = false;
                exportProgress.failed = false;
                exportThread = std::thread([root = gameContent.root(), path = *file, &exportProgress]() {
                    content::exportTexturePack(root, path, exportProgress);
                });
                exportPath = *file;
            }
        }
        if (exportThread.joinable()) {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.15f), ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::Begin("##export", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing);
            size_t total = exportProgress.total, done = exportProgress.done;
            ImGui::Text("exporting CS:S textures... %zu / %zu files", done, total);
            ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(320, 0));
            ImGui::End();
            if (exportProgress.finished) {
                exportThread.join();
                showToast(exportProgress.failed ? "export failed (disk full, or no write access there?)"
                                                : "texture pack saved: " + exportPath);
            }
        }

        if (menuState.openEditorClicked) {
            menuState.openEditorClicked = false;
            std::vector<std::string> args;
            args.push_back(*mapPath);  // a .bsp gets decompiled into the editor
            if (!platform::launchSibling("movengine_editor", args)) {
                showToast("couldn't start the editor (movengine_editor next to this exe?)");
            }
        }

        // live reload: editor saves the .vmf you're playing and the map swaps in place, you stay where you are. waits for the write time to hold still across two checks so half written saves don't load
        if (loaded->isVmf) {
            reloadCheckTimer -= dt;
            if (reloadCheckTimer <= 0.0f) {
                reloadCheckTimer = 0.25f;
                std::error_code ec;
                auto writeTime = std::filesystem::last_write_time(*mapPath, ec);
                if (!ec && writeTime != mapWriteTime) {
                    if (pendingWriteTime && *pendingWriteTime == writeTime) {
                        pendingWriteTime.reset();
                        if (auto reloaded = loadMap(*mapPath)) {
                            mapWriteTime = writeTime;
                            loaded = std::move(reloaded);
                            destroyRenderGroups(renderGroups);
                            renderGroups = buildRenderGroups(*loaded, gameContent);
                            buildToolBrushes();
                            uploadLightmap();
                            spawnPosition = loaded->spawnPosition;
                            spawnYawDeg = loaded->spawnYawDeg;
                            movement::resetTriggerRuntime(triggerRuntime, loaded->triggers);
                            loadZonesForMap();
                            showToast("map updated");
                        }
                    } else {
                        pendingWriteTime = writeTime;
                    }
                }
            }
        }

        if (menuState.execConfigClicked) {
            menuState.execConfigClicked = false;
            if (auto cfgPath = platform::chooseConfigFile()) {
                console::execConfigFile(*cfgPath, cvars);
            }
        }
        if (menuState.exitClicked) {
            quitRequested = true;
        }

        glClearColor(kSkyColor.r, kSkyColor.g, kSkyColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float aspect = static_cast<float>(window.width()) / static_cast<float>(window.height());
        // far plane at draw distance, or past the biggest maps when unlimited
        glm::mat4 viewProj =
            camera.projectionMatrix(aspect, 3.0f, drawDistance > 0.0f ? drawDistance : 65536.0f) * camera.viewMatrix();

        glUseProgram(program);
        glUniformMatrix4fv(uViewProjLoc, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform1i(uTextureLoc, 0);
        lightingUniforms.apply(lightingModeFor(*loaded), lightmapTexture.id(), loaded->lighting, camera.position);
        glUniform1f(uFogEndLoc, drawDistance);
        glUniform3fv(uFogColorLoc, 1, glm::value_ptr(kSkyColor));
        glUniform1f(uExposureLoc, brightness);
        glUniform3fv(uViewPosLoc, 1, glm::value_ptr(camera.position));
        // opaque and cutout first, depth writes on. $alphatest isn't blending, a texel is there or it isn't, so the depth buffer sorts it
        std::vector<const RenderGroup*> translucentGroups;
        for (const auto& rg : renderGroups) {
            if (rg.translucent) {
                translucentGroups.push_back(&rg);
                continue;
            }
            glUniform1f(uAlphaTestRefLoc, rg.alphaTest ? rg.alphaTestRef : 0.0f);
            rg.texture.bind(0);
            rg.mesh.draw();
        }
        glUniform1f(uAlphaTestRefLoc, 0.0f);

        // then blended ones, furthest first. sorting whole materials not triangles gets glass behind glass right on most maps without a per frame tri sort. depth writes off so they don't hide each other
        if (!translucentGroups.empty()) {
            std::sort(translucentGroups.begin(), translucentGroups.end(),
                       [&](const RenderGroup* a, const RenderGroup* b) {
                           return glm::dot(a->center - camera.position, a->center - camera.position) >
                                  glm::dot(b->center - camera.position, b->center - camera.position);
                       });
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            for (const RenderGroup* rg : translucentGroups) {
                rg->texture.bind(0);
                rg->mesh.draw();
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        if ((showClips != 0.0f && clipBrushCount > 0) || (showTriggers != 0.0f && triggerBrushCount > 0)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);  // see the ones behind, and the world through them
            // brushPolygons winds faces ccw from outside so culling back faces leaves only ones pointing at you. text reads the right way round and standing inside a trigger (bhop maps wrap the whole course in one) doesn't wash the screen
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            // lighting mode 0, these are volumes not lit surfaces. plain directional shading keeps every face readable
            lightingUniforms.apply(0, 0, loaded->lighting, camera.position, 0.4f);
            if (showClips != 0.0f && clipBrushCount > 0) {
                clipGroup.texture.bind(0);
                clipGroup.mesh.draw();
            }
            if (showTriggers != 0.0f && triggerBrushCount > 0) {
                triggerGroup.texture.bind(0);
                triggerGroup.mesh.draw();
            }
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);  // translucent, don't let zone boxes occlude each other or the world
            // only faces pointing at you. standing in a zone used to draw the far side over the whole screen
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glUseProgram(debugBoxProgram);
            glUniformMatrix4fv(uDebugViewProjLoc, 1, GL_FALSE, glm::value_ptr(viewProj));
            for (int t = 0; t < movement::kMaxTracks; ++t) {
                const movement::TrackZones& track = zones.tracks[t];
                // shavit's colours: green/red for the main course, blue/purple for bonuses.
                bool bonus = t != 0;
                if (track.start.defined) {
                    if (bonus) glUniform4f(uDebugColorLoc, 0.2f, 0.55f, 1.0f, 0.35f);
                    else glUniform4f(uDebugColorLoc, 0.2f, 1.0f, 0.3f, 0.35f);
                    debugBox.draw(uDebugModelLoc, track.start.min, track.start.max);
                }
                if (track.end.defined) {
                    if (bonus) glUniform4f(uDebugColorLoc, 0.75f, 0.3f, 1.0f, 0.35f);
                    else glUniform4f(uDebugColorLoc, 1.0f, 0.25f, 0.2f, 0.35f);
                    debugBox.draw(uDebugModelLoc, track.end.min, track.end.max);
                }
            }
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        imgui.render();

        // after everything's drawn, before the swap, so the back buffer is the finished frame. on video_fps's own clock or a 300fps game hands ffmpeg 300 frames a sec labelled 60 (clip 5x too fast)
        if (videoRecorder.recording()) {
            const float interval = 1.0f / std::max(videoFps, 1.0f);
            videoAccum += frameDt;
            // how many recorded frames this real time is worth. above the rec rate it's 0 most frames (skip), below it more than one (dupe) so the clip stays realtime. capped so one long hitch can't dump a second of dupes
            int due = static_cast<int>(videoAccum / interval);
            if (due > 0) {
                videoAccum -= static_cast<float>(due) * interval;
                videoRecorder.captureFrame(std::min(due, 8));
            }
        }

        discord.setActivity(kDiscordDetails,
                            "playing " + std::filesystem::path(*mapPath).stem().string());
        discord.update();
        window.swap();
        // fps_max: sleep out the rest of the frame. uncapped by default and the 100t tick loop doesn't care
        if (fpsMax > 0.0f) {
            const Uint64 target = static_cast<Uint64>(1.0e9 / fpsMax);
            const Uint64 spent = SDL_GetTicksNS() - nowTicks;
            if (spent < target) SDL_DelayNS(target - spent);
        }
    }

    if (exportThread.joinable()) exportThread.join();  // a half-written pack is never left behind, but let it finish
    keybinds.save(keybindsPath);
    console::saveConfigFile(settingsPath, cvars);
    videoRecorder.stop();  // finalises the mp4; ffmpeg needs its stdin closed
    imgui.shutdown();
    clipGroup.mesh.destroy();
    clipGroup.texture.destroy();
    triggerGroup.mesh.destroy();
    triggerGroup.texture.destroy();
    destroyRenderGroups(renderGroups);
    lightmapTexture.destroy();
    debugBox.destroy();
    glDeleteProgram(program);
    glDeleteProgram(debugBoxProgram);
    window.shutdown();
    return 0;
}
