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
const uint32_t kOpcodeMad = 50u;
const uint32_t kOpcodeUmin = 84u;
const uint32_t kOpcodeDclConstantBuffer = 89u;
const uint32_t kOpcodeDp4 = 17u;
const uint32_t kOpcodeDiv = 14u;

const uint32_t kOperandTypeMask = 0x000ff000u;
const uint32_t kOperandTemp = 0x00000000u;
const uint32_t kOperandConstantBuffer = 0x00008000u;

const uint32_t kNullOperand = 0x0000d000u;
const uint32_t kTempDestAll = 0x001000f2u;
const uint32_t kTempSourceXyzw = 0x00100e46u;
const uint32_t kInputSourceXyzw = 0x00101e46u;
const uint32_t kImmediate32Vector = 0x00004002u;
const uint32_t kImmediate32Scalar = 0x00004001u;
const uint32_t kOpcodeAdd = 0u;

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

// Reads a register index out of an operand, skipping extended tokens.
bool operandRegister(const uint32_t* words, unsigned wordCount,
                     unsigned tokenAt, uint32_t type, unsigned* value) {
    if (tokenAt >= wordCount) return false;
    uint32_t token = words[tokenAt];
    if ((token & kOperandTypeMask) != type || ((token >> 20) & 3u) != 1u)
        return false;
    unsigned at = tokenAt + 1;
    while (token & 0x80000000u) {
        if (at >= wordCount) return false;
        token = words[at++];
    }
    if (at >= wordCount) return false;
    if (value) *value = words[at];
    return true;
}

// Titan Quest applies directional shadows in one deferred screen-space pass,
// not per material. Its pixel shader reconstructs world position from the
// depth buffer, projects it with a world-to-shadow matrix held in its own
// constant buffer, and runs the PCF taps there. The per-material receivers in
// the shader archive reference worldToShadowMatrix but never carry the
// directional map at runtime.
//
// The shape below is that receiver: four DP4s against consecutive cb0
// registers, three into one temporary and the fourth writing the homogeneous W
// back into the world temporary, followed by a projective divide that reorders
// the result to (depth, u, v). The point-light receiver divides by a distance
// instead and does not match.
bool isDeferredShadowReceiver(const uint32_t* code, unsigned wordCount) {
    struct Instruction { unsigned at, count, opcode; };
    Instruction instructions[512];
    unsigned instructionCount = 0;
    for (unsigned at = 2; at < wordCount;) {
        unsigned count = instructionLength(code + at, wordCount - at);
        if (!count || instructionCount == 512) return false;
        instructions[instructionCount++] = {at, count, code[at] & kOpcodeMask};
        at += count;
    }
    for (unsigned i = 0; i + 3 < instructionCount; ++i) {
        bool four = true;
        for (unsigned n = 0; n < 4; ++n)
            four = four && instructions[i + n].opcode == kOpcodeDp4
                        && instructions[i + n].count == 8;
        if (!four) continue;
        const uint32_t* first = code + instructions[i].at;
        unsigned projection = 0, world = 0;
        if (!operandRegister(first, 8, 1, kOperandTemp, &projection)
            || !operandRegister(first, 8, 3, kOperandTemp, &world)
            || projection == world)
            continue;
        const uint32_t masks[] = {0x12u, 0x22u, 0x42u};
        unsigned base = first[7];
        bool shaped = true;
        for (unsigned n = 0; n < 3 && shaped; ++n) {
            const uint32_t* p = code + instructions[i + n].at;
            unsigned destination = 0, source = 0;
            shaped = (p[1] & 0xffu) == masks[n]
                  && operandRegister(p, 8, 1, kOperandTemp, &destination)
                  && destination == projection
                  && operandRegister(p, 8, 3, kOperandTemp, &source)
                  && source == world
                  && (p[5] & kOperandTypeMask) == kOperandConstantBuffer
                  && p[6] == 0 && p[7] == base + n;
        }
        const uint32_t* fourth = code + instructions[i + 3].at;
        unsigned destination = 0, source = 0;
        shaped = shaped
              && (fourth[1] & 0xffu) == 0x82u
              && operandRegister(fourth, 8, 1, kOperandTemp, &destination)
              && destination == world
              && operandRegister(fourth, 8, 3, kOperandTemp, &source)
              && source == world
              && (fourth[5] & kOperandTypeMask) == kOperandConstantBuffer
              && fourth[6] == 0 && fourth[7] == base + 3;
        if (!shaped) continue;
        for (unsigned k = i + 4; k < instructionCount; ++k) {
            if (instructions[k].opcode != kOpcodeDiv) continue;
            const uint32_t* d = code + instructions[k].at;
            unsigned quotient = 0, numerator = 0, denominator = 0;
            return operandRegister(d, instructions[k].count, 1,
                                   kOperandTemp, &quotient)
                && quotient == projection
                && operandRegister(d, instructions[k].count, 3,
                                   kOperandTemp, &numerator)
                && numerator == projection
                && operandRegister(d, instructions[k].count, 5,
                                   kOperandTemp, &denominator)
                && denominator == world;
        }
    }
    return false;
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

bool enhanceShadowPcf(const void* bytecode, SIZE_T bytecodeSize, PatchResult* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!bytecode || !out || bytecodeSize < 36 || read32((const BYTE*)bytecode) != kDxbc)
        return false;
    const BYTE* src = (const BYTE*)bytecode;
    if (read32(src + 24) != bytecodeSize) return false;
    uint32_t chunks = read32(src + 28);
    if (!chunks || chunks > 64 || !range(32, chunks * 4u, bytecodeSize)) return false;
    SIZE_T shaderChunk = 0;
    uint32_t shaderBytes = 0;
    for (unsigned i = 0; i < chunks; ++i) {
        SIZE_T at = read32(src + 32 + i * 4u);
        if (!range(at, 8, bytecodeSize)) return false;
        uint32_t bytes = read32(src + at + 4);
        if (!range(at + 8, bytes, bytecodeSize)) return false;
        uint32_t fourcc = read32(src + at);
        if (fourcc == kShex || fourcc == kShdr) {
            if (shaderChunk) return false;
            shaderChunk = at; shaderBytes = bytes;
        }
    }
    if (!shaderChunk || shaderBytes < 8 || (shaderBytes & 3u)) return false;
    // These names make the match specific to TQ's shadow-receiving material
    // shaders before the instruction signature is considered.
    const char names[][24] = {"ShadowSamplerTex", "shadowBluriness", "worldToShadowMatrix"};
    for (unsigned n = 0; n < 3; ++n) {
        bool found = false;
        SIZE_T len = strlen(names[n]);
        for (SIZE_T i = 0; i + len <= bytecodeSize; ++i)
            if (!memcmp(src + i, names[n], len)) { found = true; break; }
        if (!found) return false;
    }
    const uint32_t* code = (const uint32_t*)(src + shaderChunk + 8);
    unsigned words = shaderBytes / 4u;
    if (code[1] != words || (code[0] >> 16) != 0) return false;  // pixel shader only
    unsigned horizontal = 0, vertical = 0;
    for (unsigned at = 2; at < words;) {
        unsigned count = instructionLength(code + at, words - at);
        if (!count) return false;
        const uint32_t* p = code + at;
        if ((p[0] & kOpcodeMask) == kOpcodeMad && count == 13 && p[6] == kImmediate32Vector) {
            if (p[7] == 0xbf000000u && p[8] == 0 && p[9] == 0x3f000000u && p[10] == 0)
                horizontal = at;
            else if (p[7] == 0 && p[8] == 0xbf000000u && p[9] == 0 && p[10] == 0x3f000000u)
                vertical = at;
        }
        at += count;
    }
    if (!horizontal || !vertical || vertical <= horizontal || vertical - horizontal > 80)
        return false;
    BYTE* dst = (BYTE*)malloc(bytecodeSize);
    if (!dst) return false;
    memcpy(dst, src, bytecodeSize);
    uint32_t* patched = (uint32_t*)(dst + shaderChunk + 8);
    patched[horizontal + 8] = 0xbf000000u;
    patched[horizontal + 10] = 0xbf000000u;
    patched[vertical + 7] = 0xbf000000u;
    patched[vertical + 8] = 0x3f000000u;
    patched[vertical + 9] = 0x3f000000u;
    memset(dst + 4, 0, 16);
    out->data = dst; out->size = bytecodeSize;
    return true;
}

bool tuneDeferredShadowFilter(const void* bytecode, SIZE_T bytecodeSize,
                              float factor, float biasScale, bool corners,
                              PatchResult* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !bytecode || bytecodeSize < 36 || !(factor > 0.0f) || factor > 1.0f
        || !(biasScale > 0.0f) || biasScale > 1.0f)
        return false;
    const BYTE* src = (const BYTE*)bytecode;
    if (read32(src) != kDxbc || read32(src + 24) != bytecodeSize) return false;
    uint32_t chunks = read32(src + 28);
    if (!chunks || chunks > 64 || !range(32, chunks * 4u, bytecodeSize)) return false;
    SIZE_T shaderChunk = 0;
    uint32_t shaderBytes = 0;
    for (unsigned i = 0; i < chunks; ++i) {
        SIZE_T at = read32(src + 32 + i * 4u);
        if (!range(at, 8, bytecodeSize)) return false;
        uint32_t bytes = read32(src + at + 4);
        if (!range(at + 8, bytes, bytecodeSize)) return false;
        uint32_t fourcc = read32(src + at);
        if (fourcc == kShex || fourcc == kShdr) {
            if (shaderChunk) return false;
            shaderChunk = at; shaderBytes = bytes;
        }
    }
    if (!shaderChunk || shaderBytes < 8 || (shaderBytes & 3u)) return false;
    const uint32_t* code = (const uint32_t*)(src + shaderChunk + 8);
    unsigned words = shaderBytes / 4u;
    if (code[1] != words || (code[0] >> 16) != 0) return false;  // pixel shader only

    // The four taps come from two MADs whose immediate vectors hold only 0 and
    // +/-0.5 and whose first source is the bluriness constant's W component.
    unsigned horizontal = ~0u, vertical = ~0u, found = 0;
    unsigned biasAt = ~0u, biasCount = 0;
    for (unsigned at = 2; at < words;) {
        unsigned count = instructionLength(code + at, words - at);
        if (!count) return false;
        const uint32_t* p = code + at;
        // The receiver depth bias: the only ADD of a small negative scalar.
        // The other immediates in this shader are +/-1, +/-0.5 or vectors.
        if ((p[0] & kOpcodeMask) == kOpcodeAdd && count == 7
            && p[5] == kImmediate32Scalar) {
            float value = 0.0f;
            memcpy(&value, &p[6], sizeof(value));
            if (value < 0.0f && value > -0.05f) { biasAt = at; ++biasCount; }
        }
        if ((p[0] & kOpcodeMask) == kOpcodeMad && count == 13
            && (p[3] & kOperandTypeMask) == kOperandConstantBuffer
            && p[6] == kImmediate32Vector) {
            bool offsets = true, any = false;
            for (unsigned c = 0; c < 4; ++c) {
                uint32_t bits = p[7 + c];
                if (!bits) continue;
                if (bits != 0x3f000000u && bits != 0xbf000000u) offsets = false;
                any = true;
            }
            if (offsets && any) {
                ++found;
                if (p[7] && !p[8] && p[9] && !p[10]) horizontal = at;
                else if (!p[7] && p[8] && !p[9] && p[10]) vertical = at;
            }
        }
        at += count;
    }
    // The same tap shape appears in the legacy per-material receivers and in
    // the point-light one, whose projection was not widened and must not move.
    if (found != 2 || horizontal == ~0u || vertical == ~0u || biasCount != 1
        || !isDeferredShadowReceiver(code, words))
        return false;

    BYTE* dst = (BYTE*)malloc(bytecodeSize);
    if (!dst) return false;
    memcpy(dst, src, bytecodeSize);
    uint32_t* patched = (uint32_t*)(dst + shaderChunk + 8);
    const float cross[2][4] = {{-0.5f, 0.0f, 0.5f, 0.0f}, {0.0f, -0.5f, 0.0f, 0.5f}};
    const float corner[2][4] = {{-0.5f, -0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f, 0.5f}};
    const unsigned target[2] = {horizontal, vertical};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned c = 0; c < 4; ++c) {
            float value = (corners ? corner[i][c] : cross[i][c]) * factor;
            uint32_t bits = 0;
            memcpy(&bits, &value, sizeof(bits));
            patched[target[i] + 7 + c] = bits;
        }
    // Scaling the bias with the fitted depth range keeps shadows attached to
    // their casters; a wider split otherwise pushes them off in world units.
    float bias = 0.0f;
    memcpy(&bias, &patched[biasAt + 6], sizeof(bias));
    bias *= biasScale;
    memcpy(&patched[biasAt + 6], &bias, sizeof(bias));
    memset(dst + 4, 0, 16);
    out->data = dst; out->size = bytecodeSize;
    return true;
}

void release(PatchResult* result) {
    if (!result) return;
    free(result->data);
    memset(result, 0, sizeof(*result));
}

}  // namespace dxbc
}  // namespace tq
