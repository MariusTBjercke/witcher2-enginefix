// The Display mode row: 0 = full screen, 1 = borderless window, 2 = windowed. It reads the
// values the game itself starts with; missing keys mean windowed (the engine's bool reader leaves
// the setting at 0 when the key is absent).
function W2EF_WindowModeIndex() : int
{
    var value : float;

    value = 0.0;
    if ( theGame.ReadConfigParamFloat( "User", "Rendering", "Fullscreen", value ) )
    {
        if ( value != 0.0 ) { return 0; }
    }
    value = 0.0;
    if ( theGame.ReadConfigParamFloat( "User", "Rendering", "EngineFixBorderless", value ) )
    {
        if ( value != 0.0 ) { return 1; }
    }
    return 2;
}

// Borderless first, Fullscreen last: the DLL sees both writes through the engine's config writer
// and switches the device when Fullscreen arrives (docs/fixes/borderless-window.md).
function W2EF_SetWindowMode( mode : int )
{
    if ( mode == 1 )
    {
        theGame.WriteConfigParamFloat( "User", "Rendering", "EngineFixBorderless", 1.0 );
    }
    else
    {
        theGame.WriteConfigParamFloat( "User", "Rendering", "EngineFixBorderless", 0.0 );
    }
    if ( mode == 0 )
    {
        theGame.WriteConfigParamFloat( "User", "Rendering", "Fullscreen", 1.0 );
    }
    else
    {
        theGame.WriteConfigParamFloat( "User", "Rendering", "Fullscreen", 0.0 );
    }
}

function W2EF_WindowText( id : int ) : string
{
    var audioLang, textLang : string;

    theGame.GetGameLanguageName( audioLang, textLang );
    if ( textLang == "PL" )
    {
        if ( id == 1 ) { return "Pe" + StrChar( 322 ) + "ny ekran"; }
        if ( id == 2 ) { return "Okno bez ramki"; }
        if ( id == 3 ) { return "Tryb wy" + StrChar( 347 ) + "wietlania"; }
        if ( id == 4 ) { return "W oknie"; }
    }
    if ( textLang == "DE" )
    {
        if ( id == 1 ) { return "Vollbild"; }
        if ( id == 2 ) { return "Rahmenloses Fenster"; }
        if ( id == 3 ) { return "Anzeigemodus"; }
        if ( id == 4 ) { return "Fenster"; }
    }
    if ( textLang == "FR" )
    {
        if ( id == 1 ) { return "Plein " + StrChar( 233 ) + "cran"; }
        if ( id == 2 ) { return "Fen" + StrChar( 234 ) + "tre sans bordure"; }
        if ( id == 3 ) { return "Mode d'affichage"; }
        if ( id == 4 ) { return "Fen" + StrChar( 234 ) + "tr" + StrChar( 233 ); }
    }
    if ( textLang == "ES" )
    {
        if ( id == 1 ) { return "Pantalla completa"; }
        if ( id == 2 ) { return "Ventana sin bordes"; }
        if ( id == 3 ) { return "Modo de pantalla"; }
        if ( id == 4 ) { return "Ventana"; }
    }
    if ( textLang == "IT" )
    {
        if ( id == 1 ) { return "Schermo intero"; }
        if ( id == 2 ) { return "Finestra senza bordi"; }
        if ( id == 3 ) { return "Modalit" + StrChar( 224 ) + " schermo"; }
        if ( id == 4 ) { return "Finestra"; }
    }
    if ( textLang == "RU" )
    {
        if ( id == 1 ) { return StrChar( 1055 ) + StrChar( 1086 ) + StrChar( 1083 ) + StrChar( 1085 ) + StrChar( 1086 ) + StrChar( 1101 ) + StrChar( 1082 ) + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1085 ) + StrChar( 1099 ) + StrChar( 1081 ) + " " + StrChar( 1088 ) + StrChar( 1077 ) + StrChar( 1078 ) + StrChar( 1080 ) + StrChar( 1084 ); }
        if ( id == 2 ) { return StrChar( 1054 ) + StrChar( 1082 ) + StrChar( 1085 ) + StrChar( 1086 ) + " " + StrChar( 1073 ) + StrChar( 1077 ) + StrChar( 1079 ) + " " + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1084 ) + StrChar( 1082 ) + StrChar( 1080 ); }
        if ( id == 3 ) { return StrChar( 1056 ) + StrChar( 1077 ) + StrChar( 1078 ) + StrChar( 1080 ) + StrChar( 1084 ) + " " + StrChar( 1101 ) + StrChar( 1082 ) + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1072 ); }
        if ( id == 4 ) { return StrChar( 1042 ) + " " + StrChar( 1086 ) + StrChar( 1082 ) + StrChar( 1085 ) + StrChar( 1077 ); }
    }
    if ( textLang == "CZ" )
    {
        if ( id == 1 ) { return "Pln" + StrChar( 225 ) + " obrazovka"; }
        if ( id == 2 ) { return "Okno bez okraje"; }
        if ( id == 3 ) { return "Re" + StrChar( 382 ) + "im zobrazen" + StrChar( 237 ); }
        if ( id == 4 ) { return "V okn" + StrChar( 283 ); }
    }
    if ( textLang == "HU" )
    {
        if ( id == 1 ) { return "Teljes k" + StrChar( 233 ) + "perny" + StrChar( 337 ); }
        if ( id == 2 ) { return "Keret n" + StrChar( 233 ) + "lk" + StrChar( 252 ) + "li ablak"; }
        if ( id == 3 ) { return "Megjelen" + StrChar( 237 ) + "t" + StrChar( 233 ) + "si m" + StrChar( 243 ) + "d"; }
        if ( id == 4 ) { return "Ablakos"; }
    }
    if ( textLang == "TR" )
    {
        if ( id == 1 ) { return "Tam ekran"; }
        if ( id == 2 ) { return StrChar( 199 ) + "er" + StrChar( 231 ) + "evesiz pencere"; }
        if ( id == 3 ) { return "G" + StrChar( 246 ) + "r" + StrChar( 252 ) + "nt" + StrChar( 252 ) + " modu"; }
        if ( id == 4 ) { return "Pencere"; }
    }
    if ( textLang == "JP" )
    {
        if ( id == 1 ) { return StrChar( 12501 ) + StrChar( 12523 ) + StrChar( 12473 ) + StrChar( 12463 ) + StrChar( 12522 ) + StrChar( 12540 ) + StrChar( 12531 ); }
        if ( id == 2 ) { return StrChar( 12508 ) + StrChar( 12540 ) + StrChar( 12480 ) + StrChar( 12540 ) + StrChar( 12524 ) + StrChar( 12473 ) + StrChar( 12454 ) + StrChar( 12451 ) + StrChar( 12531 ) + StrChar( 12489 ) + StrChar( 12454 ); }
        if ( id == 3 ) { return StrChar( 34920 ) + StrChar( 31034 ) + StrChar( 12514 ) + StrChar( 12540 ) + StrChar( 12489 ); }
        if ( id == 4 ) { return StrChar( 12454 ) + StrChar( 12451 ) + StrChar( 12531 ) + StrChar( 12489 ) + StrChar( 12454 ); }
    }
    if ( textLang == "ZH" )
    {
        if ( id == 1 ) { return StrChar( 20840 ) + StrChar( 34722 ) + StrChar( 24149 ); }
        if ( id == 2 ) { return StrChar( 28961 ) + StrChar( 37002 ) + StrChar( 30028 ) + StrChar( 35222 ) + StrChar( 31383 ); }
        if ( id == 3 ) { return StrChar( 39023 ) + StrChar( 31034 ) + StrChar( 27169 ) + StrChar( 24335 ); }
        if ( id == 4 ) { return StrChar( 35222 ) + StrChar( 31383 ); }
    }
    if ( id == 1 ) { return "Fullscreen"; }
    if ( id == 2 ) { return "Borderless window"; }
    if ( id == 3 ) { return "Display mode"; }
    if ( id == 4 ) { return "Windowed"; }
    return "";
}
