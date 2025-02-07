/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  memcached - memory caching daemon
 *
 *       http://www.memcached.org/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */
#include "memcached.h"
#ifdef EXTSTORE
#include "storage.h"
#endif
#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <stdarg.h>

/* some POSIX systems need the following definition
 * to get mlockall flags out of sys/mman.h.  */
#ifndef _P1003_1B_VISIBLE
#define _P1003_1B_VISIBLE
#endif
/* need this to get IOV_MAX on some platforms. */
#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif
#include <pwd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sysexits.h>
#include <stddef.h>

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#endif

#define DISABLE_PRIVILEGE_CHECK

#ifdef DBG
int th_yield1 = 0;
int th_yield2 = 0;

//extern int sched_parked1;
//extern int sig_usr1_park;
//extern int preempt_park;
#endif

/* FreeBSD 4.x doesn't have IOV_MAX exposed. */
#ifndef IOV_MAX
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__GNU__)
# define IOV_MAX 1024
/* GNU/Hurd don't set MAXPATHLEN
 * http://www.gnu.org/software/hurd/hurd/porting/guidelines.html#PATH_MAX_tt_MAX_PATH_tt_MAXPATHL */
#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif
#endif
#endif

#define htonll(x) hton64(x)
#define htonl(x) hton32(x)
#define htons(x) hton16(x)
#define ntohll(x) ntoh64(x)
#define ntohl(x) ntoh32(x)
#define ntohs(x) ntoh16(x)

static struct tcache *udp_conn_tcache;
static __thread struct tcache_perthread udp_conn_tcache_pt;

/*
 * forward declarations
 */
static int try_read_command(conn *c);

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occurred (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

static enum try_read_result try_read_network(conn *c);

static void conn_set_state(conn *c, enum conn_states state);

/* stats */
static void stats_init(void);
static void server_stats(ADD_STAT add_stats, conn *c);
static void process_stat_settings(ADD_STAT add_stats, void *c);


/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void udp_handler(struct udp_spawn_data *d);
static void conn_close(conn *c);
conn *conn_new(enum conn_states init_state,
                const int read_buffer_size,
                enum network_transport transport);
static void complete_nread(conn *c);
static void process_command(conn *c, char *command);
static void write_and_free(conn *c, char *buf, int bytes);
static int ensure_iov_space(conn *c);
static int add_iov(conn *c, const void *buf, int len);
static int add_chunked_item_iovs(conn *c, item *it, int len);
static int add_msghdr(conn *c);
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow);
static void write_bin_miss_response(conn *c, char *key, size_t nkey);

#ifdef EXTSTORE
static void _get_extstore_cb(void *e, obj_io *io, int ret);
static inline int _get_extstore(conn *c, item *it, int iovst, int iovcnt);
#endif
static void conn_free(conn *c);

/** exported globals **/
struct stats stats;
struct stats_state stats_state;
struct settings settings;
time_t process_started;     /* when the process was started */

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;
#ifdef EXTSTORE
/* hoping this is temporary; I'd prefer to cut globals, but will complete this
 * battle another day.
 */
void *ext_storage;
#endif
/** file scope variables **/
#if 0
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;
#endif

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

static enum transmit_result transmit(conn *c);

#if 0
/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
static struct event maxconnsevent;
static void maxconns_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false) {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}
#endif

#define REALTIME_MAXDELTA 60*60*24*30

/*
 * given time value that's either unix time or delta from current unix time, return
 * unix time. Use the fact that delta can't exceed one month (and real time value can't
 * be that low).
 */
static rel_time_t realtime(const time_t exptime) {
    /* no. of seconds in 30 days - largest possible delta exptime */

    if (exptime == 0) return 0; /* 0 means never expire */

    if (exptime > REALTIME_MAXDELTA) {
        /* if item expiration is at/before the server started, give it an
           expiration time of 1 second after the server started.
           (because 0 means don't expire).  without this, we'd
           underflow and wrap around to some large value way in the
           future, effectively making items expiring in the past
           really expiring never */
        if (exptime <= process_started)
            return (rel_time_t)1;
        return (rel_time_t)(exptime - process_started);
    } else {
        return (rel_time_t)(exptime + current_time);
    }
}

static void stats_init(void) {
    memset(&stats, 0, sizeof(struct stats));
    memset(&stats_state, 0, sizeof(struct stats_state));
    stats_state.accepting_conns = true; /* assuming we start in this state. */

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    process_started = time(0) - ITEM_UPDATE_INTERVAL - 2;
    stats_prefix_init();
}

static void stats_reset(void) {
    STATS_LOCK();
    memset(&stats, 0, sizeof(struct stats));
    stats_prefix_clear();
    STATS_UNLOCK();
    threadlocal_stats_reset();
    item_stats_reset();
}

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 0;
    settings.udpport = 11211;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;          /* supplements accuracy of oldest_live */
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.factor = 1.25;
    settings.chunk_size = 48;         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024; /* The famous 1MB upper limit. */
    settings.slab_page_size = 1024 * 1024; /* chunks are split from 1MB pages. */
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.sasl = false;
    settings.maxconns_fast = true;
    settings.lru_crawler = false;
    settings.lru_crawler_sleep = 100;
    settings.lru_crawler_tocrawl = 0;
    settings.lru_maintainer_thread = false;
    settings.lru_segmented = true;
    settings.hot_lru_pct = 20;
    settings.warm_lru_pct = 40;
    settings.hot_max_factor = 0.2;
    settings.warm_max_factor = 2.0;
    settings.inline_ascii_response = false;
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.idle_timeout = 0; /* disabled */
    settings.hashpower_init = 0;
    settings.slab_reassign = true;
    settings.slab_automove = 1;
    settings.slab_automove_ratio = 0.8;
    settings.slab_automove_window = 30;
    settings.shutdown_command = false;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
    settings.dump_enabled = true;
    settings.crawls_persleep = 1000;
    settings.logger_watcher_buf_size = LOGGER_WATCHER_BUF_SIZE;
    settings.logger_buf_size = LOGGER_BUF_SIZE;
    settings.drop_privileges = false;
#ifdef MEMCACHED_DEBUG
    settings.relaxed_privileges = false;
#endif
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn *c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

#if 0
    if (IS_UDP(c->transport) && c->request_addr_size > 0) {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }
#endif

    c->msgbytes = 0;
    c->msgused++;

    if (IS_UDP(c->transport)) {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return 0;
}

/* Connection timeout thread bits */
static mutex_t tcp_conn_lock;
static conn **active_tcp_conns;
static uint64_t nactiveconns;

#define CONNS_PER_SLICE 100

static void conn_timeout_thread(void *arg) {
    int i;
    conn *c;
    rel_time_t oldest_last_cmd;
    int sleep_time;
    useconds_t timeslice = 1000000 / (settings.maxconns / CONNS_PER_SLICE);

    mutex_lock(&tcp_conn_lock);

    while(1) {
        if (settings.verbose > 2)
            fprintf(stderr, "idle timeout thread at top of connection list\n");

        oldest_last_cmd = current_time;

        for (i = 0; i < settings.maxconns; i++) {
            if ((i % CONNS_PER_SLICE) == 0) {
                if (settings.verbose > 2)
                    fprintf(stderr, "idle timeout thread sleeping for %ulus\n",
                        (unsigned int)timeslice);
                mutex_unlock(&tcp_conn_lock);
                timer_sleep(timeslice);
                mutex_lock(&tcp_conn_lock);
            }

            if (!active_tcp_conns[i])
                continue;

            c = active_tcp_conns[i];

            if (!IS_TCP(c->transport))
                continue;

            if (c->state != conn_new_cmd && c->state != conn_read)
                continue;

            if ((current_time - c->last_cmd_time) > settings.idle_timeout) {
                log_debug("Aborting idle connection with port %d", tcp_remote_addr(c->tcp_conn).port);
                tcp_abort(c->tcp_conn);
            } else {
                if (c->last_cmd_time < oldest_last_cmd)
                    oldest_last_cmd = c->last_cmd_time;
            }
        }

        /* This is the soonest we could have another connection time out */
        sleep_time = settings.idle_timeout - (current_time - oldest_last_cmd) + 1;
        if (sleep_time <= 0)
            sleep_time = 1;

        if (settings.verbose > 2)
            fprintf(stderr,
                    "idle timeout thread finished pass, sleeping for %ds\n",
                    sleep_time);
        mutex_unlock(&tcp_conn_lock);
        timer_sleep((useconds_t) sleep_time * 1000000);
        mutex_lock(&tcp_conn_lock);
    }

    mutex_unlock(&tcp_conn_lock);

}


#if 0
static int start_conn_timeout_thread() {
    int ret;

    if (settings.idle_timeout == 0)
        return -1;

    if ((ret = pthread_create(&conn_timeout_tid, NULL,
        conn_timeout_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create idle connection timeout thread: %s\n",
            strerror(ret));
        return -1;
    }

    return 0;
}

/*
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
    /* We're unlikely to see an FD much higher than maxconns. */
    int next_fd = dup(1);
    int headroom = 10;      /* account for extra unexpected open FDs */
    struct rlimit rl;

    max_fds = settings.maxconns + headroom + next_fd;

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxconns\n");
    }

    close(next_fd);

    if ((conns = calloc(max_fds, sizeof(conn *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        /* This is unrecoverable so bail out early. */
        exit(1);
    }
}
#endif

static const char *prot_text(enum protocol prot) {
    char *rv = "unknown";
    switch(prot) {
        case ascii_prot:
            rv = "ascii";
            break;
        case binary_prot:
            rv = "binary";
            break;
        case negotiating_prot:
            rv = "auto-negotiate";
            break;
    }
    return rv;
}

#if 0
void conn_close_idle(conn *c) {
    if (settings.idle_timeout > 0 &&
        (current_time - c->last_cmd_time) > settings.idle_timeout) {
        if (c->state != conn_new_cmd && c->state != conn_read) {
            if (settings.verbose > 1)
                fprintf(stderr,
                    "fd %p wants to timeout, but isn't in read state", c);
            return;
        }

        if (settings.verbose > 1)
            fprintf(stderr, "Closing idle fd %p\n", c);

        mythr()->stats.idle_kicks++;

        conn_set_state(c, conn_closing);
        drive_machine(c);
    }
}

/* bring conn back from a sidethread. could have had its event base moved. */
void conn_worker_readd(conn *c) {
    c->ev_flags = EV_READ | EV_PERSIST;
    event_set(&c->event, c->sfd, c->ev_flags, event_handler, (void *)c);
    event_base_set(c->thread->base, &c->event);
    c->state = conn_new_cmd;

    // TODO: call conn_cleanup/fail/etc
    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
    }
#ifdef EXTSTORE
    // If we had IO objects, process
    if (c->io_wraplist) {
        //assert(c->io_wrapleft == 0); // assert no more to process
        conn_set_state(c, conn_mwrite);
        drive_machine(c);
    }
#endif
}
#endif

conn *conn_new(enum conn_states init_state,
                const int read_buffer_size,
                enum network_transport transport) {
    conn *c;

        if (!(c = (conn *)aligned_alloc(CACHE_LINE_SIZE, sizeof(conn)))) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }
        memset(c, 0, sizeof(conn));
        MEMCACHED_CONN_CREATE(c);

        c->rbuf = c->wbuf = 0;
        c->ilist = 0;
        c->suffixlist = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;
        c->idx = -1;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->suffixsize = SUFFIX_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->ilist = (item **)malloc(sizeof(item *) * c->isize);
        c->suffixlist = (char **)malloc(sizeof(char *) * c->suffixsize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
                c->msglist == 0 || c->suffixlist == 0) {
            conn_free(c);
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            fprintf(stderr, "Failed to allocate buffers for connection\n");
            return NULL;
        }

    c->transport = transport;
    c->protocol = settings.binding_protocol;

#if 0
    /* unix socket mode doesn't need this, so zeroed out.  but why
     * is this done for every command?  presumably for UDP
     * mode.  */
    if (!settings.socketpath) {
        c->request_addr_size = sizeof(c->request_addr);
    } else {
        c->request_addr_size = 0;
    }

    if (transport == tcp_transport && init_state == conn_new_cmd) {
        if (getpeername(sfd, (struct sockaddr *) &c->request_addr,
                        &c->request_addr_size)) {
            perror("getpeername");
            memset(&c->request_addr, 0, sizeof(c->request_addr));
        }
    }
#endif

    if (settings.verbose > 1) {
        if (IS_UDP(transport)) {
            fprintf(stderr, "<%p server listening (udp)\n", c);
        } else if (c->protocol == negotiating_prot) {
            fprintf(stderr, "<%p new auto-negotiating client connection\n",
                    c);
        } else if (c->protocol == ascii_prot) {
            fprintf(stderr, "<%p new ascii client connection.\n", c);
        } else if (c->protocol == binary_prot) {
            fprintf(stderr, "<%p new binary client connection.\n", c);
        } else {
            fprintf(stderr, "<%p new unknown (%d) client connection\n",
                c, c->protocol);
            BUG();
        }
    }

    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
    c->ileft = 0;
    c->suffixleft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->authenticated = false;
    c->last_cmd_time = current_time; /* initialize for idle kicker */
#ifdef EXTSTORE
    c->io_wraplist = NULL;
    c->io_wrapleft = 0;
#endif

    c->write_and_go = init_state;
    c->write_and_free = 0;
    c->item = 0;

    c->noreply = false;

#if 0
    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }

    STATS_LOCK();
    stats_state.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();
#endif

    MEMCACHED_CONN_ALLOCATE(c->sfd);

    return c;
}
#ifdef EXTSTORE
static void recache_or_free(conn *c, io_wrap *wrap) {
    item *it;
    it = (item *)wrap->io.buf;
    bool do_free = true;
    // If request was ultimately a miss, unlink the header.
    if (wrap->miss) {
        do_free = false;
        size_t ntotal = ITEM_ntotal(wrap->hdr_it);
        item_unlink(wrap->hdr_it);
        slabs_free(it, ntotal, slabs_clsid(ntotal));
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.miss_from_extstore++;
        if (wrap->badcrc)
            c->thread->stats.badcrc_from_extstore++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    } else if (settings.ext_recache_rate) {
        // hashvalue is cuddled during store
        uint32_t hv = (uint32_t)it->time;
        // opt to throw away rather than wait on a lock.
        void *hold_lock = item_trylock(hv);
        if (hold_lock != NULL) {
            item *h_it = wrap->hdr_it;
            uint8_t flags = ITEM_LINKED|ITEM_FETCHED|ITEM_ACTIVE;
            // Item must be recently hit at least twice to recache.
            if (((h_it->it_flags & flags) == flags) &&
                    h_it->time > current_time - ITEM_UPDATE_INTERVAL &&
                    c->recache_counter++ % settings.ext_recache_rate == 0) {
                do_free = false;
                // In case it's been updated.
                it->exptime = h_it->exptime;
                it->it_flags &= ~ITEM_LINKED;
                it->refcount = 0;
                it->h_next = NULL; // might not be necessary.
                STORAGE_delete(c->thread->storage, h_it);
                item_replace(h_it, it, hv);
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.recache_from_extstore++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
            }
        }
        if (hold_lock)
            item_trylock_unlock(hold_lock);
    }
    if (do_free)
        slabs_free(it, ITEM_ntotal(it), ITEM_clsid(it));

    wrap->io.buf = NULL; // sanity.
    wrap->io.next = NULL;
    wrap->next = NULL;
    wrap->active = false;

    // TODO: reuse lock and/or hv.
    item_remove(wrap->hdr_it);
}
#endif
static void conn_release_items(conn *c) {
    assert(c != NULL);

    if (c->item) {
        item_remove(c->item);
        c->item = 0;
    }

    while (c->ileft > 0) {
        item *it = *(c->icurr);
        assert((it->it_flags & ITEM_SLABBED) == 0);
        item_remove(it);
        c->icurr++;
        c->ileft--;
    }

    if (c->suffixleft != 0) {
        for (; c->suffixleft > 0; c->suffixleft--, c->suffixcurr++) {
            do_cache_free(mythr()->suffix_cache, *(c->suffixcurr));
        }
    }
#ifdef EXTSTORE
    if (c->io_wraplist) {
        io_wrap *tmp = c->io_wraplist;
        while (tmp) {
            io_wrap *next = tmp->next;
            recache_or_free(c, tmp);
            do_cache_free(c->thread->io_cache, tmp); // lockless
            tmp = next;
        }
        c->io_wraplist = NULL;
    }
#endif
    c->icurr = c->ilist;
    c->suffixcurr = c->suffixlist;
}


/*
 * Frees a connection.
 */
void conn_free(conn *c) {
    if (c) {
        assert(c != NULL);

        MEMCACHED_CONN_DESTROY(c);
        if (c->hdrbuf)
            free(c->hdrbuf);
        if (c->msglist)
            free(c->msglist);
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        if (c->ilist)
            free(c->ilist);
        if (c->suffixlist)
            free(c->suffixlist);
        if (c->iov)
            free(c->iov);
        BUG_ON(c->idx != -1);
        free(c);
    }
}

static void conn_tcache_free(struct tcache *tc, int nr, void **items)
{
    for (int i = 0; i < nr; i++)
        conn_free(items[i]);
}

static int udp_conn_tcache_alloc(struct tcache *tc, int nr, void **items)
{
    int i;

    for (i = 0; i < nr; i++) {
        items[i] = conn_new(conn_parse_cmd, UDP_READ_BUFFER_SIZE, udp_transport);
        if (items[i] == NULL) {
            conn_tcache_free(tc, i, items);
            return -ENOMEM;
        }
    }
    return 0;
}

static const struct tcache_ops udp_conn_tcache_ops = {
    .alloc = udp_conn_tcache_alloc, .free = conn_tcache_free,
};

static void conn_close(conn *c) {
    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "<%p connection closed.\n", c);

    conn_release_items(c);

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }

    if (c->sasl_conn) {
        assert(settings.sasl);
        sasl_dispose(&c->sasl_conn);
        c->sasl_conn = NULL;
    }

    MEMCACHED_CONN_RELEASE(c);

    if (likely(IS_UDP(c->transport))) {
        conn_set_state(c, conn_parse_cmd);
        udp_spawn_data_release(c->spawn_data->release_data);
        tcache_free(&udp_conn_tcache_pt, c);
    } else {
        mutex_lock(&tcp_conn_lock);
        active_tcp_conns[c->idx] = active_tcp_conns[--nactiveconns];
        active_tcp_conns[nactiveconns] = NULL;
        if (active_tcp_conns[c->idx])
            active_tcp_conns[c->idx]->idx = c->idx;
        c->idx = -1;
        mutex_unlock(&tcp_conn_lock);
        tcp_abort(c->tcp_conn);
        tcp_close(c->tcp_conn);
        conn_free(c);
    }

    return;
}

/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn *c) {
    assert(c != NULL);

    if (IS_UDP(c->transport))
        return;

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE) {
        char *newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);

        newbuf = (char *)realloc((void *)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf) {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ITEM_LIST_HIGHWAT) {
        item **newbuf = (item**) realloc((void *)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]));
        if (newbuf) {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT) {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf) {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
    /* TODO check return value */
    }
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state) {
    const char* const statenames[] = { "conn_listening",
                                       "conn_new_cmd",
                                       "conn_waiting",
                                       "conn_read",
                                       "conn_parse_cmd",
                                       "conn_write",
                                       "conn_nread",
                                       "conn_swallow",
                                       "conn_closing",
                                       "conn_mwrite",
                                       "conn_closed",
                                       "conn_watch" };

    return statenames[state];
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);

    if (state != c->state) {
        if (settings.verbose > 2) {
            fprintf(stderr, "%p: going from %s to %s\n",
                    c, state_text(c->state),
                    state_text(state));
        }

        if (state == conn_write || state == conn_mwrite) {
            MEMCACHED_PROCESS_COMMAND_END(c, c->wbuf, c->wbytes);
        }
        c->state = state;
    }
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 * Note: This is a hot path for at least ASCII protocol. While there is
 * redundant code in splitting TCP/UDP handling, any reduction in steps has a
 * large impact for TCP connections.
 */

static int add_iov(conn *c, const void *buf, int len) {
    struct msghdr *m;
    int leftover;

    assert(c != NULL);

    if (IS_UDP(c->transport)) {
        do {
            m = &c->msglist[c->msgused - 1];

            /*
             * Limit UDP packets to UDP_MAX_PAYLOAD_SIZE bytes.
             */

            /* We may need to start a new msghdr if this one is full. */
            if (m->msg_iovlen == IOV_MAX ||
                (c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
                add_msghdr(c);
                m = &c->msglist[c->msgused - 1];
            }

            if (ensure_iov_space(c) != 0)
                return -1;

            /* If the fragment is too big to fit in the datagram, split it up */
            if (len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
                leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
                len -= leftover;
            } else {
                leftover = 0;
            }

            m = &c->msglist[c->msgused - 1];
            m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
            m->msg_iov[m->msg_iovlen].iov_len = len;

            c->msgbytes += len;
            c->iovused++;
            m->msg_iovlen++;

            buf = ((char *)buf) + len;
            len = leftover;
        } while (leftover > 0);
    } else {
        /* Optimized path for TCP connections */
        m = &c->msglist[c->msgused - 1];
        if (m->msg_iovlen == IOV_MAX) {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;
        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;
    }

    return 0;
}

static int add_chunked_item_iovs(conn *c, item *it, int len) {
    assert(it->it_flags & ITEM_CHUNKED);
    item_chunk *ch = (item_chunk *) ITEM_data(it);
    while (ch) {
        int todo = (len > ch->used) ? ch->used : len;
        if (add_iov(c, ch->data, todo) != 0) {
            return -1;
        }
        ch = ch->next;
        len -= todo;
    }
    return 0;
}

/*
 * Constructs a set of UDP headers and attaches them to the outgoing messages.
 */
static int build_udp_headers(conn *c) {
    int i;
    unsigned char *hdr;

    assert(c != NULL);

    if (c->msgused > c->hdrsize) {
        void *new_hdrbuf;
        if (c->hdrbuf) {
            new_hdrbuf = realloc(c->hdrbuf, c->msgused * 2 * UDP_HEADER_SIZE);
        } else {
            new_hdrbuf = malloc(c->msgused * 2 * UDP_HEADER_SIZE);
        }

        if (! new_hdrbuf) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->hdrbuf = (unsigned char *)new_hdrbuf;
        c->hdrsize = c->msgused * 2;
    }

    hdr = c->hdrbuf;
    for (i = 0; i < c->msgused; i++) {
        c->msglist[i].msg_iov[0].iov_base = (void*)hdr;
        c->msglist[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;
        *hdr++ = c->request_id / 256;
        *hdr++ = c->request_id % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msgused / 256;
        *hdr++ = c->msgused % 256;
        *hdr++ = 0;
        *hdr++ = 0;
        assert((void *) hdr == (caddr_t)c->msglist[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return 0;
}


static void out_string(conn *c, const char *str) {
    size_t len;

    assert(c != NULL);

    if (c->noreply) {
        if (settings.verbose > 1)
            fprintf(stderr, ">%p NOREPLY %s\n", c, str);
        c->noreply = false;
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%p %s\n", c, str);

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}

/*
 * Outputs a protocol-specific "out of memory" error. For ASCII clients,
 * this is equivalent to out_string().
 */
static void out_of_memory(conn *c, char *ascii_error) {
    const static char error_prefix[] = "SERVER_ERROR ";
    const static int error_prefix_len = sizeof(error_prefix) - 1;

    if (c->protocol == binary_prot) {
        /* Strip off the generic error prefix; it's irrelevant in binary */
        if (!strncmp(ascii_error, error_prefix, error_prefix_len)) {
            ascii_error += error_prefix_len;
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, ascii_error, 0);
    } else {
        out_string(c, ascii_error);
    }
}

/*
 * we get here after reading the value in set/add/replace commands. The command
 * has been stored in c->cmd, and the item is ready in c->item.
 */
static void complete_nread_ascii(conn *c) {
    assert(c != NULL);

    item *it = c->item;
    int comm = c->cmd;
    enum store_item_type ret;
    bool is_valid = false;

    STATS_LOCAL_LOCK();
    mythr()->stats.slab_stats[ITEM_clsid(it)].set_cmds++;
    STATS_LOCAL_UNLOCK();

    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) == 0) {
            is_valid = true;
        }
    } else {
        char buf[2];
        /* should point to the final item chunk */
        item_chunk *ch = (item_chunk *) c->ritem;
        assert(ch->used != 0);
        /* :( We need to look at the last two bytes. This could span two
         * chunks.
         */
        if (ch->used > 1) {
            buf[0] = ch->data[ch->used - 2];
            buf[1] = ch->data[ch->used - 1];
        } else {
            assert(ch->prev);
            assert(ch->used == 1);
            buf[0] = ch->prev->data[ch->prev->used - 1];
            buf[1] = ch->data[ch->used - 1];
        }
        if (strncmp(buf, "\r\n", 2) == 0) {
            is_valid = true;
        } else {
            BUG();
        }
    }

    if (!is_valid) {
        out_string(c, "CLIENT_ERROR bad data chunk");
    } else {
      ret = store_item(it, comm, c);

#ifdef ENABLE_DTRACE
      uint64_t cas = ITEM_get_cas(it);
      switch (c->cmd) {
      case NREAD_ADD:
          MEMCACHED_COMMAND_ADD(c, ITEM_key(it), it->nkey,
                                (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_REPLACE:
          MEMCACHED_COMMAND_REPLACE(c, ITEM_key(it), it->nkey,
                                    (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_APPEND:
          MEMCACHED_COMMAND_APPEND(c, ITEM_key(it), it->nkey,
                                   (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_PREPEND:
          MEMCACHED_COMMAND_PREPEND(c, ITEM_key(it), it->nkey,
                                    (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_SET:
          MEMCACHED_COMMAND_SET(c, ITEM_key(it), it->nkey,
                                (ret == 1) ? it->nbytes : -1, cas);
          break;
      case NREAD_CAS:
          MEMCACHED_COMMAND_CAS(c, ITEM_key(it), it->nkey, it->nbytes,
                                cas);
          break;
      }
#endif

      switch (ret) {
      case STORED:
          out_string(c, "STORED");
          break;
      case EXISTS:
          out_string(c, "EXISTS");
          break;
      case NOT_FOUND:
          out_string(c, "NOT_FOUND");
          break;
      case NOT_STORED:
          out_string(c, "NOT_STORED");
          break;
      default:
          out_string(c, "SERVER_ERROR Unhandled storage type.");
      }

    }

    item_remove(c->item);       /* release the c->item reference */
    c->item = 0;
}

/**
 * get a pointer to the start of the request struct for the current command
 */
static void* binary_get_request(conn *c) {
    char *ret = c->rcurr;
    ret -= (sizeof(c->binary_header) + c->binary_header.request.keylen +
            c->binary_header.request.extlen);

    assert(ret >= c->rbuf);
    return ret;
}

/**
 * get a pointer to the key in this request
 */
static char* binary_get_key(conn *c) {
    return c->rcurr - (c->binary_header.request.keylen);
}

static void add_bin_header(conn *c, uint16_t err, uint8_t hdr_len, uint16_t key_len, uint32_t body_len) {
    protocol_binary_response_header* header;

    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        /* This should never run out of memory because iov and msg lists
         * have minimum sizes big enough to hold an error response.
         */
        out_of_memory(c, "SERVER_ERROR out of memory adding binary header");
        return;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
    header->response.keylen = (uint16_t)htons(key_len);

    header->response.extlen = (uint8_t)hdr_len;
    header->response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES;
    header->response.status = (uint16_t)htons(err);

    header->response.bodylen = htonl(body_len);
    header->response.opaque = c->opaque;
    header->response.cas = htonll(c->cas);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, ">%p Writing bin response:", c);
        for (ii = 0; ii < sizeof(header->bytes); ++ii) {
            if (ii % 4 == 0) {
                fprintf(stderr, "\n>%p  ", c);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }

    add_iov(c, c->wbuf, sizeof(header->response));
}

/**
 * Writes a binary error response. If errstr is supplied, it is used as the
 * error text; otherwise a generic description of the error status code is
 * included.
 */
static void write_bin_error(conn *c, protocol_binary_response_status err,
                            const char *errstr, int swallow) {
    size_t len;

    if (!errstr) {
        switch (err) {
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            errstr = "Unknown command";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
            errstr = "Not found";
            break;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            errstr = "Invalid arguments";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
            errstr = "Data exists for key.";
            break;
        case PROTOCOL_BINARY_RESPONSE_E2BIG:
            errstr = "Too large.";
            break;
        case PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL:
            errstr = "Non-numeric server-side value for incr or decr";
            break;
        case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
            errstr = "Not stored.";
            break;
        case PROTOCOL_BINARY_RESPONSE_AUTH_ERROR:
            errstr = "Auth failure.";
            break;
        default:
            errstr = "UNHANDLED ERROR";
            fprintf(stderr, ">%p UNHANDLED ERROR: %d\n", c, err);
            BUG();
        }
    }

    if (settings.verbose > 1) {
        fprintf(stderr, ">%p Writing an error: %s\n", c, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, 0, 0, len);
    if (len > 0) {
        add_iov(c, errstr, len);
    }
    conn_set_state(c, conn_mwrite);
    if(swallow > 0) {
        c->sbytes = swallow;
        c->write_and_go = conn_swallow;
    } else {
        c->write_and_go = conn_new_cmd;
    }
}

/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, int hlen, int keylen, int dlen) {
    if (!c->noreply || c->cmd == PROTOCOL_BINARY_CMD_GET ||
        c->cmd == PROTOCOL_BINARY_CMD_GETK) {
        add_bin_header(c, 0, hlen, keylen, dlen);
        if(dlen > 0) {
            add_iov(c, d, dlen);
        }
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
    } else {
        conn_set_state(c, conn_new_cmd);
    }
}

static void complete_incr_bin(conn *c) {
    item *it;
    char *key;
    size_t nkey;
    /* Weird magic in add_delta forces me to pad here */
    char tmpbuf[INCR_MAX_STORAGE_LEN];
    uint64_t cas = 0;

    protocol_binary_response_incr* rsp = (protocol_binary_response_incr*)c->wbuf;
    protocol_binary_request_incr* req = binary_get_request(c);

    assert(c != NULL);
    assert(c->wsize >= sizeof(*rsp));

    /* fix byteorder in the request */
    req->message.body.delta = ntohll(req->message.body.delta);
    req->message.body.initial = ntohll(req->message.body.initial);
    req->message.body.expiration = ntohl(req->message.body.expiration);
    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    if (settings.verbose > 1) {
        int i;
        fprintf(stderr, "incr ");

        for (i = 0; i < nkey; i++) {
            fprintf(stderr, "%c", key[i]);
        }
        fprintf(stderr, " %lld, %llu, %d\n",
                (long long)req->message.body.delta,
                (long long)req->message.body.initial,
                req->message.body.expiration);
    }

    if (c->binary_header.request.cas != 0) {
        cas = c->binary_header.request.cas;
    }
    switch(add_delta(c, key, nkey, c->cmd == PROTOCOL_BINARY_CMD_INCREMENT,
                     req->message.body.delta, tmpbuf,
                     &cas)) {
    case OK:
        rsp->message.body.value = htonll(strtoull(tmpbuf, NULL, 10));
        if (cas) {
            c->cas = cas;
        }
        write_bin_response(c, &rsp->message.body, 0, 0,
                           sizeof(rsp->message.body.value));
        break;
    case NON_NUMERIC:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL, NULL, 0);
        break;
    case EOM:
        out_of_memory(c, "SERVER_ERROR Out of memory incrementing value");
        break;
    case DELTA_ITEM_NOT_FOUND:
        if (req->message.body.expiration != 0xffffffff) {
            /* Save some room for the response */
            rsp->message.body.value = htonll(req->message.body.initial);

            snprintf(tmpbuf, INCR_MAX_STORAGE_LEN, "%llu",
                (unsigned long long)req->message.body.initial);
            int res = strlen(tmpbuf);
            it = item_alloc(key, nkey, 0, realtime(req->message.body.expiration),
                            res + 2);

            if (it != NULL) {
                memcpy(ITEM_data(it), tmpbuf, res);
                memcpy(ITEM_data(it) + res, "\r\n", 2);

                if (store_item(it, NREAD_ADD, c)) {
                    c->cas = ITEM_get_cas(it);
                    write_bin_response(c, &rsp->message.body, 0, 0, sizeof(rsp->message.body.value));
                } else {
                    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_NOT_STORED,
                                    NULL, 0);
                }
                item_remove(it);         /* release our reference */
            } else {
                out_of_memory(c,
                        "SERVER_ERROR Out of memory allocating new item");
            }
        } else {
            STATS_LOCAL_LOCK();
            if (c->cmd == PROTOCOL_BINARY_CMD_INCREMENT) {
                mythr()->stats.incr_misses++;
            } else {
                mythr()->stats.decr_misses++;
            }
            STATS_LOCAL_UNLOCK();

            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        }
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        break;
    }
}

static void complete_update_bin(conn *c) {
    protocol_binary_response_status eno = PROTOCOL_BINARY_RESPONSE_EINVAL;
    enum store_item_type ret = NOT_STORED;
    assert(c != NULL);

    item *it = c->item;

    STATS_LOCAL_LOCK();
    mythr()->stats.slab_stats[ITEM_clsid(it)].set_cmds++;
    STATS_LOCAL_UNLOCK();

    /* We don't actually receive the trailing two characters in the bin
     * protocol, so we're going to just set them here */
    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        *(ITEM_data(it) + it->nbytes - 2) = '\r';
        *(ITEM_data(it) + it->nbytes - 1) = '\n';
    } else {
        assert(c->ritem);
        item_chunk *ch = (item_chunk *) c->ritem;
        if (ch->size == ch->used)
            ch = ch->next;
        assert(ch->size - ch->used >= 2);
        ch->data[ch->used] = '\r';
        ch->data[ch->used + 1] = '\n';
        ch->used += 2;
    }

    ret = store_item(it, c->cmd, c);

#ifdef ENABLE_DTRACE
    uint64_t cas = ITEM_get_cas(it);
    switch (c->cmd) {
    case NREAD_ADD:
        MEMCACHED_COMMAND_ADD(c, ITEM_key(it), it->nkey,
                              (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_REPLACE:
        MEMCACHED_COMMAND_REPLACE(c, ITEM_key(it), it->nkey,
                                  (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_APPEND:
        MEMCACHED_COMMAND_APPEND(c, ITEM_key(it), it->nkey,
                                 (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_PREPEND:
        MEMCACHED_COMMAND_PREPEND(c, ITEM_key(it), it->nkey,
                                 (ret == STORED) ? it->nbytes : -1, cas);
        break;
    case NREAD_SET:
        MEMCACHED_COMMAND_SET(c, ITEM_key(it), it->nkey,
                              (ret == STORED) ? it->nbytes : -1, cas);
        break;
    }
#endif

    switch (ret) {
    case STORED:
        /* Stored */
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case EXISTS:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        break;
    case NOT_FOUND:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        break;
    case NOT_STORED:
    case TOO_LARGE:
    case NO_MEMORY:
        if (c->cmd == NREAD_ADD) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
        } else if(c->cmd == NREAD_REPLACE) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
        } else {
            eno = PROTOCOL_BINARY_RESPONSE_NOT_STORED;
        }
        write_bin_error(c, eno, NULL, 0);
    }

    item_remove(c->item);       /* release the c->item reference */
    c->item = 0;
}

static void write_bin_miss_response(conn *c, char *key, size_t nkey) {
    if (nkey) {
        char *ofs = c->wbuf + sizeof(protocol_binary_response_header);
        add_bin_header(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                0, nkey, nkey);
        memcpy(ofs, key, nkey);
        add_iov(c, ofs, nkey);
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
    } else {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                        NULL, 0);
    }
}

static void process_bin_get_or_touch(conn *c) {
    item *it;

    protocol_binary_response_get* rsp = (protocol_binary_response_get*)c->wbuf;
    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;
    int should_touch = (c->cmd == PROTOCOL_BINARY_CMD_TOUCH ||
                        c->cmd == PROTOCOL_BINARY_CMD_GAT ||
                        c->cmd == PROTOCOL_BINARY_CMD_GATK);
    int should_return_key = (c->cmd == PROTOCOL_BINARY_CMD_GETK ||
                             c->cmd == PROTOCOL_BINARY_CMD_GATK);
    int should_return_value = (c->cmd != PROTOCOL_BINARY_CMD_TOUCH);
    bool failed = false;

    if (settings.verbose > 1) {
        fprintf(stderr, "<%p %s ", c, should_touch ? "TOUCH" : "GET");
        if (fwrite(key, 1, nkey, stderr)) {}
        fputc('\n', stderr);
    }

    if (should_touch) {
        protocol_binary_request_touch *t = binary_get_request(c);
        time_t exptime = ntohl(t->message.body.expiration);

        it = item_touch(key, nkey, realtime(exptime), c);
    } else {
        it = item_get(key, nkey, c, DO_UPDATE);
    }

    if (it) {
        /* the length has two unnecessary bytes ("\r\n") */
        uint16_t keylen = 0;
        uint32_t bodylen = sizeof(rsp->message.body) + (it->nbytes - 2);

        STATS_LOCAL_LOCK();
        if (should_touch) {
            mythr()->stats.touch_cmds++;
            mythr()->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        } else {
            mythr()->stats.get_cmds++;
            mythr()->stats.lru_hits[it->slabs_clsid]++;
        }
        STATS_LOCAL_UNLOCK();

        if (should_touch) {
            MEMCACHED_COMMAND_TOUCH(c, ITEM_key(it), it->nkey,
                                    it->nbytes, ITEM_get_cas(it));
        } else {
            MEMCACHED_COMMAND_GET(c, ITEM_key(it), it->nkey,
                                  it->nbytes, ITEM_get_cas(it));
        }

        if (c->cmd == PROTOCOL_BINARY_CMD_TOUCH) {
            bodylen -= it->nbytes - 2;
        } else if (should_return_key) {
            bodylen += nkey;
            keylen = nkey;
        }

        add_bin_header(c, 0, sizeof(rsp->message.body), keylen, bodylen);
        rsp->message.header.response.cas = htonll(ITEM_get_cas(it));

        // add the flags
        if (settings.inline_ascii_response) {
            rsp->message.body.flags = htonl(strtoul(ITEM_suffix(it), NULL, 10));
        } else if (it->nsuffix > 0) {
            rsp->message.body.flags = htonl(*((uint32_t *)ITEM_suffix(it)));
        } else {
            rsp->message.body.flags = 0;
        }
        add_iov(c, &rsp->message.body, sizeof(rsp->message.body));

        if (should_return_key) {
            add_iov(c, ITEM_key(it), nkey);
        }

        if (should_return_value) {
            /* Add the data minus the CRLF */
#ifdef EXTSTORE
            if (it->it_flags & ITEM_HDR) {
                int iovcnt = 4;
                int iovst = c->iovused - 3;
                if (!should_return_key) {
                    iovcnt = 3;
                    iovst = c->iovused - 2;
                }
                // FIXME: this can return an error, but code flow doesn't
                // allow bailing here.
                if (_get_extstore(c, it, iovst, iovcnt) != 0)
                    failed = true;
            } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
                add_iov(c, ITEM_data(it), it->nbytes - 2);
            } else {
                add_chunked_item_iovs(c, it, it->nbytes - 2);
            }
#else
            if ((it->it_flags & ITEM_CHUNKED) == 0) {
                add_iov(c, ITEM_data(it), it->nbytes - 2);
            } else {
                add_chunked_item_iovs(c, it, it->nbytes - 2);
            }
#endif
        }

        if (!failed) {
            conn_set_state(c, conn_mwrite);
            c->write_and_go = conn_new_cmd;
            /* Remember this command so we can garbage collect it later */
#ifdef EXTSTORE
            if ((it->it_flags & ITEM_HDR) == 0) {
                c->item = it;
            } else {
                c->item = NULL;
            }
#else
            c->item = it;
#endif
        } else {
            item_remove(it);
        }
    } else {
        failed = true;
    }

    if (failed) {
        STATS_LOCAL_LOCK();
        if (should_touch) {
            mythr()->stats.touch_cmds++;
            mythr()->stats.touch_misses++;
        } else {
            mythr()->stats.get_cmds++;
            mythr()->stats.get_misses++;
        }
        STATS_LOCAL_UNLOCK();

        if (should_touch) {
            MEMCACHED_COMMAND_TOUCH(c, key, nkey, -1, 0);
        } else {
            MEMCACHED_COMMAND_GET(c, key, nkey, -1, 0);
        }

        if (c->noreply) {
            conn_set_state(c, conn_new_cmd);
        } else {
            if (should_return_key) {
                write_bin_miss_response(c, key, nkey);
            } else {
                write_bin_miss_response(c, NULL, 0);
            }
        }
    }

    if (settings.detail_enabled) {
        stats_prefix_record_get(key, nkey, NULL != it);
    }
}

static void append_bin_stats(const char *key, const uint16_t klen,
                             const char *val, const uint32_t vlen,
                             conn *c) {
    char *buf = c->stats.buffer + c->stats.offset;
    uint32_t bodylen = klen + vlen;
    protocol_binary_response_header header = {
        .response.magic = (uint8_t)PROTOCOL_BINARY_RES,
        .response.opcode = PROTOCOL_BINARY_CMD_STAT,
        .response.keylen = (uint16_t)htons(klen),
        .response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES,
        .response.bodylen = htonl(bodylen),
        .response.opaque = c->opaque
    };

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);

    if (klen > 0) {
        memcpy(buf, key, klen);
        buf += klen;

        if (vlen > 0) {
            memcpy(buf, val, vlen);
        }
    }

    c->stats.offset += sizeof(header.response) + bodylen;
}

static void append_ascii_stats(const char *key, const uint16_t klen,
                               const char *val, const uint32_t vlen,
                               conn *c) {
    char *pos = c->stats.buffer + c->stats.offset;
    uint32_t nbytes = 0;
    int remaining = c->stats.size - c->stats.offset;
    int room = remaining - 1;

    if (klen == 0 && vlen == 0) {
        nbytes = snprintf(pos, room, "END\r\n");
    } else if (vlen == 0) {
        nbytes = snprintf(pos, room, "STAT %s\r\n", key);
    } else {
        nbytes = snprintf(pos, room, "STAT %s %s\r\n", key, val);
    }

    c->stats.offset += nbytes;
}

static bool grow_stats_buf(conn *c, size_t needed) {
    size_t nsize = c->stats.size;
    size_t available = nsize - c->stats.offset;
    bool rv = true;

    /* Special case: No buffer -- need to allocate fresh */
    if (c->stats.buffer == NULL) {
        nsize = 1024;
        available = c->stats.size = c->stats.offset = 0;
    }

    while (needed > available) {
        assert(nsize > 0);
        nsize = nsize << 1;
        available = nsize - c->stats.offset;
    }

    if (nsize != c->stats.size) {
        char *ptr = realloc(c->stats.buffer, nsize);
        if (ptr) {
            c->stats.buffer = ptr;
            c->stats.size = nsize;
        } else {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            rv = false;
        }
    }

    return rv;
}

static void append_stats(const char *key, const uint16_t klen,
                  const char *val, const uint32_t vlen,
                  const void *cookie)
{
    /* value without a key is invalid */
    if (klen == 0 && vlen > 0) {
        return ;
    }

    conn *c = (conn*)cookie;

    if (c->protocol == binary_prot) {
        size_t needed = vlen + klen + sizeof(protocol_binary_response_header);
        if (!grow_stats_buf(c, needed)) {
            return ;
        }
        append_bin_stats(key, klen, val, vlen, c);
    } else {
        size_t needed = vlen + klen + 10; // 10 == "STAT = \r\n"
        if (!grow_stats_buf(c, needed)) {
            return ;
        }
        append_ascii_stats(key, klen, val, vlen, c);
    }

    assert(c->stats.offset <= c->stats.size);
}

static void process_bin_stat(conn *c) {
    char *subcommand = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, "<%p STATS ", c);
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", subcommand[ii]);
        }
        fprintf(stderr, "\n");
    }

    if (nkey == 0) {
        /* request all statistics */
        server_stats(&append_stats, c);
        (void)get_stats(NULL, 0, &append_stats, c);
    } else if (strncmp(subcommand, "reset", 5) == 0) {
        stats_reset();
    } else if (strncmp(subcommand, "settings", 8) == 0) {
        process_stat_settings(&append_stats, c);
    } else if (strncmp(subcommand, "detail", 6) == 0) {
        char *subcmd_pos = subcommand + 6;
        if (strncmp(subcmd_pos, " dump", 5) == 0) {
            int len;
            char *dump_buf = stats_prefix_dump(&len);
            if (dump_buf == NULL || len <= 0) {
                out_of_memory(c, "SERVER_ERROR Out of memory generating stats");
                if (dump_buf != NULL)
                    free(dump_buf);
                return;
            } else {
                append_stats("detailed", strlen("detailed"), dump_buf, len, c);
                free(dump_buf);
            }
        } else if (strncmp(subcmd_pos, " on", 3) == 0) {
            settings.detail_enabled = 1;
        } else if (strncmp(subcmd_pos, " off", 4) == 0) {
            settings.detail_enabled = 0;
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
            return;
        }
    } else {
        if (get_stats(subcommand, nkey, &append_stats, c)) {
            if (c->stats.buffer == NULL) {
                out_of_memory(c, "SERVER_ERROR Out of memory generating stats");
            } else {
                write_and_free(c, c->stats.buffer, c->stats.offset);
                c->stats.buffer = NULL;
            }
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        }

        return;
    }

    /* Append termination package and start the transfer */
    append_stats(NULL, 0, NULL, 0, c);
    if (c->stats.buffer == NULL) {
        out_of_memory(c, "SERVER_ERROR Out of memory preparing to send stats");
    } else {
        write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

static void bin_read_key(conn *c, enum bin_substates next_substate, int extra) {
    assert(c);
    c->substate = next_substate;
    c->rlbytes = c->keylen + extra;

    /* Ok... do we have room for the extras and the key in the input buffer? */
    ptrdiff_t offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
    if (c->rlbytes > c->rsize - offset) {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(protocol_binary_request_header);

        while (size > nsize) {
            nsize *= 2;
        }

        if (nsize != c->rsize) {
            if (settings.verbose > 1) {
                fprintf(stderr, "%p: Need to grow buffer from %lu to %lu\n",
                        c, (unsigned long)c->rsize, (unsigned long)nsize);
            }
            char *newm = realloc(c->rbuf, nsize);
            if (newm == NULL) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose) {
                    fprintf(stderr, "%p: Failed to grow buffer.. closing connection\n",
                            c);
                }
                conn_set_state(c, conn_closing);
                return;
            }

            c->rbuf= newm;
            /* rcurr should point to the same offset in the packet */
            c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
            c->rsize = nsize;
        }
        if (c->rbuf != c->rcurr) {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (settings.verbose > 1) {
                fprintf(stderr, "%p: Repack input buffer\n", c);
            }
        }
    }

    /* preserve the header in the buffer.. */
    c->ritem = c->rcurr + sizeof(protocol_binary_request_header);
    conn_set_state(c, conn_nread);
}

/* Just write an error message and disconnect the client */
static void handle_binary_protocol_error(conn *c) {
    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
    if (settings.verbose) {
        fprintf(stderr, "Protocol error (opcode %02x), close connection %p\n",
                c->binary_header.request.opcode, c);
    }
    c->write_and_go = conn_closing;
}

static void init_sasl_conn(conn *c) {
    assert(c);
    /* should something else be returned? */
    if (!settings.sasl)
        return;

    c->authenticated = false;

    if (!c->sasl_conn) {
        int result=sasl_server_new("memcached",
                                   NULL,
                                   my_sasl_hostname[0] ? my_sasl_hostname : NULL,
                                   NULL, NULL,
                                   NULL, 0, &c->sasl_conn);
        if (result != SASL_OK) {
            if (settings.verbose) {
                fprintf(stderr, "Failed to initialize SASL conn.\n");
            }
            c->sasl_conn = NULL;
        }
    }
}

static void bin_list_sasl_mechs(conn *c) {
    // Guard against a disabled SASL.
    if (!settings.sasl) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                        c->binary_header.request.bodylen
                        - c->binary_header.request.keylen);
        return;
    }

    init_sasl_conn(c);
    const char *result_string = NULL;
    unsigned int string_length = 0;
    int result=sasl_listmech(c->sasl_conn, NULL,
                             "",   /* What to prepend the string with */
                             " ",  /* What to separate mechanisms with */
                             "",   /* What to append to the string */
                             &result_string, &string_length,
                             NULL);
    if (result != SASL_OK) {
        /* Perhaps there's a better error for this... */
        if (settings.verbose) {
            fprintf(stderr, "Failed to list SASL mechanisms.\n");
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        return;
    }
    write_bin_response(c, (char*)result_string, 0, 0, string_length);
}

static void process_bin_sasl_auth(conn *c) {
    // Guard for handling disabled SASL on the server.
    if (!settings.sasl) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                        c->binary_header.request.bodylen
                        - c->binary_header.request.keylen);
        return;
    }

    assert(c->binary_header.request.extlen == 0);

    int nkey = c->binary_header.request.keylen;
    int vlen = c->binary_header.request.bodylen - nkey;

    if (nkey > MAX_SASL_MECH_LEN) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        c->write_and_go = conn_swallow;
        return;
    }

    char *key = binary_get_key(c);
    assert(key);

    item *it = item_alloc(key, nkey, 0, 0, vlen+2);

    /* Can't use a chunked item for SASL authentication. */
    if (it == 0 || (it->it_flags & ITEM_CHUNKED)) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, NULL, vlen);
        c->write_and_go = conn_swallow;
        return;
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_reading_sasl_auth_data;
}

static void process_bin_complete_sasl_auth(conn *c) {
    assert(settings.sasl);
    const char *out = NULL;
    unsigned int outlen = 0;

    assert(c->item);
    init_sasl_conn(c);

    int nkey = c->binary_header.request.keylen;
    int vlen = c->binary_header.request.bodylen - nkey;

    if (nkey > ((item*) c->item)->nkey) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        c->write_and_go = conn_swallow;
        item_unlink(c->item);
        return;
    }

    char mech[nkey+1];
    memcpy(mech, ITEM_key((item*)c->item), nkey);
    mech[nkey] = 0x00;

    if (settings.verbose)
        fprintf(stderr, "mech:  ``%s'' with %d bytes of data\n", mech, vlen);

    const char *challenge = vlen == 0 ? NULL : ITEM_data((item*) c->item);

    if (vlen > ((item*) c->item)->nbytes) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, vlen);
        c->write_and_go = conn_swallow;
        item_unlink(c->item);
        return;
    }

    int result=-1;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_AUTH:
        result = sasl_server_start(c->sasl_conn, mech,
                                   challenge, vlen,
                                   &out, &outlen);
        break;
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        result = sasl_server_step(c->sasl_conn,
                                  challenge, vlen,
                                  &out, &outlen);
        break;
    default:
        BUG(); /* CMD should be one of the above */
        /* This code is pretty much impossible, but makes the compiler
           happier */
        if (settings.verbose) {
            fprintf(stderr, "Unhandled command %d with challenge %s\n",
                    c->cmd, challenge);
        }
        break;
    }

    item_unlink(c->item);

    if (settings.verbose) {
        fprintf(stderr, "sasl result code:  %d\n", result);
    }

    switch(result) {
    case SASL_OK:
        c->authenticated = true;
        write_bin_response(c, "Authenticated", 0, 0, strlen("Authenticated"));
        STATS_LOCAL_LOCK();
        mythr()->stats.auth_cmds++;
        STATS_LOCAL_UNLOCK();
        break;
    case SASL_CONTINUE:
        add_bin_header(c, PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE, 0, 0, outlen);
        if(outlen > 0) {
            add_iov(c, out, outlen);
        }
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
        break;
    default:
        if (settings.verbose)
            fprintf(stderr, "Unknown sasl response:  %d\n", result);
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        STATS_LOCAL_LOCK();
        mythr()->stats.auth_cmds++;
        mythr()->stats.auth_errors++;
        STATS_LOCAL_UNLOCK();
    }
}

static bool authenticated(conn *c) {
    assert(settings.sasl);
    bool rv = false;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_AUTH:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_STEP:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_VERSION:         /* FALLTHROUGH */
        rv = true;
        break;
    default:
        rv = c->authenticated;
    }

    if (settings.verbose > 1) {
        fprintf(stderr, "authenticated() in cmd 0x%02x is %s\n",
                c->cmd, rv ? "true" : "false");
    }

    return rv;
}

static void dispatch_bin_command(conn *c) {
    int protocol_error = 0;

    uint8_t extlen = c->binary_header.request.extlen;
    uint16_t keylen = c->binary_header.request.keylen;
    uint32_t bodylen = c->binary_header.request.bodylen;

    if (keylen > bodylen || keylen + extlen > bodylen) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL, 0);
        c->write_and_go = conn_closing;
        return;
    }

    if (settings.sasl && !authenticated(c)) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        c->write_and_go = conn_closing;
        return;
    }

    MEMCACHED_PROCESS_COMMAND_START(c, c->rcurr, c->rbytes);
    c->noreply = true;

    /* binprot supports 16bit keys, but internals are still 8bit */
    if (keylen > KEY_MAX_LENGTH) {
        handle_binary_protocol_error(c);
        return;
    }

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SETQ:
        c->cmd = PROTOCOL_BINARY_CMD_SET;
        break;
    case PROTOCOL_BINARY_CMD_ADDQ:
        c->cmd = PROTOCOL_BINARY_CMD_ADD;
        break;
    case PROTOCOL_BINARY_CMD_REPLACEQ:
        c->cmd = PROTOCOL_BINARY_CMD_REPLACE;
        break;
    case PROTOCOL_BINARY_CMD_DELETEQ:
        c->cmd = PROTOCOL_BINARY_CMD_DELETE;
        break;
    case PROTOCOL_BINARY_CMD_INCREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_INCREMENT;
        break;
    case PROTOCOL_BINARY_CMD_DECREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_DECREMENT;
        break;
    case PROTOCOL_BINARY_CMD_QUITQ:
        c->cmd = PROTOCOL_BINARY_CMD_QUIT;
        break;
    case PROTOCOL_BINARY_CMD_FLUSHQ:
        c->cmd = PROTOCOL_BINARY_CMD_FLUSH;
        break;
    case PROTOCOL_BINARY_CMD_APPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_APPEND;
        break;
    case PROTOCOL_BINARY_CMD_PREPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_PREPEND;
        break;
    case PROTOCOL_BINARY_CMD_GETQ:
        c->cmd = PROTOCOL_BINARY_CMD_GET;
        break;
    case PROTOCOL_BINARY_CMD_GETKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GETK;
        break;
    case PROTOCOL_BINARY_CMD_GATQ:
        c->cmd = PROTOCOL_BINARY_CMD_GAT;
        break;
    case PROTOCOL_BINARY_CMD_GATKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GATK;
        break;
    default:
        c->noreply = false;
    }

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_VERSION:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                write_bin_response(c, VERSION, 0, 0, strlen(VERSION));
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_FLUSH:
            if (keylen == 0 && bodylen == extlen && (extlen == 0 || extlen == 4)) {
                bin_read_key(c, bin_read_flush_exptime, extlen);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_NOOP:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                write_bin_response(c, NULL, 0, 0, 0);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SET: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_ADD: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_REPLACE:
            if (extlen == 8 && keylen != 0 && bodylen >= (keylen + 8)) {
                bin_read_key(c, bin_reading_set_header, 8);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_GETQ:  /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GET:   /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GETKQ: /* FALLTHROUGH */
        case PROTOCOL_BINARY_CMD_GETK:
            if (extlen == 0 && bodylen == keylen && keylen > 0) {
                bin_read_key(c, bin_reading_get_key, 0);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_DELETE:
            if (keylen > 0 && extlen == 0 && bodylen == keylen) {
                bin_read_key(c, bin_reading_del_header, extlen);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_INCREMENT:
        case PROTOCOL_BINARY_CMD_DECREMENT:
            if (keylen > 0 && extlen == 20 && bodylen == (keylen + extlen)) {
                bin_read_key(c, bin_reading_incr_header, 20);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_APPEND:
        case PROTOCOL_BINARY_CMD_PREPEND:
            if (keylen > 0 && extlen == 0) {
                bin_read_key(c, bin_reading_set_header, 0);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_STAT:
            if (extlen == 0) {
                bin_read_key(c, bin_reading_stat, 0);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_QUIT:
            if (keylen == 0 && extlen == 0 && bodylen == 0) {
                write_bin_response(c, NULL, 0, 0, 0);
                c->write_and_go = conn_closing;
                if (c->noreply) {
                    conn_set_state(c, conn_closing);
                }
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS:
            if (extlen == 0 && keylen == 0 && bodylen == 0) {
                bin_list_sasl_mechs(c);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_SASL_AUTH:
        case PROTOCOL_BINARY_CMD_SASL_STEP:
            if (extlen == 0 && keylen != 0) {
                bin_read_key(c, bin_reading_sasl_auth, 0);
            } else {
                protocol_error = 1;
            }
            break;
        case PROTOCOL_BINARY_CMD_TOUCH:
        case PROTOCOL_BINARY_CMD_GAT:
        case PROTOCOL_BINARY_CMD_GATQ:
        case PROTOCOL_BINARY_CMD_GATK:
        case PROTOCOL_BINARY_CMD_GATKQ:
            if (extlen == 4 && keylen != 0) {
                bin_read_key(c, bin_reading_touch_key, 4);
            } else {
                protocol_error = 1;
            }
            break;
        default:
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                            bodylen);
    }

    if (protocol_error)
        handle_binary_protocol_error(c);
}

static void process_bin_update(conn *c) {
    char *key;
    int nkey;
    int vlen;
    item *it;
    protocol_binary_request_set* req = binary_get_request(c);

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    /* fix byteorder in the request */
    req->message.body.flags = ntohl(req->message.body.flags);
    req->message.body.expiration = ntohl(req->message.body.expiration);

    vlen = c->binary_header.request.bodylen - (nkey + c->binary_header.request.extlen);

    if (settings.verbose > 1) {
        int ii;
        if (c->cmd == PROTOCOL_BINARY_CMD_ADD) {
            fprintf(stderr, "<%p ADD ", c);
        } else if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            fprintf(stderr, "<%p SET ", c);
        } else {
            fprintf(stderr, "<%p REPLACE ", c);
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }

        fprintf(stderr, " Value len is %d", vlen);
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, req->message.body.flags,
            realtime(req->message.body.expiration), vlen+2);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, req->message.body.flags, vlen + 2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR Out of memory allocating item");
            /* This error generating method eats the swallow value. Add here. */
            c->sbytes = vlen;
            status = NO_MEMORY;
        }
        /* FIXME: losing c->cmd since it's translated below. refactor? */
        LOGGER_LOG(mythr()->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, 0, key, nkey, req->message.body.expiration,
                ITEM_clsid(it));

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(mythr()->storage, it);
                item_remove(it);
            }
        }

        /* swallow the data line */
        c->write_and_go = conn_swallow;
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_ADD:
            c->cmd = NREAD_ADD;
            break;
        case PROTOCOL_BINARY_CMD_SET:
            c->cmd = NREAD_SET;
            break;
        case PROTOCOL_BINARY_CMD_REPLACE:
            c->cmd = NREAD_REPLACE;
            break;
        default:
            BUG();
    }

    if (ITEM_get_cas(it) != 0) {
        c->cmd = NREAD_CAS;
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_append_prepend(conn *c) {
    char *key;
    int nkey;
    int vlen;
    item *it;

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;
    vlen = c->binary_header.request.bodylen - nkey;

    if (settings.verbose > 1) {
        fprintf(stderr, "Value len is %d\n", vlen);
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, 0, 0, vlen+2);

    if (it == 0) {
        if (! item_size_ok(nkey, 0, vlen + 2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
        } else {
            out_of_memory(c, "SERVER_ERROR Out of memory allocating item");
            /* OOM calls eat the swallow value. Add here. */
            c->sbytes = vlen;
        }
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_APPEND:
            c->cmd = NREAD_APPEND;
            break;
        case PROTOCOL_BINARY_CMD_PREPEND:
            c->cmd = NREAD_PREPEND;
            break;
        default:
            BUG();
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_flush(conn *c) {
    time_t exptime = 0;
    protocol_binary_request_flush* req = binary_get_request(c);
    rel_time_t new_oldest = 0;

    if (!settings.flush_enabled) {
      // flush_all is not allowed but we log it on stats
      write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
      return;
    }

    if (c->binary_header.request.extlen == sizeof(req->message.body)) {
        exptime = ntohl(req->message.body.expiration);
    }

    if (exptime > 0) {
        new_oldest = realtime(exptime);
    } else {
        new_oldest = current_time;
    }
    if (settings.use_cas) {
        settings.oldest_live = new_oldest - 1;
        if (settings.oldest_live <= current_time)
            settings.oldest_cas = get_cas_id();
    } else {
        settings.oldest_live = new_oldest;
    }

    STATS_LOCAL_LOCK();
    mythr()->stats.flush_cmds++;
    STATS_LOCAL_UNLOCK();

    write_bin_response(c, NULL, 0, 0, 0);
}

static void process_bin_delete(conn *c) {
    item *it;

    protocol_binary_request_delete* req = binary_get_request(c);

    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;

    assert(c != NULL);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, "Deleting ");
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get(key, nkey, c, DONT_UPDATE);
    if (it) {
        uint64_t cas = ntohll(req->message.header.request.cas);
        if (cas == 0 || cas == ITEM_get_cas(it)) {
            MEMCACHED_COMMAND_DELETE(c, ITEM_key(it), it->nkey);
            STATS_LOCAL_LOCK();
            mythr()->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
            STATS_LOCAL_UNLOCK();
            item_unlink(it);
            STORAGE_delete(mythr()->storage, it);
            write_bin_response(c, NULL, 0, 0, 0);
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        }
        item_remove(it);      /* release our reference */
    } else {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        STATS_LOCAL_LOCK();
        mythr()->stats.delete_misses++;
        STATS_LOCAL_UNLOCK();
    }
}

static void complete_nread_binary(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);

    switch(c->substate) {
    case bin_reading_set_header:
        if (c->cmd == PROTOCOL_BINARY_CMD_APPEND ||
                c->cmd == PROTOCOL_BINARY_CMD_PREPEND) {
            process_bin_append_prepend(c);
        } else {
            process_bin_update(c);
        }
        break;
    case bin_read_set_value:
        complete_update_bin(c);
        break;
    case bin_reading_get_key:
    case bin_reading_touch_key:
        process_bin_get_or_touch(c);
        break;
    case bin_reading_stat:
        process_bin_stat(c);
        break;
    case bin_reading_del_header:
        process_bin_delete(c);
        break;
    case bin_reading_incr_header:
        complete_incr_bin(c);
        break;
    case bin_read_flush_exptime:
        process_bin_flush(c);
        break;
    case bin_reading_sasl_auth:
        process_bin_sasl_auth(c);
        break;
    case bin_reading_sasl_auth_data:
        process_bin_complete_sasl_auth(c);
        break;
    default:
        fprintf(stderr, "Not handling substate %d\n", c->substate);
        BUG();
    }
}

static void reset_cmd_handler(conn *c) {
    c->cmd = -1;
    c->substate = bin_no_state;
    if(c->item != NULL) {
        item_remove(c->item);
        c->item = NULL;
    }
    conn_shrink(c);
    if (c->rbytes > 0) {
        conn_set_state(c, conn_parse_cmd);
    } else if (IS_UDP(c->transport)) {
        conn_set_state(c, conn_closing);
    } else {
        conn_set_state(c, conn_read);
    }
}

static void complete_nread(conn *c) {
    assert(c != NULL);
    assert(c->protocol == ascii_prot
           || c->protocol == binary_prot);

    if (c->protocol == ascii_prot) {
        complete_nread_ascii(c);
    } else if (c->protocol == binary_prot) {
        complete_nread_binary(c);
    }
}

/* Destination must always be chunked */
/* This should be part of item.c */
static int _store_item_copy_chunks(item *d_it, item *s_it, const int len) {
    item_chunk *dch = (item_chunk *) ITEM_data(d_it);
    /* Advance dch until we find free space */
    while (dch->size == dch->used) {
        if (dch->next) {
            dch = dch->next;
        } else {
            break;
        }
    }

    if (s_it->it_flags & ITEM_CHUNKED) {
        int remain = len;
        item_chunk *sch = (item_chunk *) ITEM_data(s_it);
        int copied = 0;
        /* Fills dch's to capacity, not straight copy sch in case data is
         * being added or removed (ie append/prepend)
         */
        while (sch && dch && remain) {
            assert(dch->used <= dch->size);
            int todo = (dch->size - dch->used < sch->used - copied)
                ? dch->size - dch->used : sch->used - copied;
            if (remain < todo)
                todo = remain;
            memcpy(dch->data + dch->used, sch->data + copied, todo);
            dch->used += todo;
            copied += todo;
            remain -= todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, remain);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
            assert(copied <= sch->used);
            if (copied == sch->used) {
                copied = 0;
                sch = sch->next;
            }
        }
        /* assert that the destination had enough space for the source */
        assert(remain == 0);
    } else {
        int done = 0;
        /* Fill dch's via a non-chunked item. */
        while (len > done && dch) {
            int todo = (dch->size - dch->used < len - done)
                ? dch->size - dch->used : len - done;
            //assert(dch->size - dch->used != 0);
            memcpy(dch->data + dch->used, ITEM_data(s_it) + done, todo);
            done += todo;
            dch->used += todo;
            assert(dch->used <= dch->size);
            if (dch->size == dch->used) {
                item_chunk *tch = do_item_alloc_chunk(dch, len - done);
                if (tch) {
                    dch = tch;
                } else {
                    return -1;
                }
            }
        }
        assert(len == done);
    }
    return 0;
}

static int _store_item_copy_data(int comm, item *old_it, item *new_it, item *add_it) {
    if (comm == NREAD_APPEND) {
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, old_it, old_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, add_it, add_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(old_it), old_it->nbytes);
            memcpy(ITEM_data(new_it) + old_it->nbytes - 2 /* CRLF */, ITEM_data(add_it), add_it->nbytes);
        }
    } else {
        /* NREAD_PREPEND */
        if (new_it->it_flags & ITEM_CHUNKED) {
            if (_store_item_copy_chunks(new_it, add_it, add_it->nbytes - 2) == -1 ||
                _store_item_copy_chunks(new_it, old_it, old_it->nbytes) == -1) {
                return -1;
            }
        } else {
            memcpy(ITEM_data(new_it), ITEM_data(add_it), add_it->nbytes);
            memcpy(ITEM_data(new_it) + add_it->nbytes - 2 /* CRLF */, ITEM_data(old_it), old_it->nbytes);
        }
    }
    return 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns the state of storage.
 */
enum store_item_type do_store_item(item *it, int comm, conn *c, const uint32_t hv) {
    char *key = ITEM_key(it);
    item *old_it = do_item_get(key, it->nkey, hv, c, DONT_UPDATE);
    enum store_item_type stored = NOT_STORED;

    item *new_it = NULL;
    uint32_t flags;

    if (old_it != NULL && comm == NREAD_ADD) {
        /* add only adds a nonexistent item, but promote to head of LRU */
        do_item_update(old_it);
    } else if (!old_it && (comm == NREAD_REPLACE
        || comm == NREAD_APPEND || comm == NREAD_PREPEND))
    {
        /* replace only replaces an existing value; don't store */
    } else if (comm == NREAD_CAS) {
        /* validate cas operation */
        if(old_it == NULL) {
            // LRU expired
            stored = NOT_FOUND;
            STATS_LOCAL_LOCK();
            mythr()->stats.cas_misses++;
            STATS_LOCAL_UNLOCK();
        }
        else if (ITEM_get_cas(it) == ITEM_get_cas(old_it)) {
            // cas validates
            // it and old_it may belong to different classes.
            // I'm updating the stats for the one that's getting pushed out
            STATS_LOCAL_LOCK();
            mythr()->stats.slab_stats[ITEM_clsid(old_it)].cas_hits++;
            STATS_LOCAL_UNLOCK();

            STORAGE_delete(mythr()->storage, old_it);
            item_replace(old_it, it, hv);
            stored = STORED;
        } else {
            STATS_LOCAL_LOCK();
            mythr()->stats.slab_stats[ITEM_clsid(old_it)].cas_badval++;
            STATS_LOCAL_UNLOCK();

            if(settings.verbose > 1) {
                fprintf(stderr, "CAS:  failure: expected %llu, got %llu\n",
                        (unsigned long long)ITEM_get_cas(old_it),
                        (unsigned long long)ITEM_get_cas(it));
            }
            stored = EXISTS;
        }
    } else {
        int failed_alloc = 0;
        /*
         * Append - combine new and old record into single one. Here it's
         * atomic and thread-safe.
         */
        if (comm == NREAD_APPEND || comm == NREAD_PREPEND) {
            /*
             * Validate CAS
             */
            if (ITEM_get_cas(it) != 0) {
                // CAS much be equal
                if (ITEM_get_cas(it) != ITEM_get_cas(old_it)) {
                    stored = EXISTS;
                }
            }
#ifdef EXTSTORE
            if ((old_it->it_flags & ITEM_HDR) != 0) {
                /* block append/prepend from working with extstore-d items.
                 * also don't replace the header with the append chunk
                 * accidentally, so mark as a failed_alloc.
                 */
                failed_alloc = 1;
            } else
#endif
            if (stored == NOT_STORED) {
                /* we have it and old_it here - alloc memory to hold both */
                /* flags was already lost - so recover them from ITEM_suffix(it) */

                if (settings.inline_ascii_response) {
                    flags = (uint32_t) strtoul(ITEM_suffix(old_it), (char **) NULL, 10);
                } else if (old_it->nsuffix > 0) {
                    flags = *((uint32_t *)ITEM_suffix(old_it));
                } else {
                    flags = 0;
                }

                new_it = do_item_alloc(key, it->nkey, flags, old_it->exptime, it->nbytes + old_it->nbytes - 2 /* CRLF */);

                /* copy data from it and old_it to new_it */
                if (new_it == NULL || _store_item_copy_data(comm, old_it, new_it, it) == -1) {
                    failed_alloc = 1;
                    stored = NOT_STORED;
                    // failed data copy, free up.
                    if (new_it != NULL)
                        item_remove(new_it);
                } else {
                    it = new_it;
                }
            }
        }

        if (stored == NOT_STORED && failed_alloc == 0) {
            if (old_it != NULL) {
                STORAGE_delete(mythr()->storage, old_it);
                item_replace(old_it, it, hv);
            } else {
                do_item_link(it, hv);
            }

            c->cas = ITEM_get_cas(it);

            stored = STORED;
        }
    }

    if (old_it != NULL)
        do_item_remove(old_it);         /* release our reference */
    if (new_it != NULL)
        do_item_remove(new_it);

    if (stored == STORED) {
        c->cas = ITEM_get_cas(it);
    }
    LOGGER_LOG(mythr()->l, LOG_MUTATIONS, LOGGER_ITEM_STORE, NULL,
            stored, comm, ITEM_key(it), it->nkey, it->exptime, ITEM_clsid(it));

    return stored;
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; /* so we don't add an extra token */
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, int bytes) {
    if (buf) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_new_cmd;
    } else {
        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
    }
}

static inline bool set_noreply_maybe(conn *c, token_t *tokens, size_t ntokens)
{
    int noreply_index = ntokens - 2;

    /*
      NOTE: this function is not the first place where we are going to
      send the reply.  We could send it instead from process_command()
      if the request line has wrong number of tokens.  However parsing
      malformed line for "noreply" option is not reliable anyway, so
      it can't be helped.
    */
    if (tokens[noreply_index].value
        && strcmp(tokens[noreply_index].value, "noreply") == 0) {
        c->noreply = true;
    }
    return c->noreply;
}

void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...) {
    char val_str[STAT_VAL_LEN];
    int vlen;
    va_list ap;

    assert(name);
    assert(add_stats);
    assert(c);
    assert(fmt);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    add_stats(name, strlen(name), val_str, vlen, c);
}

inline static void process_stats_detail(conn *c, const char *command) {
    assert(c != NULL);

    if (strcmp(command, "on") == 0) {
        settings.detail_enabled = 1;
        out_string(c, "OK");
    }
    else if (strcmp(command, "off") == 0) {
        settings.detail_enabled = 0;
        out_string(c, "OK");
    }
    else if (strcmp(command, "dump") == 0) {
        int len;
        char *stats = stats_prefix_dump(&len);
        write_and_free(c, stats, len);
    }
    else {
        out_string(c, "CLIENT_ERROR usage: stats detail on|off|dump");
    }
}

/* return server specific stats only */
static void server_stats(ADD_STAT add_stats, conn *c) {
    pid_t pid = getpid();
    rel_time_t now = current_time;

    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);
    struct slab_stats slab_stats;
    slab_stats_aggregate(&thread_stats, &slab_stats);
#ifdef EXTSTORE
    struct extstore_stats st;
#endif
#ifndef WIN32
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#endif /* !WIN32 */

    STATS_LOCK();

    APPEND_STAT("pid", "%lu", (long)pid);
    APPEND_STAT("uptime", "%u", now - ITEM_UPDATE_INTERVAL);
    APPEND_STAT("time", "%ld", now + (long)process_started);
    APPEND_STAT("version", "%s", VERSION);
    APPEND_STAT("pointer_size", "%d", (int)(8 * sizeof(void *)));

#ifndef WIN32
    append_stat("rusage_user", add_stats, c, "%ld.%06ld",
                (long)usage.ru_utime.tv_sec,
                (long)usage.ru_utime.tv_usec);
    append_stat("rusage_system", add_stats, c, "%ld.%06ld",
                (long)usage.ru_stime.tv_sec,
                (long)usage.ru_stime.tv_usec);
#endif /* !WIN32 */

    APPEND_STAT("max_connections", "%d", settings.maxconns);
    APPEND_STAT("curr_connections", "%llu", (unsigned long long)stats_state.curr_conns - 1);
    APPEND_STAT("total_connections", "%llu", (unsigned long long)stats.total_conns);
    if (settings.maxconns_fast) {
        APPEND_STAT("rejected_connections", "%llu", (unsigned long long)stats.rejected_conns);
    }
    APPEND_STAT("connection_structures", "%u", stats_state.conn_structs);
    APPEND_STAT("reserved_fds", "%u", stats_state.reserved_fds);
    APPEND_STAT("cmd_get", "%llu", (unsigned long long)thread_stats.get_cmds);
    APPEND_STAT("cmd_set", "%llu", (unsigned long long)slab_stats.set_cmds);
    APPEND_STAT("cmd_flush", "%llu", (unsigned long long)thread_stats.flush_cmds);
    APPEND_STAT("cmd_touch", "%llu", (unsigned long long)thread_stats.touch_cmds);
    APPEND_STAT("get_hits", "%llu", (unsigned long long)slab_stats.get_hits);
    APPEND_STAT("get_misses", "%llu", (unsigned long long)thread_stats.get_misses);
    APPEND_STAT("get_expired", "%llu", (unsigned long long)thread_stats.get_expired);
    APPEND_STAT("get_flushed", "%llu", (unsigned long long)thread_stats.get_flushed);
#ifdef EXTSTORE
    if (c->thread->storage) {
        APPEND_STAT("get_extstore", "%llu", (unsigned long long)thread_stats.get_extstore);
        APPEND_STAT("recache_from_extstore", "%llu", (unsigned long long)thread_stats.recache_from_extstore);
        APPEND_STAT("miss_from_extstore", "%llu", (unsigned long long)thread_stats.miss_from_extstore);
        APPEND_STAT("badcrc_from_extstore", "%llu", (unsigned long long)thread_stats.badcrc_from_extstore);
    }
#endif
    APPEND_STAT("delete_misses", "%llu", (unsigned long long)thread_stats.delete_misses);
    APPEND_STAT("delete_hits", "%llu", (unsigned long long)slab_stats.delete_hits);
    APPEND_STAT("incr_misses", "%llu", (unsigned long long)thread_stats.incr_misses);
    APPEND_STAT("incr_hits", "%llu", (unsigned long long)slab_stats.incr_hits);
    APPEND_STAT("decr_misses", "%llu", (unsigned long long)thread_stats.decr_misses);
    APPEND_STAT("decr_hits", "%llu", (unsigned long long)slab_stats.decr_hits);
    APPEND_STAT("cas_misses", "%llu", (unsigned long long)thread_stats.cas_misses);
    APPEND_STAT("cas_hits", "%llu", (unsigned long long)slab_stats.cas_hits);
    APPEND_STAT("cas_badval", "%llu", (unsigned long long)slab_stats.cas_badval);
    APPEND_STAT("touch_hits", "%llu", (unsigned long long)slab_stats.touch_hits);
    APPEND_STAT("touch_misses", "%llu", (unsigned long long)thread_stats.touch_misses);
    APPEND_STAT("auth_cmds", "%llu", (unsigned long long)thread_stats.auth_cmds);
    APPEND_STAT("auth_errors", "%llu", (unsigned long long)thread_stats.auth_errors);
    if (settings.idle_timeout) {
        APPEND_STAT("idle_kicks", "%llu", (unsigned long long)thread_stats.idle_kicks);
    }
    APPEND_STAT("bytes_read", "%llu", (unsigned long long)thread_stats.bytes_read);
    APPEND_STAT("bytes_written", "%llu", (unsigned long long)thread_stats.bytes_written);
    APPEND_STAT("limit_maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("accepting_conns", "%u", stats_state.accepting_conns);
    APPEND_STAT("listen_disabled_num", "%llu", (unsigned long long)stats.listen_disabled_num);
    APPEND_STAT("time_in_listen_disabled_us", "%llu", stats.time_in_listen_disabled_us);
    APPEND_STAT("threads", "%d", settings.num_threads);
    APPEND_STAT("conn_yields", "%llu", (unsigned long long)thread_stats.conn_yields);
    APPEND_STAT("hash_power_level", "%u", stats_state.hash_power_level);
    APPEND_STAT("hash_bytes", "%llu", (unsigned long long)stats_state.hash_bytes);
    APPEND_STAT("hash_is_expanding", "%u", stats_state.hash_is_expanding);
    if (settings.slab_reassign) {
        APPEND_STAT("slab_reassign_rescues", "%llu", stats.slab_reassign_rescues);
        APPEND_STAT("slab_reassign_chunk_rescues", "%llu", stats.slab_reassign_chunk_rescues);
        APPEND_STAT("slab_reassign_evictions_nomem", "%llu", stats.slab_reassign_evictions_nomem);
        APPEND_STAT("slab_reassign_inline_reclaim", "%llu", stats.slab_reassign_inline_reclaim);
        APPEND_STAT("slab_reassign_busy_items", "%llu", stats.slab_reassign_busy_items);
        APPEND_STAT("slab_reassign_busy_deletes", "%llu", stats.slab_reassign_busy_deletes);
        APPEND_STAT("slab_reassign_running", "%u", stats_state.slab_reassign_running);
        APPEND_STAT("slabs_moved", "%llu", stats.slabs_moved);
    }
    if (settings.lru_crawler) {
        APPEND_STAT("lru_crawler_running", "%u", stats_state.lru_crawler_running);
        APPEND_STAT("lru_crawler_starts", "%u", stats.lru_crawler_starts);
    }
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("lru_maintainer_juggles", "%llu", (unsigned long long)stats.lru_maintainer_juggles);
    }
    APPEND_STAT("malloc_fails", "%llu",
                (unsigned long long)stats.malloc_fails);
    APPEND_STAT("log_worker_dropped", "%llu", (unsigned long long)stats.log_worker_dropped);
    APPEND_STAT("log_worker_written", "%llu", (unsigned long long)stats.log_worker_written);
    APPEND_STAT("log_watcher_skipped", "%llu", (unsigned long long)stats.log_watcher_skipped);
    APPEND_STAT("log_watcher_sent", "%llu", (unsigned long long)stats.log_watcher_sent);
    STATS_UNLOCK();
#ifdef EXTSTORE
    if (c->thread->storage) {
        STATS_LOCK();
        APPEND_STAT("extstore_compact_lost", "%llu", (unsigned long long)stats.extstore_compact_lost);
        APPEND_STAT("extstore_compact_rescues", "%llu", (unsigned long long)stats.extstore_compact_rescues);
        APPEND_STAT("extstore_compact_skipped", "%llu", (unsigned long long)stats.extstore_compact_skipped);
        STATS_UNLOCK();
        extstore_get_stats(c->thread->storage, &st);
        APPEND_STAT("extstore_page_allocs", "%llu", (unsigned long long)st.page_allocs);
        APPEND_STAT("extstore_page_evictions", "%llu", (unsigned long long)st.page_evictions);
        APPEND_STAT("extstore_page_reclaims", "%llu", (unsigned long long)st.page_reclaims);
        APPEND_STAT("extstore_pages_free", "%llu", (unsigned long long)st.pages_free);
        APPEND_STAT("extstore_pages_used", "%llu", (unsigned long long)st.pages_used);
        APPEND_STAT("extstore_objects_evicted", "%llu", (unsigned long long)st.objects_evicted);
        APPEND_STAT("extstore_objects_read", "%llu", (unsigned long long)st.objects_read);
        APPEND_STAT("extstore_objects_written", "%llu", (unsigned long long)st.objects_written);
        APPEND_STAT("extstore_objects_used", "%llu", (unsigned long long)st.objects_used);
        APPEND_STAT("extstore_bytes_evicted", "%llu", (unsigned long long)st.bytes_evicted);
        APPEND_STAT("extstore_bytes_written", "%llu", (unsigned long long)st.bytes_written);
        APPEND_STAT("extstore_bytes_read", "%llu", (unsigned long long)st.bytes_read);
        APPEND_STAT("extstore_bytes_used", "%llu", (unsigned long long)st.bytes_used);
        APPEND_STAT("extstore_bytes_fragmented", "%llu", (unsigned long long)st.bytes_fragmented);
        APPEND_STAT("extstore_limit_maxbytes", "%llu", (unsigned long long)(st.page_count * st.page_size));
    }
#endif
}

static void process_stat_settings(ADD_STAT add_stats, void *c) {
    assert(add_stats);
    APPEND_STAT("maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("maxconns", "%d", settings.maxconns);
    APPEND_STAT("tcpport", "%d", settings.port);
    APPEND_STAT("udpport", "%d", settings.udpport);
    APPEND_STAT("inter", "%s", settings.inter ? settings.inter : "NULL");
    APPEND_STAT("verbosity", "%d", settings.verbose);
    APPEND_STAT("oldest", "%lu", (unsigned long)settings.oldest_live);
    APPEND_STAT("evictions", "%s", settings.evict_to_free ? "on" : "off");
    APPEND_STAT("domain_socket", "%s",
                settings.socketpath ? settings.socketpath : "NULL");
    APPEND_STAT("umask", "%o", settings.access);
    APPEND_STAT("growth_factor", "%.2f", settings.factor);
    APPEND_STAT("chunk_size", "%d", settings.chunk_size);
    APPEND_STAT("num_threads", "%d", settings.num_threads);
    APPEND_STAT("num_threads_per_udp", "%d", settings.num_threads_per_udp);
    APPEND_STAT("stat_key_prefix", "%c", settings.prefix_delimiter);
    APPEND_STAT("detail_enabled", "%s",
                settings.detail_enabled ? "yes" : "no");
    APPEND_STAT("reqs_per_event", "%d", settings.reqs_per_event);
    APPEND_STAT("cas_enabled", "%s", settings.use_cas ? "yes" : "no");
    APPEND_STAT("tcp_backlog", "%d", settings.backlog);
    APPEND_STAT("binding_protocol", "%s",
                prot_text(settings.binding_protocol));
    APPEND_STAT("auth_enabled_sasl", "%s", settings.sasl ? "yes" : "no");
    APPEND_STAT("item_size_max", "%d", settings.item_size_max);
    APPEND_STAT("maxconns_fast", "%s", settings.maxconns_fast ? "yes" : "no");
    APPEND_STAT("hashpower_init", "%d", settings.hashpower_init);
    APPEND_STAT("slab_reassign", "%s", settings.slab_reassign ? "yes" : "no");
    APPEND_STAT("slab_automove", "%d", settings.slab_automove);
    APPEND_STAT("slab_automove_ratio", "%.2f", settings.slab_automove_ratio);
    APPEND_STAT("slab_automove_window", "%u", settings.slab_automove_window);
    APPEND_STAT("slab_chunk_max", "%d", settings.slab_chunk_size_max);
    APPEND_STAT("lru_crawler", "%s", settings.lru_crawler ? "yes" : "no");
    APPEND_STAT("lru_crawler_sleep", "%d", settings.lru_crawler_sleep);
    APPEND_STAT("lru_crawler_tocrawl", "%lu", (unsigned long)settings.lru_crawler_tocrawl);
    APPEND_STAT("tail_repair_time", "%d", settings.tail_repair_time);
    APPEND_STAT("flush_enabled", "%s", settings.flush_enabled ? "yes" : "no");
    APPEND_STAT("dump_enabled", "%s", settings.dump_enabled ? "yes" : "no");
    APPEND_STAT("hash_algorithm", "%s", settings.hash_algorithm);
    APPEND_STAT("lru_maintainer_thread", "%s", settings.lru_maintainer_thread ? "yes" : "no");
    APPEND_STAT("lru_segmented", "%s", settings.lru_segmented ? "yes" : "no");
    APPEND_STAT("hot_lru_pct", "%d", settings.hot_lru_pct);
    APPEND_STAT("warm_lru_pct", "%d", settings.warm_lru_pct);
    APPEND_STAT("hot_max_factor", "%.2f", settings.hot_max_factor);
    APPEND_STAT("warm_max_factor", "%.2f", settings.warm_max_factor);
    APPEND_STAT("temp_lru", "%s", settings.temp_lru ? "yes" : "no");
    APPEND_STAT("temporary_ttl", "%u", settings.temporary_ttl);
    APPEND_STAT("idle_timeout", "%d", settings.idle_timeout);
    APPEND_STAT("watcher_logbuf_size", "%u", settings.logger_watcher_buf_size);
    APPEND_STAT("worker_logbuf_size", "%u", settings.logger_buf_size);
    APPEND_STAT("track_sizes", "%s", item_stats_sizes_status() ? "yes" : "no");
    APPEND_STAT("inline_ascii_response", "%s", settings.inline_ascii_response ? "yes" : "no");
#ifdef EXTSTORE
    APPEND_STAT("ext_item_size", "%u", settings.ext_item_size);
    APPEND_STAT("ext_item_age", "%u", settings.ext_item_age);
    APPEND_STAT("ext_low_ttl", "%u", settings.ext_low_ttl);
    APPEND_STAT("ext_recache_rate", "%u", settings.ext_recache_rate);
    APPEND_STAT("ext_wbuf_size", "%u", settings.ext_wbuf_size);
    APPEND_STAT("ext_compact_under", "%u", settings.ext_compact_under);
    APPEND_STAT("ext_drop_under", "%u", settings.ext_drop_under);
    APPEND_STAT("ext_max_frag", "%.2f", settings.ext_max_frag);
    APPEND_STAT("slab_automove_freeratio", "%.3f", settings.slab_automove_freeratio);
    APPEND_STAT("ext_drop_unread", "%s", settings.ext_drop_unread ? "yes" : "no");
#endif
}

#if 0
static void conn_to_str(const conn *c, char *buf) {
    char addr_text[MAXPATHLEN];

    if (!c) {
        strcpy(buf, "<null>");
    } else if (c->state == conn_closed) {
        strcpy(buf, "<closed>");
    } else {
        const char *protoname = "?";
        struct sockaddr_in6 local_addr;
        struct sockaddr *addr = (void *)&c->request_addr;
        int af;
        unsigned short port = 0;

        /* For listen ports and idle UDP ports, show listen address */
        if (c->state == conn_listening ||
                (IS_UDP(c->transport) &&
                 c->state == conn_read)) {
            socklen_t local_addr_len = sizeof(local_addr);

            if (getsockname(c->sfd,
                        (struct sockaddr *)&local_addr,
                        &local_addr_len) == 0) {
                addr = (struct sockaddr *)&local_addr;
            }
        }

        af = addr->sa_family;
        addr_text[0] = '\0';

        switch (af) {
            case AF_INET:
                (void) inet_ntop(af,
                        &((struct sockaddr_in *)addr)->sin_addr,
                        addr_text,
                        sizeof(addr_text) - 1);
                port = ntohs(((struct sockaddr_in *)addr)->sin_port);
                protoname = IS_UDP(c->transport) ? "udp" : "tcp";
                break;

            case AF_INET6:
                addr_text[0] = '[';
                addr_text[1] = '\0';
                if (inet_ntop(af,
                        &((struct sockaddr_in6 *)addr)->sin6_addr,
                        addr_text + 1,
                        sizeof(addr_text) - 2)) {
                    strcat(addr_text, "]");
                }
                port = ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
                protoname = IS_UDP(c->transport) ? "udp6" : "tcp6";
                break;

            case AF_UNIX:
                strncpy(addr_text,
                        ((struct sockaddr_un *)addr)->sun_path,
                        sizeof(addr_text) - 1);
                addr_text[sizeof(addr_text)-1] = '\0';
                protoname = "unix";
                break;
        }

        if (strlen(addr_text) < 2) {
            /* Most likely this is a connected UNIX-domain client which
             * has no peer socket address, but there's no portable way
             * to tell for sure.
             */
            sprintf(addr_text, "<AF %d>", af);
        }

        if (port) {
            sprintf(buf, "%s:%s:%u", protoname, addr_text, port);
        } else {
            sprintf(buf, "%s:%s", protoname, addr_text);
        }
    }
}
#endif

static void process_stats_conns(ADD_STAT add_stats, void *c) {
#if 0
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    char conn_name[MAXPATHLEN + sizeof("unix:") + sizeof("65535")];
    int klen = 0, vlen = 0;

    assert(add_stats);

    for (i = 0; i < max_fds; i++) {
        if (conns[i]) {
            /* This is safe to do unlocked because conns are never freed; the
             * worst that'll happen will be a minor inconsistency in the
             * output -- not worth the complexity of the locking that'd be
             * required to prevent it.
             */
            if (conns[i]->state != conn_closed) {
                conn_to_str(conns[i], conn_name);

                APPEND_NUM_STAT(i, "addr", "%s", conn_name);
                APPEND_NUM_STAT(i, "state", "%s",
                        state_text(conns[i]->state));
                APPEND_NUM_STAT(i, "secs_since_last_cmd", "%d",
                        current_time - conns[i]->last_cmd_time);
            }
        }
    }
#endif
}
#ifdef EXTSTORE
static void process_extstore_stats(ADD_STAT add_stats, conn *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    int klen = 0, vlen = 0;
    struct extstore_stats st;

    assert(add_stats);

    void *storage = c->thread->storage;
    extstore_get_stats(storage, &st);
    st.page_data = calloc(st.page_count, sizeof(struct extstore_page_data));
    extstore_get_page_data(storage, &st);

    for (i = 0; i < st.page_count; i++) {
        APPEND_NUM_STAT(i, "version", "%llu",
                (unsigned long long) st.page_data[i].version);
        APPEND_NUM_STAT(i, "bytes", "%llu",
                (unsigned long long) st.page_data[i].bytes_used);
        APPEND_NUM_STAT(i, "bucket", "%u",
                st.page_data[i].bucket);
    }
}
#endif
static void process_stat(conn *c, token_t *tokens, const size_t ntokens) {
    const char *subcommand = tokens[SUBCOMMAND_TOKEN].value;
    assert(c != NULL);

    if (ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    if (ntokens == 2) {
        server_stats(&append_stats, c);
        (void)get_stats(NULL, 0, &append_stats, c);
    } else if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET");
        return ;
    } else if (strcmp(subcommand, "detail") == 0) {
        /* NOTE: how to tackle detail with binary? */
        if (ntokens < 4)
            process_stats_detail(c, "");  /* outputs the error message */
        else
            process_stats_detail(c, tokens[2].value);
        /* Output already generated */
        return ;
    } else if (strcmp(subcommand, "settings") == 0) {
        process_stat_settings(&append_stats, c);
    } else if (strcmp(subcommand, "cachedump") == 0) {
        char *buf;
        unsigned int bytes, id, limit = 0;

        if (!settings.dump_enabled) {
            out_string(c, "CLIENT_ERROR stats cachedump not allowed");
            return;
        }

        if (ntokens < 5) {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }

        if (!safe_strtoul(tokens[2].value, &id) ||
            !safe_strtoul(tokens[3].value, &limit)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        if (id >= MAX_NUMBER_OF_SLAB_CLASSES) {
            out_string(c, "CLIENT_ERROR Illegal slab id");
            return;
        }

        buf = item_cachedump(id, limit, &bytes);
        write_and_free(c, buf, bytes);
        return ;
    } else if (strcmp(subcommand, "conns") == 0) {
        process_stats_conns(&append_stats, c);
#ifdef EXTSTORE
    } else if (strcmp(subcommand, "extstore") == 0) {
        process_extstore_stats(&append_stats, c);
#endif
    } else {
        /* getting here means that the subcommand is either engine specific or
           is invalid. query the engine and see. */
        if (get_stats(subcommand, strlen(subcommand), &append_stats, c)) {
            if (c->stats.buffer == NULL) {
                out_of_memory(c, "SERVER_ERROR out of memory writing stats");
            } else {
                write_and_free(c, c->stats.buffer, c->stats.offset);
                c->stats.buffer = NULL;
            }
        } else {
            out_string(c, "ERROR");
        }
        return ;
    }

    /* append terminator and start the transfer */
    append_stats(NULL, 0, NULL, 0, c);

    if (c->stats.buffer == NULL) {
        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
    } else {
        write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

/* nsuffix == 0 means use no storage for client flags */
static inline int make_ascii_get_suffix(char *suffix, item *it, bool return_cas, int nbytes) {
    char *p = suffix;
    if (!settings.inline_ascii_response) {
        *p = ' ';
        p++;
        if (it->nsuffix == 0) {
            *p = '0';
            p++;
        } else {
            p = itoa_u32(*((uint32_t *) ITEM_suffix(it)), p);
        }
        *p = ' ';
        p = itoa_u32(nbytes-2, p+1);
    } else {
        p = suffix;
    }
    if (return_cas) {
        *p = ' ';
        p = itoa_u64(ITEM_get_cas(it), p+1);
    }
    *p = '\r';
    *(p+1) = '\n';
    *(p+2) = '\0';
    return (p - suffix) + 2;
}

#define IT_REFCOUNT_LIMIT 60000
static inline item* limited_get(char *key, size_t nkey, conn *c, uint32_t exptime, bool should_touch) {
    item *it;
    if (should_touch) {
        it = item_touch(key, nkey, exptime, c);
    } else {
        it = item_get(key, nkey, c, DO_UPDATE);
    }
    if (it && it->refcount > IT_REFCOUNT_LIMIT) {
        item_remove(it);
        it = NULL;
    }
    return it;
}

static inline int _ascii_get_expand_ilist(conn *c, int i) {
    if (i >= c->isize) {
        item **new_list = realloc(c->ilist, sizeof(item *) * c->isize * 2);
        if (new_list) {
            c->isize *= 2;
            c->ilist = new_list;
        } else {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
    }
    return 0;
}

static inline char *_ascii_get_suffix_buf(conn *c, int i) {
    char *suffix;
    /* Goofy mid-flight realloc. */
    if (i >= c->suffixsize) {
    char **new_suffix_list = realloc(c->suffixlist,
                           sizeof(char *) * c->suffixsize * 2);
    if (new_suffix_list) {
        c->suffixsize *= 2;
        c->suffixlist  = new_suffix_list;
    } else {
        STATS_LOCK();
        stats.malloc_fails++;
        STATS_UNLOCK();
        return NULL;
    }
    }

    suffix = do_cache_alloc(mythr()->suffix_cache);
    if (suffix == NULL) {
      STATS_LOCK();
      stats.malloc_fails++;
      STATS_UNLOCK();
      out_of_memory(c, "SERVER_ERROR out of memory making CAS suffix");
      return NULL;
    }
    *(c->suffixlist + i) = suffix;
    return suffix;
}
#ifdef EXTSTORE
// FIXME: This runs in the IO thread. to get better IO performance this should
// simply mark the io wrapper with the return value and decrement wrapleft, if
// zero redispatching. Still a bit of work being done in the side thread but
// minimized at least.
static void _get_extstore_cb(void *e, obj_io *io, int ret) {
    // FIXME: assumes success
    io_wrap *wrap = (io_wrap *)io->data;
    conn *c = wrap->c;
    assert(wrap->active == true);
    item *read_it = (item *)io->buf;
    bool miss = false;

    // TODO: How to do counters for hit/misses?
    if (ret < 1) {
        miss = true;
    } else {
        uint32_t crc2;
        uint32_t crc = (uint32_t) read_it->exptime;
        int x;
        // item is chunked, crc the iov's
        if (io->iov != NULL) {
            // first iov is the header, which we don't use beyond crc
            crc2 = crc32c(0, (char *)io->iov[0].iov_base+32, io->iov[0].iov_len-32);
            // make sure it's not sent. hack :(
            io->iov[0].iov_len = 0;
            for (x = 1; x < io->iovcnt; x++) {
                crc2 = crc32c(crc2, (char *)io->iov[x].iov_base, io->iov[x].iov_len);
            }
        } else {
            crc2 = crc32c(0, (char *)read_it+32, io->len-32);
        }

        if (crc != crc2) {
            miss = true;
            wrap->badcrc = true;
        }
    }

    if (miss) {
        int i;
        struct iovec *v;
        // TODO: This should be movable to the worker thread.
        if (c->protocol == binary_prot) {
            protocol_binary_response_header *header =
                (protocol_binary_response_header *)c->wbuf;
            // this zeroes out the iovecs since binprot never stacks them.
            if (header->response.keylen) {
                write_bin_miss_response(c, ITEM_key(wrap->hdr_it), wrap->hdr_it->nkey);
            } else {
                write_bin_miss_response(c, 0, 0);
            }
        } else {
            for (i = 0; i < wrap->iovec_count; i++) {
                v = &c->iov[wrap->iovec_start + i];
                v->iov_len = 0;
                v->iov_base = NULL;
            }
        }
        wrap->miss = true;
    } else {
        assert(read_it->slabs_clsid != 0);
        // kill \r\n for binprot
        if (io->iov == NULL) {
            c->iov[wrap->iovec_data].iov_base = ITEM_data(read_it);
            if (c->protocol == binary_prot)
                c->iov[wrap->iovec_data].iov_len -= 2;
        } else {
            // FIXME: Might need to go back and ensure chunked binprots don't
            // ever span two chunks for the final \r\n
            if (c->protocol == binary_prot) {
                if (io->iov[io->iovcnt-1].iov_len >= 2) {
                    io->iov[io->iovcnt-1].iov_len -= 2;
                } else {
                    io->iov[io->iovcnt-1].iov_len = 0;
                    io->iov[io->iovcnt-2].iov_len -= 1;
                }
            }
        }
        // iov_len is already set
        // TODO: Should do that here instead and cuddle in the wrap object
    }
    c->io_wrapleft--;
    wrap->active = false;
    //assert(c->io_wrapleft >= 0);

    // All IO's have returned, lets re-attach this connection to our original
    // thread.
    if (c->io_wrapleft == 0) {
        assert(c->io_queued == true);
        c->io_queued = false;
        redispatch_conn(c);
    }
}

// FIXME: This completely breaks UDP support.
static inline int _get_extstore(conn *c, item *it, int iovst, int iovcnt) {
    item_hdr *hdr = (item_hdr *)ITEM_data(it);
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid = slabs_clsid(ntotal);
    item *new_it;
    bool chunked = false;
    if (ntotal > settings.slab_chunk_size_max) {
        // Pull a chunked item header.
        // FIXME: make a func. used in several places.
        uint32_t flags;
        if (settings.inline_ascii_response) {
            flags = (uint32_t) strtoul(ITEM_suffix(it), (char **) NULL, 10);
        } else if (it->nsuffix > 0) {
            flags = *((uint32_t *)ITEM_suffix(it));
        } else {
            flags = 0;
        }

        new_it = item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, it->nbytes);
        assert(new_it == NULL || (new_it->it_flags & ITEM_CHUNKED));
        chunked = true;
    } else {
        new_it = do_item_alloc_pull(ntotal, clsid);
    }
    if (new_it == NULL)
        return -1;
    assert(!c->io_queued); // FIXME: debugging.
    // so we can free the chunk on a miss
    new_it->slabs_clsid = clsid;

    io_wrap *io = do_cache_alloc(c->thread->io_cache);
    io->active = true;
    io->miss = false;
    io->badcrc = false;
    // io_wrap owns the reference for this object now.
    io->hdr_it = it;

    // FIXME: error handling.
    // The offsets we'll wipe on a miss.
    io->iovec_start = iovst;
    io->iovec_count = iovcnt;
    // This is probably super dangerous. keep it at 0 and fill into wrap
    // object?
    if (chunked) {
        unsigned int ciovcnt = 1;
        size_t remain = new_it->nbytes;
        item_chunk *chunk = (item_chunk *) ITEM_data(new_it);
        io->io.iov = &c->iov[c->iovused];
        // fill the header so we can get the full data + crc back.
        add_iov(c, new_it, ITEM_ntotal(new_it) - new_it->nbytes);
        while (remain > 0) {
            chunk = do_item_alloc_chunk(chunk, remain);
            // TODO: counter bump
            if (chunk == NULL) {
                item_remove(new_it);
                do_cache_free(c->thread->io_cache, io);
                return -1;
            }
            add_iov(c, chunk->data, (remain < chunk->size) ? remain : chunk->size);
            chunk->used = (remain < chunk->size) ? remain : chunk->size;
            remain -= chunk->size;
            ciovcnt++;
        }
        io->io.iovcnt = ciovcnt;
        // header object was already accounted for, remove one from total
        io->iovec_count += ciovcnt-1;
    } else {
        io->io.iov = NULL;
        io->iovec_data = c->iovused;
        add_iov(c, "", it->nbytes);
    }
    io->io.buf = (void *)new_it;
    // The offset we'll fill in on a hit.
    io->c = c;
    // We need to stack the sub-struct IO's together as well.
    if (c->io_wraplist) {
        io->io.next = &c->io_wraplist->io;
    } else {
        io->io.next = NULL;
    }
    // IO queue for this connection.
    io->next = c->io_wraplist;
    c->io_wraplist = io;
    assert(c->io_wrapleft >= 0);
    c->io_wrapleft++;
    // reference ourselves for the callback.
    io->io.data = (void *)io;

    // Now, fill in io->io based on what was in our header.
    io->io.page_version = hdr->page_version;
    io->io.page_id = hdr->page_id;
    io->io.offset = hdr->offset;
    io->io.len = ntotal;
    io->io.mode = OBJ_IO_READ;
    io->io.cb = _get_extstore_cb;

    //fprintf(stderr, "EXTSTORE: IO stacked %u\n", io->iovec_data);
    // FIXME: This stat needs to move to reflect # of flash hits vs misses
    // for now it's a good gauge on how often we request out to flash at
    // least.
    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.get_extstore++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    return 0;
}
#endif
// FIXME: the 'breaks' around memory malloc's should break all the way down,
// fill ileft/suffixleft, then run conn_releaseitems()
/* ntokens is overwritten here... shrug.. */
static inline void process_get_command(conn *c, token_t *tokens, size_t ntokens, bool return_cas, bool should_touch) {
    char *key;
    size_t nkey;
    int i = 0;
    int si = 0;
    item *it;
    token_t *key_token = &tokens[KEY_TOKEN];
    char *suffix;
    int32_t exptime_int = 0;
    rel_time_t exptime = 0;
    assert(c != NULL);

    if (should_touch) {
        // For get and touch commands, use first token as exptime
        if (!safe_strtol(tokens[1].value, &exptime_int)) {
            out_string(c, "CLIENT_ERROR invalid exptime argument");
            return;
        }
        key_token++;
        exptime = realtime(exptime_int);
    }

    do {
        while(key_token->length != 0) {

            key = key_token->value;
            nkey = key_token->length;

            if (nkey > KEY_MAX_LENGTH) {
                out_string(c, "CLIENT_ERROR bad command line format");
                while (i-- > 0) {
                    item_remove(*(c->ilist + i));
                    if (return_cas || !settings.inline_ascii_response) {
                        do_cache_free(mythr()->suffix_cache, *(c->suffixlist + i));
                    }
                }
                return;
            }

            it = limited_get(key, nkey, c, exptime, should_touch);
            if (settings.detail_enabled) {
                stats_prefix_record_get(key, nkey, NULL != it);
            }
            if (it) {
                if (_ascii_get_expand_ilist(c, i) != 0) {
                    item_remove(it);
                    break; // FIXME: Should bail down to error.
                }

                /*
                 * Construct the response. Each hit adds three elements to the
                 * outgoing data list:
                 *   "VALUE "
                 *   key
                 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
                 */

                if (return_cas || !settings.inline_ascii_response)
                {
                  MEMCACHED_COMMAND_GET(c, ITEM_key(it), it->nkey,
                                        it->nbytes, ITEM_get_cas(it));
                  int nbytes;
                  suffix = _ascii_get_suffix_buf(c, si);
                  if (suffix == NULL) {
                      item_remove(it);
                      break;
                  }
                  si++;
                  nbytes = it->nbytes;
                  int suffix_len = make_ascii_get_suffix(suffix, it, return_cas, nbytes);
                  if (add_iov(c, "VALUE ", 6) != 0 ||
                      add_iov(c, ITEM_key(it), it->nkey) != 0 ||
                      (settings.inline_ascii_response && add_iov(c, ITEM_suffix(it), it->nsuffix - 2) != 0) ||
                      add_iov(c, suffix, suffix_len) != 0)
                      {
                          item_remove(it);
                          break;
                      }
#ifdef EXTSTORE
                  if (it->it_flags & ITEM_HDR) {
                      if (_get_extstore(c, it, c->iovused-3, 4) != 0) {
                          item_remove(it);
                          break;
                      }
                  } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
#else
                  if ((it->it_flags & ITEM_CHUNKED) == 0) {
#endif
                      add_iov(c, ITEM_data(it), it->nbytes);
                  } else if (add_chunked_item_iovs(c, it, it->nbytes) != 0) {
                      item_remove(it);
                      break;
                  }
                }
                else
                {
                  MEMCACHED_COMMAND_GET(c, ITEM_key(it), it->nkey,
                                        it->nbytes, ITEM_get_cas(it));
                  if (add_iov(c, "VALUE ", 6) != 0 ||
                      add_iov(c, ITEM_key(it), it->nkey) != 0)
                      {
                          item_remove(it);
                          break;
                      }
                  if ((it->it_flags & ITEM_CHUNKED) == 0)
                      {
                          if (add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) != 0)
                          {
                              item_remove(it);
                              break;
                          }
                      } else if (add_iov(c, ITEM_suffix(it), it->nsuffix) != 0 ||
                                 add_chunked_item_iovs(c, it, it->nbytes) != 0) {
                          item_remove(it);
                          break;
                      }
                }


                if (settings.verbose > 1) {
                    int ii;
                    fprintf(stderr, ">%p sending key ", c);
                    for (ii = 0; ii < it->nkey; ++ii) {
                        fprintf(stderr, "%c", key[ii]);
                    }
                    fprintf(stderr, "\n");
                }

                /* item_get() has incremented it->refcount for us */
                STATS_LOCAL_LOCK();
                if (should_touch) {
                    mythr()->stats.touch_cmds++;
                    mythr()->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
                } else {
                    mythr()->stats.lru_hits[it->slabs_clsid]++;
                    mythr()->stats.get_cmds++;
                }
                STATS_LOCAL_UNLOCK();
#ifdef EXTSTORE
                /* If ITEM_HDR, an io_wrap owns the reference. */
                if ((it->it_flags & ITEM_HDR) == 0) {
                    *(c->ilist + i) = it;
                    i++;
                }
#else
                *(c->ilist + i) = it;
                i++;
#endif
            } else {
                STATS_LOCAL_LOCK();
                if (should_touch) {
                    mythr()->stats.touch_cmds++;
                    mythr()->stats.touch_misses++;
                } else {
                    mythr()->stats.get_misses++;
                    mythr()->stats.get_cmds++;
                }
                MEMCACHED_COMMAND_GET(c, key, nkey, -1, 0);
                STATS_LOCAL_UNLOCK();
            }

            key_token++;
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens.
         */
        if(key_token->value != NULL) {
            ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
        }

    } while(key_token->value != NULL);

    c->icurr = c->ilist;
    c->ileft = i;
    if (return_cas || !settings.inline_ascii_response) {
        c->suffixcurr = c->suffixlist;
        c->suffixleft = si;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%p END\n", c);

    /*
        If the loop was terminated because of out-of-memory, it is not
        reliable to add END\r\n to the buffer, because it might not end
        in \r\n. So we send SERVER_ERROR instead.
    */
    if (key_token->value != NULL || add_iov(c, "END\r\n", 5) != 0
        || (IS_UDP(c->transport) && build_udp_headers(c) != 0)) {
        out_of_memory(c, "SERVER_ERROR out of memory writing get response");
    }
    else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }
}

static void process_update_command(conn *c, token_t *tokens, const size_t ntokens, int comm, bool handle_cas) {
    char *key;
    size_t nkey;
    unsigned int flags;
    int32_t exptime_int = 0;
    time_t exptime;
    int vlen;
    uint64_t req_cas_id=0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (! (safe_strtoul(tokens[2].value, (uint32_t *)&flags)
           && safe_strtol(tokens[3].value, &exptime_int)
           && safe_strtol(tokens[4].value, (int32_t *)&vlen))) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    /* Ubuntu 8.04 breaks when I pass exptime to safe_strtol */
    exptime = exptime_int;

    /* Negative exptimes can underflow and end up immortal. realtime() will
       immediately expire values that are greater than REALTIME_MAXDELTA, but less
       than process_started, so lets aim for that. */
    if (exptime < 0)
        exptime = REALTIME_MAXDELTA + 1;

    // does cas value exist?
    if (handle_cas) {
        if (!safe_strtoull(tokens[5].value, &req_cas_id)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
    }

    if (vlen < 0 || vlen > (INT_MAX - 2)) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    vlen += 2;

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, flags, realtime(exptime), vlen);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, flags, vlen)) {
            out_string(c, "SERVER_ERROR object too large for cache");
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR out of memory storing object");
            status = NO_MEMORY;
        }
        LOGGER_LOG(mythr()->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, comm, key, nkey, 0, 0);
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        c->sbytes = vlen;

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (comm == NREAD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(mythr()->storage, it);
                item_remove(it);
            }
        }

        return;
    }
    ITEM_set_cas(it, req_cas_id);

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = it->nbytes;
    c->cmd = comm;
    conn_set_state(c, conn_nread);
}

static void process_touch_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int32_t exptime_int = 0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtol(tokens[2].value, &exptime_int)) {
        out_string(c, "CLIENT_ERROR invalid exptime argument");
        return;
    }

    it = item_touch(key, nkey, realtime(exptime_int), c);
    if (it) {
        STATS_LOCAL_LOCK();
        mythr()->stats.touch_cmds++;
        mythr()->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        STATS_LOCAL_UNLOCK();

        out_string(c, "TOUCHED");
        item_remove(it);
    } else {
        STATS_LOCAL_LOCK();
        mythr()->stats.touch_cmds++;
        mythr()->stats.touch_misses++;
        STATS_LOCAL_UNLOCK();

        out_string(c, "NOT_FOUND");
    }
}

static void process_arithmetic_command(conn *c, token_t *tokens, const size_t ntokens, const bool incr) {
    char temp[INCR_MAX_STORAGE_LEN];
    uint64_t delta;
    char *key;
    size_t nkey;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtoull(tokens[2].value, &delta)) {
        out_string(c, "CLIENT_ERROR invalid numeric delta argument");
        return;
    }

    switch(add_delta(c, key, nkey, incr, delta, temp, NULL)) {
    case OK:
        out_string(c, temp);
        break;
    case NON_NUMERIC:
        out_string(c, "CLIENT_ERROR cannot increment or decrement non-numeric value");
        break;
    case EOM:
        out_of_memory(c, "SERVER_ERROR out of memory");
        break;
    case DELTA_ITEM_NOT_FOUND:
        STATS_LOCAL_LOCK();
        if (incr) {
            mythr()->stats.incr_misses++;
        } else {
            mythr()->stats.decr_misses++;
        }
        STATS_LOCAL_UNLOCK();

        out_string(c, "NOT_FOUND");
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        break; /* Should never get here */
    }
}

/*
 * adds a delta value to a numeric item.
 *
 * c     connection requesting the operation
 * it    item to adjust
 * incr  true to increment value, false to decrement
 * delta amount to adjust value by
 * buf   buffer for response string
 *
 * returns a response string to send back to the client.
 */
enum delta_result_type do_add_delta(conn *c, const char *key, const size_t nkey,
                                    const bool incr, const int64_t delta,
                                    char *buf, uint64_t *cas,
                                    const uint32_t hv) {
    char *ptr;
    uint64_t value;
    int res;
    item *it;

    it = do_item_get(key, nkey, hv, c, DONT_UPDATE);
    if (!it) {
        return DELTA_ITEM_NOT_FOUND;
    }

    /* Can't delta zero byte values. 2-byte are the "\r\n" */
    /* Also can't delta for chunked items. Too large to be a number */
#ifdef EXTSTORE
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED|ITEM_HDR)) != 0) {
#else
    if (it->nbytes <= 2 || (it->it_flags & (ITEM_CHUNKED)) != 0) {
#endif
        return NON_NUMERIC;
    }

    if (cas != NULL && *cas != 0 && ITEM_get_cas(it) != *cas) {
        do_item_remove(it);
        return DELTA_ITEM_CAS_MISMATCH;
    }

    ptr = ITEM_data(it);

    if (!safe_strtoull(ptr, &value)) {
        do_item_remove(it);
        return NON_NUMERIC;
    }

    if (incr) {
        value += delta;
        MEMCACHED_COMMAND_INCR(c, ITEM_key(it), it->nkey, value);
    } else {
        if(delta > value) {
            value = 0;
        } else {
            value -= delta;
        }
        MEMCACHED_COMMAND_DECR(c, ITEM_key(it), it->nkey, value);
    }

    STATS_LOCAL_LOCK();
    if (incr) {
        mythr()->stats.slab_stats[ITEM_clsid(it)].incr_hits++;
    } else {
        mythr()->stats.slab_stats[ITEM_clsid(it)].decr_hits++;
    }
    STATS_LOCAL_UNLOCK();

    snprintf(buf, INCR_MAX_STORAGE_LEN, "%llu", (unsigned long long)value);
    res = strlen(buf);
    /* refcount == 2 means we are the only ones holding the item, and it is
     * linked. We hold the item's lock in this function, so refcount cannot
     * increase. */
    if (res + 2 <= it->nbytes && it->refcount == 2) { /* replace in-place */
        /* When changing the value without replacing the item, we
           need to update the CAS on the existing item. */
        /* We also need to fiddle it in the sizes tracker in case the tracking
         * was enabled at runtime, since it relies on the CAS value to know
         * whether to remove an item or not. */
        item_stats_sizes_remove(it);
        ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
        item_stats_sizes_add(it);
        memcpy(ITEM_data(it), buf, res);
        memset(ITEM_data(it) + res, ' ', it->nbytes - res - 2);
        do_item_update(it);
    } else if (it->refcount > 1) {
        item *new_it;
        uint32_t flags;
        if (settings.inline_ascii_response) {
            flags = (uint32_t) strtoul(ITEM_suffix(it), (char **) NULL, 10);
        } else if (it->nsuffix > 0) {
            flags = *((uint32_t *)ITEM_suffix(it));
        } else {
            flags = 0;
        }
        new_it = do_item_alloc(ITEM_key(it), it->nkey, flags, it->exptime, res + 2);
        if (new_it == 0) {
            do_item_remove(it);
            return EOM;
        }
        memcpy(ITEM_data(new_it), buf, res);
        memcpy(ITEM_data(new_it) + res, "\r\n", 2);
        item_replace(it, new_it, hv);
        // Overwrite the older item's CAS with our new CAS since we're
        // returning the CAS of the old item below.
        ITEM_set_cas(it, (settings.use_cas) ? ITEM_get_cas(new_it) : 0);
        do_item_remove(new_it);       /* release our reference */
    } else {
        /* Should never get here. This means we somehow fetched an unlinked
         * item. TODO: Add a counter? */
        if (settings.verbose) {
            fprintf(stderr, "Tried to do incr/decr on invalid item\n");
        }
        if (it->refcount == 1)
            do_item_remove(it);
        return DELTA_ITEM_NOT_FOUND;
    }

    if (cas) {
        *cas = ITEM_get_cas(it);    /* swap the incoming CAS value */
    }
    do_item_remove(it);         /* release our reference */
    return OK;
}

static void process_delete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;

    assert(c != NULL);

    if (ntokens > 3) {
        bool hold_is_zero = strcmp(tokens[KEY_TOKEN+1].value, "0") == 0;
        bool sets_noreply = set_noreply_maybe(c, tokens, ntokens);
        bool valid = (ntokens == 4 && (hold_is_zero || sets_noreply))
            || (ntokens == 5 && hold_is_zero && sets_noreply);
        if (!valid) {
            out_string(c, "CLIENT_ERROR bad command line format.  "
                       "Usage: delete <key> [noreply]");
            return;
        }
    }


    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get(key, nkey, c, DONT_UPDATE);
    if (it) {
        MEMCACHED_COMMAND_DELETE(c, ITEM_key(it), it->nkey);

        STATS_LOCAL_LOCK();
        mythr()->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
        STATS_LOCAL_UNLOCK();

        item_unlink(it);
        STORAGE_delete(mythr()->storage, it);
        item_remove(it);      /* release our reference */
        out_string(c, "DELETED");
    } else {
        STATS_LOCAL_LOCK();
        mythr()->stats.delete_misses++;
        STATS_LOCAL_UNLOCK();

        out_string(c, "NOT_FOUND");
    }
}

static void process_verbosity_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    level = strtoul(tokens[1].value, NULL, 10);
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

#ifdef MEMCACHED_DEBUG
static void process_misbehave_command(conn *c) {
    int allowed = 0;

    // try opening new TCP socket
    int i = socket(AF_INET, SOCK_STREAM, 0);
    if (i != -1) {
        allowed++;
        close(i);
    }

    // try executing new commands
    i = system("sleep 0");
    if (i != -1) {
        allowed++;
    }

    if (allowed) {
        out_string(c, "ERROR");
    } else {
        out_string(c, "OK");
    }
}
#endif

static void process_slabs_automove_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;
    double ratio;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[2].value, "ratio") == 0) {
        if (ntokens < 5 || !safe_strtod(tokens[3].value, &ratio)) {
            out_string(c, "ERROR");
            return;
        }
        settings.slab_automove_ratio = ratio;
    } else {
        level = strtoul(tokens[2].value, NULL, 10);
        if (level == 0) {
            settings.slab_automove = 0;
        } else if (level == 1 || level == 2) {
            settings.slab_automove = level;
        } else {
            out_string(c, "ERROR");
            return;
        }
    }
    out_string(c, "OK");
    return;
}

/* TODO: decide on syntax for sampling? */
static void process_watch_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint16_t f = 0;
    int x;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);
    if (ntokens > 2) {
        for (x = COMMAND_TOKEN + 1; x < ntokens - 1; x++) {
            if ((strcmp(tokens[x].value, "rawcmds") == 0)) {
                f |= LOG_RAWCMDS;
            } else if ((strcmp(tokens[x].value, "evictions") == 0)) {
                f |= LOG_EVICTIONS;
            } else if ((strcmp(tokens[x].value, "fetchers") == 0)) {
                f |= LOG_FETCHERS;
            } else if ((strcmp(tokens[x].value, "mutations") == 0)) {
                f |= LOG_MUTATIONS;
            } else if ((strcmp(tokens[x].value, "sysevents") == 0)) {
                f |= LOG_SYSEVENTS;
            } else {
                out_string(c, "ERROR");
                return;
            }
        }
    } else {
        f |= LOG_FETCHERS;
    }

    switch(logger_add_watcher(c, f)) {
        case LOGGER_ADD_WATCHER_TOO_MANY:
            out_string(c, "WATCHER_TOO_MANY log watcher limit reached");
            break;
        case LOGGER_ADD_WATCHER_FAILED:
            out_string(c, "WATCHER_FAILED failed to add log watcher");
            break;
        case LOGGER_ADD_WATCHER_OK:
            conn_set_state(c, conn_watch);
            break;
    }
}

static void process_memlimit_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t memlimit;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (!safe_strtoul(tokens[1].value, &memlimit)) {
        out_string(c, "ERROR");
    } else {
        if (memlimit < 8) {
            out_string(c, "MEMLIMIT_TOO_SMALL cannot set maxbytes to less than 8m");
        } else {
            if (memlimit > 1000000000) {
                out_string(c, "MEMLIMIT_ADJUST_FAILED input value is megabytes not bytes");
            } else if (slabs_adjust_mem_limit((size_t) memlimit * 1024 * 1024)) {
                if (settings.verbose > 0) {
                    fprintf(stderr, "maxbytes adjusted to %llum\n", (unsigned long long)memlimit);
                }

                out_string(c, "OK");
            } else {
                out_string(c, "MEMLIMIT_ADJUST_FAILED out of bounds or unable to adjust");
            }
        }
    }
}

static void process_lru_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t pct_hot;
    uint32_t pct_warm;
    double hot_factor;
    int32_t ttl;
    double factor;

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[1].value, "tune") == 0 && ntokens >= 7) {
        if (!safe_strtoul(tokens[2].value, &pct_hot) ||
            !safe_strtoul(tokens[3].value, &pct_warm) ||
            !safe_strtod(tokens[4].value, &hot_factor) ||
            !safe_strtod(tokens[5].value, &factor)) {
            out_string(c, "ERROR");
        } else {
            if (pct_hot + pct_warm > 80) {
                out_string(c, "ERROR hot and warm pcts must not exceed 80");
            } else if (factor <= 0 || hot_factor <= 0) {
                out_string(c, "ERROR hot/warm age factors must be greater than 0");
            } else {
                settings.hot_lru_pct = pct_hot;
                settings.warm_lru_pct = pct_warm;
                settings.hot_max_factor = hot_factor;
                settings.warm_max_factor = factor;
                out_string(c, "OK");
            }
        }
    } else if (strcmp(tokens[1].value, "mode") == 0 && ntokens >= 3 &&
               settings.lru_maintainer_thread) {
        if (strcmp(tokens[2].value, "flat") == 0) {
            settings.lru_segmented = false;
            out_string(c, "OK");
        } else if (strcmp(tokens[2].value, "segmented") == 0) {
            settings.lru_segmented = true;
            out_string(c, "OK");
        } else {
            out_string(c, "ERROR");
        }
    } else if (strcmp(tokens[1].value, "temp_ttl") == 0 && ntokens >= 3 &&
               settings.lru_maintainer_thread) {
        if (!safe_strtol(tokens[2].value, &ttl)) {
            out_string(c, "ERROR");
        } else {
            if (ttl < 0) {
                settings.temp_lru = false;
            } else {
                settings.temp_lru = true;
                settings.temporary_ttl = ttl;
            }
            out_string(c, "OK");
        }
    } else {
        out_string(c, "ERROR");
    }
}
#ifdef EXTSTORE
static void process_extstore_command(conn *c, token_t *tokens, const size_t ntokens) {
    set_noreply_maybe(c, tokens, ntokens);
    bool ok = true;
    if (ntokens < 4) {
        ok = false;
    } else if (strcmp(tokens[1].value, "free_memchunks") == 0 && ntokens > 4) {
        /* per-slab-class free chunk setting. */
        unsigned int clsid = 0;
        unsigned int limit = 0;
        if (!safe_strtoul(tokens[2].value, &clsid) ||
                !safe_strtoul(tokens[3].value, &limit)) {
            ok = false;
        } else {
            if (clsid < MAX_NUMBER_OF_SLAB_CLASSES) {
                settings.ext_free_memchunks[clsid] = limit;
            } else {
                ok = false;
            }
        }
    } else if (strcmp(tokens[1].value, "item_size") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_size))
            ok = false;
    } else if (strcmp(tokens[1].value, "item_age") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_age))
            ok = false;
    } else if (strcmp(tokens[1].value, "low_ttl") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_low_ttl))
            ok = false;
    } else if (strcmp(tokens[1].value, "recache_rate") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_recache_rate))
            ok = false;
    } else if (strcmp(tokens[1].value, "compact_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_compact_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_drop_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "max_frag") == 0) {
        if (!safe_strtod(tokens[2].value, &settings.ext_max_frag))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_unread") == 0) {
        unsigned int v;
        if (!safe_strtoul(tokens[2].value, &v)) {
            ok = false;
        } else {
            settings.ext_drop_unread = v == 0 ? false : true;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        out_string(c, "ERROR");
    } else {
        out_string(c, "OK");
    }
}
#endif
static void process_command(conn *c, char *command) {

    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int comm;

    assert(c != NULL);

    MEMCACHED_PROCESS_COMMAND_START(c, c->rcurr, c->rbytes);

    if (settings.verbose > 1)
        fprintf(stderr, "<%p %s\n", c, command);


    /*
     * for commands set/add/replace, we build an item and read the data
     * directly into it, then continue in nread_complete().
     */

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        out_of_memory(c, "SERVER_ERROR out of memory preparing response");
        return;
    }

    ntokens = tokenize_command(command, tokens, MAX_TOKENS);
    if (ntokens >= 3 &&
        ((strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) ||
         (strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0))) {

        process_get_command(c, tokens, ntokens, false, false);

    } else if ((ntokens == 6 || ntokens == 7) &&
               ((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (comm = NREAD_ADD)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (comm = NREAD_SET)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (comm = NREAD_REPLACE)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0 && (comm = NREAD_PREPEND)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 && (comm = NREAD_APPEND)) )) {

        process_update_command(c, tokens, ntokens, comm, false);

    } else if ((ntokens == 7 || ntokens == 8) && (strcmp(tokens[COMMAND_TOKEN].value, "cas") == 0 && (comm = NREAD_CAS))) {

        process_update_command(c, tokens, ntokens, comm, true);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 1);

    } else if (ntokens >= 3 && (strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0)) {

        process_get_command(c, tokens, ntokens, true, false);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 0);

    } else if (ntokens >= 3 && ntokens <= 5 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {

        process_delete_command(c, tokens, ntokens);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "touch") == 0)) {

        process_touch_command(c, tokens, ntokens);

    } else if (ntokens >= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "gat") == 0)) {

        process_get_command(c, tokens, ntokens, false, true);

    } else if (ntokens >= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "gats") == 0)) {

        process_get_command(c, tokens, ntokens, true, true);

    } else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {

        process_stat(c, tokens, ntokens);

    } else if (ntokens >= 2 && ntokens <= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
        time_t exptime = 0;
        rel_time_t new_oldest = 0;

        set_noreply_maybe(c, tokens, ntokens);

        STATS_LOCAL_LOCK();
        mythr()->stats.flush_cmds++;
        STATS_LOCAL_UNLOCK();

        if (!settings.flush_enabled) {
            // flush_all is not allowed but we log it on stats
            out_string(c, "CLIENT_ERROR flush_all not allowed");
            return;
        }

        if (ntokens != (c->noreply ? 3 : 2)) {
            exptime = strtol(tokens[1].value, NULL, 10);
            if(errno == ERANGE) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
        }

        /*
          If exptime is zero realtime() would return zero too, and
          realtime(exptime) - 1 would overflow to the max unsigned
          value.  So we process exptime == 0 the same way we do when
          no delay is given at all.
        */
        if (exptime > 0) {
            new_oldest = realtime(exptime);
        } else { /* exptime == 0 */
            new_oldest = current_time;
        }

        if (settings.use_cas) {
            settings.oldest_live = new_oldest - 1;
            if (settings.oldest_live <= current_time)
                settings.oldest_cas = get_cas_id();
        } else {
            settings.oldest_live = new_oldest;
        }
        out_string(c, "OK");
        return;

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {

        out_string(c, "VERSION " VERSION);

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {

        conn_set_state(c, conn_closing);

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "shutdown") == 0)) {

        if (settings.shutdown_command) {
            conn_set_state(c, conn_closing);
            raise(SIGINT);
        } else {
            out_string(c, "ERROR: shutdown not enabled");
        }

    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0) {
        if (ntokens == 5 && strcmp(tokens[COMMAND_TOKEN + 1].value, "reassign") == 0) {
            int src, dst, rv;

            if (settings.slab_reassign == false) {
                out_string(c, "CLIENT_ERROR slab reassignment disabled");
                return;
            }

            src = strtol(tokens[2].value, NULL, 10);
            dst = strtol(tokens[3].value, NULL, 10);

            if (errno == ERANGE) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }

            rv = slabs_reassign(src, dst);
            switch (rv) {
            case REASSIGN_OK:
                out_string(c, "OK");
                break;
            case REASSIGN_RUNNING:
                out_string(c, "BUSY currently processing reassign request");
                break;
            case REASSIGN_BADCLASS:
                out_string(c, "BADCLASS invalid src or dst class id");
                break;
            case REASSIGN_NOSPARE:
                out_string(c, "NOSPARE source class has no spare pages");
                break;
            case REASSIGN_SRC_DST_SAME:
                out_string(c, "SAME src and dst class are identical");
                break;
            }
            return;
        } else if (ntokens >= 4 &&
            (strcmp(tokens[COMMAND_TOKEN + 1].value, "automove") == 0)) {
            process_slabs_automove_command(c, tokens, ntokens);
        } else {
            out_string(c, "ERROR");
        }
    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "lru_crawler") == 0) {
        if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "crawl") == 0) {
            int rv;
            if (settings.lru_crawler == false) {
                out_string(c, "CLIENT_ERROR lru crawler disabled");
                return;
            }

            rv = lru_crawler_crawl(tokens[2].value, CRAWLER_EXPIRED, NULL,
                    settings.lru_crawler_tocrawl);
            switch(rv) {
            case CRAWLER_OK:
                out_string(c, "OK");
                break;
            case CRAWLER_RUNNING:
                out_string(c, "BUSY currently processing crawler request");
                break;
            case CRAWLER_BADCLASS:
                out_string(c, "BADCLASS invalid class id");
                break;
            case CRAWLER_NOTSTARTED:
                out_string(c, "NOTSTARTED no items to crawl");
                break;
            case CRAWLER_ERROR:
                out_string(c, "ERROR an unknown error happened");
                break;
            }
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "metadump") == 0) {
            if (settings.lru_crawler == false) {
                out_string(c, "CLIENT_ERROR lru crawler disabled");
                return;
            }
            if (!settings.dump_enabled) {
                out_string(c, "ERROR metadump not allowed");
                return;
            }

            int rv = lru_crawler_crawl(tokens[2].value, CRAWLER_METADUMP,
                    c, LRU_CRAWLER_CAP_REMAINING);
            switch(rv) {
                case CRAWLER_OK:
                    out_string(c, "OK");
                    // TODO: Don't reuse conn_watch here.
                    conn_set_state(c, conn_watch);
                    break;
                case CRAWLER_RUNNING:
                    out_string(c, "BUSY currently processing crawler request");
                    break;
                case CRAWLER_BADCLASS:
                    out_string(c, "BADCLASS invalid class id");
                    break;
                case CRAWLER_NOTSTARTED:
                    out_string(c, "NOTSTARTED no items to crawl");
                    break;
                case CRAWLER_ERROR:
                    out_string(c, "ERROR an unknown error happened");
                    break;
            }
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "tocrawl") == 0) {
            uint32_t tocrawl;
             if (!safe_strtoul(tokens[2].value, &tocrawl)) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            settings.lru_crawler_tocrawl = tocrawl;
            out_string(c, "OK");
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "sleep") == 0) {
            uint32_t tosleep;
            if (!safe_strtoul(tokens[2].value, &tosleep)) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            if (tosleep > 1000000) {
                out_string(c, "CLIENT_ERROR sleep must be one second or less");
                return;
            }
            settings.lru_crawler_sleep = tosleep;
            out_string(c, "OK");
            return;
        } else if (ntokens == 3) {
            if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "enable") == 0)) {
                if (start_item_crawler_thread() == 0) {
                    out_string(c, "OK");
                } else {
                    out_string(c, "ERROR failed to start lru crawler thread");
                }
            } else if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "disable") == 0)) {
                if (stop_item_crawler_thread() == 0) {
                    out_string(c, "OK");
                } else {
                    out_string(c, "ERROR failed to stop lru crawler thread");
                }
            } else {
                out_string(c, "ERROR");
            }
            return;
        } else {
            out_string(c, "ERROR");
        }
    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "watch") == 0) {
        process_watch_command(c, tokens, ntokens);
    } else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "cache_memlimit") == 0)) {
        process_memlimit_command(c, tokens, ntokens);
    } else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {
        process_verbosity_command(c, tokens, ntokens);
    } else if (ntokens >= 3 && strcmp(tokens[COMMAND_TOKEN].value, "lru") == 0) {
        process_lru_command(c, tokens, ntokens);
#ifdef MEMCACHED_DEBUG
    // commands which exist only for testing the memcached's security protection
    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "misbehave") == 0)) {
        process_misbehave_command(c);
#endif
#ifdef EXTSTORE
    } else if (ntokens >= 3 && strcmp(tokens[COMMAND_TOKEN].value, "extstore") == 0) {
        process_extstore_command(c, tokens, ntokens);
#endif
    } else {
        if (ntokens >= 2 && strncmp(tokens[ntokens - 2].value, "HTTP/", 5) == 0) {
            conn_set_state(c, conn_closing);
        } else {
            out_string(c, "ERROR");
        }
    }
    return;
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c) {
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if (c->protocol == negotiating_prot || c->transport == udp_transport)  {
        if ((unsigned char)c->rbuf[0] == (unsigned char)PROTOCOL_BINARY_REQ) {
            c->protocol = binary_prot;
        } else {
            c->protocol = ascii_prot;
        }

        if (settings.verbose > 1) {
            fprintf(stderr, "%p: Client using the %s protocol\n", c,
                    prot_text(c->protocol));
        }
    }

    if (c->protocol == binary_prot) {
        /* Do we have the complete packet header? */
        if (c->rbytes < sizeof(c->binary_header)) {
            /* need more data! */
            return 0;
        } else {
#ifdef NEED_ALIGN
            if (((long)(c->rcurr)) % 8 != 0) {
                /* must realign input buffer */
                memmove(c->rbuf, c->rcurr, c->rbytes);
                c->rcurr = c->rbuf;
                if (settings.verbose > 1) {
                    fprintf(stderr, "%p: Realign input buffer\n", c);
                }
            }
#endif
            protocol_binary_request_header* req;
            req = (protocol_binary_request_header*)c->rcurr;

            if (settings.verbose > 1) {
                /* Dump the packet before we convert it to host order */
                int ii;
                fprintf(stderr, "<%p Read binary protocol data:", c);
                for (ii = 0; ii < sizeof(req->bytes); ++ii) {
                    if (ii % 4 == 0) {
                        fprintf(stderr, "\n<%p   ", c);
                    }
                    fprintf(stderr, " 0x%02x", req->bytes[ii]);
                }
                fprintf(stderr, "\n");
            }

            c->binary_header = *req;
            c->binary_header.request.keylen = ntohs(req->request.keylen);
            c->binary_header.request.bodylen = ntohl(req->request.bodylen);
            c->binary_header.request.cas = ntohll(req->request.cas);

            if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ) {
                if (settings.verbose) {
                    fprintf(stderr, "Invalid magic:  %x\n",
                            c->binary_header.request.magic);
                }
                conn_set_state(c, conn_closing);
                return -1;
            }

            c->msgcurr = 0;
            c->msgused = 0;
            c->iovused = 0;
            if (add_msghdr(c) != 0) {
                out_of_memory(c,
                        "SERVER_ERROR Out of memory allocating headers");
                return 0;
            }

            c->cmd = c->binary_header.request.opcode;
            c->keylen = c->binary_header.request.keylen;
            c->opaque = c->binary_header.request.opaque;
            /* clear the returned cas value */
            c->cas = 0;
            c->last_cmd_time = current_time;

            dispatch_bin_command(c);

            c->rbytes -= sizeof(c->binary_header);
            c->rcurr += sizeof(c->binary_header);
        }
    } else {
        char *el, *cont;

        if (c->rbytes == 0)
            return 0;

        el = memchr(c->rcurr, '\n', c->rbytes);
        if (!el) {
            if (c->rbytes > 1024) {
                /*
                 * We didn't have a '\n' in the first k. This _has_ to be a
                 * large multiget, if not we should just nuke the connection.
                 */
                char *ptr = c->rcurr;
                while (*ptr == ' ') { /* ignore leading whitespaces */
                    ++ptr;
                }

                if (ptr - c->rcurr > 100 ||
                    (strncmp(ptr, "get ", 4) && strncmp(ptr, "gets ", 5))) {

                    conn_set_state(c, conn_closing);
                    return 1;
                }
            }

            return 0;
        }
        cont = el + 1;
        if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
            el--;
        }
        *el = '\0';

        assert(cont <= (c->rcurr + c->rbytes));

        c->last_cmd_time = current_time;
        process_command(c, c->rcurr);

        c->rbytes -= (cont - c->rcurr);
        c->rcurr = cont;

        assert(c->rcurr <= (c->rbuf + c->rsize));
    }

    return 1;
}

#if 0
/*
 * read a UDP request.
 */
static enum try_read_result try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, (struct sockaddr *)&c->request_addr,
                   &c->request_addr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}
#endif

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */
static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    ssize_t res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        if (c->rbytes >= c->rsize) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose > 0) {
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }
        int avail = c->rsize - c->rbytes;
        res = tcp_read(c->tcp_conn, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            STATS_LOCAL_LOCK();
            mythr()->stats.bytes_read += res;
            STATS_LOCAL_UNLOCK();
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res <= 0) {
            return READ_ERROR;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}

#if 0
static bool update_event(conn *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept) {
    conn *next;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, settings.backlog) != 0) {
                perror("listen");
            }
        }
        else {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0) {
                perror("listen");
            }
        }
    }

    if (do_accept) {
        struct timeval maxconns_exited;
        uint64_t elapsed_us;
        gettimeofday(&maxconns_exited,NULL);
        STATS_LOCK();
        elapsed_us =
            (maxconns_exited.tv_sec - stats.maxconns_entered.tv_sec) * 1000000
            + (maxconns_exited.tv_usec - stats.maxconns_entered.tv_usec);
        stats.time_in_listen_disabled_us += elapsed_us;
        stats_state.accepting_conns = true;
        STATS_UNLOCK();
    } else {
        STATS_LOCK();
        stats_state.accepting_conns = false;
        gettimeofday(&stats.maxconns_entered,NULL);
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}
#endif

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c) {
    assert(c != NULL);

    if (c->msgcurr < c->msgused &&
            c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];
        if (IS_UDP(c->transport))
            res = udp_respondv(m->msg_iov, m->msg_iovlen, c->spawn_data);
        else
            res = tcp_writev(c->tcp_conn, m->msg_iov, m->msg_iovlen);
        if (res > 0) {
            STATS_LOCAL_LOCK();
            mythr()->stats.bytes_written += res;
            STATS_LOCAL_UNLOCK();

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }

        if (res == -105)
            return TRANSMIT_SOFT_ERROR;

        conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}

/* Does a looped read to fill data chunks */
/* TODO: restrict number of times this can loop.
 * Also, benchmark using readv's.
 */
static int read_into_chunked_item(conn *c) {
    int total = 0;
    int res;
    assert(c->rcurr != c->ritem);

    while (c->rlbytes > 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        assert(ch->used <= ch->size);
        if (ch->size == ch->used) {
            // FIXME: ch->next is currently always 0. remove this?
            if (ch->next) {
                c->ritem = (char *) ch->next;
            } else {
                /* Allocate next chunk. Binary protocol needs 2b for \r\n */
                c->ritem = (char *) do_item_alloc_chunk(ch, c->rlbytes +
                       ((c->protocol == binary_prot) ? 2 : 0));
                if (!c->ritem) {
                    // We failed an allocation. Let caller handle cleanup.
                    total = -2;
                    break;
                }
                // ritem has new chunk, restart the loop.
                continue;
                //assert(c->rlbytes == 0);
            }
        }

        int unused = ch->size - ch->used;
        /* first check if we have leftovers in the conn_read buffer */
        if (c->rbytes > 0) {
            total = 0;
            int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
            tocopy = tocopy > unused ? unused : tocopy;
            if (c->ritem != c->rcurr) {
                memmove(ch->data + ch->used, c->rcurr, tocopy);
            }
            total += tocopy;
            c->rlbytes -= tocopy;
            c->rcurr += tocopy;
            c->rbytes -= tocopy;
            ch->used += tocopy;
            if (c->rlbytes == 0) {
                break;
            }
        } else {
            /*  now try reading from the socket */
            if (IS_UDP(c->transport)) {
                res = 0;
            } else {
                res = tcp_read(c->tcp_conn, ch->data + ch->used,
                                    (unused > c->rlbytes ? c->rlbytes : unused));
            }
            if (res > 0) {
                STATS_LOCAL_LOCK();
                mythr()->stats.bytes_read += res;
                STATS_LOCAL_UNLOCK();
                ch->used += res;
                total += res;
                c->rlbytes -= res;
            } else {
                /* Reset total to the latest result so caller can handle it */
                total = res;
                break;
            }
        }
    }

    /* At some point I will be able to ditch the \r\n from item storage and
       remove all of these kludges.
       The above binprot check ensures inline space for \r\n, but if we do
       exactly enough allocs there will be no additional chunk for \r\n.
     */
    if (c->rlbytes == 0 && c->protocol == binary_prot && total >= 0) {
        item_chunk *ch = (item_chunk *)c->ritem;
        if (ch->size - ch->used < 2) {
            c->ritem = (char *) do_item_alloc_chunk(ch, 2);
            if (!c->ritem) {
                total = -2;
            }
        }
    }
    return total;
}

void drive_machine(void *arg) {
    conn *c = arg;
    bool stop = false;
    int nreqs = settings.reqs_per_event;
    int res;

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            BUG();
#if 0
            addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
            if (use_accept4) {
                sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
            } else {
                sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
            }
#else
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
            if (sfd == -1) {
                if (use_accept4 && errno == ENOSYS) {
                    use_accept4 = 0;
                    continue;
                }
                perror(use_accept4 ? "accept4()" : "accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (!use_accept4) {
                if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
                    perror("setting O_NONBLOCK");
                    close(sfd);
                    break;
                }
            }

            if (settings.maxconns_fast &&
                stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
                str = "ERROR Too many open connections\r\n";
                res = write(sfd, str, strlen(str));
                close(sfd);
                STATS_LOCK();
                stats.rejected_conns++;
                STATS_UNLOCK();
            } else {
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                     DATA_BUFFER_SIZE, c->transport);
            }

            stop = true;
            break;
#endif

        case conn_waiting:
            BUG();
#if 0
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;
#endif

        case conn_read:
            //printf("CONN_READ: res: %d\n", res);
            BUG_ON(IS_UDP(c->transport));
            res = try_read_network(c);
            switch (res) {
            case READ_NO_DATA_RECEIVED:
                break;
            case READ_DATA_RECEIVED:
                conn_set_state(c, conn_parse_cmd);
                break;
            case READ_ERROR:
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd :
            if (try_read_command(c) == 0) {
                /* wee need more data! */
                if (IS_UDP(c->transport))
                    conn_set_state(c, conn_closing);
                else
                    conn_set_state(c, conn_read);
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other
               connections */

            --nreqs;
            if (nreqs >= 0) {
                reset_cmd_handler(c);
            } else {
                STATS_LOCAL_LOCK();
                mythr()->stats.conn_yields++;
#ifdef DBG
                th_yield1++;
#endif
                STATS_LOCAL_UNLOCK();
                thread_yield();
                nreqs = settings.reqs_per_event;
                //stop = true;
            }
            break;

        case conn_nread:
            if (c->rlbytes == 0) {
                complete_nread(c);
                break;
            }

            /* Check if rbytes < 0, to prevent crash */
            if (c->rlbytes < 0) {
                if (settings.verbose) {
                    fprintf(stderr, "Invalid rlbytes to read: len %d\n", c->rlbytes);
                }
                conn_set_state(c, conn_closing);
                break;
            }

            if (!c->item || (((item *)c->item)->it_flags & ITEM_CHUNKED) == 0) {
                /* first check if we have leftovers in the conn_read buffer */
                if (c->rbytes > 0) {
                    int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                    if (c->ritem != c->rcurr) {
                        memmove(c->ritem, c->rcurr, tocopy);
                    }
                    c->ritem += tocopy;
                    c->rlbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    if (c->rlbytes == 0) {
                        break;
                    }
                }
                /*  now try reading from the socket */
                if (IS_UDP(c->transport)) {
                    res = 0;
                } else {
                    res = tcp_read(c->tcp_conn, c->ritem, c->rlbytes);
                }
                if (res > 0) {
                    STATS_LOCAL_LOCK();
                    mythr()->stats.bytes_read += res;
                    STATS_LOCAL_UNLOCK();
                    if (c->rcurr == c->ritem) {
                        c->rcurr += res;
                    }
                    c->ritem += res;
                    c->rlbytes -= res;
                    break;
                }
            } else {
                res = read_into_chunked_item(c);
                if (res > 0)
                    break;
            }

            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }

            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }

            /* Memory allocation failure */
            if (res == -2) {
                out_of_memory(c, "SERVER_ERROR Out of memory during read");
                c->sbytes = c->rlbytes;
                c->write_and_go = conn_swallow;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0) {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                        "errno: %d %s \n"
                        "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                        errno, strerror(errno),
                        (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                        (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes <= 0) {
                conn_set_state(c, conn_new_cmd);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iovused == 0 || (IS_UDP(c->transport) && c->iovused == 1)) {
                if (add_iov(c, c->wcurr, c->wbytes) != 0) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
#ifdef EXTSTORE
            /* have side IO's that must process before transmit() can run.
             * remove the connection from the worker thread and dispatch the
             * IO queue
             */
            if (c->io_wrapleft) {
                assert(c->io_queued == false);
                assert(c->io_wraplist != NULL);
                // TODO: create proper state for this condition
                conn_set_state(c, conn_watch);
                event_del(&c->event);
                c->io_queued = true;
                extstore_submit(c->thread->storage, &c->io_wraplist->io);
                stop = true;
                break;
            }
#endif
          if (IS_UDP(c->transport) && c->msgcurr == 0 && build_udp_headers(c) != 0) {
            if (settings.verbose > 0)
              fprintf(stderr, "Failed to build UDP headers\n");
            conn_set_state(c, conn_closing);
            break;
          }
            switch (transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    conn_release_items(c);
                    /* XXX:  I don't know why this wasn't the general case */
                    if(c->protocol == binary_prot) {
                        conn_set_state(c, c->write_and_go);
                    } else {
                        conn_set_state(c, conn_new_cmd);
                    }
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_SOFT_ERROR:
#ifdef DBG
                th_yield2++;
#endif
                thread_yield();
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */
            }
            break;

        case conn_closing:
            stop = true;
            break;

        case conn_closed:
            /* This only happens if dormando is an idiot. */
            BUG();
            break;

        case conn_watch:
            /* We handed off our connection to the logger thread. */
            stop = true;
            return;

        case conn_max_state:
            BUG();
            break;
        }
    }

    BUG_ON(c->state != conn_closing);
    conn_close(c);

    return;
}

static void handle_tcp_conn(void *arg)
{
    //printf("HANDLING TCP Connection!!\n");
    tcpconn_t *tconn = arg;
    conn *c = conn_new(conn_read, DATA_BUFFER_SIZE, tcp_transport);
    if (!c)
        goto abort_conn;

    c->last_cmd_time = current_time;
    c->tcp_conn = tconn;


    mutex_lock(&tcp_conn_lock);
    if (nactiveconns == settings.maxconns) {
        mutex_unlock(&tcp_conn_lock);
        log_err("MAX CONNS REACHED");
        conn_free(c);
        goto abort_conn;
    }

    c->idx = nactiveconns++;
    active_tcp_conns[c->idx] = c;
    mutex_unlock(&tcp_conn_lock);

    drive_machine(c);

    return;

abort_conn:
    tcp_abort(tconn);
    tcp_close(tconn);
}

static void udp_handler(struct udp_spawn_data *d) {
    conn *c;

    size_t res = d->len;

    if (res <= 8 || (c = tcache_alloc(&udp_conn_tcache_pt)) == NULL) {
        udp_spawn_data_release(d->release_data);
        return;
    }

    c->spawn_data = d;

    BUG_ON(res > c->rsize);
    memcpy(c->rbuf, d->buf, res);

    unsigned char *buf = (unsigned char *)c->rbuf;
    STATS_LOCAL_LOCK();
    mythr()->stats.bytes_read += res;
    STATS_LOCAL_UNLOCK();

    /* Beginning of UDP packet is the request ID; save it. */
    c->request_id = buf[0] * 256 + buf[1];

    /* If this is a multi-packet request, drop it. */
    if (buf[4] != 0 || buf[5] != 1) {
        out_string(c, "SERVER_ERROR multi-packet request not supported");
    }

    /* Don't care about any of the rest of the header. */
    res -= 8;
    memmove(c->rbuf, c->rbuf + 8, res);

    c->rbytes = res;
    c->rcurr = c->rbuf;

    drive_machine(c);
}

#if 0
void event_handler(const int fd, const short which, void *arg) {
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
        if (settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP / UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(const char *interface,
                         int port,
                         enum network_transport transport,
                         FILE *portnumber_file) {
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
                              .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;

    hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error= getaddrinfo(interface, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
          perror("getaddrinfo()");
        return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE) {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(EX_OSERR);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                perror("setsockopt");
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (IS_UDP(transport)) {
            maximize_sndbuf(sfd);
        } else {
            error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
            if (error != 0)
                perror("setsockopt");

            error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
            if (error != 0)
                perror("setsockopt");
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        } else {
            success++;
            if (!IS_UDP(transport) && listen(sfd, settings.backlog) == -1) {
                perror("listen()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(portnumber_file, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(portnumber_file, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (IS_UDP(transport)) {
            int c;

            for (c = 0; c < settings.num_threads_per_udp; c++) {
                /* Allocate one UDP file descriptor per worker thread;
                 * this allows "stats conns" to separately list multiple
                 * parallel UDP requests in progress.
                 *
                 * The dispatch code round-robins new connection requests
                 * among threads, so this is guaranteed to assign one
                 * FD to each thread.
                 */
                int per_thread_fd = c ? dup(sfd) : sfd;
                dispatch_conn_new(per_thread_fd, conn_read,
                                  EV_READ | EV_PERSIST,
                                  UDP_READ_BUFFER_SIZE, transport);
            }
        } else {
            if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base))) {
                fprintf(stderr, "failed to create listening connection\n");
                exit(EXIT_FAILURE);
            }
            listen_conn_add->next = listen_conn;
            listen_conn = listen_conn_add;
        }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static int server_sockets(int port, enum network_transport transport,
                          FILE *portnumber_file) {
    if (settings.inter == NULL) {
        return server_socket(settings.inter, port, transport, portnumber_file);
    } else {
        // tokenize them and bind to each one of them..
        char *b;
        int ret = 0;
        char *list = strdup(settings.inter);

        if (list == NULL) {
            fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
            return 1;
        }
        for (char *p = strtok_r(list, ";,", &b);
             p != NULL;
             p = strtok_r(NULL, ";,", &b)) {
            int the_port = port;

            char *h = NULL;
            if (*p == '[') {
                // expecting it to be an IPv6 address enclosed in []
                // i.e. RFC3986 style recommended by RFC5952
                char *e = strchr(p, ']');
                if (e == NULL) {
                    fprintf(stderr, "Invalid IPV6 address: \"%s\"", p);
                    free(list);
                    return 1;
                }
                h = ++p; // skip the opening '['
                *e = '\0';
                p = ++e; // skip the closing ']'
            }

            char *s = strchr(p, ':');
            if (s != NULL) {
                // If no more semicolons - attempt to treat as port number.
                // Otherwise the only valid option is an unenclosed IPv6 without port, until
                // of course there was an RFC3986 IPv6 address previously specified -
                // in such a case there is no good option, will just send it to fail as port number.
                if (strchr(s + 1, ':') == NULL || h != NULL) {
                    *s = '\0';
                    ++s;
                    if (!safe_strtol(s, &the_port)) {
                        fprintf(stderr, "Invalid port number: \"%s\"", s);
                        free(list);
                        return 1;
                    }
                }
            }

            if (h != NULL)
                p = h;

            if (strcmp(p, "*") == 0) {
                p = NULL;
            }
            ret |= server_socket(p, the_port, transport, portnumber_file);
        }
        free(list);
        return ret;
    }
}

static int new_socket_unix(void) {
    int sfd;
    int flags;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}

static int server_socket_unix(const char *path, int access_mask) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    struct stat tstat;
    int flags =1;
    int old_umask;

    if (!path) {
        return 1;
    }

    if ((sfd = new_socket_unix()) == -1) {
        return 1;
    }

    /*
     * Clean up a previous socket file if we left it around
     */
    if (lstat(path, &tstat) == 0) {
        if (S_ISSOCK(tstat.st_mode))
            unlink(path);
    }

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /*
     * the memset call clears nonstandard fields in some implementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    assert(strcmp(addr.sun_path, path) == 0);
    old_umask = umask( ~(access_mask&0777));
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        close(sfd);
        umask(old_umask);
        return 1;
    }
    umask(old_umask);
    if (listen(sfd, settings.backlog) == -1) {
        perror("listen()");
        close(sfd);
        return 1;
    }
    if (!(listen_conn = conn_new(sfd, conn_listening,
                                 EV_READ | EV_PERSIST, 1,
                                 local_transport, main_base))) {
        fprintf(stderr, "failed to create listening connection\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}
#endif

/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas there can be tens of thousands
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;

/* libevent uses a monotonic clock when available for event scheduling. Aside
 * from jitter, simply ticking our internal timer here is accurate enough.
 * Note that users who are setting explicit dates for expiration times *must*
 * ensure their clocks are correct before starting memcached. */
static void clock_thread(void *arg) {
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    static bool monotonic = false;
    static time_t monotonic_start;
#endif

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            monotonic = true;
            monotonic_start = ts.tv_sec - ITEM_UPDATE_INTERVAL - 2;
        }
#endif

    while (1) {
        timer_sleep(ONE_SECOND);
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
        if (monotonic) {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
                continue;
            current_time = (rel_time_t) (ts.tv_sec - monotonic_start);
            continue;
        }
#endif
        {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            current_time = (rel_time_t) (tv.tv_sec - process_started);
        }
    }
}

static void usage(void) {
    printf(PACKAGE " " VERSION "\n");
    printf("-p, --port=<num>          TCP port to listen on (default: 11211)\n"
           "-U, --udp-port=<num>      UDP port to listen on (default: 11211, 0 is off)\n"
           "-s, --unix-socket=<file>  UNIX socket to listen on (disables network support)\n"
           "-A, --enable-shutdown     enable ascii \"shutdown\" command\n"
           "-a, --unix-mask=<mask>    access mask for UNIX socket, in octal (default: 0700)\n"
           "-l, --listen=<addr>       interface to listen on (default: INADDR_ANY)\n"
           "-d, --daemon              run as a daemon\n"
           "-r, --enable-coredumps    maximize core file limit\n"
           "-u, --user=<user>         assume identity of <username> (only when run as root)\n"
           "-m, --memory-limit=<num>  item memory in megabytes (default: 64 MB)\n"
           "-M, --disable-evictions   return error on memory exhausted instead of evicting\n"
           "-c, --conn-limit=<num>    max simultaneous connections (default: 1024)\n"
           "-k, --lock-memory         lock down all paged memory\n"
           "-v, --verbose             verbose (print errors/warnings while in event loop)\n"
           "-vv                       very verbose (also print client commands/responses)\n"
           "-vvv                      extremely verbose (internal state transitions)\n"
           "-h, --help                print this help and exit\n"
           "-i, --license             print memcached and libevent license\n"
           "-V, --version             print version and exit\n"
           "-P, --pidfile=<file>      save PID in <file>, only used with -d option\n"
           "-f, --slab-growth-factor=<num> chunk size growth factor (default: 1.25)\n"
           "-n, --slab-min-size=<bytes> min space used for key+value+flags (default: 48)\n");
    printf("-L, --enable-largepages  try to use large memory pages (if available)\n");
    printf("-D <char>     Use <char> as the delimiter between key prefixes and IDs.\n"
           "              This is used for per-prefix stats reporting. The default is\n"
           "              \":\" (colon). If this option is specified, stats collection\n"
           "              is turned on automatically; if not, then it may be turned on\n"
           "              by sending the \"stats detail on\" command to the server.\n");
    printf("-t, --threads=<num>       number of threads to use (default: 4)\n");
    printf("-R, --max-reqs-per-event  maximum number of requests per event, limits the\n"
           "                          requests processed per connection to prevent \n"
           "                          starvation (default: 20)\n");
    printf("-C, --disable-cas         disable use of CAS\n");
    printf("-b, --listen-backlog=<num> set the backlog queue limit (default: 1024)\n");
    printf("-B, --protocol=<name>     protocol - one of ascii, binary, or auto (default)\n");
    printf("-I, --max-item-size=<num> adjusts max item size\n"
           "                          (default: 1mb, min: 1k, max: 128m)\n");
#ifdef ENABLE_SASL
    printf("-S, --enable-sasl         turn on Sasl authentication\n");
#endif
    printf("-F, --disable-flush-all   disable flush_all command\n");
    printf("-X, --disable-dumping     disable stats cachedump and lru_crawler metadump\n");
    printf("-o, --extended            comma separated list of extended options\n"
           "                          most options have a 'no_' prefix to disable\n"
           "   - maxconns_fast:       immediately close new connections after limit\n"
           "   - hashpower:           an integer multiplier for how large the hash\n"
           "                          table should be. normally grows at runtime.\n"
           "                          set based on \"STAT hash_power_level\"\n"
           "   - tail_repair_time:    time in seconds for how long to wait before\n"
           "                          forcefully killing LRU tail item.\n"
           "                          disabled by default; very dangerous option.\n"
           "   - hash_algorithm:      the hash table algorithm\n"
           "                          default is murmur3 hash. options: jenkins, murmur3\n"
           "   - lru_crawler:         enable LRU Crawler background thread\n"
           "   - lru_crawler_sleep:   microseconds to sleep between items\n"
           "                          default is 100.\n"
           "   - lru_crawler_tocrawl: max items to crawl per slab per run\n"
           "                          default is 0 (unlimited)\n"
           "   - lru_maintainer:      enable new LRU system + background thread\n"
           "   - hot_lru_pct:         pct of slab memory to reserve for hot lru.\n"
           "                          (requires lru_maintainer)\n"
           "   - warm_lru_pct:        pct of slab memory to reserve for warm lru.\n"
           "                          (requires lru_maintainer)\n"
           "   - hot_max_factor:      items idle > cold lru age * drop from hot lru.\n"
           "   - warm_max_factor:     items idle > cold lru age * this drop from warm.\n"
           "   - temporary_ttl:       TTL's below get separate LRU, can't be evicted.\n"
           "                          (requires lru_maintainer)\n"
           "   - idle_timeout:        timeout for idle connections\n"
           "   - slab_chunk_max:      (EXPERIMENTAL) maximum slab size. use extreme care.\n"
           "   - watcher_logbuf_size: size in kilobytes of per-watcher write buffer.\n"
           "   - worker_logbuf_size:  size in kilobytes of per-worker-thread buffer\n"
           "                          read by background thread, then written to watchers.\n"
           "   - track_sizes:         enable dynamic reports for 'stats sizes' command.\n"
           "   - no_inline_ascii_resp: save up to 24 bytes per item.\n"
           "                           small perf hit in ASCII, no perf difference in\n"
           "                           binary protocol. speeds up all sets.\n"
           "   - no_hashexpand:       disables hash table expansion (dangerous)\n"
           "   - modern:              enables options which will be default in future.\n"
           "             currently: nothing\n"
           "   - no_modern:           uses defaults of previous major version (1.4.x)\n"
#ifdef HAVE_DROP_PRIVILEGES
           "   - no_drop_privileges: Disable drop_privileges in case it causes issues with\n"
           "                          some customisation.\n"
#ifdef MEMCACHED_DEBUG
           "   - relaxed_privileges: Running tests requires extra privileges.\n"
#endif
#endif
#ifdef EXTSTORE
           "   - ext_path:            file to write to for external storage.\n"
           "   - ext_page_size:       size in megabytes of storage pages.\n"
           "   - ext_wbuf_size:       size in megabytes of page write buffers.\n"
           "   - ext_threads:         number of IO threads to run.\n"
           "   - ext_item_size:       store items larger than this (bytes)\n"
           "   - ext_item_age:        store items idle at least this long\n"
           "   - ext_low_ttl:         consider TTLs lower than this specially\n"
           "   - ext_drop_unread:     don't re-write unread values during compaction\n"
           "   - ext_recache_rate:    recache an item every N accesses\n"
           "   - ext_compact_under:   compact when fewer than this many free pages\n"
           "   - ext_drop_under:      drop COLD items when fewer than this many free pages\n"
           "   - ext_max_frag:        max page fragmentation to tolerage\n"
           "                          (see doc/storage.txt for more info)\n"
#endif
           );
    return;
}

static void usage_license(void) {
    printf(PACKAGE " " VERSION "\n\n");
    printf(
    "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Niels Provos.\n"
    "\n"
    "[ libevent ]\n"
    "\n"
    "Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "1. Redistributions of source code must retain the above copyright\n"
    "   notice, this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright\n"
    "   notice, this list of conditions and the following disclaimer in the\n"
    "   documentation and/or other materials provided with the distribution.\n"
    "3. All advertising materials mentioning features or use of this software\n"
    "   must display the following acknowledgement:\n"
    "      This product includes software developed by Niels Provos.\n"
    "4. The name of the author may not be used to endorse or promote products\n"
    "   derived from this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n"
    "IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n"
    "OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.\n"
    "IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n"
    "INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n"
    "NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
    "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    );

    return;
}

static void save_pid(const char *pid_file) {
    FILE *fp;
    if (access(pid_file, F_OK) == 0) {
        if ((fp = fopen(pid_file, "r")) != NULL) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), fp) != NULL) {
                unsigned int pid;
                if (safe_strtoul(buffer, &pid) && kill((pid_t)pid, 0) == 0) {
                    fprintf(stderr, "WARNING: The pid file contained the following (running) pid: %u\n", pid);
                }
            }
            fclose(fp);
        }
    }

    /* Create the pid file first with a temporary name, then
     * atomically move the file to the real name to avoid a race with
     * another process opening the file to read the pid, but finding
     * it empty.
     */
    char tmp_pid_file[1024];
    snprintf(tmp_pid_file, sizeof(tmp_pid_file), "%s.tmp", pid_file);

    if ((fp = fopen(tmp_pid_file, "w")) == NULL) {
        vperror("Could not open the pid file %s for writing", tmp_pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)getpid());
    if (fclose(fp) == -1) {
        vperror("Could not close the pid file %s", tmp_pid_file);
    }

    if (rename(tmp_pid_file, pid_file) != 0) {
        vperror("Could not rename the pid file from %s to %s",
                tmp_pid_file, pid_file);
    }
}

static void remove_pidfile(const char *pid_file) {
  if (pid_file == NULL)
      return;

  if (unlink(pid_file) != 0) {
      vperror("Could not remove the pid file %s", pid_file);
  }

}

static void sig_handler(const int sig) {
    printf("Signal handled: %s.\n", strsignal(sig));
#ifdef DBG
    printf("Number of thread yields: %d & %d\n", th_yield1, th_yield2);
    printf("Number of involuntary parks: %d\n", sched_parked1);
    printf("Number of sigusr1 parks: %d\n", sig_usr1_park);
    printf("Number of preempt parks: %d\n", preempt_park);
    printf("Number of preempt parks because of preempt sig: %d\n", sched_preempt_needed);
    printf("Number of tcp read wait: %d, %d\n", ww_actual_cnt1, ww_cnt1);
    printf("Number of tcp write wait: %d, %d\n", ww_actual_cnt2, ww_cnt2);
    printf("Number of tcp transmit wait: %d, %d\n", ww_actual_cnt3, ww_cnt3);
#endif
    exit(EXIT_SUCCESS);
}

#ifndef HAVE_SIGIGNORE
static int sigignore(int sig) {
    struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = 0 };

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1) {
        return -1;
    }
    return 0;
}
#endif


/*
 * On systems that supports multiple page sizes we may reduce the
 * number of TLB-misses by using the biggest available page size
 */
static int enable_large_pages(void) {
#if defined(HAVE_GETPAGESIZES) && defined(HAVE_MEMCNTL)
    int ret = -1;
    size_t sizes[32];
    int avail = getpagesizes(sizes, 32);
    if (avail != -1) {
        size_t max = sizes[0];
        struct memcntl_mha arg = {0};
        int ii;

        for (ii = 1; ii < avail; ++ii) {
            if (max < sizes[ii]) {
                max = sizes[ii];
            }
        }

        arg.mha_flags   = 0;
        arg.mha_pagesize = max;
        arg.mha_cmd = MHA_MAPSIZE_BSSBRK;

        if (memcntl(0, 0, MC_HAT_ADVISE, (caddr_t)&arg, 0, 0) == -1) {
            fprintf(stderr, "Failed to set large pages: %s\n",
                    strerror(errno));
            fprintf(stderr, "Will use default page size\n");
        } else {
            ret = 0;
        }
    } else {
        fprintf(stderr, "Failed to get supported pagesizes: %s\n",
                strerror(errno));
        fprintf(stderr, "Will use default page size\n");
    }

    return ret;
#else
    return -1;
#endif
}

#if 0
/**
 * Do basic sanity check of the runtime environment
 * @return true if no errors found, false if we can't use this env
 */
static bool sanitycheck(void) {
    /* One of our biggest problems is old and bogus libevents */
    const char *ever = event_get_version();
    if (ever != NULL) {
        if (strncmp(ever, "1.", 2) == 0) {
            /* Require at least 1.3 (that's still a couple of years old) */
            if (('0' <= ever[2] && ever[2] < '3') && !isdigit(ever[3])) {
                fprintf(stderr, "You are using libevent %s.\nPlease upgrade to"
                        " a more recent version (1.3 or newer)\n",
                        event_get_version());
                return false;
            }
        }
    }

    return true;
}
#endif

static bool _parse_slab_sizes(char *s, uint32_t *slab_sizes) {
    char *b = NULL;
    uint32_t size = 0;
    int i = 0;
    uint32_t last_size = 0;

    if (strlen(s) < 1)
        return false;

    for (char *p = strtok_r(s, "-", &b);
         p != NULL;
         p = strtok_r(NULL, "-", &b)) {
        if (!safe_strtoul(p, &size) || size < settings.chunk_size
             || size > settings.slab_chunk_size_max) {
            fprintf(stderr, "slab size %u is out of valid range\n", size);
            return false;
        }
        if (last_size >= size) {
            fprintf(stderr, "slab size %u cannot be lower than or equal to a previous class size\n", size);
            return false;
        }
        if (size <= last_size + CHUNK_ALIGN_BYTES) {
            fprintf(stderr, "slab size %u must be at least %d bytes larger than previous class\n",
                    size, CHUNK_ALIGN_BYTES);
            return false;
        }
        slab_sizes[i++] = size;
        last_size = size;
        if (i >= MAX_NUMBER_OF_SLAB_CLASSES-1) {
            fprintf(stderr, "too many slab classes specified\n");
            return false;
        }
    }

    slab_sizes[i] = 0;
    return true;
}


static bool lock_memory = false;
static bool do_daemonize = false;
static bool preallocate = false;
static int maxcore = 0;
static char *username = NULL;
static char *pid_file = NULL;
#ifndef DISABLE_PRIVILEGE_CHECK
static struct passwd *pw;
#endif
static struct rlimit rlim;
static bool protocol_specified = false;
static bool start_lru_maintainer = true;
static bool start_lru_crawler = true;
static bool start_assoc_maint = false; // we don't support hashexpand
static enum hashfunc_type hash_type = MURMUR3_HASH;
static char *slab_sizes_unparsed = NULL;
static bool slab_chunk_size_changed = false;
static bool use_slab_sizes = false;
static uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES];



static void arg_parse(void *arg)
{
    char **argv = arg;
    int argc = 0;
    for (; argv[argc]; argc++) ;

    int c;
    char *buf;
    int size_max = 0;
    char unit = '\0';
    uint32_t tocrawl;

#ifdef EXTSTORE
    void *storage = NULL;
    char *storage_file = NULL;
    struct extstore_conf ext_cf;
#endif
    char *subopts, *subopts_orig;
    char *subopts_value;
    enum {
        MAXCONNS_FAST = 0,
        HASHPOWER_INIT,
        NO_HASHEXPAND,
        SLAB_REASSIGN,
        SLAB_AUTOMOVE,
        SLAB_AUTOMOVE_RATIO,
        SLAB_AUTOMOVE_WINDOW,
        TAIL_REPAIR_TIME,
        HASH_ALGORITHM,
        LRU_CRAWLER,
        LRU_CRAWLER_SLEEP,
        LRU_CRAWLER_TOCRAWL,
        LRU_MAINTAINER,
        HOT_LRU_PCT,
        WARM_LRU_PCT,
        HOT_MAX_FACTOR,
        WARM_MAX_FACTOR,
        TEMPORARY_TTL,
        IDLE_TIMEOUT,
        WATCHER_LOGBUF_SIZE,
        WORKER_LOGBUF_SIZE,
        SLAB_SIZES,
        SLAB_CHUNK_MAX,
        TRACK_SIZES,
        NO_INLINE_ASCII_RESP,
        MODERN,
        NO_MODERN,
        NO_CHUNKED_ITEMS,
        NO_SLAB_REASSIGN,
        NO_SLAB_AUTOMOVE,
        NO_MAXCONNS_FAST,
        INLINE_ASCII_RESP,
        NO_LRU_CRAWLER,
        NO_LRU_MAINTAINER,
        NO_DROP_PRIVILEGES,
#ifdef MEMCACHED_DEBUG
        RELAXED_PRIVILEGES,
#endif
#ifdef EXTSTORE
        EXT_PAGE_SIZE,
        EXT_PAGE_COUNT,
        EXT_WBUF_SIZE,
        EXT_THREADS,
        EXT_IO_DEPTH,
        EXT_PATH,
        EXT_ITEM_SIZE,
        EXT_ITEM_AGE,
        EXT_LOW_TTL,
        EXT_RECACHE_RATE,
        EXT_COMPACT_UNDER,
        EXT_DROP_UNDER,
        EXT_MAX_FRAG,
        EXT_DROP_UNREAD,
        SLAB_AUTOMOVE_FREERATIO,
#endif
    };
    char *const subopts_tokens[] = {
        [MAXCONNS_FAST] = "maxconns_fast",
        [HASHPOWER_INIT] = "hashpower",
        [NO_HASHEXPAND] = "no_hashexpand",
        [SLAB_REASSIGN] = "slab_reassign",
        [SLAB_AUTOMOVE] = "slab_automove",
        [SLAB_AUTOMOVE_RATIO] = "slab_automove_ratio",
        [SLAB_AUTOMOVE_WINDOW] = "slab_automove_window",
        [TAIL_REPAIR_TIME] = "tail_repair_time",
        [HASH_ALGORITHM] = "hash_algorithm",
        [LRU_CRAWLER] = "lru_crawler",
        [LRU_CRAWLER_SLEEP] = "lru_crawler_sleep",
        [LRU_CRAWLER_TOCRAWL] = "lru_crawler_tocrawl",
        [LRU_MAINTAINER] = "lru_maintainer",
        [HOT_LRU_PCT] = "hot_lru_pct",
        [WARM_LRU_PCT] = "warm_lru_pct",
        [HOT_MAX_FACTOR] = "hot_max_factor",
        [WARM_MAX_FACTOR] = "warm_max_factor",
        [TEMPORARY_TTL] = "temporary_ttl",
        [IDLE_TIMEOUT] = "idle_timeout",
        [WATCHER_LOGBUF_SIZE] = "watcher_logbuf_size",
        [WORKER_LOGBUF_SIZE] = "worker_logbuf_size",
        [SLAB_SIZES] = "slab_sizes",
        [SLAB_CHUNK_MAX] = "slab_chunk_max",
        [TRACK_SIZES] = "track_sizes",
        [NO_INLINE_ASCII_RESP] = "no_inline_ascii_resp",
        [MODERN] = "modern",
        [NO_MODERN] = "no_modern",
        [NO_CHUNKED_ITEMS] = "no_chunked_items",
        [NO_SLAB_REASSIGN] = "no_slab_reassign",
        [NO_SLAB_AUTOMOVE] = "no_slab_automove",
        [NO_MAXCONNS_FAST] = "no_maxconns_fast",
        [INLINE_ASCII_RESP] = "inline_ascii_resp",
        [NO_LRU_CRAWLER] = "no_lru_crawler",
        [NO_LRU_MAINTAINER] = "no_lru_maintainer",
        [NO_DROP_PRIVILEGES] = "no_drop_privileges",
#ifdef MEMCACHED_DEBUG
        [RELAXED_PRIVILEGES] = "relaxed_privileges",
#endif
#ifdef EXTSTORE
        [EXT_PAGE_SIZE] = "ext_page_size",
        [EXT_PAGE_COUNT] = "ext_page_count",
        [EXT_WBUF_SIZE] = "ext_wbuf_size",
        [EXT_THREADS] = "ext_threads",
        [EXT_IO_DEPTH] = "ext_io_depth",
        [EXT_PATH] = "ext_path",
        [EXT_ITEM_SIZE] = "ext_item_size",
        [EXT_ITEM_AGE] = "ext_item_age",
        [EXT_LOW_TTL] = "ext_low_ttl",
        [EXT_RECACHE_RATE] = "ext_recache_rate",
        [EXT_COMPACT_UNDER] = "ext_compact_under",
        [EXT_DROP_UNDER] = "ext_drop_under",
        [EXT_MAX_FRAG] = "ext_max_frag",
        [EXT_DROP_UNREAD] = "ext_drop_unread",
        [SLAB_AUTOMOVE_FREERATIO] = "slab_automove_freeratio",
#endif
        NULL
    };

#if 0
    if (!sanitycheck()) {
        return EX_OSERR;
    }

    /* handle SIGINT and SIGTERM */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* init settings */
    settings_init();
#ifdef EXTSTORE
    settings.ext_item_size = 512;
    settings.ext_item_age = UINT_MAX;
    settings.ext_low_ttl = 0;
    settings.ext_recache_rate = 2000;
    settings.ext_max_frag = 0.8;
    settings.ext_drop_unread = false;
    settings.ext_wbuf_size = 1024 * 1024 * 4;
    settings.ext_compact_under = 0;
    settings.ext_drop_under = 0;
    settings.slab_automove_freeratio = 0.01;
    ext_cf.page_size = 1024 * 1024 * 64;
    ext_cf.page_count = 64;
    ext_cf.wbuf_size = settings.ext_wbuf_size;
    ext_cf.io_threadcount = 1;
    ext_cf.io_depth = 1;
    ext_cf.page_buckets = 4;
    ext_cf.wbuf_count = ext_cf.page_buckets;
#endif

    /* Run regardless of initializing it later */
    init_lru_maintainer();

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);
#endif

    char *shortopts =
          "a:"  /* access mask for unix socket */
          "A"  /* enable admin shutdown command */
          "p:"  /* TCP port number to listen on */
          "s:"  /* unix socket path to listen on */
          "U:"  /* UDP port number to listen on */
          "m:"  /* max memory to use for items in megabytes */
          "M"   /* return error on memory exhausted */
          "c:"  /* max simultaneous connections */
          "k"   /* lock down all paged memory */
          "hiV" /* help, licence info, version */
          "r"   /* maximize core file limit */
          "v"   /* verbose */
          "d"   /* daemon mode */
          "l:"  /* interface to listen on */
          "u:"  /* user identity to run as */
          "P:"  /* save PID in file */
          "f:"  /* factor? */
          "n:"  /* minimum space allocated for key+value+flags */
          "t:"  /* threads */
          "D:"  /* prefix delimiter? */
          "L"   /* Large memory pages */
          "R:"  /* max requests per event */
          "C"   /* Disable use of CAS */
          "b:"  /* backlog queue limit */
          "B:"  /* Binding protocol */
          "I:"  /* Max item size */
          "S"   /* Sasl ON */
          "F"   /* Disable flush_all */
          "X"   /* Disable dump commands */
          "o:"  /* Extended generic options */
          ;

    /* process arguments */
#ifdef HAVE_GETOPT_LONG
    const struct option longopts[] = {
        {"unix-mask", required_argument, 0, 'a'},
        {"enable-shutdown", no_argument, 0, 'A'},
        {"port", required_argument, 0, 'p'},
        {"unix-socket", required_argument, 0, 's'},
        {"udp-port", required_argument, 0, 'U'},
        {"memory-limit", required_argument, 0, 'm'},
        {"disable-evictions", no_argument, 0, 'M'},
        {"conn-limit", required_argument, 0, 'c'},
        {"lock-memory", no_argument, 0, 'k'},
        {"help", no_argument, 0, 'h'},
        {"license", no_argument, 0, 'i'},
        {"version", no_argument, 0, 'V'},
        {"enable-coredumps", no_argument, 0, 'r'},
        {"verbose", optional_argument, 0, 'v'},
        {"daemon", no_argument, 0, 'd'},
        {"listen", required_argument, 0, 'l'},
        {"user", required_argument, 0, 'u'},
        {"pidfile", required_argument, 0, 'P'},
        {"slab-growth-factor", required_argument, 0, 'f'},
        {"slab-min-size", required_argument, 0, 'n'},
        {"threads", required_argument, 0, 't'},
        {"enable-largepages", no_argument, 0, 'L'},
        {"max-reqs-per-event", required_argument, 0, 'R'},
        {"disable-cas", no_argument, 0, 'C'},
        {"listen-backlog", required_argument, 0, 'b'},
        {"protocol", required_argument, 0, 'B'},
        {"max-item-size", required_argument, 0, 'I'},
        {"enable-sasl", no_argument, 0, 'S'},
        {"disable-flush-all", no_argument, 0, 'F'},
        {"disable-dumping", no_argument, 0, 'X'},
        {"extended", required_argument, 0, 'o'},
        {0, 0, 0, 0}
    };
    int optindex;
    while (-1 != (c = getopt_long(argc, argv, shortopts,
                    longopts, &optindex))) {
#else
    while (-1 != (c = getopt(argc, argv, shortopts))) {
#endif
        switch (c) {
        case 'A':
            /* enables "shutdown" command */
            settings.shutdown_command = true;
            break;

        case 'a':
            /* access for unix domain socket, as octal mask (like chmod)*/
            settings.access= strtol(optarg,NULL,8);
            break;
        case 'U':
            settings.udpport = atoi(optarg);
            break;
        case 'p':
            settings.port = atoi(optarg);
            break;
        case 's':
            settings.socketpath = optarg;
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024;
            break;
        case 'M':
            settings.evict_to_free = 0;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            if (settings.maxconns <= 0) {
                fprintf(stderr, "Maximum connections must be greater than 0\n");
                exit(1);
            }
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'V':
            printf(PACKAGE " " VERSION "\n");
            exit(EXIT_SUCCESS);
        case 'k':
            lock_memory = true;
            break;
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            if (settings.inter != NULL) {
                if (strstr(settings.inter, optarg) != NULL) {
                    break;
                }
                size_t len = strlen(settings.inter) + strlen(optarg) + 2;
                char *p = malloc(len);
                if (p == NULL) {
                    fprintf(stderr, "Failed to allocate memory\n");
                    exit(1);
                }
                snprintf(p, len, "%s,%s", settings.inter, optarg);
                free(settings.inter);
                settings.inter = p;
            } else {
                settings.inter= strdup(optarg);
            }
            break;
        case 'd':
            do_daemonize = true;
            break;
        case 'r':
            maxcore = 1;
            break;
        case 'R':
            settings.reqs_per_event = atoi(optarg);
            if (settings.reqs_per_event == 0) {
                fprintf(stderr, "Number of requests per event must be greater than 0\n");
                exit(1);
            }
            break;
        case 'u':
            username = optarg;
            break;
        case 'P':
            pid_file = optarg;
            break;
        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                fprintf(stderr, "Factor must be greater than 1\n");
                exit(1);
            }
            break;
        case 'n':
            settings.chunk_size = atoi(optarg);
            if (settings.chunk_size == 0) {
                fprintf(stderr, "Chunk size must be greater than 0\n");
                exit(1);
            }
            break;
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads <= 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                exit(1);
            }
            /* There're other problems when you get above 64 threads.
             * In the future we should portably detect # of cores for the
             * default.
             */
            if (settings.num_threads > 64) {
                fprintf(stderr, "WARNING: Setting a high number of worker"
                                "threads is not recommended.\n"
                                " Set this value to the number of cores in"
                                " your machine or less.\n");
            }
            break;
        case 'D':
            if (! optarg || ! optarg[0]) {
                fprintf(stderr, "No delimiter specified\n");
                exit(1);
            }
            settings.prefix_delimiter = optarg[0];
            settings.detail_enabled = 1;
            break;
        case 'L' :
            if (enable_large_pages() == 0) {
                preallocate = true;
            } else {
                fprintf(stderr, "Cannot enable large pages on this system\n"
                    "(There is no Linux support as of this version)\n");
                exit(1);
            }
            break;
        case 'C' :
            settings.use_cas = false;
            break;
        case 'b' :
            settings.backlog = atoi(optarg);
            break;
        case 'B':
            protocol_specified = true;
            if (strcmp(optarg, "auto") == 0) {
                settings.binding_protocol = negotiating_prot;
            } else if (strcmp(optarg, "binary") == 0) {
                settings.binding_protocol = binary_prot;
            } else if (strcmp(optarg, "ascii") == 0) {
                settings.binding_protocol = ascii_prot;
            } else {
                fprintf(stderr, "Invalid value for binding protocol: %s\n"
                        " -- should be one of auto, binary, or ascii\n", optarg);
                exit(EX_USAGE);
            }
            break;
        case 'I':
            buf = strdup(optarg);
            unit = buf[strlen(buf)-1];
            if (unit == 'k' || unit == 'm' ||
                unit == 'K' || unit == 'M') {
                buf[strlen(buf)-1] = '\0';
                size_max = atoi(buf);
                if (unit == 'k' || unit == 'K')
                    size_max *= 1024;
                if (unit == 'm' || unit == 'M')
                    size_max *= 1024 * 1024;
                settings.item_size_max = size_max;
            } else {
                settings.item_size_max = atoi(buf);
            }
            free(buf);
            break;
        case 'S': /* set Sasl authentication to true. Default is false */
#ifndef ENABLE_SASL
            fprintf(stderr, "This server is not built with SASL support.\n");
            exit(EX_USAGE);
#endif
            settings.sasl = true;
            break;
       case 'F' :
            settings.flush_enabled = false;
            break;
       case 'X' :
            settings.dump_enabled = false;
            break;
        case 'o': /* It's sub-opts time! */
            subopts_orig = subopts = strdup(optarg); /* getsubopt() changes the original args */

            while (*subopts != '\0') {

            switch (getsubopt(&subopts, subopts_tokens, &subopts_value)) {
            case MAXCONNS_FAST:
                settings.maxconns_fast = true;
                break;
            case HASHPOWER_INIT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for hashpower\n");
                    exit(1);
                }
                settings.hashpower_init = atoi(subopts_value);
                if (settings.hashpower_init < 12) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too low\n",
                        settings.hashpower_init);
                    exit(1);
                } else if (settings.hashpower_init > 32) {
                    fprintf(stderr, "Initial hashtable multiplier of %d is too high\n"
                        "Choose a value based on \"STAT hash_power_level\" from a running instance\n",
                        settings.hashpower_init);
                    exit(1);
                }
                break;
            case NO_HASHEXPAND:
                start_assoc_maint = false;
                break;
            case SLAB_REASSIGN:
                settings.slab_reassign = true;
                break;
            case SLAB_AUTOMOVE:
                if (subopts_value == NULL) {
                    settings.slab_automove = 1;
                    break;
                }
                settings.slab_automove = atoi(subopts_value);
                if (settings.slab_automove < 0 || settings.slab_automove > 2) {
                    fprintf(stderr, "slab_automove must be between 0 and 2\n");
                    exit(1);
                }
                break;
            case SLAB_AUTOMOVE_RATIO:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_ratio argument\n");
                    exit(1);
                }
                settings.slab_automove_ratio = atof(subopts_value);
                if (settings.slab_automove_ratio <= 0 || settings.slab_automove_ratio > 1) {
                    fprintf(stderr, "slab_automove_ratio must be > 0 and < 1\n");
                    exit(1);
                }
                break;
            case SLAB_AUTOMOVE_WINDOW:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_window argument\n");
                    exit(1);
                }
                settings.slab_automove_window = atoi(subopts_value);
                if (settings.slab_automove_window < 3) {
                    fprintf(stderr, "slab_automove_window must be > 2\n");
                    exit(1);
                }
                break;
            case TAIL_REPAIR_TIME:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for tail_repair_time\n");
                    exit(1);
                }
                settings.tail_repair_time = atoi(subopts_value);
                if (settings.tail_repair_time < 10) {
                    fprintf(stderr, "Cannot set tail_repair_time to less than 10 seconds\n");
                    exit(1);
                }
                break;
            case HASH_ALGORITHM:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hash_algorithm argument\n");
                    exit(1);
                };
                if (strcmp(subopts_value, "jenkins") == 0) {
                    hash_type = JENKINS_HASH;
                } else if (strcmp(subopts_value, "murmur3") == 0) {
                    hash_type = MURMUR3_HASH;
                } else {
                    fprintf(stderr, "Unknown hash_algorithm option (jenkins, murmur3)\n");
                    exit(1);
                }
                break;
            case LRU_CRAWLER:
                start_lru_crawler = true;
                break;
            case LRU_CRAWLER_SLEEP:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_sleep value\n");
                    exit(1);
                }
                settings.lru_crawler_sleep = atoi(subopts_value);
                if (settings.lru_crawler_sleep > 1000000 || settings.lru_crawler_sleep < 0) {
                    fprintf(stderr, "LRU crawler sleep must be between 0 and 1 second\n");
                    exit(1);
                }
                break;
            case LRU_CRAWLER_TOCRAWL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing lru_crawler_tocrawl value\n");
                    exit(1);
                }
                if (!safe_strtoul(subopts_value, &tocrawl)) {
                    fprintf(stderr, "lru_crawler_tocrawl takes a numeric 32bit value\n");
                    exit(1);
                }
                settings.lru_crawler_tocrawl = tocrawl;
                break;
            case LRU_MAINTAINER:
                start_lru_maintainer = true;
                settings.lru_segmented = true;
                break;
            case HOT_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_lru_pct argument\n");
                    exit(1);
                }
                settings.hot_lru_pct = atoi(subopts_value);
                if (settings.hot_lru_pct < 1 || settings.hot_lru_pct >= 80) {
                    fprintf(stderr, "hot_lru_pct must be > 1 and < 80\n");
                    exit(1);
                }
                break;
            case WARM_LRU_PCT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_lru_pct argument\n");
                    exit(1);
                }
                settings.warm_lru_pct = atoi(subopts_value);
                if (settings.warm_lru_pct < 1 || settings.warm_lru_pct >= 80) {
                    fprintf(stderr, "warm_lru_pct must be > 1 and < 80\n");
                    exit(1);
                }
                break;
            case HOT_MAX_FACTOR:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing hot_max_factor argument\n");
                    exit(1);
                }
                settings.hot_max_factor = atof(subopts_value);
                if (settings.hot_max_factor <= 0) {
                    fprintf(stderr, "hot_max_factor must be > 0\n");
                    exit(1);
                }
                break;
            case WARM_MAX_FACTOR:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing warm_max_factor argument\n");
                    exit(1);
                }
                settings.warm_max_factor = atof(subopts_value);
                if (settings.warm_max_factor <= 0) {
                    fprintf(stderr, "warm_max_factor must be > 0\n");
                    exit(1);
                }
                break;
            case TEMPORARY_TTL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing temporary_ttl argument\n");
                    exit(1);
                }
                settings.temp_lru = true;
                settings.temporary_ttl = atoi(subopts_value);
                break;
            case IDLE_TIMEOUT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing numeric argument for idle_timeout\n");
                    exit(1);
                }
                settings.idle_timeout = atoi(subopts_value);
                break;
            case WATCHER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing watcher_logbuf_size argument\n");
                    exit(1);
                }
                if (!safe_strtoul(subopts_value, &settings.logger_watcher_buf_size)) {
                    fprintf(stderr, "could not parse argument to watcher_logbuf_size\n");
                    exit(1);
                }
                settings.logger_watcher_buf_size *= 1024; /* kilobytes */
                break;
            case WORKER_LOGBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing worker_logbuf_size argument\n");
                    exit(1);
                }
                if (!safe_strtoul(subopts_value, &settings.logger_buf_size)) {
                    fprintf(stderr, "could not parse argument to worker_logbuf_size\n");
                    exit(1);
                }
                settings.logger_buf_size *= 1024; /* kilobytes */
            case SLAB_SIZES:
                slab_sizes_unparsed = subopts_value;
                break;
            case SLAB_CHUNK_MAX:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_chunk_max argument\n");
                }
                if (!safe_strtol(subopts_value, &settings.slab_chunk_size_max)) {
                    fprintf(stderr, "could not parse argument to slab_chunk_max\n");
                }
                slab_chunk_size_changed = true;
                break;
            case TRACK_SIZES:
                item_stats_sizes_init();
                break;
            case NO_INLINE_ASCII_RESP:
                settings.inline_ascii_response = false;
                break;
            case INLINE_ASCII_RESP:
                settings.inline_ascii_response = true;
                break;
            case NO_CHUNKED_ITEMS:
                settings.slab_chunk_size_max = settings.slab_page_size;
                break;
            case NO_SLAB_REASSIGN:
                settings.slab_reassign = false;
                break;
            case NO_SLAB_AUTOMOVE:
                settings.slab_automove = 0;
                break;
            case NO_MAXCONNS_FAST:
                settings.maxconns_fast = false;
                break;
            case NO_LRU_CRAWLER:
                settings.lru_crawler = false;
                start_lru_crawler = false;
                break;
            case NO_LRU_MAINTAINER:
                start_lru_maintainer = false;
                settings.lru_segmented = false;
                break;
#ifdef EXTSTORE
            case EXT_PAGE_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_page_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.page_size)) {
                    fprintf(stderr, "could not parse argument to ext_page_size\n");
                    return 1;
                }
                ext_cf.page_size *= 1024 * 1024; /* megabytes */
                break;
            case EXT_PAGE_COUNT:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_page_count argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.page_count)) {
                    fprintf(stderr, "could not parse argument to ext_page_count\n");
                    return 1;
                }
                break;
            case EXT_WBUF_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_wbuf_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.wbuf_size)) {
                    fprintf(stderr, "could not parse argument to ext_wbuf_size\n");
                    return 1;
                }
                ext_cf.wbuf_size *= 1024 * 1024; /* megabytes */
                settings.ext_wbuf_size = ext_cf.wbuf_size;
                break;
            case EXT_THREADS:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_threads argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.io_threadcount)) {
                    fprintf(stderr, "could not parse argument to ext_threads\n");
                    return 1;
                }
                break;
            case EXT_IO_DEPTH:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_io_depth argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &ext_cf.io_depth)) {
                    fprintf(stderr, "could not parse argument to ext_io_depth\n");
                    return 1;
                }
                break;
            case EXT_ITEM_SIZE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_item_size argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_item_size)) {
                    fprintf(stderr, "could not parse argument to ext_item_size\n");
                    return 1;
                }
                break;
            case EXT_ITEM_AGE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_item_age argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_item_age)) {
                    fprintf(stderr, "could not parse argument to ext_item_age\n");
                    return 1;
                }
                break;
            case EXT_LOW_TTL:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_low_ttl argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_low_ttl)) {
                    fprintf(stderr, "could not parse argument to ext_low_ttl\n");
                    return 1;
                }
                break;
            case EXT_RECACHE_RATE:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_recache_rate argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_recache_rate)) {
                    fprintf(stderr, "could not parse argument to ext_recache_rate\n");
                    return 1;
                }
                break;
            case EXT_COMPACT_UNDER:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_compact_under argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_compact_under)) {
                    fprintf(stderr, "could not parse argument to ext_compact_under\n");
                    return 1;
                }
                break;
            case EXT_DROP_UNDER:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_drop_under argument\n");
                    return 1;
                }
                if (!safe_strtoul(subopts_value, &settings.ext_drop_under)) {
                    fprintf(stderr, "could not parse argument to ext_drop_under\n");
                    return 1;
                }
                break;
            case EXT_MAX_FRAG:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing ext_max_frag argument\n");
                    return 1;
                }
                if (!safe_strtod(subopts_value, &settings.ext_max_frag)) {
                    fprintf(stderr, "could not parse argument to ext_max_frag\n");
                    return 1;
                }
                break;
            case SLAB_AUTOMOVE_FREERATIO:
                if (subopts_value == NULL) {
                    fprintf(stderr, "Missing slab_automove_freeratio argument\n");
                    return 1;
                }
                if (!safe_strtod(subopts_value, &settings.slab_automove_freeratio)) {
                    fprintf(stderr, "could not parse argument to slab_automove_freeratio\n");
                    return 1;
                }
                break;
            case EXT_DROP_UNREAD:
                settings.ext_drop_unread = true;
                break;
            case EXT_PATH:
                storage_file = strdup(subopts_value);
                break;
#endif
            case MODERN:
                /* currently no new defaults */
                break;
            case NO_MODERN:
                if (!slab_chunk_size_changed) {
                    settings.slab_chunk_size_max = settings.slab_page_size;
                }
                settings.slab_reassign = false;
                settings.slab_automove = 0;
                settings.inline_ascii_response = true;
                settings.lru_segmented = false;
                hash_type = JENKINS_HASH;
                start_lru_crawler = false;
                start_lru_maintainer = false;
                break;
            case NO_DROP_PRIVILEGES:
                settings.drop_privileges = false;
                break;
#ifdef MEMCACHED_DEBUG
            case RELAXED_PRIVILEGES:
                settings.relaxed_privileges = true;
                break;
#endif
            default:
                printf("Illegal suboption \"%s\"\n", subopts_value);
                exit(1);
            }

            }
            free(subopts_orig);
            break;
        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            exit(1);
        }
    }
}

static int memcached_init(void);
static void memcached_main(void *arg);


static void validate_settings(void)
{
    if (settings.item_size_max < 1024) {
        fprintf(stderr, "Item max size cannot be less than 1024 bytes.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > (settings.maxbytes / 2)) {
        fprintf(stderr, "Cannot set item size limit higher than 1/2 of memory max.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > (1024 * 1024 * 1024)) {
        fprintf(stderr, "Cannot set item size limit higher than a gigabyte.\n");
        exit(EX_USAGE);
    }
    if (settings.item_size_max > 1024 * 1024) {
        if (!slab_chunk_size_changed) {
            // Ideal new default is 16k, but needs stitching.
            settings.slab_chunk_size_max = settings.slab_page_size / 2;
        }
    }

    if (settings.slab_chunk_size_max > settings.item_size_max) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) cannot be larger than -I (item_size_max %d)\n",
                settings.slab_chunk_size_max, settings.item_size_max);
        exit(EX_USAGE);
    }

    if (settings.item_size_max % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "-I (item_size_max: %d) must be evenly divisible by slab_chunk_max (bytes: %d)\n",
                settings.item_size_max, settings.slab_chunk_size_max);
        exit(EX_USAGE);
    }

    if (settings.slab_page_size % settings.slab_chunk_size_max != 0) {
        fprintf(stderr, "slab_chunk_max (bytes: %d) must divide evenly into %d (slab_page_size)\n",
                settings.slab_chunk_size_max, settings.slab_page_size);
        exit(EX_USAGE);
    }
#ifdef EXTSTORE
    if (storage_file) {
        if (settings.item_size_max > ext_cf.wbuf_size) {
            fprintf(stderr, "-I (item_size_max: %d) cannot be larger than ext_wbuf_size: %d\n",
                settings.item_size_max, ext_cf.wbuf_size);
            exit(EX_USAGE);
        }

        /* This is due to the suffix header being generated with the wrong length
         * value for the ITEM_HDR replacement. The cuddled nbytes no longer
         * matches, so we end up losing a few bytes on readback.
         */
        if (settings.inline_ascii_response) {
            fprintf(stderr, "Cannot use inline_ascii_response with extstore enabled\n");
            exit(EX_USAGE);
        }

        if (settings.udpport) {
            fprintf(stderr, "Cannot use UDP with extstore enabled (-U 0 to disable)\n");
            exit(EX_USAGE);
        }
    }
#endif
    // Reserve this for the new default. If factor size hasn't changed, use
    // new default.
    /*if (settings.slab_chunk_size_max == 16384 && settings.factor == 1.25) {
        settings.factor = 1.08;
    }*/

    if (slab_sizes_unparsed != NULL) {
        if (_parse_slab_sizes(slab_sizes_unparsed, slab_sizes)) {
            use_slab_sizes = true;
        } else {
            exit(EX_USAGE);
        }
    }

    if (settings.hot_lru_pct + settings.warm_lru_pct > 80) {
        fprintf(stderr, "hot_lru_pct + warm_lru_pct cannot be more than 80%% combined\n");
        exit(EX_USAGE);
    }

    if (settings.temp_lru && !start_lru_maintainer) {
        fprintf(stderr, "temporary_ttl requires lru_maintainer to be enabled\n");
        exit(EX_USAGE);
    }

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize hash_algorithm!\n");
        exit(EX_USAGE);
    }

    /*
     * Use one workerthread to serve each UDP port if the user specified
     * multiple ports
     */
    if (settings.inter != NULL && strchr(settings.inter, ',')) {
        settings.num_threads_per_udp = 1;
    } else {
        settings.num_threads_per_udp = settings.num_threads;
    }

    if (settings.sasl) {
        if (!protocol_specified) {
            settings.binding_protocol = binary_prot;
        } else {
            if (settings.binding_protocol != binary_prot) {
                fprintf(stderr, "ERROR: You cannot allow the ASCII protocol while using SASL.\n");
                exit(EX_USAGE);
            }
        }
    }
}


static int memcached_init(void) {

    /* Run regardless of initializing it later */
    init_lru_maintainer();

    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            return -EX_OSERR;
        }
    }


    /* lose root privileges if we have them */
#ifndef DISABLE_PRIVILEGE_CHECK
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            return -EX_USAGE;
        }
        if ((pw = getpwnam(username)) == 0) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            return -EX_NOUSER;
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            return -EX_OSERR;
        }
    }
#endif

    /* Initialize Sasl if -S was specified */
    if (settings.sasl) {
        init_sasl();
    }

    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            perror("Failed to ignore SIGHUP");
        }
        if (daemonize(maxcore, settings.verbose) == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            return EXIT_FAILURE;
        }
    }

    /* lock paged memory if needed */
    if (lock_memory) {
#ifdef HAVE_MLOCKALL
        int res = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (res != 0) {
            fprintf(stderr, "warning: -k invalid, mlockall() failed: %s\n",
                    strerror(errno));
        }
#else
        fprintf(stderr, "warning: -k invalid, mlockall() not supported on this platform.  proceeding without.\n");
#endif
    }

#if 0
/* initialize main thread libevent instance */
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
    /* If libevent version is larger/equal to 2.0.2-alpha, use newer version */
    struct event_config *ev_config;
    ev_config = event_config_new();
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
    main_base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
#else
    /* Otherwise, use older API */
    main_base = event_init();
#endif
#endif

    items_init();
    stats_init();
    logger_init();
    slabs_global_init();

    memcached_thread_init(settings.num_threads, NULL);

    udp_conn_tcache = tcache_create("udp_conn_tcache", &udp_conn_tcache_ops,
                                TCACHE_DEFAULT_MAG_SIZE, sizeof(conn));
    BUG_ON(udp_conn_tcache == NULL);

    return 0;
}

static void memcached_main(void *arg)
{
    int ret;
    printf("memcached_main STARTED!!!\n");
    /* initialize other stuff */
   if (start_logger_thread() != 0) {
        abort();
    }
    assoc_init(settings.hashpower_init);
    slabs_init(settings.maxbytes, settings.factor, preallocate,
            use_slab_sizes ? slab_sizes : NULL);
#ifdef EXTSTORE
    if (storage_file) {
        enum extstore_res eres;
        if (settings.ext_compact_under == 0) {
            settings.ext_compact_under = ext_cf.page_count / 4;
            /* Only rescues non-COLD items if below this threshold */
            settings.ext_drop_under = ext_cf.page_count / 4;
        }
        crc32c_init();
        /* Init free chunks to zero. */
        for (int x = 0; x < MAX_NUMBER_OF_SLAB_CLASSES; x++) {
            settings.ext_free_memchunks[x] = 0;
        }
        storage = extstore_init(storage_file, &ext_cf, &eres);
        if (storage == NULL) {
            fprintf(stderr, "Failed to initialize external storage: %s\n",
                    extstore_err(eres));
            if (eres == EXTSTORE_INIT_OPEN_FAIL) {
                perror("extstore open");
            }
            exit(EXIT_FAILURE);
        }
        ext_storage = storage;
        /* page mover algorithm for extstore needs memory prefilled */
        slabs_prefill_global();
    }
#endif
    init_lru_crawler(NULL);

    if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }
    if (start_lru_crawler && start_item_crawler_thread() != 0) {
        fprintf(stderr, "Failed to enable LRU crawler thread\n");
        exit(EXIT_FAILURE);
    }

    if (start_lru_maintainer && start_lru_maintainer_thread(NULL) != 0) {
        fprintf(stderr, "Failed to enable LRU maintainer thread\n");
        exit(EXIT_FAILURE);
    }

    if (settings.slab_reassign &&
        start_slab_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    /* initialise clock event */
    ret = thread_spawn(clock_thread, NULL);
    BUG_ON(ret);

#if 0
    /* create the listening socket, bind it, and init */
    if (settings.socketpath == NULL) {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char *temp_portnumber_filename = NULL;
        size_t len;
        FILE *portnumber_file = NULL;

        if (portnumber_filename != NULL) {
            len = strlen(portnumber_filename)+4+1;
            temp_portnumber_filename = malloc(len);
            snprintf(temp_portnumber_filename,
                     len,
                     "%s.lck", portnumber_filename);
             portnumber_file = fopen(temp_portnumber_filename, "a");
            if (portnumber_file == NULL) {
                fprintf(stderr, "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        errno = 0;
        if (settings.port && server_sockets(settings.port, tcp_transport,
                                           portnumber_file)) {
            vperror("failed to listen on TCP port %d", settings.port);
            exit(EX_OSERR);
        }
         /*
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonize if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking).
         */
         /* create the UDP listening socket and bind it */
        errno = 0;
        if (settings.udpport && server_sockets(settings.udpport, udp_transport,
                                              portnumber_file)) {
            vperror("failed to listen on UDP port %d", settings.udpport);
            exit(EX_OSERR);
        }

        if (portnumber_file) {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
        if (temp_portnumber_filename)
            free(temp_portnumber_filename);
    }
     /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);
    if (stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
        fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
        exit(EXIT_FAILURE);
    }
#endif

    timer_sleep(1000);

    active_tcp_conns = calloc(settings.maxconns, sizeof(conn *));
    if (!active_tcp_conns) {
        abort();
    }

    mutex_init(&tcp_conn_lock);

    if (settings.idle_timeout)
        thread_spawn(conn_timeout_thread, NULL);

    if (pid_file != NULL) {
        save_pid(pid_file);
    }

    /* Drop privileges no longer needed */
    if (settings.drop_privileges) {
        drop_privileges();
    }

    /* Initialize the uriencode lookup table. */
    uriencode_init();

    BUG_ON(!settings.udpport && !settings.port);

    if (settings.udpport) {
        struct netaddr l_udp = {
            .ip = 0,
            .port = settings.udpport
        };

        udpspawner_t *spawner;
        ret = udp_create_spawner(l_udp, udp_handler, &spawner);
        BUG_ON(ret);
    }

    if (settings.port) {
        struct netaddr l_tcp = {
            .ip = 0,
            .port = settings.port
        };

        tcpqueue_t *queue;
        ret = tcp_listen(l_tcp, settings.backlog, &queue);
        BUG_ON(ret);

        // Accept new connections
        while (true) {
            tcpconn_t *conn;
            ret = tcp_accept(queue, &conn);
            BUG_ON(ret);
            thread_spawn(handle_tcp_conn, conn);
        }
    }

    waitgroup_t wg;
    waitgroup_init(&wg);
    waitgroup_add(&wg, 1);
    waitgroup_wait(&wg);

    stop_assoc_maintenance_thread();

    /* remove the PID file if we're a daemon */
    if (do_daemonize)
        remove_pidfile(pid_file);
#if 0
    /* Clean up strdup() call for bind() address */
    if (settings.inter)
      free(settings.inter);
    if (l_socket)
      free(l_socket);
    if (u_socket)
      free(u_socket);

    /* cleanup base */
    event_base_free(main_base);
#endif

    exit(EXIT_SUCCESS);
}

int perthread_initializer(void);

int perthread_initializer(void)
{
    tcache_init_perthread(udp_conn_tcache, &udp_conn_tcache_pt);

    return memcached_perthread_init();
}

int late_initializer(void);

int late_initializer(void)
{
    // Preallocate some connections. seems to help with random segfaults
#define NREP 8000
    void *conns[NREP];
    int i,j;

    for (i = 0; i < NREP; i++) {
        conns[i] = tcache_alloc(&udp_conn_tcache_pt);
        if(!conns[i])
            break;
    }

    for (j = 0; j < i; j++) {
        tcache_free(&udp_conn_tcache_pt, conns[j]);
    }
    return i == NREP ? 0 : -1;
}

int main(int argc, char **argv) {
    int ret;
    printf("Memcached STARTED\n");

    if (argc < 2) {
            printf("arg must be config file\n");
            return -EINVAL;
    }

    /* init settings */
    settings_init();

    /* handle SIGINT and SIGTERM */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    char *cfgpath = argv[1];
    argv[1] = argv[0];

    arg_parse(argv + 1);

    validate_settings();

    ret = runtime_set_initializers(memcached_init, perthread_initializer, late_initializer);
    BUG_ON(ret);

    ret = runtime_init(cfgpath, memcached_main, argv + 1);
    if (ret) {
            printf("failed to start runtime\n");
            return ret;
    }

    return 0;
}
