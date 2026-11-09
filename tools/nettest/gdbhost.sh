#!/usr/bin/env bash
# Runs the host under gdb and a plain client, prints the host backtrace if it crashes.
# usage: tools/nettest/gdbhost.sh [secs] [-- extra host args]
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export PATH=/c/msys64/mingw64/bin:$PATH
D="$(cygpath -m "${PD_DATA:-$ROOT/../pd-data}")"
T="${1:-40}"; shift; [ "${1:-}" = "--" ] && shift
OUT="$D/nettest/gdb-host.txt"
mkdir -p "$D/nettest/host" "$D/nettest/client1"
(timeout "$T" gdb -batch -ex run -ex bt -ex "info locals" --args "$ROOT/build/pd.x86_64.exe" --basedir "$D" --savedir "$D/nettest/host" \
	--logfile "$D/nettest/host/pd.log" --no-sound --host "$@" > "$OUT" 2>&1 &)
sleep 8
("$ROOT/build/pd.x86_64.exe" --basedir "$D" --savedir "$D/nettest/client1" --logfile "$D/nettest/client1/pd.log" \
	--no-sound --connect 127.0.0.1 ${PD_CLIENT_ARGS:-} > /dev/null 2>&1 &)
sleep $((T - 6))
taskkill //F //IM pd.x86_64.exe > /dev/null 2>&1
sleep 1
grep -v "Thread\|loading segment" "$OUT" | sed -n '/^NET\|^#\|signal\|SIG/,$p' | head -60
