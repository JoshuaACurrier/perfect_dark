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

### Joining and leaving a match that is running

You can join a Combat Simulator match, a co-op mission or a counter-operative mission that has already started. The number of players in a match is
fixed when its stage loads, so the host starts every match with a few spare player slots that nobody
controls (`Net.Server.SpareSlots`, default 2, up to 8 players in all; an empty slot's player is kept
dead and hidden on every machine and isn't on the scoreboard). When you connect during a match and a
slot is free, your game loads the stage as it was at the start, the host sends what has changed since
(who is alive and armed with what, pickups taken, guns lying around, open doors, lifts, the scores and
the clock), and you spawn. It takes a few seconds; the others see "joined the match".

When a player disconnects, their slot becomes empty again and the next person to connect, or the same
person coming back, gets it. Their score starts from zero.

Limits for now:

* When no slot is free you wait in the lobby (`Match in progress`, chat still works) and play from
  the next match.
* Something that had been picked up when you joined comes back at the same moment for you as for
  everyone else (it used to start a fresh 20 second wait on the joiner's machine).
* A late joiner plays as their own character: the spare slot they take over is rebuilt with their
  body and head on every machine (custom camera heads excepted).
* Broken glass is the same for everyone, late joiners included. In Combat Simulator a broken pane
  comes back 20 seconds after it is cleared away; that used to happen only once someone on that
  machine came near it, so machines disagreed about which panes were back.
* `Net.Server.JoinInProgress=0` on the host switches all of this off (no spare slots, late joiners
  wait for the next match).

### Co-op missions (2 players)

1. Host: `Network Game` -> `Host Game` -> `Co-Operative Mission`, then pick the mission and difficulty as usual.
   The host plays Joanna; the player who joins is the co-op buddy.
2. Client: join as above. The mission starts for both when the host accepts it.
3. The host's game runs all guards and mission scripts. Objectives, HUD messages, alarms,
   countdown timers, music changes, guards spawned by the mission, and guards' dropped guns
   and bodies are sent to the client.
4. The end screen and `Retry` / next mission follow the host. Aborting on the client disconnects it.

5. The buddy can join later. A host who starts a mission alone gets an empty buddy slot (the
   buddy counts as dead until someone takes it). Whoever connects during the mission loads the
   stage as it was at the start and is sent what has changed: guards that are dead, gone or newly
   spawned, guns they dropped, objects taken, moved or destroyed, open doors, the objectives, the
   alarm and the countdown. They spawn next to the action the way a co-op buddy respawns, but
   without taking half of the host's health. If the buddy disconnects, the slot is free again.
6. What mission scripts change about objects is sent to the clients (`SVC_PROP_STATE`): hidden
   or shown, locked or unlocked doors, collectable, invincible, switched off, and how destroyed
   a thing is. Before this a client's copy of such an object stayed as the level file had it.

Known gaps in co-op: cutscenes are skipped; the buddy uses the Combat Sim character model;
items handed out by mission scripts are not synced yet; objects that scripts move are not;
a late joiner starts with the mission's starting weapons and hears the default music;
only 2 players.

### Counter-operative missions (2 to 8 players)

1. Host: `Network Game` -> `Host Game` -> `Counter-Operative Mission`, then pick the mission and
   difficulty. The host plays Joanna. Everyone who has joined by the time the host accepts the
   mission plays a guard. (Alone, the host just plays the mission.)
2. Each counter-operative takes over a guard near Joanna that she is not looking at, with that
   guard's looks, gun and health. Killed, they press fire to take over another one. The mission
   ends when Joanna dies, aborts or completes it; the end screens follow the host.
3. Only the host chooses the guard (`SVC_ANTI_SPAWN`): it removes the guard everywhere, moves the
   player there and tells every machine the body, head and gun. `pd.log` has
   `NET: counter-op: player N takes over guard ...` on the host and `... is now guard ...` on the
   clients; the lines should name the same guards in the same order.

4. Joining and leaving work as in the other modes: the mission starts with spare slots
   (`Net.Server.SpareSlots`), someone who connects during it loads in and takes over a guard, and
   a counter-operative who disconnects just dies.

Limits for now: the host is always Joanna.

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

### Getting the game and updates (this fork's builds)

Download `PerfectDarkOnline.zip` from the fork's latest release once, unpack it, put your own ROM
in `data/`, and start the game with `Play.bat`. From then on `Play.bat` checks the latest release
each time it starts and, if there is a newer build, downloads only the program files that changed
(a few megabytes), checks them and swaps them in before starting the game. Your ROM, save,
settings, logs and texture packs are never touched. `version.txt` says which build you have; an
empty file called `noupdate.txt` in the folder switches the check off. Without internet the game
simply starts as it is.

Everyone in a game needs the same build. If joining says "Protocol or version mismatch", both
sides should close the game and start `Play.bat` again.

### Hits on other characters

When someone other than you is hit (a punch, a pistol whip, a bullet), the hit sound comes from the
one who was hit: it has a direction, fades with distance and is muffled by walls. The original
plays it as a plain sound with only a volume, and a punch carried so far that anyone's punches
sounded like your own. Punches now carry about 14 metres. Hits on you sound as before.

### Other players' guns

The other players' gun sounds (shots, reloads, drawing a weapon) come from where those players
are, with distance and walls applied like any other sound in the level. The looping sounds of a few
weapons (the Reaper's blades, the laser's stream, a Mauler charging) are not played for other
players yet.

### Lobby, chat and scoreboard

While you are connected and no match is running, a list of the connected players shows at the top
left, each with a mark: `HOST`, `READY`, `NOT READY`, or `IN MATCH` for players still in a match you
joined too late for. Joining players press `R` to switch their mark; the host starts the match when
they like (the marks don't block it). The list also shows each player's ping.

`T` opens a chat line, in the lobby and during a match: type, `Enter` sends, `Esc` cancels. In the
lobby the chat window stays up at the bottom right; in a match the last lines show at the right
edge for ten seconds.
The console (`~`) still works for chat too.

During a Combat Simulator match:

* the bar at the top shows the leading team or player, the time left and the score to reach, and
  your own team or place (`Score And Timer Bar`, `Game.MpScoreBar`);
* kills are listed at the top left for a few seconds (`Kill Feed`, `Game.MpKillFeed`);
* hold `Tab` for the scoreboard: every player and simulant with score, deaths and ping. In a
  match the keyboard's `Tab` only does this, even if it is also bound to Start; `Esc` (or Start
  on a controller) opens the pause menu.

`T`, `R` and `Tab` are fixed keys for now; if you have bound one of them to something else, both
things happen.

### Lag compensation and ping

Your shots are tested by the host against where the other players, simulants and guards were on
your screen when you fired, so you aim at what you see instead of ahead of it. The host keeps about a
second of positions for this and goes back at most `Net.Server.LagCompensationMaxMs` (300); set
`Net.Server.LagCompensation=0` on the host to turn it off. The other side of it: a player with a high
ping can still hit you a moment after you got behind cover on your own screen.

`Show Ping` (Extended options, `Net.ShowPing`) puts your ping at the top right, with packet loss when
there is any; the host sees the highest of the other players'. The ping is measured inside the game, from the host
sending a snapshot to your input for it arriving back, so it is a frame or two higher than what a
network tool would show. Loss is the share of the host's snapshots that never reached you.

### Staying in sync, smoothness and bandwidth

* **Self-healing.** Every 10 seconds (`Net.Server.ResyncSecs`, 0 = off) the host lists the pickups
  that are gone or waiting to come back, the loose guns that exist and which doors are open. Each
  client removes what it shouldn't have, asks the host for loose guns it is missing, and opens or
  closes a door that was different two lists in a row. A repair is written to `pd.log` as
  `NET: resync repaired: ...`, so the log shows whether a game drifted apart.
* **The Slayer's guided rocket works for everyone.** The pilot's own machine steers the rocket, so
  it responds at once, and decides when it hits something; it tells the host where the rocket is
  every frame (`CLC_ROCKET`), and the host tells everyone else (`SVC_ROCKET`), its own guided
  rockets included. Copies on other machines only follow: they do not collide with anything and
  go off when the host says the rocket is gone. The blast and its damage are the host's, at the
  place the pilot reported. Before this the host had no controls for a remote pilot: the rocket
  flew straight while its pilot stood frozen, and nobody saw the host's own rocket turn.
  `pd.log` has `NET: rocket ...` lines with the end position on every machine.
* **Scenarios follow the host.** In King of the Hill, Pop a Cap, Hold the Briefcase, Hacker Central
  and Capture the Case the host decides and tells everyone (`SVC_SCENARIO`, when something changes,
  every 10 seconds anyway, and to a late joiner): where the hill is, who holds it and when it
  scores; who the Pop a Cap victim is and its points; who carries the briefcase, the uplink or a
  team's case; Hacker Central download points; the host's hill time. Before this each machine
  ran the scenario by itself with its own random numbers and player numbering, so the hill could
  move to different rooms on different machines and clients disagreed about the victim. Empty
  spare slots are never the victim. `pd.log` has a `NET: scenario: ...` line for every change,
  worded the same on every machine so logs can be compared.
* **Other players move smoothly.** Positions carry the sender's clock, and a remote player is shown
  a little in the past, between two known positions. How far in the past adapts to how unevenly the
  packets arrive: about 2 to 3 frames on a steady connection, more on a jittery one (the same
  method as for simulants and guards). Lag compensation uses the delay each client reports.
  `Net.SmoothPlayers=0` goes back to chasing the newest position.
* **Bandwidth.** Movement messages are 39 bytes instead of 59. With four or more players the host
  sends positions 30 times a second instead of 60 (`Net.Server.AutoUpdateFrames`, on by default;
  clients still send their input every frame). A firing player's ammo is sent 10 times a second
  instead of every frame. Measured with 4 players and 2 simulants over localhost: host upload
  69 KB/s before, 24 KB/s after. For 8 players expect roughly 80 KB/s (0.7 Mbit/s) of upload on
  the host.

### Misc

* Everyone has to have the same ROM. The game only compares the file name, but different regions in one game will definitely break everything.
* Press `~` to bring up the console, press `F9` for network stats.
* You can chat at any time during a net game by entering messages in the console and hitting Enter.
* Server port and other network settings can be configured in `pd.ini`.
* For a 4+ player game I would recommend changing `Net.Server.UpdateFrames` to `2` in `pd.ini` on the server to lower the bandwidth usage.
* You can paste stuff into the `Enter Address` prompt by hitting `CTRL+V`.
* Expect nothing to work right.

### Known issues

* Clients can't use the alt fire mode on the FarSight and probably a couple other guns (not
  reproduced or fixed yet).
* Cloaking (the device and the RCP-120's) is decided by the cloaked player's own game and shown the
  same on every machine. Rockets point the same way on every machine (checked with
  `tools/nettest/rotcheck.py`: 0 degrees apart).
* Most likely only Combat works properly.
* With spare player slots in a match (see above), the original end-of-match screens may list the
  empty slots as players called "Open".
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
