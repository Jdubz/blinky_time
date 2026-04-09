#!/bin/bash
#
# Disk audit for blinky_time project
#
# Checks all known data locations for bloat and reports actionable findings.
# Run periodically or before large operations (dataprep, training).
#
# Usage: ./scripts/disk-audit.sh

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ML_DATA="/mnt/storage/blinky-ml-data"

echo -e "${BOLD}=== Blinky Disk Audit ===${NC}"
echo ""

# --- Drive overview ---
echo -e "${BOLD}Drive Usage:${NC}"
df -h / /mnt/storage /mnt/sda /mnt/speedy 2>/dev/null | tail -n +2 | while read line; do
    use_pct=$(echo "$line" | awk '{print $5}' | tr -d '%')
    if [ "$use_pct" -gt 80 ] 2>/dev/null; then
        echo -e "  ${RED}$line${NC}"
    elif [ "$use_pct" -gt 60 ] 2>/dev/null; then
        echo -e "  ${YELLOW}$line${NC}"
    else
        echo -e "  ${GREEN}$line${NC}"
    fi
done
echo ""

# --- Git pack size ---
echo -e "${BOLD}Git Repository:${NC}"
cd "$REPO_ROOT"
PACK_KB=$(git count-objects -v 2>/dev/null | awk '/size-pack:/ {print $2}')
PACK_MB=$((PACK_KB / 1024))
GIT_TOTAL=$(du -sh .git 2>/dev/null | awk '{print $1}')
if [ "$PACK_MB" -gt 500 ]; then
    echo -e "  ${RED}.git: $GIT_TOTAL (pack: ${PACK_MB} MB) — run 'git gc --prune=now'${NC}"
else
    echo -e "  ${GREEN}.git: $GIT_TOTAL (pack: ${PACK_MB} MB)${NC}"
fi
echo ""

# --- ML processed data ---
if [ -d "$ML_DATA" ]; then
    echo -e "${BOLD}ML Processed Data:${NC}"
    processed_count=0
    for d in "$ML_DATA"/processed_*; do
        [ -d "$d" ] || continue
        processed_count=$((processed_count + 1))
        d_size=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
        # Check if it has both train and val
        has_train=$([ -f "$d/X_train.npy" ] && echo "yes" || echo "no")
        has_val=$([ -f "$d/X_val.npy" ] && echo "yes" || echo "no")
        if [ "$has_val" = "no" ]; then
            echo -e "  ${RED}$d: $d_size — INCOMPLETE (no val split)${NC}"
        else
            echo -e "  $d: $d_size (train=$has_train, val=$has_val)"
        fi
    done
    if [ "$processed_count" -eq 0 ]; then
        echo -e "  ${GREEN}No processed dirs (clean)${NC}"
    elif [ "$processed_count" -gt 1 ]; then
        echo -e "  ${YELLOW}$processed_count versions present — consider deleting old ones${NC}"
    fi
    echo ""

    # --- Mel cache ---
    echo -e "${BOLD}Mel Cache:${NC}"
    if [ -d "$ML_DATA/mel_cache" ]; then
        cache_count=0
        for d in "$ML_DATA/mel_cache"/*/; do
            [ -d "$d" ] || continue
            cache_count=$((cache_count + 1))
            d_size=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
            track_count=$(find "$d" -maxdepth 1 -type d | wc -l)
            track_count=$((track_count - 1))
            echo "  $(basename "$d"): $d_size ($track_count tracks)"
        done
        if [ "$cache_count" -gt 1 ]; then
            echo -e "  ${YELLOW}$cache_count cache dirs — only the current config hash is needed${NC}"
        fi
    else
        echo "  No mel cache"
    fi
    echo ""

    # --- Orphaned shard dirs ---
    echo -e "${BOLD}Orphaned Shard Dirs:${NC}"
    orphans=$(find "$ML_DATA" -maxdepth 2 -name 'blinky_*' -type d 2>/dev/null)
    if [ -n "$orphans" ]; then
        echo "$orphans" | while read d; do
            d_size=$(du -sh "$d" 2>/dev/null | awk '{print $1}')
            echo -e "  ${RED}$d: $d_size — orphaned temp dir, safe to delete${NC}"
        done
    else
        echo -e "  ${GREEN}None${NC}"
    fi
    echo ""

    # --- Summary ---
    echo -e "${BOLD}Storage Summary:${NC}"
    total=$(du -sh "$ML_DATA" 2>/dev/null | awk '{print $1}')
    free=$(df -h "$ML_DATA" 2>/dev/null | tail -1 | awk '{print $4}')
    echo "  ML data total: $total"
    echo "  Free space: $free"
fi
