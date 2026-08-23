// tests/device_driver.cpp -- a tiny standalone CLI that drives PluginCore
// directly, the same way fsplugin.cpp's exports do, so tests/device_test.sh
// and tests/bench.sh exercise the plugin's OWN code paths against a real
// device instead of shelling out to `adb` for the operations under test.
// (They still use plain `adb` for independent verification -- `stat`,
// `shasum` -- exactly as a human reviewer would, and as the baseline half
// of bench.sh's comparison.)
//
// This file has its own main() and is intentionally NOT named test_*.cpp:
// run_tests.sh's unit-test build compiles `tests/main.cpp tests/test_*.cpp`
// and must never pick this up. It is compiled separately, and only when
// ADB_WFX_DEVICE_TESTS=1 (see run_tests.sh -- this binary is the one place
// besides fsplugin.cpp itself that is allowed to build a real TcpTransport
// and talk to a real adb server).
//
// Every WFX-style path argument below ("/<serial>/<on-device-path>") is
// exactly what PluginCore consumes -- see adbutils.hpp's parseRemotePath
// and fsplugin_impl.hpp's public surface. Nothing here
// re-implements any plugin logic; it only converts argv into calls on
// PluginCore and prints the result.
#include "../adbclient.hpp"
#include "../adbserver.hpp"
#include "../fsplugin_impl.hpp"
#include "../transport.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

// Loopback host AdbClient's TransportFactory connects to -- the same
// constant fsplugin.cpp uses. The port itself is resolved via
// adbServerPort() (ANDROID_ADB_SERVER_PORT, falling back to
// ADB_DEFAULT_PORT).
constexpr const char* ADB_SERVER_HOST = "127.0.0.1";

// device_driver always requests FS_COPYFLAGS_OVERWRITE on get/put so that
// device_test.sh and bench.sh can be re-run without first hand-deleting
// leftovers from a previous run; nothing here decides overwrite policy on
// behalf of a real Double Commander session.
constexpr int DRIVER_COPY_FLAGS = FS_COPYFLAGS_OVERWRITE;

// Exit codes. 0/1 are the usual success/failure; ABORTED and USAGE are
// their own distinct codes so a calling script can tell "the transfer was
// cancelled, as expected by the test" apart from "the transfer failed",
// and "you called this wrong" apart from either.
constexpr int EXIT_OK = 0;
constexpr int EXIT_ERROR = 1;
constexpr int EXIT_ABORTED = 2;
constexpr int EXIT_USAGE = 3;

void printUsage() {
    std::cerr <<
        "usage: device_driver <command> [args...]\n"
        "  list <wfxpath>\n"
        "  get <wfxremote> <local> [cancelAfterBytes]\n"
        "  put <local> <wfxremote> [cancelAfterBytes]\n"
        "  putmany <localdir> <wfxdir>\n"
        "  getmany <wfxdir> <localdir>\n"
        "  mkdir <wfxpath>\n"
        "  rm <wfxpath>\n"
        "  rmdir <wfxpath>\n"
        "  mv <wfxfrom> <wfxto> [overwrite(0|1)]\n"
        "  settime <wfxremote> <epochSeconds>\n"
        "  settime-t <wfxremote> <epochSeconds>\n"
        "  cachettl <wfxdir> <sleep1Seconds> <sleep2Seconds>\n"
        "\n"
        "wfxpath/wfxremote/wfxfrom/wfxto are WFX-style paths:\n"
        "  \"/<serial>/<on-device-absolute-path>\", e.g.\n"
        "  \"/27281FDH2008DM/sdcard/adb_wfx_test/file.bin\".\n"
        "\n"
        "cancelAfterBytes, if given and > 0, makes get/put cancel the\n"
        "transfer (as if the user clicked Cancel) once that many bytes\n"
        "have moved -- used by device_test.sh's cancellation check.\n"
        "\n"
        "putmany/getmany move every regular file of a directory in ONE\n"
        "process, over one PluginCore -- the shape `adb push`/`adb pull`\n"
        "already have, and the only fair comparison for tests/bench.sh.\n"
        "\n"
        "settime-t forces the `touch -t` fallback that settime only\n"
        "reaches when the device's touch rejects `-d @<epoch>`.\n"
        "\n"
        "cachettl lists <wfxdir> three times through ONE PluginCore,\n"
        "sleeping between them, and prints a count each time -- so a\n"
        "caller can change the directory from outside and see the cache\n"
        "hold the old listing and then expire.\n";
}

// Builds the same AdbClient wiring FsInitW uses in fsplugin.cpp: find and
// start a local adb server, then connect over TCP to it per request.
//
// The server is only started when a plain connect to the port fails.
// startAdbServer is a posix_spawn + waitpid of the adb binary, and this
// binary is invoked once per file by the older shape of tests/bench.sh --
// paying that process launch a hundred times per run made the plugin arm
// look 30-70% slower than it is. Probing first costs one loopback connect
// against an already-running server, and still starts one when there
// genuinely isn't one.
std::unique_ptr<AdbClient> makeClient() {
    auto getEnv = [](const char* name) -> const char* { return std::getenv(name); };
    auto isExecutable = [](const std::string& path) -> bool {
        return ::access(path.c_str(), X_OK) == 0;
    };

    int port = adbServerPort(getEnv);
    std::string probeError;
    std::unique_ptr<TcpTransport> probe = TcpTransport::connectTo(ADB_SERVER_HOST, port, &probeError);
    if (probe) {
        probe->close();
    } else {
        std::string adbBinary = findAdbBinary(getEnv, isExecutable);
        if (!adbBinary.empty()) {
            startAdbServer(adbBinary);
        }
    }

    TransportFactory factory = [port](std::string* error) -> std::unique_ptr<Transport> {
        return TcpTransport::connectTo(ADB_SERVER_HOST, port, error);
    };
    return std::make_unique<AdbClient>(std::move(factory));
}

// A ProgressFn that cancels once `cancelAfterBytes` bytes have moved
// (0 means "never cancel"). Used for both get and put.
ProgressFn makeCancellingProgress(uint64_t cancelAfterBytes) {
    return [cancelAfterBytes](uint64_t done, uint64_t /*total*/) -> bool {
        if (cancelAfterBytes > 0 && done >= cancelAfterBytes) {
            return false; // false == "stop", matching ProgressFn's contract
        }
        return true;
    };
}

// Parses a trailing optional numeric argument (cancelAfterBytes, epoch,
// overwrite flag, ...), returning `defaultValue` when absent. Exits with
// EXIT_USAGE on anything that doesn't parse as a plain decimal integer.
int64_t parseOptionalInt(int argc, char** argv, int index, int64_t defaultValue) {
    if (index >= argc) {
        return defaultValue;
    }
    const char* raw = argv[index];
    char* end = nullptr;
    long long value = std::strtoll(raw, &end, 10);
    if (end == raw || *end != '\0') {
        std::cerr << "device_driver: not an integer: " << raw << "\n";
        std::exit(EXIT_USAGE);
    }
    return static_cast<int64_t>(value);
}

// Second, independent guard against rm/rmdir being pointed at a device's
// entire filesystem root -- belt and braces alongside whatever guard the
// calling script applies to the string it constructs. parseRemotePath
// (adbutils.hpp, transitively included via fsplugin_impl.hpp) normalizes
// "/<serial>" (no further path component) to path "/", and the WFX root
// "/" itself parses as isRoot with path "/" too -- both are refused here
// regardless of what any caller's own path-construction logic intended.
bool refusesDeviceRoot(const std::string& wfxPath, std::string* reason) {
    RemotePath rp = parseRemotePath(wfxPath);
    if (rp.isRoot || rp.path.empty() || rp.path == "/") {
        *reason = "refusing to rm/rmdir a device's filesystem root: '" + wfxPath + "'";
        return true;
    }
    return false;
}

int cmdList(AdbClient& client, const std::string& wfxPath) {
    PluginCore core(client);
    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    if (!core.listDirectory(wfxPath, &entries, &warning, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    if (!warning.empty()) {
        std::cerr << "WARN: " << warning << "\n";
    }
    for (const FindResult& e : entries) {
        std::cout << e.name << "\t" << e.size << "\t" << e.mtime << "\t" << e.unixMode << "\t"
                  << (e.isDir ? "1" : "0") << "\n";
    }
    return EXIT_OK;
}

int cmdGet(AdbClient& client, const std::string& wfxRemote, const std::string& localPath,
           uint64_t cancelAfterBytes) {
    PluginCore core(client);
    std::string error;
    int result = core.getFile(wfxRemote, localPath, DRIVER_COPY_FLAGS,
                              makeCancellingProgress(cancelAfterBytes), &error);
    if (result == FS_FILE_OK) {
        std::cout << "OK\n";
        return EXIT_OK;
    }
    if (result == FS_FILE_USERABORT) {
        std::cout << "ABORTED\n";
        return EXIT_ABORTED;
    }
    std::cerr << "ERROR: getFile failed (code " << result << "): " << error << "\n";
    return EXIT_ERROR;
}

int cmdPut(AdbClient& client, const std::string& localPath, const std::string& wfxRemote,
           uint64_t cancelAfterBytes) {
    PluginCore core(client);
    std::string error;
    int result = core.putFile(localPath, wfxRemote, DRIVER_COPY_FLAGS,
                              makeCancellingProgress(cancelAfterBytes), &error);
    if (result == FS_FILE_OK) {
        std::cout << "OK\n";
        return EXIT_OK;
    }
    if (result == FS_FILE_USERABORT) {
        std::cout << "ABORTED\n";
        return EXIT_ABORTED;
    }
    std::cerr << "ERROR: putFile failed (code " << result << "): " << error << "\n";
    return EXIT_ERROR;
}

// Names of the regular files directly inside localDir, sorted so a run is
// reproducible. Subdirectories, dotfiles and anything that is not a
// regular file are skipped -- putmany is a flat corpus mover, not a
// recursive copy.
bool localRegularFileNames(const std::string& localDir, std::vector<std::string>* out) {
    DIR* dir = ::opendir(localDir.c_str());
    if (dir == nullptr) {
        return false;
    }
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') {
            continue;
        }
        struct stat st;
        if (::stat((localDir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        out->push_back(name);
    }
    ::closedir(dir);
    std::sort(out->begin(), out->end());
    return true;
}

// Uploads every regular file of localDir into wfxDir, in ONE process over
// ONE PluginCore. tests/bench.sh's baseline arm is a single `adb push` of
// the whole directory over a single connection; driving the plugin one
// file per process meant paying a process launch (and, before the probe
// in makeClient above, an `adb start-server` spawn) a hundred times that
// the baseline never paid. This is the matching shape.
int cmdPutMany(AdbClient& client, const std::string& localDir, const std::string& wfxDir) {
    std::vector<std::string> names;
    if (!localRegularFileNames(localDir, &names)) {
        std::cerr << "ERROR: cannot read local directory: " << localDir << "\n";
        return EXIT_ERROR;
    }
    PluginCore core(client);
    for (const std::string& name : names) {
        std::string error;
        int result = core.putFile(localDir + "/" + name, joinWfxPath(wfxDir, name),
                                  DRIVER_COPY_FLAGS, ProgressFn(), &error);
        if (result != FS_FILE_OK) {
            std::cerr << "ERROR: putFile(" << name << ") failed (code " << result << "): " << error
                      << "\n";
            return EXIT_ERROR;
        }
    }
    std::cout << "OK " << names.size() << "\n";
    return EXIT_OK;
}

// The download counterpart of cmdPutMany: lists wfxDir once and pulls
// every non-directory entry into localDir, all in one process.
int cmdGetMany(AdbClient& client, const std::string& wfxDir, const std::string& localDir) {
    PluginCore core(client);
    std::vector<FindResult> entries;
    std::string warning;
    std::string error;
    if (!core.listDirectory(wfxDir, &entries, &warning, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    size_t moved = 0;
    for (const FindResult& e : entries) {
        if (e.isDir) {
            continue;
        }
        std::string getError;
        int result = core.getFile(joinWfxPath(wfxDir, e.name), localDir + "/" + e.name,
                                  DRIVER_COPY_FLAGS, ProgressFn(), &getError);
        if (result != FS_FILE_OK) {
            std::cerr << "ERROR: getFile(" << e.name << ") failed (code " << result
                      << "): " << getError << "\n";
            return EXIT_ERROR;
        }
        ++moved;
    }
    std::cout << "OK " << moved << "\n";
    return EXIT_OK;
}

int cmdMkdir(AdbClient& client, const std::string& wfxPath) {
    PluginCore core(client);
    std::string error;
    if (!core.makeDir(wfxPath, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
    return EXIT_OK;
}

int cmdRm(AdbClient& client, const std::string& wfxPath) {
    std::string rootReason;
    if (refusesDeviceRoot(wfxPath, &rootReason)) {
        std::cerr << "ERROR: " << rootReason << "\n";
        return EXIT_ERROR;
    }
    PluginCore core(client);
    std::string error;
    if (!core.deleteFile(wfxPath, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
    return EXIT_OK;
}

int cmdRmdir(AdbClient& client, const std::string& wfxPath) {
    std::string rootReason;
    if (refusesDeviceRoot(wfxPath, &rootReason)) {
        std::cerr << "ERROR: " << rootReason << "\n";
        return EXIT_ERROR;
    }
    PluginCore core(client);
    std::string error;
    if (!core.removeDir(wfxPath, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
    return EXIT_OK;
}

int cmdMv(AdbClient& client, const std::string& wfxFrom, const std::string& wfxTo, bool overwrite) {
    PluginCore core(client);
    std::string error;
    bool crossDevice = false;
    bool targetExists = false;
    bool ok = core.renameOrMove(wfxFrom, wfxTo, /*move=*/true, overwrite, &error, &crossDevice,
                                &targetExists);
    if (!ok) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
    return EXIT_OK;
}

int cmdSettime(AdbClient& client, const std::string& wfxRemote, int64_t epoch,
               TouchStrategy strategy) {
    PluginCore core(client);
    std::string error;
    if (!core.setModificationTime(wfxRemote, epoch, &error, strategy)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
    return EXIT_OK;
}

// Lists wfxDir three times through one PluginCore -- and therefore one
// listing cache -- sleeping between them, printing "FIRST/CACHED/EXPIRED
// <count>". A caller that changes the directory from outside during the
// first sleep sees CACHED still report the old count (the cache is really
// serving) and EXPIRED report the new one (the TTL really elapsed). Both
// halves matter: a cache that never expired hid changes made on the phone
// forever, and a cache that never hit would make browsing slow.
int cmdCacheTtl(AdbClient& client, const std::string& wfxDir, unsigned sleep1, unsigned sleep2) {
    PluginCore core(client);
    const char* const LABELS[] = {"FIRST", "CACHED", "EXPIRED"};
    const unsigned sleeps[] = {sleep1, sleep2, 0};
    for (int i = 0; i < 3; ++i) {
        std::vector<FindResult> entries;
        std::string warning;
        std::string error;
        if (!core.listDirectory(wfxDir, &entries, &warning, &error)) {
            std::cerr << "ERROR: " << error << "\n";
            return EXIT_ERROR;
        }
        std::cout << LABELS[i] << " " << entries.size() << "\n";
        std::cout.flush();
        if (sleeps[i] > 0) {
            ::sleep(sleeps[i]);
        }
    }
    return EXIT_OK;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            printUsage();
            return EXIT_USAGE;
        }
        std::string command = argv[1];
        std::unique_ptr<AdbClient> client = makeClient();

        if (command == "list") {
            if (argc != 3) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdList(*client, argv[2]);
        }
        if (command == "get") {
            if (argc < 4 || argc > 5) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t cancelAfter = parseOptionalInt(argc, argv, 4, 0);
            return cmdGet(*client, argv[2], argv[3], static_cast<uint64_t>(cancelAfter));
        }
        if (command == "put") {
            if (argc < 4 || argc > 5) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t cancelAfter = parseOptionalInt(argc, argv, 4, 0);
            return cmdPut(*client, argv[2], argv[3], static_cast<uint64_t>(cancelAfter));
        }
        if (command == "putmany") {
            if (argc != 4) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdPutMany(*client, argv[2], argv[3]);
        }
        if (command == "getmany") {
            if (argc != 4) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdGetMany(*client, argv[2], argv[3]);
        }
        if (command == "mkdir") {
            if (argc != 3) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdMkdir(*client, argv[2]);
        }
        if (command == "rm") {
            if (argc != 3) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdRm(*client, argv[2]);
        }
        if (command == "rmdir") {
            if (argc != 3) {
                printUsage();
                return EXIT_USAGE;
            }
            return cmdRmdir(*client, argv[2]);
        }
        if (command == "mv") {
            if (argc < 4 || argc > 5) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t overwrite = parseOptionalInt(argc, argv, 4, 0);
            return cmdMv(*client, argv[2], argv[3], overwrite != 0);
        }
        if (command == "settime" || command == "settime-t") {
            if (argc != 4) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t epoch = parseOptionalInt(argc, argv, 3, 0);
            TouchStrategy strategy = (command == "settime-t") ? TouchStrategy::TouchTOnly
                                                              : TouchStrategy::EpochThenFallback;
            return cmdSettime(*client, argv[2], epoch, strategy);
        }
        if (command == "cachettl") {
            if (argc != 5) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t sleep1 = parseOptionalInt(argc, argv, 3, 0);
            int64_t sleep2 = parseOptionalInt(argc, argv, 4, 0);
            if (sleep1 < 0 || sleep2 < 0) {
                std::cerr << "device_driver: sleep seconds must not be negative\n";
                return EXIT_USAGE;
            }
            return cmdCacheTtl(*client, argv[2], static_cast<unsigned>(sleep1),
                                static_cast<unsigned>(sleep2));
        }

        printUsage();
        return EXIT_USAGE;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: unhandled exception: " << e.what() << "\n";
        return EXIT_ERROR;
    } catch (...) {
        std::cerr << "ERROR: unhandled exception\n";
        return EXIT_ERROR;
    }
}
