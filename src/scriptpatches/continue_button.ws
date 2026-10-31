if ( saves.Size() == 0 )
{
    RefreshSaveData();
}
if ( saves.Size() > 0 )
{
    InsertMenuButton( AS_MenuItems, 9001, StrUpperUTF( GetLocStringByKeyExt( "menuContinue" ) ), StrUpperUTF( saves[ 0 ].displayName ) );
}