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

// The Draw distance row: the DLL's presets 0 = vanilla (the game's own), 1 = high, 2 = ultra,
// 3 = extreme. The DLL keeps [Rendering] EngineFixDrawDistance equal to the preset in use (also
// after Ctrl+F10), so the row opens on the live level; a missing key means extreme, the DLL's
// default.
function W2EF_DrawDistanceIndex() : int
{
    var value : float;

    value = 3.0;
    if ( !theGame.ReadConfigParamFloat( "User", "Rendering", "EngineFixDrawDistance", value ) )
    {
        return 3;
    }
    if ( value < 0.5 ) { return 0; }
    if ( value < 1.5 ) { return 1; }
    if ( value < 2.5 ) { return 2; }
    return 3;
}

// The DLL sees the write through the engine's config writer and switches the preset at once.
function W2EF_SetDrawDistance( level : int )
{
    theGame.WriteConfigParamFloat( "User", "Rendering", "EngineFixDrawDistance", ( float ) level );
}

// The Shadow quality row: 0 = vanilla (the game's own), 1 = high, 2 = ultra. The DLL keeps [Rendering]
// EngineFixShadowQuality equal to the level in use, so the row opens on the live level.
function W2EF_ShadowQualityIndex() : int
{
    var value : float;

    value = 0.0;
    if ( !theGame.ReadConfigParamFloat( "User", "Rendering", "EngineFixShadowQuality", value ) )
    {
        return 0;
    }
    if ( value < 0.5 ) { return 0; }
    if ( value < 1.5 ) { return 1; }
    return 2;
}

// The DLL sees the write through the engine's config writer and switches the level at once.
function W2EF_SetShadowQuality( level : int )
{
    theGame.WriteConfigParamFloat( "User", "Rendering", "EngineFixShadowQuality", ( float ) level );
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
        if ( id == 5 ) { return "Zasi" + StrChar( 281 ) + "g rysowania"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Wysoki"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Ekstremalny"; }
        if ( id == 10 ) { return "Jako" + StrChar( 347 ) + StrChar( 263 ) + " cieni"; }
    }
    if ( textLang == "DE" )
    {
        if ( id == 1 ) { return "Vollbild"; }
        if ( id == 2 ) { return "Rahmenloses Fenster"; }
        if ( id == 3 ) { return "Anzeigemodus"; }
        if ( id == 4 ) { return "Fenster"; }
        if ( id == 5 ) { return "Sichtweite"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Hoch"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Extrem"; }
        if ( id == 10 ) { return "Schattenqualit" + StrChar( 228 ) + "t"; }
    }
    if ( textLang == "FR" )
    {
        if ( id == 1 ) { return "Plein " + StrChar( 233 ) + "cran"; }
        if ( id == 2 ) { return "Fen" + StrChar( 234 ) + "tre sans bordure"; }
        if ( id == 3 ) { return "Mode d'affichage"; }
        if ( id == 4 ) { return "Fen" + StrChar( 234 ) + "tr" + StrChar( 233 ); }
        if ( id == 5 ) { return "Distance d'affichage"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return StrChar( 201 ) + "lev" + StrChar( 233 ) + "e"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Extr" + StrChar( 234 ) + "me"; }
        if ( id == 10 ) { return "Qualit" + StrChar( 233 ) + " des ombres"; }
    }
    if ( textLang == "ES" )
    {
        if ( id == 1 ) { return "Pantalla completa"; }
        if ( id == 2 ) { return "Ventana sin bordes"; }
        if ( id == 3 ) { return "Modo de pantalla"; }
        if ( id == 4 ) { return "Ventana"; }
        if ( id == 5 ) { return "Distancia de dibujado"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Alta"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Extrema"; }
        if ( id == 10 ) { return "Calidad de sombras"; }
    }
    if ( textLang == "IT" )
    {
        if ( id == 1 ) { return "Schermo intero"; }
        if ( id == 2 ) { return "Finestra senza bordi"; }
        if ( id == 3 ) { return "Modalit" + StrChar( 224 ) + " schermo"; }
        if ( id == 4 ) { return "Finestra"; }
        if ( id == 5 ) { return "Distanza visiva"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Alta"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Estrema"; }
        if ( id == 10 ) { return "Qualit" + StrChar( 224 ) + " delle ombre"; }
    }
    if ( textLang == "RU" )
    {
        if ( id == 1 ) { return StrChar( 1055 ) + StrChar( 1086 ) + StrChar( 1083 ) + StrChar( 1085 ) + StrChar( 1086 ) + StrChar( 1101 ) + StrChar( 1082 ) + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1085 ) + StrChar( 1099 ) + StrChar( 1081 ) + " " + StrChar( 1088 ) + StrChar( 1077 ) + StrChar( 1078 ) + StrChar( 1080 ) + StrChar( 1084 ); }
        if ( id == 2 ) { return StrChar( 1054 ) + StrChar( 1082 ) + StrChar( 1085 ) + StrChar( 1086 ) + " " + StrChar( 1073 ) + StrChar( 1077 ) + StrChar( 1079 ) + " " + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1084 ) + StrChar( 1082 ) + StrChar( 1080 ); }
        if ( id == 3 ) { return StrChar( 1056 ) + StrChar( 1077 ) + StrChar( 1078 ) + StrChar( 1080 ) + StrChar( 1084 ) + " " + StrChar( 1101 ) + StrChar( 1082 ) + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1072 ); }
        if ( id == 4 ) { return StrChar( 1042 ) + " " + StrChar( 1086 ) + StrChar( 1082 ) + StrChar( 1085 ) + StrChar( 1077 ); }
        if ( id == 5 ) { return StrChar( 1044 ) + StrChar( 1072 ) + StrChar( 1083 ) + StrChar( 1100 ) + StrChar( 1085 ) + StrChar( 1086 ) + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1100 ) + " " + StrChar( 1087 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1088 ) + StrChar( 1080 ) + StrChar( 1089 ) + StrChar( 1086 ) + StrChar( 1074 ) + StrChar( 1082 ) + StrChar( 1080 ); }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return StrChar( 1042 ) + StrChar( 1099 ) + StrChar( 1089 ) + StrChar( 1086 ) + StrChar( 1082 ) + StrChar( 1072 ) + StrChar( 1103 ); }
        if ( id == 8 ) { return StrChar( 1059 ) + StrChar( 1083 ) + StrChar( 1100 ) + StrChar( 1090 ) + StrChar( 1088 ) + StrChar( 1072 ); }
        if ( id == 9 ) { return StrChar( 1069 ) + StrChar( 1082 ) + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1088 ) + StrChar( 1080 ) + StrChar( 1084 ); }
        if ( id == 10 ) { return StrChar( 1050 ) + StrChar( 1072 ) + StrChar( 1095 ) + StrChar( 1077 ) + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1074 ) + StrChar( 1086 ) + " " + StrChar( 1090 ) + StrChar( 1077 ) + StrChar( 1085 ) + StrChar( 1077 ) + StrChar( 1081 ); }
    }
    if ( textLang == "CZ" )
    {
        if ( id == 1 ) { return "Pln" + StrChar( 225 ) + " obrazovka"; }
        if ( id == 2 ) { return "Okno bez okraje"; }
        if ( id == 3 ) { return "Re" + StrChar( 382 ) + "im zobrazen" + StrChar( 237 ); }
        if ( id == 4 ) { return "V okn" + StrChar( 283 ); }
        if ( id == 5 ) { return "Vzd" + StrChar( 225 ) + "lenost vykreslov" + StrChar( 225 ) + "n" + StrChar( 237 ); }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Vysok" + StrChar( 225 ); }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Extr" + StrChar( 233 ) + "mn" + StrChar( 237 ); }
        if ( id == 10 ) { return "Kvalita st" + StrChar( 237 ) + "n" + StrChar( 367 ); }
    }
    if ( textLang == "HU" )
    {
        if ( id == 1 ) { return "Teljes k" + StrChar( 233 ) + "perny" + StrChar( 337 ); }
        if ( id == 2 ) { return "Keret n" + StrChar( 233 ) + "lk" + StrChar( 252 ) + "li ablak"; }
        if ( id == 3 ) { return "Megjelen" + StrChar( 237 ) + "t" + StrChar( 233 ) + "si m" + StrChar( 243 ) + "d"; }
        if ( id == 4 ) { return "Ablakos"; }
        if ( id == 5 ) { return "L" + StrChar( 225 ) + "t" + StrChar( 243 ) + "t" + StrChar( 225 ) + "vols" + StrChar( 225 ) + "g"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Magas"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Extr" + StrChar( 233 ) + "m"; }
        if ( id == 10 ) { return StrChar( 193 ) + "rny" + StrChar( 233 ) + "k min" + StrChar( 337 ) + "s" + StrChar( 233 ) + "ge"; }
    }
    if ( textLang == "TR" )
    {
        if ( id == 1 ) { return "Tam ekran"; }
        if ( id == 2 ) { return StrChar( 199 ) + "er" + StrChar( 231 ) + "evesiz pencere"; }
        if ( id == 3 ) { return "G" + StrChar( 246 ) + "r" + StrChar( 252 ) + "nt" + StrChar( 252 ) + " modu"; }
        if ( id == 4 ) { return "Pencere"; }
        if ( id == 5 ) { return "G" + StrChar( 246 ) + "r" + StrChar( 252 ) + StrChar( 351 ) + " mesafesi"; }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return "Y" + StrChar( 252 ) + "ksek"; }
        if ( id == 8 ) { return "Ultra"; }
        if ( id == 9 ) { return "Ekstrem"; }
        if ( id == 10 ) { return "G" + StrChar( 246 ) + "lge kalitesi"; }
    }
    if ( textLang == "JP" )
    {
        if ( id == 1 ) { return StrChar( 12501 ) + StrChar( 12523 ) + StrChar( 12473 ) + StrChar( 12463 ) + StrChar( 12522 ) + StrChar( 12540 ) + StrChar( 12531 ); }
        if ( id == 2 ) { return StrChar( 12508 ) + StrChar( 12540 ) + StrChar( 12480 ) + StrChar( 12540 ) + StrChar( 12524 ) + StrChar( 12473 ) + StrChar( 12454 ) + StrChar( 12451 ) + StrChar( 12531 ) + StrChar( 12489 ) + StrChar( 12454 ); }
        if ( id == 3 ) { return StrChar( 34920 ) + StrChar( 31034 ) + StrChar( 12514 ) + StrChar( 12540 ) + StrChar( 12489 ); }
        if ( id == 4 ) { return StrChar( 12454 ) + StrChar( 12451 ) + StrChar( 12531 ) + StrChar( 12489 ) + StrChar( 12454 ); }
        if ( id == 5 ) { return StrChar( 25551 ) + StrChar( 30011 ) + StrChar( 36317 ) + StrChar( 38626 ); }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return StrChar( 39640 ); }
        if ( id == 8 ) { return StrChar( 12454 ) + StrChar( 12523 ) + StrChar( 12488 ) + StrChar( 12521 ); }
        if ( id == 9 ) { return StrChar( 12456 ) + StrChar( 12463 ) + StrChar( 12473 ) + StrChar( 12488 ) + StrChar( 12522 ) + StrChar( 12540 ) + StrChar( 12512 ); }
        if ( id == 10 ) { return StrChar( 24433 ) + StrChar( 12398 ) + StrChar( 21697 ) + StrChar( 36074 ); }
    }
    if ( textLang == "ZH" )
    {
        if ( id == 1 ) { return StrChar( 20840 ) + StrChar( 34722 ) + StrChar( 24149 ); }
        if ( id == 2 ) { return StrChar( 28961 ) + StrChar( 37002 ) + StrChar( 30028 ) + StrChar( 35222 ) + StrChar( 31383 ); }
        if ( id == 3 ) { return StrChar( 39023 ) + StrChar( 31034 ) + StrChar( 27169 ) + StrChar( 24335 ); }
        if ( id == 4 ) { return StrChar( 35222 ) + StrChar( 31383 ); }
        if ( id == 5 ) { return StrChar( 32362 ) + StrChar( 35069 ) + StrChar( 36317 ) + StrChar( 38626 ); }
        if ( id == 6 ) { return "Vanilla"; }
        if ( id == 7 ) { return StrChar( 39640 ); }
        if ( id == 8 ) { return StrChar( 26997 ) + StrChar( 39640 ); }
        if ( id == 9 ) { return StrChar( 26997 ) + StrChar( 33268 ); }
        if ( id == 10 ) { return StrChar( 38512 ) + StrChar( 24433 ) + StrChar( 21697 ) + StrChar( 36074 ); }
    }
    if ( id == 1 ) { return "Fullscreen"; }
    if ( id == 2 ) { return "Borderless window"; }
    if ( id == 3 ) { return "Display mode"; }
    if ( id == 4 ) { return "Windowed"; }
    if ( id == 5 ) { return "Draw distance"; }
    if ( id == 6 ) { return "Vanilla"; }
    if ( id == 7 ) { return "High"; }
    if ( id == 8 ) { return "Ultra"; }
    if ( id == 9 ) { return "Extreme"; }
    if ( id == 10 ) { return "Shadow quality"; }
    return "";
}
