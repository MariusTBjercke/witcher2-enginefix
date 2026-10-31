// Shared helpers for the script-patch code (src/sp_*.c). Compiled into both the DLL and
// the offline test harness; nothing here touches the game process.
#ifndef SP_COMMON_H
#define SP_COMMON_H

#include <stddef.h>
#include <wchar.h>

// Owned byte buffer; free with sp_free. p == NULL is valid and means empty.
typedef struct { unsigned char* p; size_t n; } sp_buf;

// Log sink for the overlay build: the DLL passes logf_, the harness passes printf.
typedef void (*sp_log_fn)(const char* fmt, ...);

void sp_free(sp_buf* b);

// 1 = ok. On failure `out` is left empty (p == NULL, n == 0).
int sp_read_file(const wchar_t* path, sp_buf* out);

// Writes through "<path>.tmp" and MoveFileExW, so a half-written file never appears
// under the final name. 1 = ok.
int sp_write_file_atomic(const wchar_t* path, const sp_buf* b);

#endif
