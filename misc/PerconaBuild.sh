#!/bin/bash

TOPDIR=`pwd`/PerconaFT

function cleanup() {
    rm -rf $TOPDIR/build $TOPDIR/install
}

function build() {
    if [ ! -d $TOPDIR ]; then
        git clone --branch ppc64 https://github.com/davidzengxhsh/PerconaFT.git $TOPDIR
        if [ $? -ne 0 ]; then
            echo "Clone PerconaFT source failed."
            return 1
        fi    
    fi

    cd $TOPDIR
    BRANCH=`git branch | awk '{print $2}'`
    echo $BRANCH
    if [ "$BRANCH" != "ppc64" ]; then
        echo "Please get the correct PerconaFT branch..."
        return 1
    fi

	#hack the code to use ZLIB compression method
    sed -i 's/TOKU_DEFAULT_COMPRESSION_METHOD/TOKU_ZLIB_METHOD/g' ft/tests/ft-serialize-benchmark.cc

    mkdir -p $TOPDIR/build $TOPDIR/install
    cd $TOPDIR/build

    cmake -D BUILD_TESTING=ON -D CMAKE_INSTALL_PREFIX=$TOPDIR/install ..
    if [ $? -ne 0 ]; then
        echo "Configure PerconaFT Failed. Please make sure all the require package are installed."
        return 1
    fi
    cmake --build . --target install
    if [ $? -ne 0 ]; then
        echo "Build PerconaFT Failed. Please make sure all the require package are installed."
        return 1
    fi

    #delete the hack code here
	cd $TOPDIR
    git checkout -- ft/tests/ft-serialize-benchmark.cc
    return 0
}

function usage() {
    echo "Usage:"
    echo "  PerconaBuild.sh [OPTIONS]"
    echo "     -R        cleanup"
    echo "     -h        help"
    echo
}

# output formatting
bold=$(tput bold)
normal=$(tput sgr0)

while getopts "Rh" opt; do
    case $opt in
    R)
    cleanup;
    exit 0;
    ;;
    h)
    usage;
    exit 0;
    ;;
    \?)
    printf "${bold}ERROR:${normal} Invalid option: -${OPTARG}\n" >&2
    exit 1
    ;;
    :)
    printf "${bold}ERROR:${normal} Option -$OPTARG requires an argument.\n" >&2
    exit 1
    ;;
    esac
done


if [ ! -f $TOPDIR/install/lib/libtokuportability.so ]; then
    echo "Build Percona..."
    if ! build
    then
        echo "Failed."
    fi 
else
    echo "Percona already exist. Let's try some test."
fi

if [ -f $TOPDIR/build/ft/tests/compress-test ]; then
    echo "Percona compression test..."
    $TOPDIR/build/ft/tests/compress-test >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "compression test failed."
        exit 1    
    else
        echo "OK"
    fi
fi
    
if [ -f $TOPDIR/build/ft/tests/subblock-test-compression ]; then
    echo "Percona subblock compression test..."
    $TOPDIR/build/ft/tests/subblock-test-compression >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "subblock compression test failed."
        exit 1
    else
        echo "OK"
    fi
    
fi
