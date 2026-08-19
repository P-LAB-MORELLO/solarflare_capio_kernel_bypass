#!/bin/sh
# Arm A rebuild: hybrid F-Stack matched to arm B's configuration.
#
# Same sources (including every purecap fix, which compiles away under
# aapcs), same disabled subsystems (RACK/BBR/HPTS, IPFW, netgraph), same
# bundled DPDK 24.11.6. The only differences from arm B are the ABI
# (-mabi=aapcs) and the driver (stock net_sfc via nic_uio instead of
# net_capio via sfc7120pol).
cd /root/f-stack/lib || exit 1
export PKG_CONFIG_PATH=/root/fsdpdk-install/libdata/pkgconfig
export FF_PATH=/root/f-stack FF_DPDK=/root/fsdpdk-install
CC="clang -mabi=aapcs -g -Wno-error -Wno-unknown-warning-option -Wno-ignored-optimization-argument -Wno-builtin-macro-redefined"
rm -f *.o libfstack.a filtered_predefined_macros.h && rm -rf machine_include
gmake -j1 FF_TCPHPTS= FF_EXTRA_TCP_STACKS= FF_IPFW= FF_NETGRAPH= \
    CC="$CC" WERROR="-Wno-unused-variable" > /root/fstack-hyb-build.log 2>&1
echo "EXIT=$?" >> /root/fstack-hyb-build.log
