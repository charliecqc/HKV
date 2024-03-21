#!/bin/bash

# Run the workload command in the background and capture the output
output=$(./workload a zipf 8&)

# Loop to check if "Elapsed_time" is present in the output
while true; do
	echo "$output"
    sleep 1
done
