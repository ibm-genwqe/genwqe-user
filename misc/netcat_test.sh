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

#
# Test-script to measure and tune performance of zlib soft- or hardware
# implementation. In combination with netcat and tar/gzip.
# Unless being executed in a tmpfs/ramdisk this test also measure I/O
# to read and write data.
#

# FIXME Adjust to your needs ...
sender=tul2

function extract_real_time() {
    local file=$1;

    duration=`grep real $file | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
}

function usage() {
    echo "netcat_test.sh [-sender|-receiver]"
}

if [ $1 = -receiver ]; then
    echo "Tidy up ..."
    rm -rf linux_from_tul2.*

    echo -n " (1) Receive plain tar ...                   "
    sync
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.plain.tar) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (2) Receive plain hw.tar.gz ...             "
    sync
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.plain.hw.tar.gz) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (3) Receive plain sw.tar.gz ...             "
    sync
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.plain.sw.tar.gz) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (4) Receive generated tar ...               "
    sync
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.generated.tar) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (5) Receive tar and extract it ...          "
    mkdir -p linux_from_tul2
    sync
    (time nc -w 10 ${sender} 7878 | \
	tar x -C linux_from_tul2 --strip-components=1) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (6) Receive sw.tar.gz ...                   "
    sync
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.generated.sw.tar.gz) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (7) Receive hw.tar.gz ...                   "
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.generated.hw.tar.gz) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (8) Receive and extract hw.tar.gz in hw ... "
    mkdir -p linux_from_tul2.hw
    sync
    (time nc -w 10 ${sender} 7878 | \
	PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar xz -C linux_from_tul2.hw \
	--strip-components=1) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    echo -n " (9) Receive and extract hw.tar.gz in sw ... "
    sync
    mkdir -p linux_from_tul2.sw
    (time nc -w 10 ${sender} 7878 | \
	tar xz -C linux_from_tul2.sw \
	--strip-components=1) 2> output.stderr
    extract_real_time output.stderr
    sleep 5

    exit 0
fi

if [ $1 = -sender ]; then
    echo " (1a) Generate tar if it is not exsiting yet ..."

    if [ ! -d linux ]; then
	echo "linux directory missing, needed to perform this measurement!"
	exit 1
    fi
    if [ ! -f linux.tar ]; then
	time tar cf linux.tar linux
    else
	echo "    linux.tar already existing, skipping"
    fi
    du -ch linux.tar
    echo "Please start receiver now."
    
    echo " (1) Send tar ..."
    cat linux.tar | nc -q 1 -l -p 7878

    echo " (2) Send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	time genwqe_gzip -c linux.tar | nc -q 1 -l -p 7878

    echo " (3) Send sw.tar.gz ..."
    time gzip -c linux.tar | nc -q 1 -l -p 7878

    echo " (4) Generate and send tar ..."
    time tar c linux | nc -q 1 -l -p 7878

    echo " (5) Generate and send tar ..."
    time tar c linux | nc -q 1 -l -p 7878

    echo " (6) Generate and send sw.tar.gz ..."
    time tar cz linux | nc -q 1 -l -p 7878

    echo " (7) Generate and send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    echo " (8) Generate and send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    echo " (9) Generate and send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    exit 0
fi

usage
