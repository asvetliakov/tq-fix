// Prints the SHEX/SHDR instruction stream of a DXBC container as word offsets
// and raw tokens. Hand-emitting instructions needs exact operand encodings, and
// the surest source of those is the shader being extended: every operand form
// the transform emits should already appear somewhere in this dump.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const unsigned kDxbc = 0x43425844u, kShex = 0x58454853u, kShdr = 0x52444853u;

unsigned read32(const unsigned char* p) {
    unsigned v; memcpy(&v, p, 4); return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: dxbc-dump <file> [first] [last]\n"); return 2; }
    FILE* file = fopen(argv[1], "rb");
    if (!file) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(file, 0, SEEK_END); long size = ftell(file); fseek(file, 0, SEEK_SET);
    unsigned char* bytes = (unsigned char*)malloc((size_t)size);
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) return 2;
    fclose(file);
    if (read32(bytes) != kDxbc) { fprintf(stderr, "not a DXBC container\n"); return 2; }

    unsigned chunks = read32(bytes + 28);
    long shader = 0; unsigned shaderBytes = 0;
    for (unsigned i = 0; i < chunks; ++i) {
        long at = read32(bytes + 32 + i * 4);
        unsigned fourcc = read32(bytes + at);
        if (fourcc == kShex || fourcc == kShdr) { shader = at; shaderBytes = read32(bytes + at + 4); }
    }
    if (!shader) { fprintf(stderr, "no shader chunk\n"); return 2; }
    const unsigned* code = (const unsigned*)(bytes + shader + 8);
    unsigned words = shaderBytes / 4;
    unsigned first = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    unsigned last  = argc > 3 ? (unsigned)atoi(argv[3]) : ~0u;

    printf("shader chunk at %ld, %u bytes, %u words, version 0x%08x\n",
           shader, shaderBytes, words, code[0]);
    unsigned index = 0;
    for (unsigned at = 2; at < words;) {
        unsigned opcode = code[at] & 0x7ffu;
        unsigned length = opcode == 53u ? code[at + 1] : (code[at] >> 24) & 0x7fu;
        if (!length || at + length > words) { printf("  malformed at word %u\n", at); break; }
        if (index >= first && index <= last) {
            printf("%4u  w%-5u op%-4u len%-3u ", index, at, opcode, length);
            for (unsigned n = 0; n < length; ++n) printf("%08x ", code[at + n]);
            printf("\n");
        }
        at += length; ++index;
    }
    free(bytes);
    return 0;
}
