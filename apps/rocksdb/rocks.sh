#!/bin/bash

git submodule update --init --recursive rocksdb
make -j -C rocksdb static_lib

