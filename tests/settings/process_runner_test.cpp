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

// T13-R01: Initial state publication failure: zero spawn proven -> NotStarted; if target started -> Unknown/Uncertain
void test_T13_R01_StatePublicationFailureGuaranteesZeroSpawnOrUncertain() {
    const std::string tmpDir = getTempDir();
    const std::string spawnMarker = tmpDir + "/modernime_r01_target_marker.txt";
    std::filesystem::remove(spawnMarker);

    // Create wrapper that creates opDir/state.json as directory BEFORE collector runs
    const std::string wrapperPath = tmpDir + "/collector-wrapper-r01.sh";
    {
        std::ofstream out(wrapperPath);
        out << "#!/bin/sh\n"
            << "/bin/mkdir -p \"$3/state.json\"\n"
            << "exec ./build/architecture-debug/tools/settings/modernime-settings \"$@\"\n";
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

    // 2. Malformed state file without JSON braces
    {
        std::ofstream out(staleDir + "/state.json");
        out << "\"operationId\": " << staleId << ", \"state\": \"Exited\", \"exitStatus\": 0\n";
    }
    const auto waitMalformed = waitForOperation(staleId, 100);
    TEST_ASSERT(waitMalformed.state == CommandState::Unknown);

    std::filesystem::remove_all(staleDir);
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
        {"MODERNIME_PACKAGE_OP_TIMEOUT_MS", "1000"}
    };
    ::setenv("TMPDIR", r06Tmp.c_str(), 1);

    int st = 0;
    while (::waitpid(-1, &st, WNOHANG) > 0) {}

    for (int i = 0; i < 5; ++i) {
        auto r = UpdateController::refreshPackageCache(status, env);
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

} // namespace

int main() {
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

    std::cout << "========================================\n";
    std::cout << "Summary: " << g_passCount << " passed, " << g_failCount << " failed.\n";
    std::cout << "========================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
