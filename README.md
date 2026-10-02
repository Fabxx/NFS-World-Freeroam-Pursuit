This script requires the freeroam cops mod that was shared on EPVP forum in order to work, affects attributes.bin to spawn cops in freeroam.

# v0.2

- Now the ASI script recreates the beta pursuit state graph that was used in early builds until 2011
- A key_bind.ini file has been added to remap the toggle key for the cops behavior (default: F9)
- A NFSWorldPursuitProbe_entrant.txt gets created one the player gets busted or evades, to fill the result screen after the event

# v0.4

- New option in Options > Gameplay: "Freeroam Pursuits" ON/OFF (needs the patched Options.gfx, see options_gfx/GamePlayOption.as). It is stored in the unused "moments" field of the gameplay options, saved by the game in %APPDATA%\Need for Speed World\Settings\UserSettings.xml. The F9 key and key_bind.ini are no longer used.
- Hitting a roadblock is no longer reported as dodged; real dodges are counted in the pursuit stats.
- NFSWorldPursuitProbe_songs.txt lists the pursuit song slots the music may use (default: 0 1).
