#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# perf_snapshot.sh — "what is eating the CPU right now", written to a file.
#
#   sh perf_snapshot.sh [seconds] [outdir]
#
# The unit ships htop/top/vmstat, but nobody can watch an interactive screen in
# the car, so this takes a batch snapshot: load, memory, top CPU consumers
# sampled twice (so the numbers mean something), top RSS, a per-daemon CPU
# delta straight from /proc, and a dedicated line for the navigation map, which
# keeps drawing while it is not in the foreground (reported ~10% of one core).
set -u
SECS=3
OUT=/rw_data/perf
[ $# -ge 1 ] && [ -n "$1" ] && SECS=$1
[ $# -ge 2 ] && [ -n "$2" ] && OUT=$2
mkdir -p "$OUT" 2>/dev/null || { echo "cannot create $OUT" >&2; exit 1; }
REPORT="$OUT/perf.txt"
CSV="$OUT/perf.csv"
NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
UP=$(cut -d. -f1 /proc/uptime 2>/dev/null || echo 0)

cpu_ticks() { awk '/^cpu /{ b=0; for (i=2; i<=9; i++) b+=$i; print b }' /proc/stat 2>/dev/null; }
proc_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null; }
rss_kb() { awk '{print int($2 * 4)}' "/proc/$1/statm" 2>/dev/null; }
comm_of() { cat "/proc/$1/comm" 2>/dev/null; }
cmdline_of() { tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | cut -c1-58; }

T1=$(cpu_ticks)
: > "$OUT/.p1"
for d in /proc/[0-9]*; do
    pid=$(basename "$d")
    t=$(proc_ticks "$pid")
    [ -n "$t" ] || continue
    printf '%s %s %s\n' "$pid" "$t" "$(comm_of "$pid")" >> "$OUT/.p1"
done

sleep "$SECS"

T2=$(cpu_ticks)
TOTAL_DELTA=$((T2 - T1))
[ "$TOTAL_DELTA" -gt 0 ] 2>/dev/null || TOTAL_DELTA=1
: > "$OUT/.cpu"
for d in /proc/[0-9]*; do
    pid=$(basename "$d")
    t=$(proc_ticks "$pid")
    [ -n "$t" ] || continue
    old=$(awk -v p="$pid" '$1 == p { print $2; exit }' "$OUT/.p1" 2>/dev/null)
    [ -n "$old" ] || continue
    delta=$((t - old))
    [ "$delta" -gt 0 ] 2>/dev/null || continue
    pct=$((delta * 10000 / TOTAL_DELTA))
    [ "$pct" -gt 0 ] || continue
    printf '%s %s %s\n' "$pct" "$pid" "$(comm_of "$pid")" >> "$OUT/.cpu"
done

{
    echo "=== perf snapshot $NOW (uptime $UP s, sample $SECS s)"
    echo "--- load / memory"
    cat /proc/loadavg 2>/dev/null
    awk '/^(MemTotal|MemFree|MemAvailable|Buffers|Cached|SwapTotal|SwapFree)/{ printf "  %s %s kB\n", $1, $2 }' /proc/meminfo 2>/dev/null
    echo "--- top CPU (percent of one core, sample window)"
    sort -rn "$OUT/.cpu" 2>/dev/null | head -n 12 | while read -r pct pid name; do
        printf "  %5s.%02s%%  pid %-6s %-24s rss=%skB  %s\n" "$((pct / 100))" "$((pct % 100))" "$pid" "$name" "$(rss_kb "$pid")" "$(cmdline_of "$pid")"
    done
    echo "--- navigation map (draws even when not in the foreground)"
    for d in /proc/[0-9]*; do
        pid=$(basename "$d")
        name=$(comm_of "$pid")
        case "$name" in
            *navi*|*Navi*|*map*|*Map*|*tmap*|*TMAP*) : ;;
            *) continue ;;
        esac
        pct=$(awk -v p="$pid" '$2 == p { print $1; exit }' "$OUT/.cpu" 2>/dev/null)
        [ -n "$pct" ] || pct=0
        printf "  %-16s pid %-6s cpu=%s.%02s%% rss=%skB  %s\n" "$name" "$pid" "$((pct / 100))" "$((pct % 100))" "$(rss_kb "$pid")" "$(cmdline_of "$pid")"
    done
    echo "--- top RSS"
    for d in /proc/[0-9]*; do
        pid=$(basename "$d")
        r=$(rss_kb "$pid")
        [ -n "$r" ] || continue
        printf '%s %s %s\n' "$r" "$pid" "$(comm_of "$pid")"
    done 2>/dev/null | sort -rn | head -n 10 | while read -r r pid name; do
        printf "  %8skB  pid %-6s %s\n" "$r" "$pid" "$name"
    done
    echo "--- our daemons"
    for name in wifi_daemon battery_daemon tweaks_daemon ambient_daemon fake_engine_daemon; do
        pid=$(pidof "$name" 2>/dev/null | awk '{print $1}')
        if [ -n "$pid" ]; then
            printf "  %-20s pid %-6s rss=%skB ticks=%s\n" "$name" "$pid" "$(rss_kb "$pid")" "$(proc_ticks "$pid")"
        else
            printf "  %-20s not running\n" "$name"
        fi
    done
    echo "--- ps by cpu"
    ps -o pid,pcpu,pmem,time,comm -e 2>/dev/null | sort -k2 -rn | head -n 12
    echo "--- vmstat 1 3"
    vmstat 1 3 2>/dev/null | sed 's/^/  /'
} > "$REPORT" 2>&1

TOPCPU=$(sort -rn "$OUT/.cpu" 2>/dev/null | head -n 1)
TOP_PCT=$(echo "$TOPCPU" | awk '{print $1}')
TOP_NAME=$(echo "$TOPCPU" | awk '{print $3}')
[ -n "$TOP_PCT" ] || TOP_PCT=0
[ -n "$TOP_NAME" ] || TOP_NAME=none
LOAD=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)
MEMFREE=$(awk '/^MemAvailable/{print $2}' /proc/meminfo 2>/dev/null)
NAVCPU=0
NAVNAME=none
for d in /proc/[0-9]*; do
    pid=$(basename "$d")
    name=$(comm_of "$pid")
    case "$name" in
        *navi*|*Navi*|*map*|*Map*|*tmap*|*TMAP*) : ;;
        *) continue ;;
    esac
    pct=$(awk -v p="$pid" '$2 == p { print $1; exit }' "$OUT/.cpu" 2>/dev/null)
    [ -n "$pct" ] || pct=0
    NAVCPU=$((NAVCPU + pct))
    NAVNAME=$name
done
printf '%s uptime=%s load=%s memavail_kb=%s top_cpu=%s.%s top=%s nav_cpu=%s.%s nav=%s\n' \
    "$NOW" "$UP" "$LOAD" "$MEMFREE" "$((TOP_PCT / 100))" "$((TOP_PCT % 100))" "$TOP_NAME" \
    "$((NAVCPU / 100))" "$((NAVCPU % 100))" "$NAVNAME" >> "$CSV"
rm -f "$OUT/.p1" "$OUT/.cpu" 2>/dev/null || true
echo "perf_snapshot: $REPORT"
head -n 8 "$REPORT"
