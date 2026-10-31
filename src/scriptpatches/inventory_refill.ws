private final function W2EF_RefillAfterReading()
{
    var AS_panel, AS_list : int;
    var hasPanel, hasList, keepScroll : bool;
    var offset, position : float;

    hasList = false;
    keepScroll = false;
    hasPanel = theHud.GetObject( "pPanelClass", AS_panel );
    if ( hasPanel )
    {
        hasList = theHud.GetObject( "m_mcItemList", AS_list, AS_panel );
    }
    if ( hasList )
    {
        keepScroll = theHud.GetFloat( "m_iCurrentOffset", offset, AS_list ) && theHud.GetFloat( "m_iCurrentPosition", position, AS_list );
    }

    ProcessInventoryChanges();
    FillInventory();

    if ( keepScroll )
    {
        theHud.InvokeOneArg( "SetOffset", FlashValueFromInt( (int)offset ), AS_list );
        theHud.InvokeOneArg( "SetPosition", FlashValueFromInt( (int)position ), AS_list );
        theHud.Invoke( "UpdateListData", AS_list );
        theHud.Invoke( "UpdateGraphic", AS_list );
    }
    if ( hasList )
    {
        theHud.ForgetObject( AS_list );
    }
    if ( hasPanel )
    {
        theHud.ForgetObject( AS_panel );
    }
}
