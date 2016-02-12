#!/bin/bash

#
# Copyright 2015, 2016, International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#
# This testcase proceses significant interrupt stress. Run that for a day
# and you will know if your device driver and software can surive sudden
# abborts while running a lot of interrrupt stress.
#
# Start N echos and kill them after a couple of seconds.
#

accelerator=GENWQE
card=0
tools_dir=genwqe-user/tools
verbose=0
iterations=10
processes=128
killtimeout=4
preload=1
runpids=""

function usage() {
    echo "Usage:"
    echo "  parallel_echo.sh"
    echo "     -A <accelerator> GENWQE|CAPI"
    echo "     -C <card>        card to be used for the test"
    echo "     -i <iterations>  repeat  multiple times for more testing"
    echo "     -p <processes>   how many processed in parallel"
    echo "     -k <seconds>     kill timeout"
    echo "     -l <N>           send <N> echos in one shot N <= 64"
    echo
    echo "Example:"
    echo "  Repro the CAPI bitstream interrupt loss problem:"
    echo "    ./scripts/parallel_echo.sh -ACAPI -C0 -i10 -p32 -k5"
    echo
}

function start_job {
    # echo "Starting: $*"
    echo "$*" > echo_$s.cmd

    exec $* $parms &
    newpid=$!
    # echo "NewPID:   $newpid"
    runpids=$runpids" "$newpid
    # echo "RunPIDs:  $runpids"
}

function stop_jobs {
    echo "Running:   "`jobs -rp`
    echo "Expected: ${runpids}"
    kill -SIGKILL `jobs -rp`
    wait
    echo "Still running: "`jobs -rp`
    runpids=""
}

function cleanup {
    echo "Stopping all jobs ..."
    stop_jobs
    sleep 1
    echo "done"
    exit 0
}
  
trap cleanup SIGINT
trap cleanup SIGKILL
trap cleanup SIGTERM

while getopts "A:C:p:i:k:l:h" opt; do
    case $opt in
	A)
	accelerator=$OPTARG;
	;;
	C)
	card=$OPTARG;
	;;
        i)
        iterations=$OPTARG;
        ;;
        p)
        processes=$OPTARG;
        ;;
	k)
	killtimeout=$OPTARG;
	;;
	l)
	preload=$OPTARG;
	;;
        h)
        usage;
        exit 0;
        ;;
        \?)
        echo "Invalid option: -$OPTARG" >&2
        ;;
    esac
done

function test_echo ()
{
    ### Start in background ...
    echo "Starting genwqe_echo in the background ... "
    for s in `seq 1 $processes` ; do
	start_job $tools_dir/genwqe_echo -A ${accelerator} -C ${card} \
	    -l ${preload} -f > echo_$s.stdout.log 2> echo_$s.stderr.log
    done
    echo "ok"

    echo "Waiting ${killtimeout} seconds ..."
    for s in `seq 0 ${killtimeout}` ; do
	sleep 1;
	echo -n "."
    done
    echo " ok"
	
    echo "Sending SIGKILL to all ... "
    stop_jobs
    echo "ok"
}

echo "Build code ..."
make -s || exit 1

echo "********************************************************************"
echo "Parallel echo TEST for ${accelerator} card ${card} starting ${processes}"
echo "********************************************************************"
echo

echo "Remove old logfiles ..."
rm -f echo_*.cmd echo_*.stdout.log echo_*.stderr.log

for i in `seq 1 ${iterations}` ; do

    echo "Check if card is replying to an echo request ..."
    $tools_dir/genwqe_echo -A ${accelerator} -C ${card} -c5
    if [ $? -ne 0 ]; then
	echo "Single echo took to long, please review results!"
	exit 1
    fi

    test_echo;

    echo "Check logfiles for string \"err\" ..."
    grep err echo_*.stderr.log
    if [ $? -ne 1 ]; then
	echo "Found potential errors ... please check logfiles"
	exit 1
    fi

    $tools_dir/genwqe_echo -A ${accelerator} -C ${card} -c5
    if [ $? -ne 0 ]; then
	echo "Single echo took to long, please review results!"
	exit 1
    fi

    echo "Remove old logfiles ..."
    rm -f echo_*.cmd echo_*.stdout.log echo_*.stderr.log
done

exit 0
