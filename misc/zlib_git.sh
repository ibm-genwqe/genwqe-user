#!/bin/bash

#
# Copyright 2017 International Business Machines
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
# Some tests to ensure proper function of the hardware accelerated zlib.
#
# Setup tools path, such that we do not need to prefix the binaries and
# test-script. This should also help to reduce change effort when we
# move our test binaries from one to another source code repository.
#

card=0

export ZLIB_ACCELERATOR=GENWQE
export ZLIB_LOGFILE=zlib.log
export ZLIB_TRACE=0x8

export PATH=`pwd`/tools:`pwd`/misc:$PATH
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH

function usage() {
    echo "Usage:"
    echo "  zlib_git.sh"
    echo "    [-A] <accelerator> use either GENWQE for the PCIe and CAPI for"
    echo "         CAPI based solution available only on System p"
    echo "    [-C <card>]        card to be used for the test"
    echo "    [-t <trace_level>]"
}

while getopts "A:C:t:h" opt; do
    case $opt in
	A)
	export ZLIB_ACCELERATOR=$OPTARG;
	;;
	C)
	card=$OPTARG;
	;;
	t)
	export ZLIB_TRACE=$OPTARG;
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

ulimit -c unlimited

if [ ! -f `pwd`/lib/libzADC.so ]; then
	echo "err: Please build the code prior executing this."
	exit -1
fi

echo -n "Trying out git log ... "
LD_PRELOAD=`pwd`/lib/libzADC.so git log > git.log
if [ $? -ne 0 ]; then
	echo "err: git log failed!"
	exit 1
fi
echo "OK"

# FIXME Add more stuff here to ensure that it fully works ...

exit 0
