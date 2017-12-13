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
tokudb=0
TOKUDB_DIR=PerconaFT
git=0

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
    echo "    [-C <card>]          card to be used for the test"
    echo "    [-T]                 enable tokudb test"
    echo "      [-B <tokudb_dir]   tokudb directory"
    echo "    [-G]                 enable git test"
    echo "    [-t <trace_level>]"
}

while getopts "A:C:TB:Gt:h" opt; do
    case $opt in
	A)
	export ZLIB_ACCELERATOR=$OPTARG;
	;;
	C)
	card=$OPTARG;
	;;
	G)
	git=1
	;;
	T)
	tokudb=1
	;;
	B)
	tokudb=1
	TOKUDB_DIR=$OPTARG;
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

# Turn accelerator off
if [ "$ZLIB_ACCELERATOR" == "SW" ]; then
        export ZLIB_DEFLATE_IMPL=0x0
        export ZLIB_INFLATE_IMPL=0x0
fi

rm -f $ZLIB_LOGFILE
ulimit -c unlimited

if [ ! -f `pwd`/lib/libzADC.so ]; then
	echo "err: Please build the code prior executing this."
	exit -1
fi

function git_test ()
{
	echo -n "Trying out git log ... "
	LD_PRELOAD=`pwd`/lib/libzADC.so git log > git.log
	if [ $? -ne 0 ]; then
		echo "err: git log failed!"
		exit 1
	fi
	echo "OK"
}

# FIXME Add more stuff here to ensure that it fully works ...
function tokudb_test ()
{
	echo "Trying to test TokuDB compression with GenWQE..."
	if [ -f $TOKUDB_DIR/build/ft/tests/compress-test ]; then
		echo "Percona compression test..."
		LD_PRELOAD=`pwd`/lib/libzADC.so $TOKUDB_DIR/build/ft/tests/compress-test > tokudb.log 2>&1
		if [ $? -ne 0 ]; then
			echo "err: compression test failed."
			exit 1
		fi
		echo "OK"
	else
		## Please compile the Percona using PerconaBuild.sh. And then try the test again.
		echo "compress-test not exists in $TOKUDB_DIR/build/ft/tests. Please check the environment."
		exit 1
	fi

	if [ -f $TOKUDB_DIR/build/ft/tests/subblock-test-compression ]; then
		echo "Percona subblock compression test..."
		LD_PRELOAD=`pwd`/lib/libzADC.so $TOKUDB_DIR/build/ft/tests/subblock-test-compression > tokudb.log 2>&1
		if [ $? -ne 0 ]; then
			echo "subblock compression test failed."
			exit 1
		fi
		echo "OK"
	else 
		echo "subblock-test-compression not exist in $TOKUDB_DIR/build/ft/tests. Please check the environment."
		exit 1
	fi

	if [ -f $TOKUDB_DIR/build/ft/tests/ft-serialize-benchmark ]; then
		echo "Percona ft serialize benchmark..."
		$TOKUDB_DIR/build/ft/tests/ft-serialize-benchmark 10000 5000 50 50 > tokudb.log 2>&1
		SW_SE=`grep "^serialize leaf" tokudb.log | awk  {'print $3'}`
		SW_DE=`grep "^deserialize leaf" tokudb.log | awk  {'print $3'}`

		LD_PRELOAD=`pwd`/lib/libzADC.so $TOKUDB_DIR/build/ft/tests/ft-serialize-benchmark 10000 5000 50 50 > tokudb.log 2>&1
		if [ $? -ne 0 ]; then
        	echo "ft serialize benchmark test failed."
        	exit 1
		fi
		HW_SE=`grep "^serialize leaf" tokudb.log | awk  {'print $3'}`
		HW_DE=`grep "^deserialize leaf" tokudb.log | awk  {'print $3'}`

		echo "serialize leaf:   SW($SW_SE ms)  -  HW($HW_SE ms)"
		echo "deserialize leaf: SW($SW_DE ms)  -  HW($HW_DE ms)"
		echo "OK"
	else
		echo "ft-serialize-benchmark not exist in $TOKUDB_DIR/build/ft/tests. Please check the environment."
		exit 1
    fi
}

if [ $tokudb -ne 0 ]; then
	tokudb_test
fi
if [ $git -ne 0 ]; then
	git_test
fi

exit 0
