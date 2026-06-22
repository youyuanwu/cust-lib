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
 * would. The server and client are expressed with the
 * `cexec_sm` state-machine combinator: each is a flat table of
 * state functions over a caller-owned context struct (here, a
 * plain stack local in the test), so the suspend/resume
 * plumbing lives in the combinator, not hand-rolled here. The
 * server uses no-I/O decision states for its accept-ok and
 * EOF branches; both sides thread their stream/buffer/count
 * through the context across suspends.
 */

#cust use std;
#cust use cust_execution;

#include <string.h>

/* ─── server: accept → read → echo ──────────────────────── */
/* states: accept, check-accept (branch), read, check-read
 * (EOF branch), echo. The two no-I/O "check" states are the
 * if/else nodes; the rest await real socket readiness. */

struct echo_ctx {
    struct cstd_alloc      a;
    struct cexec_listener *lis;
    struct cexec_stream    stream;
    bool                   have_stream;
    char                   buf[64];
    isize                  n;
};

enum { SV_ACCEPT, SV_AFTER_ACCEPT, SV_READ, SV_AFTER_READ, SV_ECHO };

static struct cexec_step sv_accept(void *p) {
    struct echo_ctx *c = p;
    return (struct cexec_step){
        cexec_listener_accept(c->a, c->lis), &c->stream, SV_AFTER_ACCEPT };
}

static struct cexec_step sv_after_accept(void *p) {
    struct echo_ctx *c = p;
    c->have_stream = cexec_stream_is_open(&c->stream);
    return (struct cexec_step){
        cexec_future_null(), (void *)0,
        c->have_stream ? SV_READ : cexec_sm_done() };
}

static struct cexec_step sv_read(void *p) {
    struct echo_ctx *c = p;
    return (struct cexec_step){
        cexec_stream_read(c->a, &c->stream, c->buf, sizeof c->buf),
        &c->n, SV_AFTER_READ };
}

static struct cexec_step sv_after_read(void *p) {
    struct echo_ctx *c = p;
    return (struct cexec_step){
        cexec_future_null(), (void *)0,
        (c->n > 0) ? SV_ECHO : cexec_sm_done() }; /* EOF/err -> stop */
}

static struct cexec_step sv_echo(void *p) {
    struct echo_ctx *c = p;
    return (struct cexec_step){
        cexec_stream_write_all(c->a, &c->stream, c->buf, (usize)c->n),
        (void *)0, cexec_sm_done() };
}

static const cexec_state_fn server_states[] = {
    sv_accept, sv_after_accept, sv_read, sv_after_read, sv_echo,
};

/* ─── client: connect → write → read + verify ───────────── */

struct echo_client_ctx {
    struct cstd_alloc       a;
    struct cexec_io_driver *io;
    u16                     port;
    i32                    *ok;          /* set to 1 on verified echo */
    struct cexec_stream     stream;
    bool                    have_stream;
    char                    buf[64];
    isize                   n;
};

enum { CL_CONNECT, CL_AFTER_CONNECT, CL_WRITE, CL_READ, CL_VERIFY };

static struct cexec_step cl_connect(void *p) {
    struct echo_client_ctx *c = p;
    return (struct cexec_step){
        cexec_tcp_connect(c->a, c->io, "127.0.0.1", c->port),
        &c->stream, CL_AFTER_CONNECT };
}

static struct cexec_step cl_after_connect(void *p) {
    struct echo_client_ctx *c = p;
    c->have_stream = cexec_stream_is_open(&c->stream);
    if (!c->have_stream) {
        *c->ok = 0;
        return (struct cexec_step){ cexec_future_null(), (void *)0,
                                    cexec_sm_done() };
    }
    return (struct cexec_step){ cexec_future_null(), (void *)0, CL_WRITE };
}

static struct cexec_step cl_write(void *p) {
    struct echo_client_ctx *c = p;
    return (struct cexec_step){
        cexec_stream_write_all(c->a, &c->stream, "ping", 4),
        (void *)0, CL_READ };
}

static struct cexec_step cl_read(void *p) {
    struct echo_client_ctx *c = p;
    return (struct cexec_step){
        cexec_stream_read(c->a, &c->stream, c->buf, sizeof c->buf),
        &c->n, CL_VERIFY };
}

static struct cexec_step cl_verify(void *p) {
    struct echo_client_ctx *c = p;
    *c->ok = (c->n == 4 && memcmp(c->buf, "ping", 4) == 0) ? 1 : 0;
    return (struct cexec_step){ cexec_future_null(), (void *)0,
                                cexec_sm_done() };
}

static const cexec_state_fn client_states[] = {
    cl_connect, cl_after_connect, cl_write, cl_read, cl_verify,
};

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

    /* The contexts are plain stack locals: block_on drives the
     * machines to completion synchronously, so they outlive the
     * run and we clean up their streams afterward. */
    i32 ok = 0;
    struct echo_ctx sv = {
        .a = a, .lis = &lis, .have_stream = false, .n = 0,
    };
    struct echo_client_ctx cl = {
        .a = a, .io = &io, .port = port, .ok = &ok,
        .have_stream = false, .n = 0,
    };

    /* One block_on drives server + client concurrently. The
     * server's accept is polled first and must Pend until the
     * client's concurrent connect arrives — exercising the real
     * async suspend/resume path. */
    struct cexec_future f = cexec_join2_in(
        a, cexec_sm_in(a, &sv, server_states, 5, SV_ACCEPT), 0,
        cexec_sm_in(a, &cl, client_states, 5, CL_CONNECT));
    bool done = cexec_executor_block_on(&ex, f, (void *)0);

    cust_assert(done);
    cust_assert_eq((i32)ok, (i32)1);

    if (sv.have_stream) {
        cexec_stream_close(&sv.stream);
    }
    if (cl.have_stream) {
        cexec_stream_close(&cl.stream);
    }
    cexec_listener_close(&lis);
    cexec_io_driver_close(&io);
    return 0;
}
