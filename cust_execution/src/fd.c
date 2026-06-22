/* cust_execution::fd — owned, registered file descriptor handle.
 *
 * The RAII-ish wrapper that turns the manual three-step fd dance
 * (cexec_io_add → … → cexec_io_remove → close) into one owned
 * value. `cexec_fd_adopt` takes ownership of an open fd, sets it
 * non-blocking, and registers it with the I/O driver; the read /
 * write readiness futures are then obtained from the handle, so
 * call sites never juggle a raw fd + driver pair. `cexec_fd_close`
 * deregisters and closes exactly once.
 *
 *   Rust/Tokio              cust_execution
 *   --------------------    ---------------------------
 *   AsyncFd<T> / OwnedFd    struct cexec_fd
 *   AsyncFd::new            cexec_fd_adopt
 *   (Drop)                  cexec_fd_close  (explicit — no Drop)
 *   .readable()/.writable() cexec_fd_readable / cexec_fd_writable
 *
 * C has no destructors, so ownership is by convention: call
 * `cexec_fd_close` exactly once. It is idempotent (safe to call
 * on an already-closed handle), and any I/O future obtained from
 * the handle must not outlive the handle's registration.
 *
 * This is the foundation the socket types build on: a TCP
 * listener, each accepted connection, and the connect side are
 * each a `cexec_fd`, so their fd lifetime and epoll registration
 * are managed in one place.
 */

#cust use std;
#cust use crate::future;
#cust use crate::reactor;
#cust use crate::io;

#include <unistd.h>
#include <fcntl.h>

struct [[cust::pub_repr]] cexec_fd {
    struct cstd_alloc       alloc;
    struct cexec_io_driver *driver;
    int                     fd;    /* -1 once closed/empty */
};

/* Take ownership of `fd`: make it non-blocking and register it
 * with `d`. On failure the fd is closed and `out` is left empty
 * (fd = -1). Returns whether registration succeeded. */
[[cust::pub]] bool cexec_fd_adopt(struct cexec_fd *out, struct cstd_alloc a,
                                  struct cexec_io_driver *d, int fd) {
    out->alloc  = a;
    out->driver = d;
    out->fd     = -1;

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (!cexec_io_add(d, fd)) {
        close(fd);
        return false;
    }
    out->fd = fd;
    return true;
}

[[cust::pub]] int cexec_fd_raw(const struct cexec_fd *h) {
    return h->fd;
}

[[cust::pub]] bool cexec_fd_is_open(const struct cexec_fd *h) {
    return h->fd >= 0;
}

/* Deregister and close the fd. Idempotent. */
[[cust::pub]] void cexec_fd_close(struct cexec_fd *h) {
    if (h->fd >= 0) {
        cexec_io_remove(h->driver, h->fd);
        close(h->fd);
        h->fd = -1;
    }
}

/* Readiness futures bound to this handle (resolve when the fd is
 * readable / writable; the caller then does the real syscall and
 * re-awaits on EAGAIN). */
[[cust::pub]] struct cexec_future cexec_fd_readable(struct cexec_fd *h) {
    return cexec_readable_in(h->alloc, h->driver, h->fd);
}

[[cust::pub]] struct cexec_future cexec_fd_writable(struct cexec_fd *h) {
    return cexec_writable_in(h->alloc, h->driver, h->fd);
}

/* ─── unit tests ────────────────────────────────────────── */

#cust use crate::executor;
#cust use crate::combinators;

#include <sys/socket.h>

[[cust::test]] int test_fd_adopt_and_close(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    struct cexec_fd h;
    cust_assert(cexec_fd_adopt(&h, a, &io, sv[0]));
    cust_assert(cexec_fd_is_open(&h));
    cust_assert(cexec_fd_raw(&h) == sv[0]);

    cexec_fd_close(&h);
    cust_assert(!cexec_fd_is_open(&h));
    cexec_fd_close(&h); /* idempotent */

    close(sv[1]);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_fd_readable_roundtrip(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    struct cexec_fd h;
    cust_assert(cexec_fd_adopt(&h, a, &io, sv[0]));

    cust_assert(write(sv[1], "q", 1) == 1);
    cust_assert(cexec_executor_block_on(&ex, cexec_fd_readable(&h), (void *)0));

    char buf = 0;
    cust_assert(read(cexec_fd_raw(&h), &buf, 1) == 1);
    cust_assert(buf == 'q');

    cexec_fd_close(&h);
    close(sv[1]);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_fd_full_duplex(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    struct cexec_fd h;
    cust_assert(cexec_fd_adopt(&h, a, &io, sv[0]));

    /* Await readable AND writable on the one handle at once. */
    cust_assert(write(sv[1], "d", 1) == 1);
    struct cexec_future f =
        cexec_join2_in(a, cexec_fd_readable(&h), 0, cexec_fd_writable(&h));
    cust_assert(cexec_executor_block_on(&ex, f, (void *)0));

    char buf = 0;
    cust_assert(read(cexec_fd_raw(&h), &buf, 1) == 1);
    cust_assert(buf == 'd');

    cexec_fd_close(&h);
    close(sv[1]);
    cexec_io_driver_close(&io);
    return 0;
}
