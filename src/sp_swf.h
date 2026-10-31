// SWF patching for the UI movies: CWS -> FWS, plus the same-length statement patches the
// reference applies. Port of the Swf class, $SwfCompare and $SwfFiles in
// tools/script-patches/ScriptPatches.ps1; that file stays authoritative.
#ifndef SP_SWF_H
#define SP_SWF_H

#include "sp_common.h"

// A byte pattern with -1 as wildcard; `orig` and `patched` have the same length (`len`).
typedef struct { const char* name; const short* orig; const short* patched; int len; } sp_swf_patch;

enum { SP_SWF_ORIGINAL, SP_SWF_PATCHED, SP_SWF_UNKNOWN };

// FWS copy; a CWS movie is inflated (two-byte zlib header skipped, raw deflate, as the
// reference does). 1 = ok.
int sp_swf_uncompressed(const sp_buf* in, sp_buf* out);

// ORIGINAL when every pattern is present exactly once and its patched form never, PATCHED
// when every patched form is present exactly once and the original never, UNKNOWN for
// anything else (also a mix, e.g. from an older copy of the patcher). On ORIGINAL,
// at[i] receives the offset of patch i's match; on UNKNOWN, why receives one line per
// patch. `at` and `why` may be NULL.
int sp_swf_state(const sp_buf* fws, const sp_swf_patch* p, int n, int* at, char* why, size_t whylen);

// Same-length patch of an ORIGINAL movie; a wildcard in `patched` keeps the byte. 1 = ok.
int sp_swf_apply(const sp_buf* fws, const sp_swf_patch* p, int n, sp_buf* out);

// $SwfCompare: the two "CompareInventories clears the tick" statements, for ui_trade.swf
// and ui_storage.swf. Each row is a same-length, stack-neutral no-op.
extern const sp_swf_patch SP_SWF_COMPARE[2];

#endif
