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
// ADB_WFX_DEVICE_TESTS=1 (see run_tests.sh and constraints.md's
// "Testability by injection" -- this binary is the one place besides
// fsplugin.cpp itself that is allowed to build a real TcpTransport and
// talk to a real adb server).
//
// Every WFX-style path argument below ("/<serial>/<on-device-path>") is
// exactly what PluginCore consumes -- see adbutils.hpp's parseRemotePath
// and fsplugin_impl.hpp's public surface (task-8-report.md). Nothing here
// re-implements any plugin logic; it only converts argv into calls on
// PluginCore and prints the result.
#include "../adbclient.hpp"
#include "../adbserver.hpp"
#include "../fsplugin_impl.hpp"
#include "../transport.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// Loopback host AdbClient's TransportFactory connects to -- the same
// constant fsplugin.cpp uses. The port itself follows constraints.md's
// protocol reference (ANDROID_ADB_SERVER_PORT, falling back to
// ADB_DEFAULT_PORT) via adbServerPort().
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
        "  mkdir <wfxpath>\n"
        "  rm <wfxpath>\n"
        "  rmdir <wfxpath>\n"
        "  mv <wfxfrom> <wfxto> [overwrite(0|1)]\n"
        "  settime <wfxremote> <epochSeconds>\n"
        "\n"
        "wfxpath/wfxremote/wfxfrom/wfxto are WFX-style paths:\n"
        "  \"/<serial>/<on-device-absolute-path>\", e.g.\n"
        "  \"/27281FDH2008DM/sdcard/adb_wfx_test/file.bin\".\n"
        "\n"
        "cancelAfterBytes, if given and > 0, makes get/put cancel the\n"
        "transfer (as if the user clicked Cancel) once that many bytes\n"
        "have moved -- used by device_test.sh's cancellation check.\n";
}

// Builds the same AdbClient wiring FsInitW uses in fsplugin.cpp: find and
// start a local adb server, then connect over TCP to it per request.
std::unique_ptr<AdbClient> makeClient() {
    auto getEnv = [](const char* name) -> const char* { return std::getenv(name); };
    auto isExecutable = [](const std::string& path) -> bool {
        return ::access(path.c_str(), X_OK) == 0;
    };

    int port = adbServerPort(getEnv);
    std::string adbBinary = findAdbBinary(getEnv, isExecutable);
    if (!adbBinary.empty()) {
        startAdbServer(adbBinary);
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

int cmdSettime(AdbClient& client, const std::string& wfxRemote, int64_t epoch) {
    PluginCore core(client);
    std::string error;
    if (!core.setModificationTime(wfxRemote, epoch, &error)) {
        std::cerr << "ERROR: " << error << "\n";
        return EXIT_ERROR;
    }
    std::cout << "OK\n";
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
        if (command == "settime") {
            if (argc != 4) {
                printUsage();
                return EXIT_USAGE;
            }
            int64_t epoch = parseOptionalInt(argc, argv, 3, 0);
            return cmdSettime(*client, argv[2], epoch);
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
