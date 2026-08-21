# nginx 1.28.0 on F-Stack/CheriBSD

Files here overlay a stock nginx-1.28.0 tree with F-Stack's ff module
applied. Both arms build from one tree; reconfigure per arm and delete
`objs/nginx` before relinking (make does not notice libfstack changes).

Purecap (fsB/fsBrev):

    export FF_PATH=/root/f-stack FF_DPDK=/root/fsdpdk-purecap-install \
           PKG_CONFIG_PATH=$FF_DPDK/libdata/pkgconfig
    ./configure --prefix=/root/nginx-pc --with-ff_module --with-cc=clang \
        --with-cc-opt='-O2 -g -Wno-error' \
        --without-http_rewrite_module --without-http_gzip_module

Hybrid (fsA):

    export FF_PATH=/root/f-stack FF_DPDK=/root/fsdpdk-install \
           PKG_CONFIG_PATH=$FF_DPDK/libdata/pkgconfig
    ./configure --prefix=/root/nginx-hyb --with-ff_module --with-cc=clang \
        --with-cc-opt='-mabi=aapcs -O2 -Wno-error' --with-ld-opt=-mabi=aapcs \
        --without-http_rewrite_module --without-http_gzip_module

Notable content:

- `src/event/modules/ngx_ff_module.c`: the fd-offset syscall interpose.
  Carries the purecap fixes: variadic `real_ioctl`, kqueue interpose gated
  until F-Stack init (EAL's interrupt thread otherwise dies), fd-0
  boundary in `restore_fstack_fd`, linux-vs-FreeBSD constant translation.
- `src/event/modules/ngx_ff_host_event_module.c`: private epoll-on-kqueue
  shim (libepoll-shim is purecap-only, so the hybrid arm cannot link it).
- `auto/types/sizeof`: adds the 16-byte-pointer case for purecap.
- `auto/make` / `auto/modules`: -lnuma removed; KQUEUE_SRCS dedupe.
- `conf/`: the exact nginx.conf per arm (env passthrough for FF_EXTRA_EAL
  and SFC_RX_WINDOW is required; nginx sanitizes the environment) and the
  f-stack.conf used by both.

Serve files are zeros: `dd if=/dev/zero of=html/f64k.bin bs=1024 count=64`
etc. for 1k/48k/64k.
