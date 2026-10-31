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