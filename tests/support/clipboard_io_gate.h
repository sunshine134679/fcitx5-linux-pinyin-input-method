#pragma once

// Test-only process-local gates. All paths must refer to synthetic fixtures.
extern "C" {
void modernime_clipboard_gate_arm(const char *path, int operation);
bool modernime_clipboard_gate_wait(unsigned timeoutMs);
void modernime_clipboard_gate_release();
void modernime_clipboard_fail_reads(const char *path, bool fail);
}
inline constexpr int clipboardAfterRead = 1;
inline constexpr int clipboardBeforeFsync = 2;
inline constexpr int clipboardAfterUnlock = 3;
