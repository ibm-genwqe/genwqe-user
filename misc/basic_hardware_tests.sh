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
# test data is not availabe. Use for automated regression testing.
#

export PATH=`pwd`/tools:$PATH
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH

# Checks
if [ ! -f cantrbry.tar.gz ]; then
	echo "We need test case data: cantrbry.tar.gz"
	echo "Get it by using:"
	echo "  wget http://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz"
	echo
fi

# Tests
for accel in CAPI GENWQE ; do
	for card in `./tools/genwqe_find_card -ACAPI`; do
		echo "TESTING ${accel} CARD ${card}"

		genwqe_mt_perf -A${accel} -C${card}
		if [ $? -ne 0 ]; then
			echo "FAILED ${accel}"
			exit 1
		fi

		genwqe_test_gz -A${accel} -C${card} -vv -t cantrbry.tar.gz
		if [ $? -ne 0 ]; then
			echo "FAILED ${accel}"
			exit 1
		fi

		echo "PASSED ${accel}"
	done
done

dmesg -W > basic_hardware_test.dmesg
exit 0;
