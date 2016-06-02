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
#

# FIXME Adjust to your needs ...
sender=tul2

function usage() {
    echo "netcat_test.sh [-sender|-receiver]"
}

if [ $1 = -receiver ]; then
    echo "Tidy up ..."
    rm -rf linux_from_tul2.*

    echo -n " (1) Receive tar ...                         "
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.tar) 2> output.stderr
    duration=`grep real output.stderr | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
    sleep 5

    echo -n " (2) Receive sw.tar.gz ...                   "
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.sw.tar.gz) 2> output.stderr
    duration=`grep real output.stderr | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
    sleep 5

    echo -n " (3) Receive hw.tar.gz ...                   "
    (time nc -w 10 ${sender} 7878 > linux_from_tul2.hw.tar.gz) 2> output.stderr
    duration=`grep real output.stderr | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
    sleep 5

    echo -n " (4) Receive and extract hw.tar.gz in hw ... "
    mkdir -p linux_from_tul2.hw
    (time nc -w 10 ${sender} 7878 | \
	PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar xz -C linux_from_tul2.hw \
	--strip-components=1) 2> output.stderr
    duration=`grep real output.stderr | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
    sleep 5

    echo -n " (5) Receive and extract hw.tar.gz in sw ... "
    mkdir -p linux_from_tul2.sw
    (time nc -w 10 ${sender} 7878 | \
	tar xz -C linux_from_tul2.sw \
	--strip-components=1) 2> output.stderr
    duration=`grep real output.stderr | perl -e '$a=<STDIN>; $a=~m/([0-9]*)m([0-9]*)\.([0-9]*)/; $sec=$1*60+$2; $msec=$3; print "$sec,$msec"'`
    echo "; ${duration}"
    sleep 5

    exit 0
fi

if [ $1 = -sender ]; then
    echo " (1) Send tar ..."
    time tar c linux | nc -q 1 -l -p 7878

    echo " (2) Send sw.tar.gz ..."
    time tar cz linux | nc -q 1 -l -p 7878

    echo " (3) Send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    echo " (4) Send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    echo " (5) Send hw.tar.gz ..."
    time PATH=/usr/bin/genwqe:$PATH ZLIB_TRACE=0x0 ZLIB_ACCELERATOR=CAPI \
	tar cz linux | nc -q 1 -l -p 7878

    exit 0
fi

usage
