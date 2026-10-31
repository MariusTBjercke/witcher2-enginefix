// Shared helpers for the script-patch code; see sp_common.h.
#include "sp_common.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

void sp_free(sp_buf* b)
{
    if (!b) return;
    free(b->p);
    b->p = NULL;
    b->n = 0;
}

int sp_read_file(const wchar_t* path, sp_buf* out)
{
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char* p;
    size_t done = 0, total;

    memset(out, 0, sizeof *out);
    if (!path) return 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > (size_t)-1) {
        CloseHandle(h);
        return 0;
    }
    total = (size_t)size.QuadPart;
    p = (unsigned char*)malloc(total ? total : 1);
    if (!p) {
        CloseHandle(h);
        return 0;
    }
    while (done < total) {
        DWORD got = 0;
        if (!ReadFile(h, p + done, (DWORD)(total - done), &got, NULL) || got == 0) {
            free(p);
            CloseHandle(h);
            return 0;
        }
        done += got;
    }
    CloseHandle(h);
    out->p = p;
    out->n = total;
    return 1;
}

int sp_write_file_atomic(const wchar_t* path, const sp_buf* b)
{
    wchar_t tmp[MAX_PATH];
    HANDLE h;
    size_t done = 0;

    if (!path || !b || (b->n && !b->p)) return 0;
    if (_snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.tmp", path) < 0) return 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (done < b->n) {
        DWORD written = 0;
        if (!WriteFile(h, b->p + done, (DWORD)(b->n - done), &written, NULL) || written == 0) {
            CloseHandle(h);
            DeleteFileW(tmp);
            return 0;
        }
        done += written;
    }
    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        DeleteFileW(tmp);
        return 0;
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}
