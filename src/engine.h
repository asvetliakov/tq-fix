#pragma once
#include <windows.h>

namespace tq { namespace engine {

// [performance] shadow_defer_cold_resources, a fix and defaulting to 1.
//
// Before any caster record exists, GraphicsMeshInstance ensures its root mesh
// merely to read the number of shadow passes. At 1, a root mesh in state 0 or
// 1 makes that exact caster return zero passes; state 0 is explicitly enqueued
// with the engine's normal preload arguments, and the caster returns when the
// mesh reaches state 2. This applies to opaque and alpha-tested mesh-instance
// casters only inside the directional map. Resident casters and the colour
// pass are unchanged.
//
// GraphicsMeshInstance's alpha-tested shadow styles need a base texture only
// to cut holes in the caster. A caster/pass whose verified base texture
// Resource is in state 0 or 1 is likewise omitted until resident. Opaque
// resident casters still render normally, but a material texture whose Name
// is absent from their active shadow shader is not loaded. This avoids
// rendering foliage/fences as solid while removing needless synchronous
// shadow-side texture load. GraphicsMeshInstance's optional
// bumpTexture override has the same stock ordering bug--EnsureAvailable runs
// before the setter checks the shader--so it is likewise skipped only when
// the active directional-shadow shader proves it has no bumpTexture input.
// The base mesh material can also carry a baseTexture that is immediately
// replaced by GraphicsMeshInstance+0x14. Inside the directional pass only,
// that earlier getter is skipped when the live override is non-null, distinct,
// and the material Name exactly matches baseTexture; the verified stock code
// ensures and binds the override before any draw.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 16384 reports omitted states, enqueue outcomes, and skipped
// material/bump dependencies when enabled.

// [performance] shadow_defer_cold_actor_pose, a fix and defaulting to 1.
//
// Run 68 proved another exact root-mesh EnsureAvailable occurs earlier, while
// GraphicsShadowMapDx11::RenderDirectional gathers actors: Actor::AddToScene
// calls Actor::UpdateMeshInstance, which enters GraphicsMeshInstance::UpdatePose.
// At 1, this switch queues a state-0 Actor root and defers that one pose update
// while the root is state 0/1. It implies the complete later shadow-defer gate,
// which then omits the still-cold caster until state 2. Other Actor update
// callers, colour rendering, point shadows, and resident actors are unchanged.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 16384 reports the exact state/enqueue outcome when enabled.

// [performance] terrain_preload_layers, a fix and defaulting to 1.
//
// Runtime TerrainRT::LoadRenderData creates each layer TerrainType's base,
// bump, and grass texture Resources by calling TerrainType::LoadTextures, but
// TerrainRT::PreLoad never calls the semantic TerrainType::PreLoad method that
// queues those Resources. At 1, the already verified LoadTextures call site is
// retargeted to a wrapper that calls the original and then the stock
// TerrainType::PreLoad(true) on that exact object. It queues through the game's
// existing ResourceLoaders and does not wait, omit colour, or invent a loader.
//
// It reaches install() with the performance probe off and brings no trace
// group. Group 32768 observes the same stock calls when enabled.

// [performance] secondary_pass_admission_budget, a fix and defaulting to 8.
//
// Run 83 found that the felt play transition introduces 134 previously unseen
// reflection renderables and 15 directional-shadow renderables at once, while
// Runs 81--82 showed that omitting one consumer merely moves GPU first use to
// the next consumer/frame. The first N identities render normally; identity
// N+1 proves a real pending population and self-arms admission. A deferred
// RenderPass still executes resource/material setup,
// but its D3D Draw/DrawIndexed calls are suppressed until that identity wins a
// later frame's budget. Normal colour rendering and already admitted objects
// are unchanged.
//
// It reaches install() with the performance probe off and brings no trace
// group. Count-only columns report triggers, admitted/deferred identities, and
// omitted secondary-pass draws when tracing is independently enabled.

// Coordinates audited Engine patches. Each performance feature owns its
// behavior; engine_probe supplies optional observers. Trace-off installation
// requests only the sites needed by the configured fixes.
void readOptions(const wchar_t* iniPath);
bool install(HMODULE engine);
void shutdown();

#ifdef TQ_SELFTEST
bool exerciseTraceOffHooksForTest();
#endif

} }
