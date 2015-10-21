#!/bin/bash

#
# Copyright 2015, International Business Machines
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
# Test-script to measure and tune performance of zlib soft- or hardware
# implementation. Use the data to figure out the #threads required to
# get best throughput and when adding more threads does not help.
#
# For the hardware implementation, it will show how many threads in parallel
# are needed to saturate the hardware.
#
# The buffersize test shows the influence of buffering and small buffers
# on throughput. Hardware implemenation will normally work best with large
# buffers.
#

export ZLIB_ACCELERATOR=GENWQE
export ZLIB_CARD=0
export ZLIB_DEFLATE_IMPL=0x01 # Use hardware by default
export ZLIB_INFLATE_IMPL=0x01
export PATH=/opt/genwqe/bin/genwqe:/sbin:/usr/sbin:$PATH

version="https://github.com/ibm-genwqe/genwqe-user"
verbose=""
test_data="/tmp/test_data.bin"

# Print usage message helper function
function usage() {
    echo "Usage of $PROGRAM:"
    echo "    [-A] <accelerator> use either GENWQE for the PCIe and CAPI for"
    echo "         CAPI based solution available only on System p"
    echo "         Use SW to use software compress/decompression"
    echo "    [-C] <card> set the compression card to use (0, 1, ... )."
    echo "          RED (or -1) drive work to all available cards."
    echo "    [-t] <test_data.bin>"
    echo "    [-v] Print status and informational output."
    echo "    [-V] Print program version (${version})"
    echo "    [-h] Print this help message."
    echo
    echo "Input data is to be placed in ${test_data}."
    echo "If it does not exist, the script will generate random example data."
    echo "Using random data will cause performance to suffer, since it"
    echo "will not compress nicely. So using something more realistic is"
    echo "certainly a good idea."
    echo
    echo "Note that the path needs to be setup to find the zlib_mt_perf tool."
    echo
    echo "E.g. run as follows:"
    echo "  Use GenWQE accelerator card 0:"
    echo "    PATH=tools:\$PATH tools/zlib_mt_perf.sh -A GENWQE -C0"
    echo
    echo "  Use CAPI accelerator card 0:"
    echo "    PATH=tools:\$PATH tools/zlib_mt_perf.sh -A CAPI -C0"
    echo
    echo "  Use software zlib:"
    echo "    PATH=tools:\$PATH tools/zlib_mt_perf.sh -A SW"
    echo
}

# Parse any options given on the command line
while getopts "A:C:t:vVh" opt; do
    case ${opt} in
	A)
	    ZLIB_ACCELERATOR=${OPTARG};
            ;;
        C)
            ZLIB_CARD=${OPTARG};
            ;;
	t)
	    test_data=${OPTARG};
	    ;;
        v)
            verbose="-v";
            ;;
        V)
            echo "${version}"
            exit 0;
            ;;
        h)
            usage;
            exit 0;
            ;;
        \?)
            echo "ERROR: Invalid option: -$OPTARG" >&2
            exit 1;
            ;;
    esac
done

if [ $ZLIB_ACCELERATOR = "SW" ]; then
    export ZLIB_DEFLATE_IMPL=0x00;
    export ZLIB_INFLATE_IMPL=0x00;
fi

# Random data cannot being compressed. Performance values might be poor.
# Text data e.g. logfiles work pretty well. Use those if available.
if [ ! -f ${test_data} ]; then
    dd if=/dev/urandom of=${test_data} count=1024 bs=4096
fi
if [ ! -f ${test_data}.gz ]; then
    gzip -f -c ${test_data} > ${test_data}.gz
fi

cpus=`cat /proc/cpuinfo | grep processor | wc -l`
bufsize=1MiB
count=1

echo
uname -a
echo "Accelerator:     ${ZLIB_ACCELERATOR}"
echo "Processors:      $cpus"
echo -n "Raw data:        "
du -h ${test_data}
echo -n "Compressed data: "
du -h ${test_data}.gz

echo "IBM Processing accelerators:"
lspci | grep "Processing accelerators: IBM"

echo
echo "DEFLATE Figure out maximum throughput and #threads which work best"
print_hdr=""
for t in 1 2 3 4 8 16 32 64 128 160 ; do
    zlib_mt_perf $verbose -i$bufsize -o$bufsize -D -f ${test_data} \
	-c$count -t$t $print_hdr;
	if [ $? -ne 0 ]; then
            echo "failed with $t Threads"
            exit 1
        fi
    # sleep 1 ;
    print_hdr="-N";
done

echo
echo "DEFLATE Use optimal #threads, guessing $cpus, influence of buffer size"
print_hdr=""
t=$cpus # FIXME ;-)
for b in 1KiB 4KiB 64KiB 128KiB 1MiB 4MiB 8MiB ; do
    zlib_mt_perf $verbose -i$b -o$b -D -f ${test_data} -c$count -t$t \
	$print_hdr;
    # sleep 1 ;
    print_hdr="-N";
done

echo
echo "INFLATE Figure out maximum throughput and #threads which work best"
print_hdr=""
for t in 1 2 3 4 8 16 32 64 128 160 ; do
    zlib_mt_perf $verbose -i$bufsize -o$bufsize -f ${test_data}.gz \
	-c$count -t$t $print_hdr;
    # sleep 1 ;
    print_hdr="-N";
done

echo
echo "INFLATE Use optimal #threads, guessing $cpus, influence of buffer size"
t=$cpus # FIXME ;-)
print_hdr=""
for b in 1KiB 4KiB 64KiB 128KiB 1MiB 4MiB 8MiB ; do
    zlib_mt_perf $verbose -i$b -o$b -f ${test_data}.gz -c$count -t$t \
	$print_hdr;
    # sleep 1 ;
    print_hdr="-N";
done

exit 0

