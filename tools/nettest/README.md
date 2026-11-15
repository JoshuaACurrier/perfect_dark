# Net test tools

Tools for testing netplay alone on one PC: run a host and clients over localhost,
script their input, and compare what each copy thinks the world looks like.

| File | What it does |
|---|---|
| `nettest.sh` | Starts a host and N clients with separate save dirs and logs, waits, kills them, prints the `NET:` log lines. `-d SECS` starts the last client late, to test joining mid-match. `-s` leaves sound on (it's off by default); at autoquit the log then says how many voice blocks went through the 3D audio path. |
| `smooth.py` | How smoothly chrs move in a `--net-dump` file (dump with `--net-dump-interval 1`): speed spikes, back-and-forth jitter and distance travelled. Run it on the host's dump and a client's; the client's should look like the host's. |
| `imgdiff.py` | Compares two screenshots of the same view (brightness, difference, a grid of what got brighter or darker), for checking what a video option changed. No image library needed. |
| `desync.py` | Compares two `--net-dump` files (host vs client): how far apart every chr and loose gun is, plus props that only the client has or that vanished on one side. |
| `audiocheck.py` | Checks captured audio for a glitch that repeats every mixer block (184 frames). Capture with `PD_AUDIO_CAPTURE=1 nettest.sh -s ...`, which writes `<name>/audio.raw` instead of playing; a clean capture's worst position is about 1.1x the median, a broken one was 18x. |
| `winshot.ps1` | Screenshots each game window (not the desktop); skips hung windows. |
| `sendkey.ps1` | Sends a key press to the game windows without focusing them. |
| `gdbhost.sh` | Runs the host under gdb with a client connecting, and prints the backtrace if it crashes. |

## Game flags (see `port/src/net/netauto.c`)

| Flag | Meaning |
|---|---|
| `--logfile PATH` | Write the log to PATH (implies `--log`). |
| `--profile N` / `--profile-name NAME` | Load agent N; creates one if the save dir is empty. |
| `--net-name NAME` | Local player name. |
| `--net-autostart N` | Host: start the match once N players (including the host) are in the lobby. |
| `--net-stage NUM` | Host: stage for the auto-started match (e.g. `0x1f` Complex). |
| `--net-sims N`, `--net-simdiff D` | Host: number of Simulants and their difficulty (0 meat .. 5 dark). |
| `--net-scenario N` | Host: scenario (0 combat, 1 hold the briefcase, ...). |
| `--net-coop STAGE`, `--net-difficulty N` | Host: start a co-op mission instead (e.g. `0x30` Defection; 0 Agent, 1 Special Agent, 2 Perfect Agent). |
| `--net-autoabort SECS` | Host: abort the mission after SECS seconds, to test the end screens. |
| `--net-rounds N` | Host: auto-start up to N matches in a row; after each one ends the host skips its end screens and goes back to the lobby after 5 s. `--net-sims` simulants are only created for the first match, so later ones show whether the setup carried over. |
| `--net-hudtest` | Keeps the hit marker up and labels characters CLEAR/HALF/HIDDEN (sound line of sight). Also: three seconds into the lobby each copy says a chat line and the client marks itself ready, and five seconds into the match each says another, so screenshots show the player list, chat, score bar and kill feed. Start with `--net-autostart 3` and two copies to stay in the lobby. |
| `--net-aimtest` | Lag compensation test. Host: everyone spawns with a CMP150, no score limit; with `--net-sims 0` and `--net-autopilot` the host walks up to the other player and strafes (better: play the host yourself and keep moving in front of the client). Client with `--net-autopilot`: keeps its sights on the nearest character in sight, as drawn, and holds fire. At `--net-autoquit` the host logs hits per firing frame and how far the client's aim line was from its target with no rewind, with the rewind used, and a few ticks either side. Use with `--net-lag` and `--net-cheats 0x24` (invincible, no reloads). |
| `--net-nolagcomp` | Host: lag compensation off for this run. |
| `--net-lagbias TICKS` | Host: rewind TICKS 60ths more (negative: less) than measured. |
| `--net-autokill SECS` | Host: every SECS seconds, shoot the guard nearest to one of the players (alternating), to test deaths, drops and alarms. |
| `--net-autoalarm SECS` | Host: turn on the alarm and a running 60 s countdown at SECS, both off at 2×SECS (checks the mission state sync; the client logs `NET: mission state: ...`). |
| `--net-cheats MASK` | Host: turn on these cheats for the auto-started match (bit N = `CHEAT_*` number N, e.g. `0x890` DK Mode + Small Characters + Unlimited Ammo); clients log `NET: host cheats ...`. |
| `--net-teams` | Host: turn teams on for the auto-started match: the players and every other simulant against the rest. |
| `--net-autoquit SECS` | Quit SECS seconds after the match starts. Logs the scoreboard as this machine sees it, how many footsteps played, frame times (`NET: frames in game: ... took 30 ms or longer`, and a `NET: slow frame` line for each of the first 20) and what the texture pack loader did. |
| `--net-autopilot [SEED]` | Drive the local player with random but repeatable input. |
| `--net-dump FILE`, `--net-dump-interval T` | Write every chr's state to FILE every T ticks. |
| `--net-lag MS`, `--net-jitter MS`, `--net-loss PCT` | Simulate a bad connection on outgoing packets. |

## Example

Online Combat Sim on Complex with 4 Simulants, client on autopilot, 45 seconds, then compare:

```sh
O=$(cygpath -m ../pd-data/nettest)
tools/nettest/nettest.sh -t 75 \
  -H "--net-name Host --net-autostart 2 --net-stage 0x1f --net-sims 4 --net-dump $O/host/dump.csv" \
  -C "--net-name Client --net-autopilot 7 --net-dump $O/client1/dump.csv" \
  -- --net-autoquit 45
python3 tools/nettest/desync.py $O/host/dump.csv $O/client1/dump.csv
```

## Packaging and updates

| Script | What it does |
|---|---|
| `package.sh [--textures] [-n "note"]...` | Builds, then updates `../pd-data/dist/PerfectDarkOnline/` in place (program files only: a ROM, save and settings in it stay), makes `PerfectDarkOnline.zip` for first installs, and `dist/release/` with every program file plus `manifest.txt` (build number = commit count, commit, protocol, notes, SHA-256 and size per file). The shipped exe is stripped; the full one goes to `../pd-data/symbols/pd-<commit>.exe` for reading crash addresses with `addr2line`. |
| `publish.sh` | Uploads `dist/release/` as release `online-<build>` on the fork (never upstream) and marks it latest. Refuses if the folder holds a ROM, save, settings or textures, if the packaged commit isn't HEAD, or if it isn't pushed. Publishing is public: ask Josh first. |
| `Update.ps1` | Shipped in the folder; `Play.bat` runs it before the game. Reads `manifest.txt` from the latest release, and if its build number is higher than `version.txt`, downloads the files whose hash differs, verifies them, and swaps them in. `PD_UPDATE_URL` points it somewhere else (tested with `python -m http.server` over a copy of `dist/release`). |
