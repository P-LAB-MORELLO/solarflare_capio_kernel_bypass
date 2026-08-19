Full sources of every file modified in the F-Stack 1.24 tree (DPDK 24.11.6
bundled in-tree) for the CheriBSD/Morello purecap port, preserving tree
paths. Same content as ../fstack-purecap.patch, provided as ready files.
Base: github.com/F-Stack/f-stack @ the commit the patch applies to.
The CAPIO PMD itself (new files, not part of the diff) lives one level up:
../rte_eth_capio.c and ../capio-meson.build go to dpdk/drivers/net/capio/
with sfc7120_user.{c,h} symlinked from the kernel stub's userlib/.
