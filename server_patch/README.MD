NFS World Offline (Lawin) - saving the rewards of NFSWorldPursuitProbe pursuits
================================================================================

The ASI computes rewards and the lucky draw itself (event 385 formula) and shows them
on the results screen. To SAVE them to the profile it calls one extra server route.

Installation:
  1. Close the server.
  2. Copy PursuitProbe.js into  <server folder>\src\routes\
     (the server loads every file in that folder automatically, nothing else changes).
  3. Restart the server (start.bat).

The console shows one line per pursuit, e.g.
  [GAME] [PursuitProbe] pursuit EVADED: +138 cash, +80 REP, heat 1.34, lucky draw juggernaut x6

The same file also counts the powerups you fire during an ASI pursuit (for the
"powerups used" statistic on the results screen). Replace the old copy when updating.

The ASI talks to http://127.0.0.1:3550 by default. Different address/port: create
NFSWorldPursuitProbe_server.txt next to the .asi with one line:  127.0.0.1 3550

If you applied the old eventManager.js patch earlier, you can restore the original file:
it is no longer needed.
