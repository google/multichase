#/bin/bash
printf "%4s %7s %7s %7s %7s\n" "CPU" "NODE0" "NODE1" "NODE2" "NODE3"
	 
for cpu in $(seq 0 24 184)
do
	printf "%4s " ${cpu}
	for numa in $(seq 0 3)
	do
		result=$(numactl -C ${cpu} -m ${numa} ./multichase -s 512 -m 1g -n 120)
		printf "%7.1f " ${result}
	done
	printf "\n"
done

