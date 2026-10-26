#!/bin/sh
# Publish real CPU temperatures; missing/stale/invalid input removes the sample.
set -eu
output=${THERMAL_OUTPUT:-/thermal}
cleanup() {
    trap - EXIT TERM INT
    if [ -n "${sleeper:-}" ]; then
        kill "$sleeper" 2>/dev/null || :
        wait "$sleeper" 2>/dev/null || :
    fi
    rm -f "$output/cpu_millidegrees" "$output/cpu.new"
}
trap cleanup EXIT
trap 'exit 0' TERM INT
mkdir -p "$output"
while :; do
    sensor=${SOURCE_FILE:-}
    if [ -z "$sensor" ]; then
        for name in /sys/class/hwmon/hwmon*/name; do
            if [ "$(cat "$name" 2>/dev/null)" = "${SENSOR_NAME:-k10temp}" ]; then
                sensor="${name%/name}/temp1_input"
                break
            fi
        done
    fi
    sample=
    if [ -n "$sensor" ] && [ -r "$sensor" ]; then
        fresh=1
        if [ -n "${SOURCE_FILE:-}" ]; then
            modified=$(stat -c %Y "$sensor" 2>/dev/null || echo 0)
            age=$(( $(date +%s) - modified ))
            if [ "$age" -lt 0 ] || [ "$age" -gt 5 ]; then fresh=0; fi
        fi
        if [ "$fresh" = 1 ]; then sample=$(cat "$sensor" 2>/dev/null || :); fi
    fi
    case "$sample" in
        ''|*[!0-9]*) sample= ;;
        *) if [ "${#sample}" -gt 6 ] || [ "$sample" -lt 1000 ] || [ "$sample" -gt 130000 ]; then sample=; fi ;;
    esac
    if [ -n "$sample" ]; then
        printf '%s\n' "$sample" > "$output/cpu.new"
        mv "$output/cpu.new" "$output/cpu_millidegrees"
    else
        rm -f "$output/cpu_millidegrees" "$output/cpu.new"
    fi
    sleep 1 & sleeper=$!
    wait "$sleeper"
    sleeper=
done
