#include "defs.h"
#include "common.h"
#include "s5.h"
#include "obfs.h"
#include "ssrbuffer.h"
#include "dump_info.h"
#include "ssr_executive.h"
#include "encrypt.h"
#include "tunnel.h"

/* A connection is modeled as an abstraction on top of two simple state
 * machines, one for reading and one for writing.  Either state machine
 * is, when active, in one of three states: busy, done or stop; the fourth
 * and final state, dead, is an end state and only relevant when shutting
 * down the connection.  A short overview:
 *
 *                          busy                  done           stop
 *  ----------|---------------------------|--------------------|------|
 *  readable  | waiting for incoming data | have incoming data | idle |
 *  writable  | busy writing out data     | completed write    | idle |
 *
 * We could remove the done state from the writable state machine. For our
 * purposes, it's functionally equivalent to the stop state.
 *
 * When the connection with upstream has been established, the struct tunnel_ctx
 * moves into a state where incoming data from the client is sent upstream
 * and vice versa, incoming data from upstream is sent to the client.  In
 * other words, we're just piping data back and forth.  See do_proxy()
 * for details.
 *
 * An interesting deviation from libuv's I/O model is that reads are discrete
 * rather than continuous events.  In layman's terms, when a read operation
 * completes, the connection stops reading until further notice.
 *
 * The rationale for this approach is that we have to wait until the data
 * has been sent out again before we can reuse the read buffer.
 *
 * It also pleasingly unifies with the request model that libuv uses for
 * writes and everything else; libuv may switch to a request model for
 * reads in the future.
 */

 /* Session states. */
enum session_state {
    session_handshake,        /* Wait for client handshake. */
    session_handshake_auth,   /* Wait for client authentication data. */
    session_req_start,        /* Start waiting for request data. */
    session_req_parse,        /* Wait for request data. */
    session_req_udp_accoc,
    session_req_lookup,       /* Wait for upstream hostname DNS lookup to complete. */
    session_req_connect,      /* Wait for uv_tcp_connect() to complete. */
    session_ssr_auth_sent,
    session_proxy_start,      /* Connected. Start piping data. */
    session_proxy,            /* Connected. Pipe data back and forth. */
    session_kill,             /* Tear down session. */
};

struct client_ctx {
    struct server_env_t *env; // __weak_ptr
    struct tunnel_cipher_ctx *cipher;
    struct buffer_t *init_pkg;
    s5_ctx parser;  /* The SOCKS protocol parser. */
    enum session_state state;
};

static struct buffer_t * initial_package_create(const s5_ctx *parser);
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void do_handshake(struct tunnel_ctx *tunnel);
static void do_handshake_auth(struct tunnel_ctx *tunnel);
static void do_req_start(struct tunnel_ctx *tunnel);
static void do_req_parse(struct tunnel_ctx *tunnel);
static void do_req_lookup(struct tunnel_ctx *tunnel);
static void do_req_connect_start(struct tunnel_ctx *tunnel);
static void do_req_connect(struct tunnel_ctx *tunnel);
static void do_ssr_auth_sent(struct tunnel_ctx *tunnel);
static void do_proxy_start(struct tunnel_ctx *tunnel);
static void do_proxy(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_dying(struct tunnel_ctx *tunnel);
static void tunnel_timeout_expire_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_outgoing_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_getaddrinfo_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket);
static size_t tunnel_get_alloc_size(struct tunnel_ctx *tunnel, size_t suggested_size);
static bool tunnel_in_streaming(struct tunnel_ctx *tunnel);
static bool can_auth_none(const uv_tcp_t *lx, const struct tunnel_ctx *cx);
static bool can_auth_passwd(const uv_tcp_t *lx, const struct tunnel_ctx *cx);
static bool can_access(const uv_tcp_t *lx, const struct tunnel_ctx *cx, const struct sockaddr *addr);

static bool init_done_cb(struct tunnel_ctx *tunnel, void *p) {
    struct server_env_t *env = (struct server_env_t *)p;

    struct client_ctx *ctx = (struct client_ctx *) calloc(1, sizeof(struct client_ctx));
    ctx->env = env;
    tunnel->data = ctx;

    tunnel->tunnel_dying = &tunnel_dying;
    tunnel->tunnel_timeout_expire_done = &tunnel_timeout_expire_done;
    tunnel->tunnel_outgoing_connected_done = &tunnel_outgoing_connected_done;
    tunnel->tunnel_read_done = &tunnel_read_done;
    tunnel->tunnel_getaddrinfo_done = &tunnel_getaddrinfo_done;
    tunnel->tunnel_write_done = &tunnel_write_done;
    tunnel->tunnel_get_alloc_size = &tunnel_get_alloc_size;
    tunnel->tunnel_in_streaming = &tunnel_in_streaming;

    objects_container_add(ctx->env->tunnel_set, tunnel);

    s5_init(&ctx->parser);
    ctx->cipher = NULL;
    ctx->state = session_handshake;

    return true;
}

void client_tunnel_initialize(uv_tcp_t *lx, unsigned int idle_timeout) {
    uv_loop_t *loop = lx->loop;
    struct server_env_t *env = (struct server_env_t *)loop->data;

    tunnel_initialize(lx, idle_timeout, &init_done_cb, env);
}

static void _do_shutdown_tunnel(void *obj, void *p) {
    tunnel_shutdown((struct tunnel_ctx *)obj);
    (void)p;
}

void client_shutdown(struct server_env_t *env) {
    objects_container_traverse(env->tunnel_set, &_do_shutdown_tunnel, NULL);
}

static struct buffer_t * initial_package_create(const s5_ctx *parser) {
    struct buffer_t *buffer = buffer_alloc(SSR_BUFF_SIZE);

    uint8_t *iter = buffer->buffer;
    uint8_t len;
    iter[0] = (char)parser->atyp;
    iter++;

    switch (parser->atyp) {
    case s5_atyp_ipv4:  // IPv4
        memcpy(iter, parser->daddr, sizeof(struct in_addr));
        iter += sizeof(struct in_addr);
        break;
    case s5_atyp_ipv6:  // IPv6
        memcpy(iter, parser->daddr, sizeof(struct in6_addr));
        iter += sizeof(struct in6_addr);
        break;
    case s5_atyp_host:
        len = (uint8_t)strlen((char *)parser->daddr);
        iter[0] = len;
        iter++;
        memcpy(iter, parser->daddr, len);
        iter += len;
        break;
    default:
        ASSERT(0);
        break;
    }
    *((unsigned short *)iter) = htons(parser->dport);
    iter += sizeof(unsigned short);

    buffer->len = iter - buffer->buffer;

    return buffer;
}

/* This is the core state machine that drives the client <-> upstream proxy.
* We move through the initial handshake and authentication steps first and
* end up (if all goes well) in the proxy state where we're just proxying
* data between the client and upstream.
*/
static void do_next(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    switch (ctx->state) {
    case session_handshake:
        do_handshake(tunnel);
        break;
    case session_handshake_auth:
        do_handshake_auth(tunnel);
        break;
    case session_req_start:
        do_req_start(tunnel);
        break;
    case session_req_parse:
        do_req_parse(tunnel);
        break;
    case session_req_udp_accoc:
        tunnel_shutdown(tunnel);
        break;
    case session_req_lookup:
        do_req_lookup(tunnel);
        break;
    case session_req_connect:
        do_req_connect(tunnel);
        break;
    case session_ssr_auth_sent:
        do_ssr_auth_sent(tunnel);
        break;
    case session_proxy_start:
        do_proxy_start(tunnel);
        break;
    case session_proxy:
        do_proxy(tunnel, socket);
        break;
    case session_kill:
        tunnel_shutdown(tunnel);
        break;
    default:
        UNREACHABLE();
    }
}

static void do_handshake(struct tunnel_ctx *tunnel) {
    enum s5_auth_method methods;
    struct socket_ctx *incoming;
    s5_ctx *parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    parser = &ctx->parser;
    incoming = tunnel->incoming;
    ASSERT(incoming->rdstate == socket_done || incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    incoming->rdstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->buf->base;
    size = (size_t)incoming->result;
    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming);
        ctx->state = session_handshake;  /* Need more data. */
        return;
    }

    if (size != 0) {
        /* Could allow a round-trip saving shortcut here if the requested auth
        * method is s5_auth_none (provided unauthenticated traffic is allowed.)
        * Requires client support however.
        */
        pr_err("junk in handshake");
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_auth_select) {
        pr_err("handshake error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    methods = s5_auth_methods(parser);
    if ((methods & s5_auth_none) && can_auth_none(tunnel->listener, tunnel)) {
        s5_select_auth(parser, s5_auth_none);
        socket_write(incoming, "\5\0", 2);  /* No auth required. */
        ctx->state = session_req_start;
        return;
    }

    if ((methods & s5_auth_passwd) && can_auth_passwd(tunnel->listener, tunnel)) {
        /* TODO(bnoordhuis) Implement username/password auth. */
        tunnel_shutdown(tunnel);
        return;
    }

    socket_write(incoming, "\5\377", 2);  /* No acceptable auth. */
    ctx->state = session_kill;
}

/* TODO(bnoordhuis) Implement username/password auth. */
static void do_handshake_auth(struct tunnel_ctx *tunnel) {
    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_req_start(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming;

    incoming = tunnel->incoming;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_done);
    incoming->wrstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming);
    ctx->state = session_req_parse;
}

static void do_req_parse(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    s5_ctx *parser;
    uint8_t *data;
    size_t size;
    enum s5_err err;
    struct server_env_t *env;
    struct server_config *config;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    env = ctx->env;
    config = env->config;

    parser = &ctx->parser;
    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_done || incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);
    incoming->rdstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("read error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    data = (uint8_t *)incoming->buf->base;
    size = (size_t)incoming->result;

    socks5_address_parse(data+3, size-3, tunnel->desired_addr);

    err = s5_parse(parser, &data, &size);
    if (err == s5_ok) {
        socket_read(incoming);
        ctx->state = session_req_parse;  /* Need more data. */
        return;
    }

    if (size != 0) {
        pr_err("junk in request %u", (unsigned)size);
        tunnel_shutdown(tunnel);
        return;
    }

    if (err != s5_exec_cmd) {
        pr_err("request error: %s", s5_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_tcp_bind) {
        /* Not supported but relatively straightforward to implement. */
        pr_warn("BIND requests are not supported.");
        tunnel_shutdown(tunnel);
        return;
    }

    if (parser->cmd == s5_cmd_udp_assoc) {
        // UDP ASSOCIATE requests
        size_t len = incoming->buf->len;
        uint8_t *buf = build_udp_assoc_package(config->udp, config->listen_host, config->listen_port,
            (uint8_t *)incoming->buf->base, &len);
        socket_write(incoming, buf, len);
        ctx->state = session_req_udp_accoc;
        return;
    }

    ASSERT(parser->cmd == s5_cmd_tcp_connect);

    ctx->init_pkg = initial_package_create(parser);
    ctx->cipher = tunnel_cipher_create(ctx->env, ctx->init_pkg);

    union sockaddr_universal remote_addr = { 0 };
    if (convert_universal_address(config->remote_host, config->remote_port, &remote_addr) != 0) {
        socket_getaddrinfo(outgoing, config->remote_host);
        ctx->state = session_req_lookup;
        return;
    }

    outgoing->addr = remote_addr;

    do_req_connect_start(tunnel);
}

static void do_req_lookup(struct tunnel_ctx *tunnel) {
    s5_ctx *parser;
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    parser = &ctx->parser;
    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result < 0) {
        /* TODO(bnoordhuis) Escape control characters in parser->daddr. */
        pr_err("lookup error for \"%s\": %s",
            parser->daddr,
            uv_strerror((int)outgoing->result));
        /* Send back a 'Host unreachable' reply. */
        socket_write(incoming, "\5\4\0\1\0\0\0\0\0\0", 10);
        ctx->state = session_kill;
        return;
    }

    /* Don't make assumptions about the offset of sin_port/sin6_port. */
    switch (outgoing->addr.addr.sa_family) {
    case AF_INET:
        outgoing->addr.addr4.sin_port = htons(parser->dport);
        break;
    case AF_INET6:
        outgoing->addr.addr6.sin6_port = htons(parser->dport);
        break;
    default:
        UNREACHABLE();
    }

    do_req_connect_start(tunnel);
}

/* Assumes that cx->outgoing.t.sa contains a valid AF_INET/AF_INET6 address. */
static void do_req_connect_start(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    int err;

    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (!can_access(tunnel->listener, tunnel, &outgoing->addr.addr)) {
        pr_warn("connection not allowed by ruleset");
        /* Send a 'Connection not allowed by ruleset' reply. */
        socket_write(incoming, "\5\2\0\1\0\0\0\0\0\0", 10);
        ctx->state = session_kill;
        return;
    }

    err = socket_connect(outgoing);
    if (err != 0) {
        pr_err("connect error: %s", uv_strerror(err));
        tunnel_shutdown(tunnel);
        return;
    }

    ctx->state = session_req_connect;
}

static void do_req_connect(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;

    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);

    if (outgoing->result == 0) {
        struct buffer_t *tmp = buffer_clone(ctx->init_pkg);
        if (ssr_ok != tunnel_encrypt(ctx->cipher, tmp)) {
            buffer_free(tmp);
            tunnel_shutdown(tunnel);
            return;
        }
        socket_write(outgoing, tmp->buffer, tmp->len);
        buffer_free(tmp);

        ctx->state = session_ssr_auth_sent;
        return;
    } else {
        socket_dump_error_info("upstream connection", outgoing);
        /* Send a 'Connection refused' reply. */
        socket_write(incoming, "\5\5\0\1\0\0\0\0\0\0", 10);
        ctx->state = session_kill;
        return;
    }

    UNREACHABLE();
    tunnel_shutdown(tunnel);
}

static void do_ssr_auth_sent(struct tunnel_ctx *tunnel) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_stop);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_done);
    outgoing->wrstate = socket_stop;

    if (outgoing->result < 0) {
        pr_err("write error: %s", uv_strerror((int)outgoing->result));
        tunnel_shutdown(tunnel);
        return;
    }

    uint8_t *buf;
    struct buffer_t *init_pkg = ctx->init_pkg;
    buf = (uint8_t *)calloc(3 + init_pkg->len, sizeof(uint8_t));

    buf[0] = 5;  // Version.
    buf[1] = 0;  // Success.
    buf[2] = 0;  // Reserved.
    memcpy(buf + 3, init_pkg->buffer, init_pkg->len);
    socket_write(incoming, buf, 3 + init_pkg->len);
    free(buf);
    ctx->state = session_proxy_start;
}

static void do_proxy_start(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;
    ASSERT(incoming->rdstate == socket_stop);
    ASSERT(incoming->wrstate == socket_done);
    ASSERT(outgoing->rdstate == socket_stop);
    ASSERT(outgoing->wrstate == socket_stop);
    incoming->wrstate = socket_stop;

    if (incoming->result < 0) {
        pr_err("write error: %s", uv_strerror((int)incoming->result));
        tunnel_shutdown(tunnel);
        return;
    }

    socket_read(incoming);
    socket_read(outgoing);
    ctx->state = session_proxy;
}

/* Proxy incoming data back and forth. */
static void do_proxy(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;

    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    incoming = tunnel->incoming;
    outgoing = tunnel->outgoing;
    ASSERT(socket == incoming || socket == outgoing);

    if (socket == outgoing) {
        struct tunnel_cipher_ctx *tc;
        struct buffer_t *buf = NULL;
        do {
            tc = ctx->cipher;

            buf = buffer_alloc(SSR_BUFF_SIZE);
            buffer_store(buf, (const uint8_t *)outgoing->buf->base, (size_t)outgoing->result);

            struct buffer_t *feedback = NULL;
            if (ssr_ok != tunnel_decrypt(tc, buf, &feedback)) {
                pr_err("decrypt failed");
                tunnel_shutdown(tunnel);
                break;
            }
            if (feedback) {
                // SSR logic
                ASSERT(buf->len == 0);
                socket_write(outgoing, feedback->buffer, feedback->len);
                buffer_free(feedback);

                socket_read_stop(incoming);
                socket_read(incoming);
            }
            if (buf->len > 0) {
                socket_write(incoming, buf->buffer, buf->len);
            }
        } while (0);
        buffer_free(buf);
    }

    if (socket == incoming) {
        struct tunnel_cipher_ctx *tc;
        struct buffer_t *buf = NULL;
        do {
            tc = ctx->cipher;

            buf = buffer_alloc(SSR_BUFF_SIZE);
            buffer_store(buf, (const uint8_t *)incoming->buf->base, (size_t)incoming->result);
            if (ssr_ok != tunnel_encrypt(tc, buf)) {
                tunnel_shutdown(tunnel);
                break;
            }
            if (buf->len > 0) {
                socket_write(outgoing, buf->buffer, buf->len);
            }
        } while (0);
        buffer_free(buf);
    }
}

static void tunnel_dying(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;

    objects_container_remove(ctx->env->tunnel_set, tunnel);
    if (ctx->cipher) {
        tunnel_cipher_release(ctx->cipher);
    }
    buffer_free(ctx->init_pkg);
    free(ctx);
}

static void tunnel_timeout_expire_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    (void)tunnel;
    (void)socket;
}

static void tunnel_outgoing_connected_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_read_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_getaddrinfo_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    do_next(tunnel, socket);
}

static void tunnel_write_done(struct tunnel_ctx *tunnel, struct socket_ctx *socket) {
    if (tunnel->tunnel_in_streaming(tunnel) == false) {
        do_next(tunnel, socket);
    }
}

static size_t tunnel_get_alloc_size(struct tunnel_ctx *tunnel, size_t suggested_size) {
    (void)tunnel;
    (void)suggested_size;
    return SSR_BUFF_SIZE;
}

static bool tunnel_in_streaming(struct tunnel_ctx *tunnel) {
    struct client_ctx *ctx = (struct client_ctx *) tunnel->data;
    return (ctx->state == session_proxy);
}

static bool can_auth_none(const uv_tcp_t *lx, const struct tunnel_ctx *cx) {
    return true;
}

static bool can_auth_passwd(const uv_tcp_t *lx, const struct tunnel_ctx *cx) {
    return false;
}

static bool can_access(const uv_tcp_t *lx, const struct tunnel_ctx *cx, const struct sockaddr *addr) {
    const struct sockaddr_in6 *addr6;
    const struct sockaddr_in *addr4;
    const uint32_t *p;
    uint32_t a, b, c, d;

#if !defined(NDEBUG) || defined(DEBUG)
    return true;
#endif

    /* TODO(bnoordhuis) Implement proper access checks.  For now, just reject
    * traffic to localhost.
    */
    if (addr->sa_family == AF_INET) {
        addr4 = (const struct sockaddr_in *) addr;
        d = ntohl(addr4->sin_addr.s_addr);
        return (d >> 24) != 0x7F;
    }

    if (addr->sa_family == AF_INET6) {
        addr6 = (const struct sockaddr_in6 *) addr;
        p = (const uint32_t *)&addr6->sin6_addr.s6_addr;
        a = ntohl(p[0]);
        b = ntohl(p[1]);
        c = ntohl(p[2]);
        d = ntohl(p[3]);
        if (a == 0 && b == 0 && c == 0 && d == 1) {
            return false;  /* "::1" style address. */
        }
        if (a == 0 && b == 0 && c == 0xFFFF && (d >> 24) == 0x7F) {
            return false;  /* "::ffff:127.x.x.x" style address. */
        }
        return true;
    }

    return false;
}
