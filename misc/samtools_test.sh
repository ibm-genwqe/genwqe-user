#!/bin/bash

#
# Copyright 2016, International Business Machines
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

# Frank Haverkamp <haver@linux.vnet.ibm.com>
#
# Trying out samtools in a way where, we can repro what we did so far.
# Let's see what is possible and what is not. We need some example BAM/SAM
# files to get this test working. Our current set of example files were
# contributed by a colleague. Thanks for providing them.
#
# /usr/bin/time did not want to accept environment variables before the
# command. We are using build in time and hope that the -p switch is
# supported to print the time in seconds, such that we can copy-paste
# the resulting output into a csv file which we use to generate some
# charts from.
#
# Usage example:
#
#   for a in CAPI GENWQE SW ; do
#       ./misc/samtools_test.sh -A${a} -C0 -d /home/haver/genomics ;
#   done
#

export ZLIB_TRACE=0x0000
export ZLIB_ACCELERATOR=GENWQE
export ZLIB_CARD=0
export ZLIB_INFLATE_IMPL=0x61
export   ZLIB_INFLATE_THRESHOLD=4KiB
export ZLIB_DEFLATE_IMPL=0x61
export PATH=/usr/bin/genwqe/bin/genwqe:/sbin:/usr/sbin:$PATH

## FIXME Files and directory are not obvious ...
verbose=0
directory=/home/${USER}/genomics
threads=`cat /proc/cpuinfo  | grep processor | wc -l`
homedir=`pwd`
sadc=/usr/lib/sysstat/sadc

## FIXME Location might differ for various Linux distributions
libz=/usr/lib64/genwqe/libz.so.1

# Measurement variables
huge=0;

function usage() {
    echo "Usage of $PROGRAM:"
    echo "    [-A] <accelerator> use either GENWQE for the PCIe and CAPI for"
    echo "         CAPI based solution available only on System p"
    echo "         Use SW to use software compress/decompression"
    echo "    [-C] <card> set the compression card to use (0, 1, ... )."
    echo "          RED (or -1) drive work to all available cards."
    echo "    [-v <verbose>]"
    echo "    [-t <trace_level>]"
    echo "    [-T <threads>]"
    echo "    [-C <card>]       Card to be used for the test (-1 autoselect)"
    echo "    [-d <directory>]  Directory containing the data"
    echo "    [-D]              Try all hw optimiaztions, if not set"
    echo "                      only the most promising config is tried."
    echo "    [-H]              Try with huge data, this takes a while ..."
    echo "    [-h]              Help."
    echo
}

while getopts "A:C:T:Hd:t:v:h" opt; do
    case $opt in
	A)
	export ZLIB_ACCELERATOR=$OPTARG;
	;;
        C)
	export ZLIB_CARD=$OPTARG;
        ;;
	d)
	directory=$OPTARG;
	;;
	T)
	threads=$OPTARG;
	;;
        t)
	export ZLIB_TRACE=$OPTARG;
        ;;
	H)
	huge=1;
	;;
        v)
        verbose=$OPTARG;
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

if [ $ZLIB_ACCELERATOR = "SW" ]; then
    export ZLIB_DEFLATE_IMPL=0x00;
    export ZLIB_INFLATE_IMPL=0x00;
fi

function p8_dma_sanity_check() {
    dmesg | grep dma | grep '\-1'
    if [ $? -ne 1 ]; then
	echo "WARNING: Check P8 DMA settings to get full PCIe performance!"
	echo "  If we see DMA configuration problems with the KVM guest"
	echo "  the performance will be way worse than it could be and the"
	echo "  measurements will not be usable at the end."
    fi
}

function indexing() {
    local file=$1

    touch sam.log
    rm -f ${file}.${ZLIB_ACCELERATOR}.bai
    (time -p LD_PRELOAD=${libz} samtools index ${file}) 2>> sam.log
    tail -n3 sam.log > time.log
    mv ${file}.bai ${file}.${ZLIB_ACCELERATOR}.bai
    sw=`grep real time.log | cut -d' ' -f2`;
    echo "   ; $sw"
}

function viewing() {
    local file=$1
    local t=$2

    touch sam.log
    rm -f ${file}.${ZLIB_ACCELERATOR}.bam
    sync
    (time -p LD_PRELOAD=${libz} \
	samtools view -@ ${t} -S -b ${file} > na1.${ZLIB_ACCELERATOR}.bam) \
	2>> sam.log
    tail -n3 sam.log > time.log
    sw=`grep real time.log | cut -d' ' -f2`;
    echo "  $t ; $sw"
}

function bam2fq() {
    local file=$1
    local t=$2

    touch sam.log
    rm -f ${file}.${ZLIB_ACCELERATOR}.fastq
    sync
    (time -p LD_PRELOAD=${libz} \
	samtools bam2fq ${file} > ${file}.${ZLIB_ACCELERATOR}.fastq) \
	2>> sam.log
    tail -n3 sam.log > time.log
    sw=`grep real time.log | cut -d' ' -f2`;
    echo "  $t ; $sw"
}

function sam_sort() {
    local file=$1
    local t=$2

    touch sam.log
    rm -f ${file}.${ZLIB_ACCELERATOR}.sorted.bam ${file}.tmp
    sync

    if [ $verbose = 1 ]; then
	echo "EXECUTE:"
	echo "  export ZLIB_TRACE=$ZLIB_TRACE"
	echo "  export ZLIB_ACCELERATOR=$ZLIB_ACCELERATOR"
	echo "  export ZLIB_CARD=$ZLIB_CARD"
	echo "  export ZLIB_DEFLATE_IMPL=$ZLIB_DEFLATE_IMPL"
	echo "  export ZLIB_INFLATE_IMPL=$ZLIB_INFLATE_IMPL"
	echo "  LD_PRELOAD=${libz} \\"
	echo "    samtools sort -@${t} -T ${file}.tmp -O bam \\"
	echo "      -o ${file}.${ZLIB_ACCELERATOR}.sorted.bam ${file}"
    fi

    (time -p LD_PRELOAD=${libz} \
	samtools sort -@${t} -T ${file}.tmp -O bam \
	-o ${file}.${ZLIB_ACCELERATOR}.sorted.bam \
	${file}) 2>> sam.log
    tail -n3 sam.log > time.log
    sw=`grep real time.log | cut -d' ' -f2`;
    echo "  $t ; $sw"
}

###############################################################################
# Preparations
###############################################################################

ulimit -c unlimited
pushd .
cd $directory

cpus=`cat /proc/cpuinfo | grep processor | wc -l`
echo
uname -a
echo "Accelerator:    ${ZLIB_ACCELERATOR}"
echo "Processors:     $cpus"
echo "Threads to try: $threads"
echo "Inflate sw fallback: $ZLIB_INFLATE_THRESHOLD"
echo "Available IBM Processing accelerators:"
lspci | grep "Processing accelerators: IBM"
echo

echo "Check availability of test data and libraries:"
for d in NA1.bam na1.sam NA12878.bam ${libz} ; do
    echo -n "  ${d} ... "
    if [ ! -f ${d} ]; then
	echo "MISSING!"
	pwd
	exit 1
    else
	echo "OK"
    fi
done
echo

p8_dma_sanity_check

###############################################################################
# System Load Logging
###############################################################################

function system_load_find_sadc() {
    if [ -x /usr/lib64/sa/sadc ]; then
	sadc=/usr/lib64/sa/sadc
    elif [ -x /usr/lib/sysstat/sadc ]; then
	sadc=/usr/lib/sysstat/sadc
    else
	echo "Cannot find sadc tool for CPU load measurement!"
	exit 1
    fi
}

function system_load_logging_start() {
    rm -f system_load.sar system_load.pid
    /usr/lib/sysstat/sadc 1 system_load.sar &
    echo $! > system_load.pid
}

function system_load_logging_stop() {
    kill -9 `cat system_load.pid`

    # Skip the 1st 4 lines, since they container some header information
    cp system_load.sar system_load.$ZLIB_ACCELERATOR.sar
    LC_TIME=posix sar -u -f system_load.sar | tail -n +4 > system_load.txt
    grep -v Average system_load.txt > system_load.csv
    LC_TIME=posix sar -u -f system_load.sar > system_load.$ZLIB_ACCELERATOR.csv

    start=`head -n1 system_load.csv | cut -f1 -d' '`
    end=`tail -n1 system_load.csv | cut -f1 -d' '`

    cat <<EOF > system_load.gnuplot
set terminal pdf size 16,8
set output "system_load.pdf"
set autoscale
set title "System Load using $ZLIB_ACCELERATOR"
set xdata time
set timefmt "%H:%M:%S"
set xlabel "Time"
set xrange ["$start":"$end"]
set ylabel "CPU Utilization"
set yrange ["0.00":"35.00"]
set style data lines
set grid
# set datafile separator " "
plot "system_load.csv" using 1:3 title "%user" with lines lw 4, '' using 1:5 title "%system" with lines lw 4
EOF

    # Instructing gnuplot to generate a png with out CPU load statistics
    cat system_load.gnuplot | gnuplot

    # Safe it under an accelerator unique name
    mv system_load.pdf system_load.${ZLIB_ACCELERATOR}.pdf
}

system_load_find_sadc
system_load_logging_start

###############################################################################
echo "SAMTOOLS sort (inflate/deflate)"
echo " threads ; ${ZLIB_ACCELERATOR}"
sam_sort NA1.bam 0
for ((t = 1; t <= $threads; t *= 2)); do
    sam_sort NA1.bam ${t}
done
echo

###############################################################################
echo "SAMTOOLS bam2fq (inflate)"
echo "         ; ${ZLIB_ACCELERATOR}"
bam2fq NA1.bam
echo

###############################################################################
echo "SAMTOOLS indexing (inflate)"
echo " threads ; ${ZLIB_ACCELERATOR}"
indexing NA1.bam
if [ $huge -eq 1 ]; then
    indexing NA12878.bam
fi
echo

###############################################################################
echo "SAMTOOLS viewing (deflate)"
echo " threads ; ${ZLIB_ACCELERATOR}"
viewing na1.sam 0
for ((t = 1; t <= $threads; t *= 2)); do
    viewing na1.sam ${t}
done
echo

###############################################################################
# Gather CPU Load Statistics
###############################################################################

system_load_logging_stop

popd
