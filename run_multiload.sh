#!/bin/bash
# Copyright 2020 Ampere Computing LLC. All Rights Reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ARCH=$(uname -p)
HOSTNAME=$(cat /etc/hostname)
DATE=`date +"%Y_%m-%d_%H.%M.%S.%p"`
TOPDIR=$(pwd)
start_time=$(date +'%s')
MULTILOAD_PATH=${TOPDIR}
BINDIR="${MULTILOAD_PATH}"
OUTPUT_DIR="${TOPDIR}/results"
mkdir -p ${OUTPUT_DIR}
LOG_FILE="${OUTPUT_DIR}/multiload_${DATE}_${HOSTNAME}_run.log"
log(){
    echo -e "[$(date +"%m/%d %H:%M:%S %p")] $1" | tee -a $LOG_FILE
}

log_without_date(){
    echo -e "$1" | tee -a $LOG_FILE
}

################################################################################
# Test configuration variables
################################################################################
RUN_TEST_TYPE=2     # 0=latency only, 1=load only, 2=loaded latency
ITERATIONS=3        # Number of times the program is run. Due to "Samples" above, reliable data can typically be had with only 1 iteration.
SAMPLES=5           # Specfies the number of data samples taken during a single run of mulitiload
                    # ~2sec per sample + 4sec warmup. Duration depends on LOAD_DELAY_* defines in multiload.c
                    # Default is to return the best latency of the samples. Command "-a" can be used to return the average.
SOCKET_EVAL=1                # 1=1P testing on a 2P system. 2=2P testing on a 2P system. Does not apply to a Non-Numa system.
USE_REMOTE_MEMNODE=0  # Numactl only: 0= use localalloc, 1=force SOCKET_EVAL=1 and use remote memory (ie. 2nd half of the numa nodes)
THREAD_AFFINITY_ENABLED=1  # enables use of tasket/numactl for thread control
MPSTAT_PROFILE_ENABLE=0     #enables mpstat data collection
VMSTAT_PROFILE_ENABLE=0     #enables vmstat data collection
PROFILING_INTERVAL_SEC=3    #defines sampling rate

usage(){
    echo "========================================================================================"
    echo "Multiload memory read latency, bandwidth, and loaded-latency Benchmark Test"
    echo "hostname is ${HOSTNAME}, $(date)"
    echo "========================================================================================"
    echo " "
    echo "Command args: $0 <run_test_type> <iterations> <samples> <socket_eval> <remote_memnode> <thread_affinity>"
    echo "Default is:   $0 $RUN_TEST_TYPE $ITERATIONS $SAMPLES $SOCKET_EVAL $USE_REMOTE_MEMNODE $THREAD_AFFINITY_ENABLED"
    echo " "
    echo "<run_test_type>"
    echo "      0 = Memory Read Latency (Runs multichase \"simple\" test, but all multichase commands should work manually)"
    echo "      1 = Memory Bandwidth    (Runs a list of bandwidth load algorithms)"
    echo "      2 = Loaded Latency.     (\"Chaseload\" combines 1 \"simple\" latency thread and multiple Bandwidth threads)"
    echo " "
    echo "The following are optional"
    echo "<iterations:    >  Number of multiload test runs.                                            (Default=$ITERATIONS)"
    echo "<samples:       >  Data samples per test run.                                                (Default=$SAMPLES)"
    echo "<socket_eval    >  2P system only: 1=test as 1P, 2=test as 2P.                               (Default=$SOCKET_EVAL)"
    echo "<remote_memnode >  NUMA system only: 0= --localalloc, 1= -membind to 2nd half of numa nodes  (Default=$USE_REMOTE_MEMNODE)"
    echo "<thread_affinity>  0=no affinity, 1=use taskset for No-Numa and use numactl for Numa systems (Default=$THREAD_AFFINITY_ENABLED)"
    echo " "
    echo "========================================================================================"
    echo "Chase algorithm list. Issue multiload -h command for full list. The 2 used by this script are:"
    echo "   simple - randomized pointer chaser latency"
    echo "   chaseload - Runs 1 thread of \"simple\" latency with multiple threads using the loads below."
    echo " "
    echo "Load algorithm list to test various rd/wr ratios. More algorithms can easily be added to multiload.c. Current algorithms are:"
    echo "   memcpy-libc    1:1 rd:wr ratio - glibc memcpy()"
    echo "   memset-libc    0:1 rd:wr ratio - glibc memset() non-zero data"
    echo "   memsetz-libc   0:1 rd:wr ratio - glibc memset() zero data"
    echo "   stream-copy    1:1 rd:wr ratio - lmbench stream copy instructions b[i]=a[i] (actual binary depends on compiler & -O level)"
    echo "   stream-sum     1:0 rd:wr ratio - lmbench stream sum instructions: a[i]+=1   (actual binary depends on compiler & -O level)"
    echo "   stream-triad   2:1 rd:wr ratio - lmbench stream triad instructions: a[i]=b[i]+(scalar*c[i])"
    echo " "
    echo "*** Due to the complexity of other options, they can only be changed by editting this script"
    echo "========================================================================================"
}

if [ "$#" == "0" ] ; then
    usage
    exit 1
fi
if [ ! -z $1 ]; then
    RUN_TEST_TYPE=$1
    if [ ! -z $2 ]; then
        ITERATIONS=$2
        if [ ! -z $3 ]; then
            SAMPLES=$3
            if [ ! -z $4 ]; then
                SOCKET_EVAL=$4
                if [ ! -z $5 ]; then
                    USE_REMOTE_MEMNODE=$5
                    if [ ! -z $6 ]; then
                        THREAD_AFFINITY_ENABLED=$6
                    fi
                fi
            fi
        fi
    fi
fi

RUN_CHASE=0
RUN_BANDWIDTH=1
RUN_CHASE_LOADED=2
if [ $RUN_TEST_TYPE = $RUN_CHASE ]; then
    PSTEP_START=1                # Parallel thread start value when running thread scaling tests.
    PSTEP_INC=4                 # Parallel thread steps when running thread scaling tests.
    PSTEP_END=512         # Will be reduced to CPUTHREADS if CPUTHREADS < PSTEP_END.
    CHASE_ALGORITHM="simple"
    LOAD_ALGORITHM_LIST="none"
    RAND_STRIDE=16      #lmbench latmemrd uses 16 for simple chase. Other chase/mem sizes may need to be bigger (ie. 512).
    BUFLIST_TYPE=0       # 0=Use MEM_SIZE* to create a memory list, 1=use buflist_custom
    let MEM_SIZE_END_B=1*1024*1024*1024
    let MEM_SIZE_START_B=4*1024
    #let MEM_SIZE_START_B=MEM_SIZE_END_B
    buflist_custom=( $((32*1024)) $((512*1024)) $((16*1024*1024)) $((1*1024*1024*1024)) )     # 64K / 1M / 32M caches

elif [ $RUN_TEST_TYPE = $RUN_BANDWIDTH ]; then
    PSTEP_START=1                # Parallel thread start value when running thread scaling tests.
    PSTEP_INC=4                 # Parallel thread steps when running thread scaling tests.
    PSTEP_END=512       # Will be reduced to CPUTHREADS if CPUTHREADS < PSTEP_END.
    CHASE_ALGORITHM="none"
    LOAD_ALGORITHM_LIST="memcpy-libc memset-libc memsetz-libc stream-sum stream-triad"
    RAND_STRIDE=16       #not used for bandwidth test
    BUFLIST_TYPE=1       # 0=Use MEM_SIZE* to create a memory list, 1=use buflist_custom
    let MEM_SIZE_END_B=1*1024*1024*1024
    #let MEM_SIZE_START_B=4*1024
    let MEM_SIZE_START_B=MEM_SIZE_END_B
    buflist_custom=( $((32*1024)) $((512*1024)) $((16*1024*1024)) $((1*1024*1024*1024)) )     # 64K / 1M / 32M caches

elif [ $RUN_TEST_TYPE = $RUN_CHASE_LOADED ]; then
    PSTEP_START=1                # Parallel thread start value when running thread scaling tests.
    PSTEP_INC=4                 # Parallel thread steps when running thread scaling tests.
    PSTEP_END=512       # Will be reduced to CPUTHREADS if CPUTHREADS < PSTEP_END.
    CHASE_ALGORITHM="chaseload"
    LOAD_ALGORITHM_LIST="memcpy-libc memset-libc memsetz-libc stream-sum stream-triad"
    RAND_STRIDE=16       #lmbench latmemrd uses 16 for simple chase. Other chase/mem sizes may need to be bigger (ie. 512).
    BUFLIST_TYPE=0       # 0=Use MEM_SIZE* to create a memory list, 1=use buflist_custom
    let MEM_SIZE_END_B=1*1024*1024*1024
    #let MEM_SIZE_START_B=4*1024
    let MEM_SIZE_START_B=MEM_SIZE_END_B
    buflist_custom=( $((32*1024)) $((512*1024)) $((16*1024*1024)) $((1*1024*1024*1024)) )     # 64K / 1M / 32M caches
else
    echo "Found unknown RUN_TEST_TYPE=$RUN_TEST_TYPE"
    usage
    exit
fi

################################################################################
# Functions
################################################################################
profiling_start(){
    if [ "$MPSTAT_PROFILE_ENABLE" == "1" ] ; then
        echo "$1" >> ${LOG_MPSTATS_FILE}
        mpstat -P ALL $PROFILING_INTERVAL_SEC >> ${LOG_MPSTATS_FILE} 2>&1 &
        mpstat_pid=$!
    fi
    if [ "$VMSTAT_PROFILE_ENABLE" == "1" ] ; then
        echo "$1" >> ${LOG_VMSTATS_FILE}
        vmstat -t $PROFILING_INTERVAL_SEC >> ${LOG_VMSTATS_FILE} 2>&1 &
        vmstat_pid=$!
    fi
}

profiling_end(){
    # kill the profiling pids and try to hide the "terminated" messages
    if [ "$MPSTAT_PROFILE_ENABLE" == "1" ] ; then
        ( kill $mpstat_pid &> /dev/null ) &
        wait $mpstat_pid &> /dev/null
    fi
    if [ "$VMSTAT_PROFILE_ENABLE" == "1" ] ; then
        ( kill $vmstat_pid &> /dev/null ) &
        wait $vmstat_pid &> /dev/null
    fi
}

get_hardware_config ()
{
        log_without_date " "
        numactl --hardware | tee -a ${LOG_FILE}                 # display current NUMA & memory setup
        log_without_date " "

        phycore_num=`lscpu | grep "Core(s) per socket" | tr -d ' ' | cut -d':' -f2 2> /dev/null`
        core_threads=`lscpu | grep "Thread(s) per core:" | tr -d ' ' | cut -d':' -f2 2> /dev/null`
        cputhread_num=`lscpu | grep "CPU(s):            " | head -n 1 | tr -d ' ' | cut -d ':' -f2 2> /dev/null`
        numa_num=`lscpu | grep "NUMA node(s)" | tr -d ' ' | cut -d':' -f2 2> /dev/null`
        socket_num=`lscpu | grep "Socket(s)" | tr -d ' ' | cut -d':' -f2 2> /dev/null`
    MEMBIND_LIST=`numactl --show 2> /dev/null | grep membind | cut -d':' -f2 2> /dev/null`
           let phycore_end=$phycore_num-1
        let ht_threads=$phycore_num*$core_threads
    #echo "get_hardware_config: phyend=$phycore_end, ht_t=$ht_threads"

        if [ -z $cputhread_num ]; then
                log_without_date "Can't find the CPU(s) core count, exiting"
                exit $?
        else
                CPUTHREADS=$cputhread_num
        fi

    log "Found the following hardware:"
        log_without_date "  sockets         = $socket_num"
        log_without_date "  physical cores  = $phycore_num"
        log_without_date "  threads per core= $core_threads"
        log_without_date "  logical threads = $cputhread_num"
        if [ -z "$numa_num" ]; then
                NUMA_NODES=1
            log_without_date "  NUMA nodes      = none found"
        else
                NUMA_NODES=$numa_num
            log_without_date "  NUMA nodes      = $NUMA_NODES"
        fi

    if [ $USE_REMOTE_MEMNODE == "1" ] && [ $NUMA_NODES -gt "1" ] && [ $THREAD_AFFINITY_ENABLED == "1" ]; then
        SOCKET_EVAL=1
        if [ $numa_num == "2" ]; then
            NODE_MEMBIND="1"
        elif [ $numa_num == "4" ]; then
            NODE_MEMBIND="2,3"
        elif [ $numa_num == "8" ]; then
            NODE_MEMBIND="4,5,6,7"
        else
            NODE_MEMBIND="0"
        fi
    fi

    if [ $socket_num -eq "1" ]; then
        #Check if this is only a 1P box force SOCKET_EVAL=1
        SOCKET_EVAL=1
    elif [ $socket_num -ge "2" ] && [ $SOCKET_EVAL -eq "1" ]; then
        #Check if doing 1P only testing on a 2P+ box and adjust CPUTHREADS for 1P
        let CPUTHREADS=$cputhread_num/$socket_num
    fi

    if [ $PSTEP_END -gt $CPUTHREADS ]; then
            PSTEP_END=$CPUTHREADS
    fi
}

duration(){    # calculates duration in secs
    duration=$SECONDS
    log_without_date
        log "$1 runtime: $(($duration / 3600)) hrs, $((($duration % 3600) / 60)) mins, $(($duration % 60)) secs"
}

#converts an array into string and deletes spaces (can also add delimiters using $1)
join_ws() { local d=$1 s=$2; shift 2 && printf %s "$s${@/#/$d}"; }

create_taskset_cpulist_x86_64()
{
    let cpu_max_4bits=$1/4+1           #need +1 in case cputhreads is not a multiple of 4.
    #create base string arrays
    for (( cpu=0; cpu<cpu_max_4bits; cpu++ ));
        do
            x02[cpu]="0"
        done

    let idx=cpu_max_4bits-1
    for (( cpu=0; cpu<cpu_max_4bits; cpu++ ));
        do
            x02[idx]="1"
            thds=`join_ws '' ${x02[@]}`
            cpulist+=("$thds")
            x02[idx]="3"
            thds=`join_ws '' ${x02[@]}`
            cpulist+=("$thds")
            x02[idx]="7"
            thds=`join_ws '' ${x02[@]}`
            cpulist+=("$thds")
            x02[idx]="F"
            thds=`join_ws '' ${x02[@]}`
            cpulist+=("$thds")
            let idx=idx-1
        done
    #for i in "${cpulist[@]}";
    #    do
    #        echo "$i"
    #    done
}

create_taskset_cpulist_arm64()
{
    cc=$1
    cpu_max_4bits_rem=$( expr ${cc} % 4 )       #in case its not a multiple of 4.
    let cpu_max_4bits=$cc/4
    #echo "4b=$cpu_max_4bits, 4br=$cpu_max_4bits_rem"

    #create base string arrays
    for (( cpu=0; cpu<(cpu_max_4bits); cpu++ ));
        do
            x00[cpu]="0"
        done
    if [ $cpu_max_4bits_rem -eq 2 ]; then
        x00[cpu]="0"
        let idx=cpu_max_4bits
    else
        let idx=cpu_max_4bits-1
    fi
    #echo "Initialize x00=${x00[@]}"
    #echo "--------------"

    for (( cpu=0; cpu<cpu_max_4bits; cpu++ ));
        do
            x00[idx]="2"
            thds=`join_ws '' ${x00[@]}`
            cpulist+=("$thds")
            #echo "idx=$idx, ${cpulist[@]}"
            x00[idx]="A"
            thds=`join_ws '' ${x00[@]}`
            cpulist+=("$thds")
            #echo "idx=$idx, ${cpulist[@]}"
            let idx=idx-1
        done
    if [ $cpu_max_4bits_rem -eq 2 ]; then
        x00[idx]="2"
        thds=`join_ws '' ${x00[@]}`
        cpulist+=("$thds")
        #echo "idx=$idx, ${cpulist[@]}"
        let idx=cpu_max_4bits
    else
        let idx=cpu_max_4bits-1
    fi
    #echo "--------------"

    for (( cpu=0; cpu<cpu_max_4bits; cpu++ ));
        do
            x00[idx]="B"
            thds=`join_ws '' ${x00[@]}`
            cpulist+=("$thds")
            #echo "idx=$idx, ${cpulist[@]}"
            x00[idx]="F"
            thds=`join_ws '' ${x00[@]}`
            cpulist+=("$thds")
            #echo "idx=$idx, ${cpulist[@]}"
            let idx=idx-1
        done
    if [ $cpu_max_4bits_rem -eq 2 ]; then
            x00[idx]="3"
            thds=`join_ws '' ${x00[@]}`
            cpulist+=("$thds")
            #echo "idx=$idx, ${cpulist[@]}"
    fi
    #echo "--------------"

    #for i in "${cpulist[@]}";
    #    do
    #        echo "$i"
    #    done
}

create_numactl_cpulist_x86_64(){
    let phycore_end=phycore_num*SOCKET_EVAL-1
    let ht_thread_start=phycore_num*socket_num
    let ht_thread_end=ht_thread_start+phycore_end
    #echo "SOCKET_EVAL=$SOCKET_EVAL, Available cpus are: -C 0-$phycore_end,$ht_thread_start-$ht_thread_end, cputhread_num=$cputhread_num, request=$1"
    if [ $1 -gt $cputhread_num ]; then
        echo "ERROR: $1 is too many threads, Max CPU threads allowed is $cputhread_num"
        exit
    fi

    for (( cpu=0; cpu<$1; cpu++ ));
        do
            if [ $cpu -gt $phycore_end ]; then
                let cpu_bind_end=$cpu-phycore_end+ht_thread_start-1
                stream_bind=0-${phycore_end},${ht_thread_start}-${cpu_bind_end}
                cpulist+=("$stream_bind")
                #echo "Using hyperthreads: -C $stream_bind"
            else
                stream_bind=0-${cpu}
                cpulist+=("$stream_bind")
                #echo "Using threads: -C $stream_bind"
            fi
        done
}

create_numactl_cpulist_arm64(){
        #echo "FIXME: early arm designs need every other cpu alogithm to test non-shared L2 followed by shared L2"
    create_numactl_cpulist_x86_64 $1
}

create_thdlist()
{
    thdtemp=$PSTEP_START
    #echo "create_thdlist start=$thdtemp, end=$PSTEP_END, by $PSTEP_INC"

    thdcount_testlist+=($thdtemp)
        if [ $thdtemp -eq "1" ]; then
                if [ $PSTEP_INC -eq "1" ]; then
                        let thdtemp=2
                else
                        let thdtemp=PSTEP_INC
                fi
        else
                let thdtemp=thdtemp+PSTEP_INC
        fi

    while [ "$thdtemp" -le "$PSTEP_END" ] ; do
        thdcount_testlist+=($thdtemp)
                let thdtemp=thdtemp+PSTEP_INC
    done
}

create_buffer_list_by_2x()
{
    #Algorithm below just doubles the mem size each time.
    size=$MEM_SIZE_START_B
    bufsize_testlist=($size)
    #echo "create_buffer_list_by_2x size=$size, end=$MEM_SIZE_END_B"

    while [ "$size" -lt "$MEM_SIZE_END_B" ] ; do
        size=$((size*2))
        bufsize_testlist+=($size)
    done
}

create_buffer_list_lmbench()
{
    #Algorithm below duplicates lmbench lat-mem-rd list.
    size=$MEM_SIZE_START_B
    bufsize_testlist=($size)
    #echo "create_buffer_list_lmbench, start=$size, end=$MEM_SIZE_END_B bytes"

    while [ "$size" -lt "$MEM_SIZE_END_B" ] ; do
        if [ "$size" -lt "1024" ]; then
            let size=$((size*2))
        elif [ "$size" -lt "4096" ]; then
            let size=size+1024
        else
            for (( temps=4096; temps<=size; temps=temps*2 ));
            do
                nothing=1
            done
            let tempss=temps/4
            let size=size+tempss
        fi
        bufsize_testlist+=($size)
    done
}

create_buffer_list_custom()
{
    for i in "${buflist_custom[@]}"; do
        bufsize_testlist+=($i)
    done
    for i in "${bufsize_testlist[@]}"; do echo "$i"; done;
}

run_test(){
    log "Starting ${1} run..."
    SECONDS=0
        for a in $LOAD_ALGORITHM_LIST; do
            if [ "$a" == "none" ]; then
                LOAD_COMMAND=""
            else
                LOAD_COMMAND="-l ${a}"
            fi
            for t in "${thdcount_testlist[@]}"; do
                for j in "${bufsize_testlist[@]}"; do
                    if [ "$THREAD_AFFINITY_ENABLED" == "0" ]; then
                        profiling_start "Run $ITERATIONS iterations, $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND} -X"
                        for i in $(seq 1 $ITERATIONS) ; do
                            log "Run iter $i/$ITERATIONS, $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND} -X"
                            ${BASE_MULTILOAD_CMD} -t $t -m $j ${LOAD_COMMAND} -X | tee -a ${OUTPUT_DIR}/$FILENAME.txt 2>&1
                        done
                    elif [ $NUMA_NODES -eq 1 ] ; then
                        profiling_start "Run $ITERATIONS iterations, taskset ${cpulist[$t-1]} $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND}"
                        for i in $(seq 1 $ITERATIONS) ; do
                            log             "Iteration $i of $ITERATIONS, taskset ${cpulist[$t-1]} $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND}"
                            taskset ${cpulist[$t-1]} ${BASE_MULTILOAD_CMD} -t $t -m $j ${LOAD_COMMAND} | tee -a ${OUTPUT_DIR}/$FILENAME.txt 2>&1
                        done
                    else
                        profiling_start "Run $ITERATIONS iterations, numactl ${MEMBIND_COMMAND} -C ${cpulist[$t-1]} $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND}"
                        for i in $(seq 1 $ITERATIONS) ; do
                            log "Run iter $i/$ITERATIONS, numactl ${MEMBIND_COMMAND} -C ${cpulist[$t-1]} $BASE_MULTILOAD_CMD -t $t -m $j ${LOAD_COMMAND}"
                            numactl ${MEMBIND_COMMAND} -C ${cpulist[$t-1]} ${BASE_MULTILOAD_CMD} -t $t -m $j ${LOAD_COMMAND} | tee -a ${OUTPUT_DIR}/$FILENAME.txt 2>&1
                        done
                    fi
                    profiling_end &> /dev/null
                    duration "Total"
                done
            done
        done
}

parse(){
    first=1
    rm -f out.txt
    while IFS= read -r line; do
        #Only keep 1st header line
        if [ "$first" -eq "1" ]; then
            echo "$line" > out.txt
            first=0
        elif [[ ! $line =~ "ample" ]]; then
            echo "$line" >> out.txt
        fi
    done < ${OUTPUT_DIR}/$1.txt

    #delete all spaces and tabs
    tr -d '[[:blank:]]' < out.txt > ${OUTPUT_DIR}/$1.csv
    rm -f out.txt
}

################################################################################
# Main
################################################################################
get_hardware_config
create_thdlist
if [ $BUFLIST_TYPE == "0" ]; then
    create_buffer_list_lmbench
else
    create_buffer_list_custom
fi

if [ $CHASE_ALGORITHM == "none" ]; then
    BASE_MULTILOAD_CMD="${BINDIR}/multiload -s ${RAND_STRIDE} -T 16g -n ${SAMPLES}"
    FILENAME="multiload_${DATE}_-s_${RAND_STRIDE}_-T_16g_-n_${SAMPLES}_-m_${bufsize_testlist[0]}-${bufsize_testlist[-1]}_-t_${thdcount_testlist[0]}-${thdcount_testlist[-1]}_EVAL_${SOCKET_EVAL}P"
else
    BASE_MULTILOAD_CMD="${BINDIR}/multiload -s ${RAND_STRIDE} -T 16g -n ${SAMPLES} -c ${CHASE_ALGORITHM}"
    FILENAME="multiload_${DATE}_-s_${RAND_STRIDE}_-T_16g_-n_${SAMPLES}_-c_${CHASE_ALGORITHM}_-m_${bufsize_testlist[0]}-${bufsize_testlist[-1]}_-t_${thdcount_testlist[0]}-${thdcount_testlist[-1]}_EVAL_${SOCKET_EVAL}P"
fi
if [ $USE_REMOTE_MEMNODE == "1" ] && [ $NUMA_NODES -gt "1" ] && [ $THREAD_AFFINITY_ENABLED == "1" ]; then
    FILENAME="${FILENAME}_REMOTE"
fi
LOG_VMSTATS_FILE="${OUTPUT_DIR}/multiload_${DATE}_${HOSTNAME}_vmstats.log"
LOG_MPSTATS_FILE="${OUTPUT_DIR}/multiload_${DATE}_${HOSTNAME}_mpstats.log"
mpstat_pid=""
vmstat_pid=""

log_without_date "Test Parameters"
log_without_date "  Date: $DATE"
log_without_date "  Output Directory: $OUTPUT_DIR"
log_without_date "  Data File: $FILENAME.txt"
log_without_date "  Data File: $FILENAME.csv"
log_without_date "  Log File: $LOG_FILE"
if [ $MPSTAT_PROFILE_ENABLE -eq 1 ]; then
    log_without_date "  Stat File: $LOG_MPSTATS_FILE"
fi
if [ $VMSTAT_PROFILE_ENABLE -eq 1 ]; then
    log_without_date "  Stat File: $LOG_VMSTATS_FILE"
fi
log_without_date "  Iterations: $ITERATIONS"
log_without_date "  Thread List: $( echo "${thdcount_testlist[@]}" )"
log_without_date "  Mem Buf List: $( echo "${bufsize_testlist[@]}" )"
log_without_date "  Random Stride: $RAND_STRIDE"
if [ "$THREAD_AFFINITY_ENABLED" == "0" ]; then
    log_without_date "  Thread affinity disabled"
    run_test "Thread affinity disabled"
elif [ "$NUMA_NODES" == "1" ] ; then
    log_without_date "  Numa runs: No"
    if [ "$ARCH" == "x86_64" ] ; then
        create_taskset_cpulist_x86_64 $CPUTHREADS
    else
        create_taskset_cpulist_arm64 $CPUTHREADS
    fi
    run_test "NUMA=${NUMA_NODES}"
else
    if [ "$ARCH" == "x86_64" ] ; then
        create_numactl_cpulist_x86_64 $CPUTHREADS
    else
        create_numactl_cpulist_arm64 $CPUTHREADS
    fi
    if [ $USE_REMOTE_MEMNODE == "1" ]; then
        #MEMBIND_COMMAND="-m ${NODE_MEMBIND}"                        # --membind causes allocation to start on the 1st node, then the next so limited to 1 node of DDR bandwidth.
        #MEMBIND_COMMAND_TEXT="m${NODE_MEMBIND}"
        MEMBIND_COMMAND="-i ${NODE_MEMBIND}"                        # --interleave does round robin allocation between the nodes giving higher DDR bandwidth for multiple nodes.
        MEMBIND_COMMAND_TEXT="i${NODE_MEMBIND}"
    else
        MEMBIND_COMMAND="--localalloc"                                        # localalloc allocates in same node as the process or thread calling a malloc() function
        MEMBIND_COMMAND_TEXT="localalloc"
    fi
    log_without_date "  Numa runs: Yes"
    log_without_date "  Numa nodes: $MEMBIND_LIST"
    log_without_date "  Remote Memory: USE_REMOTE_MEMNODE=$USE_REMOTE_MEMNODE, MEMBIND_COMMAND=$MEMBIND_COMMAND"
    log_without_date "  Socket eval: Testing cores from $SOCKET_EVAL out of $socket_num sockets"
    run_test "NUMA=${NUMA_NODES}"
fi

parse $FILENAME
finish_time=$(date +'%s')
log_without_date
log "Total eval runtime = $((($finish_time-$start_time) / 3600)) hrs.. $(((($finish_time-$start_time) % 3600) / 60)) mins.. $((($finish_time-$start_time) % 60)) secs.."
log_without_date
exit
