// SPDX-License-Identifier: GPL-2.0
/*
 * PowerFS Lock Wire-Protocol TLV Codec implementation (phase 4-6).
 *
 * See tlk_codec.h for the byte-level spec. Mirrors the Rust
 * `powerfs-lock-net/src/codec.rs` byte-for-byte.
 *
 * Dual-environment: compiles in the kernel build (with linux/types.h)
 * and in userspace (for the round-trip test in tests/test_tlk_codec.c).
 * The only kernel-vs-userspace branch is the memcpy/string init — both
 * branches use the same byte logic so a userspace test passing means
 * the kernel will produce the same wire bytes.
 */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <linux/types.h>
#include <errno.h>
#include <string.h>
#endif

#include "tlk_codec.h"

/* Maximum field value length we accept to decode. The wire format
 * allows up to 4 GiB per field (u32 length), but real lock messages
 * are < 1 KiB. Rejecting absurd lengths early avoids integer
 * overflow in `cursor + len` arithmetic on a hostile/corrupt input.
 *
 * Tuned to fit the largest expected token (opaque strings up to ~256
 * bytes) plus generous headroom for future fields. */
#define TLK_MAX_FIELD_LEN (16 * 1024)

/* ========== Internal helpers ========== */

static inline void tlk_put_le32(__u8 *p, __u32 v)
{
    p[0] = (__u8)(v & 0xFF);
    p[1] = (__u8)((v >> 8) & 0xFF);
    p[2] = (__u8)((v >> 16) & 0xFF);
    p[3] = (__u8)((v >> 24) & 0xFF);
}

static inline void tlk_put_le64(__u8 *p, __u64 v)
{
    p[0] = (__u8)(v & 0xFF);
    p[1] = (__u8)((v >> 8) & 0xFF);
    p[2] = (__u8)((v >> 16) & 0xFF);
    p[3] = (__u8)((v >> 24) & 0xFF);
    p[4] = (__u8)((v >> 32) & 0xFF);
    p[5] = (__u8)((v >> 40) & 0xFF);
    p[6] = (__u8)((v >> 48) & 0xFF);
    p[7] = (__u8)((v >> 56) & 0xFF);
}

static inline __u32 tlk_get_le32(const __u8 *p)
{
    return  (__u32)p[0]        |
           ((__u32)p[1] << 8) |
           ((__u32)p[2] << 16)|
           ((__u32)p[3] << 24);
}

static inline __u64 tlk_get_le64(const __u8 *p)
{
    return  (__u64)p[0]        |
           ((__u64)p[1] << 8)  |
           ((__u64)p[2] << 16) |
           ((__u64)p[3] << 24) |
           ((__u64)p[4] << 32) |
           ((__u64)p[5] << 40) |
           ((__u64)p[6] << 48) |
           ((__u64)p[7] << 56);
}

/* ========== Encoder ========== */

void tlk_enc_init(struct tlk_enc *enc, __u8 *buf, size_t cap)
{
    enc->buf = buf;
    enc->cap = cap;
    enc->len = 0;
    enc->payload_len_off = 0;
    enc->frame_started = 0;
}

int tlk_enc_frame_start(struct tlk_enc *enc, __u8 msg_type)
{
    if (enc->frame_started)
        return TLK_ERR_BAD_ENCODING; /* already started */
    if (enc->cap < 5)
        return TLK_ERR_NO_SPACE;

    enc->buf[0] = msg_type;
    /* Reserve 4 bytes for payload_len; we'll back-patch at finalize. */
    enc->buf[1] = 0;
    enc->buf[2] = 0;
    enc->buf[3] = 0;
    enc->buf[4] = 0;
    enc->len = 5;
    enc->payload_len_off = 1; /* bytes [1..5) hold the length */
    enc->frame_started = 1;
    return 0;
}

int tlk_enc_field(struct tlk_enc *enc, __u8 tag,
                  const void *value, __u32 len)
{
    if (!enc->frame_started)
        return TLK_ERR_BAD_ENCODING;
    /* 1 byte tag + 4 bytes len + `len` bytes value */
    if (enc->len + 5 + len > enc->cap)
        return TLK_ERR_NO_SPACE;

    enc->buf[enc->len++] = tag;
    tlk_put_le32(enc->buf + enc->len, len);
    enc->len += 4;
    memcpy(enc->buf + enc->len, value, len);
    enc->len += len;
    return 0;
}

int tlk_enc_u8(struct tlk_enc *enc, __u8 tag, __u8 val)
{
    return tlk_enc_field(enc, tag, &val, 1);
}

int tlk_enc_u32(struct tlk_enc *enc, __u8 tag, __u32 val)
{
    __u8 buf[4];
    tlk_put_le32(buf, val);
    return tlk_enc_field(enc, tag, buf, 4);
}

int tlk_enc_u64(struct tlk_enc *enc, __u8 tag, __u64 val)
{
    __u8 buf[8];
    tlk_put_le64(buf, val);
    return tlk_enc_field(enc, tag, buf, 8);
}

int tlk_enc_str(struct tlk_enc *enc, __u8 tag, const char *str, __u32 len)
{
    return tlk_enc_field(enc, tag, str, len);
}

int tlk_enc_frame_finalize(struct tlk_enc *enc)
{
    if (!enc->frame_started)
        return TLK_ERR_BAD_ENCODING;
    /* payload_len = total_len - 5 (frame header is 5 bytes). */
    if (enc->len < 5)
        return TLK_ERR_BAD_ENCODING;
    __u32 payload_len = (__u32)(enc->len - 5);
    tlk_put_le32(enc->buf + enc->payload_len_off, payload_len);
    return (int)enc->len;
}

/* ========== Decoder ========== */

/* Sentinel value in `field_off` meaning "tag not present". We use
 * 0xFFFF because real offsets in lock messages (< 1 KiB) are tiny. */
#define TLK_OFF_ABSENT 0xFFFFu

int tlk_dec_init(struct tlk_dec *dec, const __u8 *frame, size_t frame_len,
                 __u8 *msg_type)
{
    if (frame_len < 5)
        return TLK_ERR_TRUNCATED;

    dec->frame = frame;
    dec->frame_len = frame_len;
    dec->msg_type = frame[0];
    dec->payload_len = tlk_get_le32(frame + 1);
    dec->payload = frame + 5;

    /* Tolerate trailing bytes (§1 forward compat: a larger frame_len
     * than payload_len + 5 means the caller passed extra trailing
     * bytes; we just decode the declared payload). Reject a payload
     * that would overrun the buffer, though. */
    if ((__u64)dec->payload_len + 5 > frame_len)
        return TLK_ERR_TRUNCATED;

    /* Initialize the field index to "all absent". */
    memset(dec->field_off, 0xFF, sizeof(dec->field_off));

    /* One-pass full parse: walk the payload, recording each tag's
     * value offset+length in the index. Duplicate tags are an error
     * (§2). Unknown tags are silently skipped (§2 forward compat).
     * This matches the Rust reference's parse-all-into-HashMap. */
    size_t cursor = 0;
    while (cursor < dec->payload_len) {
        /* Field header: 1 byte tag + 4 bytes len. */
        if (cursor + 5 > dec->payload_len)
            return TLK_ERR_TRUNCATED;
        __u8 ftag = dec->payload[cursor];
        __u32 flen = tlk_get_le32(dec->payload + cursor + 1);
        cursor += 5;

        if (flen > TLK_MAX_FIELD_LEN)
            return TLK_ERR_TOO_LONG;
        if (cursor + flen > dec->payload_len)
            return TLK_ERR_TRUNCATED;

        /* Duplicate detection (§2). */
        if (dec->field_off[ftag] != TLK_OFF_ABSENT)
            return TLK_ERR_DUPLICATE;

        /* Record offset+length (capped to __u16; lock messages are
         * < 1 KiB so this never truncates in practice). */
        if (cursor + flen > 0xFFFF) {
            /* Would overflow __u16 — the wire format allows huge
             * fields, but the kernel lock-client doesn't. Reject. */
            return TLK_ERR_TOO_LONG;
        }
        dec->field_off[ftag] = (__u16)cursor;
        dec->field_len[ftag] = (__u16)flen;
        cursor += flen;
        /* Unknown tags land here too (we record them, but callers
         * never look them up — they just sit in the index unused).
         * That's the §2 forward-compat behavior. */
    }

    if (msg_type)
        *msg_type = dec->msg_type;
    return 0;
}

/* Internal: look up a field by tag in the pre-built index. Returns 0
 * on found (fills data+len), -ENODATA on absent. */
static int tlk_dec_lookup(const struct tlk_dec *dec, __u8 tag,
                          const __u8 **out_data, __u32 *out_len)
{
    __u16 off = dec->field_off[tag];
    if (off == TLK_OFF_ABSENT)
        return -ENODATA;
    *out_data = dec->payload + off;
    *out_len = dec->field_len[tag];
    return 0;
}

int tlk_dec_u8(struct tlk_dec *dec, __u8 tag, __u8 *out)
{
    const __u8 *data;
    __u32 len;
    int rc = tlk_dec_lookup(dec, tag, &data, &len);
    if (rc != 0)
        return rc;
    if (len != 1)
        return TLK_ERR_BAD_ENCODING;
    *out = data[0];
    return 0;
}

int tlk_dec_u32(struct tlk_dec *dec, __u8 tag, __u32 *out)
{
    const __u8 *data;
    __u32 len;
    int rc = tlk_dec_lookup(dec, tag, &data, &len);
    if (rc != 0)
        return rc;
    if (len != 4)
        return TLK_ERR_BAD_ENCODING;
    *out = tlk_get_le32(data);
    return 0;
}

int tlk_dec_u64(struct tlk_dec *dec, __u8 tag, __u64 *out)
{
    const __u8 *data;
    __u32 len;
    int rc = tlk_dec_lookup(dec, tag, &data, &len);
    if (rc != 0)
        return rc;
    if (len != 8)
        return TLK_ERR_BAD_ENCODING;
    *out = tlk_get_le64(data);
    return 0;
}

int tlk_dec_str(struct tlk_dec *dec, __u8 tag, struct tlk_str *out)
{
    return tlk_dec_field(dec, tag, out);
}

int tlk_dec_field(struct tlk_dec *dec, __u8 tag, struct tlk_str *out)
{
    const __u8 *data;
    __u32 len;
    int rc = tlk_dec_lookup(dec, tag, &data, &len);
    if (rc != 0)
        return rc;
    out->data = data;
    out->len = len;
    return 0;
}
