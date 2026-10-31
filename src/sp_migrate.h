// One-time cleanup of the 0.4/0.5 on-disk script patches (plan Task 8, owner decision 3).
// Port of the Uninstall side of tools/script-patches/ScriptPatches.ps1 (Read-Manifest,
// Restore-Leaf): every file is put back from the old patcher's backup only while it still
// holds exactly what that patcher wrote, so another mod's later change is never undone.
// The DLL runs it once before its own overlay is built; the old patcher's files are the only
// game files the DLL ever writes.
#ifndef SP_MIGRATE_H
#define SP_MIGRATE_H

#include "sp_common.h"

// Lowercase hex SHA-256 of `b` (the format of the old manifest). 1 = ok.
int sp_sha256_hex(const sp_buf* b, char out[65]);

#define SP_MIGRATE_FAILED  (-1)  // I/O error: patches off, backup kept so the next start retries
#define SP_MIGRATE_NONE      0   // no manifest: nothing installed by the old patcher
#define SP_MIGRATE_DONE      1   // everything restored (or already was); backup renamed
#define SP_MIGRATE_PARTIAL   2   // a file changed since patching or an unknown leaf: left as
                                 // it is, backup renamed, patches off for this start

// `game` is the game root, `backup_dir` the old patcher's backup folder
// (<game>\Witcher2EngineFix_Optional\ScriptPatches\backup in a real install). When the
// manifest is handled, `backup_dir` is renamed to "<backup_dir>.restored-by-dll" (or a free
// numbered variant), so this runs once.
int sp_migrate_run(const wchar_t* game, const wchar_t* backup_dir, sp_log_fn log);

#endif
