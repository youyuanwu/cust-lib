/* Integration test: concurrent async TCP echo over loopback.
 *
 * Unlike the in-module tcp test (which front-loads every step as
 * sequential block_on calls and so only exercises the syscall
 * fast path), this drives BOTH sides on a single executor inside
 * one block_on(join2(server, client)). The server's `accept` is
 * polled *before* the client connects, so it genuinely returns
 * Pending and parks on the listener's readability — then the
 * concurrent client's connect makes the listener readable and
 * epoll wakes the parked accept. This is what actually proves
 * the accept/connect/read futures suspend and resume, not just
 * that the syscalls are wired up.
 *
 * Reaches the crate only through its public surface
 * (`#cust use cust_execution;`), the way a downstream consumer
 * would. The server/client are hand-written multi-step state
 * machine futures (the manual form of an async fn) because the
 * `then` combinator discards its first future's output and so
 * can't thread the accepted/connected stream forward.
 */

#cust use std;
#cust use cust_execution;

#include <string.h>

/* ─── server: accept → read → echo ──────────────────────── */

struct server_fut {
    struct cstd_alloc      a;
    struct cexec_listener *lis;
    struct cexec_stream    stream;
    bool                   have_stream;
    char                   buf[64];
    isize                  n;
    u8                     stage; /* 0 accept, 1 read, 2 write, 3 done */
    struct cexec_future    sub;
    bool                   have_sub;
};

static cexec_poll server_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct server_fut *s = self;
    for (;;) {
        if (s->stage == 0) {
            if (!s->have_sub) {
                s->sub = cexec_listener_accept(s->a, s->lis);
                s->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(s->sub, w, &s->stream))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(s->sub);
            s->have_sub = false;
            s->have_stream = cexec_stream_is_open(&s->stream);
            if (!s->have_stream) {
                return cexec_poll_ready();
            }
            s->stage = 1;
            continue;
        }
        if (s->stage == 1) {
            if (!s->have_sub) {
                s->sub = cexec_stream_read(s->a, &s->stream, s->buf, sizeof s->buf);
                s->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(s->sub, w, &s->n))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(s->sub);
            s->have_sub = false;
            if (s->n <= 0) {
                return cexec_poll_ready();
            }
            s->stage = 2;
            continue;
        }
        if (s->stage == 2) {
            if (!s->have_sub) {
                s->sub = cexec_stream_write_all(s->a, &s->stream, s->buf,
                                                (usize)s->n);
                s->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(s->sub, w, (void *)0))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(s->sub);
            s->have_sub = false;
            s->stage = 3;
            return cexec_poll_ready();
        }
        return cexec_poll_ready();
    }
}

static void server_drop(void *self) {
    struct server_fut *s = self;
    if (s->have_sub) {
        cexec_future_drop(s->sub);
    }
    if (s->have_stream) {
        cexec_stream_close(&s->stream);
    }
    cstd_alloc_deallocate(s->a, s, sizeof *s, _Alignof(struct server_fut));
}

static const struct cexec_future_vtable server_vtable = {
    .poll = server_poll,
    .drop = server_drop,
};

static struct cexec_future server_future(struct cstd_alloc a,
                                         struct cexec_listener *lis) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct server_fut *s = cstd_alloc_allocate(a, sizeof *s,
                                               _Alignof(struct server_fut));
    if (!s) {
        return f;
    }
    s->a           = a;
    s->lis         = lis;
    s->have_stream = false;
    s->n           = 0;
    s->stage       = 0;
    s->have_sub    = false;
    f.self         = s;
    f.vtable       = &server_vtable;
    return f;
}

/* ─── client: connect → write → read + verify ───────────── */

struct client_fut {
    struct cstd_alloc       a;
    struct cexec_io_driver *io;
    u16                     port;
    i32                    *ok;          /* set to 1 on verified echo */
    struct cexec_stream     stream;
    bool                    have_stream;
    char                    buf[64];
    isize                   n;
    u8                      stage; /* 0 connect, 1 write, 2 read, 3 done */
    struct cexec_future     sub;
    bool                    have_sub;
};

static cexec_poll client_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct client_fut *c = self;
    for (;;) {
        if (c->stage == 0) {
            if (!c->have_sub) {
                c->sub = cexec_tcp_connect(c->a, c->io, "127.0.0.1", c->port);
                c->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(c->sub, w, &c->stream))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(c->sub);
            c->have_sub = false;
            c->have_stream = cexec_stream_is_open(&c->stream);
            if (!c->have_stream) {
                *c->ok = 0;
                return cexec_poll_ready();
            }
            c->stage = 1;
            continue;
        }
        if (c->stage == 1) {
            if (!c->have_sub) {
                c->sub = cexec_stream_write_all(c->a, &c->stream, "ping", 4);
                c->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(c->sub, w, (void *)0))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(c->sub);
            c->have_sub = false;
            c->stage = 2;
            continue;
        }
        if (c->stage == 2) {
            if (!c->have_sub) {
                c->sub = cexec_stream_read(c->a, &c->stream, c->buf, sizeof c->buf);
                c->have_sub = true;
            }
            if (!cexec_poll_is_ready(cexec_future_poll(c->sub, w, &c->n))) {
                return cexec_poll_pending();
            }
            cexec_future_drop(c->sub);
            c->have_sub = false;
            *c->ok = (c->n == 4 && memcmp(c->buf, "ping", 4) == 0) ? 1 : 0;
            c->stage = 3;
            return cexec_poll_ready();
        }
        return cexec_poll_ready();
    }
}

static void client_drop(void *self) {
    struct client_fut *c = self;
    if (c->have_sub) {
        cexec_future_drop(c->sub);
    }
    if (c->have_stream) {
        cexec_stream_close(&c->stream);
    }
    cstd_alloc_deallocate(c->a, c, sizeof *c, _Alignof(struct client_fut));
}

static const struct cexec_future_vtable client_vtable = {
    .poll = client_poll,
    .drop = client_drop,
};

static struct cexec_future client_future(struct cstd_alloc a,
                                         struct cexec_io_driver *io,
                                         u16 port, i32 *ok) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct client_fut *c = cstd_alloc_allocate(a, sizeof *c,
                                               _Alignof(struct client_fut));
    if (!c) {
        return f;
    }
    c->a           = a;
    c->io          = io;
    c->port        = port;
    c->ok          = ok;
    c->have_stream = false;
    c->n           = 0;
    c->stage       = 0;
    c->have_sub    = false;
    f.self         = c;
    f.vtable       = &client_vtable;
    return f;
}

/* ─── the test ──────────────────────────────────────────── */

[[cust::test]] int test_concurrent_tcp_echo(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    struct cexec_listener lis;
    cust_assert(cexec_tcp_listen(&lis, a, &io, "127.0.0.1", 0, 16));
    u16 port = cexec_tcp_local_port(&lis);
    cust_assert(port != 0);

    /* One block_on drives server + client concurrently. The
     * server's accept is polled first and must Pend until the
     * client's concurrent connect arrives — exercising the real
     * async suspend/resume path. */
    i32 ok = 0;
    struct cexec_future f =
        cexec_join2_in(a, server_future(a, &lis), 0,
                       client_future(a, &io, port, &ok));
    bool done = cexec_executor_block_on(&ex, f, (void *)0);

    cust_assert(done);
    cust_assert_eq((i32)ok, (i32)1);

    cexec_listener_close(&lis);
    cexec_io_driver_close(&io);
    return 0;
}
