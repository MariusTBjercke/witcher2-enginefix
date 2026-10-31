// DZIP v2 reader/writer. The format is what Gibbed.RED writes and what the game mounts:
// a 32-byte header, entry data made of independently compressed 64 KiB LZF blocks behind a
// per-entry block-offset table, the entry table at the end, and in the header a FNV-1a 64
// variant over the entry table at +24 and the table offset at +16.
//
// Port of the Dzip class in tools/script-patches/ScriptPatches.ps1; the reference is
// authoritative until the golden test (Task 6) proves byte equality.
#ifndef SP_DZIP_H
#define SP_DZIP_H

#include "sp_common.h"

typedef struct {
    char      name[260];        // as stored, without the trailing NUL, '\' separators
    long long time, usize, offset, csize;
} sp_dzip_entry;

typedef struct { sp_buf data; sp_dzip_entry* e; int count; long long table; } sp_dzip;

// Parses the header and the entry table and verifies the table hash. `data` is only
// borrowed: the caller keeps it alive and frees it, sp_dzip_close frees the entry array
// only. The entry data itself need not be inside `data` - a caller that read just the
// header and the table can still open a multi-GB archive; sp_dzip_extract needs it.
int sp_dzip_open(const sp_buf* data, sp_dzip* out);

// Index of the entry, case-insensitive, -1 when there is none.
int sp_dzip_find(const sp_dzip* z, const char* name);

// 1 = ok. LZF, 64 KiB blocks; fails on a corrupt stream or an entry outside `data`.
int sp_dzip_extract(const sp_dzip* z, int i, sp_buf* out);

unsigned long long sp_dzip_hash(const sp_dzip_entry* e, int count);

// Replaces the entries named in `names` and adds the ones that do not exist yet (the new
// entries get `filetime` and go to the end of the table). Every other entry keeps its
// compressed bytes and its place. The new table goes where the old one started.
int sp_dzip_write(const sp_dzip* z, int n, const char* const* names, const sp_buf* datas,
                  long long filetime, sp_buf* out);

void sp_dzip_close(sp_dzip* z);

#endif
