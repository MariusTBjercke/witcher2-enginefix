// WitcherScript edit engine: the C port of Add-Edit, ConvertTo-Tabs, Get-CollectBody and
// New-ReadIconFunction in ScriptPatches.ps1 (git tag v0.5.1), plus the
// edit table that used to live in its $FeatureList. The golden test proved the port byte for
// byte before the removal.
//
// The patch data (src/scriptpatches/edits.txt and the .ws fragments) is embedded as RCDATA
// by src/scriptpatches.rc, so the DLL and the test harness need no file next to them.
#ifndef SP_SCRIPT_H
#define SP_SCRIPT_H

#include "sp_common.h"

// The marker that wraps every inserted block, as the reference writes it.
extern const char SP_MARKER[];

// Where an edit puts its code; the names match the .ps1's Where values.
enum { SP_BEFORE, SP_AFTER, SP_BODY, SP_REPLACE, SP_END };

typedef struct {
    const char* feature;   // "BookMarker", "Continue", "PauseQuit"
    const char* entry;     // script entry name, e.g. "game\gui\guiutils.ws"
    int         where;     // SP_BEFORE .. SP_END
    int         count;     // the exact number of anchor matches; 0 for SP_END
    const char* anchor;    // as in the script: literal text, every whitespace run flexible
    const char* code;      // text to insert, or "@<fragment>" for one in src/scriptpatches/
} sp_edit;

// One script entry as Latin-1 text (the game's compiler treats it byte by byte). name is
// borrowed from the caller; text is owned and freed with sp_free.
typedef struct { const char* name; sp_buf text; } sp_text;

typedef struct {
    // CGuiInventory.AddBookKnowledge's braces and body, with its reward calls redirected to
    // W2EFBookGrants (Get-CollectBody of the reference), LF line endings. Required by
    // BookMarker.
    const char* collect_body;
    // Books that got a read icon, for the generated W2EF_HasReadIcon (Get-ReadIcons in the
    // reference). Names must be plain ASCII without '"' or '\'. Required by BookMarker.
    const char* const* books;
    int nbooks;
} sp_ctx;

// Parses the RCDATA edits.txt. The table and its strings stay valid for the process (they
// point into the cached resource). 1 = ok; on failure sp_script_error() says why.
int sp_edits_load(const sp_edit** out, int* n);

// The RCDATA fragment named "book_helpers.ws" etc. (NUL-terminated), or NULL. The text is
// cached and stays valid for the process.
const char* sp_fragment(const char* name);

// 1 when the text already carries SP_MARKER (i.e. another copy of the patcher wrote it).
int sp_script_marked(const sp_buf* text);

// Port of Get-CollectBody: the braces and body of CGuiInventory.AddBookKnowledge with
// thePlayer.MaxKnowledgeAccumulator( and thePlayer.AddJournalEntry( redirected to the
// W2EFBookGrants members, CRLF -> LF. `out` is owned by the caller (sp_free). 1 = ok; a
// function that is missing twice or does more than grant knowledge fails with `why`.
int sp_script_collect_body(const sp_buf* inventory, sp_buf* out, char* why, size_t whylen);

// Applies the rows of an arbitrary edit table whose feature matches `feature`, in table
// order, exactly as the .ps1 does. sp_script_apply passes the RCDATA table; the unit tests
// build their own. All or nothing: when any anchor does not match as declared, or a fragment
// or ctx input is missing, every text is left unchanged and 0 is returned with a reason in
// `why`. A text that already carries this feature's marker is refused (the .ps1 refuses any
// marker in the whole archive before installing; the overlay does that check once on the
// source archive). 1 = ok.
int sp_script_apply_edits(const sp_edit* edits, int nedits, const char* feature,
                          sp_text* texts, int ntexts, const sp_ctx* ctx,
                          char* why, size_t whylen);

// sp_script_apply_edits with the table from sp_edits_load.
int sp_script_apply(const char* feature, sp_text* texts, int ntexts, const sp_ctx* ctx,
                    char* why, size_t whylen);

// The reason of the last failing sp_edits_load/sp_script_apply ("": none yet).
const char* sp_script_error(void);

#endif
