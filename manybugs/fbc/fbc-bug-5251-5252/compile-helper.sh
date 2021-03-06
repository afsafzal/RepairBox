#!/bin/bash
LOCAL_ROOT=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
LOCAL_ROOT="$LOCAL_ROOT/local-root"

pushd fbc

echo "Uninstalling..."
./install.sh -u $LOCAL_ROOT > /dev/null

echo "Building compiler..."
pushd src/compiler/obj/linux && \
../../configure --prefix="$LOCAL_ROOT" --build=i686-linux-gnu "CFLAGS=-m32" "LDFLAGS=-m32" "CXXFLAGS=-m32" && \
make && \
make install && \
popd && \

echo "Building rtlib..." && \
pushd src/rtlib/obj/linux && \
../../configure --prefix="$LOCAL_ROOT" --build=i686-linux-gnu "CFLAGS=-m32 -O2" "LDFLAGS=-m32" "CXXFLAGS=-m32" && \
export MULTITHREADED= && \
make  && \
make MULTITHREADED=1 && \
make install && \
popd && \

echo "Building gfxlib2..." && \
pushd src/gfxlib2/obj/linux && \
../../configure --prefix="$LOCAL_ROOT" --build=i686-linux-gnu "CFLAGS=-m32 -O2" "LDFLAGS=-m32" "CXXFLAGS=-m32" && \
make && \
make install && \
popd && \

echo "Installing FBC..." && \
./install.sh -i $LOCAL_ROOT

if [ $? == 0 ]; then
  echo "Installed FBC"
  exit 0
else
  echo "Failed to compile"
  exit 1
fi
