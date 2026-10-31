# Shop "already read" marker on books

Status: opt-in patch implemented 2026-10-30, now the `BookMarker` feature of
`tools/script-patches/ScriptPatches.ps1` (the tool is described in `docs/fixes/menu-buttons.md`). Version 1
(read or carried) confirmed in game the same day; version 2 (also "nothing new", list status
and tooltip, see *Version 2*) installed for its in-game test. Version 3 (2026-10-31) moves the
logic out of the shop into the shared item fill, so every item list and the loot window get the
same marker (*Version 3*); the inventory list also updates as soon as a book is read
(*Refresh after reading*). Since 0.5.0's final form the tick means *read* (or, at a merchant and
in the storage chest, *carried*) only; "nothing new" is text (*Tick only for read*). Script and
Scaleform changes, not DLL code. Target: Steam Enhanced
Edition 3.5.0.26.

## Symptom

At a merchant (seen at Einar, the bookseller in Flotsam) a book in the merchant's list
had a tick-style overlay on its icon. After some selling it was gone, although nothing
about the player's reading history had changed.

## Cause

The overlay means "you carry a book with this name right now", not "you have read
it".

The trade screen is `globals\gui\ui_trade.swf` from `abetterui.dzip` (A Better UI,
shipped with the EE). Disassembled AS2 (`CTrade` class, `CompareInventories`, called
from `Init`, which is `mTrade.onData` and runs after every `FillShop` commit, so after
every buy and sell):

1. Mask = `ItemEnum.TYPE_BOOK | ItemEnum.TYPE_CRAFTING_SCHEMATICS`
   (`0x10000000 | 0x02000000`).
2. For every entry of `mTrade.UserItems` (the player's inventory) whose `Mask` matches,
   store its `Name` (the localized display name) in a set.
3. For every entry of `mTrade.ShopItems` whose `Mask` matches, set
   `_alreadyRead = (Name in that set)`.
4. The list row renderer (`AttachData`) loads
   `img://globals/gui/icons/tooltip/alreadyread_64x64.dds` over the icon (and
   `alreadyread_15x15.dds` next to the quantity) when `_alreadyRead` is true.

So selling your copy of a book removes the marker at the next refresh. Books in the
player storage chest never count. Schematics behave the same way. `ui_storage.swf`
has an identical `CompareInventories`. `ui_inventory.swf` and `ui_journal.swf` have
the renderer but nothing sets the field there.

The scripts only fill `Mask`, `Name`, etc. (`CGuiShopNew.FillShop` in
`game\gui\guishop.ws` -> `CGuiUtils.FillItemObject` in `game\gui\guiutils.ws`);
nothing about reading is passed to Flash.

## What the game does record

Reading a book from the inventory (`CGuiInventory.ExamineItem`,
`game\gui\guiinventory.ws`) runs `AddBookKnowledge` and then:

```
FactsAdd( NameToString( itemName ) + "_Examined", 1, -1 );
```

That fact is permanent and lives in the save, so "has read" is available to scripts
via `FactsDoesExist( itemName + "_Examined" )`. Reading done outside `ExamineItem`
(quest scripts, lecterns, if any) does not create it.

## Options considered

Two changes; the DLL cannot do it alone:

* **Script:** in `FillItemObject`, for book items set a field such as
  `theHud.SetBool( "Examined", FactsDoesExist( NameToString( itemName ) + "_Examined" ), AS_item )`.
* **Flash:** in `CompareInventories` of `ui_trade.swf` and `ui_storage.swf`, make the
  shop side `_alreadyRead = owned || item.Examined` (a small AS2 bytecode edit). The
  renderer can stay as it is.

Costs:

* Shipping a modified `base_scripts.dzip` collides with any other script mod. On the
  owner's machine Project Mersey Fix Pack already replaces it
  (`docs/plans/00-context.md`), so a patch would have to be built on top of that
  pack's scripts.
* Shipping modified `ui_trade.swf`/`ui_storage.swf` overrides whichever copy is active.
  On the owner's machine that is not the `abetterui.dzip` one (part of the Steam
  install, not in `added_files.txt`) but the loose
  `CookedPC\globals\gui\ui_trade.swf` / `ui_storage.swf` from *Witcher 2 HD UI AI
  Upscale* (2.2 MB / 1.45 MB, upscaled embedded graphics). Its `CompareInventories`
  bytecode is identical to the A Better UI version, so the same patch applies to both.
  Checked byte for byte: the `CompareInventories` DefineFunction2 body is 407 bytes in
  all four files, SHA-256 prefix `9e987436873d` in both `ui_trade.swf` variants and
  `e82ddd4973f9` in both `ui_storage.swf` variants.
* Because of that, the sensible delivery is a patch tool that edits the user's active
  swf in place (back up, match the known `CompareInventories` bytes, refuse otherwise),
  the same "build from the user's own files" approach as the interior maps, rather
  than redistributing either variant or requiring the HD UI mod.
* If shipping files is preferred after all: the HD UI mod is
  [Nexus 1142](https://www.nexusmods.com/witcher2/mods/1142) by RGB Panda (uploader
  `rgb1110`), v1.3. Its permissions (read 2026-10-30) allow re-upload, modification
  and asset use with credit, also in mods that earn Donation Points, but not in paid
  mods. So an opt-in folder with patched copies of its two swf files, credited, is
  allowed. The swf code itself is CDPR's/A Better UI's, which the interior maps
  already set a precedent for shipping derived game data.
* Optionally keep the "owned" meaning too, or show a different overlay for "owned but
  unread" if both are worth telling apart.

## Implementation: opt-in patcher

First shipped alone as `tools/book-read-marker/BookReadMarker.ps1`; since the menu buttons it is
the `BookMarker` feature of `tools/script-patches/ScriptPatches.ps1` (PowerShell 5.1 with inline
C#), shipped as `Witcher2EngineFix_Optional\ScriptPatches\` with `Install.bat` / `Uninstall.bat`.
The mechanics below are unchanged; the inserted blocks are now marked
`//===witcher2-enginefix=== BEGIN/END BookMarker`.
Nothing changes unless the player runs it. `-Action Check` only reports.

It patches whatever the game currently loads, so vanilla, Project Mersey and HD UI all
work without the release shipping any of their files:

* **Script.** In the active `CookedPC\base_scripts.dzip`, entry `game\gui\guishop.ws`,
  after the single line
  `theHud.m_utils.FillItemObject( m_merchantInventory, m_playerStats, itemId, i, AS_item, slotItems );`
  it inserts (marked `//===witcher2-enginefix=== BEGIN/END - book read marker`):

  ```
  if ( itemTags.Contains( 'SortTypeBook' ) || itemTags.Contains( 'SortTypeSchematic' ) )
  {
      theHud.SetBool( "_alreadyRead", FactsDoesExist( NameToString( m_merchantInventory.GetItemName( itemId ) ) + "_Examined" ) || m_playerInventory.GetItemQuantityByName( m_merchantInventory.GetItemName( itemId ) ) > 0, AS_item );
  }
  ```

  The tags are the ones `GetItemTypeFlags` builds `TYPE_BOOK` / `TYPE_CRAFTING_SCHEMATICS`
  from (`inventorycomponent.ws`), so the item set is the swf's. Ownership is now matched by
  item name instead of localized display name. `itemTags` is already filled for the item
  in that loop. No script in the game uses a bitwise `&`, so the tags are used instead.
* **DZIP rewrite.** Only that entry is re-encoded (LZF literal runs, valid LZF, ~3%
  larger). It is appended where the old entry table was, every other entry keeps its
  original compressed bytes, and a new table and header hash are written. The hash is
  Gibbed.RED's `EntriesHash` (FNV-1a 64 prime, seed `0xFFFFFFFF`, over name bytes, name
  length, FILETIME, sizes and offset). Checked against vanilla and Mersey
  `base_scripts.dzip` and `elf_flotsam.dzip` before use.
* **Scripts are compiled from the dzip.** `CookedPC\compiledscripts.w2scripts` dates from
  the install and has none of Mersey's identifiers (`IsGlowItem`), so the game must compile
  `base_scripts.dzip` itself; otherwise Mersey would have no effect either.
* **Swf.** In `ui_trade.swf` and `ui_storage.swf` the 24-byte statement after
  `9D 02 00 18 00` (`if` skipping 24 bytes when the item is no book/schematic) is
  `96 0C 00 04 0A 09 ?? ?? 04 03 04 0A 09 ?? ?? 4E 4E 96 01 00 02 49 12 4F`
  (`r10._alreadyRead = r3[r10.Name] != null`; `??` = constant pool indices, which differ
  between trade and storage). It becomes `96 0B 00 04 0A` + `02`x9 + `17`x10
  (push r10 and nine nulls, pop ten: same length, stack-neutral), so no tag, function or
  branch length changes. The pattern must match exactly once. If the swf is not a loose
  file, the `abetterui.dzip` copy is written out as a loose file (loose files override the
  archives, as the interior maps and HD UI rely on).
* **Safety.** Refuses on any unknown content and changes nothing then; refuses while a
  `witcher2.exe` from that game folder is running. Install is idempotent. Originals go to
  `backup\` with a manifest of the SHA-256 written; Uninstall only restores or removes a
  file that still has that hash, so a later mod update is never rolled back.

### Dry run, 2026-10-30

On copies of the owner's files in a scratch game folder, once with the HD UI loose swfs and
once without (abetterui.dzip only):

* Check -> Install -> Install (reports "Already installed") -> Check (all `patched`).
* Independent Python check: header hash valid; 460/460 entries, only `game\gui\guishop.ws`
  differs; its diff is exactly the six inserted lines with LF endings like the file.
* Swf: same length, 19-20 changed bytes all inside the 24-byte window; disassembly shows
  `if -> 20868`, `push r10 null x9`, `pop x10`, then `_OrigIndex` as before.
* Uninstall: base_scripts.dzip and the HD swfs byte-identical to the originals; in the
  vanilla folder the created loose swfs are removed.

Not verified yet: that the game compiles the patched script and shows the overlay.

### In-game test

1. Close the game, run `Install.bat` (or `ScriptPatches.ps1 -Action Install -Features BookMarker -GameDir <game>`).
2. Start the game; it must reach the main menu normally (a script compile error would show
   here).
3. At a bookseller, a book read earlier but no longer carried shows the overlay; an unread
   book you do not carry shows none; a book you carry still shows it.
4. Storage chest: same rules for books in the chest.

### Confirmed in game, 2026-10-30

Installed on the owner's machine (Mersey `base_scripts.dzip`, HD UI swfs) with
`-Action Install`; backups matched the original files byte for byte. The game compiled
the patched script and reached the menu; at Einar in Flotsam the owner confirmed the
overlay "works very well". So the game does compile `base_scripts.dzip` at startup.

Resulting rules (merchant list and storage chest, books and schematics only):

| Item | Overlay |
|---|---|
| Book read earlier from the inventory (fact `<item>_Examined`), sold or not | yes |
| Book or schematic Geralt carries (same item name) | yes |
| Unread book not carried (also: only in the storage chest) | no |
| Schematic not carried | no (schematics are never "read") |
| Geralt's own list in the trade screen | no (the swf still clears it there) |

## Version 2: "nothing new" and a visible status

Books can also be worth nothing to a player who never read them: monster knowledge comes
from fights and journal entries from quests. `CGuiInventory.AddBookKnowledge` (49 books in
vanilla: 18 monster books, 3 knowledge books, 28 glossary/places books) grants only two
things, both readable from script:

* `thePlayer.MaxKnowledgeAccumulator( cat, 100 )`: knowledge category to its maximum, 3, or
  1 for categories 9-11 (`IncreaseKnowledgeAccumulator`). Current level:
  `thePlayer.GetKnowledgeAccum( cat ).m_level`. (`Book of Bruxas` grants category 12, the
  same as `Knowledge Crafting Book`; that is the game's data, kept as is.)
* `thePlayer.AddJournalEntry( group, id, subId, ... )`: present when
  `thePlayer.m_knowledge.m_groups[ group ]` has an entry with `m_id == id` whose `m_subIds`
  contains `subId` (`CGuiJournal` reads `m_knowledge` the same way).

The script patch is now one line in `FillShop` after the merchant `FillItemObject`,
`W2EF_MarkShopItem( m_merchantInventory, m_playerInventory, itemId, AS_item );`, plus helpers
appended to the end of `guishop.ws`:

* `class W2EFBookGrants` with `Knowledge(cat, amount)` and `Journal(group, id, sub, cat, img)`
  counters, and `Collect( itemName )`, whose body is `AddBookKnowledge` copied from the
  installed `guiinventory.ws` with `thePlayer.MaxKnowledgeAccumulator(` -> `Knowledge(` and
  `thePlayer.AddJournalEntry(` -> `Journal(`. Its conditions (Adda 0a/0b on
  `witcher1_adda_lives`) and Mersey's extra Flaming Rose entry come along. The patcher refuses
  if anything else in that body touches `thePlayer`.
* `W2EF_MarkShopItem`: schematics keep "carried"; for books
  `_alreadyRead = read || carried || (grants > 0 && none new)`, then `Desc` (the type text under
  the name in the list, upper-cased by the swf) gets ` / Read`, ` / Nothing new` or
  ` / Unread`, and `DescFull` (tooltip, grey HTML) gets a status sentence and, for unread
  books, one line per reward: `New: All knowledge on Rotfiends (you have 0/3)` (orange
  `#ff9900`, the colour `guicharacter.ws` uses) or `Known: Rotfiend (bestiary entry)`.
  Reading always sets a category to its maximum, so only the current level is shown.
  Journal names come from `GetLocStringByKeyExt( entryId )` like the journal screen (raw id
  if empty); a second page of the same entry says "(journal entry, next page)". Lore-only
  books say so. Texts follow the game's text language (see *Languages*). (Earlier wordings, "New for you: 2 of 2" with bare
  levels and then "Rotfiends knowledge 0/3, the book raises it to 3/3" /
  "Bestiary entry: Rotfiend", worked in game but read as cryptic or wordy.)

No swf change beyond version 1: `Desc`/`DescFull` already exist and are read back with
`theHud.GetString`.

Dry run (same procedure as above, plus true vanilla `base_scripts.dzip` from
`_backup_premods`): valid hash, the generated `Collect` has 21 knowledge and 58 journal calls
on Mersey (57 on vanilla), no `thePlayer.AddJournalEntry`/`MaxKnowledgeAccumulator` left,
names do not clash with game classes or functions; Uninstall restores both archives
byte-identically. A v1 install is reported as "older version, run Uninstall first"; the
owner's v1 was uninstalled and v2 installed that way, backups again byte-identical to the
Mersey and HD UI originals.

First v2 start failed: *Script compilation error, game\gui\guishop.ws[375]: parse error,
expecting `TOKEN_IDENT', near 'entry'*. `entry` is a WitcherScript keyword (entry functions);
a local was named `entry`. The game refuses to start on any script error; Uninstall.bat
recovered it. Renamed to `accum`, and every declared name in the helpers is now checked
against the compiler's keyword table, found as UTF-16 strings next to `virtual_parent` in the
unpacked exe (`0xA781EC` file offset): abstract array bool break brix byte case class cleanup
const continue def default defaults delete do editable else enter entry enum event exec
extends final float for function if import in inlined int latent local name new NULL
optional out parent private public quest return saved savepoint state storyscene string
struct super switch this timer var virtual_parent void while, plus theGame theHud thePlayer
theCamera theServer theSound. The helpers also only use constructs that occur in the game's
own scripts (class without `extends` + `new ... in`, `theHud.GetString` out param, journal
lookup via local struct copies as in `CGuiJournal`).

Version 2 in-game test: the game reaches the main menu (the new script compiled); a book read
earlier shows `/ READ`; an unread monster book for a monster already at 3/3 with its bestiary
entry shows `/ NOTHING NEW` and the overlay; an unread book with new content shows `/ UNREAD`,
no overlay, and the tooltip lists what is new.

## Version 3: every item list and the loot window

Version 1 and 2 only reached the shop and storage screens because the marking was a call in
`CGuiShopNew.FillShop`. The overlay itself is a property of the item object (`_alreadyRead`),
read by the Scaleform list row renderer, so any screen that puts a book into a list can have it.

What the UI actually does (all verified against the game's own files):

| Movie | `_alreadyRead` renderer | Tooltip (`DescFull`) | Filled by |
|---|---|---|---|
| `ui_inventory.swf` | yes (`_mcAlreadyReadIconOverlay`, `alreadyread_64x64.dds`, `alreadyread_15x15.dds`) | yes, imports `ctooltip.swf` | `CGuiInventory.FillInventory` |
| `ui_trade.swf`, `ui_storage.swf` | yes | yes | `CGuiShopNew.FillShop` (player and merchant side) |
| `ui_craft.swf`, `ui_elixirs.swf` | yes | yes | `CGuiCraft`, `CGuiElixirs` |
| `clist.swf`, `clistinfo.swf` (A Better UI, from `abetterui.dzip`) | yes, the shared list component | - | imported by the panels |
| `ui_loot.swf` | **no** | **no**, imports only `xfontlib.swf` | `CContainer.FillLootWindow` |

All four panels above fill their rows through one function, `CGuiUtils.FillItemObject` in
`game\gui\guiutils.ws` (`theHud.m_utils`). It sets `Desc` (the type text under the name),
`DescFull` (the tooltip), `Mask` and the rest. That makes it the one place where the marker
belongs:

* **Script.** After the statement that assembles `DescFull`, every item gets
  `W2EF_MarkItem( inventory, itemId, AS_item, custom );`. The helpers (`W2EF_Text`,
  `W2EFBookGrants`, `W2EF_BookStatus`, `W2EF_MarkItem`, `W2EF_LootName`, `W2EF_LootIcon`) moved
  from `guishop.ws` to the end of `guiutils.ws` as file-level functions, which the game's own
  scripts do across files (`FlashValueFromString` in `engine\flashinstance.ws` is called from
  `container_class.ws`). `W2EFBookGrants.Status( isRead )` is now the single classification:
  1 read, 2 unread but nothing it gives is new, 3 unread with something new.
* **Context.** `FillItemObject` already had an unused `optional custom : name` (it only tests
  `'alchemy'` and `'schematic'`). The merchant/storage side of `FillShop` now passes
  `'merchant'`, so carrying a copy keeps marking the item there; everywhere else (the player's
  own inventory, the trade screen's player side, crafting, alchemy, elixirs, the character
  screen) the marker means *read* or *nothing new*, which is what a player wants in their own
  bag. `guishop.ws` therefore keeps only that one changed line and no helpers.
* **Loot window.** `ui_loot.swf` has eight fixed rows (`Item1..Item8` with `Name`, `RName`,
  `Quant`, `Icon`, `ID`, `Glow`) and no tooltip of any kind, so there the status goes after the
  name (` / Read`, ` / Nothing new`, ` / Unread`) and the same `alreadyread_15x15.dds` image the
  lists use is put in front of the icon. The icon is not drawn by the movie: `FillLootWindow`
  builds it as an HTML string (`"<img src='img://globals/gui/icons/items/...'>"`) and the row
  shows that field as HTML, so a second `<img>` needs no SWF change. Both loops of
  `FillLootWindow` push the display name and the icon, so the patcher rewrites both statements
  (four lines in all).

Coverage after version 3:

| View | Overlay | Status in the row | Tooltip |
|---|---|---|---|
| Inventory (books Geralt carries) | read (nothing new too until *Tick only for read*) | yes | yes, with what is new |
| Trade screen / storage, player's list | read (since *Every window*, below) | yes | yes |
| Trade screen / storage, merchant side | read or carried | yes | yes |
| Craft, alchemy, elixirs, character | only if such a list held a book (none does) | - | - |
| Loot window | read, on the icon (since *Third in-game test*) | "nothing new" only, in the final form | none (the window has none) |
| Journal, book reader | untouched | - | - |

Correction (2026-10-31): as first shipped, the player's side of the trade and storage screens
got the status text and tooltip but not the overlay, because `CompareInventories` clears the
flag there too. Fixed in *Every window: the player's side of trade and storage*.

Known limits, on purpose: the loot window has no hover tooltip to fill, so the "what is new"
detail exists only in the panels that have one; and the marker call sits in the
`if ( ! itemTags.Contains('NoDescription') )` branch next to the tooltip, so a book tagged
`NoDescription` would keep no marker (books always have a tooltip text; no such book is known).

**Tool.** `tools/script-patches/ScriptPatches.ps1` grew one edit mode: `Where = "Replace"`
replaces whole lines that match the anchor (exactly `Count` of them), still inside their
indentation and with BEGIN/END markers, which is used for the merchant call and the four loot
lines. BookMarker now edits `guiutils.ws` (the call plus the helper block), `guishop.ws` (the
context argument) and `container_class.ws` (the loot rows); the SWF no-op patch on
`ui_trade.swf`/`ui_storage.swf` is unchanged and still needed, otherwise `CompareInventories`
overwrites the merchant side's flag after every buy/sell. Install/Check/Uninstall now look at
all four entries.

### Dry run, 2026-10-31

Two scratch game folders: the pre-mod vanilla `base_scripts.dzip` from `_backup_premods`, and
the owner's Mersey archive with the HD UI loose `ui_trade.swf`/`ui_storage.swf` from the
install's `backup\`. In both: Check -> Install -> Install -> Check, `-Features All` ->
`-Features BookMarker`, then Uninstall. Independent Python checks on the produced archive:
header hash valid, entry count/names unchanged, only `guiutils.ws`, `guishop.ws` and
`container_class.ws` differ, the helper block is 684/685 lines with no non-ASCII, braces
balanced, `__COLLECT_BODY__`/`__TEXT_FUNCTION__` substituted, no `thePlayer.AddJournalEntry`/
`MaxKnowledgeAccumulator` left, and every declared name compared against the compiler's keyword
list (this is how the `state` local was caught and renamed to `markState` before shipping).
Uninstall restored `base_scripts.dzip` and both loose swfs byte-identically; in the vanilla
folder the two created loose swfs were removed.

### In-game test

1. Close the game, run `Install.bat` (or `ScriptPatches.ps1 -Action Install -Features BookMarker`,
   `All` to keep Continue/Quit).
2. Start the game; the main menu must appear normally (a script compile error stops it there).
3. Inventory (I): a read book shows the tick and `/ READ`; an unread book with new content shows
   `/ UNREAD` and its tooltip lists what is new; a book with nothing new shows `/ NOTHING NEW`.
4. Hover a book in the inventory: the tooltip ends with the status paragraph.
5. A merchant and the storage chest: behaviour unchanged from version 2.
6. Loot a corpse or container with books: rows show the tick and `/ READ`, `/ NOTHING NEW` or
   `/ UNREAD` after the name. Look at the row layout: if the tick does not fit in front of the
   30x30 icon, `W2EF_LootIcon` is the one line to change (append it after the icon instead).

## Refresh after reading (2026-10-31)

First in-game result of version 3: the inventory marker works, but a book read there kept its
old status until the inventory was closed and opened again.

Why: the inventory list is filled once, by `CGuiInventory.FillInventory` when the panel opens
(`FillData`), and again only after trashing (`RemoveItem`) or upgrading (`OnItemUpgraded`).
Reading is `ExamineItem`, called from Flash (`ExternalInterface.call( "ExamineItem", id )` in
the panel's input handler, which then shows `m_mcBook`/`m_mcCover`); it grants the rewards,
adds `<item>_Examined` and sends the text with `SetBookText`, but never refills the list.

Updating only the one row does not work without a SWF change: the list component
(`UpdateListData`) re-attaches a row only when the object in `m_aItemsData` is a different
object from the one the row shows, so changing fields of the existing object is not drawn, and
`m_aItemsData` is the list's own filtered copy, so replacing an element of `mInv.Items` is not
seen either. A full refill is also the right scope: reading one book can change other rows too
(another copy, or a different book giving the same knowledge or journal entry).

The patch, in `game\gui\guiinventory.ws` (part of BookMarker):

* `ExamineItem` calls `W2EF_RefillAfterReading()` after its `SetBookText` line.
* `W2EF_RefillAfterReading` is a new private method of `CGuiInventory` (inserted before
  `ExamineItem`). It does what `RemoveItem` does, `ProcessInventoryChanges(); FillInventory();`,
  so equipment changes still only in the Flash list are applied first instead of being lost.
* `FillInventory` ends in `Commit` -> `onData` -> the panel's `Init`, which calls
  `m_mcItemList.SetData( Items )` (keeps filter, sort and offset) and then
  `m_mcItemList.SetPosition( 0 )`, so the list would jump to the top on the next mouse move,
  exactly as vanilla does after trashing an item. The method therefore reads `m_iCurrentOffset`
  and `m_iCurrentPosition` from `_global.pPanelClass.m_mcItemList` first and afterwards calls
  `SetOffset`, `SetPosition`, `UpdateListData` and `UpdateGraphic` on it, the sequence the list's
  own `ScrollUsed` uses. It reaches the list with `GetObject( name, out id, parent )` member by
  member, the form the game's scripts use, not a dotted path. If a movie lacks these members the
  reads fail and only the scroll is lost.
* `Init` does not touch `m_mcBook`/`m_mcCover`, and the Flash caller only shows the book after
  `ExamineItem` returns, so the book still opens normally. Refilling from inside a Flash call is
  what `RemoveItem` already does.

Checked: `ui_inventory.swf` from `abetterui.dzip` and the HD UI loose copy disassemble
identically; `ui_inventoryxbox.swf` (gamepad) has the same member names. Anchors are unique in
vanilla and Mersey `guiinventory.ws` (Mersey's is CRLF, vanilla's LF; both kept). Dry run as
before on both archives, plus an upgrade from the installed version 3 with the new tool
(Install over Install), then Uninstall: byte-identical originals.

Other inventory actions need nothing: trashing and upgrading already refill; equipping and quick
slots do not change a book's status; only `ExamineItem` grants book rewards or `_Examined` (no
other script calls `AddBookKnowledge`). The shop and storage refill after every buy, sell and
move (`FillShop` + `Commit`), and the loot window cannot read books.

In-game test:

1. Close the game, copy the new `ScriptPatches.ps1` into
   `<game>\Witcher2EngineFix_Optional\ScriptPatches\` and run `Install.bat` again (it rebuilds
   from `backup\`).
2. Main menu appears (script compiles).
3. Inventory, scroll down to an unread book, read it, close the book: the row now says
   `/ READ` with the tick, the list is still at the same place, and the tooltip says it was read.
4. A second copy of that book, or another book giving the same knowledge, also changed.
5. Equip a sword in the inventory, then read a book: the sword stays equipped.

## Every window: the player's side of trade and storage (2026-10-31)

Goal (owner): wherever Geralt handles a book, its status must be visible. The script side
already reaches every list (`FillItemObject`); what was missing is the overlay on the player's
own list in the trade and storage screens, the place where one decides what to sell or store.

`CompareInventories` (both movies, A Better UI and HD UI alike) walks `mTrade.UserItems` first
and, for every book or schematic, runs

```
if ( !( (item.Mask & books) > 0 ) ) skip 28 bytes      9D 02 00 1C 00
item._alreadyRead = false                              96 07 00 04 07 09 ?? ?? 05 00 4F
owned[ item.Name ] = true                              96 07 00 04 03 04 07 09 ?? ?? 4E 96 02 00 05 01 4F
```

(`207a8` in the HD UI `ui_trade.swf`, `0a9bd` in its `ui_storage.swf`). Those are the only two
writes of `_alreadyRead` in either movie besides the merchant-side one. The first statement
becomes `96 05 00 04 07 04 07 02 17 17 17` (push r7, r7, null; pop x3): same 11 bytes,
stack-neutral, so the `if` length and every branch stay valid; `owned[...]` stays, it is what
the merchant side used to read.

`ScriptPatches.ps1` now holds a list of SWF statements (`$SwfPatches`: player side, merchant
side), each with its original and patched bytes (`-1` = wildcard for the constant-pool indices
that differ between trade and storage). A movie counts as original or patched only when every
statement is found exactly once in that state; a mix, such as a movie patched by an older copy
of the tool, is refused. The upgrade path does not hit that: Install always starts from the
originals in `backup\`.

Checked: the pattern matches exactly once in all four movies (A Better UI and HD UI, trade and
storage). After Install, a byte diff against the uncompressed originals shows only the two
statements changed (27 bytes in trade, 25 in storage); the disassembly reads `push r7 r7 null`,
`pop` x3, then `owned[r7.Name] = true` and the branch target `207c4` as before. Dry run on both
archives, the upgrade from the version installed earlier the same day, and Uninstall: originals
byte-identical, created loose files removed.

The other windows, checked so that nothing is left out:

| Window | Can hold a book | Status |
|---|---|---|
| Inventory | yes | overlay, status, tooltip; updates right after reading |
| Trade, both lists; storage chest, both lists | yes | overlay, status, tooltip; refilled after every buy, sell and move |
| Loot window (corpses, chests, dropped bags) | yes | overlay in the icon and status after the name; no tooltip exists |
| Craft (`CraftingIngridient`, `Schematic`), alchemy (`Recipe`, `AlchemyIngridient`), elixirs (`Elixir`) | no, filtered by tag | - |
| Character (mutagens), quick-slot radial (`guifastmenu.ws`, own fill) | no | - |
| Arena mode lists (`guiarena*.ws`) | own arena inventory | status and tooltip via `FillItemObject` |
| `ui_inventoryxbox.swf` | - | no "xbox" swf name in the exe or any script, and no overlay renderer; `ui_inventory.swf` handles the pad itself (`m_bUseXboxpad`) |

In-game test, in addition to *Refresh after reading*:

1. A merchant: the books in Geralt's own list show the tick when read or nothing new, and
   `/ READ` etc. as before; the merchant side is unchanged.
2. Sell a read book and buy it back: the tick moves with it.
3. The storage chest: the same on Geralt's side.

## Loot window icon and loot preview (2026-10-31)

Owner's in-game test of the loot window (a bookshelf): the row showed the tick and `/ Read`,
but the book's own icon was gone; the preview next to the shelf, before opening it, showed the
icon and the name but no tick.

**Why the icon vanished.** `ui_loot.swf` draws a row's icon as HTML: `setItems` sets
`fxItemNN.lbIcon.htmlText` to the `<img ... width='30' height='30'>` string the script sends.
`lbIcon` (DefineEditText, character 23, shared by the seven item rows) is 34 x 34 px with word
wrap, so about 30 px are usable: `W2EF_LootIcon` put the 15 px tick first, and the 30 px icon
went onto a second line that the field cannot show. The name (`lbName`) and quantity (`lbQuant`)
cannot take the tick instead: the movie's `onEnterFrame` sets both with `.text` every frame.

**First fix** (not enough, see *Second in-game test and fix*). The field's bounds RECT in the DefineEditText tag goes from Xmin 1134 to 814 twips
(56.7 -> 40.7 px; `62 37 38 B7 EC 14 10` -> `61 97 38 B7 EC 14 10`): 16 px wider to the left,
same 12-bit fields, same length, right edge unchanged. With the tick first and both images
`hspace='0'`, the tick sits left of the icon and the icon stays exactly where it was. The row's
left margin should have room (estimated from the owner's screenshot: about 10 px of the window
remain left of the tick; to be confirmed in game). The
same 17 bytes (tag header, id, RECT, flags) are unique in both the A Better UI and HD UI movie.

**Preview.** Walking up to a container calls `CContainer.ShowLootPreview`, which sends up to four
`{ Name, Icon, Class }` objects to `hud.swf`'s `setLootTable`. That loads the icon into a clip
(`AssetLoader.loadAsset( Icon, fxIcon, 33, 33 )`) and sets `lbName.text = Name` once per call.
`lbName` (character 532) is an HTML field already, so the one statement becomes `.htmlText`: the
pushed constant 278 `'text'` -> 272 `'htmlText'` (`09 16 01` -> `09 10 01`), one byte, identical in
the A Better UI and the 32 MB HD UI `hud.swf`. Unformatted HTML uses the field's default text
format, the same one `.text` uses, so names look as before. `ShowLootPreview`'s two
`SetString( "Name", ... )` lines now call `W2EF_PreviewName`, which escapes `&`, `<`, `>` in every
name and puts the 15 px tick in front of a book that is read or has nothing new. No status text
there: the field has two lines and long names already fill them.

**Tool.** `$SwfFiles` maps each movie to its statements (`ui_trade.swf`/`ui_storage.swf`: the two
`CompareInventories` writes, `ui_loot.swf`: the icon field, `hud.swf`: the preview name). A movie
that is not found or does not match is now left alone with a note, instead of stopping the whole
install, and script edits marked `NeedsSwf` are left out with it: without the patched `hud.swf`
the preview would print the `<img>` tag as text, without the patched `ui_loot.swf` the tick would
hide the icon again.

Checked: byte diffs against the originals are exactly 2 bytes in `ui_loot.swf` and 1 byte in
`hud.swf`, in both variants; the parser reads the new field as 40.7..90.7 px; the disassembly of
the patched `hud.swf` reads `push 'htmlText' r2 r3` in `setLootTable`. Dry run on vanilla and
Mersey/HD UI, the upgrade from the copy installed earlier the same day, and Uninstall: all five
files byte-identical, created loose files removed. Fallback: an unknown loose `hud.swf` is left
untouched, the note is printed, and the script then has no `W2EF_PreviewName` call.

### Second in-game test and fix (2026-10-31)

Result of the version above: the preview showed the tick in front of the name, but a long name
was cut to its first line ("The Feudal"); the loot window still showed the tick without the
icon, the tick about 12 px further left than before (so the wider field was in effect).

Both fit one rule: Scaleform lays out an HTML `<img>` as an inline character (no float as in
Flash) and a text field shows whole lines only.

* Preview: the 15 px tick made the first line taller than the text, so the second line no longer
  fitted in the 29 px the field has inside its gutters.
* Loot window: tick + icon (45 px) did not fit in the 46 px the 16 px wider field had, so the
  icon wrapped onto the hidden second line. The `hspace='0'` written in front of `src` in the
  icon tag may not have helped either; the icon tag is now passed on unchanged.

Fix, still same-length edits:

* `ui_loot.swf`: the icon field RECT Xmin 1134 -> 334 twips (`60 A7 38 B7 EC 14 10`), 40 px wider
  to the left in all, and `W2EF_LootIcon` wraps tick + icon in `<p align='right'>`. The right
  edge did not move, so the icon stays exactly where it always was and the tick sits to its left
  however much room the layout takes.
* `hud.swf`: the tick in the preview name is 10 x 10 px, about the height of the 10 px text, and
  the name field (character 532) RECT Ymax 622 -> 702 twips (`... 5F 00`) gives it 4 px more
  height (35.1 px) as headroom for a slightly taller first line.

Checked: byte diffs are 2 bytes in `ui_loot.swf` (RECT) and 2 in `hud.swf` (RECT, constant) in both
the A Better UI and the HD UI movies; the parser reads the fields as 16.7..90.7 px and
-2..35.1 px; install and uninstall on both folders, originals byte-identical.

In-game test:

1. Walk up to a bookshelf or corpse with a read book: the preview shows a small tick in front
   of the name, and a long name still has both lines; other items look as before.
2. Open it: the row shows the book's icon where it always is, the tick just left of it, and
   `/ Read` after the name. Rows without books look as before.
3. An item name with `&` (if one is around) still reads correctly in the preview.

### Third in-game test: read icons instead (2026-10-31)

Result: the preview worked (small tick, both lines of the name), but in the loot window the
tick sat far out at the window's left edge, about 30 px left of the icon: the right alignment
did not hold and the 40 px wider field let the tick drift. The owner suggested what the
inventory does instead: show the icon with the tick on it, which needs no room at all.

The inventory's overlay is `alreadyread_64x64.dds` (A Better UI, `abetterui.dzip`): a black
tick on a transparent 64 x 64 DXT5, loaded over the icon at `_alpha` 90. The loot window and the
preview take an icon path, so a book that is read or has nothing new now gets a different path,
to a copy of its own icon with that overlay already drawn in:

* **The patcher builds the icons** from the player's own files at install time (nothing from the
  game is shipped): every item tagged `SortTypeBook` in an items definition (`def_*.xml` in any
  archive, 171 in this install, including quest letters and notices), its 64 x 64 icon from
  `globals\gui\icons\items\` (DXT1, DXT3, DXT5 or 32-bit; 152 have one), the overlay at 90 %
  opacity on top, written as uncompressed 32-bit DDS (the layout of the game's own 32-bit item
  icons) to `CookedPC\globals\gui\icons\items\w2ef_read\<icon>_64x64.dds`, about 2.5 MB in all.
  Files resolve as the game loads them: loose over archives, and among archives the later name
  wins (assumed from the `z_`/`ZZ_` names mods use; it only matters if two archives ship the same
  book icon). `pack0.dzip` (10.5 GB) is read through a streaming reader (`DzipFile`), not loaded.
  Install takes about 5 s.
* **Script.** `W2EF_ReadIcon( inventory, itemId, icon )` returns `icon` with `/icons/items/`
  replaced by `/icons/items/w2ef_read/` when the book is read or has nothing new and
  `W2EF_HasReadIcon( itemName )` (generated with the list of books that got an icon) says it has
  one; otherwise the game's own icon. `FillLootWindow`'s two icon pushes and `ShowLootPreview`'s
  two `SetString( "Icon", ... )` lines call it. The status after the name in the loot window
  stays.
* **No SWF change for these two screens any more.** The `ui_loot.swf` field widening and the
  `hud.swf` preview edits are gone from `$SwfFiles`; an install over the earlier version restores
  both movies from `backup\` (the tool now puts back any file in its manifest that it no longer
  patches). The trade/storage `CompareInventories` edits stay: those screens draw the
  inventory's overlay themselves.
* **Manifest.** The icon folder is entry `w2ef_read` with mode `folder`; Install rewrites it,
  Install without BookMarker and Uninstall remove it.
* Books with an apostrophe in the item name (`Dandelion's Book of Poetry`, quest letters): the
  loot window's icon name drops apostrophes, so they get their icon there; the preview's does
  not, so it finds no icon for them, the game's or ours, as before.

Checked: the generated `glossaryscoiatael_64x64.dds` (The Feudal Society and Its Enemies)
decodes to the book with the black tick, identical to a Python prototype of the same overlay;
Install / Install with Continue only / Install / Uninstall on copies of the vanilla and the
Mersey/HD UI folders (archives hard-linked, never written); the upgrade from the version with
the SWF edits restores `ui_loot.swf` and `hud.swf` byte-identically; all originals
byte-identical after Uninstall.

In-game test:

1. Walk up to a bookshelf with a read book: the preview shows the book's icon with the tick on
   it, and the name as before.
2. Open it: the row shows that icon in its usual place and `/ Read` after the name.
3. An unread book with something new shows its plain icon in both.

## Tick only for read (2026-10-31, final form of 0.5.0)

The owner's third loot test was good; then two decisions:

* **The tick means read.** Until now it also showed for an unread book whose knowledge and
  journal entries were all known ("nothing new"). That is not what a tick named `alreadyread`
  tells a player, and such a book still has a text that was never read; selling it on the tick's
  word loses that. It is not rare either: bestiary knowledge also comes from fights, so later in
  the game many of the 49 books that grant something are "nothing new" before they are opened.
  So `_alreadyRead = markState == 1 || isOwned` in `W2EF_MarkItem` (carried still counts at a
  merchant and in the storage chest, the vanilla meaning there), and `W2EF_ReadIcon` swaps in
  the ticked icon for read books only. "Nothing new" stays in the list status, the tooltip (with
  its `Known: ...` lines) and the loot window.
* **Loot window text only where the icon says nothing.** A read book has the tick on its icon and
  an unread one is the normal case, so `W2EF_LootName` now adds only ` (Nothing new)` (the
  language's text 2), for that state alone. ` / Read` and ` / Unread` were the same length as
  `(read)` would have been, so dropping them is what saves the room.

Dry run on the vanilla scratch copy (generated `W2EF_LootName`, `W2EF_ReadIcon` and the
`_alreadyRead` line checked, Uninstall restores), then installed on the owner's game.

## Languages

The list labels and tooltip lines come from `W2EF_Text( id )`, generated into the BookMarker
block from `$BookTexts` in `ScriptPatches.ps1`: 15 texts each for EN, PL, DE, FR, ES, IT, RU,
CZ, HU, TR, JP and ZH (Traditional, like the game's `zh0`). The language is
`theGame.GetGameLanguageName( audio, text )`, the codes `CGuiMainMenu` itself checks; anything
else (BR and KR ship only English placeholder texts in this install) gets English.

* Script string literals: the game's own scripts only have non-ASCII in Polish debug logs
  (Windows-1250 bytes), never in displayed text, so a literal's code page is unknown. The tool
  writes each non-ASCII character as `StrChar( n )`; the native (`008E5A0A`) builds the string
  from one UTF-16 unit. `$BookTexts` itself holds `\uXXXX` escapes and is unescaped with
  `[regex]::Unescape`, so `ScriptPatches.ps1` stays pure ASCII (PowerShell 5.1 reads a file
  without BOM in the ANSI code page).
* Fonts: `tools/script-patches/check_texts.py <game>\CookedPC` decodes the game's
  `.w2strings` (`docs/research.md` section 9) and checks every character, labels upper-cased
  too, against that language's own texts. All 12 pass. German has no `ß` anywhere in the game
  text (its `vergaß` decodes as `verga^`), so the German texts avoid it.
* Monster names (`Knowledge_N`) and journal entry names come from the game in its language;
  `%1` in "All knowledge on %1" / " (you have %1)" takes them, so word order can differ per
  language (`%1の完全な知識`).
* Dry run: the generated `W2EF_Text` was evaluated back to text in Python and matches the source
  for all 12 languages.
* The translations are the author's, not a native speaker's; corrections are welcome.

## How this was found

`tools/build-interior-maps.py`'s DZIP reader extracted `base_scripts.dzip` and the
swf files from `abetterui.dzip` into a scratch folder. A throwaway AS2 disassembler
(DoAction/DoInitAction plus constant pools) dumped the swf code. Nothing was written to
the game folder.
