# Net test tools

Tools for testing netplay alone on one PC: run a host and clients over localhost,
script their input, and compare what each copy thinks the world looks like.

| File | What it does |
|---|---|
| `nettest.sh` | Starts a host and N clients with separate save dirs and logs, waits, kills them, prints the `NET:` log lines. |
| `desync.py` | Compares two `--net-dump` files (host vs client): how far apart every chr and loose gun is, plus props that only the client has or that vanished on one side. |
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
| `--net-autokill SECS` | Host: every SECS seconds, shoot the guard nearest to one of the players (alternating), to test deaths, drops and alarms. |
| `--net-autoalarm SECS` | Host: turn on the alarm and a running 60 s countdown at SECS, both off at 2×SECS (checks the mission state sync; the client logs `NET: mission state: ...`). |
| `--net-autoquit SECS` | Quit SECS seconds after the match starts. |
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
