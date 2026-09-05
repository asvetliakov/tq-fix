// Counts how many programs of the extracted Titan Quest shader corpus a DXBC
// transform accepts. Every transform in src/dxbc_patch.cpp is aimed at exactly
// one program, so a count other than one means the shape test is too loose --
// the +/-0.5 tap shape alone matches 65 shaders, including the point-light
// receiver, which must never be touched.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxbc_patch.h"

namespace {

void* readFile(const char* path, long* size) {
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long bytes = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = bytes > 0 ? malloc((size_t)bytes) : nullptr;
    if (data && fread(data, 1, (size_t)bytes, file) != (size_t)bytes) {
        free(data);
        data = nullptr;
    }
    fclose(file);
    if (data) *size = bytes;
    return data;
}

bool accepts(const char* transform, const void* bytes, SIZE_T size) {
    tq::dxbc::PatchResult patch = {};
    bool matched = false;
    if (!strcmp(transform, "deferred"))
        matched = tq::dxbc::tuneDeferredShadowFilter(bytes, size, 0.381f, 0.695f,
                                                     true, &patch);
    else if (!strcmp(transform, "pcf"))
        matched = tq::dxbc::enhanceShadowPcf(bytes, size, &patch);
    else if (!strcmp(transform, "contact"))
        matched = tq::dxbc::addContactShadowMarch(bytes, size, 8, &patch);
    else if (!strcmp(transform, "receiver"))
        return tq::dxbc::matchesDeferredShadowReceiver(bytes, size);
    else if (!strcmp(transform, "bones"))
        matched = tq::dxbc::clampBoneIndices(bytes, size, &patch);
    tq::dxbc::release(&patch);
    return matched;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: corpus-match <transform> <corpus-directory>\n"
                        "transforms: deferred, contact, receiver, pcf, bones\n");
        return 2;
    }
    DIR* directory = opendir(argv[2]);
    if (!directory) {
        fprintf(stderr, "cannot read corpus directory: %s\n", argv[2]);
        return 2;
    }
    unsigned examined = 0, matched = 0;
    for (struct dirent* entry; (entry = readdir(directory));) {
        if (entry->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", argv[2], entry->d_name);
        long size = 0;
        void* bytes = readFile(path, &size);
        if (!bytes) continue;
        ++examined;
        if (accepts(argv[1], bytes, (SIZE_T)size)) {
            ++matched;
            printf("match %s\n", entry->d_name);
        }
        free(bytes);
    }
    closedir(directory);
    printf("%s: %u of %u corpus programs\n", argv[1], matched, examined);
    return 0;
}
