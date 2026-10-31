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
