/* proto.c — BHTTP/1 framing helpers. See SPEC.md. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "proto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *const bh_static_table[BH_STATIC_COUNT + 1] = {
    NULL,
    "host",            /* 1  client */
    "user-agent",      /* 2  client */
    "accept",          /* 3  client */
    "content-type",    /* 4  server */
    "content-length",  /* 5  server */
    "last-modified",   /* 6  server */
    "etag",            /* 7  server */
    "date",            /* 8  server */
    "server",          /* 9  server */
    "allow",           /* 10 server, on 501 */
};

int bh_static_index(const char *name)
{
    for (int i = 1; i <= BH_STATIC_COUNT; i++)
        if (strcasecmp(name, bh_static_table[i]) == 0)
            return i;
    return 0;
}

void bh_pack_header(uint8_t out[BH_HDR_LEN], const struct bh_frame *f)
{
    out[0] = (uint8_t)(f->len >> 16);
    out[1] = (uint8_t)(f->len >> 8);
    out[2] = (uint8_t)f->len;
    out[3] = f->type;
    out[4] = f->flags;
    out[5] = (uint8_t)(f->id >> 16);
    out[6] = (uint8_t)(f->id >> 8);
    out[7] = (uint8_t)f->id;
}

void bh_unpack_header(const uint8_t in[BH_HDR_LEN], struct bh_frame *f)
{
    f->len = (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
    f->type = in[3];
    f->flags = in[4];
    f->id = (uint32_t)in[5] << 16 | (uint32_t)in[6] << 8 | in[7];
}

const char *bh_type_name(uint8_t type)
{
    switch (type) {
    case BH_DATA:     return "DATA";
    case BH_REQUEST:  return "REQUEST";
    case BH_RESPONSE: return "RESPONSE";
    case BH_GOAWAY:   return "GOAWAY";
    default:          return "UNKNOWN";
    }
}

int bh_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 1;
}

int bh_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int bh_skip(int fd, uint32_t n)
{
    uint8_t sink[4096];
    while (n > 0) {
        size_t chunk = n < sizeof sink ? n : sizeof sink;
        if (bh_read_full(fd, sink, chunk) != 1)
            return -1;
        n -= (uint32_t)chunk;
    }
    return 0;
}

int bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t id,
                  const void *payload, uint32_t len, FILE *trace)
{
    struct bh_frame f = { len, type, flags, id };
    uint8_t *frame = malloc(BH_HDR_LEN + (size_t)len);
    if (!frame)
        return -1;
    bh_pack_header(frame, &f);
    if (len)
        memcpy(frame + BH_HDR_LEN, payload, len);
    /* one write per frame: header and payload leave in the same segment */
    int rc = bh_write_full(fd, frame, BH_HDR_LEN + (size_t)len);
    if (trace) {
        fprintf(trace, "> %s id=%u flags=0x%02x%s length=%u\n", bh_type_name(type), id,
                flags, (flags & BH_FLAG_END) ? " [END]" : "", len);
        bh_hexdump(trace, ">   ", frame, BH_HDR_LEN + (size_t)len);
    }
    free(frame);
    return rc;
}

void bh_buf_put(struct bh_buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            perror("realloc");
            exit(1);
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

void bh_buf_u8(struct bh_buf *b, uint8_t v)
{
    bh_buf_put(b, &v, 1);
}

void bh_buf_u16(struct bh_buf *b, uint16_t v)
{
    uint8_t be[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    bh_buf_put(b, be, 2);
}

void bh_buf_free(struct bh_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

int bh_put_header(struct bh_buf *b, const char *name, const char *value)
{
    size_t nl = strlen(name), vl = strlen(value);
    if (nl == 0 || nl > 255 || vl > 0xFFFF)
        return -1;
    int idx = bh_static_index(name);
    if (idx) {
        bh_buf_u8(b, (uint8_t)idx);
    } else {
        bh_buf_u8(b, 0);
        bh_buf_u8(b, (uint8_t)nl);
        bh_buf_put(b, name, nl);
    }
    bh_buf_u16(b, (uint16_t)vl);
    bh_buf_put(b, value, vl);
    return 0;
}

int bh_next_header(const uint8_t *p, size_t len, size_t *off, struct bh_header *h)
{
    size_t o = *off;
    if (o >= len)
        return 0;
    h->index = p[o++];
    if (h->index == 0) {
        if (o >= len)
            return -1;
        h->name_len = p[o++];
        if (h->name_len == 0 || h->name_len > len - o)
            return -1;
        h->name = p + o;
        o += h->name_len;
    } else if (h->index <= BH_STATIC_COUNT) {
        h->name = (const uint8_t *)bh_static_table[h->index];
        h->name_len = strlen(bh_static_table[h->index]);
    } else {
        /* reserved index: value is still length-prefixed, so it can be skipped */
        h->name = NULL;
        h->name_len = 0;
    }
    if (len - o < 2)
        return -1;
    h->value_len = (size_t)p[o] << 8 | p[o + 1];
    o += 2;
    if (h->value_len > len - o)
        return -1;
    h->value = p + o;
    o += h->value_len;
    *off = o;
    return 1;
}

void bh_hexdump(FILE *out, const char *prefix, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        fprintf(out, "%s%08zx  ", prefix, i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n)
                fprintf(out, "%02x ", p[i + j]);
            else
                fputs("   ", out);
            if (j == 7)
                fputc(' ', out);
        }
        fputs(" |", out);
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            fputc(c >= 0x20 && c < 0x7f ? c : '.', out);
        }
        fputs("|\n", out);
    }
}
