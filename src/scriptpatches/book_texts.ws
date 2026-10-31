function W2EF_Text( id : int ) : string
{
    var audioLang, textLang : string;

    theGame.GetGameLanguageName( audioLang, textLang );
    if ( textLang == "PL" )
    {
        if ( id == 1 ) { return "Przeczytana"; }
        if ( id == 2 ) { return "Nic nowego"; }
        if ( id == 3 ) { return "Nieprzeczytana"; }
        if ( id == 4 ) { return "Przeczyta" + StrChar( 322 ) + "e" + StrChar( 347 ) + " t" + StrChar( 281 ) + " ksi" + StrChar( 261 ) + StrChar( 380 ) + "k" + StrChar( 281 ) + "."; }
        if ( id == 5 ) { return "Nieprzeczytana, ale znasz ju" + StrChar( 380 ) + " wszystko, czego uczy:"; }
        if ( id == 6 ) { return "Jeszcze nieprzeczytana."; }
        if ( id == 7 ) { return " Po przeczytaniu otrzymasz:"; }
        if ( id == 8 ) { return "Brak wiedzy o potworach i wpis" + StrChar( 243 ) + "w w dzienniku, tylko tekst."; }
        if ( id == 9 ) { return "Nowe: "; }
        if ( id == 10 ) { return "Znane: "; }
        if ( id == 11 ) { return "Pe" + StrChar( 322 ) + "na wiedza: %1"; }
        if ( id == 12 ) { return " (masz %1)"; }
        if ( id == 13 ) { return "wpis w bestiariuszu"; }
        if ( id == 14 ) { return "wpis w dzienniku"; }
        if ( id == 15 ) { return "kolejna strona"; }
    }
    if ( textLang == "DE" )
    {
        if ( id == 1 ) { return "Gelesen"; }
        if ( id == 2 ) { return "Nichts Neues"; }
        if ( id == 3 ) { return "Ungelesen"; }
        if ( id == 4 ) { return "Du hast dieses Buch gelesen."; }
        if ( id == 5 ) { return "Nicht gelesen, aber du kennst bereits alles, was es lehrt:"; }
        if ( id == 6 ) { return "Noch nicht gelesen."; }
        if ( id == 7 ) { return " Beim Lesen erh" + StrChar( 228 ) + "ltst du:"; }
        if ( id == 8 ) { return "Kein Monsterwissen und keine Tagebucheintr" + StrChar( 228 ) + "ge, nur der Text."; }
        if ( id == 9 ) { return "Neu: "; }
        if ( id == 10 ) { return "Bekannt: "; }
        if ( id == 11 ) { return "Gesamtes Wissen " + StrChar( 252 ) + "ber %1"; }
        if ( id == 12 ) { return " (du hast %1)"; }
        if ( id == 13 ) { return "Bestiarium-Eintrag"; }
        if ( id == 14 ) { return "Tagebucheintrag"; }
        if ( id == 15 ) { return "n" + StrChar( 228 ) + "chste Seite"; }
    }
    if ( textLang == "FR" )
    {
        if ( id == 1 ) { return "Lu"; }
        if ( id == 2 ) { return "Rien de nouveau"; }
        if ( id == 3 ) { return "Non lu"; }
        if ( id == 4 ) { return "Vous avez lu ce livre."; }
        if ( id == 5 ) { return "Non lu, mais vous savez d" + StrChar( 233 ) + "j" + StrChar( 224 ) + " tout ce qu'il enseigne :"; }
        if ( id == 6 ) { return "Pas encore lu."; }
        if ( id == 7 ) { return " Sa lecture vous apporte :"; }
        if ( id == 8 ) { return "Aucune connaissance sur les monstres ni entr" + StrChar( 233 ) + "e de journal, seulement le texte."; }
        if ( id == 9 ) { return "Nouveau : "; }
        if ( id == 10 ) { return "Connu : "; }
        if ( id == 11 ) { return "Connaissances compl" + StrChar( 232 ) + "tes : %1"; }
        if ( id == 12 ) { return " (vous avez %1)"; }
        if ( id == 13 ) { return "entr" + StrChar( 233 ) + "e du bestiaire"; }
        if ( id == 14 ) { return "entr" + StrChar( 233 ) + "e du journal"; }
        if ( id == 15 ) { return "page suivante"; }
    }
    if ( textLang == "ES" )
    {
        if ( id == 1 ) { return "Le" + StrChar( 237 ) + "do"; }
        if ( id == 2 ) { return "Nada nuevo"; }
        if ( id == 3 ) { return "Sin leer"; }
        if ( id == 4 ) { return "Has le" + StrChar( 237 ) + "do este libro."; }
        if ( id == 5 ) { return "Sin leer, pero ya sabes todo lo que ense" + StrChar( 241 ) + "a:"; }
        if ( id == 6 ) { return "A" + StrChar( 250 ) + "n sin leer."; }
        if ( id == 7 ) { return " Al leerlo obtienes:"; }
        if ( id == 8 ) { return "Sin conocimientos de monstruos ni entradas del diario, solo el texto."; }
        if ( id == 9 ) { return "Nuevo: "; }
        if ( id == 10 ) { return "Conocido: "; }
        if ( id == 11 ) { return "Conocimiento completo: %1"; }
        if ( id == 12 ) { return " (tienes %1)"; }
        if ( id == 13 ) { return "entrada del bestiario"; }
        if ( id == 14 ) { return "entrada del diario"; }
        if ( id == 15 ) { return "p" + StrChar( 225 ) + "gina siguiente"; }
    }
    if ( textLang == "IT" )
    {
        if ( id == 1 ) { return "Letto"; }
        if ( id == 2 ) { return "Niente di nuovo"; }
        if ( id == 3 ) { return "Non letto"; }
        if ( id == 4 ) { return "Hai letto questo libro."; }
        if ( id == 5 ) { return "Non letto, ma conosci gi" + StrChar( 224 ) + " tutto ci" + StrChar( 242 ) + " che insegna:"; }
        if ( id == 6 ) { return "Non ancora letto."; }
        if ( id == 7 ) { return " Leggendolo ottieni:"; }
        if ( id == 8 ) { return "Nessuna conoscenza sui mostri n" + StrChar( 233 ) + " voci del diario, solo il testo."; }
        if ( id == 9 ) { return "Nuovo: "; }
        if ( id == 10 ) { return "Noto: "; }
        if ( id == 11 ) { return "Conoscenza completa: %1"; }
        if ( id == 12 ) { return " (hai %1)"; }
        if ( id == 13 ) { return "voce del bestiario"; }
        if ( id == 14 ) { return "voce del diario"; }
        if ( id == 15 ) { return "pagina successiva"; }
    }
    if ( textLang == "RU" )
    {
        if ( id == 1 ) { return StrChar( 1055 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1086 ); }
        if ( id == 2 ) { return StrChar( 1053 ) + StrChar( 1080 ) + StrChar( 1095 ) + StrChar( 1077 ) + StrChar( 1075 ) + StrChar( 1086 ) + " " + StrChar( 1085 ) + StrChar( 1086 ) + StrChar( 1074 ) + StrChar( 1086 ) + StrChar( 1075 ) + StrChar( 1086 ); }
        if ( id == 3 ) { return StrChar( 1053 ) + StrChar( 1077 ) + " " + StrChar( 1087 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1086 ); }
        if ( id == 4 ) { return StrChar( 1042 ) + StrChar( 1099 ) + " " + StrChar( 1087 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + StrChar( 1072 ) + StrChar( 1083 ) + StrChar( 1080 ) + " " + StrChar( 1101 ) + StrChar( 1090 ) + StrChar( 1091 ) + " " + StrChar( 1082 ) + StrChar( 1085 ) + StrChar( 1080 ) + StrChar( 1075 ) + StrChar( 1091 ) + "."; }
        if ( id == 5 ) { return StrChar( 1053 ) + StrChar( 1077 ) + " " + StrChar( 1087 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1086 ) + ", " + StrChar( 1085 ) + StrChar( 1086 ) + " " + StrChar( 1074 ) + StrChar( 1099 ) + " " + StrChar( 1091 ) + StrChar( 1078 ) + StrChar( 1077 ) + " " + StrChar( 1079 ) + StrChar( 1085 ) + StrChar( 1072 ) + StrChar( 1077 ) + StrChar( 1090 ) + StrChar( 1077 ) + " " + StrChar( 1074 ) + StrChar( 1089 ) + StrChar( 1105 ) + ", " + StrChar( 1095 ) + StrChar( 1077 ) + StrChar( 1084 ) + StrChar( 1091 ) + " " + StrChar( 1086 ) + StrChar( 1085 ) + StrChar( 1072 ) + " " + StrChar( 1091 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + ":"; }
        if ( id == 6 ) { return StrChar( 1045 ) + StrChar( 1097 ) + StrChar( 1105 ) + " " + StrChar( 1085 ) + StrChar( 1077 ) + " " + StrChar( 1087 ) + StrChar( 1088 ) + StrChar( 1086 ) + StrChar( 1095 ) + StrChar( 1080 ) + StrChar( 1090 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1086 ) + "."; }
        if ( id == 7 ) { return " " + StrChar( 1063 ) + StrChar( 1090 ) + StrChar( 1077 ) + StrChar( 1085 ) + StrChar( 1080 ) + StrChar( 1077 ) + " " + StrChar( 1076 ) + StrChar( 1072 ) + StrChar( 1089 ) + StrChar( 1090 ) + " " + StrChar( 1074 ) + StrChar( 1072 ) + StrChar( 1084 ) + ":"; }
        if ( id == 8 ) { return StrChar( 1053 ) + StrChar( 1077 ) + StrChar( 1090 ) + " " + StrChar( 1079 ) + StrChar( 1085 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1080 ) + StrChar( 1081 ) + " " + StrChar( 1086 ) + " " + StrChar( 1095 ) + StrChar( 1091 ) + StrChar( 1076 ) + StrChar( 1086 ) + StrChar( 1074 ) + StrChar( 1080 ) + StrChar( 1097 ) + StrChar( 1072 ) + StrChar( 1093 ) + " " + StrChar( 1080 ) + " " + StrChar( 1079 ) + StrChar( 1072 ) + StrChar( 1087 ) + StrChar( 1080 ) + StrChar( 1089 ) + StrChar( 1077 ) + StrChar( 1081 ) + " " + StrChar( 1074 ) + " " + StrChar( 1078 ) + StrChar( 1091 ) + StrChar( 1088 ) + StrChar( 1085 ) + StrChar( 1072 ) + StrChar( 1083 ) + StrChar( 1077 ) + ", " + StrChar( 1090 ) + StrChar( 1086 ) + StrChar( 1083 ) + StrChar( 1100 ) + StrChar( 1082 ) + StrChar( 1086 ) + " " + StrChar( 1090 ) + StrChar( 1077 ) + StrChar( 1082 ) + StrChar( 1089 ) + StrChar( 1090 ) + "."; }
        if ( id == 9 ) { return StrChar( 1053 ) + StrChar( 1086 ) + StrChar( 1074 ) + StrChar( 1086 ) + StrChar( 1077 ) + ": "; }
        if ( id == 10 ) { return StrChar( 1048 ) + StrChar( 1079 ) + StrChar( 1074 ) + StrChar( 1077 ) + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1085 ) + StrChar( 1086 ) + ": "; }
        if ( id == 11 ) { return StrChar( 1055 ) + StrChar( 1086 ) + StrChar( 1083 ) + StrChar( 1085 ) + StrChar( 1099 ) + StrChar( 1077 ) + " " + StrChar( 1079 ) + StrChar( 1085 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1080 ) + StrChar( 1103 ) + ": %1"; }
        if ( id == 12 ) { return " (" + StrChar( 1091 ) + " " + StrChar( 1074 ) + StrChar( 1072 ) + StrChar( 1089 ) + " %1)"; }
        if ( id == 13 ) { return StrChar( 1079 ) + StrChar( 1072 ) + StrChar( 1087 ) + StrChar( 1080 ) + StrChar( 1089 ) + StrChar( 1100 ) + " " + StrChar( 1074 ) + " " + StrChar( 1073 ) + StrChar( 1077 ) + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1080 ) + StrChar( 1072 ) + StrChar( 1088 ) + StrChar( 1080 ) + StrChar( 1080 ); }
        if ( id == 14 ) { return StrChar( 1079 ) + StrChar( 1072 ) + StrChar( 1087 ) + StrChar( 1080 ) + StrChar( 1089 ) + StrChar( 1100 ) + " " + StrChar( 1074 ) + " " + StrChar( 1078 ) + StrChar( 1091 ) + StrChar( 1088 ) + StrChar( 1085 ) + StrChar( 1072 ) + StrChar( 1083 ) + StrChar( 1077 ); }
        if ( id == 15 ) { return StrChar( 1089 ) + StrChar( 1083 ) + StrChar( 1077 ) + StrChar( 1076 ) + StrChar( 1091 ) + StrChar( 1102 ) + StrChar( 1097 ) + StrChar( 1072 ) + StrChar( 1103 ) + " " + StrChar( 1089 ) + StrChar( 1090 ) + StrChar( 1088 ) + StrChar( 1072 ) + StrChar( 1085 ) + StrChar( 1080 ) + StrChar( 1094 ) + StrChar( 1072 ); }
    }
    if ( textLang == "CZ" )
    {
        if ( id == 1 ) { return "P" + StrChar( 345 ) + "e" + StrChar( 269 ) + "teno"; }
        if ( id == 2 ) { return "Nic nov" + StrChar( 233 ) + "ho"; }
        if ( id == 3 ) { return "Nep" + StrChar( 345 ) + "e" + StrChar( 269 ) + "teno"; }
        if ( id == 4 ) { return "Tuto knihu jsi p" + StrChar( 345 ) + "e" + StrChar( 269 ) + "etl."; }
        if ( id == 5 ) { return "Nep" + StrChar( 345 ) + "e" + StrChar( 269 ) + "teno, ale v" + StrChar( 353 ) + "e, co u" + StrChar( 269 ) + StrChar( 237 ) + ", u" + StrChar( 382 ) + " zn" + StrChar( 225 ) + StrChar( 353 ) + ":"; }
        if ( id == 6 ) { return "Zat" + StrChar( 237 ) + "m nep" + StrChar( 345 ) + "e" + StrChar( 269 ) + "teno."; }
        if ( id == 7 ) { return " P" + StrChar( 345 ) + "e" + StrChar( 269 ) + "ten" + StrChar( 237 ) + "m z" + StrChar( 237 ) + "sk" + StrChar( 225 ) + StrChar( 353 ) + ":"; }
        if ( id == 8 ) { return StrChar( 381 ) + StrChar( 225 ) + "dn" + StrChar( 233 ) + " znalosti o netvorech ani z" + StrChar( 225 ) + "znamy v den" + StrChar( 237 ) + "ku, jen text."; }
        if ( id == 9 ) { return "Nov" + StrChar( 233 ) + ": "; }
        if ( id == 10 ) { return "Zn" + StrChar( 225 ) + "m" + StrChar( 233 ) + ": "; }
        if ( id == 11 ) { return StrChar( 218 ) + "pln" + StrChar( 233 ) + " znalosti: %1"; }
        if ( id == 12 ) { return " (m" + StrChar( 225 ) + StrChar( 353 ) + " %1)"; }
        if ( id == 13 ) { return "z" + StrChar( 225 ) + "znam v besti" + StrChar( 225 ) + StrChar( 345 ) + "i"; }
        if ( id == 14 ) { return "z" + StrChar( 225 ) + "znam v den" + StrChar( 237 ) + "ku"; }
        if ( id == 15 ) { return "dal" + StrChar( 353 ) + StrChar( 237 ) + " strana"; }
    }
    if ( textLang == "HU" )
    {
        if ( id == 1 ) { return "Elolvasva"; }
        if ( id == 2 ) { return "Semmi " + StrChar( 250 ) + "j"; }
        if ( id == 3 ) { return "Olvasatlan"; }
        if ( id == 4 ) { return "Ezt a k" + StrChar( 246 ) + "nyvet m" + StrChar( 225 ) + "r elolvastad."; }
        if ( id == 5 ) { return "Olvasatlan, de m" + StrChar( 225 ) + "r mindent tudsz, amit tan" + StrChar( 237 ) + "t:"; }
        if ( id == 6 ) { return "M" + StrChar( 233 ) + "g nem olvastad."; }
        if ( id == 7 ) { return " Elolvasva ezt kapod:"; }
        if ( id == 8 ) { return "Nincs benne sz" + StrChar( 246 ) + "rnyismeret vagy napl" + StrChar( 243 ) + "bejegyz" + StrChar( 233 ) + "s, csak a sz" + StrChar( 246 ) + "veg."; }
        if ( id == 9 ) { return StrChar( 218 ) + "j: "; }
        if ( id == 10 ) { return "Ismert: "; }
        if ( id == 11 ) { return "Teljes tud" + StrChar( 225 ) + "s: %1"; }
        if ( id == 12 ) { return " (neked: %1)"; }
        if ( id == 13 ) { return "besti" + StrChar( 225 ) + "rium-bejegyz" + StrChar( 233 ) + "s"; }
        if ( id == 14 ) { return "napl" + StrChar( 243 ) + "bejegyz" + StrChar( 233 ) + "s"; }
        if ( id == 15 ) { return "k" + StrChar( 246 ) + "vetkez" + StrChar( 337 ) + " oldal"; }
    }
    if ( textLang == "TR" )
    {
        if ( id == 1 ) { return "Okundu"; }
        if ( id == 2 ) { return "Yeni bir " + StrChar( 351 ) + "ey yok"; }
        if ( id == 3 ) { return "Okunmad" + StrChar( 305 ); }
        if ( id == 4 ) { return "Bu kitab" + StrChar( 305 ) + " okudun."; }
        if ( id == 5 ) { return "Okunmad" + StrChar( 305 ) + ", ama " + StrChar( 246 ) + StrChar( 287 ) + "retti" + StrChar( 287 ) + "i her " + StrChar( 351 ) + "eyi zaten biliyorsun:"; }
        if ( id == 6 ) { return "Hen" + StrChar( 252 ) + "z okunmad" + StrChar( 305 ) + "."; }
        if ( id == 7 ) { return " Okursan " + StrChar( 351 ) + "unlar" + StrChar( 305 ) + " kazan" + StrChar( 305 ) + "rs" + StrChar( 305 ) + "n:"; }
        if ( id == 8 ) { return "Canavar bilgisi ya da g" + StrChar( 252 ) + "nl" + StrChar( 252 ) + "k kayd" + StrChar( 305 ) + " yok, sadece metin."; }
        if ( id == 9 ) { return "Yeni: "; }
        if ( id == 10 ) { return "Bilinen: "; }
        if ( id == 11 ) { return "T" + StrChar( 252 ) + "m bilgi: %1"; }
        if ( id == 12 ) { return " (sende %1)"; }
        if ( id == 13 ) { return "bestiyer kayd" + StrChar( 305 ); }
        if ( id == 14 ) { return "g" + StrChar( 252 ) + "nl" + StrChar( 252 ) + "k kayd" + StrChar( 305 ); }
        if ( id == 15 ) { return "sonraki sayfa"; }
    }
    if ( textLang == "JP" )
    {
        if ( id == 1 ) { return StrChar( 26082 ) + StrChar( 35501 ); }
        if ( id == 2 ) { return StrChar( 26032 ) + StrChar( 24773 ) + StrChar( 22577 ) + StrChar( 12394 ) + StrChar( 12375 ); }
        if ( id == 3 ) { return StrChar( 26410 ) + StrChar( 35501 ); }
        if ( id == 4 ) { return StrChar( 12371 ) + StrChar( 12398 ) + StrChar( 26412 ) + StrChar( 12399 ) + StrChar( 35501 ) + StrChar( 12435 ) + StrChar( 12384 ) + StrChar( 12371 ) + StrChar( 12392 ) + StrChar( 12364 ) + StrChar( 12354 ) + StrChar( 12427 ) + StrChar( 12290 ); }
        if ( id == 5 ) { return StrChar( 26410 ) + StrChar( 35501 ) + StrChar( 12384 ) + StrChar( 12364 ) + StrChar( 12289 ) + StrChar( 20869 ) + StrChar( 23481 ) + StrChar( 12399 ) + StrChar( 12377 ) + StrChar( 12409 ) + StrChar( 12390 ) + StrChar( 30693 ) + StrChar( 12387 ) + StrChar( 12390 ) + StrChar( 12356 ) + StrChar( 12427 ) + StrChar( 65306 ); }
        if ( id == 6 ) { return StrChar( 12414 ) + StrChar( 12384 ) + StrChar( 35501 ) + StrChar( 12435 ) + StrChar( 12391 ) + StrChar( 12356 ) + StrChar( 12394 ) + StrChar( 12356 ) + StrChar( 12290 ); }
        if ( id == 7 ) { return " " + StrChar( 35501 ) + StrChar( 12416 ) + StrChar( 12392 ) + StrChar( 24471 ) + StrChar( 12425 ) + StrChar( 12428 ) + StrChar( 12427 ) + StrChar( 12418 ) + StrChar( 12398 ) + StrChar( 65306 ); }
        if ( id == 8 ) { return StrChar( 24618 ) + StrChar( 29289 ) + StrChar( 12398 ) + StrChar( 30693 ) + StrChar( 35672 ) + StrChar( 12420 ) + StrChar( 26085 ) + StrChar( 35468 ) + StrChar( 12398 ) + StrChar( 38917 ) + StrChar( 30446 ) + StrChar( 12399 ) + StrChar( 12394 ) + StrChar( 12367 ) + StrChar( 12289 ) + StrChar( 26412 ) + StrChar( 25991 ) + StrChar( 12398 ) + StrChar( 12415 ) + StrChar( 12290 ); }
        if ( id == 9 ) { return StrChar( 26032 ) + StrChar( 35215 ) + StrChar( 65306 ); }
        if ( id == 10 ) { return StrChar( 26082 ) + StrChar( 30693 ) + StrChar( 65306 ); }
        if ( id == 11 ) { return "%1" + StrChar( 12398 ) + StrChar( 23436 ) + StrChar( 20840 ) + StrChar( 12394 ) + StrChar( 30693 ) + StrChar( 35672 ); }
        if ( id == 12 ) { return StrChar( 65288 ) + StrChar( 29694 ) + StrChar( 22312 ) + " %1" + StrChar( 65289 ); }
        if ( id == 13 ) { return StrChar( 22259 ) + StrChar( 37969 ) + StrChar( 12398 ) + StrChar( 38917 ) + StrChar( 30446 ); }
        if ( id == 14 ) { return StrChar( 26085 ) + StrChar( 35468 ) + StrChar( 12398 ) + StrChar( 38917 ) + StrChar( 30446 ); }
        if ( id == 15 ) { return StrChar( 27425 ) + StrChar( 12398 ) + StrChar( 12506 ) + StrChar( 12540 ) + StrChar( 12472 ); }
    }
    if ( textLang == "ZH" )
    {
        if ( id == 1 ) { return StrChar( 24050 ) + StrChar( 35712 ); }
        if ( id == 2 ) { return StrChar( 28961 ) + StrChar( 26032 ) + StrChar( 20839 ) + StrChar( 23481 ); }
        if ( id == 3 ) { return StrChar( 26410 ) + StrChar( 35712 ); }
        if ( id == 4 ) { return StrChar( 20320 ) + StrChar( 24050 ) + StrChar( 35712 ) + StrChar( 36942 ) + StrChar( 36889 ) + StrChar( 26412 ) + StrChar( 26360 ) + StrChar( 12290 ); }
        if ( id == 5 ) { return StrChar( 26410 ) + StrChar( 35712 ) + StrChar( 65292 ) + StrChar( 20294 ) + StrChar( 20320 ) + StrChar( 24050 ) + StrChar( 30693 ) + StrChar( 36947 ) + StrChar( 23427 ) + StrChar( 35352 ) + StrChar( 36617 ) + StrChar( 30340 ) + StrChar( 19968 ) + StrChar( 20999 ) + StrChar( 65306 ); }
        if ( id == 6 ) { return StrChar( 23578 ) + StrChar( 26410 ) + StrChar( 38321 ) + StrChar( 35712 ) + StrChar( 12290 ); }
        if ( id == 7 ) { return " " + StrChar( 38321 ) + StrChar( 35712 ) + StrChar( 24460 ) + StrChar( 21487 ) + StrChar( 29554 ) + StrChar( 24471 ) + StrChar( 65306 ); }
        if ( id == 8 ) { return StrChar( 27794 ) + StrChar( 26377 ) + StrChar( 24618 ) + StrChar( 29289 ) + StrChar( 30693 ) + StrChar( 35672 ) + StrChar( 25110 ) + StrChar( 26085 ) + StrChar( 35468 ) + StrChar( 26781 ) + StrChar( 30446 ) + StrChar( 65292 ) + StrChar( 21482 ) + StrChar( 26377 ) + StrChar( 20839 ) + StrChar( 25991 ) + StrChar( 12290 ); }
        if ( id == 9 ) { return StrChar( 26032 ) + StrChar( 65306 ); }
        if ( id == 10 ) { return StrChar( 24050 ) + StrChar( 30693 ) + StrChar( 65306 ); }
        if ( id == 11 ) { return "%1" + StrChar( 30340 ) + StrChar( 20840 ) + StrChar( 37096 ) + StrChar( 30693 ) + StrChar( 35672 ); }
        if ( id == 12 ) { return StrChar( 65288 ) + StrChar( 20320 ) + StrChar( 30446 ) + StrChar( 21069 ) + " %1" + StrChar( 65289 ); }
        if ( id == 13 ) { return StrChar( 24618 ) + StrChar( 29289 ) + StrChar( 22294 ) + StrChar( 37969 ) + StrChar( 26781 ) + StrChar( 30446 ); }
        if ( id == 14 ) { return StrChar( 26085 ) + StrChar( 35468 ) + StrChar( 26781 ) + StrChar( 30446 ); }
        if ( id == 15 ) { return StrChar( 19979 ) + StrChar( 19968 ) + StrChar( 38913 ); }
    }
    if ( id == 1 ) { return "Read"; }
    if ( id == 2 ) { return "Nothing new"; }
    if ( id == 3 ) { return "Unread"; }
    if ( id == 4 ) { return "You have read this book."; }
    if ( id == 5 ) { return "Not read, but you already know everything it teaches:"; }
    if ( id == 6 ) { return "Not read yet."; }
    if ( id == 7 ) { return " Reading it gives you:"; }
    if ( id == 8 ) { return "No bestiary knowledge or journal entries, only the text."; }
    if ( id == 9 ) { return "New: "; }
    if ( id == 10 ) { return "Known: "; }
    if ( id == 11 ) { return "All knowledge on %1"; }
    if ( id == 12 ) { return " (you have %1)"; }
    if ( id == 13 ) { return "bestiary entry"; }
    if ( id == 14 ) { return "journal entry"; }
    if ( id == 15 ) { return "next page"; }
    return "";
}