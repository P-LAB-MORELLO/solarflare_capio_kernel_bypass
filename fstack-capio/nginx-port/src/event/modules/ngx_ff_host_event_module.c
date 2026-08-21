 /*
 * Copyright (C) 2017-2021 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

 #include <ngx_config.h>
 #include <ngx_core.h>
 #include <ngx_event.h>
 #include <ngx_channel.h>
 #include <ngx_cycle.h>

 #include <pthread.h>

#if defined(__FreeBSD__)
/*
 * ngx_kq_shim: minimal epoll emulation over kqueue, private to this module.
 * Level-triggered unless EPOLLET; one kqueue filter per direction.
 */
#include <sys/event.h>
#include <sys/time.h>
#include <dlfcn.h>

/* The ff module interposes kqueue()/kevent() in this binary; the host-event
 * shim must reach the KERNEL implementations regardless. */
static int (*shim_real_kqueue)(void);
static int (*shim_real_kevent)(int, const struct kevent *, int,
    struct kevent *, int, const struct timespec *);

static inline int
shim_kqueue(void)
{
    if (shim_real_kqueue == NULL)
        shim_real_kqueue = dlsym(RTLD_NEXT, "kqueue");
    return shim_real_kqueue();
}

static inline int
shim_kevent(int kq, const struct kevent *cl, int ncl,
    struct kevent *el, int nel, const struct timespec *ts)
{
    if (shim_real_kevent == NULL)
        shim_real_kevent = dlsym(RTLD_NEXT, "kevent");
    return shim_real_kevent(kq, cl, ncl, el, nel, ts);
}

#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLLET     (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct epoll_event {
    uint32_t events;
    union { void *ptr; } data;
};

static int
epoll_create(int size)
{
    (void) size;
    return shim_kqueue();
}

static int
epoll_ctl(int kq, int op, int fd, struct epoll_event *ee)
{
    struct kevent kev[2];
    int n = 0;
    uint32_t events = (op == EPOLL_CTL_DEL) ? 0 : ee->events;
    void *ud = (op == EPOLL_CTL_DEL) ? NULL : ee->data.ptr;
    unsigned short clear = (events & EPOLLET) ? EV_CLEAR : 0;

    if (events & EPOLLIN)
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | clear,
               0, 0, ud);
    else
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

    if (events & EPOLLOUT)
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | clear,
               0, 0, ud);
    else
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

    /* Deleting an absent filter is expected (ENOENT); report real errors. */
    if (shim_kevent(kq, kev, n, NULL, 0, NULL) == -1 && errno != ENOENT)
        return -1;
    return 0;
}

static int
epoll_wait(int kq, struct epoll_event *events, int maxevents, int timeout_ms)
{
    struct kevent   kev[64];
    struct timespec ts, *tsp;
    int             i, n, out = 0;

    if (maxevents > 64)
        maxevents = 64;

    if (timeout_ms < 0) {
        tsp = NULL;
    } else {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    n = shim_kevent(kq, NULL, 0, kev, maxevents, tsp);
    if (n <= 0)
        return n;

    for (i = 0; i < n; i++) {
        uint32_t rev = 0;
        if (kev[i].flags & EV_ERROR)
            rev |= EPOLLERR;
        if (kev[i].filter == EVFILT_READ) {
            rev |= EPOLLIN;
            if (kev[i].flags & EV_EOF)
                rev |= EPOLLRDHUP;
        } else if (kev[i].filter == EVFILT_WRITE) {
            rev |= EPOLLOUT;
        }
        events[out].events = rev;
        events[out].data.ptr = kev[i].udata;
        out++;
    }
    return out;
}
#endif /* __FreeBSD__ / ngx_kq_shim */

 #if (NGX_HAVE_FSTACK)
 static void * ngx_ff_host_event_create_conf(ngx_cycle_t *cycle);
 static char * ngx_ff_host_event_init_conf(ngx_cycle_t *cycle,
	 void *conf);
 static ngx_int_t ngx_ff_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
 static ngx_int_t ngx_ff_epoll_add_event(ngx_event_t *ev,
	 ngx_int_t event, ngx_uint_t flags);
 static ngx_int_t ngx_ff_epoll_del_event(ngx_event_t *ev,
	 ngx_int_t event, ngx_uint_t flags);
 static ngx_int_t ngx_ff_epoll_process_events(ngx_cycle_t *cycle,
	 ngx_msec_t timer, ngx_uint_t flags);

 static int ep = -1;
 static struct epoll_event *event_list;
 static ngx_uint_t nevents;

 typedef struct {
	 ngx_uint_t  events;
 } ngx_ff_host_event_conf_t;


 static ngx_command_t  ngx_ff_host_event_commands[] = {
	 ngx_null_command
 };

 ngx_core_module_t  ngx_ff_host_event_module_ctx = {
	 ngx_string("ff_host_event"),
	 ngx_ff_host_event_create_conf,          /* create configuration */
	 ngx_ff_host_event_init_conf,            /* init configuration */
 };

 ngx_module_t  ngx_ff_host_event_module = {
	 NGX_MODULE_V1,
	 &ngx_ff_host_event_module_ctx,          /* module context */
	 ngx_ff_host_event_commands,             /* module directives */
	 NGX_CORE_MODULE,                     /* module type */
	 NULL,                                /* init master */
	 NULL,                                /* init module */
	 NULL,                                /* init process */
	 NULL,                                /* init thread */
	 NULL,                                /* exit thread */
	 NULL,                                /* exit process */
	 NULL,                                /* exit master */
	 NGX_MODULE_V1_PADDING
 };
 
 static void *
 ngx_ff_host_event_create_conf(ngx_cycle_t *cycle)
 {
	 ngx_ff_host_event_conf_t  *cf;
	 cf = ngx_palloc(cycle->pool, sizeof(ngx_ff_host_event_conf_t));
	 if (cf == NULL) {
		 return NULL;
	 }
	 cf->events = NGX_CONF_UNSET;
	 return cf;
 }
 
 static char *
 ngx_ff_host_event_init_conf(ngx_cycle_t *cycle, void *conf)
 {
	 ngx_ff_host_event_conf_t *cf = conf;
	 cf->events = 8;
	 return NGX_CONF_OK;
 }
 
 
 static ngx_int_t
 ngx_ff_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
 {
	 if (ep == -1) {
		 /* The size is just a hint */
		 ep = epoll_create(100);
 
		 if (ep == -1) {
			 ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
						   "epoll_create() failed");
			 return NGX_ERROR;
		 }
	 }
 
	 if (event_list) {
		 ngx_free(event_list);
	 }
 
	 nevents = 64;
 
	 event_list = ngx_alloc(sizeof(struct epoll_event) * nevents, cycle->log);
	 if (event_list == NULL) {
		 return NGX_ERROR;
	 }
 
	 return NGX_OK;
 }
 
 static void
 ngx_ff_epoll_done(ngx_cycle_t *cycle)
 {
	 if (close(ep) == -1) {
		 ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
					   "epoll close() failed");
	 }
 
	 ep = -1;
 
	 ngx_free(event_list);
 
	 event_list = NULL;
	 nevents = 0;
 }
 
 static ngx_int_t
 ngx_ff_epoll_add_event(ngx_event_t *ev, ngx_int_t event,
	 ngx_uint_t flags)
 {
	 int                  op;
	 uint32_t             events, prev;
	 ngx_event_t         *e;
	 ngx_connection_t    *c;
	 struct epoll_event   ee;
 
	 c = ev->data;
 
	 events = (uint32_t) event;
 
	 if (event == NGX_READ_EVENT) {
		 e = c->write;
		 prev = EPOLLOUT;
 #if (NGX_READ_EVENT != EPOLLIN|EPOLLRDHUP)
		 events = EPOLLIN|EPOLLRDHUP;
 #endif
 
	 } else {
		 e = c->read;
		 prev = EPOLLIN|EPOLLRDHUP;
 #if (NGX_WRITE_EVENT != EPOLLOUT)
		 events = EPOLLOUT;
 #endif
	 }
 
	 if (e->active) {
		 op = EPOLL_CTL_MOD;
		 events |= prev;
 
	 } else {
		 op = EPOLL_CTL_ADD;
	 }
 
	 ee.events = events | (uint32_t) flags;
	 ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);
 
	 ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
					"epoll add event: fd:%d op:%d ev:%08XD",
					c->fd, op, ee.events);
 
	 if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
		 ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
					   "epoll_ctl(%d, %d) failed", op, c->fd);
		 return NGX_ERROR;
	 }
 
	 ev->active = 1;
 #if 0
	 ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;
 #endif
 
	 return NGX_OK;
 }
 
 static ngx_int_t
 ngx_ff_epoll_del_event(ngx_event_t *ev, ngx_int_t event,
	 ngx_uint_t flags)
 {
	 int                  op;
	 uint32_t             prev;
	 ngx_event_t         *e;
	 ngx_connection_t    *c;
	 struct epoll_event   ee;
 
	 /*
	  * when the file descriptor is closed, the epoll automatically deletes
	  * it from its queue, so we do not need to delete explicitly the event
	  * before the closing the file descriptor
	  */
 
	 if (flags & NGX_CLOSE_EVENT) {
		 ev->active = 0;
		 return NGX_OK;
	 }
 
	 c = ev->data;
 
	 if (event == NGX_READ_EVENT) {
		 e = c->write;
		 prev = EPOLLOUT;
 
	 } else {
		 e = c->read;
		 prev = EPOLLIN|EPOLLRDHUP;
	 }
 
	 if (e->active) {
		 op = EPOLL_CTL_MOD;
		 ee.events = prev | (uint32_t) flags;
		 ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);
 
	 } else {
		 op = EPOLL_CTL_DEL;
		 ee.events = 0;
		 ee.data.ptr = NULL;
	 }
 
	 ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
					"epoll del event: fd:%d op:%d ev:%08XD",
					c->fd, op, ee.events);
 
	 if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
		 ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
					   "epoll_ctl(%d, %d) failed", op, c->fd);
		 return NGX_ERROR;
	 }
 
	 ev->active = 0;
 
	 return NGX_OK;
 }
 
 static ngx_int_t
 ngx_ff_epoll_add_connection(ngx_connection_t *c)
 {
	 struct epoll_event  ee;
 
	 ee.events = EPOLLIN|EPOLLOUT|EPOLLET|EPOLLRDHUP;
	 ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);
 
	 ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
					"epoll add connection: fd:%d ev:%08XD", c->fd, ee.events);
 
	 if (epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ee) == -1) {
		 ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
					   "epoll_ctl(EPOLL_CTL_ADD, %d) failed", c->fd);
		 return NGX_ERROR;
	 }
 
	 c->read->active = 1;
	 c->write->active = 1;
 
	 return NGX_OK;
 }
 
 
 static ngx_int_t
 ngx_ff_epoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
 {
	 int                 op;
	 struct epoll_event  ee;
 
	 /*
	  * when the file descriptor is closed the epoll automatically deletes
	  * it from its queue so we do not need to delete explicitly the event
	  * before the closing the file descriptor
	  */
 
	 if (flags & NGX_CLOSE_EVENT) {
		 c->read->active = 0;
		 c->write->active = 0;
		 return NGX_OK;
	 }
 
	 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
					"epoll del connection: fd:%d", c->fd);
 
	 op = EPOLL_CTL_DEL;
	 ee.events = 0;
	 ee.data.ptr = NULL;
 
	 if (epoll_ctl(ep, op, c->fd, &ee) == -1) {
		 ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
					   "epoll_ctl(%d, %d) failed", op, c->fd);
		 return NGX_ERROR;
	 }
 
	 c->read->active = 0;
	 c->write->active = 0;
 
	 return NGX_OK;
 }
 
 static ngx_int_t
 ngx_ff_epoll_process_events(ngx_cycle_t *cycle,
	 ngx_msec_t timer, ngx_uint_t flags)
 {
	 int                events;
	 uint32_t           revents;
	 ngx_int_t          instance, i;
	 ngx_uint_t         level;
	 ngx_err_t          err;
	 ngx_event_t       *rev, *wev;
	 ngx_connection_t  *c;
 
	 /* NGX_TIMER_INFINITE == INFTIM */
 /*
	 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
					"epoll timer: %M", timer);
 */
	 events = epoll_wait(ep, event_list, (int) nevents, timer);
 
	 err = (events == -1) ? ngx_errno : 0;
 
	 if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
		 ngx_time_update();
	 }
 
	 if (err) {
		 if (err == NGX_EINTR) {
			 level = NGX_LOG_INFO;
		 } else {
			 level = NGX_LOG_ALERT;
		 }
 
		 ngx_log_error(level, cycle->log, err, "epoll_wait() failed");
		 return NGX_ERROR;
	 }
 
	 if (events == 0) {
		 if (timer != NGX_TIMER_INFINITE) {
			 return NGX_OK;
		 }
 
		 ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
					   "epoll_wait() returned no events without timeout");
		 return NGX_ERROR;
	 }
 
	 for (i = 0; i < events; i++) {
		 c = event_list[i].data.ptr;
 
		 instance = (uintptr_t) c & 1;
		 c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);
 
		 rev = c->read;
 
		 if (c->fd == -1 || rev->instance != instance) {
 
			 /*
			  * the stale event from a file descriptor
			  * that was just closed in this iteration
			  */
 
			 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
							"epoll: stale event %p", c);
			 continue;
		 }
 
		 revents = event_list[i].events;
 
		 ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
						"epoll: fd:%d ev:%04XD d:%p",
						c->fd, revents, event_list[i].data.ptr);
 
		 if (revents & (EPOLLERR|EPOLLHUP)) {
			 ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
							"epoll_wait() error on fd:%d ev:%04XD",
							c->fd, revents);
 
			 /*
			  * if the error events were returned, add EPOLLIN and EPOLLOUT
			  * to handle the events at least in one active handler
			  */
 
			 revents |= EPOLLIN|EPOLLOUT;
		 }
 
		 if ((revents & EPOLLIN) && rev->active) {
			 rev->ready = 1;
			 rev->available = -1;
			 rev->handler(rev);
		 }
 
		 wev = c->write;
 
		 if ((revents & EPOLLOUT) && wev->active) {
 
			 if (c->fd == -1 || wev->instance != instance) {
 
				 /*
				  * the stale event from a file descriptor
				  * that was just closed in this iteration
				  */
 
				 ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
								"epoll: stale event %p", c);
				 continue;
			 }
 
			 wev->ready = 1;
 #if (NGX_THREADS)
			 wev->complete = 1;
 #endif
			 wev->handler(wev);
		 }
	 }
 
	 return NGX_OK;
 }
 
 ngx_event_actions_t   ngx_ff_host_event_actions = {
	 ngx_ff_epoll_add_event,             /* add an event */
	 ngx_ff_epoll_del_event,             /* delete an event */
	 ngx_ff_epoll_add_event,             /* enable an event */
	 ngx_ff_epoll_add_event,             /* disable an event */
	 ngx_ff_epoll_add_connection,        /* add an connection */
	 ngx_ff_epoll_del_connection,        /* delete an connection */
	 NULL,                               /* trigger a notify */
	 ngx_ff_epoll_process_events,        /* process the events */
	 ngx_ff_epoll_init,                  /* init the events */
	 ngx_ff_epoll_done,                  /* done the events */
 };
 
 #endif
 