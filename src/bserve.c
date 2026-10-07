/* bserve — serve the files under ROOT over BHTTP/1 (see SPEC.md).
 *
 *   ./bserve ./www 9000
 *
 * One process per connection; each connection serves requests until the
 * client closes, sends GOAWAY, or stays idle for IDLE_SECS.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "proto.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "bserve/1.0"
#define IDLE_SECS   30

static char root_real[PATH_MAX];
static char peer[NI_MAXHOST];

static void http_date(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "text/javascript" },
        { ".json", "application/json" },
        { ".svg",  "image/svg+xml" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
            if (strcasecmp(dot, types[i].ext) == 0)
                return types[i].type;
    return "application/octet-stream";
}

static void log_request(uint32_t id, const char *method, const uint8_t *path, size_t plen,
                        int status, long long bytes)
{
    fprintf(stderr, "%s id=%u %s %.*s %d bytes=%lld\n", peer, id, method,
            (int)(plen > 200 ? 200 : plen), (const char *)path, status, bytes);
}

/* Error response with a one-line text/plain body (omitted for HEAD). */
static int send_error(int fd, uint32_t id, int status, int head, const char *msg)
{
    char body[160], clen[24], date[64];
    int blen = snprintf(body, sizeof body, "%d %s\n", status, msg);
    snprintf(clen, sizeof clen, "%d", blen);
    http_date(time(NULL), date, sizeof date);

    struct bh_buf b = { 0 };
    bh_buf_u16(&b, (uint16_t)status);
    bh_put_header(&b, "content-type", "text/plain; charset=utf-8");
    bh_put_header(&b, "content-length", clen);
    bh_put_header(&b, "date", date);
    bh_put_header(&b, "server", SERVER_NAME);
    if (status == 501)
        bh_put_header(&b, "allow", "GET, HEAD");

    int rc = bh_send_frame(fd, BH_RESPONSE, head ? BH_FLAG_END : 0, id, b.p, (uint32_t)b.len, NULL);
    if (rc == 0 && !head)
        rc = bh_send_frame(fd, BH_DATA, BH_FLAG_END, id, body, (uint32_t)blen, NULL);
    bh_buf_free(&b);
    return rc;
}

/* Resolve a request path to an open regular file under root_real.
 * Returns the fd, or -status (404/403/400) on failure. */
static int open_under_root(const uint8_t *path, size_t plen, char *resolved, struct stat *st)
{
    size_t rlen = strlen(root_real);
    const uint8_t *q = memchr(path, '?', plen);
    if (q)
        plen = (size_t)(q - path);
    if (rlen + plen + sizeof "/index.html" > PATH_MAX)
        return -400;

    char full[PATH_MAX];
    memcpy(full, root_real, rlen);
    memcpy(full + rlen, path, plen);
    full[rlen + plen] = '\0';
    if (full[rlen + plen - 1] == '/')
        strcat(full, "index.html");
    else if (stat(full, st) == 0 && S_ISDIR(st->st_mode))
        strcat(full, "/index.html");

    if (!realpath(full, resolved))
        return errno == EACCES ? -403 : -404;
    /* must stay inside the root (defeats "..", and symlinks pointing out) */
    if (rlen > 1 && (strncmp(resolved, root_real, rlen) != 0 ||
                     (resolved[rlen] != '/' && resolved[rlen] != '\0')))
        return -404;

    int ffd = open(resolved, O_RDONLY);
    if (ffd < 0)
        return errno == EACCES ? -403 : -404;
    if (fstat(ffd, st) != 0 || !S_ISREG(st->st_mode)) {
        close(ffd);
        return -404;
    }
    return ffd;
}

/* Handle one REQUEST payload. Returns -1 only if the connection must close. */
static int handle_request(int fd, uint32_t id, const uint8_t *p, size_t len)
{
    static const uint8_t none[] = "?";
    if (len < 3) {
        log_request(id, "?", none, 1, 400, 0);
        return send_error(fd, id, 400, 0, "request frame too short");
    }
    uint8_t method = p[0];
    int head = method == BH_HEAD;
    const char *mname = method == BH_GET ? "GET" : head ? "HEAD" : "?";
    size_t plen = (size_t)p[1] << 8 | p[2];
    if (plen > len - 3) {
        log_request(id, mname, none, 1, 400, 0);
        return send_error(fd, id, 400, head, "path length overruns frame");
    }
    const uint8_t *path = p + 3;

    /* v1 servers act on no request header, but the block must still parse */
    size_t off = 3 + plen;
    struct bh_header h;
    int r;
    while ((r = bh_next_header(p, len, &off, &h)) == 1)
        ;
    if (r < 0) {
        log_request(id, mname, path, plen, 400, 0);
        return send_error(fd, id, 400, head, "malformed header block");
    }
    if (plen == 0 || path[0] != '/' || memchr(path, '\0', plen)) {
        log_request(id, mname, path, plen, 400, 0);
        return send_error(fd, id, 400, head, "path must start with / and contain no NUL");
    }
    if (method != BH_GET && method != BH_HEAD) {
        log_request(id, mname, path, plen, 501, 0);
        return send_error(fd, id, 501, 0, "method not implemented");
    }

    char resolved[PATH_MAX];
    struct stat st;
    int ffd = open_under_root(path, plen, resolved, &st);
    if (ffd < 0) {
        int status = -ffd;
        log_request(id, mname, path, plen, status, 0);
        return send_error(fd, id, status, head,
                          status == 404 ? "not found" : status == 403 ? "forbidden" : "bad path");
    }

    char clen[24], lastmod[64], date[64], etag[48];
    snprintf(clen, sizeof clen, "%lld", (long long)st.st_size);
    http_date(st.st_mtime, lastmod, sizeof lastmod);
    http_date(time(NULL), date, sizeof date);
    snprintf(etag, sizeof etag, "\"%llx-%llx\"", (unsigned long long)st.st_size,
             (unsigned long long)st.st_mtime);

    struct bh_buf b = { 0 };
    bh_buf_u16(&b, 200);
    bh_put_header(&b, "content-type", mime_type(resolved));
    bh_put_header(&b, "content-length", clen);
    bh_put_header(&b, "last-modified", lastmod);
    bh_put_header(&b, "etag", etag);
    bh_put_header(&b, "date", date);
    bh_put_header(&b, "server", SERVER_NAME);
    int no_body = head || st.st_size == 0;
    int rc = bh_send_frame(fd, BH_RESPONSE, no_body ? BH_FLAG_END : 0, id, b.p, (uint32_t)b.len, NULL);
    bh_buf_free(&b);

    long long sent = 0;
    if (rc == 0 && !no_body) {
        uint8_t chunk[BH_DATA_CHUNK];
        long long remaining = st.st_size;
        while (remaining > 0) {
            size_t want = remaining < BH_DATA_CHUNK ? (size_t)remaining : BH_DATA_CHUNK;
            ssize_t n = read(ffd, chunk, want);
            if (n <= 0) {
                /* file shrank under us: end the message; the client sees the short body */
                rc = bh_send_frame(fd, BH_DATA, BH_FLAG_END, id, NULL, 0, NULL);
                break;
            }
            remaining -= n;
            sent += n;
            rc = bh_send_frame(fd, BH_DATA, remaining == 0 ? BH_FLAG_END : 0, id, chunk,
                               (uint32_t)n, NULL);
            if (rc != 0)
                break;
        }
    }
    close(ffd);
    log_request(id, mname, path, plen, 200, sent);
    return rc;
}

static void send_goaway(int fd, uint16_t code, const char *reason)
{
    struct bh_buf b = { 0 };
    bh_buf_u16(&b, code);
    bh_buf_put(&b, reason, strlen(reason));
    bh_send_frame(fd, BH_GOAWAY, 0, 0, b.p, (uint32_t)b.len, NULL);
    bh_buf_free(&b);
}

static int timed_out(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void serve_connection(int fd)
{
    uint8_t preface[BH_PREFACE_LEN];
    if (bh_read_full(fd, preface, sizeof preface) != 1 ||
        memcmp(preface, BH_PREFACE, BH_PREFACE_LEN) != 0) {
        fprintf(stderr, "%s bad preface, closing\n", peer);
        send_goaway(fd, BH_PROTOCOL_ERROR, "bad preface");
        return;
    }

    static uint8_t payload[BH_MAX_MSG];
    for (;;) {
        uint8_t hdr[BH_HDR_LEN];
        int r = bh_read_full(fd, hdr, sizeof hdr);
        if (r == 0)
            return;                      /* client closed between frames */
        if (r < 0) {
            if (timed_out())
                send_goaway(fd, BH_IDLE_TIMEOUT, "idle timeout");
            return;
        }
        struct bh_frame f;
        bh_unpack_header(hdr, &f);

        if (f.type == BH_GOAWAY) {
            bh_skip(fd, f.len);
            return;
        }
        if (f.type != BH_REQUEST) {
            /* DATA (v1 requests have no body), RESPONSE, and unknown types: skip */
            if (bh_skip(fd, f.len) != 0)
                return;
            continue;
        }
        if (f.id == 0) {
            /* ID 0 is the connection itself: there is no request to answer */
            fprintf(stderr, "%s REQUEST on id 0, closing\n", peer);
            bh_skip(fd, f.len);
            send_goaway(fd, BH_PROTOCOL_ERROR, "request on id 0");
            return;
        }
        if (f.len > BH_MAX_MSG) {
            if (bh_skip(fd, f.len) != 0)
                return;
            log_request(f.id, "?", (const uint8_t *)"?", 1, 400, 0);
            if (send_error(fd, f.id, 400, 0, "request frame too large") != 0)
                return;
            continue;
        }
        if (bh_read_full(fd, payload, f.len) != 1)
            return;
        if (handle_request(fd, f.id, payload, f.len) != 0)
            return;
    }
}

/* Closing with unread input makes TCP send RST, which can destroy a GOAWAY we just
 * wrote before the peer reads it. Half-close, then drain briefly, then close. */
static void close_gracefully(int fd)
{
    shutdown(fd, SHUT_WR);
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t sink[4096];
    for (int i = 0; i < 64 && read(fd, sink, sizeof sink) > 0; i++)
        ;
    close(fd);
}

static int listen_on(const char *port)
{
    struct addrinfo hints = { 0 }, *res;
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int fd = -1;
    /* dual-stack IPv6 socket first (so "localhost" works either way), then IPv4 */
    for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
        if (attempt == 1)
            hints.ai_family = AF_INET;
        if (getaddrinfo(NULL, port, &hints, &res) != 0)
            continue;
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd >= 0) {
            int one = 1, zero = 0;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (res->ai_family == AF_INET6)
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
            if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 128) != 0) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s ROOT PORT\n", argv[0]);
        return 2;
    }
    struct stat st;
    if (!realpath(argv[1], root_real) || stat(root_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: %s is not a directory\n", argv[1]);
        return 2;
    }
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*end || port < 1 || port > 65535) {
        fprintf(stderr, "bserve: bad port %s\n", argv[2]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* children are reaped automatically */

    int lfd = listen_on(argv[2]);
    if (lfd < 0) {
        perror("bserve: listen");
        return 1;
    }
    fprintf(stderr, "bserve: serving %s on port %ld\n", root_real, port);

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
        if (cfd < 0) {
            if (errno != EINTR)
                perror("bserve: accept");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            if (getnameinfo((struct sockaddr *)&ss, sl, peer, sizeof peer, NULL, 0, NI_NUMERICHOST) != 0)
                strcpy(peer, "?");
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            struct timeval tv = { IDLE_SECS, 0 };
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            serve_connection(cfd);
            close_gracefully(cfd);
            _exit(0);
        }
        if (pid < 0)
            perror("bserve: fork");
        close(cfd);
    }
}
