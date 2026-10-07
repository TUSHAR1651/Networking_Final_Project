/* proto.h — BHTTP/1 wire format shared by bserve and bcurl. See SPEC.md. */
#ifndef BHTTP_PROTO_H
#define BHTTP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BH_PREFACE      "BHTTP/1\n"
#define BH_PREFACE_LEN  8
#define BH_HDR_LEN      8          /* fixed frame header */
#define BH_MAX_ID       0xFFFFFFu  /* 24-bit request id */
#define BH_MAX_MSG      16384      /* REQUEST payload every server must accept */
#define BH_DATA_CHUNK   16384      /* DATA payload size senders should not exceed */

/* frame types (SPEC §4); anything else is skipped */
enum { BH_DATA = 0x00, BH_REQUEST = 0x01, BH_RESPONSE = 0x02, BH_GOAWAY = 0x03 };

/* flags */
#define BH_FLAG_END 0x01

/* methods */
enum { BH_GET = 0x01, BH_HEAD = 0x02 };

/* GOAWAY codes */
enum { BH_NO_ERROR = 0, BH_PROTOCOL_ERROR = 1, BH_IDLE_TIMEOUT = 2 };

/* static header table (SPEC §5): index 1..BH_STATIC_COUNT */
#define BH_STATIC_COUNT 10
extern const char *const bh_static_table[BH_STATIC_COUNT + 1];
int bh_static_index(const char *name);   /* 0 if not in table */

struct bh_frame {
    uint32_t len;    /* 24 bits */
    uint8_t  type;
    uint8_t  flags;
    uint32_t id;     /* 24 bits */
};

void bh_pack_header(uint8_t out[BH_HDR_LEN], const struct bh_frame *f);
void bh_unpack_header(const uint8_t in[BH_HDR_LEN], struct bh_frame *f);
const char *bh_type_name(uint8_t type);

/* 1 = got n bytes, 0 = clean EOF before the first byte, -1 = error / EOF mid-read */
int bh_read_full(int fd, void *buf, size_t n);
int bh_write_full(int fd, const void *buf, size_t n);   /* 0 ok, -1 error */
int bh_skip(int fd, uint32_t n);                         /* 0 ok, -1 error */

/* Writes header + payload. If trace is non-NULL the whole frame is hexdumped to it. */
int bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t id,
                  const void *payload, uint32_t len, FILE *trace);

/* growable byte buffer for building payloads */
struct bh_buf {
    uint8_t *p;
    size_t len, cap;
};
void bh_buf_put(struct bh_buf *b, const void *src, size_t n);
void bh_buf_u8(struct bh_buf *b, uint8_t v);
void bh_buf_u16(struct bh_buf *b, uint16_t v);
void bh_buf_free(struct bh_buf *b);

/* header block (SPEC §5) */
int bh_put_header(struct bh_buf *b, const char *name, const char *value);  /* -1 if too long */

struct bh_header {
    uint8_t index;          /* 0 = literal name, 1..10 static, >10 unknown */
    const uint8_t *name;    /* NULL for an unknown index: caller ignores the entry */
    size_t name_len;
    const uint8_t *value;
    size_t value_len;
};
/* 1 = entry parsed, 0 = end of block, -1 = malformed */
int bh_next_header(const uint8_t *p, size_t len, size_t *off, struct bh_header *h);

void bh_hexdump(FILE *out, const char *prefix, const uint8_t *p, size_t n);

#endif
