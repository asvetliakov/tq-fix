#pragma once

#include <windows.h>
#include <d3d11.h>

namespace tq {
namespace grass {

// Runtime probe of Titan Quest's grass subsystem.
//
// Grass is not level data and not an entity. Engine.dll exports the whole
// subsystem by name -- 5599 mangled C++ symbols, of which sixteen are grass --
// and every one of them hangs off Terrain:
//
//   Terrain::CreateGrassGeometry() -> TerrainBase::GrassGeometry
//   Terrain::DisplaceGrass(float, float, float, float)
//   TerrainRenderInterface[RT]::RenderGrass(Name&, Canvas&, SceneRenderer&,
//                                           RenderablePass&)
//   TerrainType::SetGrassShaderParams(GraphicsShader2*)
//   Terrain::maxNumGrassPlanes = 350
//   Terrain::useGrassBufferCache = true, maxGrassBufferCacheSize = 15
//
// The level stores a painted terrain type; the blade planes are generated at
// run time from that type's BladeHeight / BladeSpacing / BladeWidth /
// BladeVariations database fields. So there is nothing in the level to
// duplicate, and a vertex shader is the wrong end of the pipe: it sees one
// vertex at a time and therefore has no pivot to turn a blade about, which is
// what closed every route in research/grass.md.
//
// CreateGrassGeometry is the other end. It allocates two blocks of 44800 and
// 11200 bytes -- baked immediates, 350 planes at 128 bytes each -- and returns
// two pointers. There the four corners of a plane exist together, so a
// per-plane pivot is an average of four floats.
//
// Measured rather than assumed: with both RenderGrass implementations
// detoured, only TerrainRenderInterfaceRT::RenderGrass ever ran, and
// Terrain::CreateGrassGeometry was never called at all -- it belongs to the
// implementation that does not. The detour that remains exists so a draw hook
// can tell a grass draw from every other draw in the frame.
//
// Every patched byte is restored by shutdown().

// The entry points this module detours. Passed explicitly rather than resolved
// inside, so the off-game test can drive it with a synthetic image.
struct Exports {
    void* renderGrass;      // TerrainRenderInterface::RenderGrass
    void* renderGrassRT;    // TerrainRenderInterfaceRT::RenderGrass
};

// Detours whichever of the three are supplied, after validating each exact
// x86 prologue. A null member is skipped; a prologue that does not match is
// rejected without being written to. True when at least one detour took.
bool install(HMODULE engine, const Exports& exports);

// Resolves the exports by name and installs. A no-op unless grass is
// enhanced: the detour exists so a draw hook can tell a grass draw from every
// other draw in the frame.
void installFromModule(HMODULE engine);

void shutdown();
bool installed();

// True only while the live RenderGrass is on the stack, which is what lets a
// draw hook tell a grass draw from every other draw in the frame without
// matching a shader or a buffer.
bool rendering();

// Once per frame, before the game's own Present. Advances any twin waiting on
// a staging read: the frame after the copy was queued it becomes readable, and
// the frame after that it is mapped without waiting.
void onPresent(ID3D11DeviceContext* context);

// ---------------------------------------------------------------------------
// Crossed blades.
//
// A blade is one flat card, so it thins out as the camera comes round to its
// edge and the field never reads as having volume. Crossing it -- a second
// card through the same centre, turned a quarter turn -- is the standard fix,
// and the measurements make it cheap to do here: four consecutive vertices are
// one card, and their own centre is the pivot to turn about.
//
// It does not cost blades. The buffer holds 350 planes and the block sampled
// drew 50, so the geometry was never the constraint; the second card is drawn
// from a rotated copy of the same buffer, over the same index range, so every
// blade keeps its position and gains a crossing card. What it does cost is a
// second draw per block and twice the grass overdraw.
//
// The copy is built where the data exists: between the game's Map and Unmap.

// True when [graphics] grass is "enhanced", which is the default. Anything
// else, including a value that is not understood, leaves the game's own grass
// alone.
bool enabled();

// Prepares the grass-buffer table. Called from the D3D install: the work
// happens entirely at the device level and needs none of the Engine detours
// above, so it stays available with the probe off.
void installBuffers();

// The rotated twin of a grass stream, or null when there is none yet. The
// caller binds it in place of `source` and repeats the draw.
ID3D11Buffer* crossedBuffer(ID3D11Buffer* source);

// Called from the unmap hook once the game's own unmap has returned, so the
// copy is uploaded from our own code rather than from inside a driver call.
void afterUnmap(ID3D11DeviceContext* context);

// Starts a twin for a stream that was adopted after the game had already
// filled it, by copying what the buffer holds now. Called from the draw when
// no twin exists; the copy is read back and turned once a Present has actually
// carried it to the GPU, and never by waiting for it.
void seedFromDraw(ID3D11DeviceContext* context, ID3D11Buffer* source);

// Turns one card a quarter turn about its own centre, in place. Position only:
// the card keeps its size, its height, its uv column and its normal. False
// when the plane is not a card.
bool rotatePlane(float* plane);

// Grass streams are identified when created -- 44800 bytes, dynamic, vertex,
// CPU-writable -- and then confirmed plane by plane before anything is
// written, so a same-sized buffer from elsewhere is left alone.
void noteBufferCreated(ID3D11Buffer* buffer, const D3D11_BUFFER_DESC* desc);
void noteMap(ID3D11Resource* resource, UINT subresource,
             const D3D11_MAPPED_SUBRESOURCE* mapped);
void noteUnmap(ID3D11Resource* resource, UINT subresource);

// ---------------------------------------------------------------------------
// Pointer index.
//
// The map hooks see every mapped resource in the frame -- 2400 per frame
// measured in a grass area, almost none of them grass -- and a walk over the
// tracked streams costs more the longer a session explores, because the tables
// only grow. This makes the lookup independent of that.
//
// Open addressed, fixed size, with tombstones so a removal cannot break the
// probe chain of a key that hashed before it. Insertion fails rather than
// grows: a key that will not fit within the probe window is simply not
// tracked, which costs one uncrossed block and never corrupts a lookup.
struct PointerIndex {
    static const unsigned kSize = 2048;   // power of two
    static const unsigned kProbe = 8;
    void* keys[kSize];
    unsigned values[kSize];
};

void indexReset(PointerIndex& index);
bool indexInsert(PointerIndex& index, void* key, unsigned value);
bool indexRemove(PointerIndex& index, void* key);
bool indexLookup(const PointerIndex& index, void* key, unsigned* value);

// Pure, and the reason the identification is safe. One plane is 32 floats:
// position, normal, uv per vertex, four vertices. A grass card has an exact
// shape -- four bitwise-equal unit normals, a shared top edge and bottom edge,
// left and right edges each sharing x and z, and uv corners at v = 0 and 1 --
// and nothing else in the game's vertex data looks like that.
bool isGrassPlane(const float* plane);

}  // namespace grass
}  // namespace tq
