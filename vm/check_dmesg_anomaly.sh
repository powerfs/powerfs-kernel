#!/bin/bash
# PowerFS dmesg anomaly checker
# Scans dmesg for kernel anomalies (BUG, Oops, SLUB, KASAN, etc.)
# Used by CI to detect kernel regressions and memory safety issues.
#
# Usage:
#   check_dmesg_anomaly.sh [--port=SSH_PORT] [--baseline=LINE_COUNT] [--label=VM_LABEL]
#
# If --baseline is given, only checks dmesg lines after that line.
# If --port is given, checks a remote VM via SSH (default: local).
#
# Exit codes:
#   0 = no anomaly found
#   1 = anomaly detected (details printed to stderr)

set -euo pipefail

SSH_PORT=""
BASELINE=0
LABEL="local"

while [ $# -gt 0 ]; do
    case "$1" in
        --port=*)     SSH_PORT="${1#*=}"; shift ;;
        --port)       SSH_PORT="$2"; shift 2 ;;
        --baseline=*) BASELINE="${1#*=}"; shift ;;
        --baseline)   BASELINE="$2"; shift 2 ;;
        --label=*)    LABEL="${1#*=}"; shift ;;
        --label)      LABEL="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--port=SSH_PORT] [--baseline=LINE_COUNT] [--label=VM_LABEL]"
            echo "Checks dmesg for kernel anomalies."
            echo "  --port: SSH port for remote VM (default: local)"
            echo "  --baseline: dmesg line count baseline (only check lines after this)"
            echo "  --label: label for output (default: local)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

# Anomaly patterns to detect
# - BUG:/Oops:/KASAN: — kernel crashes and memory safety
# - Poison overwritten/Object corrupt/slab_err — SLUB use-after-free / buffer overflow
# - Call Trace — any kernel stack trace (filter for powerfs if too noisy)
# - WARNING:.*powerfs — powerfs-specific warnings
# - general protection fault / unable to handle — segfault / page fault
# - RCU stall / hung task — scheduler issues
# - soft lockup — CPU lockup
#
# Exclusion: "report a bug" is a standard PCI driver message, not an actual bug.
ANOMALY_PATTERN='BUG:|Oops:|KASAN:|Poison overwritten|Object corrupt|slab_err|Call Trace|WARNING:.*powerfs|general protection fault|unable to handle|RCU stall|hung task|soft lockup|Kernel panic'
EXCLUDE_PATTERN='report a bug'

# Collect dmesg
if [ -n "$SSH_PORT" ]; then
    if [ "$BASELINE" -gt 0 ]; then
        DMESG_OUTPUT=$(ssh -p "$SSH_PORT" -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@localhost "dmesg | tail -n +$((BASELINE + 1))" 2>/dev/null || true)
    else
        DMESG_OUTPUT=$(ssh -p "$SSH_PORT" -o StrictHostKeyChecking=no -o ConnectTimeout=5 root@localhost "dmesg" 2>/dev/null || true)
    fi
else
    if [ "$BASELINE" -gt 0 ]; then
        DMESG_OUTPUT=$(dmesg | tail -n +$((BASELINE + 1)) 2>/dev/null || true)
    else
        DMESG_OUTPUT=$(dmesg 2>/dev/null || true)
    fi
fi

if [ -z "$DMESG_OUTPUT" ]; then
    echo "[$LABEL] dmesg empty or inaccessible"
    exit 0
fi

# Check for anomalies
ANOMALIES=$(echo "$DMESG_OUTPUT" | grep -iE "$ANOMALY_PATTERN" | grep -ivE "$EXCLUDE_PATTERN" || true)

if [ -n "$ANOMALIES" ]; then
    echo "[$LABEL] DMESG ANOMALY DETECTED:" >&2
    echo "$ANOMALIES" >&2

    # Extract SLUB-specific details for better issue reporting
    SLUB_DETAILS=$(echo "$DMESG_OUTPUT" | grep -iE 'Poison overwritten|Object corrupt|slab_err|kmalloc-' || true)
    if [ -n "$SLUB_DETAILS" ]; then
        echo "" >&2
        echo "[$LABEL] SLUB Details:" >&2
        echo "$SLUB_DETAILS" | head -5 >&2
    fi

    # Extract Call Trace context (2 lines before, 10 after)
    TRACE_CONTEXT=$(echo "$DMESG_OUTPUT" | grep -B 2 -A 10 "Call Trace" | head -30 || true)
    if [ -n "$TRACE_CONTEXT" ]; then
        echo "" >&2
        echo "[$LABEL] Call Trace context:" >&2
        echo "$TRACE_CONTEXT" >&2
    fi

    exit 1
else
    DMESG_LINES=$(echo "$DMESG_OUTPUT" | wc -l)
    echo "[$LABEL] dmesg OK ($DMESG_LINES lines checked, no anomaly)"
    exit 0
fi
