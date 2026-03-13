#!/bin/bash
# Get total MediaWrites across both DIMMs
get_media_writes() {
    sudo ipmctl show -dimm -performance MediaWrites | grep MediaWrites | awk -F'=' '{sum += $2} END {print sum}'
}
rm -rf /mnt/pmem0/* 
WRITES_BEFORE=$(get_media_writes)
numactl --cpunodebind=0 --membind=0 ./project a zipf 16 /mnt/pmem0 --insert-only
WRITES_AFTER=$(get_media_writes)

DELTA=$((WRITES_AFTER - WRITES_BEFORE))
echo "Total PMEM writes: $(echo "scale=2; $DELTA * 64 / 1024 / 1024 / 1024" | bc) GB"
