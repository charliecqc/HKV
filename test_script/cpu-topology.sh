processor_list=$(lscpu | grep "Core(s) per socket" | awk '{print $NF}')
echo $processor_list
