#!/bin/sh
# Purecap F-Stack build (arm B). Default clang on Morello is already purecap.
#
# The rm line is load-bearing: this directory is shared with the hybrid build,
# and an incremental gmake across an ABI switch silently reuses the other
# ABI's objects (bitten twice). The generated filtered_predefined_macros.h and
# machine_include are equally ABI-stale and must go too.
cd /root/f-stack/lib || exit 1
rm -f *.o libfstack.a filtered_predefined_macros.h
rm -rf machine_include
export PKG_CONFIG_PATH=/root/fsdpdk-purecap-install/libdata/pkgconfig
export FF_PATH=/root/f-stack FF_DPDK=/root/fsdpdk-purecap-install
CC="clang -g -Wno-error -Wno-unknown-warning-option -Wno-ignored-optimization-argument -Wno-builtin-macro-redefined"
gmake -j1 FF_TCPHPTS= FF_EXTRA_TCP_STACKS= FF_IPFW= FF_NETGRAPH= \
    CC="$CC" WERROR="-Wno-unused-variable" > /root/fstack-pc-build.log 2>&1
echo "EXIT=$?" >> /root/fstack-pc-build.log
