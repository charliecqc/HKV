#!/bin/bash

# Run your workload here and get its PID
{{your_workload_command}} &

# Get the PID of the last background process
workload_pid=$!

# Run perf record with the workload's PID
perf record -p $workload_pid
