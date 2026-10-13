#pragma once
// Forge — batteries-included 2D game engine.
// One include to rule them all:
//
//   #include <forge/forge.hpp>
//   int main() {
//       forge::EngineConfig cfg;
//       cfg.window.title = "My Game";
//       forge::Engine engine(cfg);
//       forge::Camera2D cam;
//       engine.runVariable([&](float dt) {
//           cam.update(dt);
//           forge::IVec2 vp = engine.viewport();
//           engine.batch().begin(cam.viewProj(vp.x, vp.y));
//           // ... draw ...
//           engine.batch().end();
//       });
//   }

#define FORGE_VERSION_MAJOR 0
#define FORGE_VERSION_MINOR 1
#define FORGE_VERSION_PATCH 0
#define FORGE_VERSION "0.1.0"

#include "forge/math/math.hpp"
#include "forge/core/logger.hpp"
#include "forge/core/time.hpp"
#include "forge/core/input.hpp"
#include "forge/core/window.hpp"
#include "forge/core/engine.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/graphics/shader.hpp"
#include "forge/graphics/texture.hpp"
#include "forge/graphics/camera.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/font.hpp"
#include "forge/graphics/framebuffer.hpp"
#include "forge/graphics/particles.hpp"
#include "forge/graphics/animation.hpp"
#include "forge/graphics/tilemap.hpp"
#include "forge/physics/physics.hpp"
#include "forge/audio/audio.hpp"
#include "forge/scene/ecs.hpp"
#include "forge/scene/components.hpp"
#include "forge/scene/scene.hpp"
#include "forge/assets/assets.hpp"
#include "forge/utils/profiler.hpp"
