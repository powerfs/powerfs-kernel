/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PowerFS Lock Wire-Protocol TLV Codec (phase 4-6 skeleton).
 *
 * Authoritative source: docs/lock-protocol.md.
 * Mirrors the Rust `powerfs-lock-net/src/codec.rs` byte-for-byte.
 *
 * Wire format (all multi-byte integers little-endian, matching the
 * Rust side which uses `to_le_bytes()`):
 *
 *   Frame:
 *     +----------+-------------------+----------------------+
 *     | msg_type | payload_len (u32) | payload (bytes)       |
 *     | 1 byte   | 4 bytes LE        | payload_len bytes    |
 *     +----------+-------------------+----------------------+
 *
 *   Payload = sequence of TLV fields, in arbitrary order:
 *     +----------+----------------+---------------------+
 *     | tag (u8) | len (u32 LE)   | value (len bytes)   |
 *     +----------+----------------+---------------------+
 *
 * IMPORTANT: This length field is 4 bytes LITTLE-endian, which
 * differs from the data-path TLV in powerfs_tlv.c (4 bytes
 * big-endian). The two codecs are NOT interchangeable — see
 * powerfs_tlv.c's header comment for the data-path convention.
 *
 * The codec is dual-environment: it compiles both in the kernel
 * build (with linux/types.h) and in userspace (for the round-trip
 * test in tests/test_tlk_codec.c). It uses only the UAPI type
 * names (__u8/__u32/__u64) which are available in both.
 */
#ifndef _TLK_CODEC_H
#define _TLK_CODEC_H

/* UAPI types: available in kernel via <linux/types.h> and in
 * userspace via the UAPI headers installed under /usr/include/linux. */
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
#include <linux/types.h>  /* UAPI: __u8, __u32, __u64, __s32 */
#include <errno.h>
#include <string.h>
#endif

/* ========== Wire protocol constants ==========
 *
 * Keep in sync with powerfs-lock-net/src/msg.rs and
 * docs/lock-protocol.md. When the protocol changes, update the doc
 * first, then both ends.
 */

/* Message types (msg_type byte) — §6 */
#define TLK_MSG_ACQUIRE      0x01  /* client → server */
#define TLK_MSG_GRANT        0x02  /* server → client */
#define TLK_MSG_RELEASE      0x03  /* client → server */
#define TLK_MSG_RELEASE_ACK  0x04  /* server → client */
#define TLK_MSG_RENEW        0x05  /* client → server */
#define TLK_MSG_RENEW_ACK    0x06  /* server → client */
#define TLK_MSG_REVOKE       0x07  /* server → client (Early Revoke) */
#define TLK_MSG_INVALIDATE   0x08  /* server → client */
#define TLK_MSG_REVOKE_ACK   0x09  /* client → server */

/* TLV field tags — §3 */
#define TLK_FIELD_INODE        0x01
#define TLK_FIELD_TOKEN        0x02
#define TLK_FIELD_MODE         0x03
#define TLK_FIELD_RANGE_START  0x04
#define TLK_FIELD_RANGE_END    0x05  /* absent = EOF */
#define TLK_FIELD_TIMEOUT_MS   0x06
#define TLK_FIELD_SN           0x07  /* omitted when sn == 0 */
#define TLK_FIELD_LEASE_MS     0x08
#define TLK_FIELD_CLIENT_ID    0x09
#define TLK_FIELD_ERROR_CODE   0x0A

/* Lock modes (FIELD_MODE byte) — §4 */
#define TLK_MODE_SHARED    0x00
#define TLK_MODE_EXCLUSIVE 0x01
#define TLK_MODE_RANGE     0x02

/* Error codes (FIELD_ERROR_CODE byte) — §5 */
#define TLK_ERR_OK                    0x00
#define TLK_ERR_NOT_FOUND             0x01
#define TLK_ERR_HOLDER_MISMATCH       0x02
#define TLK_ERR_EXPIRED               0x03
#define TLK_ERR_EXPIRED_BEYOND_GRACE  0x04
#define TLK_ERR_CONFLICT              0x05
#define TLK_ERR_KEY_NOT_COVERED       0x06
#define TLK_ERR_QUARANTINED           0x07
#define TLK_ERR_NETWORK               0x08
#define TLK_ERR_INTERNAL              0x09

/* EOF sentinel for FIELD_RANGE_END when the field must be present
 * (range-level locks). §3 says: prefer omitting the field for
 * full-inode invalidation. */
#define TLK_RANGE_END_EOF_SENTINEL 0xFFFFFFFFFFFFFFFFULL

/* ========== Error codes returned by the codec ==========
 *
 * In the kernel these come from <linux/errno.h>; in userspace from
 * <errno.h>. We use the same values since they overlap on Linux:
 *   -ERANGE = -34, -ENOSPC = -28, -EILSEQ = -84, -EBADMSG = -74,
 *   -ENODATA = -61, -E2BIG = -7.
 */
#ifndef ERANGE
#define ERANGE 34
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef E2BIG
#define E2BIG 7
#endif

/* Negative errno-style return values, matching kernel convention. */
#define TLK_ERR_TRUNCATED    (-ERANGE)     /* payload ended mid-field */
#define TLK_ERR_NO_SPACE     (-ENOSPC)     /* output buffer full */
#define TLK_ERR_BAD_ENCODING (-EILSEQ)     /* invalid LE bytes / bad len */
#define TLK_ERR_BAD_MSG_TYPE (-EBADMSG)    /* unknown msg_type byte */
#define TLK_ERR_DUPLICATE    (-ENODATA)    /* tag appears twice in one msg */
#define TLK_ERR_TOO_LONG     (-E2BIG)      /* field value length unreasonably large */

/* ========== Encoder ==========
 *
 * The encoder writes into a caller-supplied buffer. The caller
 * retains ownership of the buffer; the encoder just tracks the
 * current length and capacity. Use `tlk_enc_frame_finalize` to
 * get back the total frame length (msg_type + payload_len + payload).
 *
 * Typical usage for an Acquire request:
 *
 *   struct tlk_enc enc;
 *   __u8 buf[256];
 *   tlk_enc_init(&enc, buf, sizeof(buf));
 *   tlk_enc_frame_start(&enc, TLK_MSG_ACQUIRE);
 *   tlk_enc_u64(&enc, TLK_FIELD_INODE, 42);
 *   tlk_enc_u8 (&enc, TLK_FIELD_MODE, TLK_MODE_EXCLUSIVE);
 *   tlk_enc_u64(&enc, TLK_FIELD_TIMEOUT_MS, 30000);
 *   tlk_enc_str(&enc, TLK_FIELD_CLIENT_ID, "client-A", 9);
 *   // enc.len is now the total frame length
 */
struct tlk_enc {
    __u8 *buf;
    size_t len;   /* total bytes written (including 5-byte frame header) */
    size_t cap;
    /* Offset of the payload_len field in the buffer (for
     * back-patching once the payload length is known). The frame
     * layout is: [msg_type:1][payload_len:4][payload:...]; we
     * reserve the 4 bytes at offset 1 and patch them at finalize. */
    size_t payload_len_off;
    /* Has the frame header been written? */
    int frame_started;
};

/* Init the encoder. Caller owns `buf`. */
void tlk_enc_init(struct tlk_enc *enc, __u8 *buf, size_t cap);

/* Write the frame header (msg_type + reserved payload_len bytes).
 * Must be called exactly once before any field writes. */
int tlk_enc_frame_start(struct tlk_enc *enc, __u8 msg_type);

/* Append a TLV field. `value` points to `len` bytes; the bytes are
 * copied verbatim. */
int tlk_enc_field(struct tlk_enc *enc, __u8 tag,
                   const void *value, __u32 len);

/* Typed field helpers (LE for integers, verbatim for strings). */
int tlk_enc_u8(struct tlk_enc *enc, __u8 tag, __u8 val);
int tlk_enc_u32(struct tlk_enc *enc, __u8 tag, __u32 val);
int tlk_enc_u64(struct tlk_enc *enc, __u8 tag, __u64 val);
/* `str` is NOT NUL-terminated on the wire; pass the real byte
 * length (e.g. strlen(s) for C strings). */
int tlk_enc_str(struct tlk_enc *enc, __u8 tag, const char *str, __u32 len);

/* Back-patch the payload_len field with the actual payload length.
 * Returns the total frame length (msg_type + 4 + payload_len) on
 * success, or a negative TLK_ERR_* on failure. Idempotent. */
int tlk_enc_frame_finalize(struct tlk_enc *enc);

/* ========== Decoder ==========
 *
 * The decoder reads from a caller-supplied frame buffer. It first
 * parses the frame header (msg_type + payload_len), then exposes
 * the payload as a sequence of TLV fields.
 *
 * Field access is by tag (the wire format allows fields in arbitrary
 * order, §2). Unknown tags are silently skipped (forward compat, §2).
 * Duplicate tags are an error (§2).
 *
 * Typical usage:
 *
 *   struct tlk_dec dec;
 *   __u8 msg_type;
 *   if (tlk_dec_init(&dec, buf, buf_len, &msg_type) < 0) ...;
 *
 *   __u64 inode;
 *   if (tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0) ...
 *
 *   struct tlk_str token;
 *   if (tlk_dec_str(&dec, TLK_FIELD_TOKEN, &token) == 0) ...
 *
 *   // Unknown fields are automatically skipped during iteration.
 */
struct tlk_str {
    const __u8 *data;  /* points into the decoder's buffer; NUL-terminate before printk */
    __u32 len;
};

struct tlk_dec {
    const __u8 *frame;     /* start of the frame buffer */
    size_t frame_len;
    /* Parsed frame header */
    __u8 msg_type;
    __u32 payload_len;
    const __u8 *payload;   /* = frame + 5 */
    /* Field index: built once in `tlk_dec_init` by scanning the
     * whole payload. `field_off[tag]` is the offset of `tag`'s
     * value within `payload` (0xFFFF = absent). `field_len[tag]`
     * is the value length. This mirrors the Rust reference's
     * `HashMap<u8, Vec<u8>>` parse-all-then-lookup approach, so:
     *   - Duplicate tags are detected during init (not lazily),
     *     matching the spec §2 "Duplicate tags are a decode error".
     *   - Re-looking up the same field works (returns the same
     *     value on each call).
     *   - Unknown tags are silently skipped during init (§2). */
    __u16 field_off[256];
    __u16 field_len[256];
};

/* Parse the frame header AND all TLV fields into the field index.
 * On success returns 0 and fills `msg_type`. `frame_len` MUST be
 * >= payload_len + 5; if it's larger, trailing bytes are tolerated
 * (§1 forward compat); if smaller, returns TLK_ERR_TRUNCATED.
 * Duplicate tags → TLK_ERR_DUPLICATE. */
int tlk_dec_init(struct tlk_dec *dec, const __u8 *frame, size_t frame_len,
                 __u8 *msg_type);

/* Look up a u8 field by tag. Returns 0 on success (found), -ENODATA
 * (not present, treated as "absent → caller uses default"), or a
 * negative TLK_ERR_* on decode error. */
int tlk_dec_u8(struct tlk_dec *dec, __u8 tag, __u8 *out);
int tlk_dec_u32(struct tlk_dec *dec, __u8 tag, __u32 *out);
int tlk_dec_u64(struct tlk_dec *dec, __u8 tag, __u64 *out);

/* Look up a string field by tag. On success `out->data` points into
 * the decoder's frame buffer (no copy); `out->len` is the byte length
 * (NOT NUL-terminated). The caller MUST `memcpy` if it needs a
 * NUL-terminated string. */
int tlk_dec_str(struct tlk_dec *dec, __u8 tag, struct tlk_str *out);

/* Look up a raw byte-range field by tag (for unknown field types or
 * opaque values). Same semantics as `tlk_dec_str`. */
int tlk_dec_field(struct tlk_dec *dec, __u8 tag, struct tlk_str *out);

#endif /* _TLK_CODEC_H */
