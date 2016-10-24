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

version="https://github.com/ibm-genwqe/genwqe-user"
card=0
verbose=0
export PATH=$PATH:./tools

dump_debugfs=0
dump_sysfs=0
dump_current=0
dump_config=0
dump_previous=0
dump_queues=0

function usage() {
    echo "Usage of $PROGRAM:"
    echo "     [-C <card>] card to be used for the FFDC gathering"
    echo "          Note: only Physical Function cards make sense here!"
    echo "     [-a] dump all available information"
    echo "     [-s] dump sysfs entries"
    echo "     [-d] dump debugfs entries"
    echo "     [-c] dump PCIe config space"
    echo "     [-q] dump all DDCB queues"
    echo "     [-t <0:current|1:previous>]"
    echo "          Mostly you might want to see \"previous\" data."
    echo "     [-V] print program version"
    echo "     [-h] help"
    echo
    echo "This utility dumps available first failure data capture (FFDC)"
    echo "information to stdout. It can be used if the card is still"
    echo "accessible, but not functioning correctly. Some functionality"
    echo "of this tool requires super-user privileges."
    echo
    echo "Note: To get all available information, you need to run this"
    echo "      script as superuser because it wants to write so some"
    echo "      card registers, which is only allowed for privileged users."
    echo
}

while getopts "C:asdcqt:Vh" opt; do
    case $opt in
	C)
	card=$OPTARG;
	;;
	a)
	dump_current=1;
	dump_previous=1;
	dump_debugfs=1;
	dump_sysfs=1;
	dump_config=1;
	dump_queues=1;
	;;
	s)
	dump_sysfs=1;
	;;
	d)
	dump_debugfs=1;
	;;
	c)
	dump_config=1;
	;;
	q)
	dump_queues=1;
	;;
	t)
	dump_type=$OPTARG;
	if [ $dump_type -eq 0 ]; then
	    dump_current=1;
	fi
	if [ $dump_type -eq 1 ]; then
	    dump_previous=1;
	fi
	if [ $dump_type -eq 2 ]; then
	    dump_current=1;
	    dump_previous=1;
	fi
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
        echo "Invalid option: -$OPTARG" >&2
        ;;
    esac
done

function do_dump_debugfs ()
{
    if [ $dump_current -eq 1 ]; then
	echo "--------------------------------------------------------------------"
	echo "Current traces"
	echo "--------------------------------------------------------------------"

	for id in 0 1 2 ; do
	    echo "curr_dbg_uid$id"
	    cat /sys/kernel/debug/genwqe/genwqe${card}_card/curr_dbg_uid${id}
	    if [ $? -ne 0 ]; then
		echo "failed!"
		exit 1
	    fi
	done
	echo

	echo "--------------------------------------------------------------------"
	echo "FIRs from current run (to check if recovery state is ok)"
	echo "  See dump below for all FIRs/FECs."
	echo "--------------------------------------------------------------------"
	
	cat /sys/kernel/debug/genwqe/genwqe${card}_card/curr_regs
	if [ $? -ne 0 ]; then
	    echo "failed!"
	    exit 1
	fi
	echo
    fi

    if [ $dump_previous -eq 1 ]; then
	echo "--------------------------------------------------------------------"
	echo "Previous traces"
	echo "--------------------------------------------------------------------"

	for id in 0 1 2 ; do
	    echo "prev_dbg_uid$id"
	    cat /sys/kernel/debug/genwqe/genwqe${card}_card/prev_dbg_uid${id}
	    if [ $? -ne 0 ]; then
		echo "failed!"
		exit 1
	    fi
	done
	echo

	echo "--------------------------------------------------------------------"
	echo "FIRs from previous run (most likely where the problem occurred)"
	echo "  See dump below for all FIRs/FECs."
	echo "--------------------------------------------------------------------"
	
	cat /sys/kernel/debug/genwqe/genwqe${card}_card/prev_regs
	if [ $? -ne 0 ]; then
	    echo "failed!"
	    exit 1
	fi
	echo

	echo "--------------------------------------------------------------------"
	echo "Current state of DDCB queue"
	echo "--------------------------------------------------------------------"
	
	cat /sys/kernel/debug/genwqe/genwqe${card}_card/ddcb_info
	if [ $? -ne 0 ]; then
	    echo "failed!"
	    exit 1
	fi
	echo
    fi

    echo "--------------------------------------------------------------------"
    echo "Driver/bitstream version information"
    echo "--------------------------------------------------------------------"

    cat /sys/kernel/debug/genwqe/genwqe${card}_card/info
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi
    echo
}

function do_dump_sysfs ()
{
    echo "===================================================================="
    echo "Genwqe Card FFDC Dump"
    echo "  `basename $0` version ${version}" 
    echo -n "  Dump taken: "
    date
    echo "===================================================================="

    echo -n "Type:              "
    cat /sys/class/genwqe/genwqe${card}_card/type
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi

    echo -n "AppID:             "
    cat /sys/class/genwqe/genwqe${card}_card/appid
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi

    echo -n "Version:           "
    cat /sys/class/genwqe/genwqe${card}_card/version
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi

    echo -n "Current bitstream: "
    cat /sys/class/genwqe/genwqe${card}_card/curr_bitstream
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi

    echo -n "Next bitstream:    "
    cat /sys/class/genwqe/genwqe${card}_card/next_bitstream
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi

    echo -n "Temperature:       "
    cat /sys/class/genwqe/genwqe${card}_card/tempsens
    if [ $? -ne 0 ]; then
	echo "failed!"
	exit 1
    fi
    echo "--------------------------------------------------------------------"
    echo
}

function do_dump_queues () {
    echo "--------------------------------------------------------------------"
    echo "Queue status of all PCI functions"
    echo "--------------------------------------------------------------------"

    genwqe_ffdc -C ${card} --dump-queues ;
    echo

}

function do_dump_config_space () {
    s=$( basename `ls -l /sys/class/genwqe/genwqe${card}_card/device | cut -d'>' -f2` )

    echo "--------------------------------------------------------------------"
    echo "PCIe Config Space of card${card}: ${s}"
    echo "--------------------------------------------------------------------"

    if [ -x /sbin/lspci ]; then
	/sbin/lspci -vvvxxxxs ${s}
    fi
    if [ -x /usr/bin/lspci ]; then
	/usr/bin/lspci -vvvxxxxs ${s}
    fi
}

# Ensure that root is executing this script.
if [ "$(id -u)" != "0" ]; then
    echo "warning: This script must be executed as root to get all available information!"
    exit 1;
fi

# Based on previous experience we dump the traces first which are in
# debugfs. Since accessing the card will at least mess up the trace
# with ffdc capturing traffic.

if [ $dump_debugfs -eq 1 ]; then
    do_dump_debugfs;
fi
if [ $dump_sysfs -eq 1 ]; then
    do_dump_sysfs;
fi
if [ $dump_config -eq 1 ]; then
    do_dump_config_space;
fi
if [ $dump_queues -eq 1 ]; then
    do_dump_queues;
fi

exit 0;
