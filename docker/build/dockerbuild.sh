#!/bin/sh
set -e
set -o xtrace

cmake -DWITH_STATIC=OFF . && make && make install
