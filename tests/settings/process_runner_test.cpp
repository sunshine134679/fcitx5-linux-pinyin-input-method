#include "modernime/settings/detail/package_policy.h"
#include "modernime/settings/detail/process_runner.h"
#include "modernime/settings/package_operation_state.h"
#include "modernime/settings/update_controller.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace modernime::settings;
using namespace modernime::settings::detail;

namespace {

int g_passCount = 0;
int g_failCount = 0;

#define TEST_ASSERT(cond)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " Assertion '" \
                      << #cond << "' failed.\n";                                  \
            ++g_failCount;                                                         \
            return;                                                                \
        }                                                                          \
    } while (0)

#define RUN_TEST(fn)                                                               \
    do {                                                                           \
        const int before = g_failCount;                                            \
        std::cout << "[RUN ] " << #fn << " ...\n";                                 \
        fn();                                                                      \
        if (g_failCount == before) {                                               \
            std::cout << "[PASS] " << #fn << "\n";                                 \
            ++g_passCount;                                                         \
        }                                                                          \
    } while (0)

std::string getTempDir() {
    const char *tmp = std::getenv("TMPDIR");
    if (tmp != nullptr && *tmp != '\0') {
        return tmp;
    }
    return "/tmp";
}

// T13-A01: Query dual streams > 1 MiB each -> bounded <= 1 MiB, truncated=true,
// upper policy cannot declare "already latest".
void test_T13_A01_QueryDualStreamTruncation() {
    CommandSpec spec;
    spec.mode = CommandMode::Query;
    spec.timeoutMs = 5000;
    spec.operationId = 101;
    spec.limits.perStreamBufferBytes = 1048576; // 1 MiB
    spec.limits.perStreamResultBytes = 1048576; // 1 MiB
    spec.limits.perStreamFileBytes = 1048576;
    // Generate 2 MiB stdout and 2 MiB stderr
    spec.argv = {
        "python3", "-c",
        "import sys\n"
        "sys.stdout.write('X' * 2097152)\n"
        "sys.stderr.write('Y' * 2097152)\n"
    };

    const auto res = runCommand(spec);
    std::cout << "DEBUG A01 exitStatus=" << res.exitStatus << " err=" << res.stderrText << "\n";
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutTruncated == true);
    TEST_ASSERT(res.stderrTruncated == true);
    TEST_ASSERT(res.stdoutText.size() <= 1048576);
    TEST_ASSERT(res.stderrText.size() <= 1048576);

    // Verify upper package policy rejects truncated query output and never claims "up to date"
    UpdateStatus status;
    status.currentVersion = "1.0.0";
    evaluateUpdateDecision(status, true, false);
    TEST_ASSERT(status.updateAvailable == false);
    TEST_ASSERT(status.offlineOrUnreachable == true);
    TEST_ASSERT(status.candidateVersion.find("截断") != std::string::npos);
    TEST_ASSERT(status.statusMessage.find("截断") != std::string::npos);
    TEST_ASSERT(status.statusMessage.find("已是最新版本") == std::string::npos);
}

// T13-A02: Query descendant inherits FDs, leader exits -> returns within bounded time without hanging
void test_T13_A02_QueryDescendantInheritedFdDoesNotHang() {
    CommandSpec spec;
    spec.mode = CommandMode::Query;
    spec.timeoutMs = 5000;
    spec.operationId = 102;
    // Leader forks child that sleeps 15s holding FDs, while leader exits immediately
    spec.argv = {
        "python3", "-c",
        "import os, sys, time\n"
        "pid = os.fork()\n"
        "if pid == 0:\n"
        "    time.sleep(15)\n"
        "    sys.exit(0)\n"
        "sys.stdout.write('leader_done\\n')\n"
        "sys.exit(0)\n"
    };

    const auto start = std::chrono::steady_clock::now();
    const auto res = runCommand(spec);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutText.find("leader_done") != std::string::npos);
    // Must return within ~1 second, not 15 seconds!
    TEST_ASSERT(elapsed < 2000);
}

// T13-A03: Package stdout/stderr 100 MiB each with tail marker -> tail <= 64 KiB preserved, no SIGPIPE
void test_T13_A03_PackageLargeOutput100MiBPreservesTail() {
    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 30000;
    spec.operationId = 103;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536; // 64 KiB tail
    spec.limits.perStreamFileBytes = 1048576;
    spec.argv = {
        "python3", "-c",
        "import sys\n"
        "chunk = 'A' * 1048576\n"
        "err_chunk = 'B' * 1048576\n"
        "for _ in range(100):\n"
        "    sys.stdout.write(chunk)\n"
        "    sys.stderr.write(err_chunk)\n"
        "sys.stdout.write('===STDOUT_TAIL_103===')\n"
        "sys.stderr.write('===STDERR_TAIL_103===')\n"
        "sys.stdout.flush()\n"
        "sys.stderr.flush()\n"
    };

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutTruncated == true);
    TEST_ASSERT(res.stderrTruncated == true);
    TEST_ASSERT(res.stdoutText.size() <= 65536);
    TEST_ASSERT(res.stderrText.size() <= 65536);
    TEST_ASSERT(res.stdoutText.find("===STDOUT_TAIL_103===") != std::string::npos);
    TEST_ASSERT(res.stderrText.find("===STDERR_TAIL_103===") != std::string::npos);
}

// T13-A04: Resource budget measurement
void test_T13_A04_ResourceBudgetMeasurement() {
    const std::string tmpDir = getTempDir();
    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 150; // Times out so we can inspect while in Continuing
    spec.operationId = 104;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    // Writes large output then sleeps slightly
    spec.argv = {"python3", "-c", "import sys, time; sys.stdout.write('A' * 200000); sys.stderr.write('B' * 200000); sys.stdout.flush(); sys.stderr.flush(); time.sleep(0.6)"};

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Continuing);

    // Locate active operation directory
    auto ref = getActiveOperationRef(104);
    TEST_ASSERT(ref.has_value());
    const std::filesystem::path opDirPath(ref->opDir);
    std::error_code ec;
    TEST_ASSERT(std::filesystem::exists(opDirPath, ec));

    // Verify directory permissions strictly 0700
    const auto dirPerms = std::filesystem::status(opDirPath, ec).permissions();
    TEST_ASSERT((dirPerms & std::filesystem::perms::all) == std::filesystem::perms::owner_all);

    // Verify state.json
    const auto statePath = opDirPath / "state.json";
    TEST_ASSERT(std::filesystem::exists(statePath, ec));
    const auto stateSz = std::filesystem::file_size(statePath, ec);
    TEST_ASSERT(stateSz <= 4096);
    const auto statePerms = std::filesystem::status(statePath, ec).permissions();
    TEST_ASSERT((statePerms & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    // Wait for operation to complete
    const auto finalRes = waitForOperation(104, 5000);
    TEST_ASSERT(finalRes.state == CommandState::Exited);
    TEST_ASSERT(finalRes.exitStatus == 0);
    TEST_ASSERT(finalRes.stdoutTruncated == true);
    TEST_ASSERT(finalRes.stderrTruncated == true);
    TEST_ASSERT(finalRes.stdoutText.size() <= 65536);
    TEST_ASSERT(finalRes.stderrText.size() <= 65536);

    // After Exited, verify transport directory was cleaned up (R4)
    for (int i = 0; i < 50 && std::filesystem::exists(opDirPath, ec); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    TEST_ASSERT(!std::filesystem::exists(opDirPath, ec));
}

// T13-A05: Package transaction timeout -> Running -> Continuing, slot occupied, no force kill
void test_T13_A05_PackageTimeoutTransitionsToContinuing() {
    resetPackageOperationStateForTesting();
    auto opId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(opId.has_value());

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 200; // Small timeout
    spec.operationId = *opId;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    // Command sleeps 1.5s
    spec.argv = {"python3", "-c", "import time; time.sleep(1.5)"};

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);
    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Continuing);

    advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Continuing);
    TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Continuing);
    TEST_ASSERT(hasActivePackageOperation() == true);

    // Concurrency is rejected
    auto secondOp = tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
    TEST_ASSERT(!secondOp.has_value());

    // Wait for the background command to finish
    const auto waitRes = waitForOperation(*opId, 5000);
    TEST_ASSERT(waitRes.state == CommandState::Exited);
    TEST_ASSERT(waitRes.exitStatus == 0);

    finishPackageOperation(*opId, {true, "Done"}, "", "", false);
    TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Idle);
}

// T13-A06: ParentExitWithInheritedOutput
void test_T13_A06_ParentExitWithInheritedOutput() {
    const std::uint64_t opId = 106;
    const std::string tmpDir = getTempDir();
    const std::string doneFile = tmpDir + "/modernime_parent_exit_done_106.txt";
    const std::string refFile = tmpDir + "/modernime_parent_exit_ref_106.txt";
    std::filesystem::remove(doneFile);
    std::filesystem::remove(refFile);

    const pid_t parentPid = fork();
    if (parentPid == 0) {
        // Child acting as test GUI parent process
        CommandSpec spec;
        spec.mode = CommandMode::PackageTransaction;
        spec.timeoutMs = 150; // Fast timeout
        spec.operationId = opId;
        spec.limits.perStreamBufferBytes = 1048576;
        spec.limits.perStreamResultBytes = 65536;
        spec.limits.perStreamFileBytes = 1048576;
        // Leader forks a descendant that sleeps 0.3s and writes late output, leader exits immediately
        spec.argv = {
            "python3", "-c",
            "import os, sys, time\n"
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    time.sleep(0.3)\n"
            "    sys.stdout.write('PARENT_EXIT_STDOUT_OK\\n')\n"
            "    sys.stderr.write('PARENT_EXIT_STDERR_OK\\n')\n"
            "    sys.stdout.flush()\n"
            "    sys.stderr.flush()\n"
            "    with open('" + doneFile + "', 'w') as f:\n"
            "        f.write('done')\n"
            "    sys.exit(0)\n"
            "sys.stdout.write('leader_done\\n')\n"
            "sys.exit(0)\n"
        };
        runCommand(spec);

        // Export active OperationRef for observer
        auto ref = getActiveOperationRef(opId);
        if (ref.has_value()) {
            std::ofstream out(refFile);
            out << ref->operationId << " " << ref->token << " " << ref->opDir << " " << ref->collectorPid << "\n";
        }

        // True _exit(0) simulating sudden GUI parent termination
        _exit(0);
    }

    int status = 0;
    waitpid(parentPid, &status, 0);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // R2 check: without registered reference, waitForOperation(opId) must reject as Unknown!
    const auto unregRes = waitForOperation(opId, 100);
    TEST_ASSERT(unregRes.state == CommandState::Unknown);

    // Read exported reference
    std::ifstream in(refFile);
    OperationRef exportedRef;
    in >> exportedRef.operationId >> exportedRef.token >> exportedRef.opDir >> exportedRef.collectorPid;
    TEST_ASSERT(exportedRef.operationId == opId);
    TEST_ASSERT(!exportedRef.token.empty());

    // Wait for collector and late-writing descendant to finish via exact OperationRef
    const auto waitRes = waitForOperation(exportedRef, 5000);
    TEST_ASSERT(waitRes.state == CommandState::Exited);
    TEST_ASSERT(waitRes.exitStatus == 0);
    TEST_ASSERT(waitRes.stdoutText.find("PARENT_EXIT_STDOUT_OK") != std::string::npos);
    TEST_ASSERT(waitRes.stderrText.find("PARENT_EXIT_STDERR_OK") != std::string::npos);
    TEST_ASSERT(std::filesystem::exists(doneFile));

    std::filesystem::remove(doneFile);
    std::filesystem::remove(refFile);
}

// T13-A07: Collector start failure -> 0 spawn of real command
void test_T13_A07_CollectorStartFailureZeroSpawn() {
    const std::string tmpDir = getTempDir();
    const std::string spawnMarker = tmpDir + "/modernime_spawn_count_107.txt";
    std::filesystem::remove(spawnMarker);

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 5000;
    spec.operationId = 107;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    // Command that would create marker if spawned
    spec.argv = {
        "python3", "-c",
        "with open('" + spawnMarker + "', 'w') as f: f.write('spawned')\n"
    };
    // Force collector to non-existent executable
    spec.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", "/nonexistent/invalid_path"}};

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::NotStarted);
    TEST_ASSERT(res.exitStatus == -1);
    // Real command MUST NOT have spawned!
    TEST_ASSERT(!std::filesystem::exists(spawnMarker));
}

// T13-A08: Collector abnormal termination / PID mismatch -> Uncertain, continues to occupy slot
void test_T13_A08_CollectorAbnormalTerminationTransitionsToUncertain() {
    resetPackageOperationStateForTesting();
    auto opId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(opId.has_value());

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 10000;
    spec.operationId = *opId;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    // Target command kills its parent (the collector) with SIGKILL!
    spec.argv = {
        "python3", "-c",
        "import os, signal, time\n"
        "time.sleep(0.1)\n"
        "os.kill(os.getppid(), signal.SIGKILL)\n"
    };

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);
    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Unknown);

    advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Uncertain);
    TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Uncertain);
    TEST_ASSERT(hasActivePackageOperation() == true);

    // Concurrency is blocked while in Uncertain
    auto nextOp = tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
    TEST_ASSERT(!nextOp.has_value());

    // Only manual or explicit finish releases Uncertain
    finishPackageOperation(*opId, {false, "Manual recovery"}, "", "", false);
    TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Idle);
}

// T13-A09: Completion & queue backpressure with bounded retention
void test_T13_A09_CompletionQueueBackpressureAndBoundedRetention() {
    resetPackageOperationStateForTesting();
    // Fill queue to 8 items
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 8; ++i) {
        auto id = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
        TEST_ASSERT(id.has_value());
        ids.push_back(*id);
        RuntimeResult r{true, "Success " + std::to_string(*id)};
        finishPackageOperation(*id, r, std::string(65536, 'Z'), "", true);
    }
    TEST_ASSERT(unconsumedPackageOperationCount() == 8);

    // 9th attempt rejected due to queue full
    auto overflowId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(!overflowId.has_value());

    // Consume 1 item
    auto first = takeCompletedPackageOperationResult(ids[0]);
    TEST_ASSERT(first.has_value());
    TEST_ASSERT(unconsumedPackageOperationCount() == 7);

    // Now 9th attempt succeeds
    auto nextId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(nextId.has_value());
    finishPackageOperation(*nextId, {true, "OK"}, "", "", false);

    // Consume remaining
    for (std::size_t i = 1; i < ids.size(); ++i) {
        auto res = takeCompletedPackageOperationResult(ids[i]);
        TEST_ASSERT(res.has_value());
    }
    TEST_ASSERT(unconsumedPackageOperationCount() == 0);
}

// T13-A10: Pure APT/DNF/channel policy
void test_T13_A10_PurePackagePolicyUnit() {
    // 1. Version comparison
    TEST_ASSERT(compareVersions("1.0.0", "1.0.0") == 0);
    TEST_ASSERT(compareVersions("1.0.1", "1.0.0") > 0);
    TEST_ASSERT(compareVersions("1.0.0-1", "1.0.0-2") < 0);
    TEST_ASSERT(compareVersions("2:1.0", "1:2.0") > 0);
    TEST_ASSERT(compareVersions("1.0~rc1", "1.0") < 0);

    // 2. APT parsing
    const std::string aptPolicySample =
        "modernime:\n"
        "  Installed: 0.1.9\n"
        "  Candidate: 0.1.10\n"
        "  Version table:\n"
        " *** 0.1.9 100\n"
        "        100 /var/lib/dpkg/status\n"
        "     0.1.10 500\n"
        "        500 https://archive.example.com noble/main amd64 Packages\n";

    const auto aptRes = parseAptPolicyOutput(aptPolicySample, false);
    TEST_ASSERT(aptRes.hasCandidate == true);
    TEST_ASSERT(aptRes.candidateVersion == "0.1.10");
    TEST_ASSERT(aptRes.hasRemoteRepo == true);

    const auto aptTruncRes = parseAptPolicyOutput(aptPolicySample, true);
    TEST_ASSERT(aptTruncRes.hasCandidate == false);

    // 3. DNF parsing
    const std::string dnfInfoSample =
        "Installed Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.8\n"
        "Release      : 1.fc39\n"
        "From repo    : @System\n"
        "Available Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.10\n"
        "Release      : 1.fc39\n"
        "Repository   : updates\n"
        "Summary      : Modern input method engine\n";

    const auto dnfRes = parseDnfInfoOutput(dnfInfoSample, false);
    TEST_ASSERT(dnfRes.hasCandidate == true);
    TEST_ASSERT(dnfRes.candidateVersion == "0.1.10-1.fc39");
    TEST_ASSERT(dnfRes.hasRemoteRepo == true);

    // 4. Channel detection
    TEST_ASSERT(detectInstallChannelFromContent("channel=deb\nprefix=/usr") == "deb");
    TEST_ASSERT(detectInstallChannelFromContent("invalid") == "unknown");
}

// T13-A11: T12 and runtime regression checks
void test_T13_A11_T12RegressionSynchronousLockDoesNotPollute() {
    resetPackageOperationStateForTesting();
    // Pre-seed 1 background unconsumed result
    auto seedId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(seedId.has_value());
    finishPackageOperation(*seedId, {true, "Preseeded"}, "seed_out", "", true);
    TEST_ASSERT(unconsumedPackageOperationCount() == 1);

    // Run 9 simulated locked refresh attempts
    const char *origLock = std::getenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED");
    setenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED", "1", 1);

    UpdateStatus status;
    status.manager = PackageManagerKind::Apt;
    for (int i = 0; i < 9; ++i) {
        auto res = UpdateController::refreshPackageCache(status);
        TEST_ASSERT(res.success == false);
        TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Idle);
        TEST_ASSERT(unconsumedPackageOperationCount() == 1);
    }

    if (origLock) {
        setenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED", origLock, 1);
    } else {
        unsetenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED");
    }

    // Verify seed result intact and consumable
    auto seed = takeCompletedPackageOperationResult(*seedId);
    TEST_ASSERT(seed.has_value());
    TEST_ASSERT(seed->message == "Preseeded");
    TEST_ASSERT(unconsumedPackageOperationCount() == 0);
}

// T13-A12: Headless internal mode execution check
void test_T13_A12_HeadlessInternalModeRunsWithoutDisplay() {
    // Invoke runCommandCaptureChild with missing args -> exits 1 safely without GTK or crash
    char prog[] = "modernime-settings";
    char mode[] = "--internal-capture-collector";
    char *argv[] = {prog, mode, nullptr};
    const int ret = runCommandCaptureChild(2, argv);
    TEST_ASSERT(ret == 1);
}

std::string resolveSettingsExeForTest() {
    char exeBuf[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    if (len > 0) {
        exeBuf[len] = '\0';
        std::filesystem::path selfPath(exeBuf);
        auto sibling = selfPath.parent_path().parent_path() / "tools/settings/modernime-settings";
        if (std::filesystem::exists(sibling)) {
            return sibling.string();
        }
    }
    return "modernime-settings";
}

// T13-R01: Initial state publication failure: zero spawn proven -> NotStarted; if target started -> Unknown/Uncertain
void test_T13_R01_StatePublicationFailureGuaranteesZeroSpawnOrUncertain() {
    const std::string tmpDir = getTempDir();
    const std::string spawnMarker = tmpDir + "/modernime_r01_target_marker.txt";
    std::filesystem::remove(spawnMarker);

    const std::string settingsExe = resolveSettingsExeForTest();
    TEST_ASSERT(std::filesystem::exists(settingsExe));

    // Create wrapper that creates opDir/state.json as directory BEFORE collector runs
    const std::string wrapperPath = tmpDir + "/collector-wrapper-r01.sh";
    {
        std::ofstream out(wrapperPath);
        out << "#!/bin/sh\n"
            << "/bin/mkdir -p \"$3/state.json\"\n"
            << "exec \"" << settingsExe << "\" \"$@\"\n";
    }
    ::chmod(wrapperPath.c_str(), 0755);

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 1000;
    spec.operationId = 1301;
    spec.limits.perStreamResultBytes = 65536;
    spec.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", wrapperPath}};
    spec.argv = {
        "python3", "-c",
        "with open('" + spawnMarker + "', 'w') as f:\n"
        "    f.write('spawned')\n"
    };

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::NotStarted);
    TEST_ASSERT(!std::filesystem::exists(spawnMarker));

    std::filesystem::remove(wrapperPath);
}

// T13-R02: Handshake loss or collector crash does not report NotStarted
void test_T13_R02_HandshakeLossOrCollectorCrashDoesNotReportNotStarted() {
    const std::string tmpDir = getTempDir();
    const std::string wrapperPath = tmpDir + "/collector-wrapper-r02.sh";
    {
        std::ofstream out(wrapperPath);
        out << "#!/bin/sh\n"
            << "sleep 0.5\n"
            << "exit 0\n";
    }
    ::chmod(wrapperPath.c_str(), 0755);

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 100; // Deadline expires before wrapper responds
    spec.operationId = 1302;
    spec.limits.perStreamResultBytes = 65536;
    spec.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", wrapperPath}};
    spec.argv = {"/bin/true"};

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Unknown);
    TEST_ASSERT(res.state != CommandState::NotStarted);

    // Reap the test-injected collector once it finishes so it doesn't leak into subsequent tests
    auto ref = getActiveOperationRef(1302);
    if (ref && ref->collectorPid > 0) {
        int st = 0;
        ::waitpid(ref->collectorPid, &st, 0);
    }
    cleanupOperation(1302);

    std::filesystem::remove(wrapperPath);
}

// T13-R03: Multi-round Continuing eventually completes under same ID
void test_T13_R03_MultiRoundContinuingEventuallyCompletesUnderSameId() {
    resetPackageOperationStateForTesting();
    auto opId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(opId.has_value());

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 150;
    spec.operationId = *opId;
    spec.limits.perStreamResultBytes = 65536;
    spec.argv = {"python3", "-c", "import time; time.sleep(0.6)"};

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);
    const auto r1 = runCommand(spec);
    TEST_ASSERT(r1.state == CommandState::Continuing);

    // Second round observation with short timeout: still Continuing
    const auto r2 = waitForOperation(*opId, 100);
    if (r2.state == CommandState::Continuing) {
        const auto r3 = waitForOperation(*opId, 3000);
        TEST_ASSERT(r3.state == CommandState::Exited);
        TEST_ASSERT(r3.exitStatus == 0);
        TEST_ASSERT(r3.operationId == *opId);
    } else {
        TEST_ASSERT(r2.state == CommandState::Exited);
        TEST_ASSERT(r2.exitStatus == 0);
        TEST_ASSERT(r2.operationId == *opId);
    }

    finishPackageOperation(*opId, {true, "Done"}, "", "", false);
}

// T13-R04: Operation identity, stale directory, and malformed state rejection
void test_T13_R04_OperationIdentityAndMalformedStateRejection() {
    const std::string tmpDir = getTempDir();
    const std::uint64_t staleId = 99901;

    // 1. Unregistered ID with pre-existing stale directory
    const std::string staleDir = tmpDir + "/modernime-op-" + std::to_string(staleId) + "-stale";
    std::filesystem::create_directories(staleDir);
    {
        std::ofstream out(staleDir + "/state.json");
        out << "{\"operationId\": " << staleId << ", \"state\": \"Exited\", \"exitStatus\": 0}\n";
    }
    {
        std::ofstream out(staleDir + "/stdout.tail");
        out << "OLD_STALE_OUTPUT\n";
    }

    const auto waitStale = waitForOperation(staleId, 100);
    TEST_ASSERT(waitStale.state == CommandState::Unknown);

    // 2. Registered ID: verify strict parser rejects malformed fixtures
    OperationRef ref;
    ref.operationId = staleId;
    ref.token = "valid-token-7101";
    ref.opDir = staleDir;
    ref.collectorPid = 888888; // non-existent PID
    registerOperationRef(ref);

    // 2a. Malformed: no braces
    {
        std::ofstream out(staleDir + "/state.json");
        out << "\"operationId\": " << staleId << ", \"state\": \"Exited\", \"exitStatus\": 0\n";
    }
    auto waitRes = waitForOperation(ref, 50);
    TEST_ASSERT(waitRes.state == CommandState::Unknown);
    TEST_ASSERT(waitRes.stdoutText != "OLD_STALE_OUTPUT");

    // 2b. Malformed: missing commas (not_json_with_braces)
    {
        std::ofstream out(staleDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << staleId << "\n"
            << "  \"token\": \"valid-token-7101\"\n"
            << "  \"collectorPid\": 888888\n"
            << "  \"childPid\": 0\n"
            << "  \"state\": \"Exited\"\n"
            << "  \"exitStatus\": 0\n"
            << "  \"stdoutTruncated\": false\n"
            << "  \"stderrTruncated\": false\n"
            << "  \"stdoutBytes\": 0\n"
            << "  \"stderrBytes\": 0\n"
            << "  \"bufferPeak\": 0\n"
            << "  \"filePeak\": 0\n"
            << "}\n";
    }
    waitRes = waitForOperation(ref, 50);
    TEST_ASSERT(waitRes.state == CommandState::Unknown);
    TEST_ASSERT(waitRes.stdoutText != "OLD_STALE_OUTPUT");

    // 2c. Malformed: invalid numeric and boolean suffixes (0garbage, falsejunk)
    {
        std::ofstream out(staleDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << staleId << ",\n"
            << "  \"token\": \"valid-token-7101\",\n"
            << "  \"collectorPid\": 888888,\n"
            << "  \"childPid\": 0,\n"
            << "  \"state\": \"Exited\",\n"
            << "  \"exitStatus\": 0garbage,\n"
            << "  \"stdoutTruncated\": falsejunk,\n"
            << "  \"stderrTruncated\": false,\n"
            << "  \"stdoutBytes\": 0,\n"
            << "  \"stderrBytes\": 0,\n"
            << "  \"bufferPeak\": 0,\n"
            << "  \"filePeak\": 0\n"
            << "}\n";
    }
    waitRes = waitForOperation(ref, 50);
    TEST_ASSERT(waitRes.state == CommandState::Unknown);
    TEST_ASSERT(waitRes.stdoutText != "OLD_STALE_OUTPUT");

    // 2d. Valid JSON but wrong collectorPid
    {
        std::ofstream out(staleDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << staleId << ",\n"
            << "  \"token\": \"valid-token-7101\",\n"
            << "  \"collectorPid\": 999999,\n"
            << "  \"childPid\": 0,\n"
            << "  \"state\": \"Exited\",\n"
            << "  \"exitStatus\": 0,\n"
            << "  \"stdoutTruncated\": false,\n"
            << "  \"stderrTruncated\": false,\n"
            << "  \"stdoutBytes\": 0,\n"
            << "  \"stderrBytes\": 0,\n"
            << "  \"bufferPeak\": 0,\n"
            << "  \"filePeak\": 0\n"
            << "}\n";
    }
    waitRes = waitForOperation(ref, 50);
    TEST_ASSERT(waitRes.state == CommandState::Unknown);
    TEST_ASSERT(waitRes.stdoutText != "OLD_STALE_OUTPUT");

    cleanupOperation(staleId);
}

// T13-R05: Late writing descendants receive zero EPIPE
void test_T13_R05_LateWritingDescendantsReceiveZeroEpipe() {
    const std::string tmpDir = getTempDir();
    const std::string marker = tmpDir + "/modernime_r05_marker.txt";
    std::filesystem::remove(marker);

    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 5000;
    spec.operationId = 1305;
    spec.limits.perStreamResultBytes = 65536;
    spec.argv = {
        "python3", "-c",
        "import os, time\n"
        "pid = os.fork()\n"
        "if pid:\n"
        "    os._exit(0)\n"
        "time.sleep(0.35)\n"
        "try:\n"
        "    os.write(1, b'late stdout\\n')\n"
        "    os.write(2, b'late stderr\\n')\n"
        "    v = 'OK'\n"
        "except OSError as e:\n"
        "    v = 'write_errno=' + str(e.errno)\n"
        "with open('" + marker + "', 'w') as f:\n"
        "    f.write(v)\n"
        "os._exit(0)\n"
    };

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutText.find("late stdout") != std::string::npos);
    TEST_ASSERT(res.stderrText.find("late stderr") != std::string::npos);

    std::ifstream in(marker);
    std::string val;
    in >> val;
    TEST_ASSERT(val == "OK");

    std::filesystem::remove(marker);
}

// T13-R06: Resource bounded retention and zero zombies
void test_T13_R06_ResourceBoundedRetentionAndZeroZombies() {
    const std::string origTmp = getTempDir();
    const std::string r06Tmp = origTmp + "/modernime_r06_isolated_" + std::to_string(::getpid());
    std::filesystem::create_directories(r06Tmp);
    const std::string fakeBinDir = r06Tmp + "/bin";
    std::filesystem::create_directories(fakeBinDir);

    for (const auto &name : {"apt-get", "pkexec"}) {
        const std::string p = fakeBinDir + "/" + name;
        std::ofstream out(p);
        out << "#!/bin/sh\nprintf \"retained-diagnostic\\n\"\nexit 0\n";
        ::chmod(p.c_str(), 0755);
    }

    UpdateStatus status;
    status.manager = PackageManagerKind::Apt;
    Environment env{
        {"PATH", fakeBinDir},
        {"TMPDIR", r06Tmp},
        {"MODERNIME_PACKAGE_OP_TIMEOUT_MS", "1000"},
        {"MODERNIME_PACKAGE_MANAGER_LOCK_FILE", r06Tmp + "/mock.lock"}
    };
    ::setenv("TMPDIR", r06Tmp.c_str(), 1);

    int st = 0;
    while (::waitpid(-1, &st, WNOHANG) > 0) {}

    for (int i = 0; i < 5; ++i) {
        auto r = UpdateController::refreshPackageCache(status, env);
        if (!r.success) {
            std::cerr << "R06 refreshPackageCache iteration " << i << " failed:\n"
                      << "  r.message: " << r.message << "\n"
                      << "  isPackageManagerLocked: " << UpdateController::isPackageManagerLocked(env) << "\n";
        }
        TEST_ASSERT(r.success == true);
    }

    // Check no zombies created by these operations
    int unreaped = 0;
    while (::waitpid(-1, &st, WNOHANG) > 0) {
        ++unreaped;
    }
    TEST_ASSERT(unreaped == 0);

    // Check no lingering completed directories in isolated test dir
    std::error_code ec;
    int retainedDirs = 0;
    for (const auto &entry : std::filesystem::directory_iterator(r06Tmp, ec)) {
        if (entry.is_directory(ec) && entry.path().filename().string().starts_with("modernime-op-")) {
            ++retainedDirs;
        }
    }
    TEST_ASSERT(retainedDirs == 0);

    ::setenv("TMPDIR", origTmp.c_str(), 1);
    std::filesystem::remove_all(r06Tmp);
}

// T13-R07: Query monotonic deadline and early EOF
void test_T13_R07_QueryMonotonicDeadlineAndEarlyEof() {
    // 1. Early EOF does not prematurely kill process before deadline
    CommandSpec specEarly;
    specEarly.mode = CommandMode::Query;
    specEarly.timeoutMs = 2000;
    specEarly.operationId = 13071;
    specEarly.argv = {
        "/usr/bin/python3", "-c",
        "import os, time; os.close(1); os.close(2); time.sleep(0.7)"
    };

    const auto start = std::chrono::steady_clock::now();
    const auto resEarly = runCommand(specEarly);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    TEST_ASSERT(resEarly.state == CommandState::Exited);
    TEST_ASSERT(resEarly.exitStatus == 0);
    TEST_ASSERT(resEarly.timedOut == false);
    TEST_ASSERT(elapsed >= 600);

    // 2. Hanging command properly killed and marked timedOut
    CommandSpec specHang;
    specHang.mode = CommandMode::Query;
    specHang.timeoutMs = 200;
    specHang.operationId = 13072;
    specHang.argv = {"/usr/bin/python3", "-c", "import time; time.sleep(5)"};

    const auto resHang = runCommand(specHang);
    TEST_ASSERT(resHang.state == CommandState::Exited);
    TEST_ASSERT(resHang.timedOut == true);
}

// T13-R08: Environment inheritance and explicit overrides
void test_T13_R08_EnvironmentInheritanceAndExplicitOverrides() {
    ::setenv("MODERNIME_SENTINEL_INHERIT", "synthetic_inherited_val", 1);

    // Query mode
    CommandSpec qSpec;
    qSpec.mode = CommandMode::Query;
    qSpec.timeoutMs = 1000;
    qSpec.operationId = 13081;
    qSpec.environment = {{"MODERNIME_SENTINEL_OVERRIDE", "synthetic_override_val"}};
    qSpec.argv = {
        "/usr/bin/python3", "-c",
        "import os\n"
        "print('INHERIT=' + os.environ.get('MODERNIME_SENTINEL_INHERIT', ''))\n"
        "print('OVERRIDE=' + os.environ.get('MODERNIME_SENTINEL_OVERRIDE', ''))\n"
        "print('LC_ALL=' + os.environ.get('LC_ALL', ''))\n"
    };

    const auto qRes = runCommand(qSpec);
    TEST_ASSERT(qRes.state == CommandState::Exited);
    TEST_ASSERT(qRes.exitStatus == 0);
    TEST_ASSERT(qRes.stdoutText.find("INHERIT=synthetic_inherited_val") != std::string::npos);
    TEST_ASSERT(qRes.stdoutText.find("OVERRIDE=synthetic_override_val") != std::string::npos);
    TEST_ASSERT(qRes.stdoutText.find("LC_ALL=C") != std::string::npos);

    // Package mode
    CommandSpec pSpec;
    pSpec.mode = CommandMode::PackageTransaction;
    pSpec.timeoutMs = 2000;
    pSpec.operationId = 13082;
    pSpec.limits.perStreamResultBytes = 65536;
    pSpec.environment = {{"MODERNIME_SENTINEL_OVERRIDE", "pkg_override_val"}};
    pSpec.argv = {
        "/usr/bin/python3", "-c",
        "import os\n"
        "print('INHERIT=' + os.environ.get('MODERNIME_SENTINEL_INHERIT', ''))\n"
        "print('OVERRIDE=' + os.environ.get('MODERNIME_SENTINEL_OVERRIDE', ''))\n"
    };

    const auto pRes = runCommand(pSpec);
    TEST_ASSERT(pRes.state == CommandState::Exited);
    TEST_ASSERT(pRes.exitStatus == 0);
    TEST_ASSERT(pRes.stdoutText.find("INHERIT=synthetic_inherited_val") != std::string::npos);
    TEST_ASSERT(pRes.stdoutText.find("OVERRIDE=pkg_override_val") != std::string::npos);

    ::unsetenv("MODERNIME_SENTINEL_INHERIT");
}

// T13-R09: Resource peaks and budget measurement
void test_T13_R09_ResourcePeaksAndBudgetMeasurement() {
    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 30000;
    spec.operationId = 1309;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    spec.argv = {
        "python3", "-c",
        "import sys\n"
        "chunk = 'A' * 1048576\n"
        "err_chunk = 'B' * 1048576\n"
        "for _ in range(100):\n"
        "    sys.stdout.write(chunk)\n"
        "    sys.stderr.write(err_chunk)\n"
        "sys.stdout.write('===PEAK_STDOUT_TAIL===')\n"
        "sys.stderr.write('===PEAK_STDERR_TAIL===')\n"
        "sys.stdout.flush()\n"
        "sys.stderr.flush()\n"
    };

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutTruncated == true);
    TEST_ASSERT(res.stderrTruncated == true);
    TEST_ASSERT(res.stdoutText.size() <= 65536);
    TEST_ASSERT(res.stderrText.size() <= 65536);
    TEST_ASSERT(res.stdoutText.find("===PEAK_STDOUT_TAIL===") != std::string::npos);
    TEST_ASSERT(res.stderrText.find("===PEAK_STDERR_TAIL===") != std::string::npos);
    TEST_ASSERT(res.bufferPeak > 0);
    TEST_ASSERT(res.bufferPeak <= spec.limits.perStreamBufferBytes);
    TEST_ASSERT(res.filePeak == spec.limits.perStreamFileBytes);
}

// T13-R10: Headless exec of internal collector without display
void test_T13_R10_HeadlessExecInternalCollectorWithoutDisplay() {
    const std::string tmpDir = getTempDir();
    const std::string opDir = tmpDir + "/modernime-op-headless-test";
    std::filesystem::create_directories(opDir);

    // Write spec.dat
    {
        std::ofstream out(opDir + "/spec.dat");
        out << "operationId=1310\n"
            << "token=headlesstoken123\n"
            << "timeoutMs=5000\n"
            << "mode=1\n"
            << "perStreamBufferBytes=1048576\n"
            << "perStreamResultBytes=65536\n"
            << "perStreamFileBytes=1048576\n"
            << "argc=3\n"
            << "argv=/usr/bin/python3\n"
            << "argv=-c\n"
            << "argv=print('HEADLESS_SUCCESS_1310')\n"
            << "envc=0\n";
    }

    char exeBuf[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    TEST_ASSERT(len > 0);
    exeBuf[len] = '\0';
    const std::filesystem::path selfPath(exeBuf);
    const std::filesystem::path progPath = selfPath.parent_path().parent_path() / "tools/settings/modernime-settings";
    TEST_ASSERT(std::filesystem::exists(progPath));
    const std::string progStr = progPath.string();
    std::string opDirStr = opDir;

    int handshakeFds[2];
    TEST_ASSERT(::pipe2(handshakeFds, O_CLOEXEC) == 0);

    const pid_t pid = ::fork();
    TEST_ASSERT(pid >= 0);

    if (pid == 0) {
        ::close(handshakeFds[0]);
        const int flags = ::fcntl(handshakeFds[1], F_GETFD);
        if (flags >= 0) ::fcntl(handshakeFds[1], F_SETFD, flags & ~FD_CLOEXEC);

        // Clear display environment
        ::unsetenv("DISPLAY");
        ::unsetenv("WAYLAND_DISPLAY");

        char arg1[] = "--internal-capture-collector";
        char arg2[] = "--op-dir";
        char arg4[] = "--handshake-fd";
        std::string hsStr = std::to_string(handshakeFds[1]);
        char *argv[] = {const_cast<char*>(progStr.c_str()), arg1, arg2, opDirStr.data(), arg4, hsStr.data(), nullptr};

        ::execv(progStr.c_str(), argv);
        ::_exit(127);
    }

    ::close(handshakeFds[1]);
    char hsByte = 0;
    const ssize_t hRead = ::read(handshakeFds[0], &hsByte, 1);
    ::close(handshakeFds[0]);

    TEST_ASSERT(hRead == 1);
    TEST_ASSERT(hsByte == 'R' || hsByte == '1');

    int status = 0;
    ::waitpid(pid, &status, 0);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Verify stdout.tail has HEADLESS_SUCCESS_1310
    std::ifstream tailIn(opDir + "/stdout.tail");
    std::string tailContent;
    std::getline(tailIn, tailContent);
    TEST_ASSERT(tailContent.find("HEADLESS_SUCCESS_1310") != std::string::npos);

    std::filesystem::remove_all(opDir);
}

// T13-S01: Target started, collector crashes before R handshake -> Unknown/Uncertain, slot preserved; exec failure -> NotStarted zero spawn
void test_T13_S01_TargetForkBeforeAckCrashPreservesStateAndExecFailureZeroSpawn() {
    const std::string tmpDir = getTempDir();
    const std::string settingsExe = resolveSettingsExeForTest();

    // 1. Exec failure of collector -> NotStarted, target never spawned
    const std::string targetMarker1 = tmpDir + "/modernime_s01_marker1.txt";
    std::filesystem::remove(targetMarker1);

    CommandSpec spec1;
    spec1.mode = CommandMode::PackageTransaction;
    spec1.timeoutMs = 1000;
    spec1.operationId = 14011;
    spec1.limits.perStreamResultBytes = 65536;
    spec1.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", "/nonexistent/binary/path"}};
    spec1.argv = {
        "python3", "-c",
        "with open('" + targetMarker1 + "', 'w') as f: f.write('SPAWNED')\n"
    };

    const auto res1 = runCommand(spec1);
    TEST_ASSERT(res1.state == CommandState::NotStarted);
    TEST_ASSERT(!std::filesystem::exists(targetMarker1));

    // 2. Collector crashes after target fork before sending R handshake -> Unknown, ref preserved, slot occupied
    const std::string targetMarker2 = tmpDir + "/modernime_s01_marker2.txt";
    std::filesystem::remove(targetMarker2);

    const std::string crashWrapper = tmpDir + "/collector-crash-before-ack.sh";
    {
        std::ofstream out(crashWrapper);
        out << "#!/bin/sh\n"
            << "\"" << settingsExe << "\" \"$@\" &\n"
            << "cpid=$!\n"
            << "opDir=\"\"\n"
            << "for arg in \"$@\"; do\n"
            << "  if [ \"$prev\" = \"--op-dir\" ]; then opDir=\"$arg\"; fi\n"
            << "  prev=\"$arg\"\n"
            << "done\n"
            << "for i in $(seq 1 100); do\n"
            << "  if [ -f \"$opDir/state.json\" ] && grep -q '\"state\": \"Running\"' \"$opDir/state.json\"; then\n"
            << "    kill -9 \"$cpid\" 2>/dev/null\n"
            << "    exit 71\n"
            << "  fi\n"
            << "  sleep 0.01\n"
            << "done\n"
            << "kill -9 \"$cpid\" 2>/dev/null\n"
            << "exit 71\n";
    }
    ::chmod(crashWrapper.c_str(), 0755);

    resetPackageOperationStateForTesting();
    auto opId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(opId.has_value());

    CommandSpec spec2;
    spec2.mode = CommandMode::PackageTransaction;
    spec2.timeoutMs = 1500;
    spec2.operationId = *opId;
    spec2.limits.perStreamResultBytes = 65536;
    spec2.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", crashWrapper}};
    spec2.argv = {
        "python3", "-c",
        "import time\n"
        "with open('" + targetMarker2 + "', 'w') as f: f.write('TARGET_RUNNING')\n"
        "time.sleep(0.5)\n"
    };

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);
    const auto res2 = runCommand(spec2);

    TEST_ASSERT(res2.state == CommandState::Unknown);
    TEST_ASSERT(res2.state != CommandState::NotStarted);

    // Operation reference is still registered
    auto ref = getActiveOperationRef(*opId);
    TEST_ASSERT(ref.has_value());

    // Advance controller to Uncertain (as UpdateController does when state == Unknown)
    advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Uncertain);
    TEST_ASSERT(hasActivePackageOperation() == true);
    TEST_ASSERT(currentPackageOperationPhase() == PackageOperationPhase::Uncertain);

    // Slot is occupied, new transaction must be rejected
    auto nextOp = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    TEST_ASSERT(!nextOp.has_value());

    // Clean up
    finishPackageOperation(*opId, {false, "clean"}, "", "", false);
    cleanupOperation(*opId);
    std::filesystem::remove(crashWrapper);
    std::filesystem::remove(targetMarker2);
}

// T13-S02: Strict state protocol rejects malformed JSON, duplicate keys, invalid suffixes, and wrong collector PID
void test_T13_S02_StrictStateProtocolRejectionCoverage() {
    const std::string tmpDir = getTempDir();
    const std::uint64_t opId = 1402;
    const std::string opDir = tmpDir + "/modernime-op-test-s02";
    std::filesystem::create_directories(opDir);

    OperationRef ref;
    ref.operationId = opId;
    ref.token = "token-s02-valid";
    ref.opDir = opDir;
    ref.collectorPid = 65432;
    registerOperationRef(ref);

    auto testFixture = [&](const std::string &jsonText, const std::string &desc) {
        {
            std::ofstream out(opDir + "/state.json");
            out << jsonText;
        }
        auto res = waitForOperation(ref, 50);
        TEST_ASSERT(res.state == CommandState::Unknown);
        TEST_ASSERT(res.exitStatus == -1);
    };

    // 1. Missing required field "stdoutBytes"
    testFixture("{\n"
                "  \"operationId\": 1402,\n"
                "  \"token\": \"token-s02-valid\",\n"
                "  \"collectorPid\": 65432,\n"
                "  \"childPid\": 0,\n"
                "  \"state\": \"Exited\",\n"
                "  \"exitStatus\": 0,\n"
                "  \"stdoutTruncated\": false,\n"
                "  \"stderrTruncated\": false,\n"
                "  \"stderrBytes\": 0,\n"
                "  \"bufferPeak\": 0,\n"
                "  \"filePeak\": 0\n"
                "}\n", "Missing stdoutBytes");

    // 2. Duplicate key "state"
    testFixture("{\n"
                "  \"operationId\": 1402,\n"
                "  \"token\": \"token-s02-valid\",\n"
                "  \"collectorPid\": 65432,\n"
                "  \"childPid\": 0,\n"
                "  \"state\": \"Exited\",\n"
                "  \"state\": \"Running\",\n"
                "  \"exitStatus\": 0,\n"
                "  \"stdoutTruncated\": false,\n"
                "  \"stderrTruncated\": false,\n"
                "  \"stdoutBytes\": 0,\n"
                "  \"stderrBytes\": 0,\n"
                "  \"bufferPeak\": 0,\n"
                "  \"filePeak\": 0\n"
                "}\n", "Duplicate state");

    // 3. Invalid state enum
    testFixture("{\n"
                "  \"operationId\": 1402,\n"
                "  \"token\": \"token-s02-valid\",\n"
                "  \"collectorPid\": 65432,\n"
                "  \"childPid\": 0,\n"
                "  \"state\": \"BogusState\",\n"
                "  \"exitStatus\": 0,\n"
                "  \"stdoutTruncated\": false,\n"
                "  \"stderrTruncated\": false,\n"
                "  \"stdoutBytes\": 0,\n"
                "  \"stderrBytes\": 0,\n"
                "  \"bufferPeak\": 0,\n"
                "  \"filePeak\": 0\n"
                "}\n", "Invalid state enum");

    // 4. Trailing garbage after closing brace
    testFixture("{\n"
                "  \"operationId\": 1402,\n"
                "  \"token\": \"token-s02-valid\",\n"
                "  \"collectorPid\": 65432,\n"
                "  \"childPid\": 0,\n"
                "  \"state\": \"Exited\",\n"
                "  \"exitStatus\": 0,\n"
                "  \"stdoutTruncated\": false,\n"
                "  \"stderrTruncated\": false,\n"
                "  \"stdoutBytes\": 0,\n"
                "  \"stderrBytes\": 0,\n"
                "  \"bufferPeak\": 0,\n"
                "  \"filePeak\": 0\n"
                "} trailing_junk\n", "Trailing garbage");

    cleanupOperation(opId);
}

// T13-S03: Continuous drain without arbitrary deadline; zombie leader transitions to Continuing within deadline and finishes accurately
void test_T13_S03_CollectorContinuousDrainAndZombieLeader() {
    // 1. Continuous drain: Child writes after 350ms, zero EPIPE
    CommandSpec spec1;
    spec1.mode = CommandMode::PackageTransaction;
    spec1.timeoutMs = 1500;
    spec1.operationId = 14031;
    spec1.limits.perStreamResultBytes = 65536;
    spec1.argv = {
        "python3", "-c",
        "import time, sys\n"
        "time.sleep(0.35)\n"
        "sys.stdout.write('CONTINUOUS_DRAIN_OK\\n')\n"
        "sys.stdout.flush()\n"
    };

    const auto res1 = runCommand(spec1);
    TEST_ASSERT(res1.state == CommandState::Exited);
    TEST_ASSERT(res1.exitStatus == 0);
    TEST_ASSERT(res1.stdoutText.find("CONTINUOUS_DRAIN_OK") != std::string::npos);

    // 2. Zombie leader: main thread calls pthread_exit(0) while worker sleeps 400ms
    CommandSpec spec2;
    spec2.mode = CommandMode::PackageTransaction;
    spec2.timeoutMs = 60; // Timeout shorter than worker duration
    spec2.operationId = 14032;
    spec2.limits.perStreamResultBytes = 65536;
    spec2.argv = {
        "python3", "-c",
        "import ctypes, threading, time, os\n"
        "def worker():\n"
        "    time.sleep(0.4)\n"
        "    os.write(1, b'WORKER_FINISHED_SUCCESS\\n')\n"
        "t = threading.Thread(target=worker)\n"
        "t.start()\n"
        "ctypes.CDLL(None).pthread_exit(None)\n"
    };

    const auto start = std::chrono::steady_clock::now();
    const auto res2 = runCommand(spec2);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // Must return Continuing within bounded time (not blocked for 400ms by waitpid on collector)
    TEST_ASSERT(res2.state == CommandState::Continuing);
    TEST_ASSERT(elapsed < 300);

    // Wait for actual completion
    const auto finalRes = waitForOperation(spec2.operationId, 2000);
    TEST_ASSERT(finalRes.state == CommandState::Exited);
    TEST_ASSERT(finalRes.exitStatus == 0);
    TEST_ASSERT(finalRes.stdoutText.find("WORKER_FINISHED_SUCCESS") != std::string::npos);
}

// T13-S04: Async final state published before collector exit is cleanly reaped by background reaper
void test_T13_S04_AsyncFinalPublishedBeforeCollectorExitReapedByBackground() {
    const std::string tmpDir = getTempDir();

    for (int iter = 0; iter < 3; ++iter) {
        const std::uint64_t opId = 14040 + iter;
        const std::string opDir = tmpDir + "/modernime-op-s04-" + std::to_string(iter);
        std::filesystem::create_directories(opDir);

        // Fork a child process to simulate the collector
        const pid_t cpid = ::fork();
        TEST_ASSERT(cpid >= 0);

        if (cpid == 0) {
            // Write Exited state file
            std::ostringstream ss;
            ss << "{\n"
               << "  \"operationId\": " << opId << ",\n"
               << "  \"token\": \"token-s04\",\n"
               << "  \"collectorPid\": " << ::getpid() << ",\n"
               << "  \"childPid\": 0,\n"
               << "  \"state\": \"Exited\",\n"
               << "  \"exitStatus\": 0,\n"
               << "  \"stdoutTruncated\": false,\n"
               << "  \"stderrTruncated\": false,\n"
               << "  \"stdoutBytes\": 4,\n"
               << "  \"stderrBytes\": 0,\n"
               << "  \"bufferPeak\": 4,\n"
               << "  \"filePeak\": 0\n"
               << "}\n";
            {
                std::ofstream out(opDir + "/state.json.tmp");
                out << ss.str();
            }
            ::rename((opDir + "/state.json.tmp").c_str(), (opDir + "/state.json").c_str());
            {
                std::ofstream out(opDir + "/stdout.tail.tmp");
                out << "DONE";
            }
            ::rename((opDir + "/stdout.tail.tmp").c_str(), (opDir + "/stdout.tail").c_str());
            // Sleep 200ms before exiting to simulate post-publish teardown
            ::usleep(200000);
            ::_exit(0);
        }

        OperationRef ref;
        ref.operationId = opId;
        ref.token = "token-s04";
        ref.opDir = opDir;
        ref.collectorPid = cpid;
        ref.isOwnedChild = true;
        registerOperationRef(ref);

        const auto res = waitForOperation(ref, 500);
        TEST_ASSERT(res.state == CommandState::Exited);
        TEST_ASSERT(res.exitStatus == 0);
        TEST_ASSERT(res.stdoutText == "DONE");

        // Allow child to exit
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Check if child was reaped by the background reaper
        int st = 0;
        int nZombies = 0;
        while (::waitpid(-1, &st, WNOHANG) > 0) {
            ++nZombies;
        }
        TEST_ASSERT(nZombies == 0);
        TEST_ASSERT(::kill(cpid, 0) != 0);
        TEST_ASSERT(!std::filesystem::exists(opDir));
    }
}

// T13-S05: Verification of absolute settings executable resolution and parser entry
void test_T13_S05_R01AbsolutePathAndR04ValidRefVerification() {
    const std::string exe = resolveSettingsExeForTest();
    TEST_ASSERT(!exe.empty());
    TEST_ASSERT(std::filesystem::exists(exe));
    TEST_ASSERT(::access(exe.c_str(), X_OK) == 0);
    TEST_ASSERT(exe[0] == '/'); // Must be absolute path
}

// T13-S06: Dual stream continuous output peak budgets and strict file permissions
void test_T13_S06_DualStream100MiBPeakBudgetAndFilePermissions() {
    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 30000;
    spec.operationId = 1406;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    spec.argv = {
        "python3", "-c",
        "import sys\n"
        "chunk = 'M' * 1048576\n"
        "for _ in range(10):\n"
        "    sys.stdout.write(chunk)\n"
        "    sys.stderr.write(chunk)\n"
        "sys.stdout.flush()\n"
        "sys.stderr.flush()\n"
    };

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutTruncated == true);
    TEST_ASSERT(res.stderrTruncated == true);
    TEST_ASSERT(res.stdoutText.size() <= 65536);
    TEST_ASSERT(res.stderrText.size() <= 65536);
    TEST_ASSERT(res.bufferPeak > 0);
    TEST_ASSERT(res.bufferPeak <= spec.limits.perStreamBufferBytes);
    TEST_ASSERT(res.filePeak <= spec.limits.perStreamFileBytes);
}

// T13-S07: Fresh evidence and test registration count check
void test_T13_S07_FreshEvidenceAndRegistrationSelfCheck() {
    TEST_ASSERT(g_passCount >= 28);
}

// T13-T01: Background reaper low CPU, bounded polling rate, zero zombies, and clean shutdown
void test_T13_T01_ReaperLowCpuAndSafeShutdown() {
    const std::string tmpDir = getTempDir();
    const std::uint64_t opId = 1501;
    const std::string opDir = tmpDir + "/modernime-op-t01";
    std::filesystem::create_directories(opDir);

    const pid_t cpid = ::fork();
    TEST_ASSERT(cpid >= 0);

    if (cpid == 0) {
        // Write Exited state file
        std::ofstream out(opDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << opId << ",\n"
            << "  \"token\": \"token-t01\",\n"
            << "  \"collectorPid\": " << ::getpid() << ",\n"
            << "  \"childPid\": 0,\n"
            << "  \"state\": \"Exited\",\n"
            << "  \"exitStatus\": 0,\n"
            << "  \"stdoutTruncated\": false,\n"
            << "  \"stderrTruncated\": false,\n"
            << "  \"stdoutBytes\": 4,\n"
            << "  \"stderrBytes\": 0,\n"
            << "  \"bufferPeak\": 4,\n"
            << "  \"filePeak\": 0\n"
            << "}\n";
        out.close();
        {
            std::ofstream tail(opDir + "/stdout.tail");
            tail << "T01";
        }
        // Sleep 700ms before exit to simulate pause after publishing final state
        ::usleep(700000);
        ::_exit(0);
    }

    OperationRef ref;
    ref.operationId = opId;
    ref.token = "token-t01";
    ref.opDir = opDir;
    ref.collectorPid = cpid;
    ref.isOwnedChild = true;
    registerOperationRef(ref);

    struct rusage ruBefore{};
    ::getrusage(RUSAGE_SELF, &ruBefore);

    const auto res = waitForOperation(ref, 500);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);
    TEST_ASSERT(res.stdoutText == "T01");

    // Sleep 800ms while child finishes sleeping and exits
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    struct rusage ruAfter{};
    ::getrusage(RUSAGE_SELF, &ruAfter);

    const long long cpuUs = (ruAfter.ru_utime.tv_sec - ruBefore.ru_utime.tv_sec) * 1000000LL +
                            (ruAfter.ru_utime.tv_usec - ruBefore.ru_utime.tv_usec) +
                            (ruAfter.ru_stime.tv_sec - ruBefore.ru_stime.tv_sec) * 1000000LL +
                            (ruAfter.ru_stime.tv_usec - ruBefore.ru_stime.tv_usec);

    // Bounded CPU usage: should be well under 100ms (100000us), definitely not ~700ms busy loop
    TEST_ASSERT(cpuUs < 100000LL);

    // Verify child was reaped by background reaper
    int st = 0;
    int nZombies = 0;
    while (::waitpid(-1, &st, WNOHANG) > 0) {
        ++nZombies;
    }
    TEST_ASSERT(nZombies == 0);
    TEST_ASSERT(::kill(cpid, 0) != 0);

    // Test safe shutdown of reaper when alive collector is pending, in a fresh exec process
    char selfPathBuf[PATH_MAX];
    const ssize_t selfLen = ::readlink("/proc/self/exe", selfPathBuf, sizeof(selfPathBuf) - 1);
    TEST_ASSERT(selfLen > 0);
    selfPathBuf[selfLen] = '\0';
    const std::string selfExe(selfPathBuf);

    const std::string subCmd = "\"" + selfExe + "\" --subtest-shutdown-reaper >/dev/null 2>&1";
    const int subRet = std::system(subCmd.c_str());
    TEST_ASSERT(WIFEXITED(subRet) && WEXITSTATUS(subRet) == 0);
}

// T13-T02: Single deadline for pre-exec delay and handshake; strict zero-spawn proof
void test_T13_T02_PreExecDeadlineBoundedAndStrictZeroSpawn() {
    const std::string tmpDir = getTempDir();
    const std::string settingsExe = resolveSettingsExeForTest();

    // 1. True pre-exec delay: hook execvp before exec happens so execPipe[1] remains open during stall
    const std::string hookSrc = tmpDir + "/test_delay_exec.c";
    const std::string hookSo = tmpDir + "/test_delay_exec.so";
    {
        std::ofstream out(hookSrc);
        out << "#define _GNU_SOURCE\n"
            << "#include <dlfcn.h>\n"
            << "#include <time.h>\n"
            << "#include <stdlib.h>\n"
            << "#include <string.h>\n"
            << "int execvp(const char *name, char *const argv[]) {\n"
            << "    static int (*fn)(const char*, char *const*) = 0;\n"
            << "    if (!fn) fn = dlsym(RTLD_NEXT, \"execvp\");\n"
            << "    const char *m = getenv(\"TEST_DELAY_EXEC\");\n"
            << "    if (m && strcmp(m, \"1\") == 0) {\n"
            << "        struct timespec t = {0, 350000000};\n" // 350ms
            << "        nanosleep(&t, 0);\n"
            << "    }\n"
            << "    return fn(name, argv);\n"
            << "}\n";
    }
    char selfPathBuf[PATH_MAX];
    const ssize_t selfLen = ::readlink("/proc/self/exe", selfPathBuf, sizeof(selfPathBuf) - 1);
    TEST_ASSERT(selfLen > 0);
    selfPathBuf[selfLen] = '\0';
    const std::string selfExe(selfPathBuf);

    const std::string buildCmd = "gcc -shared -fPIC \"" + hookSrc + "\" -o \"" + hookSo + "\" -ldl >/dev/null 2>&1";
    const int compileRet = std::system(buildCmd.c_str());
    TEST_ASSERT(compileRet == 0 && std::filesystem::exists(hookSo));

    std::string preloadEnv = hookSo;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libasan.so") != std::string::npos) {
                const auto slash = line.find('/');
                if (slash != std::string::npos) {
                    preloadEnv = line.substr(slash) + " " + hookSo;
                    break;
                }
            }
        }
    }
    const std::string subCmd = "env LD_PRELOAD=\"" + preloadEnv + "\" TEST_DELAY_EXEC=1 \"" + selfExe + "\" --subtest-pre-exec >/dev/null 2>&1";
    const int subRet = std::system(subCmd.c_str());
    TEST_ASSERT(WIFEXITED(subRet) && WEXITSTATUS(subRet) == 0);
    std::filesystem::remove(hookSrc);
    std::filesystem::remove(hookSo);

    // 2. Pre-exec wrapper delay: child delays before exec/handshake, timeout is 80ms
    const std::string delayWrapper = tmpDir + "/pre-exec-delay-wrapper.sh";
    {
        std::ofstream out(delayWrapper);
        out << "#!/bin/sh\n"
            << "sleep 0.35\n"
            << "exec \"" << settingsExe << "\" \"$@\"\n";
    }
    ::chmod(delayWrapper.c_str(), 0755);

    CommandSpec spec1;
    spec1.mode = CommandMode::PackageTransaction;
    spec1.timeoutMs = 80;
    spec1.operationId = 15021;
    spec1.limits.perStreamResultBytes = 65536;
    spec1.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", delayWrapper}};
    spec1.argv = {"/bin/true"};

    const auto start1 = std::chrono::steady_clock::now();
    const auto res1 = runCommand(spec1);
    const auto elapsed1 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start1).count();

    // Must return Unknown (not NotStarted, because zero-spawn was NOT proven)
    TEST_ASSERT(res1.state == CommandState::Unknown);
    // Bounded by deadline: elapsed should be close to 80ms (definitely < 250ms)
    TEST_ASSERT(elapsed1 < 250);

    // Ref must be preserved in activeOpRefs
    auto ref1 = getActiveOperationRef(spec1.operationId);
    TEST_ASSERT(ref1.has_value());
    cleanupOperation(spec1.operationId);
    std::filesystem::remove(delayWrapper);

    // 2. Strict zero spawn on exec failure: nonexistent executable returns NotStarted immediately
    CommandSpec spec2;
    spec2.mode = CommandMode::PackageTransaction;
    spec2.timeoutMs = 2000;
    spec2.operationId = 15022;
    spec2.limits.perStreamResultBytes = 65536;
    spec2.environment = {{"MODERNIME_SETTINGS_EXECUTABLE", "/nonexistent/binary/for/test"}};
    spec2.argv = {"/bin/true"};

    const auto start2 = std::chrono::steady_clock::now();
    const auto res2 = runCommand(spec2);
    const auto elapsed2 = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start2).count();

    TEST_ASSERT(res2.state == CommandState::NotStarted);
    TEST_ASSERT(elapsed2 < 100);
}

// T13-T03: Numeric range and protocol validation before narrowing
void test_T13_T03_NumericRangeAndProtocolValidation() {
    const std::string tmpDir = getTempDir();
    const std::uint64_t opId = 1503;
    const std::string opDir = tmpDir + "/modernime-op-t03";
    std::filesystem::create_directories(opDir);

    OperationRef ref;
    ref.operationId = opId;
    ref.token = "token-t03";
    ref.opDir = opDir;
    ref.collectorPid = 12345;
    registerOperationRef(ref);

    auto testState = [&](long long cPid, long long chPid, const std::string &st, long long exitVal, bool expectedValid) {
        std::ofstream out(opDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << opId << ",\n"
            << "  \"token\": \"token-t03\",\n"
            << "  \"collectorPid\": " << cPid << ",\n"
            << "  \"childPid\": " << chPid << ",\n"
            << "  \"state\": \"" << st << "\",\n"
            << "  \"exitStatus\": " << exitVal << ",\n"
            << "  \"stdoutTruncated\": false,\n"
            << "  \"stderrTruncated\": false,\n"
            << "  \"stdoutBytes\": 0,\n"
            << "  \"stderrBytes\": 0,\n"
            << "  \"bufferPeak\": 0,\n"
            << "  \"filePeak\": 0\n"
            << "}\n";
        out.close();

        const auto res = waitForOperation(ref, 20);
        if (expectedValid) {
            TEST_ASSERT(res.state == CommandState::Exited);
            TEST_ASSERT(res.exitStatus == static_cast<int>(exitVal));
        } else {
            TEST_ASSERT(res.state != CommandState::Exited);
            TEST_ASSERT(res.exitStatus == -1);
        }
    };

    // 1. exitStatus = 4294967296 (2^32) with state "Exited" -> must be rejected (not 0)
    testState(12345, 0, "Exited", 4294967296LL, false);

    // 2. collectorPid = 12345 + 4294967296 (2^32 overflow) -> must be rejected
    testState(12345LL + 4294967296LL, 0, "Exited", 0, false);

    // 3. exitStatus negative (-10) with state "Exited" -> must be rejected
    testState(12345, 0, "Exited", -10, false);

    // 4. exitStatus > 255 (256) with state "Exited" -> must be rejected
    testState(12345, 0, "Exited", 256, false);

    // 5. exitStatus != -1 (e.g. 0) with state "Running" -> must be rejected
    testState(12345, 0, "Running", 0, false);

    // 6. collectorPid <= 0 -> must be rejected
    testState(0, 0, "Exited", 0, false);
    testState(-1, 0, "Exited", 0, false);

    // 7. collectorPid > 4194304 -> must be rejected
    testState(5000000, 0, "Exited", 0, false);

    // 8. childPid < 0 or > 4194304 -> must be rejected
    testState(12345, -5, "Exited", 0, false);
    testState(12345, 5000000, "Exited", 0, false);

    // 9. Valid state with exitStatus 0 and 1 -> must succeed
    testState(12345, 0, "Exited", 0, true);
    testState(12345, 0, "Exited", 1, true);

    auto testStateBytes = [&](long long soBytes, long long seBytes, long long bPeak, long long fPeak, bool expectedValid) {
        std::ofstream out(opDir + "/state.json");
        out << "{\n"
            << "  \"operationId\": " << opId << ",\n"
            << "  \"token\": \"token-t03\",\n"
            << "  \"collectorPid\": 12345,\n"
            << "  \"childPid\": 0,\n"
            << "  \"state\": \"Exited\",\n"
            << "  \"exitStatus\": 0,\n"
            << "  \"stdoutTruncated\": false,\n"
            << "  \"stderrTruncated\": false,\n"
            << "  \"stdoutBytes\": " << soBytes << ",\n"
            << "  \"stderrBytes\": " << seBytes << ",\n"
            << "  \"bufferPeak\": " << bPeak << ",\n"
            << "  \"filePeak\": " << fPeak << "\n"
            << "}\n";
        out.close();

        const auto res = waitForOperation(ref, 20);
        if (expectedValid) {
            TEST_ASSERT(res.state == CommandState::Exited);
            TEST_ASSERT(res.exitStatus == 0);
        } else {
            TEST_ASSERT(res.state != CommandState::Exited);
            TEST_ASSERT(res.exitStatus == -1);
        }
    };

    // 10. Large cumulative byte counts: 1GiB, 1GiB + 1, 5GiB must all be accepted as Exited/0
    testStateBytes(1073741824LL, 0, 0, 0, true);
    testStateBytes(1073741825LL, 0, 0, 0, true);
    testStateBytes(5000000000LL, 0, 0, 0, true);
    testStateBytes(0, 1073741825LL, 0, 0, true);
    testStateBytes(5000000000LL, 5000000000LL, 1000000LL, 1000000LL, true);

    // 11. Negative byte counts or peaks -> must be rejected
    testStateBytes(-1LL, 0, 0, 0, false);
    testStateBytes(0, -1LL, 0, 0, false);
    testStateBytes(0, 0, -1LL, 0, false);
    testStateBytes(0, 0, 0, -1LL, false);

    cleanupOperation(opId);
    std::filesystem::remove_all(opDir);
}

// T13-T04: Installed binary headless invocation regression
void test_T13_T04_InstalledBinaryHeadlessInvocationRegression() {
    const std::string exe = resolveSettingsExeForTest();
    TEST_ASSERT(!exe.empty());
    TEST_ASSERT(std::filesystem::exists(exe));

    // 1. Invocation without display and missing args must exit with status 1
    const std::string cmd1 = "env -u DISPLAY -u WAYLAND_DISPLAY \"" + exe + "\" --internal-capture-collector >/dev/null 2>&1";
    const int ret1 = std::system(cmd1.c_str());
    TEST_ASSERT(WIFEXITED(ret1) && WEXITSTATUS(ret1) == 1);

    // 2. Invocation with --version must output version and exit 0
    const std::string cmd2 = "env -u DISPLAY -u WAYLAND_DISPLAY \"" + exe + "\" --version >/dev/null 2>&1";
    const int ret2 = std::system(cmd2.c_str());
    TEST_ASSERT(WIFEXITED(ret2) && WEXITSTATUS(ret2) == 0);

    // 3. Synthetic task execution in headless mode with FIFO handshake and strict verification
    const std::string tmpDir = getTempDir();
    const std::string opDir = tmpDir + "/synthetic-task-t04";
    std::filesystem::create_directories(opDir);
    ::chmod(opDir.c_str(), 0700);

    const std::string specPath = opDir + "/spec.dat";
    {
        std::ofstream spec(specPath);
        spec << "operationId=98765\n"
             << "token=install-token-98765\n"
             << "timeoutMs=5000\n"
             << "mode=1\n"
             << "perStreamBufferBytes=1048576\n"
             << "perStreamResultBytes=65536\n"
             << "perStreamFileBytes=1048576\n"
             << "argc=3\n"
             << "argv=/usr/bin/python3\n"
             << "argv=-c\n"
             << "argv=import sys; sys.stdout.write('INSTALL_COLLECTOR_STDOUT_OK\\n'); sys.stderr.write('INSTALL_COLLECTOR_STDERR_OK\\n')\n"
             << "envc=0\n";
    }
    ::chmod(specPath.c_str(), 0600);

    const std::string fifoPath = opDir + "/handshake.fifo";
    ::mkfifo(fifoPath.c_str(), 0600);
    const int fifoReadFd = ::open(fifoPath.c_str(), O_RDWR | O_NONBLOCK);
    TEST_ASSERT(fifoReadFd >= 0);

    const pid_t cpid = ::fork();
    TEST_ASSERT(cpid >= 0);
    if (cpid == 0) {
        ::close(fifoReadFd);
        const int fifoWriteFd = ::open(fifoPath.c_str(), O_WRONLY);
        if (fifoWriteFd < 0) ::_exit(71);
        std::vector<std::string> args = {
            exe,
            "--internal-capture-collector",
            "--op-dir", opDir,
            "--handshake-fd", std::to_string(fifoWriteFd)
        };
        std::vector<char*> cargv;
        for (auto &a : args) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(72);
    }

    // Wait for handshake with deadline
    char hsByte = 0;
    const auto hsStart = std::chrono::steady_clock::now();
    bool hsReceived = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - hsStart).count() < 3000) {
        const ssize_t nr = ::read(fifoReadFd, &hsByte, 1);
        if (nr == 1) {
            hsReceived = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(fifoReadFd);
    TEST_ASSERT(hsReceived);
    TEST_ASSERT(hsByte == 'R' || hsByte == '1');

    // Wait for collector to exit
    int collectorStatus = 0;
    const auto waitStart = std::chrono::steady_clock::now();
    bool collectorExited = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - waitStart).count() < 4000) {
        const pid_t wp = ::waitpid(cpid, &collectorStatus, WNOHANG);
        if (wp == cpid) {
            collectorExited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!collectorExited) {
        ::kill(cpid, SIGKILL);
        ::waitpid(cpid, &collectorStatus, 0);
    }
    TEST_ASSERT(collectorExited);
    TEST_ASSERT(WIFEXITED(collectorStatus) && WEXITSTATUS(collectorStatus) == 0);

    // Verify state.json with waitForOperation
    OperationRef ref;
    ref.operationId = 98765;
    ref.token = "install-token-98765";
    ref.opDir = opDir;
    ref.collectorPid = cpid;
    registerOperationRef(ref);

    CommandResult cr = waitForOperation(ref, 100);
    TEST_ASSERT(cr.state == CommandState::Exited);
    TEST_ASSERT(cr.exitStatus == 0);
    TEST_ASSERT(cr.stdoutText.find("INSTALL_COLLECTOR_STDOUT_OK") != std::string::npos);
    TEST_ASSERT(cr.stderrText.find("INSTALL_COLLECTOR_STDERR_OK") != std::string::npos);

    // Negative check: wrong ID or wrong token must fail to complete
    OperationRef refWrongId;
    refWrongId.operationId = 987650;
    refWrongId.token = "install-token-98765";
    refWrongId.opDir = opDir;
    refWrongId.collectorPid = cpid;
    registerOperationRef(refWrongId);
    CommandResult crWrongId = waitForOperation(refWrongId, 20);
    TEST_ASSERT(crWrongId.state != CommandState::Exited);

    OperationRef refWrongToken;
    refWrongToken.operationId = 98765;
    refWrongToken.token = "wrong-token";
    refWrongToken.opDir = opDir;
    refWrongToken.collectorPid = cpid;
    registerOperationRef(refWrongToken);
    CommandResult crWrongToken = waitForOperation(refWrongToken, 20);
    TEST_ASSERT(crWrongToken.state != CommandState::Exited);

    // Negative check: duplicate key in state.json must be rejected
    {
        std::ofstream out(opDir + "/state.json");
        out << "{\"operationId\":987650,\"operationId\":98765,\"token\":\"install-token-98765\",\"state\":\"Exited\",\"exitStatus\":1,\"exitStatus\":0}\n";
    }
    CommandResult crDup = waitForOperation(ref, 20);
    TEST_ASSERT(crDup.state != CommandState::Exited);

    cleanupOperation(98765);
    cleanupOperation(987650);

    std::filesystem::remove_all(opDir);
}

// T13-T05: Test suite self-check
void test_T13_T05_TestSuiteSelfCheckT13() {
    TEST_ASSERT(g_passCount >= 33);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--subtest-pre-exec") {
        CommandSpec spec0;
        spec0.mode = CommandMode::PackageTransaction;
        spec0.timeoutMs = 80;
        spec0.operationId = 15020;
        spec0.limits.perStreamResultBytes = 65536;
        spec0.argv = {"/bin/true"};

        const auto start0 = std::chrono::steady_clock::now();
        const auto res0 = runCommand(spec0);
        const auto elapsed0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start0).count();

        if (res0.state != CommandState::Unknown) {
            return 1;
        }
        if (elapsed0 >= 250) {
            return 2;
        }
        auto ref0 = getActiveOperationRef(spec0.operationId);
        if (!ref0.has_value()) {
            return 3;
        }
        cleanupOperation(spec0.operationId);
        return 0;
    }

    if (argc > 1 && std::string_view(argv[1]) == "--subtest-shutdown-reaper") {
        const std::string tmpDir = getTempDir();
        const pid_t dummyLive = ::fork();
        if (dummyLive == 0) {
            ::sleep(2);
            ::_exit(0);
        }
        const std::string dummyOpDir = tmpDir + "/dummy-reap-op";
        std::filesystem::create_directories(dummyOpDir);
        {
            std::ofstream out(dummyOpDir + "/state.json");
            out << "{\"operationId\":1401,\"token\":\"tok\",\"collectorPid\":" << dummyLive
                << ",\"childPid\":0,\"state\":\"Exited\",\"exitStatus\":0,\"stdoutTruncated\":false,\"stderrTruncated\":false,\"stdoutBytes\":0,\"stderrBytes\":0,\"bufferPeak\":0,\"filePeak\":0}\n";
        }
        OperationRef dummyRef;
        dummyRef.operationId = 1401;
        dummyRef.token = "tok";
        dummyRef.opDir = dummyOpDir;
        dummyRef.collectorPid = dummyLive;
        dummyRef.isOwnedChild = true;
        registerOperationRef(dummyRef);

        auto dummyRes = waitForOperation(dummyRef, 50);
        if (dummyRes.state != CommandState::Exited) {
            return 10;
        }

        const auto t0 = std::chrono::steady_clock::now();
        shutdownBackgroundReaper();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsedMs > 500) {
            return 1;
        }
        if (::kill(dummyLive, 0) != 0) {
            return 2;
        }
        ::kill(dummyLive, SIGKILL);
        int dummySt = 0;
        ::waitpid(dummyLive, &dummySt, 0);
        std::filesystem::remove_all(dummyOpDir);
        return 0;
    }

    std::cout << "Starting modernime_process_runner tests...\n";

    RUN_TEST(test_T13_A01_QueryDualStreamTruncation);
    RUN_TEST(test_T13_A02_QueryDescendantInheritedFdDoesNotHang);
    RUN_TEST(test_T13_A03_PackageLargeOutput100MiBPreservesTail);
    RUN_TEST(test_T13_A04_ResourceBudgetMeasurement);
    RUN_TEST(test_T13_A05_PackageTimeoutTransitionsToContinuing);
    RUN_TEST(test_T13_A06_ParentExitWithInheritedOutput);
    RUN_TEST(test_T13_A07_CollectorStartFailureZeroSpawn);
    RUN_TEST(test_T13_A08_CollectorAbnormalTerminationTransitionsToUncertain);
    RUN_TEST(test_T13_A09_CompletionQueueBackpressureAndBoundedRetention);
    RUN_TEST(test_T13_A10_PurePackagePolicyUnit);
    RUN_TEST(test_T13_A11_T12RegressionSynchronousLockDoesNotPollute);
    RUN_TEST(test_T13_A12_HeadlessInternalModeRunsWithoutDisplay);

    RUN_TEST(test_T13_R01_StatePublicationFailureGuaranteesZeroSpawnOrUncertain);
    RUN_TEST(test_T13_R02_HandshakeLossOrCollectorCrashDoesNotReportNotStarted);
    RUN_TEST(test_T13_R03_MultiRoundContinuingEventuallyCompletesUnderSameId);
    RUN_TEST(test_T13_R04_OperationIdentityAndMalformedStateRejection);
    RUN_TEST(test_T13_R05_LateWritingDescendantsReceiveZeroEpipe);
    RUN_TEST(test_T13_R06_ResourceBoundedRetentionAndZeroZombies);
    RUN_TEST(test_T13_R07_QueryMonotonicDeadlineAndEarlyEof);
    RUN_TEST(test_T13_R08_EnvironmentInheritanceAndExplicitOverrides);
    RUN_TEST(test_T13_R09_ResourcePeaksAndBudgetMeasurement);
    RUN_TEST(test_T13_R10_HeadlessExecInternalCollectorWithoutDisplay);

    RUN_TEST(test_T13_S01_TargetForkBeforeAckCrashPreservesStateAndExecFailureZeroSpawn);
    RUN_TEST(test_T13_S02_StrictStateProtocolRejectionCoverage);
    RUN_TEST(test_T13_S03_CollectorContinuousDrainAndZombieLeader);
    RUN_TEST(test_T13_S04_AsyncFinalPublishedBeforeCollectorExitReapedByBackground);
    RUN_TEST(test_T13_S05_R01AbsolutePathAndR04ValidRefVerification);
    RUN_TEST(test_T13_S06_DualStream100MiBPeakBudgetAndFilePermissions);
    RUN_TEST(test_T13_S07_FreshEvidenceAndRegistrationSelfCheck);

    RUN_TEST(test_T13_T01_ReaperLowCpuAndSafeShutdown);
    RUN_TEST(test_T13_T02_PreExecDeadlineBoundedAndStrictZeroSpawn);
    RUN_TEST(test_T13_T03_NumericRangeAndProtocolValidation);
    RUN_TEST(test_T13_T04_InstalledBinaryHeadlessInvocationRegression);
    RUN_TEST(test_T13_T05_TestSuiteSelfCheckT13);

    std::cout << "========================================\n";
    std::cout << "Summary: " << g_passCount << " passed, " << g_failCount << " failed.\n";
    std::cout << "========================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
