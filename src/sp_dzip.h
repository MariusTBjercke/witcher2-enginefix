// DZIP v2 reader/writer. The format is what Gibbed.RED writes and what the game mounts:
// a 32-byte header, entry data made of independently compressed 64 KiB LZF blocks behind a
// per-entry block-offset table, the entry table at the end, and in the header a FNV-1a 64
// variant over the entry table at +24 and the table offset at +16.
//
// Port of the Dzip class in ScriptPatches.ps1 (git tag v0.5.1), proven
// equal by the golden test before the removal.
#ifndef SP_DZIP_H
#define SP_DZIP_H

#include <windows.h>
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

// Like sp_dzip_find, but the last entry of that name: the one the game's script compiler
// uses when an archive holds a name twice (0041495C keeps the later index).
int sp_dzip_find_last(const sp_dzip* z, const char* name);

// How many entries carry that name, case-insensitive.
int sp_dzip_count_name(const sp_dzip* z, const char* name);

// 1 = ok. LZF, 64 KiB blocks; fails on a corrupt stream or an entry outside `data`.
int sp_dzip_extract(const sp_dzip* z, int i, sp_buf* out);

// Decompresses only block k (the 64 KiB of output at k * 0x10000) of entry i into dst,
// which must hold 0x10000 bytes. Blocks are compressed independently. Returns the bytes
// produced (the last block is shorter; fewer means a corrupt stream), -1 on a bad table.
long long sp_dzip_block(const sp_dzip* z, int i, long long k, unsigned char* dst);

unsigned long long sp_dzip_hash(const sp_dzip_entry* e, int count);

// Replaces the entries named in `names` and adds the ones that do not exist yet (the new
// entries get `filetime` and go to the end of the table). A name held more than once is
// replaced in every entry that carries it. Every other entry keeps its compressed bytes and
// its place. The new table goes where the old one started.
int sp_dzip_write(const sp_dzip* z, int n, const char* const* names, const sp_buf* datas,
                  long long filetime, sp_buf* out);

void sp_dzip_close(sp_dzip* z);

// ---- file-backed access -------------------------------------------------
// For archives too large to load (pack0.dzip is ~10 GB): the 32-byte header and the entry
// table are read into `table` (so sp_dzip_find works), and one entry is extracted with
// reads from the file, a 64 KiB block at a time. The archive is opened for shared read and
// the handle stays open until sp_dzip_file_close. `path` is the .dzip file.
typedef struct {
    HANDLE    h;            // INVALID_HANDLE_VALUE unless open
    long long size;         // file size, for offset validation
    sp_buf    table;        // header (table offset rewritten to 32) + entry table
    sp_dzip   z;            // opened on `table`
} sp_dzip_file;

int  sp_dzip_file_open(const wchar_t* path, sp_dzip_file* out);
int  sp_dzip_file_extract(sp_dzip_file* f, int i, sp_buf* out);
void sp_dzip_file_close(sp_dzip_file* f);

#endif
