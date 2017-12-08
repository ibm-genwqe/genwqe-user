#!/bin/sh

TOPDIR=`pwd`/PerconaFT

function build() {

    git clone --branch ppc64 https://github.com/davidzengxhsh/PerconaFT.git $TOPDIR
    if [ $? -ne 0 ]; then
        echo "Clone PerconaFT source failed."
        return 1
    fi    

    cd $TOPDIR
    BRANCH=`git branch | awk '{print $2}'`
    echo $BRANCH
    if [ "$BRANCH" != "ppc64" ]; then
        echo "Please get the correct PerconaFT branch..."
        return 1
    fi

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
    return 0
}

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
