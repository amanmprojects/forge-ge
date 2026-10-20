// test_docs — compile-time + runtime API drift guard.
//
// The agent docs claim to be copy-pasteable. This test takes the address of
// every signature that docs/agents/* documents with an explicit type, so a
// rename or parameter change fails the build instead of silently rotting the
// docs. It also checks that the documented pages exist and that the roadmap
// still lists every phase.
//
// Deliberately headless: no window, no GL, no audio.

#include <forge/forge.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

template <typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

// ---- signatures documented in docs/agents/* ----
// Core
using EngineDefaultProj = forge::Mat4 (forge::Engine::*)() const;
using WindowTitle = const std::string& (forge::Window::*)() const;
using WindowConfigGet = const forge::WindowConfig& (forge::Window::*)() const;
using InputStickDz = void (forge::Input::*)(float);
using InputTouchDown = void (forge::Input::*)(int64_t, forge::Vec2, float);
using InputTouchDrag = forge::Vec2 (forge::Input::*)(int64_t) const;
using InputSaveBinds = bool (forge::Input::*)(const std::string&) const;
using InputLoadBinds = bool (forge::Input::*)(const std::string&, std::string*);

// Graphics
using BatchScissor = void (forge::SpriteBatch::*)(forge::Rect);
using BatchBlend = void (forge::SpriteBatch::*)(forge::SpriteBatch::BlendMode);
using BatchNineSlice = void (forge::SpriteBatch::*)(const forge::Texture2D&, forge::Rect,
                                                    forge::Rect, float, forge::Color);
using BatchQuad = void (forge::SpriteBatch::*)(forge::Vec2, forge::Vec2, forge::Vec2,
                                               forge::Vec2, forge::Color,
                                               const forge::Texture2D*, forge::Rect);
using FbCreate = bool (forge::Framebuffer::*)(int, int, int);
using FbResize = bool (forge::Framebuffer::*)(int, int);
using FbRead = bool (forge::Framebuffer::*)(std::vector<uint8_t>&) const;
using TexAdopt = void (forge::Texture2D::*)(uint32_t, int, int, bool);
using TexUpdate = bool (forge::Texture2D::*)(const uint8_t*, int, int, int, int, int);
using FontBitmap = bool (forge::Font::*)(const std::string&);
using FontWrappedH = float (forge::Font::*)(const std::string&, float, float) const;
using TileSlopeSet = void (forge::Tilemap::*)(int, float, float);
using TileCollide = bool (forge::Tilemap::*)(forge::Rect, forge::Vec2*) const;
using AtlasAdd = int (forge::AtlasPacker::*)(int, int, const std::string&);
using AtlasAddRect = int (forge::AtlasPacker::*)(const forge::Rect&, const std::string&);
using AtlasPack = bool (forge::AtlasPacker::*)();
using LightsRender = void (forge::LightingSystem::*)(forge::SpriteBatch&, const forge::Camera2D&,
                                                     forge::IVec2, forge::Framebuffer*);
using PostApply = void (forge::PostProcess::*)(const forge::Texture2D&, forge::IVec2);
using StackPush = const forge::Texture2D& (forge::RenderTargetStack::*)(int, int);
using SkeletonLoad = bool (forge::Skeleton::*)(const std::string&);

// Physics
using PhysTrigger = void (forge::PhysicsWorld::*)(forge::TriggerCallback);
using PhysJoint = uint32_t (forge::PhysicsWorld::*)(const forge::Joint&);
using PhysDebug = void (forge::PhysicsWorld::*)(forge::SpriteBatch&,
                                                const forge::PhysicsDebugOptions&) const;
using PhysRay = bool (forge::PhysicsWorld::*)(forge::Vec2, forge::Vec2, uint32_t*,
                                              forge::Vec2*) const;

// Scene / assets / script / tools
using SaveSceneFn = bool (*)(const forge::Scene&, const std::string&,
                             const forge::AssetManager*);
using LoadSceneFn = bool (*)(forge::Scene&, const std::string&, forge::AssetManager*,
                             forge::SceneSaveStats*);
using MakePrefabFn = forge::Prefab (*)(const forge::Scene&, forge::Entity, const std::string&);
using InstantiateFn = forge::Entity (*)(forge::Scene&, const forge::Prefab&, forge::Vec2,
                                        forge::AssetManager*);
using AssetsSearch = void (forge::AssetManager::*)(const std::string&);
using AssetsShader = forge::Shader* (forge::AssetManager::*)(const std::string&, const std::string&);
using VmLoad = bool (forge::script::ScriptVM::*)(const std::string&, const std::string&);
using VmCall = forge::script::Value (forge::script::ScriptVM::*)(const std::string&,
                                                                 const std::vector<forge::script::Value>&);
using VmBind = void (forge::script::ScriptVM::*)(const std::string&, forge::script::HostFn);
using OverlayDraw = void (forge::DebugOverlay::*)(forge::SpriteBatch&, forge::IVec2,
                                                 const forge::Scene*,
                                                 const forge::PhysicsWorld*);
using JsonParseFile = forge::json::Value (*)(const std::string&, forge::json::ParseError*);
using JsonWriteFile = bool (*)(const std::string&, const forge::json::Value&, bool);

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // ---- compile-time: the documented signatures still exist ----
    static_assert(same<decltype(&forge::Engine::defaultProj), EngineDefaultProj>);
    static_assert(same<decltype(&forge::Window::title), WindowTitle>);
    static_assert(same<decltype(&forge::Window::config), WindowConfigGet>);
    static_assert(same<decltype(&forge::Input::setStickDeadzone), InputStickDz>);
    static_assert(same<decltype(&forge::Input::touchDown), InputTouchDown>);
    static_assert(same<decltype(&forge::Input::touchDrag), InputTouchDrag>);
    static_assert(same<decltype(&forge::Input::saveBinds), InputSaveBinds>);
    static_assert(same<decltype(&forge::Input::loadBinds), InputLoadBinds>);

    static_assert(same<decltype(&forge::SpriteBatch::setScissor), BatchScissor>);
    static_assert(same<decltype(&forge::SpriteBatch::setBlendMode), BatchBlend>);
    static_assert(same<decltype(&forge::SpriteBatch::drawNineSlice), BatchNineSlice>);
    static_assert(same<decltype(&forge::SpriteBatch::drawQuad), BatchQuad>);
    static_assert(same<decltype(&forge::Framebuffer::create), FbCreate>);
    static_assert(same<decltype(&forge::Framebuffer::resize), FbResize>);
    static_assert(same<decltype(&forge::Framebuffer::readPixels), FbRead>);
    static_assert(same<decltype(&forge::Texture2D::adoptRaw), TexAdopt>);
    static_assert(same<decltype(&forge::Texture2D::updateRegion), TexUpdate>);
    static_assert(same<decltype(&forge::Font::loadBitmapFnt), FontBitmap>);
    static_assert(same<decltype(&forge::Font::heightWrapped), FontWrappedH>);
    static_assert(same<decltype(&forge::Tilemap::setSlope), TileSlopeSet>);
    static_assert(same<decltype(&forge::Tilemap::collideRect), TileCollide>);
    static_assert(same<decltype(static_cast<AtlasAdd>(&forge::AtlasPacker::add)), AtlasAdd>);
    static_assert(same<decltype(static_cast<AtlasAddRect>(&forge::AtlasPacker::add)),
                       AtlasAddRect>);
    static_assert(same<decltype(&forge::AtlasPacker::pack), AtlasPack>);
    static_assert(same<decltype(&forge::LightingSystem::render), LightsRender>);
    static_assert(same<decltype(&forge::PostProcess::apply), PostApply>);
    static_assert(same<decltype(&forge::RenderTargetStack::push), StackPush>);
    static_assert(same<decltype(&forge::Skeleton::loadJson), SkeletonLoad>);

    static_assert(same<decltype(&forge::PhysicsWorld::onTriggerEnter), PhysTrigger>);
    static_assert(same<decltype(&forge::PhysicsWorld::addJoint), PhysJoint>);
    static_assert(same<decltype(&forge::PhysicsWorld::debugDraw), PhysDebug>);
    static_assert(same<decltype(&forge::PhysicsWorld::raycast), PhysRay>);

    static_assert(same<decltype(&forge::saveScene), SaveSceneFn>);
    static_assert(same<decltype(&forge::loadScene), LoadSceneFn>);
    static_assert(same<decltype(&forge::makePrefab), MakePrefabFn>);
    static_assert(same<decltype(&forge::instantiate), InstantiateFn>);
    static_assert(same<decltype(&forge::AssetManager::addSearchPath), AssetsSearch>);
    static_assert(same<decltype(&forge::AssetManager::loadShader), AssetsShader>);
    static_assert(same<decltype(&forge::script::ScriptVM::loadSource), VmLoad>);
    static_assert(same<decltype(&forge::script::ScriptVM::call), VmCall>);
    static_assert(same<decltype(&forge::script::ScriptVM::bind), VmBind>);
    static_assert(same<decltype(&forge::DebugOverlay::draw), OverlayDraw>);
    static_assert(same<decltype(&forge::json::parseFile), JsonParseFile>);
    static_assert(same<decltype(&forge::json::writeFile), JsonWriteFile>);

    // Version drift: the docs say 0.2.0.
    check(FORGE_VERSION_MAJOR == 0 && FORGE_VERSION_MINOR == 2,
          "version macros match the documented 0.2.0");

    // ---- runtime: documented files exist ----
    std::filesystem::path root = FORGE_SOURCE_DIR;
    const char* pages[] = {
        "docs/README.md",
        "docs/ROADMAP.md",
        "docs/agents/00-overview.md",
        "docs/agents/01-getting-started.md",
        "docs/agents/02-core.md",
        "docs/agents/03-math.md",
        "docs/agents/04-graphics.md",
        "docs/agents/05-animation-particles.md",
        "docs/agents/06-physics.md",
        "docs/agents/07-scene-ecs.md",
        "docs/agents/08-audio-assets.md",
        "docs/agents/09-recipes.md",
        "docs/agents/10-pitfalls.md",
        "docs/agents/11-scripting-tools.md",
        "CHANGELOG.md",
        "AGENTS.md",
        "README.md",
        "web/README.md",
    };
    for (const char* p : pages)
        check(std::filesystem::exists(root / p), p);

    // The roadmap must keep all phases (it is the post-compaction source of
    // truth; silently dropping a phase is a documentation regression).
    std::FILE* f = std::fopen((root / "docs/ROADMAP.md").string().c_str(), "rb");
    check(f != nullptr, "roadmap is readable");
    if (f) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        for (const char* phase : {"## Phase 0", "## Phase 1", "## Phase 2", "## Phase 3",
                                  "## Phase 4", "## Phase 5", "## Explicit non-goals"})
            check(text.find(phase) != std::string::npos, phase);
    }

    // The doc index must link every agent page (catch an orphaned new page).
    std::FILE* idx = std::fopen((root / "docs/README.md").string().c_str(), "rb");
    check(idx != nullptr, "docs index is readable");
    if (idx) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), idx)) > 0) text.append(buf, n);
        std::fclose(idx);
        for (const char* page : {"00-overview", "01-getting-started", "02-core", "03-math",
                                 "04-graphics", "05-animation-particles", "06-physics",
                                 "07-scene-ecs", "08-audio-assets", "09-recipes",
                                 "10-pitfalls", "11-scripting-tools"})
            check(text.find(page) != std::string::npos, page);
    }

    if (failures == 0) {
        std::printf("DOCS IN SYNC\n");
        return 0;
    }
    std::printf("%d DOC DRIFT FAILURES\n", failures);
    return 1;
}
