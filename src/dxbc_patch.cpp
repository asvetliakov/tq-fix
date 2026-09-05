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

// Everything tuneDeferredShadowFilter needs to locate before it decides
// whether a shader is the one deferred receiver: the shader chunk, the two tap
// MADs and the depth-bias ADD. Split out so the receiver can also be
// recognised without being modified -- the contact-shadow path has to know
// which created shader is the receiver even when the filter is left alone.
struct ReceiverShape {
    SIZE_T shaderChunk;
    unsigned words;
    unsigned horizontal;
    unsigned vertical;
    unsigned biasAt;
};

bool findDeferredShadowReceiver(const void* bytecode, SIZE_T bytecodeSize,
                                ReceiverShape* out) {
    if (!bytecode || bytecodeSize < 36) return false;
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
    if (out) {
        out->shaderChunk = shaderChunk;
        out->words = words;
        out->horizontal = horizontal;
        out->vertical = vertical;
        out->biasAt = biasAt;
    }
    return true;
}

// ------------------------------------------------- screen-space contact march
//
// Appended to the one deferred receiver, after its shadow term is final and
// before anything reads it. Purely additive: three instructions are inserted
// into the existing stream, the rest is a straight-line block at the end, and
// not one original instruction is rewritten.
//
// The march walks towards the light in clip space, which is affine, so a step
// is one add. Its origin is not computed: writing A for the inverse
// view-projection the shader holds and B for the forward one supplied in b13,
// the receiver's own reconstruction is h = A * (ndc, 1) and P = h.xyz / h.w, so
// B * (P, 1) is exactly (ndc, 1) / h.w. Scaling a homogeneous vector changes no
// UV, so the origin is the register that already holds (ndc.xy, depth, 1) and
// the step is scaled by h.w instead. That is exact at step zero, and it is why
// only two values have to be kept alive past the instructions that overwrite
// them.

const uint32_t kOpcodeAnd = 1u;
const uint32_t kOpcodeIf = 31u;
const uint32_t kOpcodeEndIf = 21u;
const uint32_t kOpcodeSampleL = 72u;
const uint32_t kTestNonzero = 0x40000u;
const uint32_t kOpcodeDiv2 = 14u;
const uint32_t kOpcodeLt = 49u;
const uint32_t kOpcodeMin = 51u;
const uint32_t kOpcodeMax = 52u;
const uint32_t kOpcodeMov = 54u;
const uint32_t kOpcodeGe = 29u;
const uint32_t kOpcodeMul = 56u;
const uint32_t kOpcodeSample = 69u;
const uint32_t kOpcodeDclTemps = 104u;
// Instruction-token saturate bit, as the shader's own mov_sat and mul_sat use.
const uint32_t kSaturate = 0x2000u;

// b13, and the two registers of it the march reads.
const unsigned kMarchBuffer = 13u;
const unsigned kMarchVectors = 7u;
const unsigned kMarchDirection = 4u;   // view-projection times the light, w = 0
// step length, depth bias, the reciprocal of the thickness limit, strength.
// The reciprocal because the thickness is a falloff, not a cut.
const unsigned kMarchParameters = 5u;
// (a, b, 0, 0) from the inverse view-projection's fourth row, so that
// 1 / (a * ndcZ + b) is the view depth. The depth comparison has to happen
// there: NDC z is so non-linear that one bias in NDC is 0.046 world units at
// ten units out and 0.80 at forty, which is wider than the whole march.
// (a, b, uprightThreshold, 1/steps). The threshold is compared against the
// G-buffer normal still encoded, so the shader needs no decode.
const unsigned kMarchLinearize = 6u;

// Write masks and swizzles, in the encoding the shader itself uses.
const unsigned kMaskX = 1u, kMaskY = 2u, kMaskZ = 4u, kMaskW = 8u;
const unsigned kMaskXy = 3u, kMaskXyz = 7u, kMaskXyzw = 15u;
const uint32_t kSwizzleXyzw = 0xe4u;
const uint32_t kSwizzleXyxx = 0x04u;
const uint32_t kSwizzleXxxx = 0x00u;
const uint32_t kSwizzleYyyy = 0x55u;
const uint32_t kSwizzleZzzz = 0xaau;
const uint32_t kSwizzleWwww = 0xffu;
// Resource swizzle placing the resource's X in the destination's W, which is
// the form both of the shader's own .w-destined samples use.
const uint32_t kResourceSwizzleYzwx = 0x39u;
// Operand modifier extension: type 1 is MODIFIER, value 2 is ABS.
const uint32_t kModifierAbs = (2u << 6) | 1u;

// One operand, already encoded. Three words is the largest form used here: a
// constant-buffer reference, or a temporary carrying a modifier extension.
struct Operand {
    uint32_t word[3];
    unsigned words;
};

Operand destination(unsigned reg, unsigned mask) {
    Operand o = {{0x00100002u | (mask << 4), reg, 0}, 2};
    return o;
}

Operand temporary(unsigned reg, uint32_t swizzle) {
    Operand o = {{0x00100006u | (swizzle << 4), reg, 0}, 2};
    return o;
}

// A single component broadcast to all four, in the shader's select-one form.
Operand component(unsigned reg, unsigned index) {
    Operand o = {{0x0010000au | (index << 4), reg, 0}, 2};
    return o;
}

Operand absComponent(unsigned reg, unsigned index) {
    Operand o = {{0x8010000au | (index << 4), kModifierAbs, reg}, 3};
    return o;
}

Operand constantVector(unsigned buffer, unsigned reg, uint32_t swizzle) {
    Operand o = {{0x00208006u | (swizzle << 4), buffer, reg}, 3};
    return o;
}

Operand scalar(uint32_t bits) {
    Operand o = {{kImmediate32Scalar, bits, 0}, 2};
    return o;
}

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Grows into a caller-owned buffer and records an overflow rather than writing
// past it, so a miscounted worst case fails the transform instead of the heap.
struct Emitter {
    uint32_t* words;
    unsigned count;
    unsigned capacity;
    bool overflow;
};

void push(Emitter* e, uint32_t word) {
    if (e->count < e->capacity) e->words[e->count++] = word;
    else e->overflow = true;
}

void pushOperand(Emitter* e, const Operand& operand) {
    for (unsigned i = 0; i < operand.words; ++i) push(e, operand.word[i]);
}

void emit(Emitter* e, uint32_t opcode, const Operand* operands, unsigned count) {
    unsigned length = 1;
    for (unsigned i = 0; i < count; ++i) length += operands[i].words;
    push(e, (length << kLengthShift) | opcode);
    for (unsigned i = 0; i < count; ++i) pushOperand(e, operands[i]);
}

void emit2(Emitter* e, uint32_t opcode, const Operand& a, const Operand& b) {
    const Operand operands[2] = {a, b};
    emit(e, opcode, operands, 2);
}

void emit3(Emitter* e, uint32_t opcode, const Operand& a, const Operand& b,
           const Operand& c) {
    const Operand operands[3] = {a, b, c};
    emit(e, opcode, operands, 3);
}

// mad with two immediate vectors, which no Operand form covers because the
// vector token carries four values.
void emitScaleBias(Emitter* e, const Operand& dest, const Operand& source,
                   const float scale[4], const float bias[4]) {
    push(e, (15u << kLengthShift) | kOpcodeMad);
    pushOperand(e, dest);
    pushOperand(e, source);
    push(e, kImmediate32Vector);
    for (unsigned i = 0; i < 4; ++i) push(e, floatBits(scale[i]));
    push(e, kImmediate32Vector);
    for (unsigned i = 0; i < 4; ++i) push(e, floatBits(bias[i]));
}

// Explicit LOD-zero samples keep the original resource/sampler bindings and
// resource-dimension/return-type tokens, without implicit derivatives in ifs.
void emitSample(Emitter* e, unsigned dest, unsigned mask,
                const Operand& coordinate, unsigned resource,
                uint32_t resourceSwizzle, unsigned sampler) {
    push(e, 0x80000000u | (13u << kLengthShift) | kOpcodeSampleL);
    push(e, 0x800000c2u);
    push(e, 0x00155543u);
    pushOperand(e, destination(dest, mask));
    pushOperand(e, coordinate);
    push(e, 0x00107006u | (resourceSwizzle << 4));
    push(e, resource);
    push(e, 0x00106000u);
    push(e, sampler);
    pushOperand(e, scalar(0)); // explicit LOD: valid in divergent control flow
}

// A source with the negate modifier, whose extension token the shader itself
// uses in `add r1.x, -r1.x, l(1.0)`.
Operand negatedComponent(unsigned reg, unsigned index) {
    Operand o = component(reg, index);
    o.word[2] = o.word[1];
    o.word[1] = (1u << 6) | 1u;  // MODIFIER extension, value 1 is NEG
    o.word[0] |= 0x80000000u;
    o.words = 3;
    return o;
}

struct ContactAnchors {
    uint32_t screenUv[2];    // the coordinate the shader samples its own depth at
    unsigned tempsAt;        // word holding the dcl_temps count
    unsigned temps;
    unsigned declarationAt;  // where the b13 declaration is inserted
    unsigned originAt;       // after the mov that completes (ndc.xy, depth, 1)
    unsigned origin;         // the register holding it
    unsigned homogeneousAt;  // after the fourth DP4, which leaves h.w in .x
    unsigned termAt;         // after the shadow term's final mad
    unsigned term;           // the register holding the shadow term
};

// Every anchor the march needs, each required to be unique. The inverse
// view-projection's DP4 group is told from the world-to-shadow group next to it
// by its fourth product writing .x back into the register all four read, which
// is what makes the projective divide reorder the result the way it does.
bool findContactAnchors(const uint32_t* code, unsigned wordCount,
                        ContactAnchors* out) {
    struct Instruction { unsigned at, count, opcode; };
    Instruction instructions[512];
    unsigned instructionCount = 0;
    for (unsigned at = 2; at < wordCount;) {
        unsigned count = instructionLength(code + at, wordCount - at);
        if (!count || instructionCount == 512) return false;
        instructions[instructionCount++] = {at, count, code[at] & kOpcodeMask};
        at += count;
    }

    ContactAnchors found = {};
    unsigned temps = 0, declarations = 0, groups = 0, terms = 0, depthSamples = 0;
    for (unsigned i = 0; i < instructionCount; ++i) {
        const Instruction& instruction = instructions[i];
        const uint32_t* p = code + instruction.at;
        if (instruction.opcode == kOpcodeDclTemps && instruction.count == 2) {
            found.tempsAt = instruction.at + 1;
            found.temps = p[1];
            ++temps;
        }
        if (instruction.opcode == kOpcodeDclConstantBuffer && instruction.count == 4) {
            // A second pass over an already-marched shader would add a second
            // b13; refuse instead.
            if (p[2] == kMarchBuffer) return false;
            found.declarationAt = instruction.at + instruction.count;
            ++declarations;
        }

        // The shader's own depth sample. Its coordinate is the screen UV, which
        // the march reuses to read the G-buffer normal, so it is taken from
        // the shader rather than assumed to be a particular input register.
        if (instruction.opcode == kOpcodeSample && instruction.count == 11
            && p[8] == 2 && p[10] == 2) {
            found.screenUv[0] = p[5];
            found.screenUv[1] = p[6];
            ++depthSamples;
        }

        // The shadow term: mad rT.w, rS.x, rT.w, l(1.0). The other nine-word
        // mad in this shader carries its immediate in a different position and
        // writes .x, so the shape is specific without being fragile.
        if (instruction.opcode == kOpcodeMad && instruction.count == 9
            && (p[1] & 0xffu) == 0x82u && p[3] == 0x0010000au
            && p[5] == 0x0010003au && p[6] == p[2]
            && p[7] == kImmediate32Scalar && p[8] == 0x3f800000u) {
            found.termAt = instruction.at + instruction.count;
            found.term = p[2];
            ++terms;
        }

        if (i + 3 >= instructionCount || i == 0) continue;
        bool four = true;
        for (unsigned n = 0; n < 4; ++n)
            four = four && instructions[i + n].opcode == kOpcodeDp4
                        && instructions[i + n].count == 8;
        if (!four) continue;
        const unsigned source = p[4];
        const unsigned base = p[7];
        // The CPU readback uses the audited cb0[8..11] layout. A matching
        // instruction shape with another matrix offset cannot use that data.
        if (base != 8) continue;
        const uint32_t masks[4] = {0x12u, 0x22u, 0x42u, 0x12u};
        bool shaped = true;
        for (unsigned n = 0; n < 4 && shaped; ++n) {
            const uint32_t* q = code + instructions[i + n].at;
            shaped = (q[1] & 0xffu) == masks[n]
                  && q[3] == kTempSourceXyzw && q[4] == source
                  && (q[5] & kOperandTypeMask) == kOperandConstantBuffer
                  && q[6] == 0 && q[7] == base + n;
        }
        // The fourth product writes .x of the register all four read; the
        // world-to-shadow group beside it writes .w of that register instead.
        if (!shaped || code[instructions[i + 3].at + 2] != source) continue;

        // Immediately before the group, the mov that puts 1.0 in .w and so
        // completes (ndc.xy, depth, 1).
        const Instruction& previous = instructions[i - 1];
        const uint32_t* m = code + previous.at;
        if (previous.opcode != kOpcodeMov || previous.count != 5
            || (m[1] & 0xffu) != 0x82u || m[2] != source
            || m[3] != kImmediate32Scalar || m[4] != 0x3f800000u)
            continue;
        found.originAt = previous.at + previous.count;
        found.origin = source;
        found.homogeneousAt = instructions[i + 3].at + instructions[i + 3].count;
        ++groups;
    }
    if (temps != 1 || !declarations || groups != 1 || terms != 1
        || depthSamples != 1)
        return false;
    if (found.temps < 1 || found.temps > 24) return false;
    if (found.originAt >= found.homogeneousAt || found.homogeneousAt >= found.termAt)
        return false;
    *out = found;
    return true;
}

// Emits the march, given the registers it may use. Returns the word count, or
// zero on overflow.
unsigned buildMarch(uint32_t* words, unsigned capacity, unsigned steps,
                    unsigned base, unsigned term, const Operand& screenUv) {
    const unsigned origin = base, step = base + 1, clip = base + 2,
                   scratch = base + 3, occlusion = base + 4;
    Emitter e = {words, 0, capacity, false};

    // Zero strength (startup, stale history, comparison toggle) skips all added
    // texture fetches. Also reject invalid homogeneous depth and fully shadowed
    // pixels, where min(native, contact) cannot change the output.
    emit3(&e, kOpcodeLt, destination(occlusion, kMaskY), scalar(0),
          constantVector(kMarchBuffer, kMarchParameters, kSwizzleWwww));
    emit3(&e, kOpcodeLt, destination(occlusion, kMaskZ), scalar(floatBits(1.0e-8f)),
          component(step, 0));
    emit3(&e, kOpcodeAnd, destination(occlusion, kMaskY), component(occlusion, 1),
          component(occlusion, 2));
    emit3(&e, kOpcodeLt, destination(occlusion, kMaskZ), scalar(0), component(term, 3));
    emit3(&e, kOpcodeAnd, destination(occlusion, kMaskY), component(occlusion, 1),
          component(occlusion, 2));
    Operand condition = component(occlusion, 1);
    emit(&e, kOpcodeIf | kTestNonzero, &condition, 1);
    // Gate receivers before paying for the ray, using the encoded world Y.
    emitSample(&e, occlusion, kMaskY, screenUv, 0, kSwizzleXyzw, 0);
    emit3(&e, kOpcodeGe, destination(occlusion, kMaskY), component(occlusion, 1),
          constantVector(kMarchBuffer, kMarchLinearize, kSwizzleZzzz));
    emit(&e, kOpcodeIf | kTestNonzero, &condition, 1);

    // The origin is the pixel's own NDC with an implicit w of one, so the whole
    // march is scaled by h.w relative to true clip space. That scale is undone
    // once, here, and reused every step to recover a view depth.
    emit2(&e, kOpcodeMov, destination(clip, kMaskXyzw),
          temporary(origin, kSwizzleXyzw));
    emit3(&e, kOpcodeMul, destination(occlusion, kMaskX), component(step, 0),
          constantVector(kMarchBuffer, kMarchParameters, kSwizzleXxxx));
    // step.x still holds h.w until the line below overwrites it, and the origin
    // register is free the moment it has been copied.
    emit3(&e, kOpcodeDiv2, destination(origin, kMaskX), scalar(floatBits(1.0f)),
          component(step, 0));
    emit3(&e, kOpcodeMul, destination(step, kMaskXyzw), component(occlusion, 0),
          constantVector(kMarchBuffer, kMarchDirection, kSwizzleXyzw));
    emit2(&e, kOpcodeMov, destination(occlusion, kMaskX), scalar(0));

    const float toUv[4] = {0.5f, -0.5f, 0.0f, 0.0f};
    const float half[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    for (unsigned i = 0; i < steps; ++i) {
        // Clip space is affine along the light, so a step is one add.
        emit3(&e, kOpcodeAdd, destination(clip, kMaskXyzw),
              temporary(clip, kSwizzleXyzw), temporary(step, kSwizzleXyzw));
        emit3(&e, kOpcodeMax, destination(scratch, kMaskW), component(clip, 3),
              scalar(floatBits(1.0e-8f)));
        emit3(&e, kOpcodeDiv2, destination(scratch, kMaskXyz),
              temporary(clip, kSwizzleXyzw), component(scratch, 3));
        // Off-screen samples must not occlude, or geometry at the screen edge
        // streaks along the light. In NDC that is one max of two magnitudes.
        emit3(&e, kOpcodeMax, destination(occlusion, kMaskZ),
              absComponent(scratch, 0), absComponent(scratch, 1));
        emit3(&e, kOpcodeLt, destination(occlusion, kMaskZ),
              component(occlusion, 2), scalar(floatBits(1.0f)));
        emit3(&e, kOpcodeGe, destination(occlusion, kMaskW), component(scratch, 2), scalar(0));
        emit3(&e, kOpcodeAnd, destination(occlusion, kMaskZ), component(occlusion, 2), component(occlusion, 3));
        emit3(&e, kOpcodeLt, destination(occlusion, kMaskW), component(scratch, 2), scalar(floatBits(1.0f)));
        emit3(&e, kOpcodeAnd, destination(occlusion, kMaskZ), component(occlusion, 2), component(occlusion, 3));
        emit3(&e, kOpcodeLt, destination(occlusion, kMaskW), scalar(0), component(clip, 3));
        emit3(&e, kOpcodeAnd, destination(occlusion, kMaskZ), component(occlusion, 2), component(occlusion, 3));
        // Invalid ray points cannot sample wrapped/clamped edge geometry.
        Operand inFrustum = component(occlusion, 2);
        emit(&e, kOpcodeIf | kTestNonzero, &inFrustum, 1);
        emitScaleBias(&e, destination(scratch, kMaskXy),
                      temporary(scratch, kSwizzleXyxx), toUv, half);
        emitSample(&e, scratch, kMaskW, temporary(scratch, kSwizzleXyxx), 2,
                   kResourceSwizzleYzwx, 2);

        // Both depths in world units. The buffer's has to be linearised; the
        // marched point's is its own clip w, undone by the scale above.
        const Operand linearize[4] = {
            destination(occlusion, kMaskY), component(scratch, 3),
            constantVector(kMarchBuffer, kMarchLinearize, kSwizzleXxxx),
            constantVector(kMarchBuffer, kMarchLinearize, kSwizzleYyyy)};
        emit(&e, kOpcodeMad, linearize, 4);
        emit3(&e, kOpcodeDiv2, destination(occlusion, kMaskY),
              scalar(floatBits(1.0f)), component(occlusion, 1));
        emit3(&e, kOpcodeMul, destination(occlusion, kMaskW), component(clip, 3),
              component(origin, 0));
        emit3(&e, kOpcodeAdd, destination(occlusion, kMaskY),
              component(occlusion, 3), negatedComponent(occlusion, 1));

        // The buffer has to be nearer than the marched point by more than the
        // bias for the step to count at all. Masking one bitwise against 1.0
        // turns the comparison's 0/~0 into 0.0/1.0 without a branch.
        emit3(&e, kOpcodeLt, destination(occlusion, kMaskW),
              constantVector(kMarchBuffer, kMarchParameters, kSwizzleYyyy),
              component(occlusion, 1));
        emit3(&e, kOpcodeAnd, destination(occlusion, kMaskZ),
              component(occlusion, 2), component(occlusion, 3));
        emit3(&e, kOpcodeAnd, destination(occlusion, kMaskZ),
              component(occlusion, 2), scalar(floatBits(1.0f)));

        // How much it counts falls off with the depth gap rather than being
        // cut at the thickness limit. A hard cut makes an occluder just inside
        // the limit count fully and one just outside count nothing, which on
        // thin noisy geometry -- grass above all -- reads as speckle. The
        // game's own HBAO+ integrates a smooth fraction for the same reason.
        emit3(&e, kOpcodeMul | kSaturate, destination(occlusion, kMaskW),
              component(occlusion, 1),
              constantVector(kMarchBuffer, kMarchParameters, kSwizzleZzzz));
        emit3(&e, kOpcodeAdd, destination(occlusion, kMaskW),
              negatedComponent(occlusion, 3), scalar(floatBits(1.0f)));
        const Operand accumulate[4] = {destination(occlusion, kMaskX),
                                       component(occlusion, 2),
                                       component(occlusion, 3),
                                       component(occlusion, 0)};
        emit(&e, kOpcodeMad, accumulate, 4);
        push(&e, (1u << kLengthShift) | kOpcodeEndIf);
    }

    // Average the occluded steps, scale by strength, and combine. The result
    // lies in [1 - strength, 1], so it can only ever darken the native term.
    emit3(&e, kOpcodeMul, destination(occlusion, kMaskX), component(occlusion, 0),
          constantVector(kMarchBuffer, kMarchLinearize, kSwizzleWwww));
    emit3(&e, kOpcodeMul, destination(occlusion, kMaskX), component(occlusion, 0),
          constantVector(kMarchBuffer, kMarchParameters, kSwizzleWwww));
    emit3(&e, kOpcodeAdd, destination(occlusion, kMaskX),
          negatedComponent(occlusion, 0), scalar(floatBits(1.0f)));
    emit3(&e, kOpcodeMin, destination(term, kMaskW), component(term, 3),
          component(occlusion, 0));
    push(&e, (1u << kLengthShift) | kOpcodeEndIf);
    push(&e, (1u << kLengthShift) | kOpcodeEndIf);
    return e.overflow ? 0 : e.count;
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
    if (!out || !(factor > 0.0f) || factor > 1.0f
        || !(biasScale > 0.0f) || biasScale > 1.0f)
        return false;
    ReceiverShape shape = {};
    if (!findDeferredShadowReceiver(bytecode, bytecodeSize, &shape)) return false;

    const BYTE* src = (const BYTE*)bytecode;
    BYTE* dst = (BYTE*)malloc(bytecodeSize);
    if (!dst) return false;
    memcpy(dst, src, bytecodeSize);
    uint32_t* patched = (uint32_t*)(dst + shape.shaderChunk + 8);
    const float cross[2][4] = {{-0.5f, 0.0f, 0.5f, 0.0f}, {0.0f, -0.5f, 0.0f, 0.5f}};
    const float corner[2][4] = {{-0.5f, -0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f, 0.5f}};
    const unsigned target[2] = {shape.horizontal, shape.vertical};
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
    memcpy(&bias, &patched[shape.biasAt + 6], sizeof(bias));
    bias *= biasScale;
    memcpy(&patched[shape.biasAt + 6], &bias, sizeof(bias));
    memset(dst + 4, 0, 16);
    out->data = dst; out->size = bytecodeSize;
    return true;
}

bool matchesDeferredShadowReceiver(const void* bytecode, SIZE_T bytecodeSize) {
    return findDeferredShadowReceiver(bytecode, bytecodeSize, nullptr);
}

bool addContactShadowMarch(const void* bytecode, SIZE_T bytecodeSize,
                           unsigned steps, PatchResult* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!out || steps < 4 || steps > 16) return false;
    ReceiverShape shape = {};
    if (!findDeferredShadowReceiver(bytecode, bytecodeSize, &shape)) return false;

    const BYTE* src = (const BYTE*)bytecode;
    const uint32_t* code = (const uint32_t*)(src + shape.shaderChunk + 8);
    ContactAnchors anchors = {};
    if (!findContactAnchors(code, shape.words, &anchors)) return false;
    // Five temporaries above whatever the shader already uses: the origin, the
    // step, the running clip position, the per-step scratch, and the
    // occlusion accumulator.
    const unsigned base = anchors.temps;
    if (base + 5 > 32) return false;

    const uint32_t declaration[4] = {
        (4u << kLengthShift) | kOpcodeDclConstantBuffer,
        0x00208e46u, kMarchBuffer, kMarchVectors};
    const uint32_t saveOrigin[5] = {
        (5u << kLengthShift) | kOpcodeMov,
        0x00100002u | (kMaskXyzw << 4), base,
        0x00100006u | (kSwizzleXyzw << 4), anchors.origin};
    const uint32_t saveHomogeneous[5] = {
        (5u << kLengthShift) | kOpcodeMov,
        0x00100002u | (kMaskX << 4), base + 1,
        0x0010000au, anchors.origin};

    // Worst-case guarded sixteen-step march, setup and combine.
    const unsigned kMarchCapacity = 8192;
    uint32_t* march = (uint32_t*)malloc(kMarchCapacity * sizeof(uint32_t));
    if (!march) return false;
    Operand screenUv = {{anchors.screenUv[0], anchors.screenUv[1], 0}, 2};
    unsigned marchWords = buildMarch(march, kMarchCapacity, steps, base,
                                     anchors.term, screenUv);
    if (!marchWords) { free(march); return false; }

    struct Insertion { unsigned at; const uint32_t* words; unsigned count; };
    Insertion insertions[4] = {
        {anchors.declarationAt, declaration, 4},
        {anchors.originAt, saveOrigin, 5},
        {anchors.homogeneousAt, saveHomogeneous, 5},
        {anchors.termAt, march, marchWords}};
    for (unsigned i = 1; i < 4; ++i)
        if (insertions[i].at < insertions[i - 1].at) { free(march); return false; }

    unsigned added = 0;
    for (unsigned i = 0; i < 4; ++i) added += insertions[i].count;
    const SIZE_T addedBytes = (SIZE_T)added * 4u;
    if (bytecodeSize > (SIZE_T)-1 - addedBytes) { free(march); return false; }
    const SIZE_T newSize = bytecodeSize + addedBytes;
    BYTE* dst = (BYTE*)malloc(newSize);
    if (!dst) { free(march); return false; }

    // Everything up to the shader chunk's instruction stream is unchanged.
    const SIZE_T streamAt = shape.shaderChunk + 8;
    memcpy(dst, src, streamAt);
    uint32_t* output = (uint32_t*)(dst + streamAt);
    unsigned written = 0, read = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned upTo = insertions[i].at;
        memcpy(output + written, code + read, (upTo - read) * sizeof(uint32_t));
        written += upTo - read;
        read = upTo;
        memcpy(output + written, insertions[i].words,
               insertions[i].count * sizeof(uint32_t));
        written += insertions[i].count;
    }
    memcpy(output + written, code + read, (shape.words - read) * sizeof(uint32_t));
    written += shape.words - read;
    free(march);
    if (written != shape.words + added) { free(dst); return false; }

    // Whatever follows the shader chunk -- the statistics chunk, and anything
    // the chunk table lists after it -- moves along by the inserted bytes.
    const SIZE_T tailAt = streamAt + (SIZE_T)shape.words * 4u;
    if (tailAt > bytecodeSize) { free(dst); return false; }
    memcpy(dst + tailAt + addedBytes, src + tailAt, bytecodeSize - tailAt);

    // The declaration count moves with whatever was inserted ahead of it.
    unsigned shift = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (insertions[i].at <= anchors.tempsAt) shift += insertions[i].count;
    output[anchors.tempsAt + shift] = base + 5;

    // Header size, the chunk table beyond the shader chunk, the chunk's own
    // length, and the instruction count the stream states for itself.
    write32(dst + 24, (uint32_t)newSize);
    const uint32_t chunks = read32(src + 28);
    for (unsigned i = 0; i < chunks; ++i) {
        const uint32_t offset = read32(src + 32 + i * 4u);
        write32(dst + 32 + i * 4u,
                offset > shape.shaderChunk ? offset + (uint32_t)addedBytes : offset);
    }
    write32(dst + shape.shaderChunk + 4,
            read32(src + shape.shaderChunk + 4) + (uint32_t)addedBytes);
    write32(dst + shape.shaderChunk + 12, shape.words + added);

    // DXMT verifies container structure but not this legacy digest. Clearing it
    // is honest; retaining the source hash would not be.
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
