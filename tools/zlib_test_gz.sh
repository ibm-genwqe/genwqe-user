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
# Purpose: Test hardware-accelerated compression/decompression for 
#          identical results after 1 or more compression/decompression
#          iteration(s) of a given set of test files.

#
# Environment variables for the hardware compression zlib by default
# distribute workload over all plugged cards.
#

export ZLIB_ACCELERATOR=GENWQE
export ZLIB_CARD=-1

# Directories used
INSTALL_DIR="/opt/genwqe"                    # Tool RPM install directory
TOOLS_DIR="${INSTALL_DIR}/bin/genwqe"        # gzip, gunzip install directory
TMP_DIR="/tmp"                               # Temporary directory to use
DATA_DIR="${TMP_DIR}/$$_testdata"            # directory for testdata
ORIG_DATA="${TMP_DIR}/orig"                  # directory for original data
GZIPTOOL="${TOOLS_DIR}/gzip"
GUNZIPTOOL="${TOOLS_DIR}/gunzip"

# GenWQE Global values
DEVICE_ID=":044b"
SUPPORTED_APPID="GZIP"
DRIVER_PREFIX=genwqe
DRIVER=${DRIVER_PREFIX}_card
PROGRAM=`basename $0`
CWD=`pwd`
ERRLOG=${TMP_DIR}/${PROGRAM}_$$_err.log
CLASS_PATH="/sys/class/${DRIVER_PREFIX}/"

# GenWQE add supported bit stream for version strings to this array:
BIT_STREAMS=( 00000b0330342260.00000002475a4950 \
              0000000330353090.00000002475a4950 )

# Helper variables
driver_check=1                               # flag for GenWQE card driver
iterations=100                               # holds the number of repeats
nr_instances=10                              # number of parallel tests
version="https://github.com/ibm-genwqe/genwqe-user"
runpids=
verbose=0
bitstream_warning=1

# Print helper function to output test depending on verbose flag
function printv() {
    if [ $verbose -gt 0 ]; then
        echo $*
    fi    
}

# Helper function to stop any running workload jobs
function stop_jobs() {
    JOBS=`jobs -rp`
    printv "Running: "$JOBS
    printv "Expected: ${runpids}"
    printv "Wait while terminating jobs..."
    kill -n 15 $JOBS
    sleep 2 
    wait
    JOBS=`jobs -rp`
    printv "Still running: "$JOBS
    runpids=""
    cleanup
}

# Cleanup helper function called when CTRL-C is pressed or process is killed
function cleanup() {
  rm -rf ${DATA_DIR}_*
  rm -f ${ORIG_DATA}/.lock_$$         # remove own lockfile which frees the
                                      # orig data for own process
  LOCKFILES=`ls ${ORIG_DATA}/.lock* 2>/dev/null`
  if [ "$LOCKFILES" == "" ]; then     # all lock files removed?
      printv "Cleaning up source data..."
      rm -rf ${ORIG_DATA}
      unset ZLIB_CARD
      printv "Done"
  fi
}
  
trap stop_jobs SIGINT
trap stop_jobs SIGKILL

# Print usage message helper function
function usage() {
    echo "Usage of $PROGRAM:"
    echo "    [-h]  Print this help message."
    echo "    [-s]  Run on software zlib if ${DRIVER} is not loaded."
    echo "    [-S]  Run on software zlib even if ${DRIVER} is loaded."
    echo "    [-i]  <iterations>  repeat multiple times for more testing."
    echo "          Default: 100"
    echo "    [-p]  <number of instances> of this test to run in parallel."
    echo "          Default: 10"
    echo "    [-A]  <accelerator> use either GENWQE for the PCIe and CAPI for"
    echo "          CAPI based solution available only on System p"
    echo "          Use SW to use software compress/decompression"
    echo "    [-C]  <card> set the compression card to use (0, 1, ... )."
    echo "          RED (or -1) drive work to all available cards."
    echo "    [-v]  Print status and informational output."
    echo "          -vv for more verbosity"
    echo "    [-V]  Print program version (${version})"
    echo
}

# Check for tools availability
function check_tools
{
    for t in ${GZIPTOOL} ${GUNZIPTOOL} ; do
	echo -n "Checking if ${t} is there ... "
	if [ ! -x ${t} ]; then
	    echo "failure"
	    exit -3;
	fi
	echo "ok"
    done
}

# Check for and create directory holding the original data
function prep_orig_data()
{
    local d=${INSTALL_DIR}/share/testdata/testdata.tar.gz

    printv "Preparing data..."

    if [ ! -f ${d} ]; then
	echo "Testdata \"${d}\" is missing. Please install it first.";
	echo
	echo "E.g.:";
	echo "  wget http://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz";
	echo "  sudo mkdir -p ${INSTALL_DIR}/share/testdata/";
	echo "  sudo cp cantrbry.tar.gz ${d}";
	echo
	exit -1;
    fi

    if [ ! -d ${ORIG_DATA} ]; then
        mkdir -p $ORIG_DATA
    fi
    cp ${d} ${ORIG_DATA}
    cd ${ORIG_DATA}
    tar zxf testdata.tar.gz
    rm -f ${ORIG_DATA}/testdata.tar.gz
}

function ffdc_msg_and_cleanup()
{
        ls -alR ${ORIG_DATA} ${WORK_DIR} >> $ERRLOG;
        echo "---------------- dmesg output -------------" >> $ERRLOG;
        dmesg >> $ERRLOG;
        echo " ========= END OF LOG     `date`==========" >> $ERRLOG;
        echo "$ERRLOG has been written."
        echo "Open an IBM-internal defect against $DRIVER development,"
        echo "copy all console messages related to the error"
        echo "and attach $ERRLOG. Thanks."
	rm -rf ${WORK_DIR}
}

# Check for and create working directories.
# These are deleted upon script termination.
function prep_work_and_run_load()
{
    WORK_DIR="${DATA_DIR}_$1"
    if [ ! -d ${WORK_DIR} ]; then
        mkdir -p $WORK_DIR
    fi
    touch ${ORIG_DATA}/.lock_$$     # mark source data as 'in use'
    cp -r * ${WORK_DIR}/
    cd ${WORK_DIR}
    # Compress/decompress and compare in each iteration
    for i in `seq 1 $iterations` ; do
        if [ $verbose -gt 1 ]; then
	    echo  "  Run #$i/Instance_$1 : unpacking, packing, comparing... ";
	fi
        for file in `ls`; do
	    ${GZIPTOOL} $file;
	    rc=$?
	    if [ $rc -ne 0 ]; then
	        echo "$PROGRAM: ERROR - ${GZIPTOOL} returned RC=$rc"     \
                 | tee $ERRLOG
		ffdc_msg_and_cleanup
		exit -4
            fi
        done
        for file in `ls`; do
            ${GUNZIPTOOL} $file;
            rc=$?
            if [ $rc -ne 0 ]; then
	        echo "$PROGRAM: ERROR - ${GUNZIPTOOL} returned RC=$rc"   \
                |tee $ERRLOG
		ffdc_msg_and_cleanup
		exit -5
            fi
	done
        for file in `ls`; do
            diff -q $file ${ORIG_DATA}/$file;
	    rc=$?
            if [ $rc -ne 0 ]; then
                echo "$PROGRAM: ERROR - miscompare in run # $i/Instance $1:"   \
                     "File ${WORK_DIR}/$file after successful compression"     \
                     "decompression" | tee $ERRLOG;
                ffdc_msg_and_cleanup
                exit -6
            fi
        done
    done
}

# Checks an input bitstream value against a fixed list of bit stream versions
# in the way that global value bitstream_warning is set as follows
#    0    if the bit stream is among the versions $DRIVER supports
#    1    if an unsupported bit stream version is on the PCIe card
# Parameters:    1. bit stream value of card to verify
#                2. Application ID (String) of card to verify, 'GZIP' for comp.
function is_supported()
{
    if [ "$2" != "${SUPPORTED_APPID}" ]; then
        # ignore an unsupported appid, prevent bitstream warning
        bitstream_warning=0;
    else
        for i in `seq 0 $((${#BIT_STREAMS[@]}-1))`; do
            if [ "$1" == "${BIT_STREAMS[$i]}" ]; then
                bitstream_warning=0;
	    fi
        done
    fi
}

# GenWQE Prerequisite: The genwqe driver needs to be loaded for this
#                      function to be able to check the bit stream version
#
function genwqe_check_supported_bitstream_versions()
{
    if [ $ZLIB_CARD -ge 0 ]; then
        # check APPID
	appid=`cat ${CLASS_PATH}${DRIVER_PREFIX}${ZLIB_CARD}_card/appid`
        # check bit stream version for a specific virtual or physical Function
	ver=`cat ${CLASS_PATH}${DRIVER_PREFIX}${ZLIB_CARD}_card/version`
	is_supported $ver $appid
    else
        # ZLIB_CARD is set to -1. Thus run against all cards, to
	# check the bit streams.
        # Determine the number of cards via lspci
        NR_CARDS=`lspci -d$DEVICE_ID|wc -l`
        for i in `seq 0 $((NR_CARDS - 1))`; do
            appid=`cat ${CLASS_PATH}${DRIVER_PREFIX}${i}_card/appid`
            ver=`cat ${CLASS_PATH}${DRIVER_PREFIX}${i}_card/version`
	    is_supported $ver $appid
        done
    fi
}

# The tools RPM is a prerequisite for this test (see purpose description above)
#
if [ ! -d ${TOOLS_DIR} ]; then
    echo "$PROGRAM: ERROR: gzip and gunzip tools not found in $TOOLS_DIR."
    echo "Make sure the GenWQELibZ RPM Package is installed and gzip and gunzip"
    echo "are executable for ${USER} in $TOOLS_DIR."
    echo
    exit -1
fi

# Parse any options given on the command line
while getopts "A:vVhsSi:C:p:" opt; do
    case ${opt} in
        s)
	    driver_check=0;
	    ;;
        S)
	    driver_check=0;
	    # use software zlib rather than hardware.
            GZIPTOOL=gzip;
            GUNZIPTOOL=gunzip;
	    ;;
        i)
            iterations=${OPTARG};
            ;;
        p)
            nr_instances=${OPTARG};
            ;;
        A)
            export ZLIB_ACCELERATOR=${OPTARG};
	    ;;
	C)
	    ZLIB_CARD=${OPTARG};
            if [ "${OPTARG}" == "-1" -o  "${OPTARG}" == "RED" ]; then
                ZLIB_CARD=-1;
            fi
	    ;;
        v)
            verbose+=1;
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

# Start of main program

if [ $ZLIB_ACCELERATOR == "GENWQE" ]; then
# warn user if the hardware acceleration for compression/decompression
# is not ready
    driver_loaded=`lsmod|grep $DRIVER`
    if [ ${driver_check} -eq 1 ]; then
	if [ "${driver_loaded}" = "" ]; then
	    echo "WARNING: ${DRIVER} is not loaded." \
		"No Hardware compression available!"
	    exit -2;
	fi
    fi
    if [ "${driver_loaded}" != "" -a ${driver_check} -eq 1 ]; then
	genwqe_check_supported_bitstream_versions $ZLIB_CARD
	if [ $bitstream_warning -eq 1 ]; then
	    2>&1 echo "ERROR: Unsupported FPGA image (bitstream) detected on one"  \
		"or more cards."
	    2>&1 echo "       Check the bitstream versions on all of your cards and"
	    2>&1 echo "       update to a supported version for the compression"   \
		"solution."
	    exit -3;
	fi
    fi
fi


# Check if tools are available
check_tools

# provide source data
prep_orig_data

TStart=`date +%s`

# Main part of test driving workload to card or software zlib
for i in `seq 1 $nr_instances`; do
    prep_work_and_run_load $i &
    newpid=$!
    if [ $verbose -ge 2 ]; then echo "NewPID:    $newpid"; fi
    runpids=$runpids" "$newpid
    if [ $verbose -ge 2 ]; then printv "RunPIDs:   $runpids"; fi
done

# Workload is kicked off, wait for it to finish
printv "Waiting for jobs to terminate ..."
wait

# Calculate duration
TEnd=`date +%s`
T=`expr ${TEnd} - ${TStart}`
printv  "Runtime: $((T/3600%24)):$((T/60%60)):$((T%60)) [H:M:S]"

# Cleanup the test data and the copy of original data
cleanup

exit 0

