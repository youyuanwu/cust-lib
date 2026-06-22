/* cust_execution::stream — family-agnostic async stream sockets.
 *
 * The shared core every connection-oriented socket family (TCP,
 * Unix stream) builds on. A SOCK_STREAM socket is a byte stream
 * over an fd regardless of address family, so accept / connect /
 * read / write are written ONCE here and parameterized only by a
 * `socket(family, …)` + `sockaddr`. The thin per-family modules
 * (tcp.c, uds.c) just marshal an address and call in here.
 *
 *   Rust/Tokio                     cust_execution
 *   --------------------------     ---------------------------
 *   TcpStream / UnixStream         cexec_stream
 *   TcpListener / UnixListener     cexec_listener
 *   AsyncReadExt::read             cexec_stream_read
 *   AsyncWriteExt::write_all       cexec_stream_write_all
 *   Listener::accept               cexec_listener_accept
 *   TcpStream::connect             cexec_net_connect
 *
 * Each type owns a `cexec_fd`, so registration + close lifetime
 * is handled in one place. The read/write/accept/connect futures
 * are small state machines that do the non-blocking syscall and,
 * on EAGAIN, await the matching readiness via the fd handle then
 * retry — the discipline epoll readiness requires.
 *
 * I/O futures yield an `isize`: >= 0 is a byte count (0 = EOF on
 * read), < 0 is `-errno`. accept / connect yield a `cexec_stream`
 * through the `out` pointer; check `cexec_stream_is_open` for
 * success.
 */

#cust use std;
#cust use crate::future;
#cust use crate::reactor;
#cust use crate::io;
#cust use crate::fd;

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

/* ─── types ─────────────────────────────────────────────── */

struct [[cust::pub_repr]] cexec_stream {
    struct cexec_fd fd;
};

struct [[cust::pub_repr]] cexec_listener {
    struct cexec_fd fd;
};

[[cust::pub]] int cexec_stream_raw(const struct cexec_stream *s) {
    return cexec_fd_raw(&s->fd);
}
[[cust::pub]] bool cexec_stream_is_open(const struct cexec_stream *s) {
    return cexec_fd_is_open(&s->fd);
}
[[cust::pub]] void cexec_stream_close(struct cexec_stream *s) {
    cexec_fd_close(&s->fd);
}

[[cust::pub]] int cexec_listener_raw(const struct cexec_listener *l) {
    return cexec_fd_raw(&l->fd);
}
[[cust::pub]] bool cexec_listener_is_open(const struct cexec_listener *l) {
    return cexec_fd_is_open(&l->fd);
}
[[cust::pub]] void cexec_listener_close(struct cexec_listener *l) {
    cexec_fd_close(&l->fd);
}

static struct cexec_future null_future(void) {
    struct cexec_future f = {(void *)0, (void *)0};
    return f;
}

static void make_closed_stream(struct cexec_stream *out, struct cstd_alloc a,
                               struct cexec_io_driver *d) {
    out->fd.alloc  = a;
    out->fd.driver = d;
    out->fd.fd     = -1;
}

/* ─── read / write ──────────────────────────────────────── */

struct rw_box {
    struct cstd_alloc    alloc;
    struct cexec_stream *stream;
    void                *buf;
    usize                len;
    bool                 is_write;
    bool                 awaiting;
    struct cexec_future  readiness;
};

static cexec_poll rw_poll(void *self, struct cexec_waker w, void *out) {
    struct rw_box *b = self;
    int fd = cexec_fd_raw(&b->stream->fd);
    for (;;) {
        if (b->awaiting) {
            if (!cexec_poll_is_ready(cexec_future_poll(b->readiness, w, (void *)0))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(b->readiness);
            b->readiness = null_future();
            b->awaiting  = false;
        }
        ssize_t n = b->is_write ? write(fd, b->buf, b->len)
                                : read(fd, b->buf, b->len);
        if (n >= 0) {
            if (out) {
                *(isize *)out = (isize)n;
            }
            return cexec_poll_ready();
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            b->readiness = b->is_write ? cexec_fd_writable(&b->stream->fd)
                                       : cexec_fd_readable(&b->stream->fd);
            b->awaiting = true;
            continue;
        }
        if (out) {
            *(isize *)out = -(isize)errno;
        }
        return cexec_poll_ready();
    }
}

static void rw_drop(void *self) {
    struct rw_box *b = self;
    if (b->awaiting) {
        cexec_future_drop(b->readiness);
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct rw_box));
}

static const struct cexec_future_vtable rw_vtable = {
    .poll = rw_poll,
    .drop = rw_drop,
};

static struct cexec_future rw_new(struct cstd_alloc a, struct cexec_stream *s,
                                  void *buf, usize len, bool is_write) {
    struct cexec_future f = null_future();
    struct rw_box *b = cstd_alloc_allocate(a, sizeof *b, _Alignof(struct rw_box));
    if (!b) {
        return f;
    }
    b->alloc     = a;
    b->stream    = s;
    b->buf       = buf;
    b->len       = len;
    b->is_write  = is_write;
    b->awaiting  = false;
    b->readiness = null_future();
    f.self       = b;
    f.vtable     = &rw_vtable;
    return f;
}

/* Read up to `len` bytes into `buf`. Yields isize: bytes read
 * (0 = EOF), or -errno. */
[[cust::pub]] struct cexec_future cexec_stream_read(struct cstd_alloc a,
                                                    struct cexec_stream *s,
                                                    void *buf, usize len) {
    return rw_new(a, s, buf, len, false);
}

/* Write up to `len` bytes from `buf` (a single write — may be
 * partial). Yields isize: bytes written, or -errno. */
[[cust::pub]] struct cexec_future cexec_stream_write(struct cstd_alloc a,
                                                     struct cexec_stream *s,
                                                     const void *buf, usize len) {
    return rw_new(a, s, (void *)buf, len, true);
}

/* ─── write_all ─────────────────────────────────────────── */

struct writeall_box {
    struct cstd_alloc    alloc;
    struct cexec_stream *stream;
    const u8            *buf;
    usize                len;
    usize                off;
    bool                 awaiting;
    struct cexec_future  readiness;
};

static cexec_poll writeall_poll(void *self, struct cexec_waker w, void *out) {
    struct writeall_box *b = self;
    int fd = cexec_fd_raw(&b->stream->fd);
    for (;;) {
        if (b->awaiting) {
            if (!cexec_poll_is_ready(cexec_future_poll(b->readiness, w, (void *)0))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(b->readiness);
            b->readiness = null_future();
            b->awaiting  = false;
        }
        if (b->off >= b->len) {
            if (out) {
                *(isize *)out = (isize)b->len;
            }
            return cexec_poll_ready();
        }
        ssize_t n = write(fd, b->buf + b->off, b->len - b->off);
        if (n > 0) {
            b->off += (usize)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            b->readiness = cexec_fd_writable(&b->stream->fd);
            b->awaiting  = true;
            continue;
        }
        /* n == 0 (unexpected) or a hard error. */
        if (out) {
            *(isize *)out = (n < 0) ? -(isize)errno : (isize)b->off;
        }
        return cexec_poll_ready();
    }
}

static void writeall_drop(void *self) {
    struct writeall_box *b = self;
    if (b->awaiting) {
        cexec_future_drop(b->readiness);
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct writeall_box));
}

static const struct cexec_future_vtable writeall_vtable = {
    .poll = writeall_poll,
    .drop = writeall_drop,
};

/* Write the whole buffer, awaiting writability between partial
 * writes. Yields isize: `len` on success, or -errno. */
[[cust::pub]] struct cexec_future cexec_stream_write_all(struct cstd_alloc a,
                                                         struct cexec_stream *s,
                                                         const void *buf,
                                                         usize len) {
    struct cexec_future f = null_future();
    struct writeall_box *b = cstd_alloc_allocate(a, sizeof *b,
                                                 _Alignof(struct writeall_box));
    if (!b) {
        return f;
    }
    b->alloc     = a;
    b->stream    = s;
    b->buf       = buf;
    b->len       = len;
    b->off       = 0;
    b->awaiting  = false;
    b->readiness = null_future();
    f.self       = b;
    f.vtable     = &writeall_vtable;
    return f;
}

/* ─── accept ────────────────────────────────────────────── */

struct accept_box {
    struct cstd_alloc      alloc;
    struct cexec_listener *lis;
    bool                   awaiting;
    struct cexec_future    readiness;
};

static cexec_poll accept_poll(void *self, struct cexec_waker w, void *out) {
    struct accept_box *b = self;
    struct cstd_alloc a = b->lis->fd.alloc;
    struct cexec_io_driver *d = b->lis->fd.driver;
    int lfd = cexec_fd_raw(&b->lis->fd);
    for (;;) {
        if (b->awaiting) {
            if (!cexec_poll_is_ready(cexec_future_poll(b->readiness, w, (void *)0))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(b->readiness);
            b->readiness = null_future();
            b->awaiting  = false;
        }
        int cfd = accept(lfd, (struct sockaddr *)0, (socklen_t *)0);
        if (cfd >= 0) {
            struct cexec_stream *os = out;
            if (os) {
                make_closed_stream(os, a, d);
                cexec_fd_adopt(&os->fd, a, d, cfd); /* nonblock + register */
            } else {
                close(cfd);
            }
            return cexec_poll_ready();
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            b->readiness = cexec_fd_readable(&b->lis->fd);
            b->awaiting  = true;
            continue;
        }
        if (out) {
            make_closed_stream(out, a, d);
        }
        return cexec_poll_ready();
    }
}

static void accept_drop(void *self) {
    struct accept_box *b = self;
    if (b->awaiting) {
        cexec_future_drop(b->readiness);
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct accept_box));
}

static const struct cexec_future_vtable accept_vtable = {
    .poll = accept_poll,
    .drop = accept_drop,
};

/* Accept one connection. Yields a `cexec_stream` via `out`
 * (check cexec_stream_is_open for success). */
[[cust::pub]] struct cexec_future cexec_listener_accept(struct cstd_alloc a,
                                                        struct cexec_listener *l) {
    struct cexec_future f = null_future();
    struct accept_box *b = cstd_alloc_allocate(a, sizeof *b,
                                               _Alignof(struct accept_box));
    if (!b) {
        return f;
    }
    b->alloc     = a;
    b->lis       = l;
    b->awaiting  = false;
    b->readiness = null_future();
    f.self       = b;
    f.vtable     = &accept_vtable;
    return f;
}

/* ─── connect ───────────────────────────────────────────── */

struct connect_box {
    struct cstd_alloc        alloc;
    struct cexec_io_driver  *driver;
    int                      family;
    struct sockaddr_storage  addr;
    u32                      addrlen;
    struct cexec_stream      stream; /* built on first poll; fd=-1 until then */
    bool                     started;
    bool                     moved;  /* stream handed to out */
    bool                     awaiting;
    struct cexec_future      readiness;
};

static void connect_hand_off(struct connect_box *b, void *out) {
    if (out) {
        *(struct cexec_stream *)out = b->stream;
    }
    b->moved = true;
    b->stream.fd.fd = -1; /* so connect_drop won't double-close */
}

static cexec_poll connect_poll(void *self, struct cexec_waker w, void *out) {
    struct connect_box *b = self;

    if (!b->started) {
        b->started = true;
        int fd = socket(b->family, SOCK_STREAM, 0);
        if (fd < 0) {
            make_closed_stream(out, b->alloc, b->driver);
            return cexec_poll_ready();
        }
        if (!cexec_fd_adopt(&b->stream.fd, b->alloc, b->driver, fd)) {
            make_closed_stream(out, b->alloc, b->driver);
            return cexec_poll_ready();
        }
        int r = connect(fd, (struct sockaddr *)&b->addr, (socklen_t)b->addrlen);
        if (r == 0) {
            connect_hand_off(b, out);
            return cexec_poll_ready();
        }
        if (errno != EINPROGRESS) {
            cexec_stream_close(&b->stream);
            make_closed_stream(out, b->alloc, b->driver);
            return cexec_poll_ready();
        }
        b->readiness = cexec_fd_writable(&b->stream.fd);
        b->awaiting  = true;
    }

    if (b->awaiting) {
        if (!cexec_poll_is_ready(cexec_future_poll(b->readiness, w, (void *)0))) {
            return cexec_poll_pending();
        }
        cexec_future_drop(b->readiness);
        b->readiness = null_future();
        b->awaiting  = false;

        int soerr = 0;
        socklen_t sl = sizeof soerr;
        getsockopt(cexec_fd_raw(&b->stream.fd), SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr == 0) {
            connect_hand_off(b, out);
        } else {
            cexec_stream_close(&b->stream);
            make_closed_stream(out, b->alloc, b->driver);
        }
    }
    return cexec_poll_ready();
}

static void connect_drop(void *self) {
    struct connect_box *b = self;
    if (b->awaiting) {
        cexec_future_drop(b->readiness);
    }
    if (!b->moved && cexec_fd_is_open(&b->stream.fd)) {
        cexec_stream_close(&b->stream);
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct connect_box));
}

static const struct cexec_future_vtable connect_vtable = {
    .poll = connect_poll,
    .drop = connect_drop,
};

/* Open a stream socket in `family` and connect it to `addr`.
 * Yields a `cexec_stream` via `out` (check cexec_stream_is_open).
 * The per-family modules build the sockaddr and call this. */
[[cust::pub]] struct cexec_future cexec_net_connect(struct cstd_alloc a,
                                                    struct cexec_io_driver *d,
                                                    int family,
                                                    const struct sockaddr *addr,
                                                    u32 addrlen) {
    struct cexec_future f = null_future();
    struct connect_box *b = cstd_alloc_allocate(a, sizeof *b,
                                                _Alignof(struct connect_box));
    if (!b) {
        return f;
    }
    b->alloc   = a;
    b->driver  = d;
    b->family  = family;
    b->addrlen = addrlen;
    memcpy(&b->addr, addr, addrlen);
    make_closed_stream(&b->stream, a, d);
    b->started   = false;
    b->moved     = false;
    b->awaiting  = false;
    b->readiness = null_future();
    f.self       = b;
    f.vtable     = &connect_vtable;
    return f;
}

/* ─── listen (synchronous setup) ────────────────────────── */

/* Create a stream socket in `family`, optionally set SO_REUSEADDR,
 * bind to `addr`, listen, and adopt into `out`. Returns false on
 * any failure (with `out` left closed). */
[[cust::pub]] bool cexec_net_listen(struct cexec_listener *out,
                                    struct cstd_alloc a,
                                    struct cexec_io_driver *d, int family,
                                    const struct sockaddr *addr, u32 addrlen,
                                    int backlog, bool reuseaddr) {
    out->fd.alloc  = a;
    out->fd.driver = d;
    out->fd.fd     = -1;

    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    if (reuseaddr) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    }
    if (bind(fd, addr, (socklen_t)addrlen) != 0 || listen(fd, backlog) != 0) {
        close(fd);
        return false;
    }
    return cexec_fd_adopt(&out->fd, a, d, fd); /* nonblock + register */
}

/* ─── unit tests ────────────────────────────────────────── */

#cust use crate::executor;

#include <sys/socket.h>

[[cust::test]] int test_stream_write_all_then_read(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    struct cexec_stream wr, rd;
    cust_assert(cexec_fd_adopt(&wr.fd, a, &io, sv[0]));
    cust_assert(cexec_fd_adopt(&rd.fd, a, &io, sv[1]));

    isize wrote = 0;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_write_all(a, &wr, "hello", 5), &wrote));
    cust_assert_eq((i64)wrote, (i64)5);

    char buf[8] = {0};
    isize got = 0;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_read(a, &rd, buf, sizeof buf), &got));
    cust_assert_eq((i64)got, (i64)5);
    cust_assert(buf[0] == 'h' && buf[4] == 'o');

    cexec_stream_close(&wr);
    cexec_stream_close(&rd);
    cexec_io_driver_close(&io);
    return 0;
}
