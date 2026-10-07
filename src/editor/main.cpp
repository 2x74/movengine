#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "collision/map_triggers.h"
#include "content/builtin_textures.h"
#include "content/game_content.h"
#include "content/materials.h"
#include "editor/editor.h"
#include "platform/file_dialog.h"
#include "platform/process.h"
#include "platform/discord.h"
#include "platform/user_files.h"
#include "platform/window.h"
#include "render/camera.h"
#include "render/debug_box.h"
#include "render/lighting.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/shaders.h"
#include "render/texture.h"
#include "ui/imgui_layer.h"
#include "vmf/geometry.h"
#include "vmf/bsp_compile.h"
#include "vmf/bsp_import.h"
#include "vmf/playtest.h"

namespace {

std::string upperAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// full size textures for materials in use, resolved through the map's sources (a decompiled map's source .bsp, css if mounted, built-ins, else the "MT" texture). kept across scene rebuilds, cleared when the content changes
class TextureCache {
public:
    struct Entry {
        render::Texture texture;
        glm::vec2 uvSize{512.0f};
        content::ResolvedMaterial::Source source = content::ResolvedMaterial::Source::Missing;
    };

    explicit TextureCache(const content::GameContent& game) : game_(game) {}
    ~TextureCache() { clear(); }

    Entry& get(const std::string& material) {
        std::string key = upperAscii(material);
        auto it = entries_.find(key);
        if (it != entries_.end()) return it->second;
        content::ResolvedMaterial resolved = content::resolveMaterial(material, &game_, pak_);
        Entry& e = entries_[key];
        e.texture.upload(resolved.pixels);
        e.uvSize = resolved.uvSize;
        e.source = resolved.source;
        return e;
    }

    void clear() {
        for (auto& [k, e] : entries_) e.texture.destroy();
        entries_.clear();
    }

    // the .bsp whose packed textures come first ("" for none)
    void setPakSource(const std::string& bspPath) {
        if (bspPath == pakSource_) return;
        pakSource_ = bspPath;
        pak_ = bspPath.empty() ? nullptr : content::bspPakReader(bspPath);
        clear();
    }

private:
    const content::GameContent& game_;
    std::string pakSource_;
    content::FileReader pak_;
    std::map<std::string, Entry> entries_;
};

// 64px previews for the texture browser, downscaled on the cpu so browsing thousands of css textures doesn't keep thousands of full size ones on the gpu
class ThumbnailCache {
public:
    explicit ThumbnailCache(const content::GameContent& game) : game_(game) {}
    ~ThumbnailCache() { clear(); }

    // null until loaded, loads at most `budget` new ones per call site per frame
    const render::Texture* get(const std::string& material, int& budget) {
        auto it = thumbs_.find(material);
        if (it != thumbs_.end()) return &it->second;
        if (budget <= 0) return nullptr;
        budget--;
        content::ResolvedMaterial resolved = content::resolveMaterial(material, &game_);
        render::Texture& t = thumbs_[material];
        t.upload(downscale(resolved.pixels, 64));
        return &t;
    }

    void clear() {
        for (auto& [k, t] : thumbs_) t.destroy();
        thumbs_.clear();
    }

private:
    static bsp::DecodedTexture downscale(const bsp::DecodedTexture& src, int size) {
        bsp::DecodedTexture out;
        out.width = out.height = size;
        out.rgba8888.resize(static_cast<size_t>(size) * size * 4);
        if (src.width <= 0 || src.height <= 0) return out;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int sx = x * src.width / size, sy = y * src.height / size;
                const uint8_t* p = &src.rgba8888[(static_cast<size_t>(sy) * src.width + sx) * 4];
                std::copy(p, p + 4, &out.rgba8888[(static_cast<size_t>(y) * size + x) * 4]);
            }
        }
        return out;
    }

    const content::GameContent& game_;
    std::map<std::string, render::Texture> thumbs_;
};

struct RenderGroup {
    render::Mesh mesh;
    GLuint texture = 0;  // owned by the TextureCache
    bool translucent = false;
};

struct SceneMeshes {
    std::vector<RenderGroup> groups;

    void destroy() {
        for (auto& g : groups) g.mesh.destroy();
        groups.clear();
    }

    void rebuild(const vmf::Document& doc, TextureCache& textures) {
        destroy();
        vmf::MaterialTriangles tris;
        // hidden means not drawn. Editor::raycast skips the same ones so what you can't see is also what you can't click
        for (const auto& s : doc.world.solids) {
            if (!vmf::isHidden(s)) vmf::appendSolidTriangles(s, true, tris, 1.0f);
        }
        for (const auto& e : doc.entities) {
            if (vmf::isHidden(e)) continue;
            for (const auto& s : e.solids) vmf::appendSolidTriangles(s, true, tris, 1.0f);
        }
        for (auto& [material, list] : tris) {
            TextureCache::Entry& tex = textures.get(material);
            for (auto& v : list) v.uv /= tex.uvSize;  // texel UVs -> this texture's size
            RenderGroup g;
            g.mesh.upload(list);
            g.texture = tex.texture.id();
            g.translucent = vmf::isToolMaterial(material);
            groups.push_back(std::move(g));
        }
    }
};

// the discord application this presence belongs to, and the headline it shows. rich presence is best effort: discord not running is the normal case and nothing here waits on it
inline constexpr const char* kDiscordAppId = "1557069026407227412";
inline constexpr const char* kDiscordDetails = "movengine - https://mve.1998.lol";
// the art asset in the dev portal, shown as the big image
inline constexpr const char* kDiscordLargeImage = "448004ebee2ca1e66b499429785f8e27";

std::string prefDir() {
    return platform::prefDir();
}

std::string contentConfigPath() { return prefDir() + "content.cfg"; }

// the copy of the map the game plays while you edit: named like the map so its zones and times are the map's
std::string livePlayPath(const std::string& mapPath) {
    std::string stem = mapPath.empty() ? "untitled" : std::filesystem::path(mapPath).stem().string();
    return prefDir() + "live/" + stem + ".vmf";
}

int inputStringResize(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

bool inputString(const char* label, std::string& s, ImGuiInputTextFlags flags = 0) {
    return ImGui::InputText(label, s.data(), s.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize,
                            inputStringResize, &s);
}

glm::vec4 pointEntityColor(const std::string& classname) {
    std::string c = collision::toLowerAscii(classname);
    if (c.rfind("info_player_", 0) == 0) return {0.3f, 1.0f, 0.4f, 0.45f};
    if (c == "info_teleport_destination") return {0.3f, 0.8f, 1.0f, 0.45f};
    if (c.rfind("light", 0) == 0) return {1.0f, 0.95f, 0.5f, 0.45f};
    return {1.0f, 0.3f, 0.9f, 0.45f};
}

std::string fileNameOf(const std::string& path) {
    return path.empty() ? "untitled" : std::filesystem::path(path).filename().string();
}

constexpr std::array<const char*, 20> kPointEntityPresets = {
    "info_target",     "info_teleport_destination", "light",          "light_spot",      "light_environment",
    "env_sprite",      "env_spritetrail",           "info_particle_system", "ambient_generic", "prop_static",
    "prop_dynamic",    "env_fog_controller",        "env_soundscape", "logic_auto",      "logic_relay",
    "math_counter",    "filter_activator_name",     "game_text",      "point_servercommand", "env_sun",
};

constexpr std::array<const char*, 10> kBrushEntityPresets = {
    "func_detail", "func_brush",      "func_illusionary", "func_wall",        "func_door",
    "func_button", "trigger_multiple", "trigger_teleport", "trigger_push", "func_breakable",
};

void drawHelpWindow(bool& open, bool fitToScreen) {
    if (!open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40.0f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 1.0f));
    bool controlsOpen = ImGui::Begin("controls", &open, ImGuiWindowFlags_AlwaysAutoResize);
    // sized by its own text, so only its position is ours to move
    ui::keepWindowOnScreen(fitToScreen, false);
    if (controlsOpen) {
        ImGui::TextUnformatted(
            "camera:   hold right mouse + WASD to fly, Q/E down/up, shift = fast,\n"
            "          scroll while flying = change speed\n"
            "click:    select (select tool) / place (any other tool)\n"
            "tools:    1 select  2 block  3 ramp  4 spawn  5 start zone  6 end zone\n"
            "          7 teleport  8 booster  9 push  0 entity  L light   esc = back to select\n"
            "drag:     drag a brush itself to slide it along the face you grabbed\n"
            "          shift+drag drags a copy and leaves the original (edit menu)\n"
            "gizmos:   ctrl+1/2/3 show only move / resize / rotate (edit menu turns it off)\n"
            "gizmo:    drag the coloured handles on a selected brush -- the three\n"
            "          out on the axes move it, the six on its faces resize it\n"
            "move:     arrows = x/y, page up/down = z (by grid)\n"
            "resize:   shift + arrows / page up/down\n"
            "grid:     [ and ] halve/double\n"
            "edit:     del delete, ctrl+d duplicate, ctrl+z undo, ctrl+y redo\n"
            "clip:     ctrl+c copy, ctrl+x cut, ctrl+v paste (in place, then drag it off)\n"
            "select:   ctrl+click adds to the selection (or takes one out again)\n"
            "groups:   ctrl+g group what is selected, ctrl+u ungroup it\n"
            "          a click picks up a whole group; ctrl+shift+g to reach one brush\n"
            "hide:     h hides what is selected, shift+h brings it all back\n"
            "purge:    ctrl+shift+c all clip brushes, ctrl+shift+t all teleports\n"
            "          (edit > remove all... for every other class)\n"
            "file:     ctrl+n new, ctrl+o open, ctrl+s save, ctrl+shift+s save as\n"
            "F5:       play it in movengine, live: edits show up as you make them\n"
            "F11:      fullscreen\n"
            "open:     .vmf maps, or a compiled .bsp (decompiled, saves as a .vmf)");
    }
    ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
    platform::Window window;
    if (!window.init("movengine editor", 1600, 900)) {
        return 1;
    }
    ui::ImGuiLayer imgui;
    if (!imgui.init(window)) {
        return 1;
    }
    // its own layout file so it and the game don't overwrite each other's. beside the executable isn't writable everywhere (Program Files) and the editor already keeps its other state in the pref folder
    static std::string editorIniPath = platform::prefFile("movengine_editor_imgui.ini");
    ImGui::GetIO().IniFilename = editorIniPath.c_str();

    GLuint program = render::createShaderProgram(render::kTexturedVertexSrc, render::kTexturedFragmentSrc);
    GLuint boxProgram = render::createShaderProgram(render::kDebugBoxVertexSrc, render::kDebugBoxFragmentSrc);
    if (!program || !boxProgram) {
        return 1;
    }
    GLint uViewProj = glGetUniformLocation(program, "uViewProj");
    GLint uTexture = glGetUniformLocation(program, "uTexture");
    render::LightingUniforms lightingUniforms;
    lightingUniforms.locate(program);
    // the editor never drew fog so the map's own atmosphere was invisible until you played it. these are the shader's fog inputs
    const GLint uFogEndLoc = glGetUniformLocation(program, "uFogEnd");
    const GLint uFogStartLoc = glGetUniformLocation(program, "uFogStart");
    const GLint uFogDensityLoc = glGetUniformLocation(program, "uFogMaxDensity");
    const GLint uFogColorLoc = glGetUniformLocation(program, "uFogColor");
    const GLint uViewPosLoc = glGetUniformLocation(program, "uViewPos");
    GLint uBoxViewProj = glGetUniformLocation(boxProgram, "uViewProj");
    GLint uBoxModel = glGetUniformLocation(boxProgram, "uModel");
    GLint uBoxColor = glGetUniformLocation(boxProgram, "uColor");
    render::DebugBox box;
    box.init();

    // two more shapes for the gizmo, drawn through the same shader as the box: a ball for the resize handles (reads as a circle from any angle, and is smaller than a cube of the same reach) and a ring for each rotation axis
    GLuint ballVao = 0, ballVbo = 0, ringVao = 0, ringVbo = 0;
    int ballVerts = 0, ringVerts = 0;
    {
        std::vector<glm::vec3> tris;
        const int rings = 8, segs = 12;  // low poly on purpose: it is 12px on screen
        auto at = [&](int i, int j) {
            float phi = glm::pi<float>() * float(i) / rings;
            float th = glm::two_pi<float>() * float(j) / segs;
            return glm::vec3(std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)) * 0.5f;
        };
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segs; ++j) {
                glm::vec3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
                tris.insert(tris.end(), {a, b, c, a, c, d});
            }
        }
        ballVerts = static_cast<int>(tris.size());
        glGenVertexArrays(1, &ballVao);
        glGenBuffers(1, &ballVbo);
        glBindVertexArray(ballVao);
        glBindBuffer(GL_ARRAY_BUFFER, ballVbo);
        glBufferData(GL_ARRAY_BUFFER, tris.size() * sizeof(glm::vec3), tris.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

        std::vector<glm::vec3> loop;
        const int ringSegs = 64;
        for (int i = 0; i < ringSegs; ++i) {
            float t = glm::two_pi<float>() * float(i) / ringSegs;
            loop.push_back(glm::vec3(std::cos(t), std::sin(t), 0.0f) * 0.5f);  // unit diameter, in XY
        }
        ringVerts = static_cast<int>(loop.size());
        glGenVertexArrays(1, &ringVao);
        glGenBuffers(1, &ringVbo);
        glBindVertexArray(ringVao);
        glBindBuffer(GL_ARRAY_BUFFER, ringVbo);
        glBufferData(GL_ARRAY_BUFFER, loop.size() * sizeof(glm::vec3), loop.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glBindVertexArray(0);
    }
    auto drawBall = [&](GLint modelLoc, const glm::vec3& c, float radius) {
        glm::mat4 m = glm::scale(glm::translate(glm::mat4(1.0f), c), glm::vec3(radius * 2.0f));
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(m));
        glBindVertexArray(ballVao);
        glDrawArrays(GL_TRIANGLES, 0, ballVerts);
        glBindVertexArray(0);
    };
    // the ring mesh lies in XY so X and Y rings are it turned onto their side
    auto drawRing = [&](GLint modelLoc, const glm::vec3& c, float radius, int axis) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), c);
        if (axis == 0) m = glm::rotate(m, glm::half_pi<float>(), glm::vec3(0, 1, 0));
        else if (axis == 1) m = glm::rotate(m, glm::half_pi<float>(), glm::vec3(1, 0, 0));
        m = glm::scale(m, glm::vec3(radius * 2.0f));
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(m));
        glBindVertexArray(ringVao);
        glDrawArrays(GL_LINE_LOOP, 0, ringVerts);
        glBindVertexArray(0);
    };

    content::GameContent gameContent;
    {
        const char* base = SDL_GetBasePath();
        content::mountConfiguredContent(gameContent, contentConfigPath(),
                                        std::filesystem::path(reinterpret_cast<const char8_t*>(base ? base : "")));
    }
    TextureCache textures(gameContent);
    ThumbnailCache thumbnails(gameContent);
    // what the browser offers: the built-ins always, plus every world material of css when it's mounted. without css the built-ins are all that can be used (anything else would show as MT)
    std::vector<std::string> browsable;
    auto refreshBrowsable = [&]() {
        browsable = content::builtinMaterialNames();
        if (gameContent.mounted()) {
            for (const auto& name : gameContent.worldMaterialNames()) browsable.push_back(upperAscii(name));
        }
    };
    refreshBrowsable();
    bool showTextures = true;
    bool showLighting = true;
    bool previewLighting = true;
    bool previewFog = true;
    std::string textureFilter;

    editor::Editor ed;
    if (argc > 1 && !ed.open(argv[1])) {
        std::fprintf(stderr, "couldn't open %s, starting a new map\n", argv[1]);
    }

    platform::DiscordPresence discord;
    discord.start(kDiscordAppId);
    discord.setLargeImage(kDiscordLargeImage, "movengine");

    render::FreeFlyCamera camera;
    camera.position = glm::vec3(-512.0f, -512.0f, 384.0f);
    camera.setYawDegrees(45.0f);
    camera.setPitchDegrees(-30.0f);
    camera.setMoveSpeed(800.0f);
    camera.setFovDegrees(90.0f);
    // remembered between runs: fly speed and look sensitivity are the two things everyone sets once and wants kept
    {
        float saved = collision::parseKeyValueFloat(
            content::loadSetting(contentConfigPath(), "camera_speed", "800"));
        if (saved >= 50.0f && saved <= 10000.0f) camera.setMoveSpeed(saved);
        float sens = collision::parseKeyValueFloat(
            content::loadSetting(contentConfigPath(), "camera_sensitivity", "2.5"));
        if (sens > 0.0f && sens <= 20.0f) camera.setMouseSensitivity(sens);
    }

    SceneMeshes scene;
    int builtRevision = -1;
    editor::Tool tool = editor::Tool::Select;
    bool looking = false;
    bool showHelp = true;
    bool showRemoveAll = false;
    bool showHidden = false;
    ImVec2 lastDisplaySize(0.0f, 0.0f);
    int fitPanelFrames = 0;
    std::string tieClass = "func_detail";
    std::string statusText;
    float statusTimer = 0.0f;
    auto status = [&](const std::string& text) {
        statusText = text;
        statusTimer = 3.0f;
        std::printf("%s\n", text.c_str());
    };

    // --- compiling to .bsp --------------------------------------------------
    // the compile runs on a worker thread: vvis and vrad take minutes on a real map and the log is the only sign it's making progress
    bool showCompile = false;
    std::string compileSdkBin = content::loadCompileToolsPath(contentConfigPath());
    std::string compileOutput;
    vmf::CompileOptions compileOptions;
    vmf::CompileTools compileTools;
    bool compileToolsStale = true;
    std::vector<std::string> compileLog;
    std::mutex compileLogMutex;
    std::atomic<bool> compileRunning{false};
    std::atomic<bool> compileCancel{false};
    std::atomic<bool> compileSucceeded{false};
    std::string compileFinishedPath;
    std::thread compileThread;
    bool compileScrollToEnd = false;

    auto compileAppend = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(compileLogMutex);
        compileLog.push_back(line);
    };

    // every material the map uses, resolved to its real texture size on this thread before the worker starts: texinfo needs the size for its texture axes, and resolving goes through the gpu backed cache which is the ui thread's alone
    auto snapshotMaterialSizes = [&]() {
        auto sizes = std::make_shared<std::map<std::string, glm::ivec2>>();
        auto note = [&](const vmf::Solid& s) {
            for (const auto& side : s.sides) {
                std::string key = upperAscii(side.material);
                if (sizes->count(key)) continue;
                glm::vec2 uv = textures.get(side.material).uvSize;
                (*sizes)[key] = glm::ivec2(static_cast<int>(uv.x), static_cast<int>(uv.y));
            }
        };
        for (const auto& s : ed.doc.world.solids) note(s);
        for (const auto& e : ed.doc.entities) {
            for (const auto& s : e.solids) note(s);
        }
        return sizes;
    };

    auto startCompile = [&]() {
        if (compileRunning) return;
        if (compileOutput.empty()) {
            status("pick where the .bsp should go first");
            return;
        }
        if (compileThread.joinable()) compileThread.join();
        {
            std::lock_guard<std::mutex> lock(compileLogMutex);
            compileLog.clear();
        }
        compileCancel = false;
        compileSucceeded = false;
        compileFinishedPath.clear();
        compileRunning = true;

        // vbsp leaves its .bsp beside the .vmf it was handed so compile from a working copy with its own name: dropping a "<map>.vmf" next to the output could otherwise overwrite the real one
        std::filesystem::path out(compileOutput);
        std::filesystem::path workVmf = out;
        workVmf.replace_extension("");
        workVmf += ".compile.vmf";

        compileThread = std::thread([&, doc = ed.doc, tools = compileTools, options = compileOptions,
                                     outPath = compileOutput, workPath = workVmf.string(),
                                     sizes = snapshotMaterialSizes()]() {
            auto lookup = [sizes](const std::string& material) -> glm::ivec2 {
                auto it = sizes->find(upperAscii(material));
                return it == sizes->end() ? glm::ivec2(0) : it->second;
            };
            vmf::CompileResult result =
                vmf::compileMap(doc, workPath, outPath, options, tools, compileAppend, lookup,
                                [&]() { return compileCancel.load(); });
            if (result.ok) {
                compileFinishedPath = result.bspPath;
                compileSucceeded = true;
                // the working .vmf (and the portal file vvis leaves beside it) are ours, not the user's map: clear them up once it worked
                std::error_code ec;
                std::filesystem::remove(workPath, ec);
                std::filesystem::path prt(workPath);
                std::filesystem::remove(prt.replace_extension(".prt"), ec);
            } else if (!result.error.empty()) {
                compileAppend("");
                compileAppend("compile failed: " + result.error);
            }
            compileRunning = false;
        });
    };

    // --- file actions -------------------------------------------------------
    auto saveAs = [&]() -> bool {
        auto file = platform::chooseVmfToSave(ed.path.empty() ? "untitled.vmf" : ed.path);
        if (!file) return false;
        if (!ed.save(*file)) {
            status("couldn't save " + *file);
            return false;
        }
        status("saved " + fileNameOf(*file));
        return true;
    };
    auto save = [&]() -> bool {
        if (ed.path.empty()) return saveAs();
        if (!ed.save(ed.path)) {
            status("couldn't save " + ed.path);
            return false;
        }
        status("saved " + fileNameOf(ed.path));
        return true;
    };
    // true when it's fine to throw the current map away
    auto confirmDiscard = [&]() -> bool {
        if (!ed.dirty) return true;
        int answer = platform::askYesNoCancel("unsaved changes", "save changes to this map first?");
        if (answer == 1) return save();
        return answer == 2;
    };
    // play live: the game runs a copy of the map that every edit is written to (a moment after you stop) and reloads it in place. your own file is only written when you save, and a new map needs no saving at all
    platform::ChildProcess game;
    std::string livePath;
    int liveRevision = -1;
    float liveWait = 0.0f;
    auto writeLive = [&]() -> bool {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(livePath).parent_path(), ec);
        // written aside and renamed over so the game never reads half a file
        std::string temp = livePath + ".tmp";
        if (!vmf::saveDocument(temp, ed.doc)) return false;
        std::filesystem::rename(temp, livePath, ec);
        if (ec) return false;
        liveRevision = ed.revision;
        return true;
    };
    auto play = [&]() {
        std::string wanted = livePlayPath(ed.path);
        bool same = game.running() && wanted == livePath;
        livePath = wanted;
        if (!writeLive()) {
            status("couldn't write " + livePath);
            return;
        }
        if (same) {
            status("already playing -- your edits show up in the game as you make them");
        } else if (game.start("movengine", {livePath})) {
            status("playing live -- edits show up in the game as you make them (ctrl+s still saves your map)");
        } else {
            status("couldn't start movengine (is it next to movengine_editor?)");
        }
    };
    auto openMap = [&]() {
        if (!confirmDiscard()) return;
        if (auto file = platform::chooseVmfToOpen()) {
            if (ed.open(*file)) status("opened " + fileNameOf(*file));
            else status("couldn't open " + *file);
        }
    };
    auto newMap = [&]() {
        if (confirmDiscard()) ed.newMap();
    };

    // --- per-frame helpers ----------------------------------------------------
    auto mouseRay = [&](glm::vec3& origin, glm::vec3& dir) {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        float aspect = static_cast<float>(window.width()) / static_cast<float>(std::max(window.height(), 1));
        glm::mat4 inv = glm::inverse(camera.projectionMatrix(aspect) * camera.viewMatrix());
        float x = 2.0f * mx / window.width() - 1.0f;
        float y = 1.0f - 2.0f * my / window.height();
        glm::vec4 nearP = inv * glm::vec4(x, y, -1.0f, 1.0f);
        glm::vec4 farP = inv * glm::vec4(x, y, 1.0f, 1.0f);
        origin = glm::vec3(nearP) / nearP.w;
        dir = glm::normalize(glm::vec3(farP) / farP.w - origin);
    };

    // ---- Unity-style transform gizmo -------------------------------------
    //
    // hammer makes you size a brush in a 2d view, the point of this editor is that you do it in the 3d one. a selected brush gets three axis arrows to drag it along and a handle on each of its six faces to drag that face in or out. everything is picked as a world space box and not in screen space so a handle behind geometry still picks the way it looks
    enum class Handle {
        None,
        MoveX, MoveY, MoveZ,
        FaceNegX, FacePosX, FaceNegY, FacePosY, FaceNegZ, FacePosZ,
        RotX, RotY, RotZ,
    };
    constexpr glm::vec3 kAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    struct Gizmo {
        Handle active = Handle::None;
        Handle hovered = Handle::None;
        glm::vec3 startMin{0.0f}, startMax{0.0f};
        float startAxisT = 0.0f;
        float startAngle = 0.0f;   // rotation drags
        float appliedAngle = 0.0f; // how much has been turned so far, so each step is a difference
    } gizmo;

    // one job at a time, optionally. by default every handle is out at once, which is quick but crowded on a small brush. turned on, the gizmo shows only the move arrows, only the resize balls or only the rotate rings, and ctrl+1/2/3 picks which, like most 3d editors
    enum class GizmoMode { Move, Resize, Rotate };
    bool gizmoModes = content::loadSetting(contentConfigPath(), "gizmo_modes", "0") == "1";
    GizmoMode gizmoMode = GizmoMode::Move;
    {
        std::string saved = content::loadSetting(contentConfigPath(), "gizmo_mode", "move");
        if (saved == "resize") gizmoMode = GizmoMode::Resize;
        else if (saved == "rotate") gizmoMode = GizmoMode::Rotate;
    }
    auto gizmoModeName = [&]() {
        switch (gizmoMode) {
            case GizmoMode::Move: return "move";
            case GizmoMode::Resize: return "resize";
            case GizmoMode::Rotate: return "rotate";
        }
        return "?";
    };
    // hammer's shift-drag. on by default: it's what anyone coming from hammer will try first, and without it shift+click is just a click
    bool shiftDragDuplicates = content::loadSetting(contentConfigPath(), "shift_drag_duplicates", "1") == "1";
    // hammer's "ignore groups", remembered between runs. off means a click picks up the whole group, on means it reaches the one brush inside
    ed.ignoreGroups = content::loadSetting(contentConfigPath(), "ignore_groups", "0") == "1";
    auto saveIgnoreGroups = [&]() {
        content::saveSetting(contentConfigPath(), "ignore_groups", ed.ignoreGroups ? "1" : "0");
    };

    auto saveGizmoModes = [&]() {
        content::saveSetting(contentConfigPath(), "gizmo_modes", gizmoModes ? "1" : "0");
        content::saveSetting(contentConfigPath(), "gizmo_mode", gizmoModeName());
    };

    // dragging the brush itself and not a handle. the handles move one axis at a time which is what you want for precision and a nuisance for "put it over there": grab the brush anywhere and it slides along the face you grabbed, so the top of a block shoves it across the floor and a wall slides it up and along
    struct BodyDrag {
        bool armed = false;   // pressed on it, not yet moved far enough to count
        bool active = false;  // past the slop, actually moving it
        glm::vec3 planePoint{0.0f};
        glm::vec3 planeNormal{0.0f, 0.0f, 1.0f};
        glm::vec3 applied{0.0f};  // moved so far, so each step is a difference
        float pressX = 0.0f, pressY = 0.0f;
    } bodyDrag;

    // handles grow with distance so they stay grabbable across the map instead of becoming a pixel at the far end of it
    auto handleSize = [&](const glm::vec3& at) {
        return std::clamp(glm::length(at - camera.position) * 0.03f, 2.0f, 256.0f);
    };
    // resize handles are balls, which read as round from every angle so stay obvious at a smaller size than a cube of the same reach
    auto faceHandleSize = [&](const glm::vec3& at) { return handleSize(at) * 0.6f; };
    auto isRotate = [](Handle h) { return h == Handle::RotX || h == Handle::RotY || h == Handle::RotZ; };
    auto isMove = [](Handle h) { return h == Handle::MoveX || h == Handle::MoveY || h == Handle::MoveZ; };
    // whether a handle is one of the ones the current mode offers. with modes off, every handle is
    auto modeShows = [&](Handle h) {
        if (!gizmoModes) return true;
        if (isRotate(h)) return gizmoMode == GizmoMode::Rotate;
        if (isMove(h)) return gizmoMode == GizmoMode::Move;
        return gizmoMode == GizmoMode::Resize;
    };

    // rings sit just outside the brush so they don't fight the face handles
    auto ringRadius = [&](const glm::vec3& mn, const glm::vec3& mx) {
        glm::vec3 half = (mx - mn) * 0.5f;
        return std::max({half.x, half.y, half.z}) + handleSize((mn + mx) * 0.5f) * 2.5f;
    };
    // where a ray crosses a ring's plane, and how far that is from the centre. a ring is picked when that distance is near its radius
    auto ringHit = [&](const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& center, int axis,
                        glm::vec3& point) {
        const glm::vec3 n = kAxis[axis];
        float denom = glm::dot(dir, n);
        if (std::abs(denom) < 1e-6f) return false;
        float t = glm::dot(center - origin, n) / denom;
        if (t <= 0.0f) return false;
        point = origin + dir * t;
        return true;
    };
    // angle of a point around a ring, measured in that ring's own plane
    auto ringAngle = [&](const glm::vec3& point, const glm::vec3& center, int axis) {
        const glm::vec3 e1 = kAxis[(axis + 1) % 3], e2 = kAxis[(axis + 2) % 3];
        glm::vec3 v = point - center;
        return glm::degrees(std::atan2(glm::dot(v, e2), glm::dot(v, e1)));
    };

    // where each handle sits for the current selection
    auto handleCenter = [&](Handle h, const glm::vec3& mn, const glm::vec3& mx) {
        glm::vec3 c = (mn + mx) * 0.5f;
        glm::vec3 half = (mx - mn) * 0.5f;
        float s = handleSize(c);
        switch (h) {
            case Handle::MoveX: return c + kAxis[0] * (half.x + s * 4.0f);
            case Handle::MoveY: return c + kAxis[1] * (half.y + s * 4.0f);
            case Handle::MoveZ: return c + kAxis[2] * (half.z + s * 4.0f);
            case Handle::FaceNegX: return glm::vec3(mn.x, c.y, c.z);
            case Handle::FacePosX: return glm::vec3(mx.x, c.y, c.z);
            case Handle::FaceNegY: return glm::vec3(c.x, mn.y, c.z);
            case Handle::FacePosY: return glm::vec3(c.x, mx.y, c.z);
            case Handle::FaceNegZ: return glm::vec3(c.x, c.y, mn.z);
            case Handle::FacePosZ: return glm::vec3(c.x, c.y, mx.z);
            default: return c;
        }
    };

    // which world axis a handle slides along, and whether it drives the min or the max of the box (move handles drive both)
    auto handleAxis = [&](Handle h) -> int {
        switch (h) {
            case Handle::MoveX: case Handle::FaceNegX: case Handle::FacePosX: case Handle::RotX: return 0;
            case Handle::MoveY: case Handle::FaceNegY: case Handle::FacePosY: case Handle::RotY: return 1;
            default: return 2;
        }
    };

    // closest point on the handle's axis to the mouse ray, as a distance along that axis. dragging is the difference between this now and when the drag started so where exactly the ray crosses doesn't matter
    auto axisParam = [&](const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& axisPoint, int axis) {
        const glm::vec3 u = kAxis[axis];
        glm::vec3 w0 = axisPoint - origin;
        float b = glm::dot(u, dir);
        float denom = 1.0f - b * b;
        if (std::abs(denom) < 1e-5f) return glm::dot(w0, u);  // ray along the axis: no usable answer
        float d = glm::dot(u, w0), e = glm::dot(dir, w0);
        return glm::dot(axisPoint, u) + (b * e - d) / denom;
    };

    auto pickHandle = [&](const glm::vec3& origin, const glm::vec3& dir) {
        glm::vec3 mn, mx;
        if (tool != editor::Tool::Select || !ed.selectionBounds(mn, mx)) return Handle::None;
        Handle best = Handle::None;
        float bestDist = std::numeric_limits<float>::max();
        // rings first: they sit outside everything else so a hit on one is unambiguous and should win over a face handle behind it
        {
            const glm::vec3 c = (mn + mx) * 0.5f;
            const float r = ringRadius(mn, mx);
            const float tol = handleSize(c) * 1.2f;
            for (int a = 0; a < 3; ++a) {
                if (!modeShows(a == 0 ? Handle::RotX : (a == 1 ? Handle::RotY : Handle::RotZ))) continue;
                glm::vec3 p;
                if (!ringHit(origin, dir, c, a, p)) continue;
                if (std::abs(glm::length(p - c) - r) > tol) continue;
                float d = glm::length(p - origin);
                if (d < bestDist) {
                    bestDist = d;
                    best = a == 0 ? Handle::RotX : (a == 1 ? Handle::RotY : Handle::RotZ);
                }
            }
        }
        for (Handle h : {Handle::MoveX, Handle::MoveY, Handle::MoveZ, Handle::FaceNegX, Handle::FacePosX,
                          Handle::FaceNegY, Handle::FacePosY, Handle::FaceNegZ, Handle::FacePosZ}) {
            if (!modeShows(h)) continue;
            glm::vec3 c = handleCenter(h, mn, mx);
            float s = isMove(h) ? handleSize(c) : faceHandleSize(c);
            // slab test against the handle's own little box
            glm::vec3 bmin = c - glm::vec3(s), bmax = c + glm::vec3(s);
            float t0 = 0.0f, t1 = std::numeric_limits<float>::max();
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::abs(dir[a]) < 1e-8f) {
                    if (origin[a] < bmin[a] || origin[a] > bmax[a]) miss = true;
                    continue;
                }
                float inv = 1.0f / dir[a];
                float ta = (bmin[a] - origin[a]) * inv, tb = (bmax[a] - origin[a]) * inv;
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
                if (t0 > t1) miss = true;
            }
            if (!miss && t0 < bestDist) {
                bestDist = t0;
                best = h;
            }
        }
        return best;
    };

    auto handleClick = [&]() {
        glm::vec3 origin, dir;
        mouseRay(origin, dir);
        if (tool == editor::Tool::Select) {
            // a handle under the cursor is a drag, not a new selection, otherwise grabbing one would deselect and the drag would never start
            Handle grabbed = pickHandle(origin, dir);
            if (grabbed != Handle::None) {
                glm::vec3 mn, mx;
                ed.selectionBounds(mn, mx);
                ed.checkpoint();  // once for the whole drag, so one undo takes it back
                gizmo.active = grabbed;
                gizmo.startMin = mn;
                gizmo.startMax = mx;
                gizmo.appliedAngle = 0.0f;
                if (isRotate(grabbed)) {
                    glm::vec3 c = (mn + mx) * 0.5f, p;
                    if (ringHit(origin, dir, c, handleAxis(grabbed), p)) {
                        gizmo.startAngle = ringAngle(p, c, handleAxis(grabbed));
                    }
                } else {
                    gizmo.startAxisT = axisParam(origin, dir, handleCenter(grabbed, mn, mx), handleAxis(grabbed));
                }
                return;
            }
            editor::RayHit hit = ed.raycast(origin, dir);
            const bool addToSelection = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            if (!hit.hit) {
                // clicking nothing clears, unless ctrl is held. losing a careful selection to one stray click is miserable
                if (!addToSelection) ed.clearSelection();
            } else if (!addToSelection && ed.selectionCount() > 1 && ed.isSelected(hit.target)) {
                // clicking inside a multiple selection keeps it so a drag moves all of it and doesn't collapse to the one brush under the cursor
            } else {
                // groups: unless they're being ignored this picks up every member, not just the one that was hit
                ed.selectClicked(hit.target, addToSelection);
            }
            if (hit.hit && !addToSelection) {
                // shift makes the drag carry a copy and leave the original where it is, like hammer. the copy lands exactly on top so letting go without moving leaves them stacked, which is also what hammer does
                if (shiftDragDuplicates && (SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
                    if (ed.duplicateSelectionInPlace()) status("dragging a copy -- the original stays put");
                }
                // armed, not started: a click that never moves has to stay a plain select with nothing shifted and no undo step pushed
                bodyDrag.armed = true;
                bodyDrag.active = false;
                bodyDrag.planePoint = hit.point;
                bodyDrag.planeNormal = hit.normal;
                bodyDrag.applied = glm::vec3(0.0f);
                SDL_GetMouseState(&bodyDrag.pressX, &bodyDrag.pressY);
            }
            return;
        }
        editor::RayHit hit = ed.raycast(origin, dir);
        glm::vec3 point, normal(0.0f, 0.0f, 1.0f);
        if (hit.hit) {
            point = hit.point;
            normal = hit.normal;
        } else if (std::abs(dir.z) > 1e-4f && (0.0f - origin.z) / dir.z > 0.0f) {
            point = origin + dir * ((0.0f - origin.z) / dir.z);  // the z=0 plane
        } else {
            point = origin + dir * 512.0f;
        }
        ed.place(tool, point, normal, camera.yawDegrees());
        status(std::string("placed ") + editor::toolName(tool));
    };

    auto handleKey = [&](const SDL_KeyboardEvent& key) {
        bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
        bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
        if (ctrl) {
            switch (key.key) {
                case SDLK_S: shift ? (void)saveAs() : (void)save(); break;
                case SDLK_O: openMap(); break;
                case SDLK_N: newMap(); break;
                case SDLK_Z: shift ? (void)ed.redo() : (void)ed.undo(); break;
                case SDLK_Y: ed.redo(); break;
                case SDLK_D: ed.duplicateSelection(); break;
                case SDLK_C:
                    if (shift) {
                        int n = ed.removeClipBrushes();
                        status(n ? "removed " + std::to_string(n) + " clip brushes"
                                 : "no clip brushes in this map");
                    } else {
                        status(ed.copySelection() ? "copied" : "nothing selected to copy");
                    }
                    break;
                case SDLK_T:
                    if (shift) {
                        int n = ed.removeEntitiesOfClass("trigger_teleport");
                        status(n ? "removed " + std::to_string(n) + " teleport triggers"
                                 : "no teleport triggers in this map");
                    }
                    break;
                case SDLK_X:
                    status(ed.cutSelection() ? "cut" : "nothing selected to cut");
                    break;
                case SDLK_G:
                    if (shift) {
                        ed.ignoreGroups = !ed.ignoreGroups;
                        saveIgnoreGroups();
                        status(ed.ignoreGroups ? "ignoring groups -- a click picks one object"
                                               : "honouring groups -- a click picks the whole group");
                    } else {
                        int n = ed.selectionCount();
                        status(ed.groupSelection()
                                   ? "grouped " + std::to_string(n) + " -- they now click and move as one"
                                   : "select two or more things to group them (ctrl+click adds)");
                    }
                    break;
                case SDLK_U:
                    status(ed.ungroupSelection() ? "ungrouped" : "nothing selected is in a group");
                    break;
                case SDLK_V:
                    // lands on top of what it came from so say so, otherwise a paste you can't see looks like one that didn't happen
                    status(ed.pasteClipboard() ? "pasted in place -- drag it off the original"
                                               : "nothing on the clipboard");
                    break;
                // picking a mode turns the modes on so the shortcut works without hunting through the menu for the switch first
                case SDLK_1: case SDLK_2: case SDLK_3: {
                    gizmoMode = key.key == SDLK_1   ? GizmoMode::Move
                                : key.key == SDLK_2 ? GizmoMode::Resize
                                                    : GizmoMode::Rotate;
                    gizmoModes = true;
                    saveGizmoModes();
                    status(std::string("gizmo: ") + gizmoModeName() + " only (edit menu turns this off)");
                    break;
                }
                default: break;
            }
            return;
        }
        if (key.key == SDLK_F5) {
            play();
            return;
        }
        if (key.key == SDLK_F11) {
            // the game has had this since forever, the editor never did so the key just did nothing here
            window.setFullscreen(!window.fullscreen());
            return;
        }
        if (key.key == SDLK_ESCAPE) {
            if (tool != editor::Tool::Select) tool = editor::Tool::Select;
            else ed.selection = {};
            return;
        }
        if (key.key == SDLK_H) {
            if (shift) {
                int n = ed.hiddenCount();
                ed.unhideAll();
                status(n ? "unhid " + std::to_string(n) : "nothing is hidden");
            } else {
                int n = ed.selectionCount();
                ed.hideSelection();
                status(n ? "hid " + std::to_string(n) + " (edit > hidden... to bring them back)"
                         : "nothing selected to hide");
            }
            return;
        }
        if (key.key == SDLK_DELETE || key.key == SDLK_BACKSPACE) {
            ed.deleteSelection();
            return;
        }
        if (key.key == SDLK_T && !looking) showTextures = !showTextures;
        if (key.key == SDLK_L && !looking) tool = editor::Tool::Light;
        if (key.key == SDLK_LEFTBRACKET) ed.grid = std::max(1.0f, ed.grid * 0.5f);
        if (key.key == SDLK_RIGHTBRACKET) ed.grid = std::min(512.0f, ed.grid * 2.0f);

        static constexpr std::array<editor::Tool, 10> kToolKeys = {
            editor::Tool::PointEntity, editor::Tool::Select,   editor::Tool::Block,    editor::Tool::Ramp,
            editor::Tool::Spawn,       editor::Tool::StartZone, editor::Tool::EndZone, editor::Tool::Teleport,
            editor::Tool::Booster,     editor::Tool::Push,
        };
        if (key.key >= SDLK_0 && key.key <= SDLK_9 && !looking) {
            tool = kToolKeys[key.key - SDLK_0];
            return;
        }

        // arrow keys nudge (or with shift, resize) the selection by the grid
        glm::vec3 axis(0.0f);
        if (key.key == SDLK_RIGHT) axis = {1, 0, 0};
        if (key.key == SDLK_LEFT) axis = {-1, 0, 0};
        if (key.key == SDLK_UP) axis = {0, 1, 0};
        if (key.key == SDLK_DOWN) axis = {0, -1, 0};
        if (key.key == SDLK_PAGEUP) axis = {0, 0, 1};
        if (key.key == SDLK_PAGEDOWN) axis = {0, 0, -1};
        if (axis == glm::vec3(0.0f)) return;
        if (shift) {
            glm::vec3 mn, mx;
            if (ed.selectionBounds(mn, mx)) ed.resizeSelection(mn, mx + axis * ed.grid);
        } else {
            ed.moveSelection(axis * ed.grid);
        }
    };

    glEnable(GL_DEPTH_TEST);
    Uint64 lastTicks = SDL_GetTicks();
    bool quitRequested = false;
    while (true) {
        bool windowClosed = false;
        float wheel = 0.0f;
        window.pollEvents(windowClosed, [&](const SDL_Event& event) {
            imgui.processEvent(event);
            ImGuiIO& io = ImGui::GetIO();
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
                !io.WantCaptureMouse) {
                looking = true;
                window.setRelativeMouseMode(true);
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT && looking) {
                looking = false;
                window.setRelativeMouseMode(false);
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && !looking &&
                !io.WantCaptureMouse) {
                handleClick();
            }
            if (event.type == SDL_EVENT_MOUSE_WHEEL && looking) wheel += event.wheel.y;
            if (event.type == SDL_EVENT_KEY_DOWN && (!io.WantCaptureKeyboard || looking)) handleKey(event.key);
        });
        // dragging a handle, or just lighting up the one under the cursor
        {
            ImGuiIO& gizmoIo = ImGui::GetIO();
            glm::vec3 origin, dir;
            mouseRay(origin, dir);
            if (gizmo.active != Handle::None) {
                if ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) == 0) {
                    gizmo.active = Handle::None;  // released
                } else {
                    const int axis = handleAxis(gizmo.active);
                    if (isRotate(gizmo.active)) {
                        const glm::vec3 c = (gizmo.startMin + gizmo.startMax) * 0.5f;
                        glm::vec3 p;
                        if (ringHit(origin, dir, c, axis, p)) {
                            float now = ringAngle(p, c, axis);
                            // shortest way round so dragging past 180 doesn't spin the brush the other way
                            float total = std::fmod(now - gizmo.startAngle + 540.0f, 360.0f) - 180.0f;
                            total = std::round(total / 15.0f) * 15.0f;  // 15-degree steps, like Hammer
                            float step = total - gizmo.appliedAngle;
                            if (std::abs(step) > 0.01f) {
                                ed.rotateSelection(kAxis[axis], step, false);
                                gizmo.appliedAngle = total;
                            }
                        }
                        // rotation handled; skip the translate/resize path
                        goto gizmoDone;
                    }
                    const glm::vec3 startCenter = handleCenter(gizmo.active, gizmo.startMin, gizmo.startMax);
                    float now = axisParam(origin, dir, startCenter, axis);
                    // snapped as a distance, so a brush on the grid stays on it and one off the grid keeps its offset
                    float delta = ed.snap(now - gizmo.startAxisT);
                    glm::vec3 mn = gizmo.startMin, mx = gizmo.startMax;
                    if (isMove(gizmo.active)) {
                        glm::vec3 shift(0.0f);
                        shift[axis] = delta;
                        ed.resizeSelection(mn + shift, mx + shift, false);
                    } else {
                        const bool negSide = gizmo.active == Handle::FaceNegX ||
                                             gizmo.active == Handle::FaceNegY || gizmo.active == Handle::FaceNegZ;
                        if (negSide) {
                            mn[axis] += delta;
                            // never let a face cross the other one: a brush turned inside out isn't a brush
                            mn[axis] = std::min(mn[axis], mx[axis] - ed.grid);
                        } else {
                            mx[axis] += delta;
                            mx[axis] = std::max(mx[axis], mn[axis] + ed.grid);
                        }
                        ed.resizeSelection(mn, mx, false);
                    }
                }
                gizmoDone:;
            } else if (bodyDrag.armed || bodyDrag.active) {
                if ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) == 0) {
                    bodyDrag.armed = false;
                    bodyDrag.active = false;
                } else {
                    float mouseX = 0.0f, mouseY = 0.0f;
                    SDL_GetMouseState(&mouseX, &mouseY);
                    // a few pixels of slop first so selecting something with a less than steady hand doesn't nudge it a grid square
                    constexpr float kDragSlop = 4.0f;
                    if (!bodyDrag.active && (std::abs(mouseX - bodyDrag.pressX) +
                                             std::abs(mouseY - bodyDrag.pressY)) > kDragSlop) {
                        bodyDrag.active = true;
                        ed.checkpoint();  // once for the whole drag, like the handles
                    }
                    if (bodyDrag.active) {
                        // where the mouse ray now crosses the plane of the face that was grabbed. seen nearly edge on that plane is almost parallel to the ray and the crossing runs off to infinity, so leave the brush where it is instead of flinging it across the map
                        const float denom = glm::dot(dir, bodyDrag.planeNormal);
                        if (std::abs(denom) > 0.02f) {
                            const float t =
                                glm::dot(bodyDrag.planePoint - origin, bodyDrag.planeNormal) / denom;
                            if (t > 0.0f) {
                                // snapped as a distance from where the drag started, so a brush on the grid stays on it and one off the grid keeps its offset
                                glm::vec3 total = ed.snap((origin + dir * t) - bodyDrag.planePoint);
                                glm::vec3 step = total - bodyDrag.applied;
                                if (step != glm::vec3(0.0f)) {
                                    ed.moveSelection(step, false);
                                    bodyDrag.applied = total;
                                }
                            }
                        }
                    }
                }
                gizmo.hovered = Handle::None;
            } else if (!gizmoIo.WantCaptureMouse && !looking) {
                gizmo.hovered = pickHandle(origin, dir);
            } else {
                gizmo.hovered = Handle::None;
            }
        }

        if (windowClosed && confirmDiscard()) quitRequested = true;
        if (quitRequested) break;

        Uint64 now = SDL_GetTicks();
        float dt = std::min(static_cast<float>(now - lastTicks) / 1000.0f, 0.1f);
        if (!livePath.empty() && ed.revision != liveRevision) {
            liveWait += dt;
            if (liveWait > 0.3f && game.running()) writeLive();
        } else {
            liveWait = 0.0f;
        }
        lastTicks = now;

        float dx = 0.0f, dy = 0.0f;
        window.consumeMouseDelta(dx, dy);
        if (looking) {
            camera.look(dx, dy);
            if (wheel != 0.0f) {
                camera.setMoveSpeed(std::clamp(camera.moveSpeed() * std::pow(1.25f, wheel), 50.0f, 10000.0f));
                content::saveSetting(contentConfigPath(), "camera_speed", vmf::formatNumber(camera.moveSpeed()));
                status("fly speed " + std::to_string(static_cast<int>(camera.moveSpeed())));
            }
            const bool* keys = SDL_GetKeyboardState(nullptr);
            render::FreeFlyCamera::MoveInput move;
            move.forward = keys[SDL_SCANCODE_W];
            move.back = keys[SDL_SCANCODE_S];
            move.left = keys[SDL_SCANCODE_A];
            move.right = keys[SDL_SCANCODE_D];
            move.up = keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_SPACE];
            move.down = keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_LCTRL];
            float baseSpeed = camera.moveSpeed();
            if (keys[SDL_SCANCODE_LSHIFT]) camera.setMoveSpeed(baseSpeed * 3.0f);
            camera.update(move, dt);
            camera.setMoveSpeed(baseSpeed);
        }

        if (builtRevision != ed.revision) {
            textures.setPakSource(ed.doc.world.get(vmf::kPakfileKey));
            scene.rebuild(ed.doc, textures);
            builtRevision = ed.revision;
        }
        std::string title = "movengine editor - " + fileNameOf(ed.path) + (ed.dirty ? " *" : "");
        SDL_SetWindowTitle(window.handle(), title.c_str());
        discord.setActivity(kDiscordDetails, "editing " + (ed.path.empty() ? std::string("a new map")
                                                                           : fileNameOf(ed.path)));
        discord.update();

        // --- UI -------------------------------------------------------------
        imgui.newFrame();
        // imgui.ini remembers where the panels were, and that layout outlives the window it was arranged in: reopening a 1600x900 arrangement on a smaller screen leaves half of them off the edge. pull them back whenever the viewport size changes, startup included, and leave them alone the rest of the time so they can still be dragged anywhere
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        if (displaySize.x != lastDisplaySize.x || displaySize.y != lastDisplaySize.y) {
            // a few frames, not one: an auto resizing panel ("controls") has no measured size on the frame it first appears so clamping it then works from a size it doesn't have yet and leaves it overhanging
            fitPanelFrames = 3;
        }
        lastDisplaySize = displaySize;
        const bool fitPanels = fitPanelFrames > 0;
        if (fitPanelFrames > 0) --fitPanelFrames;
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("file")) {
                if (ImGui::MenuItem("new", "ctrl+n")) newMap();
                if (ImGui::MenuItem("open...", "ctrl+o")) openMap();
                if (ImGui::MenuItem("save", "ctrl+s")) save();
                if (ImGui::MenuItem("save as...", "ctrl+shift+s")) saveAs();
                ImGui::Separator();
                if (ImGui::MenuItem("play", "F5")) play();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("opens the game on this map; every edit shows up in it as you go, no saving needed");
                }
                if (ImGui::MenuItem("compile to .bsp...")) {
                    showCompile = true;
                    compileToolsStale = true;
                    if (compileOutput.empty()) {
                        // default to the map's own name so the usual answer is just pressing compile
                        std::filesystem::path guess(ed.path.empty() ? "untitled.vmf" : ed.path);
                        guess.replace_extension(".bsp");
                        compileOutput = guess.string();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("makes a .bsp you can put on a server, with vbsp/vvis/vrad if they are "
                                      "installed and the built-in writer if not");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("exit") && confirmDiscard()) quitRequested = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("edit")) {
                if (ImGui::MenuItem("undo", "ctrl+z")) ed.undo();
                if (ImGui::MenuItem("redo", "ctrl+y")) ed.redo();
                ImGui::Separator();
                ImGui::Separator();
                if (ImGui::MenuItem("cut", "ctrl+x")) {
                    status(ed.cutSelection() ? "cut" : "nothing selected to cut");
                }
                if (ImGui::MenuItem("copy", "ctrl+c")) {
                    status(ed.copySelection() ? "copied" : "nothing selected to copy");
                }
                if (ImGui::MenuItem("paste", "ctrl+v", false, ed.hasClipboard())) {
                    status(ed.pasteClipboard() ? "pasted in place -- drag it off the original"
                                               : "nothing on the clipboard");
                }
                if (ImGui::IsItemHovered() && ed.hasClipboard()) {
                    ImGui::SetTooltip("pastes where it was copied from, selected, ready to drag away");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("duplicate", "ctrl+d")) ed.duplicateSelection();
                if (ImGui::MenuItem("delete", "del")) ed.deleteSelection();
                ImGui::Separator();
                if (ImGui::MenuItem("group", "ctrl+g", false, ed.selectionCount() > 1)) {
                    int n = ed.selectionCount();
                    status(ed.groupSelection() ? "grouped " + std::to_string(n) : "nothing to group");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("ctrl+click two or more things, then group them: from here on a "
                                      "click picks up the lot");
                }
                if (ImGui::MenuItem("ungroup", "ctrl+u", false, ed.selectionCount() > 0)) {
                    status(ed.ungroupSelection() ? "ungrouped" : "nothing selected is in a group");
                }
                if (ImGui::MenuItem("ignore groups", "ctrl+shift+g", &ed.ignoreGroups)) saveIgnoreGroups();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("click straight through to the one brush under the cursor instead of "
                                      "its whole group");
                }
                if (int groups = ed.groupCount(); groups > 0) {
                    ImGui::TextDisabled("   %d group%s in this map", groups, groups == 1 ? "" : "s");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("hide selected", "h")) {
                    int n = ed.selectionCount();
                    ed.hideSelection();
                    status(n ? "hid " + std::to_string(n) : "nothing selected to hide");
                }
                ImGui::MenuItem("hidden...", nullptr, &showHidden);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("what is hidden, and how to bring it back");
                ImGui::Separator();
                ImGui::MenuItem("remove all...", nullptr, &showRemoveAll);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("clear out every entity of one class, or every clip brush");
                }
                ImGui::Separator();
                if (ImGui::MenuItem("shift-drag makes a copy", nullptr, &shiftDragDuplicates)) {
                    content::saveSetting(contentConfigPath(), "shift_drag_duplicates",
                                         shiftDragDuplicates ? "1" : "0");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("shift+drag a brush to drag a copy of it, leaving the original");
                }
                if (ImGui::MenuItem("one gizmo at a time", "ctrl+1/2/3", &gizmoModes)) saveGizmoModes();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("show only the move arrows, the resize balls or the rotate rings, "
                                      "rather than all of them at once");
                }
                if (gizmoModes) ImGui::TextDisabled("   showing: %s", gizmoModeName());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("content")) {
                ImGui::MenuItem("texture browser", "T", &showTextures);
                ImGui::MenuItem("lighting", nullptr, &showLighting);
                if (ImGui::MenuItem("cs:s folder...")) {
                    if (auto folder = platform::chooseFolder("pick your Counter-Strike Source folder")) {
                        if (content::isCssInstall(*folder)) {
                            content::saveCssPath(contentConfigPath(), *folder);
                            gameContent.mount(*folder);
                            textures.clear();
                            thumbnails.clear();
                            refreshBrowsable();
                            builtRevision = -1;
                            status("CS:S content mounted");
                        } else {
                            status("that folder has no cstrike/cstrike_pak_dir.vpk");
                        }
                    }
                }
                if (ImGui::MenuItem("texture pack...")) {
                    if (auto file = platform::chooseZipToOpen("pick a CS:S texture pack")) {
                        if (gameContent.mountPack(*file)) {
                            content::saveTexturePackPath(contentConfigPath(), *file);
                            content::saveCssPath(contentConfigPath(), {});
                            textures.clear();
                            thumbnails.clear();
                            refreshBrowsable();
                            builtRevision = -1;
                            status("texture pack loaded");
                        } else {
                            status("couldn't open that texture pack");
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("a .zip from the game's file > export cs:s textures (or css_textures.zip next to the app)");
                }
                ImGui::TextDisabled("%s", gameContent.root().empty()
                                              ? (gameContent.mounted() ? "CS:S: texture pack" : "CS:S: not found (built-in textures only)")
                                              : "CS:S: mounted");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("help")) {
                ImGui::MenuItem("controls", nullptr, &showHelp);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // tools
        ImGui::SetNextWindowPos(ImVec2(8, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(230, 0), ImGuiCond_FirstUseEver);
        bool toolsOpen = ImGui::Begin("tools");
        ui::keepWindowOnScreen(fitPanels);
        if (toolsOpen) {
            static constexpr std::array<std::pair<editor::Tool, const char*>, 11> kTools = {{
                {editor::Tool::Select, "1"}, {editor::Tool::Block, "2"}, {editor::Tool::Ramp, "3"},
                {editor::Tool::Spawn, "4"}, {editor::Tool::StartZone, "5"}, {editor::Tool::EndZone, "6"},
                {editor::Tool::Teleport, "7"}, {editor::Tool::Booster, "8"}, {editor::Tool::Push, "9"},
                {editor::Tool::PointEntity, "0"}, {editor::Tool::Light, "L"},
            }};
            for (const auto& [t, key] : kTools) {
                std::string label = std::string(key) + "  " + editor::toolName(t);
                if (ImGui::RadioButton(label.c_str(), tool == t)) tool = t;
            }
            ImGui::Separator();
            if (tool == editor::Tool::Ramp) {
                static constexpr const char* kDirs[] = {"high at +x", "high at -x", "high at +y", "high at -y"};
                int dir = static_cast<int>(ed.rampDir);
                if (ImGui::Combo("##rampdir", &dir, kDirs, 4)) ed.rampDir = static_cast<vmf::RampDir>(dir);
                ImGui::TextDisabled("taller than wide = surf ramp");
            }
            if (tool == editor::Tool::StartZone || tool == editor::Tool::EndZone) {
                static constexpr const char* kTracks[] = {"main", "bonus 1", "bonus 2", "bonus 3", "bonus 4",
                                                          "bonus 5", "bonus 6", "bonus 7", "bonus 8"};
                ImGui::Combo("track", &ed.zoneTrack, kTracks, vmf::kZoneTracks);
                ImGui::TextDisabled("placing again moves that track's zone");
            }
            if (tool == editor::Tool::PointEntity) {
                ImGui::TextUnformatted("classname");
                inputString("##pointclass", ed.pointEntityClass);
                if (ImGui::BeginCombo("##presets", "presets...")) {
                    for (const char* preset : kPointEntityPresets) {
                        if (ImGui::Selectable(preset)) ed.pointEntityClass = preset;
                    }
                    ImGui::EndCombo();
                }
            }
            if (tool == editor::Tool::Block || tool == editor::Tool::Ramp) {
                ImGui::TextUnformatted("texture (pick in the browser, T)");
                ImGui::TextDisabled("%s", ed.brushMaterial.c_str());
            }
            ImGui::Separator();
            ImGui::Text("grid %g  ([ ])", ed.grid);
            ImGui::Separator();
            {
                float speed = camera.moveSpeed();
                ImGui::SetNextItemWidth(-1);
                // logarithmic: the useful range runs from inching around a jump to crossing a whole map, and that isn't a linear slide
                if (ImGui::SliderFloat("##flyspeed", &speed, 50.0f, 10000.0f, "fly speed %.0f",
                                       ImGuiSliderFlags_Logarithmic)) {
                    camera.setMoveSpeed(speed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    content::saveSetting(contentConfigPath(), "camera_speed",
                                         vmf::formatNumber(camera.moveSpeed()));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("scroll while flying does this too; shift flies 3x");

                float sens = camera.mouseSensitivity();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##sensitivity", &sens, 0.1f, 10.0f, "sensitivity %.2f")) {
                    camera.setMouseSensitivity(sens);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    content::saveSetting(contentConfigPath(), "camera_sensitivity",
                                         vmf::formatNumber(camera.mouseSensitivity()));
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("same scale as CS:S, so copy your in-game value");
            }
        }
        ImGui::End();

        // inspector
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(window.width()) - 408, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 520), ImGuiCond_FirstUseEver);
        bool inspectorOpen = ImGui::Begin("inspector");
        ui::keepWindowOnScreen(fitPanels);
        if (inspectorOpen) {
            glm::vec3 mn, mx;
            bool hasBounds = ed.selectionBounds(mn, mx);
            vmf::Entity* entity = ed.selectedEntity();
            vmf::Solid* worldSolid = ed.selectedWorldSolid();

            if (!entity && !worldSolid) {
                ImGui::TextDisabled("nothing selected -- click something with the select tool (1)");
            }

            // brush bounds (world brushes and brush entities)
            bool brushy = worldSolid || (entity && !entity->solids.empty());
            if (brushy && hasBounds) {
                glm::vec3 newMin = mn, newMax = mx;
                bool c1 = ImGui::DragFloat3("min", glm::value_ptr(newMin), 1.0f);
                if (ImGui::IsItemActivated()) ed.checkpoint();
                bool c2 = ImGui::DragFloat3("max", glm::value_ptr(newMax), 1.0f);
                if (ImGui::IsItemActivated()) ed.checkpoint();
                if (c1 || c2) ed.resizeSelection(newMin, newMax, false);

                const vmf::Solid& first = worldSolid ? *worldSolid : entity->solids.front();
                std::string material = first.sides.empty() ? "" : first.sides.front().material;
                if (inputString("material", material, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    ed.setSelectionMaterial(material);
                }
                if (textures.get(material).source == content::ResolvedMaterial::Source::Missing) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "MT: texture not found%s",
                                       gameContent.mounted() ? "" : " (no CS:S mounted)");
                } else {
                    ImGui::TextDisabled("enter to apply to every face");
                }
            }

            if (worldSolid) {
                ImGui::Separator();
                ImGui::TextUnformatted("world brush");
                inputString("##tieclass", tieClass);
                ImGui::SameLine();
                if (ImGui::BeginCombo("##tiepresets", "", ImGuiComboFlags_NoPreview)) {
                    for (const char* preset : kBrushEntityPresets) {
                        if (ImGui::Selectable(preset)) tieClass = preset;
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("tie to entity")) ed.tieSelectionToEntity(tieClass);
            }

            if (entity) {
                ImGui::Separator();
                ImGui::Text("entity: %s", entity->classname().c_str());
                if (!entity->solids.empty() && ImGui::Button("move brushes to world")) {
                    ed.moveSelectionToWorld();
                    entity = nullptr;
                }
            }
            if (entity && entity->solids.empty()) {
                glm::vec3 origin = collision::parseKeyValueVec3(entity->get("origin"));
                if (ImGui::DragFloat3("origin", glm::value_ptr(origin), 1.0f)) {
                    entity->set("origin", vmf::formatVec3(origin));
                    ed.touched();
                }
                if (ImGui::IsItemActivated()) ed.checkpoint();
            }
            if (entity) {
                ImGui::Separator();
                ImGui::TextUnformatted("keyvalues");
                int removeAt = -1;
                for (size_t i = 0; i < entity->keyValues.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    std::string key = entity->keyValues[i].first;
                    std::string value = entity->keyValues[i].second;
                    ImGui::SetNextItemWidth(130);
                    bool k = inputString("##k", key);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-30);
                    bool v = inputString("##v", value);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    if (k || v) {
                        entity->keyValues[i] = {key, value};
                        ed.touched();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (removeAt >= 0) {
                    ed.checkpoint();
                    entity->keyValues.erase(entity->keyValues.begin() + removeAt);
                    ed.touched();
                }
                if (ImGui::SmallButton("+ keyvalue")) {
                    ed.checkpoint();
                    entity->keyValues.emplace_back("key", "value");
                    ed.touched();
                }

                ImGui::Separator();
                ImGui::TextUnformatted("outputs  (target,input,param,delay,times)");
                removeAt = -1;
                for (size_t i = 0; i < entity->connections.size(); ++i) {
                    ImGui::PushID(1000 + static_cast<int>(i));
                    std::string name = entity->connections[i].first;
                    std::string value = entity->connections[i].second;
                    ImGui::SetNextItemWidth(110);
                    bool k = inputString("##o", name);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-30);
                    bool v = inputString("##ov", value);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    if (k || v) {
                        entity->connections[i] = {name, value};
                        ed.touched();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (removeAt >= 0) {
                    ed.checkpoint();
                    entity->connections.erase(entity->connections.begin() + removeAt);
                    ed.touched();
                }
                if (ImGui::SmallButton("+ output")) {
                    ed.checkpoint();
                    entity->connections.emplace_back("OnStartTouch", "!activator,AddOutput,basevelocity 0 0 500,0,-1");
                    ed.touched();
                }
            }
        }
        ImGui::End();

        // texture browser
        if (showTextures) {
            ImGui::SetNextWindowPos(ImVec2(8, 360), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
            bool texturesOpen = ImGui::Begin("textures", &showTextures);
            ui::keepWindowOnScreen(fitPanels);
            if (texturesOpen) {
                if (!gameContent.mounted()) {
                    ImGui::TextWrapped("No CS:S found: built-in textures only. content > cs:s folder... to point at it.");
                }
                ImGui::SetNextItemWidth(-1);
                inputString("##texfilter", textureFilter);
                if (textureFilter.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("search, e.g. brick, grass, dev");
                std::string needle = upperAscii(textureFilter);
                std::vector<const std::string*> matches;
                for (const auto& name : browsable) {
                    if (needle.empty() || name.find(needle) != std::string::npos) matches.push_back(&name);
                }
                ImGui::Text("%zu textures  |  current: %s", matches.size(), ed.brushMaterial.c_str());
                bool hasSelection = ed.selectedWorldSolid() || (ed.selectedEntity() && !ed.selectedEntity()->solids.empty());
                ImGui::BeginDisabled(!hasSelection);
                if (ImGui::Button("apply to selection")) ed.setSelectionMaterial(ed.brushMaterial);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("click = pick, double-click = pick + apply");

                constexpr float kThumb = 72.0f;
                ImGui::BeginChild("##grid");
                int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (kThumb + 8.0f)));
                int rows = (static_cast<int>(matches.size()) + perRow - 1) / perRow;
                int loadBudget = 6;  // decoding is slow; spread new thumbnails over frames
                ImGuiListClipper clipper;
                clipper.Begin(rows, kThumb + 8.0f);
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        for (int col = 0; col < perRow; ++col) {
                            size_t i = static_cast<size_t>(row * perRow + col);
                            if (i >= matches.size()) break;
                            const std::string& name = *matches[i];
                            if (col > 0) ImGui::SameLine();
                            ImGui::PushID(static_cast<int>(i));
                            const render::Texture* thumb = thumbnails.get(name, loadBudget);
                            bool selected = upperAscii(ed.brushMaterial) == name;
                            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
                            bool clicked = thumb ? ImGui::ImageButton("##t", static_cast<ImTextureID>(thumb->id()),
                                                                      ImVec2(kThumb, kThumb))
                                                 : ImGui::Button("...", ImVec2(kThumb + 6, kThumb + 6));
                            if (selected) ImGui::PopStyleColor();
                            if (clicked) ed.brushMaterial = name;
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", name.c_str());
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hasSelection) {
                                    ed.brushMaterial = name;
                                    ed.setSelectionMaterial(name);
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // compile: where the .bsp goes, which compiler makes it, and the tools' own output as it runs
        if (showCompile) {
            if (compileToolsStale && !compileRunning) {
                compileTools = vmf::findCompileTools(gameContent.root().string(), compileSdkBin);
                compileToolsStale = false;
            }
            ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_FirstUseEver);
            bool compileOpen = ImGui::Begin("compile to .bsp", &showCompile);
            ui::keepWindowOnScreen(fitPanels);
            if (compileOpen) {
                ImGui::TextUnformatted("output");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-90);
                char outBuf[1024];
                std::snprintf(outBuf, sizeof(outBuf), "%s", compileOutput.c_str());
                if (ImGui::InputText("##output", outBuf, sizeof(outBuf))) compileOutput = outBuf;
                ImGui::SameLine();
                if (ImGui::Button("browse...")) {
                    if (auto file = platform::chooseBspToSave(compileOutput)) compileOutput = *file;
                }

                ImGui::Separator();

                int backend = static_cast<int>(compileOptions.backend);
                ImGui::TextUnformatted("compiler");
                ImGui::RadioButton("auto", &backend, static_cast<int>(vmf::CompileBackend::Auto));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("vbsp/vvis/vrad when they are installed, else built-in");
                ImGui::SameLine();
                ImGui::RadioButton("vbsp/vvis/vrad", &backend, static_cast<int>(vmf::CompileBackend::ValveTools));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("a real compile: proper visibility and baked lighting, ready for a server");
                }
                ImGui::SameLine();
                ImGui::RadioButton("built-in", &backend, static_cast<int>(vmf::CompileBackend::Native));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("needs nothing installed, but no visibility data and fullbright lighting");
                }
                compileOptions.backend = static_cast<vmf::CompileBackend>(backend);

                if (compileTools.found) {
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "tools: %s", compileTools.note.c_str());
                } else {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "tools: %s", compileTools.note.c_str());
                    ImGui::PopTextWrapPos();
                }
                if (ImGui::Button("compile tools...")) {
                    if (auto folder = platform::chooseFolder("pick the Source SDK bin folder (with vbsp)")) {
                        compileSdkBin = *folder;
                        content::saveCompileToolsPath(contentConfigPath(), compileSdkBin);
                        compileToolsStale = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("the bin folder holding vbsp, vvis and vrad");
                }
                if (!compileSdkBin.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("forget")) {
                        compileSdkBin.clear();
                        content::saveCompileToolsPath(contentConfigPath(), {});
                        compileToolsStale = true;
                    }
                }

                ImGui::BeginDisabled(compileOptions.backend == vmf::CompileBackend::Native);
                ImGui::Checkbox("run vvis", &compileOptions.runVvis);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("visibility. the slow one; skip it while iterating");
                ImGui::SameLine();
                ImGui::Checkbox("run vrad", &compileOptions.runVrad);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("lighting. without it the map compiles unlit");
                ImGui::SameLine();
                ImGui::Checkbox("fast", &compileOptions.fast);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("-fast for vvis and vrad: rougher, far quicker");
                ImGui::EndDisabled();

                ImGui::Separator();

                if (compileRunning) {
                    if (ImGui::Button("cancel", ImVec2(120, 0))) compileCancel = true;
                    ImGui::SameLine();
                    ImGui::Text("compiling...");
                } else {
                    if (ImGui::Button("compile", ImVec2(120, 0))) {
                        startCompile();
                        compileScrollToEnd = true;
                    }
                    if (compileSucceeded && !compileFinishedPath.empty()) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "wrote %s",
                                           fileNameOf(compileFinishedPath).c_str());
                        ImGui::SameLine();
                        if (ImGui::Button("play it")) {
                            if (!platform::launchSibling("movengine", {compileFinishedPath})) {
                                status("couldn't start movengine");
                            }
                        }
                    }
                }

                ImGui::BeginChild("compilelog", ImVec2(0, 0), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                {
                    std::lock_guard<std::mutex> lock(compileLogMutex);
                    for (const auto& line : compileLog) ImGui::TextUnformatted(line.c_str());
                    if (compileRunning || compileScrollToEnd) {
                        ImGui::SetScrollHereY(1.0f);
                        compileScrollToEnd = false;
                    }
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // hidden: what's out of the way, and the way back. a hidden brush is neither drawn nor clickable so without this list it would be lost
        if (showHidden) {
            ImGui::SetNextWindowSize(ImVec2(340, 300), ImGuiCond_FirstUseEver);
            bool hiddenOpen = ImGui::Begin("hidden", &showHidden);
            ui::keepWindowOnScreen(fitPanels);
            if (hiddenOpen) {
                auto items = ed.hiddenItems();
                if (items.empty()) {
                    ImGui::TextDisabled("nothing is hidden");
                    ImGui::TextDisabled("select something and press h");
                } else {
                    ImGui::Text("%zu hidden", items.size());
                    ImGui::SameLine();
                    if (ImGui::Button("unhide all  (shift+h)")) {
                        status("unhid " + std::to_string(ed.hiddenCount()));
                        ed.unhideAll();
                    }
                    ImGui::Separator();
                    ImGui::BeginChild("hiddenlist");
                    for (size_t i = 0; i < items.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Button("unhide", ImVec2(80, 0))) {
                            ed.unhide(items[i].second);
                            ImGui::PopID();
                            break;  // the list just shifted
                        }
                        ImGui::SameLine();
                        ImGui::TextUnformatted(items[i].first.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                }
            }
            ImGui::End();
        }

        // remove all: what the map holds, by the thousand, with a button each. decompiled maps arrive full of things you don't want
        if (showRemoveAll) {
            ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
            bool removeOpen = ImGui::Begin("remove all", &showRemoveAll);
            ui::keepWindowOnScreen(fitPanels);
            if (removeOpen) {
                ImGui::TextDisabled("one undo puts any of these back");
                ImGui::Separator();

                const int clips = ed.clipBrushCount();
                ImGui::BeginDisabled(clips == 0);
                if (ImGui::Button("remove##clips", ImVec2(90, 0))) {
                    int n = ed.removeClipBrushes();
                    status("removed " + std::to_string(n) + " clip brushes");
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Text("%d clip brushes", clips);
                ImGui::SameLine();
                ImGui::TextDisabled("ctrl+shift+c");

                ImGui::Separator();
                auto counts = ed.entityClassCounts();
                if (counts.empty()) {
                    ImGui::TextDisabled("no entities in this map");
                }
                ImGui::BeginChild("classes");
                for (const auto& [cls, n] : counts) {
                    ImGui::PushID(cls.c_str());
                    if (ImGui::Button("remove", ImVec2(90, 0))) {
                        int removed = ed.removeEntitiesOfClass(cls);
                        status("removed " + std::to_string(removed) + " " + cls);
                        ImGui::PopID();
                        break;  // the list just changed under us
                    }
                    ImGui::SameLine();
                    ImGui::Text("%d x %s", n, cls.c_str());
                    if (collision::toLowerAscii(cls) == "trigger_teleport") {
                        ImGui::SameLine();
                        ImGui::TextDisabled("ctrl+shift+t");
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // lighting: the sun (light_environment) and the selected light, as sliders and color pickers writing real source keyvalues
        if (showLighting) {
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(window.width()) - 408, 560), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
            bool lightingOpen = ImGui::Begin("lighting", &showLighting);
            ui::keepWindowOnScreen(fitPanels);
            if (lightingOpen) {
                ImGui::Checkbox("preview lighting", &previewLighting);
                ImGui::SameLine();
                ImGui::TextDisabled("(no shadows until compiled)");

                // "_light"/"_ambient" style "r g b brightness" <-> color + brightness.
                auto lightEditor = [&](vmf::Entity& e, const char* key, const char* label, float maxBrightness) {
                    std::istringstream iss(e.get(key, "255 255 255 200"));
                    float r = 255, g = 255, b = 255, br = 200;
                    iss >> r >> g >> b >> br;
                    float color[3] = {r / 255.0f, g / 255.0f, b / 255.0f};
                    ImGui::PushID(key);
                    bool changed = ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_NoInputs);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    changed |= ImGui::SliderFloat("##brightness", &br, 0.0f, maxBrightness, "brightness %.0f");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    ImGui::PopID();
                    if (changed) {
                        e.set(key, vmf::formatNumber(std::round(color[0] * 255)) + " " +
                                       vmf::formatNumber(std::round(color[1] * 255)) + " " +
                                       vmf::formatNumber(std::round(color[2] * 255)) + " " +
                                       vmf::formatNumber(std::round(br)));
                        ed.touched();
                    }
                };

                ImGui::SeparatorText("sun");
                if (vmf::Entity* s = ed.sun()) {
                    glm::vec3 angles = collision::parseKeyValueVec3(s->get("angles"));
                    float pitch = collision::parseKeyValueFloat(s->get("pitch", "0"));
                    if (pitch == 0.0f) pitch = angles.x;
                    float yaw = angles.y;
                    bool c1 = ImGui::SliderFloat("direction", &yaw, 0.0f, 360.0f, "%.0f deg");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    float elevation = -pitch;
                    bool c2 = ImGui::SliderFloat("height", &elevation, 1.0f, 90.0f, "%.0f deg above horizon");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    if (c1 || c2) {
                        s->set("angles", "0 " + vmf::formatNumber(std::round(yaw)) + " 0");
                        s->set("pitch", vmf::formatNumber(-std::round(elevation)));
                        ed.touched();
                    }
                    lightEditor(*s, "_light", "sun", 1000.0f);
                    lightEditor(*s, "_ambient", "ambient", 200.0f);
                    if (ImGui::Button("remove sun")) ed.removeSun();
                } else {
                    ImGui::TextDisabled("no sun in this map");
                    if (ImGui::Button("add sun")) ed.addSun();
                }

                ImGui::SeparatorText("atmosphere");
                if (vmf::Entity* f = ed.fog()) {
                    ImGui::Checkbox("preview fog", &previewFog);
                    glm::vec3 c = collision::parseKeyValueVec3(f->get("fogcolor", "160 180 200"));
                    float col[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
                    bool changed = ImGui::ColorEdit3("fog colour", col, ImGuiColorEditFlags_NoInputs);
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    float start = collision::parseKeyValueFloat(f->get("fogstart", "1500"));
                    float end = collision::parseKeyValueFloat(f->get("fogend", "6000"));
                    float density = collision::parseKeyValueFloat(f->get("fogmaxdensity", "1"));
                    bool c1 = ImGui::SliderFloat("starts at", &start, 0.0f, 8000.0f, "%.0f units");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    bool c2 = ImGui::SliderFloat("solid by", &end, 1.0f, 16000.0f, "%.0f units");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    bool c3 = ImGui::SliderFloat("thickness", &density, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                    if (changed || c1 || c2 || c3) {
                        // keep them in order or the fog has no depth to fade over
                        if (end <= start) end = start + 1.0f;
                        f->set("fogcolor", vmf::formatNumber(std::round(col[0] * 255)) + " " +
                                               vmf::formatNumber(std::round(col[1] * 255)) + " " +
                                               vmf::formatNumber(std::round(col[2] * 255)));
                        f->set("fogstart", vmf::formatNumber(std::round(start)));
                        f->set("fogend", vmf::formatNumber(std::round(end)));
                        f->set("fogmaxdensity", vmf::formatNumber(density));
                        f->set("fogenable", "1");
                        ed.touched();
                    }
                    if (ImGui::Button("remove fog")) ed.removeFog();
                } else {
                    ImGui::TextDisabled("no fog in this map");
                    if (ImGui::Button("add fog")) ed.addFog();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("an env_fog_controller: distance haze, and it compiles into the map");
                    }
                }

                ImGui::SeparatorText("selected light");
                vmf::Entity* sel = ed.selectedEntity();
                std::string cls = sel ? collision::toLowerAscii(sel->classname()) : "";
                if (sel && (cls == "light" || cls == "light_spot")) {
                    lightEditor(*sel, "_light", "color", 1000.0f);
                    float fifty = collision::parseKeyValueFloat(sel->get("_fifty_percent_distance", "128"));
                    if (ImGui::SliderFloat("reach", &fifty, 16.0f, 2048.0f, "half bright at %.0f units")) {
                        sel->set("_fifty_percent_distance", vmf::formatNumber(std::round(fifty)));
                        ed.touched();
                    }
                    if (ImGui::IsItemActivated()) ed.checkpoint();
                } else {
                    ImGui::TextDisabled("place one with the light tool (L), or select one");
                }
            }
            ImGui::End();
        }

        drawHelpWindow(showHelp, fitPanels);

        // status bar
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 0));
        ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing);
        statusTimer -= dt;
        // only mentioned when there's something to mention so the usual case stays as short as it was
        const int selectedNow = ed.selectionCount();
        const int hiddenNow = ed.hiddenCount();
        std::string selectedLabel = selectedNow > 1 ? "  |  " + std::to_string(selectedNow) + " selected" : "";
        std::string hiddenLabel = hiddenNow > 0 ? "  |  " + std::to_string(hiddenNow) + " hidden" : "";
        // worth saying out loud: with it on, clicking a grouped platform picks up one brush, which looks like grouping having quietly stopped working
        std::string groupLabel = ed.ignoreGroups ? "  |  ignoring groups" : "";
        ImGui::Text("%s  |  tool: %s  |  grid %g  |  %zu brushes, %zu entities%s%s%s  |  %s", fileNameOf(ed.path).c_str(),
                    editor::toolName(tool), ed.grid, ed.doc.world.solids.size(), ed.doc.entities.size(), selectedLabel.c_str(), hiddenLabel.c_str(), groupLabel.c_str(),
                    statusTimer > 0.0f ? statusText.c_str() : "hold right mouse to fly, F5 to play");
        ImGui::End();

        // --- 3D view ----------------------------------------------------------
        glClearColor(0.16f, 0.17f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        float aspect = static_cast<float>(window.width()) / static_cast<float>(std::max(window.height(), 1));
        glm::mat4 viewProj = camera.projectionMatrix(aspect) * camera.viewMatrix();

        glUseProgram(program);
        glUniformMatrix4fv(uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
        glUniform1i(uTexture, 0);
        bsp::SceneLighting lighting = vmf::sceneLighting(ed.doc);
        int lightingMode = (previewLighting && lighting.hasLights) ? 2 : 0;
        lightingUniforms.apply(lightingMode, 0, lighting, camera.position, 1.0f);
        {
            // off unless the map has a fog controller: an editor that hides the far half of your map by default would be no help
            bsp::SceneFog fog = previewFog ? vmf::sceneFog(ed.doc) : bsp::SceneFog{};
            glUniform1f(uFogEndLoc, fog.enabled ? fog.end : 0.0f);
            glUniform1f(uFogStartLoc, fog.enabled ? fog.start : 0.0f);
            glUniform1f(uFogDensityLoc, fog.enabled ? fog.maxDensity : 0.0f);
            glUniform3fv(uFogColorLoc, 1, glm::value_ptr(fog.color));
            glUniform3fv(uViewPosLoc, 1, glm::value_ptr(camera.position));
        }
        glActiveTexture(GL_TEXTURE0);
        for (const auto& g : scene.groups) {
            if (g.translucent) continue;
            glBindTexture(GL_TEXTURE_2D, g.texture);
            g.mesh.draw();
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(boxProgram);
        glUniformMatrix4fv(uBoxViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
        for (const auto& e : ed.doc.entities) {
            if (!e.solids.empty()) continue;
            glm::vec3 pmn, pmx;
            editor::Editor::pointEntityBounds(e, pmn, pmx);
            glm::vec4 c = pointEntityColor(e.classname());
            glUniform4f(uBoxColor, c.r, c.g, c.b, c.a);
            box.draw(uBoxModel, pmn, pmx);
        }

        glUseProgram(program);
        lightingUniforms.apply(lightingMode, 0, lighting, camera.position, 0.4f);
        for (const auto& g : scene.groups) {
            if (!g.translucent) continue;
            glBindTexture(GL_TEXTURE_2D, g.texture);
            g.mesh.draw();
        }

        glm::vec3 smn, smx;
        if (ed.selectionBounds(smn, smx)) {
            glUseProgram(boxProgram);
            glDisable(GL_DEPTH_TEST);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glUniform4f(uBoxColor, 1.0f, 0.9f, 0.2f, 1.0f);
            box.draw(uBoxModel, smn - glm::vec3(0.5f), smx + glm::vec3(0.5f));
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            // handles, solid and still with depth off so they're reachable even when the brush is inside something. red/green/blue per axis, as every 3d editor does it. the one under the cursor or being dragged goes white
            if (tool == editor::Tool::Select) {
                auto axisColour = [&](Handle h, bool lit) {
                    const int a = handleAxis(h);
                    glm::vec3 col(a == 0 ? 1.0f : 0.25f, a == 1 ? 1.0f : 0.25f, a == 2 ? 1.0f : 0.25f);
                    return lit ? glm::vec3(1.0f) : col;
                };
                auto lit = [&](Handle h) {
                    return gizmo.active == h || (gizmo.active == Handle::None && gizmo.hovered == h);
                };
                // move: cubes out on the axes
                for (Handle h : {Handle::MoveX, Handle::MoveY, Handle::MoveZ}) {
                    if (!modeShows(h)) continue;
                    glm::vec3 c = handleCenter(h, smn, smx);
                    float s = handleSize(c);
                    glm::vec3 col = axisColour(h, lit(h));
                    glUniform4f(uBoxColor, col.r, col.g, col.b, 1.0f);
                    box.draw(uBoxModel, c - glm::vec3(s), c + glm::vec3(s));
                }
                // resize: balls on the faces
                for (Handle h : {Handle::FaceNegX, Handle::FacePosX, Handle::FaceNegY, Handle::FacePosY,
                                  Handle::FaceNegZ, Handle::FacePosZ}) {
                    if (!modeShows(h)) continue;
                    glm::vec3 c = handleCenter(h, smn, smx);
                    glm::vec3 col = axisColour(h, lit(h));
                    glUniform4f(uBoxColor, col.r, col.g, col.b, 1.0f);
                    drawBall(uBoxModel, c, faceHandleSize(c));
                }
                // rotate: a ring per axis, just outside the rest
                {
                    const glm::vec3 c = (smn + smx) * 0.5f;
                    const float r = ringRadius(smn, smx);
                    for (int a = 0; a < 3; ++a) {
                        Handle h = a == 0 ? Handle::RotX : (a == 1 ? Handle::RotY : Handle::RotZ);
                        if (!modeShows(h)) continue;
                        glm::vec3 col = axisColour(h, lit(h));
                        glUniform4f(uBoxColor, col.r, col.g, col.b, 1.0f);
                        drawRing(uBoxModel, c, r, a);
                    }
                }
            }
            glEnable(GL_DEPTH_TEST);
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        imgui.render();
        window.swap();
    }

    // a compile in flight holds references to locals in this scope so it has to finish before any of them go away. ask it to stop first so quitting mid compile doesn't wait out a whole vvis run
    if (compileThread.joinable()) {
        compileCancel = true;
        compileThread.join();
    }

    scene.destroy();
    textures.clear();
    thumbnails.clear();
    box.destroy();
    glDeleteProgram(program);
    glDeleteProgram(boxProgram);
    imgui.shutdown();
    window.shutdown();
    return 0;
}
