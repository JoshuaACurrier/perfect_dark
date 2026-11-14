## Experimental netplay branch

This branch contains a **highly experimental** implementation of network play.

Currently only Combat Sim is partially working with experimental 8 player support and really bad netcode.  
Expect nothing to work right.

### Hosting

1. Forward the server port (`27100` by default, can be changed in `pd.ini`).
    * You can also use something like ZeroTier or Hamachi if you can't port forward.
3. Choose `Network Game` -> `Host Game`, configure the player cap, press `Start Game`.
4. Configure Combat Sim as desired, but do not start it yet. Keep in mind:
    * You can save and load the settings as normal.
    * Your player will have the config of Combat Sim player 1.
5. Wait for everyone to join. You can use the console (press `~`) to chat in the meantime.
6. Hit Enter/Start. During the game no more players will be able to join.
7. After game is over, you will be kicked back to Combat Sim settings, at which point people can join again and the process repeats from step 3.

### Joining

1. Your player will have the config of Combat Sim player 1, so set that up beforehand in the Combat Sim menu.
2. Choose `Network Game` -> `Join Game`, enter the address (and port if it's not 27100), press `Start Game`.
3. If the message says `Waiting for host`, wait until the host configures the game. You can use the console (press `~`) to chat in the meantime or `ESC` to disconnect.
4. The game will start automatically.
5. After the game is over, press Enter/Start on the stats screen and you will be kicked back to the `Waiting for host` screen, at which point the process repeats from step 3.

You can join while a match is already running. The screen then says `Match in progress`: you wait in the
lobby (chat still works) and play from the next match the host starts. The host sees "joined and will play
from the next match". Dropping straight into the running match isn't supported yet.

### Co-op missions (2 players)

1. Host: `Network Game` -> `Host Game` -> `Co-Operative Mission`, then pick the mission and difficulty as usual.
   The host plays Joanna; the player who joins is the co-op buddy.
2. Client: join as above. The mission starts for both when the host accepts it.
3. The host's game runs all guards and mission scripts. Objectives, HUD messages, alarms,
   countdown timers, music changes, guards spawned by the mission, and guards' dropped guns
   and bodies are sent to the client.
4. The end screen and `Retry` / next mission follow the host. Aborting on the client disconnects it.

Known gaps in co-op: cutscenes are skipped; the buddy uses the Combat Sim character model;
items handed out by mission scripts are not synced yet; only 2 players.

### Unlocks

While a network game is running, every Combat Sim arena, scenario, weapon and simulant type,
and every co-op mission and difficulty up to Perfect Agent, can be picked even if the host's
save hasn't unlocked them. The save itself is not changed. Set `Net.UnlockAll=0` in `pd.ini`
to turn this off.

### Cheats

The host's cheats (picked in the Cheats menu before hosting) are sent to every player when a
match or mission starts, so everyone plays with the same cheats. A player's own cheat choices
don't apply in a network game. As in single player, the "weapons for Jo" cheats only work in Solo.

### Scores, kills and the HUD

The host decides every kill and sends it to everyone, along with the whole scoreboard whenever it
changes, so all players see the same scores and the usual "Killed by" / "Killed" messages. The death
screen shows who killed you and counts down to the respawn; the host's `Auto Respawn` setting
(`Game.MpAutoRespawn`) applies to everyone. Hit markers come from the host too, so they show up about
one round trip after the shot. Footsteps (yours, other players', guards' and simulants') play in network
games, from where they are; the original only plays them with one player, since split screen players
share the speakers.

### Misc

* Everyone has to have the same ROM. The game only compares the file name, but different regions in one game will definitely break everything.
* Press `~` to bring up the console, press `F9` for network stats.
* You can chat at any time during a net game by entering messages in the console and hitting Enter.
* Server port and other network settings can be configured in `pd.ini`.
* For a 4+ player game I would recommend changing `Net.Server.UpdateFrames` to `2` in `pd.ini` on the server to lower the bandwidth usage.
* You can paste stuff into the `Enter Address` prompt by hitting `CTRL+V`.
* Expect nothing to work right.

### Known issues

* Clients see rocket rotation incorrectly.
* Clients can't use Slayer fly-by-wire rockets and they will not correctly sync if the server uses them.
* Clients can't use the alt fire mode on the FarSight and probably a couple other guns.
* Cloaking device is not synced.
* Most likely only Combat works properly.
* Simulants and guards are run by the host. Clients show them a little in the past (normally 2 to 6 frames,
  more on an uneven connection; `pd.log` has a `chr display delay` line every 30 s) on top of the ping, and
  smoothly in between the host's updates, at any frame rate.
* The host decides hits on everyone using where it last heard each player was, so a player who has just
  ducked behind a wall can still be hit for about one round trip (your ping on `F9`).
* Death floors, long falls and falling out of the level are decided by each player's own game (the host
  only approximates where remote players are), then reported to the host.
* For bug reports, start the game with `Play.bat` (in the release zip) or `--log`: it writes `pd.log`, and
  the previous run is kept as `pd.log.prev`.
* Bandwidth consumption on the server is pretty large on the default settings, especially with 8 players.
 

### Testing

`tools/nettest/` has scripts for running a host and clients on one PC and comparing what they see.
See `tools/nettest/README.md` for the command line flags (auto host/join/start, autopilot, state dumps, simulated lag and packet loss).
