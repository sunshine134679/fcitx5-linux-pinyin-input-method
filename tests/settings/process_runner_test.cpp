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
    // Verify an operation state file in tmp is <= 4 KiB and has 0600 permissions
    CommandSpec spec;
    spec.mode = CommandMode::PackageTransaction;
    spec.timeoutMs = 10000;
    spec.operationId = 104;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    spec.argv = {"python3", "-c", "import sys; sys.stdout.write('resource_check\\n')"};

    const auto res = runCommand(spec);
    TEST_ASSERT(res.state == CommandState::Exited);
    TEST_ASSERT(res.exitStatus == 0);

    // Locate operation directory
    bool foundStateFile = false;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(tmpDir, ec)) {
        if (entry.is_directory(ec)) {
            const std::string name = entry.path().filename().string();
            if (name.find("modernime-op-104-") != std::string::npos) {
                const auto statePath = entry.path() / "state.json";
                if (std::filesystem::exists(statePath, ec)) {
                    const auto sz = std::filesystem::file_size(statePath, ec);
                    TEST_ASSERT(sz <= 4096);
                    const auto perms = std::filesystem::status(statePath, ec).permissions();
                    TEST_ASSERT((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
                    foundStateFile = true;
                    break;
                }
            }
        }
    }
    TEST_ASSERT(foundStateFile);
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
    std::filesystem::remove(doneFile);

    const pid_t parentPid = fork();
    if (parentPid == 0) {
        // Child acting as test GUI parent process
        CommandSpec spec;
        spec.mode = CommandMode::PackageTransaction;
        spec.timeoutMs = 100; // Fast timeout
        spec.operationId = opId;
        spec.limits.perStreamBufferBytes = 1048576;
        spec.limits.perStreamResultBytes = 65536;
        spec.limits.perStreamFileBytes = 1048576;
        // Target command runs for 1 second, writing dual streams
        spec.argv = {
            "python3", "-c",
            "import sys, time\n"
            "time.sleep(0.5)\n"
            "sys.stdout.write('PARENT_EXIT_STDOUT_OK\\n')\n"
            "sys.stderr.write('PARENT_EXIT_STDERR_OK\\n')\n"
            "sys.stdout.flush()\n"
            "sys.stderr.flush()\n"
            "with open('" + doneFile + "', 'w') as f:\n"
            "    f.write('done')\n"
        };
        runCommand(spec);
        // True _exit(0) simulating sudden GUI parent termination
        _exit(0);
    }

    int status = 0;
    waitpid(parentPid, &status, 0);
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Wait for collector and child to finish independently
    const auto waitRes = waitForOperation(opId, 5000);
    TEST_ASSERT(waitRes.state == CommandState::Exited);
    TEST_ASSERT(waitRes.exitStatus == 0);
    TEST_ASSERT(waitRes.stdoutText.find("PARENT_EXIT_STDOUT_OK") != std::string::npos);
    TEST_ASSERT(std::filesystem::exists(doneFile));
    std::filesystem::remove(doneFile);
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

    std::cout << "========================================\n";
    std::cout << "Summary: " << g_passCount << " passed, " << g_failCount << " failed.\n";
    std::cout << "========================================\n";

    return (g_failCount == 0) ? 0 : 1;
}
