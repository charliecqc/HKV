#!/bin/bash
threads=(4 8 12 16)

for thread in "${threads[@]}"
do
	echo "Running workload with $thread threads"
	../clear_prism.sh
	../workload c zipf $thread > result_$thread.txt
done
