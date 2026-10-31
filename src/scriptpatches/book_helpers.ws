// Books anywhere in the UI: _alreadyRead (the A Better UI overlay) when the book was read
// (<item>_Examined, set by CGuiInventory.ExamineItem); a merchant's or the storage chest's list
// also marks a carried copy. Books get a short status (Read / Nothing new / Unread) after their
// type in the list and a few lines in the tooltip. The loot window (ui_loot.swf) has no tooltip:
// there a read book's icon carries the tick and "nothing new" goes after the name.
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
// else only having read the book does. "Nothing new" is told in the text, not by the tick: such a
// book still has a text the player has not read.
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
    theHud.SetBool( "_alreadyRead", markState == 1 || isOwned, AS_item );

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
// script builds as HTML, and no tooltip of any kind. A read book shows the tick on its icon and
// an unread one nothing, so only "nothing new" goes after the name.
function W2EF_LootName( inventory : CInventoryComponent, itemId : SItemUniqueId, text : string ) : string
{
    if ( W2EF_BookStatus( inventory, itemId ) != 2 )
    {
        return text;
    }
    return text + " (" + W2EF_Text( 2 ) + ")";
}

// Loot window rows and the loot preview next to a container have no overlay of their own, so a
// read book shows a copy of its icon with the inventory's tick drawn in (written by the patcher
// to globals\gui\icons\items\w2ef_read\, see W2EF_HasReadIcon).
// icon is the game's own icon path or <img> tag; anything else is returned as it is.
function W2EF_ReadIcon( inventory : CInventoryComponent, itemId : SItemUniqueId, icon : string ) : string
{
    var markState : int;

    markState = W2EF_BookStatus( inventory, itemId );
    if ( markState != 1 || ! W2EF_HasReadIcon( inventory.GetItemName( itemId ) ) )
    {
        return icon;
    }
    return StrReplace( icon, "/icons/items/", "/icons/items/w2ef_read/" );
}

__READ_ICONS__