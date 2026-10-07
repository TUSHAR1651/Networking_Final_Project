/* bcurl — fetch URLs from a BHTTP/1 server (see SPEC.md).
 *
 *   ./bcurl [-v] [-I] [-H 'name: value']... URL [URL...]
 *
 * URL is [bhttp://]host[:port][/path]; the default port is 9000.
 * All URLs must name the same host:port: they share ONE connection.
 * Bodies go to stdout; -v hexdumps every frame (both directions) to stderr.
 *
 * Exit: 0 all < 400 · 4 some 4xx · 5 some 5xx · 1 network/protocol error · 2 usage
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "proto.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USER_AGENT  "bcurl/1.0"
#define MAX_EXTRA   32

struct url {
    char host[256];
    char port[6];
    char authority[300];   /* host[:port] exactly as written, for the host header */
    const char *path;
};

struct extra_header {
    char name[256];
    const char *value;
};

static FILE *trace;   /* stderr when -v */

static void usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] [-I] [-H 'name: value']... URL [URL...]\n");
    exit(2);
}

static int parse_url(const char *s, struct url *u)
{
    if (strncmp(s, "bhttp://", 8) == 0)
        s += 8;
    const char *slash = strchr(s, '/');
    size_t alen = slash ? (size_t)(slash - s) : strlen(s);
    if (alen == 0 || alen >= sizeof u->authority)
        return -1;
    memcpy(u->authority, s, alen);
    u->authority[alen] = '\0';
    u->path = slash ? slash : "/";

    const char *colon;
    size_t hlen;
    if (u->authority[0] == '[') {                    /* [v6addr]:port */
        const char *close = strchr(u->authority, ']');
        if (!close)
            return -1;
        hlen = (size_t)(close - u->authority - 1);
        memcpy(u->host, u->authority + 1, hlen);
        colon = close[1] == ':' ? close + 1 : NULL;
        if (close[1] && !colon)
            return -1;
    } else {
        colon = strchr(u->authority, ':');
        hlen = colon ? (size_t)(colon - u->authority) : alen;
        memcpy(u->host, u->authority, hlen);
    }
    u->host[hlen] = '\0';
    if (hlen == 0)
        return -1;

    const char *port = colon ? colon + 1 : "9000";
    size_t plen = strlen(port);
    if (plen == 0 || plen > 5 || strspn(port, "0123456789") != plen || atoi(port) < 1 ||
        atoi(port) > 65535)
        return -1;
    memcpy(u->port, port, plen + 1);
    return 0;
}

static int dial(const struct url *u)
{
    struct addrinfo hints = { 0 }, *res, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(u->host, u->port, &hints, &res);
    if (err) {
        fprintf(stderr, "bcurl: %s: %s\n", u->host, gai_strerror(err));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "bcurl: cannot connect to %s:%s: %s\n", u->host, u->port, strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int has_extra(const struct extra_header *x, int nx, const char *name)
{
    for (int i = 0; i < nx; i++)
        if (strcmp(x[i].name, name) == 0)
            return 1;
    return 0;
}

static void trace_headers(const char *dir, const uint8_t *p, size_t len, size_t off)
{
    struct bh_header h;
    while (bh_next_header(p, len, &off, &h) == 1) {
        if (h.name)
            fprintf(trace, "%s   %s%.*s: %.*s\n", dir, h.index ? "" : "(literal) ",
                    (int)h.name_len, (const char *)h.name, (int)h.value_len, (const char *)h.value);
        else
            fprintf(trace, "%s   (unknown index %u, ignored)\n", dir, h.index);
    }
}

static int protocol_error(const char *what)
{
    fprintf(stderr, "bcurl: protocol error: %s\n", what);
    return -1;
}

/* Send one request and read its complete response.
 * Returns the HTTP status, or -1 if the connection is no longer usable. */
static int fetch(int fd, uint32_t id, const struct url *u, int head,
                 const struct extra_header *x, int nx)
{
    size_t plen = strlen(u->path);
    if (plen > 0xFFFF) {
        fprintf(stderr, "bcurl: path too long\n");
        return -1;
    }
    struct bh_buf b = { 0 };
    bh_buf_u8(&b, head ? BH_HEAD : BH_GET);
    bh_buf_u16(&b, (uint16_t)plen);
    bh_buf_put(&b, u->path, plen);
    if (!has_extra(x, nx, "host"))
        bh_put_header(&b, "host", u->authority);
    if (!has_extra(x, nx, "user-agent"))
        bh_put_header(&b, "user-agent", USER_AGENT);
    if (!has_extra(x, nx, "accept"))
        bh_put_header(&b, "accept", "*/*");
    for (int i = 0; i < nx; i++)
        if (bh_put_header(&b, x[i].name, x[i].value) != 0) {
            fprintf(stderr, "bcurl: header %s too long\n", x[i].name);
            bh_buf_free(&b);
            return -1;
        }
    if (b.len > BH_MAX_MSG)
        fprintf(stderr, "bcurl: warning: request frame is %zu bytes; servers may reject > %d\n",
                b.len, BH_MAX_MSG);

    int rc = bh_send_frame(fd, BH_REQUEST, BH_FLAG_END, id, b.p, (uint32_t)b.len, trace);
    if (trace) {
        fprintf(trace, ">   %s %s\n", head ? "HEAD" : "GET", u->path);
        trace_headers(">", b.p, b.len, 3 + plen);
    }
    bh_buf_free(&b);
    if (rc != 0) {
        fprintf(stderr, "bcurl: write failed: %s\n", strerror(errno));
        return -1;
    }

    int status = -1;
    long long clen = -1, received = 0;
    uint8_t *payload = NULL;
    for (;;) {
        uint8_t hdr[BH_HDR_LEN];
        int r = bh_read_full(fd, hdr, sizeof hdr);
        if (r != 1) {
            free(payload);
            return protocol_error(r == 0 ? "server closed the connection" : "truncated frame header");
        }
        struct bh_frame f;
        bh_unpack_header(hdr, &f);

        /* whole frame in memory (bounded by the 24-bit length) so -v can dump it */
        uint8_t *frame = realloc(payload, BH_HDR_LEN + (size_t)f.len);
        if (!frame) {
            free(payload);
            return protocol_error("out of memory");
        }
        payload = frame;
        memcpy(frame, hdr, BH_HDR_LEN);
        if (bh_read_full(fd, frame + BH_HDR_LEN, f.len) != 1) {
            free(payload);
            return protocol_error("truncated frame payload");
        }
        const uint8_t *p = frame + BH_HDR_LEN;
        if (trace) {
            fprintf(trace, "< %s", bh_type_name(f.type));
            if (f.type > BH_GOAWAY)
                fprintf(trace, "(0x%02x)", f.type);
            fprintf(trace, " id=%u flags=0x%02x%s length=%u\n", f.id, f.flags,
                    (f.flags & BH_FLAG_END) ? " [END]" : "", f.len);
            bh_hexdump(trace, "<   ", frame, BH_HDR_LEN + (size_t)f.len);
        }

        int end = f.flags & BH_FLAG_END;
        switch (f.type) {
        case BH_RESPONSE: {
            if (status >= 0 || f.id != id || f.len < 2) {
                free(payload);
                return protocol_error("unexpected RESPONSE frame");
            }
            status = p[0] << 8 | p[1];
            size_t off = 2;
            struct bh_header h;
            int hr;
            while ((hr = bh_next_header(p, f.len, &off, &h)) == 1)
                if (h.name && h.name_len == 14 && memcmp(h.name, "content-length", 14) == 0 &&
                    h.value_len < 21) {
                    char num[21];
                    memcpy(num, h.value, h.value_len);
                    num[h.value_len] = '\0';
                    clen = strtoll(num, NULL, 10);
                }
            if (hr < 0) {
                free(payload);
                return protocol_error("malformed response header block");
            }
            if (trace) {
                fprintf(trace, "<   status %d\n", status);
                trace_headers("<", p, f.len, 2);
            }
            break;
        }
        case BH_DATA:
            if (status < 0 || f.id != id) {
                free(payload);
                return protocol_error("DATA frame for an unexpected request");
            }
            if (!head && f.len)
                fwrite(p, 1, f.len, stdout);
            received += f.len;
            break;
        case BH_GOAWAY: {
            unsigned code = f.len >= 2 ? (unsigned)(p[0] << 8 | p[1]) : 0;
            fprintf(stderr, "bcurl: server sent GOAWAY code=%u reason=%.*s\n", code,
                    f.len > 2 ? (int)(f.len - 2) : 0, f.len > 2 ? (const char *)p + 2 : "");
            free(payload);
            return -1;
        }
        default:
            /* SPEC §4: unknown frame types are skipped; the payload is already consumed */
            if (trace)
                fprintf(trace, "<   unknown frame type 0x%02x, skipped\n", f.type);
            continue;
        }
        if (status >= 0 && end)
            break;
    }
    free(payload);
    fflush(stdout);

    if (!head && clen >= 0 && received != clen)
        return protocol_error("body length does not match content-length");
    return status;
}

int main(int argc, char **argv)
{
    int head = 0, nx = 0, opt;
    struct extra_header x[MAX_EXTRA];
    while ((opt = getopt(argc, argv, "vIH:h")) != -1) {
        switch (opt) {
        case 'v':
            trace = stderr;
            break;
        case 'I':
            head = 1;
            break;
        case 'H': {
            const char *colon = strchr(optarg, ':');
            size_t nl = colon ? (size_t)(colon - optarg) : 0;
            if (!colon || nl == 0 || nl > 255 || nx == MAX_EXTRA) {
                fprintf(stderr, "bcurl: bad header '%s' (want 'name: value')\n", optarg);
                return 2;
            }
            for (size_t i = 0; i < nl; i++)
                x[nx].name[i] = (char)tolower((unsigned char)optarg[i]);
            x[nx].name[nl] = '\0';
            x[nx].value = colon + 1 + strspn(colon + 1, " \t");
            nx++;
            break;
        }
        default:
            usage();
        }
    }
    int nurls = argc - optind;
    if (nurls < 1)
        usage();

    struct url *urls = calloc((size_t)nurls, sizeof *urls);
    for (int i = 0; i < nurls; i++) {
        if (parse_url(argv[optind + i], &urls[i]) != 0) {
            fprintf(stderr, "bcurl: bad URL %s\n", argv[optind + i]);
            return 2;
        }
        if (strcmp(urls[i].host, urls[0].host) != 0 || strcmp(urls[i].port, urls[0].port) != 0) {
            fprintf(stderr, "bcurl: all URLs must share one host:port (one connection)\n");
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    int fd = dial(&urls[0]);
    if (fd < 0)
        return 1;

    if (trace) {
        fprintf(trace, "> preface (%d bytes)\n", BH_PREFACE_LEN);
        bh_hexdump(trace, ">   ", (const uint8_t *)BH_PREFACE, BH_PREFACE_LEN);
    }
    if (bh_write_full(fd, BH_PREFACE, BH_PREFACE_LEN) != 0) {
        fprintf(stderr, "bcurl: write failed: %s\n", strerror(errno));
        return 1;
    }

    int worst = 0;
    for (int i = 0; i < nurls; i++) {
        uint32_t id = (uint32_t)(i % BH_MAX_ID) + 1;   /* 1..2^24-1, then wraps */
        int status = fetch(fd, id, &urls[i], head, x, nx);
        if (status < 0) {
            close(fd);
            return 1;
        }
        if (status > worst)
            worst = status;
    }

    struct bh_buf b = { 0 };
    bh_buf_u16(&b, BH_NO_ERROR);
    bh_send_frame(fd, BH_GOAWAY, 0, 0, b.p, (uint32_t)b.len, trace);
    bh_buf_free(&b);
    close(fd);
    free(urls);

    return worst >= 500 ? 5 : worst >= 400 ? 4 : 0;
}
