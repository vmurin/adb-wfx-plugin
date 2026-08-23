// Proves fsplugin.wfx64 is a real, loadable WFX plugin exporting every
// symbol Double Commander needs -- dlopen()ing the actual built artifact
// and dlsym()ing each export, not merely calling the functions as C++
// from this translation unit.
//
// Skips (prints SKIP, does not fail) when fsplugin.wfx64 has not been
// built yet, so this test compiles and passes on a clean checkout even
// before compile_mac.sh has run. run_tests.sh always builds the plugin
// before running the suite (see run_tests.sh), so in the normal path this
// test never actually takes the skip branch -- it only matters for
// someone running tests/bin/run_tests directly without that build step.
#include "sdk.h"
#include "testing.hpp"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>

namespace {

constexpr const char* PLUGIN_PATH = "./fsplugin.wfx64";

// Every export the brief's table requires. Deliberately only the W
// variants (and FsGetDefRootName, which the SDK has no W variant of) --
// the ANSI variants are not implemented per the brief.
constexpr const char* REQUIRED_EXPORTS[] = {
    "FsInitW",     "FsFindFirstW",  "FsFindNextW",   "FsFindClose",
    "FsGetFileW",  "FsPutFileW",    "FsRenMovFileW", "FsDeleteFileW",
    "FsRemoveDirW", "FsMkDirW",     "FsSetTimeW",    "FsStatusInfoW",
    "FsGetDefRootName", "FsDisconnectW",
};

bool pluginIsBuilt() {
    struct stat st;
    return ::stat(PLUGIN_PATH, &st) == 0;
}

} // namespace

TEST(ExportsSuite, everyRequiredSymbolIsPresent) {
    if (!pluginIsBuilt()) {
        std::printf("SKIP ExportsSuite.everyRequiredSymbolIsPresent: %s not built yet\n",
                    PLUGIN_PATH);
        return;
    }

    void* handle = dlopen(PLUGIN_PATH, RTLD_NOW);
    CHECK(handle != nullptr);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", PLUGIN_PATH, dlerror());
        return;
    }

    for (const char* name : REQUIRED_EXPORTS) {
        void* sym = dlsym(handle, name);
        CHECK(sym != nullptr);
    }

    dlclose(handle);
}

// FsSetTimeW called out by name, not only swept up in the loop above:
// Double Commander gates copying file dates in *both* directions on
// Assigned(FsSetTime) or Assigned(FsSetTimeW), and silently drops the
// user's "copy file dates" option when neither is exported. This is the
// one export in the table that must never quietly disappear from
// REQUIRED_EXPORTS above without this test still catching it.
TEST(ExportsSuite, fsSetTimeWIsExported) {
    if (!pluginIsBuilt()) {
        std::printf("SKIP ExportsSuite.fsSetTimeWIsExported: %s not built yet\n", PLUGIN_PATH);
        return;
    }

    void* handle = dlopen(PLUGIN_PATH, RTLD_NOW);
    CHECK(handle != nullptr);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", PLUGIN_PATH, dlerror());
        return;
    }

    CHECK(dlsym(handle, "FsSetTimeW") != nullptr);

    dlclose(handle);
}

TEST(ExportsSuite, fsGetDefRootNameYieldsADB) {
    if (!pluginIsBuilt()) {
        std::printf("SKIP ExportsSuite.fsGetDefRootNameYieldsADB: %s not built yet\n",
                    PLUGIN_PATH);
        return;
    }

    void* handle = dlopen(PLUGIN_PATH, RTLD_NOW);
    CHECK(handle != nullptr);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", PLUGIN_PATH, dlerror());
        return;
    }

    void* sym = dlsym(handle, "FsGetDefRootName");
    CHECK(sym != nullptr);
    if (sym != nullptr) {
        // DCPCALL, not a bare (*): it is empty on macOS and Linux but expands
        // to __stdcall on Windows, and calling a stdcall export through a
        // cdecl pointer corrupts the stack on x86. Wrong here today would be
        // invisible until the first Windows build.
        using FsGetDefRootNameFn = void(DCPCALL*)(char*, int);
        auto fn = reinterpret_cast<FsGetDefRootNameFn>(sym);
        char buf[MAX_PATH];
        std::memset(buf, 'Z', sizeof(buf));
        fn(buf, static_cast<int>(sizeof(buf)));
        CHECK_STR_EQ(std::string(buf), std::string("ADB"));
    }

    dlclose(handle);
}
