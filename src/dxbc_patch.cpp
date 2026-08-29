#include "dxbc_patch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace tq {
namespace dxbc {

namespace {

const uint32_t kDxbc = 0x43425844u;  // DXBC
const uint32_t kShex = 0x58454853u;  // SHEX
const uint32_t kShdr = 0x52444853u;  // SHDR

const uint32_t kOpcodeMask = 0x7ffu;
const uint32_t kLengthMask = 0x7f000000u;
const unsigned kLengthShift = 24;
const uint32_t kOpcodeCustomData = 53u;
const uint32_t kOpcodeImul = 38u;
const uint32_t kOpcodeUmin = 84u;
const uint32_t kOpcodeDclConstantBuffer = 89u;

const uint32_t kNullOperand = 0x0000d000u;
const uint32_t kTempDestAll = 0x001000f2u;
const uint32_t kTempSourceXyzw = 0x00100e46u;
const uint32_t kInputSourceXyzw = 0x00101e46u;
const uint32_t kImmediate32Vector = 0x00004002u;

uint32_t read32(const BYTE* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

void write32(BYTE* p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

bool range(SIZE_T at, SIZE_T bytes, SIZE_T size) {
    return at <= size && bytes <= size - at;
}

unsigned instructionLength(const uint32_t* p, unsigned remaining) {
    if (!remaining) return 0;
    uint32_t op = p[0] & kOpcodeMask;
    unsigned n = op == kOpcodeCustomData && remaining >= 2
               ? p[1] : (p[0] & kLengthMask) >> kLengthShift;
    return n && n <= remaining ? n : 0;
}

bool isBoneImul(const uint32_t* p, unsigned remaining) {
    return remaining >= 11 && (p[0] & kOpcodeMask) == kOpcodeImul
        && ((p[0] & kLengthMask) >> kLengthShift) == 11
        && p[1] == kNullOperand
        && p[2] == kTempDestAll
        && p[4] == kInputSourceXyzw
        && p[6] == kImmediate32Vector
        && p[7] == 3 && p[8] == 3 && p[9] == 3 && p[10] == 3;
}

}  // namespace

bool clampBoneIndices(const void* bytecode, SIZE_T bytecodeSize, PatchResult* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!bytecode || !out || bytecodeSize < 36 || read32((const BYTE*)bytecode) != kDxbc)
        return false;

    const BYTE* src = (const BYTE*)bytecode;
    uint32_t statedSize = read32(src + 24);
    uint32_t chunks = read32(src + 28);
    if (statedSize != bytecodeSize || !chunks || chunks > 64 || !range(32, chunks * 4u, bytecodeSize))
        return false;

    SIZE_T shaderChunk = 0;
    uint32_t shaderBytes = 0;
    unsigned shaderChunkIndex = 0;
    for (unsigned i = 0; i < chunks; i++) {
        SIZE_T at = read32(src + 32 + i * 4u);
        if (!range(at, 8, bytecodeSize)) return false;
        uint32_t fourcc = read32(src + at);
        uint32_t bytes = read32(src + at + 4);
        if (!range(at + 8, bytes, bytecodeSize)) return false;
        if (fourcc == kShex || fourcc == kShdr) {
            if (shaderChunk) return false;
            shaderChunk = at;
            shaderBytes = bytes;
            shaderChunkIndex = i;
        }
    }
    if (!shaderChunk || shaderBytes < 8 || (shaderBytes & 3u)) return false;

    const uint32_t* code = (const uint32_t*)(src + shaderChunk + 8);
    unsigned words = shaderBytes / 4u;
    if (code[1] != words) return false;

    unsigned imulAt = 0;
    UINT declarationVectors = 0;
    unsigned matches = 0;
    for (unsigned at = 2; at < words;) {
        unsigned n = instructionLength(code + at, words - at);
        if (!n) return false;
        uint32_t op = code[at] & kOpcodeMask;
        if (op == kOpcodeDclConstantBuffer && (code[at] & 0x800u) && n == 4) {
            // cb0[size], dynamicIndexed: operand token, slot 0, vector count.
            if (code[at + 1] == 0x00208e46u && code[at + 2] == 0)
                declarationVectors = code[at + 3];
        }
        if (isBoneImul(code + at, words - at)) {
            imulAt = at;
            matches++;
        }
        at += n;
    }

    // Every captured TQ skinning shader declares 27 float4x3 matrices: its
    // dynamic array therefore occupies the final 81 vectors. Refuse broader
    // shaders rather than guessing at an unrelated integer multiply.
    if (matches != 1 || declarationVectors < 81 || declarationVectors > 128)
        return false;

    const unsigned insertedWords = 10;
    const SIZE_T insertedBytes = insertedWords * 4u;
    if (bytecodeSize > (SIZE_T)-1 - insertedBytes) return false;
    SIZE_T newSize = bytecodeSize + insertedBytes;
    BYTE* dst = (BYTE*)malloc(newSize);
    if (!dst) return false;

    SIZE_T insertByte = shaderChunk + 8 + imulAt * 4u;
    memcpy(dst, src, insertByte);
    memcpy(dst + insertByte + insertedBytes, src + insertByte, bytecodeSize - insertByte);

    // Header and all chunks after SHEX moved by the inserted instruction.
    write32(dst + 24, (uint32_t)newSize);
    for (unsigned i = 0; i < chunks; i++) {
        uint32_t oldOffset = read32(src + 32 + i * 4u);
        write32(dst + 32 + i * 4u,
                i > shaderChunkIndex ? oldOffset + (uint32_t)insertedBytes : oldOffset);
    }
    write32(dst + shaderChunk + 4, shaderBytes + (uint32_t)insertedBytes);
    write32(dst + shaderChunk + 12, words + insertedWords);

    uint32_t* add = (uint32_t*)(dst + insertByte);
    UINT temp = code[imulAt + 3];
    UINT input = code[imulAt + 5];
    add[0] = (insertedWords << kLengthShift) | kOpcodeUmin;
    add[1] = kTempDestAll;
    add[2] = temp;
    add[3] = kInputSourceXyzw;
    add[4] = input;
    add[5] = kImmediate32Vector;
    add[6] = 26;
    add[7] = 26;
    add[8] = 26;
    add[9] = 26;

    // Existing IMUL now reads the just-clamped temp instead of the vertex
    // input and writes the scaled indices back to that same temp.
    uint32_t* movedImul = add + insertedWords;
    movedImul[4] = kTempSourceXyzw;
    movedImul[5] = temp;

    // DXMT's parser verifies container structure but does not consult this
    // legacy digest. Clear it rather than falsely retaining the source hash.
    memset(dst + 4, 0, 16);

    out->data = dst;
    out->size = newSize;
    return true;
}

void release(PatchResult* result) {
    if (!result) return;
    free(result->data);
    memset(result, 0, sizeof(*result));
}

}  // namespace dxbc
}  // namespace tq
