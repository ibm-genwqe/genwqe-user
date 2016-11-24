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
# Some tests to ensure proper function of the hardware accelerated zlib.
#
# Setup tools path, such that we do not need to prefix the binaries and
# test-script. This should also help to reduce change effort when we
# move our test binaries from one to another source code repository.
#

export PATH=`pwd`/tools:`pwd`/misc:$PATH
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH

card=0
verbose=0
trace=0
ibuf_size="1MiB"
export ZLIB_ACCELERATOR=GENWQE;

function usage() {
    echo "Usage:"
    echo "  zlib_test.sh"
    echo "    [-A] <accelerator> use either GENWQE for the PCIe and CAPI for"
    echo "         CAPI based solution available only on System p"
    echo "         Use SW to use software compress/decompression"
    echo "    [-C <card>]        card to be used for the test"
    echo "    [-v <verbose>]"
    echo "    [-t <trace_level>]"
    echo "    [-i <ibuf_size>]"
}

while getopts "A:C:i:t:v:h" opt; do
    case $opt in
	A)
	export ZLIB_ACCELERATOR=$OPTARG;
	;;
	C)
	card=$OPTARG;
	;;
	t)
	trace=$OPTARG;
	;;
	i)
	ibuf_size=$OPTARG;
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

ulimit -c unlimited

function test_compress_decompress() {
    local fname=$1;

    echo "Compress ${fname} ..."
    echo "  zpipe < ${fname} > ${fname}.rfc1950"
    time zpipe < ${fname} > ${fname}.rfc1950
    if [ $? -ne 0 ]; then
	echo "zpipe failed!"
	echo "  zpipe < ${fname} > ${fname}.rfc1950"
	exit 1
    fi
    #od -tx1 ${fname}.rfc1950 | head
    #echo "..."
    #od -tx1 ${fname}.rfc1950 | tail
    echo "ok"

    echo "Check size of resulting file ..."
    du -ch ${fname}.rfc1950
    echo

    echo "Decompress data ..."
    echo "  zpipe -d < ${fname}.rfc1950 > ${fname}.out"
    time zpipe -d < ${fname}.rfc1950 > ${fname}.out
    if [ $? -ne 0 ]; then
	echo "zpipe failed!"
	echo "  zpipe -d < ${fname}.rfc1950 > ${fname}.out"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff ${fname} ${fname}.out &> /dev/null
    if [ $? -ne 0 ]; then
	echo "${fname} and ${fname}.out are different!"
	exit 1
    fi
    echo "ok"
}

function test_compress_decompress_rnd() {
    local fname=$1;
    local bufsize=$2;

    echo "--- bufsize=$bufsize/randomized ------------------------------------"
    echo "Compress ${fname} with random buffer sizes 1..$bufsize"
    time zpipe_rnd -i$bufsize -o$bufsize -r \
	< ${fname} > ${fname}.rfc1950
    if [ $? -ne 0 ]; then
	echo "zpipe_rnd failed!"
	echo "  zpipe_rnd -i$bufsize -o$bufsize -r < ${fname} > ${fname}.rfc1950"
	exit 1
    fi
    #od -tx1 ${fname}.rfc1950 | head
    echo "ok"

    echo "Check size of resulting file ..."
    du -ch ${fname}.rfc1950
    echo

    echo "Decompress data ..."
    time zpipe_rnd -i$bufsize -o$bufsize -r -d \
	< ${fname}.rfc1950 > ${fname}.out
    if [ $? -ne 0 ]; then
	echo "zpipe_rnd failed! in=${fname}.rfc1950 out=${fname}.out"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff ${fname} ${fname}.out &> /dev/null
    if [ $? -ne 0 ]; then
	echo "${fname} and ${fname}.out are different!"
	exit 1
    fi
    echo "ok"
}

function test_compress_decompress_fixed() {
    local fname=$1;
    local bufsize=$2;

    echo "--- bufsize=$bufsize/fixed -----------------------------------------"
    echo "Compress ${fname} with fixed buffer size $bufsize"
    time zpipe_rnd -i$bufsize -o$bufsize \
	< ${fname} > ${fname}.rfc1950
    if [ $? -ne 0 ]; then
	echo "zpipe_rnd failed! in=${fname}.out out=${fname}.rfc1950 "
	exit 1
    fi
    echo "ok"

    echo "Check size of resulting file ..."
    du -ch ${fname}.rfc1950
    echo

    echo "Decompress data ..."
    time zpipe_rnd -i$bufsize -o$bufsize -d \
	< ${fname}.rfc1950 > ${fname}.out
    if [ $? -ne 0 ]; then
	echo "zpipe_rnd failed! in=${fname}.rfc1950 out=${fname}.out"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff ${fname} ${fname}.out &> /dev/null
    if [ $? -ne 0 ]; then
	echo "${fname} and ${fname}.out are different!"
	exit 1
    fi
    echo "ok"
}

function build_code ()
{
    echo "--------------------------------------------------------------------"
    echo "Build code ..."
    make || exit 1

    echo "--------------------------------------------------------------------"
    if [ -f test_data.bin ]; then
	echo "test_data.bin is already existing, continue ..."
    else
	echo "Copy test data ..."
	cat /usr/bin* /usr/lib/* > test_data.bin 2> /dev/null
    fi
    du -ch test_data.bin

    if [ -f empty.bin ]; then
	echo "empty.bin is already existing, continue ..."
    else
	touch empty.bin
    fi
    du -ch empty.bin
}

function zlib_software () {
    echo "--------------------------------------------------------------------"
    echo "- SOFTWARE ---------------------------------------------------------"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=0
    export ZLIB_DEFLATE_IMPL=0
    export ZLIB_TRACE=${trace}

    echo "Use SW libz zipe with standard 16KiB buffers"
    env | grep ZLIB

    test_compress_decompress test_data.bin
    mv test_data.bin.rfc1950 test_data.bin.rfc1950.zlib

    test_compress_decompress empty.bin
    mv empty.bin.rfc1950 empty.bin.rfc1950.zlib
}

function zlib_hardware_no_buffering ()
{
    echo "--------------------------------------------------------------------"
    echo "- HARDWARE without buffering ---------------------------------------"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_TRACE=${trace}
    export ZLIB_CARD=${card}
    export ZLIB_VERBOSE=${verbose}

    echo "--- zpipe with 16KiB in- and output buffers ------------------------"
    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    env | grep ZLIB

    echo "Use HW libz using card ${card} ..."
    echo "  CARD:       ${ZLIB_CARD}"
    echo "  IBUF_TOTAL: ${ZLIB_IBUF_TOTAL}"
    echo "  OBUF_TOTAL: ${ZLIB_OBUF_TOTAL}"

    test_compress_decompress test_data.bin
    mv test_data.bin.rfc1950 test_data.bin.rfc1950.genwqe

    test_compress_decompress empty.bin
    mv empty.bin.rfc1950 empty.bin.rfc1950.genwqe
}

function zlib_hardware_buffering ()
{
    echo "--------------------------------------------------------------------"
    echo "- HARDWARE with buffering ------------------------------------------"
    echo "--------------------------------------------------------------------"

    echo "--- zpipe with 16KiB in- and output buffers ------------------------"

    export ZLIB_IBUF_TOTAL=${ibuf_size}
    export ZLIB_OBUF_TOTAL=${ibuf_size}

    echo "Use HW libz using card ${card} ..."
    echo "  CARD:       ${ZLIB_CARD}"
    echo "  IBUF_TOTAL: ${ZLIB_IBUF_TOTAL}"
    echo "  OBUF_TOTAL: ${ZLIB_OBUF_TOTAL}"

    test_compress_decompress test_data.bin
    mv test_data.bin.rfc1950 test_data.bin.rfc1950.genwqe

    echo "--- zpipe_rnd with buffer size variations --------------------------"

    bufsizes="1023 4095 128KiB 256KiB 4MiB 7MiB 16MiB";

    for bufsize in $bufsizes ; do
	test_compress_decompress_rnd test_data.bin $bufsize
	mv test_data.bin.rfc1950 test_data.bin.rfc1950.genwqe
    done

    for bufsize in $bufsizes ; do
	test_compress_decompress_fixed test_data.bin $bufsize
	mv test_data.bin.rfc1950 test_data.bin.rfc1950.genwqe
    done

    echo "--------------------------------------------------------------------"
    echo "- HARDWARE without buffering but using large buffers ---------------"
    echo "--------------------------------------------------------------------"

    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    echo "Use HW libz using card ${card} ..."
    echo "  CARD:       ${ZLIB_CARD}"
    echo "  IBUF_TOTAL: ${ZLIB_IBUF_TOTAL}"
    echo "  OBUF_TOTAL: ${ZLIB_OBUF_TOTAL}"

    for bufsize in 256KiB 512KiB 1MiB 2MiB 4MiB ; do
	test_compress_decompress_fixed test_data.bin $bufsize
	mv test_data.bin.rfc1950 test_data.bin.rfc1950.genwqe
    done

    export ZLIB_IBUF_TOTAL=${ibuf_size}
    export ZLIB_OBUF_TOTAL=${ibuf_size}

    echo "--------------------------------------------------------------------"
    echo "Test: Decompress SW compressed data with HW $ibuf_size buffers ..."
    echo "--------------------------------------------------------------------"

    time zpipe_rnd -s1MiB -d \
	< test_data.bin.rfc1950.zlib > test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "zpipe failed!"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff test_data.bin test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "test_data.bin and test_data.bin.out are different!"
	exit 1
    fi
    echo "ok"

    echo "--------------------------------------------------------------------"
    echo "Test: Decompress SW data + padding using $ibuf_size buffers ..."
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=${ibuf_size}
    export ZLIB_OBUF_TOTAL=0 # We must not buffer for inflate for this test

    dd if=/dev/urandom bs=1 count=100 of=padding.bin
    cat test_data.bin.rfc1950.zlib padding.bin > \
	test_data.bin.rfc1950.padded.zlib

    time zpipe_rnd -s1MiB -d \
	< test_data.bin.rfc1950.padded.zlib > test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "zpipe failed!"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff test_data.bin test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "test_data.bin and test_data.bin.out are different!"
	exit 1
    fi
    echo "ok"

    echo "--------------------------------------------------------------------"
    echo "Test: Decompress SW data + padding using $ibuf_size buffers (fully)"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=${ibuf_size}
    export ZLIB_OBUF_TOTAL=${ibuf_size}

    dd if=/dev/urandom bs=1 count=100 of=padding.bin
    cat test_data.bin.rfc1950.zlib padding.bin > \
	test_data.bin.rfc1950.padded.zlib

    time zpipe_rnd -s1MiB -d \
	< test_data.bin.rfc1950.padded.zlib > test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "zpipe failed!"
	exit 1
    fi
    echo "ok"

    echo "Compare data ..."
    diff test_data.bin test_data.bin.out
    if [ $? -ne 0 ]; then
	echo "test_data.bin and test_data.bin.out are different!"
	exit 1
    fi
    echo "ok"
}

function zlib_append ()
{
    local flush=$1
    local params=$2

    # Use default settings ...
    # Set size large enough that hardware inflate is realy used
    #
    # hhh [0x3ffff1c655d8] loops=0 flush=1 Z_PARTIAL_FLUSH
    # hhh [0x3ffff1c655d8] *** giving out 100 bytes ...
    # hhh Accumulated input data:
    #  00000000: 54 68 69 73 20 69 73 20 74 68 65 20 45 4e 44 21 | This.is.the.END.
    #
    # hhh     d=2
    # hhh     d=50, 0 is goodness
    # hhh [0x3ffff1c655d8]            flush=1 Z_PARTIAL_FLUSH avail_in=16 avail_out=0
    unset ZLIB_INFLATE_IMPL
    unset ZLIB_DEFLATE_IMPL
    unset ZLIB_IBUF_TOTAL
    unset ZLIB_OBUF_TOTAL

    echo "Special zpipe_append setup, which failed once ... "
    echo -n "  zpipe_append -FZLIB -fZ_PARTIAL_FLUSH -i2MiB -o4KiB -s256KiB -p122846 -t122846 "
    zpipe_append -FZLIB -fZ_PARTIAL_FLUSH -i2MiB -o4KiB -s256KiB -p122846
    if [ $? -ne 0 ]; then
	echo "failed"
	exit 1
    fi
    echo "ok"

    for f in ZLIB DEFLATE GZIP ; do
	for ibuf in 2MiB 1MiB 128KiB 4KiB 1000 100 ; do
	    for obuf in 1MiB 128KiB 4KiB 1000 100 ; do
		echo -n "zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} -s256KiB -e -E ${params} "
		zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} -s256KiB -e -E ${params}
		if [ $? -ne 0 ]; then
		    echo "failed"
		    exit 1
		fi
		echo "ok"
	    done
	done
    done
    export ZLIB_INFLATE_IMPL=0x41
    export ZLIB_DEFLATE_IMPL=0x41
    #unset ZLIB_INFLATE_IMPL
    #unset ZLIB_DEFLATE_IMPL
    unset ZLIB_IBUF_TOTAL
    unset ZLIB_OBUF_TOTAL

    env | grep ZLIB
    for f in ZLIB DEFLATE GZIP ; do
	for ibuf in 2MiB 1MiB 128KiB 4KiB 1000 100 ; do
	    for obuf in 1MiB 128KiB 4KiB 1000 100 ; do
		# echo "Append feature: format=${f} ib=${ibuf} ob=${obuf} ... "
		echo -n "zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} -s256KiB ${params} "
		zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} -s256KiB ${params}
		if [ $? -ne 0 ]; then
		    echo "failed"
		    exit 1
		fi
		echo "ok"
	    done
	done
    done

    echo "--------------------------------------------------------------------"
    echo "zpipe_append: HW compression/decompression without buffering"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    env | grep ZLIB
    for f in ZLIB DEFLATE GZIP ; do
	for ibuf in 2MiB 1MiB 128KiB 4KiB 1000 100 ; do
	    for obuf in 1MiB 128KiB 4KiB 1000 100 ; do
	    # echo "Append feature: format=${f} ib=${ibuf} ob=${obuf} ... "
		echo -n "zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params} "
		zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params}
		if [ $? -ne 0 ]; then
		    echo "failed"
		    exit 1
		fi
		echo "ok"
	    done
	done
    done

    echo "--------------------------------------------------------------------"
    echo "zpipe_append: HW compression/decompression with buffering"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=1MiB
    export ZLIB_OBUF_TOTAL=0 # known to fail

    env | grep ZLIB
    for f in ZLIB DEFLATE GZIP ; do
	for ibuf in 2MiB 1MiB 128KiB 4KiB 1000 100 ; do
	    for obuf in 1MiB 128KiB 4KiB 1000 100 ; do
	    # echo "Append feature: format=${f} ib=${ibuf} ob=${obuf} ... "
		echo -n "zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params} "
		zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params}
		if [ $? -ne 0 ]; then
		    echo "failed"
		    exit 1
		fi
		echo "ok"
	    done
	done
    done

    echo "--------------------------------------------------------------------"
    echo "zpipe_append: HW compression/decompression with buffering obuf=1MiB"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=1MiB
    export ZLIB_OBUF_TOTAL=1MiB

    env | grep ZLIB
    for f in ZLIB DEFLATE GZIP ; do
	for ibuf in 2MiB 1MiB 128KiB 4KiB 1000 100 ; do
	    for obuf in 1MiB 128KiB 4KiB 1000 100 ; do
	    # echo "Append feature: format=${f} ib=${ibuf} ob=${obuf} ... "
		echo -n "zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params} "
		zpipe_append -F${f} -f${flush} -i${ibuf} -o${obuf} ${params}
		if [ $? -ne 0 ]; then
		    echo "failed"
		    exit 1
		fi
		echo "ok"
	    done
	done
    done
}

function multithreading_quick ()
{
    echo "--------------------------------------------------------------------"
    echo "zpipe_mt: HW compression/decompression without buffering"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    echo -n "Multithreading: sniff-test ... "
    zpipe_mt -t32 -c128 -i1MiB -o1MiB 2> /dev/null
    if [ $? -ne 0 ]; then
	echo "failed"
	exit 1
    fi
    echo "ok"
}

function multithreading_unbuffered_memalign ()
{
    echo "--------------------------------------------------------------------"
    echo "zpipe_mt: HW comp/decomp w/o buffering and posix_memalign"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    for ibuf in 256KiB 4KiB 1000 ; do
	for obuf in 256KiB 4KiB 1000 ; do
	    echo -n "Multithreading: ib=${ibuf} ob=${obuf} posix_memalign  ... "
	    zpipe_mt -t32 -c100 -i${ibuf} -o${obuf} -p 2> /dev/null
	    if [ $? -ne 0 ]; then
		echo "failed"
		exit 1
	    fi
	    echo "ok"
	done
    done
}

function multithreading_unbuffered ()
{
    echo "--------------------------------------------------------------------"
    echo "zpipe_mt: HW compression/decompression without buffering"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=1
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=0
    export ZLIB_OBUF_TOTAL=0

    for ibuf in 256KiB 4KiB 1000 ; do
	for obuf in 256KiB 4KiB 1000 ; do
	    echo -n "Multithreading: ib=${ibuf} ob=${obuf} ... "
	    zpipe_mt -t32 -c64 -i${ibuf} -o${obuf} 2> /dev/null
	    if [ $? -ne 0 ]; then
		echo "failed"
		exit 1
	    fi
	    echo "ok"
	done
    done
}

function multithreading_buffered ()
{
    echo "--------------------------------------------------------------------"
    echo "zpipe_mt: HW compression/decompression with buffering"
    echo "--------------------------------------------------------------------"

    export ZLIB_INFLATE_IMPL=0
    export ZLIB_DEFLATE_IMPL=1
    export ZLIB_IBUF_TOTAL=1MiB
    export ZLIB_OBUF_TOTAL=1MiB

    for ibuf in 256KiB 4KiB 1000 ; do
	for obuf in 256KiB 4KiB 1000 ; do
	    echo -n "Multithreading: ib=${ibuf} ob=${obuf} ... "
	    zpipe_mt -t32 -c64 -i${ibuf} -o${obuf} 2> /dev/null
	    if [ $? -ne 0 ]; then
		echo "failed"
		exit 1
	    fi
	    echo "ok"
	done
    done
}

build_code

for flush in Z_PARTIAL_FLUSH Z_NO_FLUSH Z_FULL_FLUSH ; do
    zlib_append ${flush}
done

zlib_software
zlib_hardware_no_buffering
zlib_hardware_buffering
multithreading_unbuffered_memalign
multithreading_quick
multithreading_unbuffered
multithreading_buffered

exit 0
