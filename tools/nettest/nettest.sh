#!/usr/bin/env bash
# Launches a host and one or more clients on this machine over localhost, waits,
# then kills them and prints the NET lines from each log.
#
# usage: tools/nettest/nettest.sh [options] [-- extra args for every instance]
#   -c N        number of clients (default 1)
#   -t SECS     how long to run before killing everything (default 40)
#   -H "ARGS"   extra args for the host only
#   -C "ARGS"   extra args for the clients only
#   -k          keep the instances running (don't kill them)
#
# env:
#   PD_EXE      path to pd.x86_64.exe (default: build/pd.x86_64.exe)
#   PD_DATA     base dir with the ROM (default: ../pd-data next to the repo)
#   PD_OUT      where save dirs and logs go (default: $PD_DATA/nettest)
#
# example: run a 2-player Combat Sim with 4 Simulants on Skedar Ruins for 60s:
#   tools/nettest/nettest.sh -t 60 -H "--net-autostart 2 --net-stage 0x26 --net-sims 4" -- --net-autoquit 55

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PD_EXE="${PD_EXE:-$ROOT/build/pd.x86_64.exe}"
PD_DATA="${PD_DATA:-$(cd "$ROOT/.." && pwd)/pd-data}"
PD_OUT="${PD_OUT:-$PD_DATA/nettest}"

NCLIENTS=1
TIME=40
HOSTARGS=""
CLIENTARGS=""
KEEP=0

while getopts "c:t:H:C:k" opt; do
	case "$opt" in
		c) NCLIENTS="$OPTARG" ;;
		t) TIME="$OPTARG" ;;
		H) HOSTARGS="$OPTARG" ;;
		C) CLIENTARGS="$OPTARG" ;;
		k) KEEP=1 ;;
		*) exit 1 ;;
	esac
done
shift $((OPTIND - 1))
COMMON="$*"

# SDL2 and zlib DLLs
export PATH="/c/msys64/mingw64/bin:$PATH"

winpath() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

DATA_W="$(winpath "$PD_DATA")"
mkdir -p "$PD_OUT"
OUT_W="$(winpath "$PD_OUT")"

PIDS=()

launch() {
	local name="$1"; shift
	mkdir -p "$PD_OUT/$name"
	rm -f "$PD_OUT/$name/pd.log"
	"$PD_EXE" --basedir "$DATA_W" --savedir "$OUT_W/$name" --logfile "$OUT_W/$name/pd.log" \
		--no-sound "$@" > "$PD_OUT/$name/stdout.txt" 2>&1 &
	PIDS+=($!)
	echo "started $name (pid $!): $*"
}

# shellcheck disable=SC2086
launch host --host $HOSTARGS $COMMON
sleep 5
for i in $(seq 1 "$NCLIENTS"); do
	# shellcheck disable=SC2086
	launch "client$i" --connect 127.0.0.1 $CLIENTARGS $COMMON
	sleep 1
done

if [ "$KEEP" = 1 ]; then
	echo "instances left running; logs in $PD_OUT"
	exit 0
fi

# wait until all exit on their own or the time runs out
end=$((SECONDS + TIME))
while [ $SECONDS -lt $end ]; do
	alive=0
	for p in "${PIDS[@]}"; do
		kill -0 "$p" 2>/dev/null && alive=1
	done
	[ $alive = 0 ] && break
	sleep 1
done

for p in "${PIDS[@]}"; do
	kill "$p" 2>/dev/null
done
taskkill //F //IM "$(basename "$PD_EXE")" > /dev/null 2>&1

for d in host $(seq -f "client%g" 1 "$NCLIENTS"); do
	echo "===== $d ====="
	grep -E "NET|FATAL|ERROR|crash" "$PD_OUT/$d/pd.log" 2>/dev/null | grep -v "loading segment" | tail -40
done
