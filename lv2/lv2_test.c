/*
 * lv2_test.c - minimal LV2 plugin load/instantiate test
 *
 * Usage:  ./lv2_test <path/to/lingot_lv2.so> [sample_rate]
 *
 * Loads the .so, calls lv2_descriptor(0), then instantiate().
 * Any crash or error from the plugin's own code will print to stderr.
 */

#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <plugin.so> [sample_rate]\n", argv[0]);
        return 1;
    }
    const char *so_path   = argv[1];
    double      rate      = (argc >= 3) ? atof(argv[2]) : 48000.0;

    /* --- 1. dlopen ---------------------------------------------------- */
    fprintf(stderr, "dlopen: %s\n", so_path);
    void *lib = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "FAILED: %s\n", dlerror());
        return 1;
    }
    fprintf(stderr, "dlopen OK\n");

    /* --- 2. resolve lv2_descriptor ------------------------------------ */
    typedef const LV2_Descriptor *(*lv2_desc_fn)(uint32_t);
    lv2_desc_fn lv2_descriptor_fn =
        (lv2_desc_fn)(intptr_t)dlsym(lib, "lv2_descriptor");
    if (!lv2_descriptor_fn) {
        fprintf(stderr, "FAILED: lv2_descriptor symbol not found: %s\n", dlerror());
        dlclose(lib);
        return 1;
    }

    /* --- 3. get descriptor for index 0 -------------------------------- */
    const LV2_Descriptor *desc = lv2_descriptor_fn(0);
    if (!desc) {
        fprintf(stderr, "FAILED: lv2_descriptor(0) returned NULL\n");
        dlclose(lib);
        return 1;
    }
    fprintf(stderr, "lv2_descriptor(0) URI: %s\n", desc->URI);

    /* --- 4. instantiate ----------------------------------------------- */
    fprintf(stderr, "instantiate at %.0f Hz ...\n", rate);
    LV2_Handle handle = desc->instantiate(desc, rate, "/tmp", NULL);
    if (!handle) {
        fprintf(stderr, "FAILED: instantiate() returned NULL\n");
        dlclose(lib);
        return 1;
    }
    fprintf(stderr, "instantiate OK\n");

    /* --- 5. activate / deactivate / cleanup --------------------------- */
    if (desc->activate)   desc->activate(handle);
    if (desc->deactivate) desc->deactivate(handle);
    desc->cleanup(handle);
    fprintf(stderr, "activate/deactivate/cleanup OK\n");

    dlclose(lib);
    fprintf(stderr, "PASS\n");
    return 0;
}
