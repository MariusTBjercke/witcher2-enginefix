# Optional script patches for The Witcher 2 EE, each one chosen separately:
#   BookMarker  every item list: the "already read" overlay for books Geralt has read or whose
#               knowledge and journal entries he already has (a merchant or the storage chest also
#               marks a carried copy); books get Read / Nothing new / Unread in the list and a
#               tooltip listing what is new, in 12 languages ($BookTexts). The loot window has no
#               tooltip, so there the same status goes after the name and a tick into the icon.
#               docs/fixes/shop-book-read-marker.md
#   Continue    main menu: "Continue" at the top loads the newest save
#   PauseQuit   pause menu: "Quit" (with confirmation) next to "Exit current game"
#               docs/fixes/menu-buttons.md
#
# Patches the files the game actually uses, whatever mod they come from:
#   CookedPC\base_scripts.dzip       game\gui\guiutils.ws, game\gui\guishop.ws,
#                                    game\gui\guimainmenu.ws, game\gameplay\container_class.ws:
#                                    blocks inserted next to anchor lines, marked
#                                    //===witcher2-enginefix=== BEGIN/END <feature>
#   CookedPC\globals\gui\ui_trade.swf, ui_storage.swf (BookMarker only)
#                                    CompareInventories no longer overwrites _alreadyRead on the
#                                    merchant side (24 bytes replaced by a same-length no-op)
# A swf that only exists inside abetterui.dzip is written out as a loose file first.
# Every Install rebuilds from the original files (kept in .\backup), so features can be added or
# removed later by running Install again. Uninstall puts back only files that still hold our
# patched bytes.
#
# Usage: ScriptPatches.ps1 [-Action Check|Install|Uninstall] [-Features All|BookMarker,Continue,PauseQuit]
#                          [-GameDir <game>]
# Install without -Features asks for each feature.
param(
    [ValidateSet("Check", "Install", "Uninstall")][string]$Action = "Check",
    [string[]]$Features = @(),
    [string]$GameDir = ""
)

$ErrorActionPreference = "Stop"
# "-Features A,B" arrives as one string through powershell -File.
$Features = @($Features | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

public class DzipEntry
{
    public string Name;
    public long TimeStamp, UncompressedSize, Offset, CompressedSize;
}

// DZIP v2 as written by Gibbed.RED: 32-byte header, entries of 64 KiB LZF blocks behind a block
// offset table, entry table at the end, header hash = FNV-1a 64 variant over the entry table.
public static class Dzip
{
    public static List<DzipEntry> ReadTable(byte[] b, out long tableOffset)
    {
        if (BitConverter.ToUInt32(b, 0) != 0x50495A44) throw new InvalidDataException("not a DZIP archive");
        if (BitConverter.ToUInt32(b, 4) != 2) throw new InvalidDataException("unsupported DZIP version");
        int count = BitConverter.ToInt32(b, 8);
        tableOffset = BitConverter.ToInt64(b, 16);
        var list = new List<DzipEntry>();
        long p = tableOffset;
        for (int i = 0; i < count; i++)
        {
            int n = BitConverter.ToUInt16(b, (int)p); p += 2;
            var e = new DzipEntry();
            e.Name = Encoding.ASCII.GetString(b, (int)p, n).TrimEnd('\0'); p += n;
            e.TimeStamp = BitConverter.ToInt64(b, (int)p);
            e.UncompressedSize = BitConverter.ToInt64(b, (int)p + 8);
            e.Offset = BitConverter.ToInt64(b, (int)p + 16);
            e.CompressedSize = BitConverter.ToInt64(b, (int)p + 24);
            p += 32;
            list.Add(e);
        }
        if (Hash(list) != BitConverter.ToUInt64(b, 24)) throw new InvalidDataException("DZIP entry table hash mismatch");
        return list;
    }

    public static ulong Hash(List<DzipEntry> entries)
    {
        const ulong P = 0x00000100000001B3UL;
        ulong h = 0x00000000FFFFFFFFUL;
        foreach (var e in entries)
        {
            if (!string.IsNullOrEmpty(e.Name))
            {
                foreach (char c in e.Name) { h ^= (byte)c; h *= P; }
                h ^= (ulong)e.Name.Length; h *= P;
            }
            h ^= (ulong)e.TimeStamp; h *= P;
            h ^= (ulong)e.UncompressedSize; h *= P;
            h ^= (ulong)e.Offset; h *= P;
            h ^= (ulong)e.CompressedSize; h *= P;
        }
        return h;
    }

    public static DzipEntry Find(List<DzipEntry> entries, string name)
    {
        foreach (var e in entries)
            if (string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase)) return e;
        return null;
    }

    public static byte[] Extract(byte[] b, DzipEntry e)
    {
        int blocks = (int)((e.UncompressedSize + 0xFFFF) >> 16);
        var outBuf = new byte[e.UncompressedSize];
        int outPos = 0;
        for (int i = 0; i < blocks; i++)
        {
            long start = e.Offset + BitConverter.ToInt32(b, (int)(e.Offset + 4 * i));
            long end = i + 1 < blocks ? e.Offset + BitConverter.ToInt32(b, (int)(e.Offset + 4 * (i + 1)))
                                      : e.Offset + e.CompressedSize;
            int want = (int)Math.Min(0x10000, e.UncompressedSize - outPos);
            outPos += LzfDecompress(b, (int)start, (int)end, outBuf, outPos, want);
        }
        if (outPos != e.UncompressedSize) throw new InvalidDataException(e.Name + ": bad size after decompression");
        return outBuf;
    }

    static int LzfDecompress(byte[] src, int i, int end, byte[] dst, int o, int want)
    {
        int start = o, limit = o + want;
        while (i < end && o < limit)
        {
            int c = src[i++];
            if (c < 32)
            {
                c++;
                Buffer.BlockCopy(src, i, dst, o, c);
                i += c; o += c;
            }
            else
            {
                int len = c >> 5;
                int r = o - ((c & 31) << 8) - 1;
                if (len == 7) len += src[i++];
                r -= src[i++];
                for (int k = 0; k < len + 2; k++) dst[o++] = dst[r + k];
            }
        }
        return o - start;
    }

    // LZF stream of literal runs only (control byte = run length - 1, max 32): valid LZF, ~3% larger.
    static byte[] EntryData(byte[] data)
    {
        int blocks = (data.Length + 0xFFFF) >> 16;
        var ms = new MemoryStream();
        var offsets = new int[blocks];
        var payload = new MemoryStream();
        for (int i = 0; i < blocks; i++)
        {
            offsets[i] = blocks * 4 + (int)payload.Length;
            int s = i << 16, e = Math.Min(data.Length, s + 0x10000);
            for (int p = s; p < e; p += 32)
            {
                int n = Math.Min(32, e - p);
                payload.WriteByte((byte)(n - 1));
                payload.Write(data, p, n);
            }
        }
        foreach (int off in offsets) ms.Write(BitConverter.GetBytes(off), 0, 4);
        payload.WriteTo(ms);
        return ms.ToArray();
    }

    // Appends the new data for the given entries where the old table was and writes a fresh
    // table + header. Every other entry keeps its original compressed bytes. A replaced entry
    // gets the current FILETIME, so the game cannot mistake it for a file it already compiled
    // (the game ships compiledscripts.w2scripts next to the archive).
    public static byte[] ReplaceEntries(byte[] b, string[] names, byte[][] datas)
    {
        long tableOffset;
        var entries = ReadTable(b, out tableOffset);
        var ms = new MemoryStream();
        ms.Write(b, 0, (int)tableOffset);
        for (int i = 0; i < names.Length; i++)
        {
            var target = Find(entries, names[i]);
            if (target == null) throw new InvalidDataException(names[i] + " not found");
            var packed = EntryData(datas[i]);
            target.Offset = ms.Length;
            target.CompressedSize = packed.Length;
            target.UncompressedSize = datas[i].Length;
            target.TimeStamp = DateTime.UtcNow.ToFileTimeUtc();
            ms.Write(packed, 0, packed.Length);
        }
        long newTable = ms.Length;
        var w = new BinaryWriter(ms);
        foreach (var e in entries)
        {
            w.Write((ushort)(e.Name.Length + 1));
            w.Write(Encoding.ASCII.GetBytes(e.Name));
            w.Write((byte)0);
            w.Write(e.TimeStamp); w.Write(e.UncompressedSize); w.Write(e.Offset); w.Write(e.CompressedSize);
        }
        w.Flush();
        var result = ms.ToArray();
        Buffer.BlockCopy(BitConverter.GetBytes(newTable), 0, result, 16, 8);
        Buffer.BlockCopy(BitConverter.GetBytes(Hash(entries)), 0, result, 24, 8);
        return result;
    }
}

public static class Swf
{
    // Returns an uncompressed (FWS) copy; CWS is inflated.
    public static byte[] Uncompressed(byte[] b)
    {
        if (b[0] == 'F' && b[1] == 'W' && b[2] == 'S') return b;
        if (b[0] != 'C' || b[1] != 'W' || b[2] != 'S') throw new InvalidDataException("not a SWF file");
        int len = BitConverter.ToInt32(b, 4);
        var outBuf = new byte[len];
        Buffer.BlockCopy(b, 0, outBuf, 0, 8);
        outBuf[0] = (byte)'F';
        using (var ds = new DeflateStream(new MemoryStream(b, 10, b.Length - 10), CompressionMode.Decompress))
        {
            int o = 8, n;
            while (o < len && (n = ds.Read(outBuf, o, len - o)) > 0) o += n;
            if (o != len) throw new InvalidDataException("truncated SWF");
        }
        return outBuf;
    }

    // Byte pattern with -1 as wildcard; returns every match position.
    public static List<int> FindAll(byte[] b, int[] pat)
    {
        var hits = new List<int>();
        for (int i = 0; i + pat.Length <= b.Length; i++)
        {
            int k = 0;
            while (k < pat.Length && (pat[k] < 0 || b[i + k] == pat[k])) k++;
            if (k == pat.Length) hits.Add(i);
        }
        return hits;
    }
}
'@

# Appended to guiutils.ws by BookMarker. __COLLECT_BODY__ is CGuiInventory.AddBookKnowledge from the
# same archive with its two reward calls redirected to the counters below, so the list of what
# each book gives (and its conditions) always matches the installed scripts.
$BookHelpers = @'
// Books anywhere in the UI: _alreadyRead (the A Better UI overlay) when the book was read
// (<item>_Examined, set by CGuiInventory.ExamineItem) or when nothing it gives is new; a
// merchant's or the storage chest's list also marks a carried copy. Books get a short status
// after their type in the list and a few lines in the tooltip. The loot window (ui_loot.swf)
// has no tooltip, so there the status goes after the name and a tick into the icon.
__TEXT_FUNCTION__

class W2EFBookGrants
{
    var m_total : int;
    var m_new : int;
    var m_lines : string;
    var m_lastId : string;

    // 1 read, 2 unread but everything it gives is already known, 3 unread with something new.
    function Status( isRead : bool ) : int
    {
        if ( isRead )
        {
            return 1;
        }
        if ( m_total > 0 && m_new == 0 )
        {
            return 2;
        }
        return 3;
    }

    // One tooltip line per reward: new ones in the game's tooltip orange.
    function AddLine( isNew : bool, text : string )
    {
        m_total += 1;
        if ( isNew )
        {
            m_new += 1;
            m_lines += "<br><font color='#ff9900'>" + W2EF_Text( 9 ) + text + "</font>";
        }
        else
        {
            m_lines += "<br>" + W2EF_Text( 10 ) + text;
        }
    }

    function Knowledge( categoryId : int, expAmount : float )
    {
        var accum : SKnowledgeAccum;
        var maxLevel : int;
        var title : string;

        accum = thePlayer.GetKnowledgeAccum( categoryId );
        maxLevel = 3;
        if ( categoryId == 9 || categoryId == 10 || categoryId == 11 )
        {
            maxLevel = 1;
        }
        // Reading always sets the category to its maximum (MaxKnowledgeAccumulator).
        title = StrReplace( W2EF_Text( 11 ), "%1", GetLocStringByKeyExt( "Knowledge_" + ( categoryId + 1 ) ) );
        if ( accum.m_level < maxLevel )
        {
            AddLine( true, title + StrReplace( W2EF_Text( 12 ), "%1", IntToString( accum.m_level ) + "/" + IntToString( maxLevel ) ) );
        }
        else
        {
            AddLine( false, title );
        }
    }

    function Journal( entryType : EJournalKnowledgeGroup, entryId : string, entrySubId : string, entryCategory : string, optional entryImageName : string )
    {
        var group : SJournalKnowledgeGroup;
        var item : SJournalKnowledgeEntry;
        var i : int;
        var found : bool;
        var title : string;

        found = false;
        if ( entryType < thePlayer.m_knowledge.m_groups.Size() )
        {
            group = thePlayer.m_knowledge.m_groups[ entryType ];
            for ( i = 0; i < group.m_entries.Size(); i += 1 )
            {
                item = group.m_entries[ i ];
                if ( item.m_id == entryId && item.m_subIds.Contains( entrySubId ) )
                {
                    found = true;
                    break;
                }
            }
        }
        if ( GetLocStringByKeyExt( entryId ) == "" )
        {
            title = entryId;
        }
        else
        {
            title = GetLocStringByKeyExt( entryId );
        }
        if ( entryType == JournalGroup_Monsters )
        {
            title += " (" + W2EF_Text( 13 );
        }
        else
        {
            title += " (" + W2EF_Text( 14 );
        }
        // Some books add two pages to the same entry.
        if ( entryId == m_lastId )
        {
            title += ", " + W2EF_Text( 15 );
        }
        m_lastId = entryId;
        AddLine( !found, title + ")" );
    }

    function Collect( itemName : name )
__COLLECT_BODY__
}

// 0 not a book, 1 read, 2 nothing new, 3 unread with something new. Books only.
function W2EF_BookStatus( inventory : CInventoryComponent, itemId : SItemUniqueId ) : int
{
    var itemName : name;
    var grants : W2EFBookGrants;

    if ( ! inventory.ItemHasTag( itemId, 'SortTypeBook' ) )
    {
        return 0;
    }
    itemName = inventory.GetItemName( itemId );
    grants = new W2EFBookGrants in theHud;
    grants.Collect( itemName );
    return grants.Status( FactsDoesExist( NameToString( itemName ) + "_Examined" ) );
}

// One call at the end of CGuiUtils.FillItemObject, so every panel that lists items (inventory,
// shop, storage, trade, craft, alchemy, ...) gets this for free. context is 'merchant' for a
// merchant's or the storage chest's list, where carrying a copy also marks the item; everywhere
// else only having read the book or gaining nothing from it does.
function W2EF_MarkItem( inventory : CInventoryComponent, itemId : SItemUniqueId, AS_item : int, context : name )
{
    var itemName : name;
    var isBook, isRead, isOwned : bool;
    var grants : W2EFBookGrants;
    var itemDesc, itemDescFull, label, status : string;
    var markState : int;

    isBook = inventory.ItemHasTag( itemId, 'SortTypeBook' );
    isOwned = false;
    if ( ! isBook && ! inventory.ItemHasTag( itemId, 'SortTypeSchematic' ) )
    {
        return;
    }
    itemName = inventory.GetItemName( itemId );
    if ( context == 'merchant' )
    {
        isOwned = thePlayer.GetInventory().GetItemQuantityByName( itemName ) > 0;
    }
    if ( ! isBook )
    {
        // Schematics are never "read"; a merchant or the chest only tells you whether you carry one.
        if ( isOwned )
        {
            theHud.SetBool( "_alreadyRead", true, AS_item );
        }
        return;
    }

    isRead = FactsDoesExist( NameToString( itemName ) + "_Examined" );
    grants = new W2EFBookGrants in theHud;
    grants.Collect( itemName );
    markState = grants.Status( isRead );
    theHud.SetBool( "_alreadyRead", markState != 3 || isOwned, AS_item );

    label = W2EF_Text( markState );
    status = W2EF_Text( 3 + markState );
    if ( markState == 3 && grants.m_total > 0 )
    {
        status += W2EF_Text( 7 );
    }
    if ( grants.m_total == 0 )
    {
        status += "<br>" + W2EF_Text( 8 );
    }
    else if ( markState != 1 )
    {
        status += grants.m_lines;
    }

    theHud.GetString( "Desc", itemDesc, AS_item );
    if ( itemDesc == "" )
    {
        theHud.SetString( "Desc", label, AS_item );
    }
    else
    {
        theHud.SetString( "Desc", itemDesc + " / " + label, AS_item );
    }
    theHud.GetString( "DescFull", itemDescFull, AS_item );
    if ( itemDescFull == "" )
    {
        theHud.SetString( "DescFull", status, AS_item );
    }
    else
    {
        theHud.SetString( "DescFull", itemDescFull + "<br><br>" + status, AS_item );
    }
}

// Loot window (ui_loot.swf): eight fixed rows with a name, a quantity and an icon that the
// script builds as HTML, and no tooltip of any kind. The status therefore goes after the name
// and the same overlay image the item lists use goes in front of the icon.
function W2EF_LootName( inventory : CInventoryComponent, itemId : SItemUniqueId, text : string ) : string
{
    var markState : int;

    markState = W2EF_BookStatus( inventory, itemId );
    if ( markState == 0 )
    {
        return text;
    }
    return text + " / " + W2EF_Text( markState );
}

function W2EF_LootIcon( inventory : CInventoryComponent, itemId : SItemUniqueId, icon : string ) : string
{
    var markState : int;

    markState = W2EF_BookStatus( inventory, itemId );
    if ( markState == 0 || markState == 3 )
    {
        return icon;
    }
    return "<img src='img://globals/gui/icons/tooltip/alreadyread_15x15.dds' width='15' height='15'>" + icon;
}
'@

# BookMarker's texts per game text language (theGame.GetGameLanguageName, the codes CGuiMainMenu
# checks). Order: 1 Read, 2 Nothing new, 3 Unread (list labels), 4 read, 5 nothing new, 6 not read,
# 7 " Reading it gives you:", 8 lore only (tooltip), 9 "New: ", 10 "Known: ", 11 knowledge line,
# 12 current level, 13 bestiary entry, 14 journal entry, 15 next page; %1 is filled in.
# Non-ASCII is written as \uXXXX here and as StrChar( n ) in the script, so neither this file
# nor the game's script compiler depends on a code page. BR and KR ship no texts of their own
# (their .w2strings hold English) and get English. tools\script-patches\check_texts.py checks
# every character against the game's own texts for that language (the font has those).
$BookTexts = [ordered]@{
    EN = @(
        "Read",
        "Nothing new",
        "Unread",
        "You have read this book.",
        "Not read, but you already know everything it teaches:",
        "Not read yet.",
        " Reading it gives you:",
        "No bestiary knowledge or journal entries, only the text.",
        "New: ",
        "Known: ",
        "All knowledge on %1",
        " (you have %1)",
        "bestiary entry",
        "journal entry",
        "next page"
    )
    PL = @(
        "Przeczytana",
        "Nic nowego",
        "Nieprzeczytana",
        "Przeczyta\u0142e\u015B t\u0119 ksi\u0105\u017Ck\u0119.",
        "Nieprzeczytana, ale znasz ju\u017C wszystko, czego uczy:",
        "Jeszcze nieprzeczytana.",
        " Po przeczytaniu otrzymasz:",
        "Brak wiedzy o potworach i wpis\u00F3w w dzienniku, tylko tekst.",
        "Nowe: ",
        "Znane: ",
        "Pe\u0142na wiedza: %1",
        " (masz %1)",
        "wpis w bestiariuszu",
        "wpis w dzienniku",
        "kolejna strona"
    )
    DE = @(
        "Gelesen",
        "Nichts Neues",
        "Ungelesen",
        "Du hast dieses Buch gelesen.",
        "Nicht gelesen, aber du kennst bereits alles, was es lehrt:",
        "Noch nicht gelesen.",
        " Beim Lesen erh\u00E4ltst du:",
        "Kein Monsterwissen und keine Tagebucheintr\u00E4ge, nur der Text.",
        "Neu: ",
        "Bekannt: ",
        "Gesamtes Wissen \u00FCber %1",
        " (du hast %1)",
        "Bestiarium-Eintrag",
        "Tagebucheintrag",
        "n\u00E4chste Seite"
    )
    FR = @(
        "Lu",
        "Rien de nouveau",
        "Non lu",
        "Vous avez lu ce livre.",
        "Non lu, mais vous savez d\u00E9j\u00E0 tout ce qu'il enseigne :",
        "Pas encore lu.",
        " Sa lecture vous apporte :",
        "Aucune connaissance sur les monstres ni entr\u00E9e de journal, seulement le texte.",
        "Nouveau : ",
        "Connu : ",
        "Connaissances compl\u00E8tes : %1",
        " (vous avez %1)",
        "entr\u00E9e du bestiaire",
        "entr\u00E9e du journal",
        "page suivante"
    )
    ES = @(
        "Le\u00EDdo",
        "Nada nuevo",
        "Sin leer",
        "Has le\u00EDdo este libro.",
        "Sin leer, pero ya sabes todo lo que ense\u00F1a:",
        "A\u00FAn sin leer.",
        " Al leerlo obtienes:",
        "Sin conocimientos de monstruos ni entradas del diario, solo el texto.",
        "Nuevo: ",
        "Conocido: ",
        "Conocimiento completo: %1",
        " (tienes %1)",
        "entrada del bestiario",
        "entrada del diario",
        "p\u00E1gina siguiente"
    )
    IT = @(
        "Letto",
        "Niente di nuovo",
        "Non letto",
        "Hai letto questo libro.",
        "Non letto, ma conosci gi\u00E0 tutto ci\u00F2 che insegna:",
        "Non ancora letto.",
        " Leggendolo ottieni:",
        "Nessuna conoscenza sui mostri n\u00E9 voci del diario, solo il testo.",
        "Nuovo: ",
        "Noto: ",
        "Conoscenza completa: %1",
        " (hai %1)",
        "voce del bestiario",
        "voce del diario",
        "pagina successiva"
    )
    RU = @(
        "\u041F\u0440\u043E\u0447\u0438\u0442\u0430\u043D\u043E",
        "\u041D\u0438\u0447\u0435\u0433\u043E \u043D\u043E\u0432\u043E\u0433\u043E",
        "\u041D\u0435 \u043F\u0440\u043E\u0447\u0438\u0442\u0430\u043D\u043E",
        "\u0412\u044B \u043F\u0440\u043E\u0447\u0438\u0442\u0430\u043B\u0438 \u044D\u0442\u0443 \u043A\u043D\u0438\u0433\u0443.",
        "\u041D\u0435 \u043F\u0440\u043E\u0447\u0438\u0442\u0430\u043D\u043E, \u043D\u043E \u0432\u044B \u0443\u0436\u0435 \u0437\u043D\u0430\u0435\u0442\u0435 \u0432\u0441\u0451, \u0447\u0435\u043C\u0443 \u043E\u043D\u0430 \u0443\u0447\u0438\u0442:",
        "\u0415\u0449\u0451 \u043D\u0435 \u043F\u0440\u043E\u0447\u0438\u0442\u0430\u043D\u043E.",
        " \u0427\u0442\u0435\u043D\u0438\u0435 \u0434\u0430\u0441\u0442 \u0432\u0430\u043C:",
        "\u041D\u0435\u0442 \u0437\u043D\u0430\u043D\u0438\u0439 \u043E \u0447\u0443\u0434\u043E\u0432\u0438\u0449\u0430\u0445 \u0438 \u0437\u0430\u043F\u0438\u0441\u0435\u0439 \u0432 \u0436\u0443\u0440\u043D\u0430\u043B\u0435, \u0442\u043E\u043B\u044C\u043A\u043E \u0442\u0435\u043A\u0441\u0442.",
        "\u041D\u043E\u0432\u043E\u0435: ",
        "\u0418\u0437\u0432\u0435\u0441\u0442\u043D\u043E: ",
        "\u041F\u043E\u043B\u043D\u044B\u0435 \u0437\u043D\u0430\u043D\u0438\u044F: %1",
        " (\u0443 \u0432\u0430\u0441 %1)",
        "\u0437\u0430\u043F\u0438\u0441\u044C \u0432 \u0431\u0435\u0441\u0442\u0438\u0430\u0440\u0438\u0438",
        "\u0437\u0430\u043F\u0438\u0441\u044C \u0432 \u0436\u0443\u0440\u043D\u0430\u043B\u0435",
        "\u0441\u043B\u0435\u0434\u0443\u044E\u0449\u0430\u044F \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0430"
    )
    CZ = @(
        "P\u0159e\u010Dteno",
        "Nic nov\u00E9ho",
        "Nep\u0159e\u010Dteno",
        "Tuto knihu jsi p\u0159e\u010Detl.",
        "Nep\u0159e\u010Dteno, ale v\u0161e, co u\u010D\u00ED, u\u017E zn\u00E1\u0161:",
        "Zat\u00EDm nep\u0159e\u010Dteno.",
        " P\u0159e\u010Dten\u00EDm z\u00EDsk\u00E1\u0161:",
        "\u017D\u00E1dn\u00E9 znalosti o netvorech ani z\u00E1znamy v den\u00EDku, jen text.",
        "Nov\u00E9: ",
        "Zn\u00E1m\u00E9: ",
        "\u00DApln\u00E9 znalosti: %1",
        " (m\u00E1\u0161 %1)",
        "z\u00E1znam v besti\u00E1\u0159i",
        "z\u00E1znam v den\u00EDku",
        "dal\u0161\u00ED strana"
    )
    HU = @(
        "Elolvasva",
        "Semmi \u00FAj",
        "Olvasatlan",
        "Ezt a k\u00F6nyvet m\u00E1r elolvastad.",
        "Olvasatlan, de m\u00E1r mindent tudsz, amit tan\u00EDt:",
        "M\u00E9g nem olvastad.",
        " Elolvasva ezt kapod:",
        "Nincs benne sz\u00F6rnyismeret vagy napl\u00F3bejegyz\u00E9s, csak a sz\u00F6veg.",
        "\u00DAj: ",
        "Ismert: ",
        "Teljes tud\u00E1s: %1",
        " (neked: %1)",
        "besti\u00E1rium-bejegyz\u00E9s",
        "napl\u00F3bejegyz\u00E9s",
        "k\u00F6vetkez\u0151 oldal"
    )
    TR = @(
        "Okundu",
        "Yeni bir \u015Fey yok",
        "Okunmad\u0131",
        "Bu kitab\u0131 okudun.",
        "Okunmad\u0131, ama \u00F6\u011Fretti\u011Fi her \u015Feyi zaten biliyorsun:",
        "Hen\u00FCz okunmad\u0131.",
        " Okursan \u015Funlar\u0131 kazan\u0131rs\u0131n:",
        "Canavar bilgisi ya da g\u00FCnl\u00FCk kayd\u0131 yok, sadece metin.",
        "Yeni: ",
        "Bilinen: ",
        "T\u00FCm bilgi: %1",
        " (sende %1)",
        "bestiyer kayd\u0131",
        "g\u00FCnl\u00FCk kayd\u0131",
        "sonraki sayfa"
    )
    JP = @(
        "\u65E2\u8AAD",
        "\u65B0\u60C5\u5831\u306A\u3057",
        "\u672A\u8AAD",
        "\u3053\u306E\u672C\u306F\u8AAD\u3093\u3060\u3053\u3068\u304C\u3042\u308B\u3002",
        "\u672A\u8AAD\u3060\u304C\u3001\u5185\u5BB9\u306F\u3059\u3079\u3066\u77E5\u3063\u3066\u3044\u308B\uFF1A",
        "\u307E\u3060\u8AAD\u3093\u3067\u3044\u306A\u3044\u3002",
        " \u8AAD\u3080\u3068\u5F97\u3089\u308C\u308B\u3082\u306E\uFF1A",
        "\u602A\u7269\u306E\u77E5\u8B58\u3084\u65E5\u8A8C\u306E\u9805\u76EE\u306F\u306A\u304F\u3001\u672C\u6587\u306E\u307F\u3002",
        "\u65B0\u898F\uFF1A",
        "\u65E2\u77E5\uFF1A",
        "%1\u306E\u5B8C\u5168\u306A\u77E5\u8B58",
        "\uFF08\u73FE\u5728 %1\uFF09",
        "\u56F3\u9451\u306E\u9805\u76EE",
        "\u65E5\u8A8C\u306E\u9805\u76EE",
        "\u6B21\u306E\u30DA\u30FC\u30B8"
    )
    ZH = @(
        "\u5DF2\u8B80",
        "\u7121\u65B0\u5167\u5BB9",
        "\u672A\u8B80",
        "\u4F60\u5DF2\u8B80\u904E\u9019\u672C\u66F8\u3002",
        "\u672A\u8B80\uFF0C\u4F46\u4F60\u5DF2\u77E5\u9053\u5B83\u8A18\u8F09\u7684\u4E00\u5207\uFF1A",
        "\u5C1A\u672A\u95B1\u8B80\u3002",
        " \u95B1\u8B80\u5F8C\u53EF\u7372\u5F97\uFF1A",
        "\u6C92\u6709\u602A\u7269\u77E5\u8B58\u6216\u65E5\u8A8C\u689D\u76EE\uFF0C\u53EA\u6709\u5167\u6587\u3002",
        "\u65B0\uFF1A",
        "\u5DF2\u77E5\uFF1A",
        "%1\u7684\u5168\u90E8\u77E5\u8B58",
        "\uFF08\u4F60\u76EE\u524D %1\uFF09",
        "\u602A\u7269\u5716\u9451\u689D\u76EE",
        "\u65E5\u8A8C\u689D\u76EE",
        "\u4E0B\u4E00\u9801"
    )
}

# --- features -------------------------------------------------------------------------------

$Marker = "//===witcher2-enginefix==="
$ShopEntry = "game\gui\guishop.ws"
$InventoryEntry = "game\gui\guiinventory.ws"
$MenuEntry = "game\gui\guimainmenu.ws"
$UtilsEntry = "game\gui\guiutils.ws"
$ContainerEntry = "game\gameplay\container_class.ws"

# Main menu: shown when the menu has a save. saves[0] is the newest one (the engine picks the
# menu background from it, 009A515C); 9001 is a button id outside EMenuButtons.
$ContinueButton = @'
if ( saves.Size() == 0 )
{
    RefreshSaveData();
}
if ( saves.Size() > 0 )
{
    InsertMenuButton( AS_MenuItems, 9001, StrUpperUTF( GetLocStringByKeyExt( "menuContinue" ) ), StrUpperUTF( saves[ 0 ].displayName ) );
}
'@

# Same as picking the top entry of the Load list (refuses a save whose player is dead).
$ContinueInput = @'
if ( button == 9001 )
{
    InputLoadGame( 0 );
    return;
}
'@

$QuitButton = @'
InsertMenuButton( AS_MenuItems, MM_BTN_QUIT, StrUpperUTF( GetLocStringByKeyExt( "menuQuit" ) ), StrUpperUTF( GetLocStringByKeyExt( "menuQuit_t" ) ) );
'@

$QuitInput = @'
if ( button == MM_BTN_QUIT )
{
    W2EF_ConfirmQuit();
    return;
}
'@

# QuitApplication is a private import of CGuiMainMenu, so the confirmation box calls back here.
# The box lives in hud.swf; with the menu movie still open on top it only took the mouse after a
# first click, so the menu closes first and No opens it again (theHud.ShowMainMenu).
$QuitMembers = @'
function W2EF_ConfirmQuit()
{
    var confirm : W2EFConfirmQuit;

    ClosePanel();
    confirm = new W2EFConfirmQuit in theHud.m_messages;
    confirm.m_menu = this;
    theHud.m_messages.ShowConfirmationBox( confirm );
}

function W2EF_Quit()
{
    QuitApplication();
}

'@

$QuitBox = @'
class W2EFConfirmQuit extends CGuiConfirmationBox
{
    var m_menu : CGuiMainMenu;

    function GetText() : string
    {
        if ( GetLocStringByKeyExt( "Exit game?" ) == "" )
        {
            return "Quit the game?";
        }
        return GetLocStringByKeyExt( "Exit game?" );
    }

    function GetSelectionOfEscape() : bool
    {
        return false;
    }

    event OnYes()
    {
        m_menu.W2EF_Quit();
    }

    event OnNo()
    {
        theHud.ShowMainMenu();
    }
}
'@

$InputMainMenu = "private final function InputMainMenu\( button : int \)"
$InputMainInGame = "private final function InputMainInGame\( button : int \)"

# The exact statements in the game's scripts that BookMarker rewrites (a mod the tool does not
# know would have changed them, and then the match fails instead of half-patching).
# FillItemObject assembles the tooltip in one statement; the marker call goes right after it.
$FillTooltip = 'theHud.SetString( "DescFull", GetLocStringByKeyExt( "Tooltip" + itemName ) + descFull + "<br>" + fullDescTootlip, AS_item );'
# The merchant/storage side of FillShop gets a context, so carrying a copy marks the item too.
$ShopMerchant = "theHud.m_utils.FillItemObject( m_merchantInventory, m_playerStats, itemId, i, AS_item, slotItems ); // TODO: slotItems?"
$ShopMerchantPatched = "theHud.m_utils.FillItemObject( m_merchantInventory, m_playerStats, itemId, i, AS_item, slotItems, 'merchant' ); // TODO: slotItems?"
# FillLootWindow pushes the display name and the icon HTML once in its quest-item loop and once
# in its ordinary loop, so both statements occur twice.
$LootName = @'
args.PushBack( FlashValueFromString( GetLocStringByKeyExt( itemName ) ) );
'@
$LootNamePatched = @'
args.PushBack( FlashValueFromString( W2EF_LootName( inventory, itemId, GetLocStringByKeyExt( itemName ) ) ) );
'@
$LootIcon = @'
args.PushBack( FlashValueFromString( "<img src='img://globals/gui/icons/items/" + StrReplaceAll( StrReplaceAll( itemName , " ", ""), "'", "" ) + "_64x64.dds' width='30' height='30'>" ) ) ;
'@
$LootIconPatched = @'
args.PushBack( FlashValueFromString( W2EF_LootIcon( inventory, itemId, "<img src='img://globals/gui/icons/items/" + StrReplaceAll( StrReplaceAll( itemName , " ", ""), "'", "" ) + "_64x64.dds' width='30' height='30'>" ) ) );
'@
$LootName = $LootName.Trim()
$LootNamePatched = $LootNamePatched.Trim()
$LootIcon = $LootIcon.Trim()
$LootIconPatched = $LootIconPatched.Trim()

# Each edit inserts Code next to the single line matching Anchor (regex): Before or After that
# line, or at the end of the file when Anchor is $null. After + Body: Anchor ends with the "{" of
# a function and the code goes first in its body. Replace: the anchor must match whole lines and
# each of them becomes the code (Count lines, or Multiple for any number). Code is indented with
# 4 spaces here and gets tabs plus the indentation of the line it lands at.
$FeatureList = [ordered]@{
    BookMarker = @{
        Title = "Book marker: Read / Nothing new / Unread in the inventory, shop, trade and loot window"
        Swf   = $true
        Edits = @(
            # One call at the end of the shared item fill: every panel that lists items gets it.
            @{ Entry = $UtilsEntry; Where = "After"; Anchor = [regex]::Escape($FillTooltip)
               Code = "W2EF_MarkItem( inventory, itemId, AS_item, custom );" },
            @{ Entry = $UtilsEntry; Where = "End"; Anchor = $null; Code = "BOOK_HELPERS" },
            # A merchant's or the storage chest's list also marks a book Geralt carries.
            @{ Entry = $ShopEntry; Where = "Replace"; Anchor = [regex]::Escape($ShopMerchant)
               Code = $ShopMerchantPatched },
            # Loot window rows: the status goes after the name and a tick in front of the icon.
            @{ Entry = $ContainerEntry; Where = "Replace"; Count = 2
               Anchor = [regex]::Escape($LootName); Code = $LootNamePatched },
            @{ Entry = $ContainerEntry; Where = "Replace"; Count = 2
               Anchor = [regex]::Escape($LootIcon); Code = $LootIconPatched }
        )
    }
    Continue = @{
        Title = "Continue button in the main menu (loads the newest save)"
        Swf   = $false
        Edits = @(
            @{ Entry = $MenuEntry; Where = "Before"; Anchor = [regex]::Escape("InsertMenuButton( AS_MenuItems, MM_BTN_ARENA,"); Code = $ContinueButton },
            @{ Entry = $MenuEntry; Where = "Body"; Anchor = $InputMainMenu + "\s*\{"; Code = $ContinueInput }
        )
    }
    PauseQuit = @{
        Title = "Quit button in the pause menu (asks first)"
        Swf   = $false
        Edits = @(
            @{ Entry = $MenuEntry; Where = "After"; Anchor = [regex]::Escape("InsertMenuButton( AS_MenuItems, MM_BTN_EXIT_GAME,"); Code = $QuitButton },
            @{ Entry = $MenuEntry; Where = "Body"; Anchor = $InputMainInGame + "\s*\{"; Code = $QuitInput },
            @{ Entry = $MenuEntry; Where = "Before"; Anchor = $InputMainMenu; Code = $QuitMembers },
            @{ Entry = $MenuEntry; Where = "End"; Anchor = $null; Code = $QuitBox }
        )
    }
}

# CompareInventories, merchant loop: if (!(item.Mask & books) > 0) skip 24 bytes;
#   item._alreadyRead = (owned[item.Name] != null)
# becomes the same if followed by "push r10, null x9; pop x10" (24 bytes, stack-neutral).
$SwfIf = @(0x9D, 0x02, 0x00, 0x18, 0x00)
$SwfOriginal = $SwfIf + @(0x96, 0x0C, 0x00, 0x04, 0x0A, 0x09, -1, -1, 0x04, 0x03, 0x04, 0x0A, 0x09, -1, -1,
                          0x4E, 0x4E, 0x96, 0x01, 0x00, 0x02, 0x49, 0x12, 0x4F)
$SwfPatch = @(0x96, 0x0B, 0x00, 0x04, 0x0A) + @(0x02) * 9 + @(0x17) * 10
$SwfPatched = $SwfIf + $SwfPatch
$SwfNames = @("ui_trade.swf", "ui_storage.swf")

# --- helpers --------------------------------------------------------------------------------

$Latin1 = [Text.Encoding]::GetEncoding(28591)

function Sha256([byte[]]$data) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($data)) -replace "-", "").ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Write-Atomic([string]$path, [byte[]]$data) {
    $tmp = "$path.w2ef.tmp"
    [IO.File]::WriteAllBytes($tmp, $data)
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    Move-Item -LiteralPath $tmp -Destination $path
}

function Get-EntryText([byte[]]$dzip, [string]$name) {
    $table = 0L
    $e = [Dzip]::Find([Dzip]::ReadTable($dzip, [ref]$table), $name)
    if (-not $e) { throw "$name not in base_scripts.dzip" }
    return $Latin1.GetString([Dzip]::Extract($dzip, $e))
}

# The braces and body of CGuiInventory.AddBookKnowledge with thePlayer.MaxKnowledgeAccumulator /
# thePlayer.AddJournalEntry pointed at W2EFBookGrants. Anything else touching thePlayer refuses.
function Get-CollectBody([string]$inv) {
    $m = [regex]::Matches($inv, "function\s+AddBookKnowledge\s*\(\s*itemName\s*:\s*name\s*\)\s*\{")
    if ($m.Count -ne 1) { throw "AddBookKnowledge found $($m.Count) times in $InventoryEntry" }
    $start = $m[0].Index + $m[0].Length - 1
    $depth = 0
    for ($k = $start; $k -lt $inv.Length; $k++) {
        if ($inv[$k] -eq "{") { $depth++ }
        elseif ($inv[$k] -eq "}") { $depth--; if ($depth -eq 0) { break } }
    }
    if ($depth -ne 0) { throw "AddBookKnowledge has unbalanced braces" }
    $body = $inv.Substring($start, $k - $start + 1)
    $body = $body.Replace("thePlayer.MaxKnowledgeAccumulator(", "Knowledge(").Replace("thePlayer.AddJournalEntry(", "Journal(")
    if ($body -match "thePlayer") { throw "AddBookKnowledge does more than grant knowledge and journal entries" }
    if ($body -notmatch "Knowledge\(" -or $body -notmatch "Journal\(") { throw "AddBookKnowledge grants nothing recognisable" }
    return $body.Replace("`r`n", "`n")
}

# "Przeczyta\u0142" (U+0142) -> "Przeczyta" + StrChar( 322 ). StrChar (native 008E5A0A) builds the string
# from one UTF-16 unit.
function ConvertTo-ScriptString([string]$s) {
    $parts = @()
    $run = ""
    foreach ($c in $s.ToCharArray()) {
        if ([int]$c -lt 128) { $run += $c }
        else {
            if ($run) { $parts += '"' + $run + '"'; $run = "" }
            $parts += "StrChar( $([int]$c) )"
        }
    }
    if ($run -or $parts.Count -eq 0) { $parts += '"' + $run + '"' }
    return $parts -join " + "
}

# W2EF_Text( id ): $BookTexts in the game's text language, English otherwise.
function New-TextFunction {
    $lines = @("function W2EF_Text( id : int ) : string", "{", "    var audioLang, textLang : string;", "",
               "    theGame.GetGameLanguageName( audioLang, textLang );")
    foreach ($lang in @($BookTexts.Keys | Where-Object { $_ -ne "EN" }) + @("EN")) {
        $texts = $BookTexts[$lang]
        if ($texts.Count -ne $BookTexts["EN"].Count) { throw "BookTexts.$lang has $($texts.Count) texts, EN has $($BookTexts['EN'].Count)" }
        $pad = "    "
        if ($lang -ne "EN") { $lines += @("    if ( textLang == `"$lang`" )", "    {"); $pad = "        " }
        for ($i = 0; $i -lt $texts.Count; $i++) {
            $lines += "${pad}if ( id == $($i + 1) ) { return $(ConvertTo-ScriptString ([regex]::Unescape($texts[$i]))); }"
        }
        if ($lang -ne "EN") { $lines += "    }" }
    }
    $lines += @("    return `"`";", "}")
    return $lines -join "`n"
}

function ConvertTo-Tabs([string]$code) {
    return [regex]::Replace($code.Replace("`r`n", "`n"), "(?m)^((?:    )+)", { param($m) "`t" * ($m.Groups[1].Length / 4) })
}

# Inserts one edit, wrapped in BEGIN/END marker lines, into $text.
# Where Replace: the anchor must match whole lines (as many as Count, or any number when
# Multiple), and each of those lines is replaced by the code, still inside its indentation.
function Add-Edit([string]$text, $edit, [string]$feature, [string]$collect) {
    if ($edit.Code -eq "BOOK_HELPERS") {
        $code = (ConvertTo-Tabs $BookHelpers).Replace("__COLLECT_BODY__", "`t" + $collect.TrimEnd()).Replace(
            "__TEXT_FUNCTION__", (ConvertTo-Tabs (New-TextFunction)))
    }
    else { $code = ConvertTo-Tabs $edit.Code }
    $lines = @("$Marker BEGIN $feature") + $code.TrimEnd("`n").Split("`n") + @("$Marker END $feature")

    if ($edit.Where -eq "End") {
        $eol = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
        if (-not $text.EndsWith("`n")) { $text += $eol }
        return $text + $eol + (($lines | ForEach-Object { $_ + $eol }) -join "")
    }

    if ($edit.Where -eq "Replace") {
        $hits = [regex]::Matches($text, $edit.Anchor)
        $want = if ($edit.ContainsKey("Count")) { [int]$edit["Count"] } elseif ($edit["Multiple"]) { -1 } else { 1 }
        if ($hits.Count -eq 0 -or ($want -ge 0 -and $hits.Count -ne $want)) {
            throw "anchor '$($edit.Anchor)' found $($hits.Count) times in $($edit.Entry), expected $(if ($want -ge 0) { $want } else { "at least one" })"
        }
        $eol = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
        for ($k = $hits.Count - 1; $k -ge 0; $k--) {
            $hit = $hits[$k]
            $lineStart = if ($hit.Index -eq 0) { 0 } else { $text.LastIndexOf("`n", $hit.Index - 1) + 1 }
            $lineStop = $text.IndexOf("`n", $hit.Index + $hit.Length)
            $contentEnd = if ($lineStop -lt 0) { $text.Length }
                          elseif ($lineStop -gt 0 -and $text[$lineStop - 1] -eq "`r") { $lineStop - 1 }
                          else { $lineStop }
            $before = $text.Substring($lineStart, $hit.Index - $lineStart)
            $after = $text.Substring($hit.Index + $hit.Length, $contentEnd - ($hit.Index + $hit.Length))
            if ($before.Trim() -ne "" -or $after.Trim() -ne "") {
                throw "anchor '$($edit.Anchor)' does not cover a whole line in $($edit.Entry)"
            }
            $indent = [regex]::Match($before, "^[ \t]*").Value
            $block = ($lines | ForEach-Object { $indent + $_ }) -join $eol
            $text = $text.Substring(0, $lineStart) + $block + $text.Substring($contentEnd)
        }
        return $text
    }

    $m = [regex]::Matches($text, $edit.Anchor)
    if ($m.Count -ne 1) { throw "anchor '$($edit.Anchor)' found $($m.Count) times in $($edit.Entry)" }
    $first = $text.LastIndexOf("`n", [Math]::Max($m[0].Index - 1, 0))
    $lineStart = if ($m[0].Index -eq 0) { 0 } else { $first + 1 }
    $lastLineEnd = $text.IndexOf("`n", $m[0].Index + $m[0].Length - 1)
    if ($lastLineEnd -lt 0) { throw "anchor '$($edit.Anchor)' is on the last line of $($edit.Entry)" }
    $eol = if ($lastLineEnd -gt 0 -and $text[$lastLineEnd - 1] -eq "`r") { "`r`n" } else { "`n" }
    $afterAnchor = $lastLineEnd + 1
    $anchorLineStart = $text.LastIndexOf("`n", $lastLineEnd - 1) + 1
    switch ($edit.Where) {
        "Before" { $at = $lineStart; $indentFrom = $lineStart }
        "After"  { $at = $afterAnchor; $indentFrom = $anchorLineStart }
        "Body"   { $at = $afterAnchor; $indentFrom = $afterAnchor }
        default  { throw "unknown edit position $($edit.Where)" }
    }
    $indent = [regex]::Match($text.Substring($indentFrom), "^[ \t]*").Value
    $block = ($lines | ForEach-Object { if ($_) { $indent + $_ + $eol } else { $eol } }) -join ""
    return $text.Insert($at, $block)
}

# Applies the chosen features to the original archive; returns the new archive bytes.
function Build-Scripts([byte[]]$orig, [string[]]$chosen) {
    $texts = [ordered]@{}
    $collect = $null
    foreach ($name in $chosen) {
        foreach ($edit in $FeatureList[$name].Edits) {
            if (-not $texts.Contains($edit.Entry)) { $texts[$edit.Entry] = Get-EntryText $orig $edit.Entry }
            if ($edit.Code -eq "BOOK_HELPERS" -and -not $collect) { $collect = Get-CollectBody (Get-EntryText $orig $InventoryEntry) }
            $texts[$edit.Entry] = Add-Edit $texts[$edit.Entry] $edit $name $collect
        }
    }
    if ($texts.Count -eq 0) { return $orig }
    $names = [string[]]@($texts.Keys)
    $datas = [byte[][]]@($names | ForEach-Object { , $Latin1.GetBytes($texts[$_]) })
    return [Dzip]::ReplaceEntries($orig, $names, $datas)
}

function Get-SwfState([byte[]]$swf) {
    $orig = [Swf]::FindAll($swf, [int[]]$SwfOriginal)
    $done = [Swf]::FindAll($swf, [int[]]$SwfPatched)
    if ($done.Count -eq 1 -and $orig.Count -eq 0) { return @{ State = "patched" } }
    if ($orig.Count -eq 1 -and $done.Count -eq 0) { return @{ State = "original"; At = $orig[0] } }
    return @{ State = "unknown"; Why = "pattern found $($orig.Count) times, patched form $($done.Count) times" }
}

function Get-PatchedSwf([byte[]]$swf) {
    $s = Get-SwfState $swf
    if ($s.State -ne "original") { throw "swf: $($s.Why)" }
    $new = [byte[]]$swf.Clone()
    $at = $s.At + $SwfIf.Count
    for ($k = 0; $k -lt $SwfPatch.Count; $k++) { $new[$at + $k] = [byte]$SwfPatch[$k] }
    return $new
}

function Read-Manifest {
    $m = [ordered]@{}
    if (Test-Path -LiteralPath $manifestPath) {
        foreach ($line in Get-Content -LiteralPath $manifestPath) {
            $leaf, $mode, $hash = $line -split "`t"
            $m[$leaf] = @{ Mode = $mode; Hash = $hash }
        }
    }
    return $m
}

function Save-Manifest($m) {
    if ($m.Count -eq 0) {
        if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
        return
    }
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    @($m.Keys | ForEach-Object { "$_`t$($m[$_].Mode)`t$($m[$_].Hash)" }) | Set-Content -LiteralPath $manifestPath -Encoding ASCII
}

# --- locate the game ------------------------------------------------------------------------

if (-not $GameDir) { $GameDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$cooked = Join-Path $GameDir "CookedPC"
$scriptsPath = Join-Path $cooked "base_scripts.dzip"
$guiDir = Join-Path $cooked "globals\gui"
$abetterui = Join-Path $cooked "abetterui.dzip"
if (-not (Test-Path -LiteralPath $scriptsPath)) {
    throw "No CookedPC\base_scripts.dzip under '$GameDir'. Run from <game>\Witcher2EngineFix_Optional\ScriptPatches or pass -GameDir."
}
$backup = Join-Path $PSScriptRoot "backup"
$manifestPath = Join-Path $backup "manifest.txt"
$manifest = Read-Manifest

if ($Action -ne "Check") {
    $running = Get-Process -Name witcher2 -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($GameDir + "\", [StringComparison]::OrdinalIgnoreCase) }
    if ($running) { throw "The Witcher 2 is running. Close the game first." }
}

function Get-LeafPath([string]$leaf) {
    if ($leaf -eq "base_scripts.dzip") { return $scriptsPath }
    return Join-Path $guiDir $leaf
}

# --- uninstall ------------------------------------------------------------------------------

if ($Action -eq "Uninstall") {
    if ($manifest.Count -eq 0) { Write-Output "Not installed (no backup manifest)."; return }
    foreach ($leaf in @($manifest.Keys)) {
        $path = Get-LeafPath $leaf
        if (-not (Test-Path -LiteralPath $path)) { Write-Output "$leaf is gone, skipped"; continue }
        if ((Sha256 ([IO.File]::ReadAllBytes($path))) -ne $manifest[$leaf].Hash) {
            Write-Output "$leaf was changed after patching (another mod or update?), left as it is"
            continue
        }
        if ($manifest[$leaf].Mode -eq "created") {
            Remove-Item -LiteralPath $path -Force
            Write-Output "Removed $leaf (game uses its packed copy again)"
        }
        else {
            Write-Atomic $path ([IO.File]::ReadAllBytes((Join-Path $backup $leaf)))
            Write-Output "Restored $leaf"
        }
    }
    Remove-Item -LiteralPath $backup -Recurse -Force
    Write-Output "Uninstalled."
    return
}

# --- originals: the backup when the current file is still ours, else the current file ---------

# Each target: Path, Leaf, Orig (bytes before any patch), Loose (orig existed as a loose file),
# Current (bytes in the game now).
function Get-Target([string]$leaf, [string]$path, [scriptblock]$fromArchive) {
    $exists = Test-Path -LiteralPath $path
    $current = if ($exists) { [IO.File]::ReadAllBytes($path) } else { $null }
    if ($manifest.Contains($leaf)) {
        if (-not $exists -or (Sha256 $current) -ne $manifest[$leaf].Hash) {
            throw "$leaf was changed after patching (another mod or update?). Run Uninstall.bat, then Install.bat."
        }
        if ($manifest[$leaf].Mode -eq "created") { return @{ Leaf = $leaf; Path = $path; Orig = (& $fromArchive); Loose = $false; Current = $current } }
        return @{ Leaf = $leaf; Path = $path; Orig = [IO.File]::ReadAllBytes((Join-Path $backup $leaf)); Loose = $true; Current = $current }
    }
    if ($exists) { return @{ Leaf = $leaf; Path = $path; Orig = $current; Loose = $true; Current = $current } }
    return @{ Leaf = $leaf; Path = $path; Orig = (& $fromArchive); Loose = $false; Current = $null }
}

$scripts = Get-Target "base_scripts.dzip" $scriptsPath { throw "base_scripts.dzip missing" }
$patchedEntries = @($UtilsEntry, $ShopEntry, $MenuEntry, $ContainerEntry)
foreach ($entry in $patchedEntries) {
    if ((Get-EntryText $scripts.Orig $entry).Contains($Marker)) {
        throw "$entry already holds witcher2-enginefix changes from another copy of this tool (BookReadMarker?). Run that copy's Uninstall.bat first."
    }
}
$swfs = @()
foreach ($name in $SwfNames) {
    $swfs += Get-Target $name (Join-Path $guiDir $name) {
        $ab = [IO.File]::ReadAllBytes($abetterui)
        $t = 0L
        $e = [Dzip]::Find([Dzip]::ReadTable($ab, [ref]$t), "globals\gui\$name")
        if (-not $e) { throw "$name not found in abetterui.dzip" }
        [Swf]::Uncompressed([Dzip]::Extract($ab, $e))
    }.GetNewClosure()
}
foreach ($t in $swfs) {
    $t.Raw = $t.Orig   # as found; backups and restores use these bytes
    $t.Orig = [Swf]::Uncompressed($t.Orig)
    $s = Get-SwfState $t.Orig
    if ($s.State -ne "original") { throw "$($t.Leaf): $($s.Why). Another copy of this tool may have patched it; run its Uninstall.bat first." }
}

# --- check ----------------------------------------------------------------------------------

$currentText = if ($scripts.Current) { ($patchedEntries | ForEach-Object { Get-EntryText $scripts.Current $_ }) -join "" } else { "" }
$installed = @($FeatureList.Keys | Where-Object { $currentText.Contains("$Marker BEGIN $_") })

foreach ($name in $FeatureList.Keys) {
    try { [void](Build-Scripts $scripts.Orig @($name)); $ok = "can be installed" }
    catch { $ok = "CANNOT be installed: $($_.Exception.Message)" }
    $state = if ($installed -contains $name) { "installed" } else { "not installed" }
    Write-Output ("{0,-11} {1,-14} {2}" -f $name, $state, $ok)
}

if ($Action -eq "Check") { return }

# --- install --------------------------------------------------------------------------------

if ($Features.Count -eq 0) {
    Write-Output ""
    Write-Output "Choose the patches to have installed (Enter = yes). Anything not chosen is removed."
    $chosen = @()
    foreach ($name in $FeatureList.Keys) {
        $answer = Read-Host "  $($FeatureList[$name].Title) [Y/n]"
        if ($answer -eq "" -or $answer -match "^[yYjJ]") { $chosen += $name }
    }
}
elseif ($Features -contains "All") { $chosen = @($FeatureList.Keys) }
else {
    $chosen = @($FeatureList.Keys | Where-Object { $Features -contains $_ })
    $unknown = @($Features | Where-Object { -not $FeatureList.Contains($_) })
    if ($unknown) { throw "Unknown feature(s): $($unknown -join ', '). Known: $($FeatureList.Keys -join ', '), All" }
}

$wantSwf = @($chosen | Where-Object { $FeatureList[$_].Swf }).Count -gt 0
$scripts.Desired = Build-Scripts $scripts.Orig $chosen
foreach ($t in $swfs) { $t.Desired = if ($wantSwf) { Get-PatchedSwf $t.Orig } else { $t.Orig } }

foreach ($t in @($scripts) + $swfs) {
    $raw = if ($t.Raw) { $t.Raw } else { $t.Orig }
    $origHash = if ($null -eq $t.Orig) { "" } else { Sha256 $t.Orig }
    $newHash = if ($null -eq $t.Desired) { "" } else { Sha256 $t.Desired }
    if ($newHash -eq $origHash) {
        if ($manifest.Contains($t.Leaf)) {
            if ($t.Loose) { Write-Atomic $t.Path $raw; Write-Output "Restored $($t.Leaf)" }
            else { Remove-Item -LiteralPath $t.Path -Force; Write-Output "Removed $($t.Leaf) (game uses its packed copy again)" }
            $bak = Join-Path $backup $t.Leaf
            if (Test-Path -LiteralPath $bak) { Remove-Item -LiteralPath $bak -Force }
            $manifest.Remove($t.Leaf)
        }
        continue
    }
    if ($t.Loose) {
        New-Item -ItemType Directory -Force -Path $backup | Out-Null
        $bak = Join-Path $backup $t.Leaf
        if (-not (Test-Path -LiteralPath $bak)) { [IO.File]::WriteAllBytes($bak, $raw) }
        $mode = "replaced"
    }
    else { $mode = "created" }
    if (-not $t.Current -or (Sha256 $t.Current) -ne $newHash) {
        Write-Atomic $t.Path $t.Desired
        Write-Output "Patched $($t.Leaf) ($mode)"
    }
    $manifest[$t.Leaf] = @{ Mode = $mode; Hash = $newHash }
    Save-Manifest $manifest
}
Save-Manifest $manifest

if ($chosen.Count -eq 0) { Write-Output "No patches chosen: the original files are back in place." }
else { Write-Output "Installed: $($chosen -join ', '). Originals are in $backup" }
