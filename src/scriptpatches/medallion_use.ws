// Uses the medallion at once, without the animation that stops Geralt: the pulse comes from
// the same function as the animation's medalion_fx event and keeps its sound and radial blur.
final function W2EF_UseMedallion()
{
    if ( !IsNotGeralt() )
    {
        if ( m_canUseMedallion )
        {
            TriggerMedalionFX();
            // TriggerMedalionFX armed the game's 2 s timer. Its line differs between
            // the vanilla and Mersey scripts, so it is replaced here instead of edited.
            RemoveTimer( 'OnEnableMedallion' );
            AddTimer( 'OnEnableMedallion', 1.f, false );
        }
        UseTalismanGuide();
    }
}

