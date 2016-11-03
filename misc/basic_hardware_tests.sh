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
# Execute basic hardware tests for all available cards in a
# test system. This runs for GENWQE as well as for CAPI GZIP cards.
# It needs some test data to work on and prints out a message if the
# test data is not available. Use for automated regression testing.
#

export PATH=`pwd`/tools:`pwd`/misc:$PATH
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH

# lock dirs/files
LOCKDIR="/tmp/genwqe_hardware_tests.lock"
PIDFILE="${LOCKDIR}/PID"

# exit codes and text
ENO_SUCCESS=0;  ETXT[0]="ENO_SUCCESS"
ENO_GENERAL=1;  ETXT[1]="ENO_GENERAL"
ENO_LOCKFAIL=2; ETXT[2]="ENO_LOCKFAIL"
ENO_RECVSIG=3;  ETXT[3]="ENO_RECVSIG"

trap 'ECODE=$?; echo "Exit: ${ETXT[ECODE]}($ECODE)" >&2' 0
echo -n "Locking: " >&2

if mkdir "${LOCKDIR}" &>/dev/null; then
    # Lock succeeded, install signal handlers before storing the PID
    # just in case storing the PID fails.
    trap 'ECODE=$?;
          echo "Removing lock. Exit: ${ETXT[ECODE]}($ECODE)" >&2
          rm -rf "${LOCKDIR}"' 0
    echo "$$" >"${PIDFILE}" 
    # The following handler will exit the script upon receiving these
    # signals the trap on "0" (EXIT) from above will be triggered by
    # this trap's "exit" command!
    trap 'echo "Killed by a signal." >&2
          exit ${ENO_RECVSIG}' 1 2 3 15
else
    # If cat isn't able to read the file, another instance is probably
    # about to remove the lock -- exit, we're *still* locked.
    # Lock failed, check if the other PID is alive.
    OTHERPID="$(cat "${PIDFILE}")"
    if [ $? != 0 ]; then
        echo "Lock failed, PID ${OTHERPID} is active" >&2
        exit ${ENO_LOCKFAIL}
    fi
    if ! kill -0 $OTHERPID &>/dev/null; then
        # lock is stale, remove it and restart
        echo "Removing stale lock of nonexistent PID ${OTHERPID}" >&2
        rm -rf "${LOCKDIR}"
        exit ${ENO_LOCKFAIL}
    else
        # Lock is valid and OTHERPID is active - exit, we're locked!
        echo "Lock failed, PID ${OTHERPID} is active" >&2
        exit ${ENO_LOCKFAIL}
    fi
fi

# Checks
if [ ! -f cantrbry.tar.gz ]; then
	echo "We need test case data: cantrbry.tar.gz"
	echo "Get it by using:"
	echo "  wget http://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz"
	echo
fi

# Tests
for accel in GENWQE CAPI ; do
	for card in `./tools/genwqe_find_card -A${accel}`; do
		echo "TESTING ${accel} CARD ${card}"

		zlib_test.sh  -A${accel} -C${card}
		if [ $? -ne 0 ]; then
			echo "FAILED ${accel} CARD ${card}"
			exit 1
		fi

		genwqe_mt_perf -A${accel} -C${card}
		if [ $? -ne 0 ]; then
			echo "FAILED ${accel} CARD ${card}"
			exit 1
		fi

		genwqe_test_gz -A${accel} -C${card} -vv -i10 -t cantrbry.tar.gz
		if [ $? -ne 0 ]; then
			echo "FAILED ${accel} CARD ${card}"
			exit 1
		fi

		echo "PASSED ${accel} CARD ${card}"
	done
done

dmesg -T > basic_hardware_test.dmesg
exit 0;
