/* cust_execution::tcp — TCP address shim over the stream core.
 *
 * The thin per-family layer: everything async (accept, connect,
 * read, write) lives in stream.c; this module only builds an
 * IPv4 `sockaddr_in` from a numeric address and forwards to the
 * generic `cexec_net_listen` / `cexec_net_connect`. A Unix-socket
 * module would be the same shape with `sockaddr_un`.
 *
 *   Rust/Tokio              cust_execution
 *   --------------------    ---------------------------
 *   TcpListener::bind       cexec_tcp_listen
 *   TcpStream::connect      cexec_tcp_connect
 *   local_addr().port()     cexec_tcp_local_port
 *
 * Numeric addresses only (inet_pton); DNS (getaddrinfo) blocks
 * and needs a thread-pool offload, so it is deferred. The
 * returned listener / stream are the same `cexec_listener` /
 * `cexec_stream` the rest of the runtime operates on.
 */

#cust use std;
#cust use crate::future;
#cust use crate::reactor;
#cust use crate::io;
#cust use crate::fd;
#cust use crate::stream;

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static bool make_v4(struct sockaddr_in *sa, const char *ip, u16 port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    return inet_pton(AF_INET, ip, &sa->sin_addr) == 1;
}

/* Bind + listen on `ip:port` (numeric IPv4, e.g. "127.0.0.1").
 * Port 0 lets the kernel choose — read it back with
 * cexec_tcp_local_port. Returns false on failure. */
[[cust::pub]] bool cexec_tcp_listen(struct cexec_listener *out,
                                    struct cstd_alloc a,
                                    struct cexec_io_driver *d,
                                    const char *ip, u16 port, int backlog) {
    struct sockaddr_in sa;
    if (!make_v4(&sa, ip, port)) {
        out->fd.alloc  = a;
        out->fd.driver = d;
        out->fd.fd     = -1;
        return false;
    }
    return cexec_net_listen(out, a, d, AF_INET, (struct sockaddr *)&sa,
                            (u32)sizeof sa, backlog, true);
}

/* Connect to `ip:port` (numeric IPv4). Yields a cexec_stream via
 * the future's `out` (check cexec_stream_is_open). */
[[cust::pub]] struct cexec_future cexec_tcp_connect(struct cstd_alloc a,
                                                    struct cexec_io_driver *d,
                                                    const char *ip, u16 port) {
    struct sockaddr_in sa;
    if (!make_v4(&sa, ip, port)) {
        struct cexec_future f = {(void *)0, (void *)0};
        return f;
    }
    return cexec_net_connect(a, d, AF_INET, (struct sockaddr *)&sa,
                             (u32)sizeof sa);
}

/* The actual local port a listener is bound to (resolves port 0
 * to the kernel-chosen one). Returns 0 on error. */
[[cust::pub]] u16 cexec_tcp_local_port(const struct cexec_listener *l) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getsockname(cexec_listener_raw(l), (struct sockaddr *)&sa, &sl) != 0) {
        return 0;
    }
    return ntohs(sa.sin_port);
}

/* ─── unit tests ────────────────────────────────────────── */

#cust use crate::executor;

#include <unistd.h>

/* End-to-end loopback echo: connect → accept → client writes →
 * server reads + echoes → client reads back. TCP loopback
 * completes the handshake in-kernel (the listen backlog accepts
 * the SYN), so non-blocking connect succeeds before accept is
 * called and the steps can run as sequential block_on calls. */
[[cust::test]] int test_tcp_echo_roundtrip(void) {
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

    /* Client connects (handshake completes in-kernel). */
    struct cexec_stream client;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_tcp_connect(a, &io, "127.0.0.1", port), &client));
    cust_assert(cexec_stream_is_open(&client));

    /* Server accepts the queued connection. */
    struct cexec_stream server;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_listener_accept(a, &lis), &server));
    cust_assert(cexec_stream_is_open(&server));

    /* Client → server. */
    isize w = 0;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_write_all(a, &client, "ping", 4), &w));
    cust_assert_eq((i64)w, (i64)4);

    char buf[8] = {0};
    isize r = 0;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_read(a, &server, buf, sizeof buf), &r));
    cust_assert_eq((i64)r, (i64)4);
    cust_assert(buf[0] == 'p' && buf[3] == 'g');

    /* Server echoes back. */
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_write_all(a, &server, buf, 4), &w));
    cust_assert_eq((i64)w, (i64)4);

    char back[8] = {0};
    cust_assert(cexec_executor_block_on(
        &ex, cexec_stream_read(a, &client, back, sizeof back), &r));
    cust_assert_eq((i64)r, (i64)4);
    cust_assert(back[0] == 'p' && back[3] == 'g');

    cexec_stream_close(&client);
    cexec_stream_close(&server);
    cexec_listener_close(&lis);
    cexec_io_driver_close(&io);
    return 0;
}

/* Connecting to a refused port resolves to a closed stream
 * rather than crashing (ERR/HUP path through writable). */
[[cust::test]] int test_tcp_connect_refused(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    /* Port 1 on loopback is almost certainly not listening. */
    struct cexec_stream s;
    cust_assert(cexec_executor_block_on(
        &ex, cexec_tcp_connect(a, &io, "127.0.0.1", 1), &s));
    cust_assert(!cexec_stream_is_open(&s));

    cexec_io_driver_close(&io);
    return 0;
}
