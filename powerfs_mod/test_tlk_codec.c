// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace round-trip test for the dual-environment TLV codec
 * (phase 4-6 §6e).
 *
 * Compiles tlk_codec.c with -U__KERNEL__ so the codec's byte logic
 * runs in userspace. Verifies the codec matches the wire format
 * defined in docs/lock-protocol.md and the Rust reference
 * (powerfs-lock-net/src/codec.rs).
 *
 * Build (no kernel tree needed):
 *   make test_codec
 *
 * The test does NOT link against any PowerFS library — it just
 * exercises the pure byte-logic of tlk_codec.c. A pass here means
 * the kernel build will produce identical wire bytes (the kernel
 * vs. userspace branches in tlk_codec.c only differ in memcpy/
 * printk init, not in byte layout).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* linux/types.h UAPI is available on the build host; this is what
 * the kernel's UAPI export provides to userspace. */
#include <linux/types.h>

#include "tlk_codec.h"

/* Test framework: simple assert with line capture. */
#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } else { \
        ++g_passes; \
    } \
} while (0)

#define TEST_CHECK_MSG(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s — %s\n", __FILE__, __LINE__, #cond, (msg)); \
        ++g_failures; \
    } else { \
        ++g_passes; \
    } \
} while (0)

static int g_passes = 0;
static int g_failures = 0;

/* ========== Helpers for building expected byte patterns ========== */

/* Build a frame manually so we have a known-good byte sequence to
 * compare against the encoder's output. Matches the wire format
 * documented in docs/lock-protocol.md §1. */
static void build_frame_manual(__u8 *buf, size_t *out_len,
                               __u8 msg_type,
                               const __u8 *payload, __u32 payload_len)
{
    buf[0] = msg_type;
    /* payload_len: 4 bytes little-endian */
    buf[1] = (__u8)(payload_len & 0xFF);
    buf[2] = (__u8)((payload_len >> 8) & 0xFF);
    buf[3] = (__u8)((payload_len >> 16) & 0xFF);
    buf[4] = (__u8)((payload_len >> 24) & 0xFF);
    memcpy(buf + 5, payload, payload_len);
    *out_len = 5 + payload_len;
}

static void put_u64_le(__u8 *p, __u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (__u8)((v >> (i * 8)) & 0xFF);
}

static void put_u32_le(__u8 *p, __u32 v)
{
    p[0] = (__u8)(v & 0xFF);
    p[1] = (__u8)((v >> 8) & 0xFF);
    p[2] = (__u8)((v >> 16) & 0xFF);
    p[3] = (__u8)((v >> 24) & 0xFF);
}

/* ========== Tests ========== */

/* 1. Round-trip a minimal Acquire frame and verify every field. */
static void test_roundtrip_acquire(void)
{
    __u8 buf[256];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));

    int rc = tlk_enc_frame_start(&enc, TLK_MSG_ACQUIRE);
    TEST_CHECK(rc == 0);

    TEST_CHECK(tlk_enc_u64(&enc, TLK_FIELD_INODE, 42) == 0);
    TEST_CHECK(tlk_enc_u8 (&enc, TLK_FIELD_MODE, TLK_MODE_EXCLUSIVE) == 0);
    TEST_CHECK(tlk_enc_u64(&enc, TLK_FIELD_TIMEOUT_MS, 30000) == 0);
    TEST_CHECK(tlk_enc_str(&enc, TLK_FIELD_CLIENT_ID,
                            "client-A", 8) == 0);

    int total = tlk_enc_frame_finalize(&enc);
    TEST_CHECK(total > 5);

    /* Verify exact byte layout against manual construction. */
    __u8 expected_payload[64];
    __u8 *p = expected_payload;
    /* FIELD_INODE (tag=0x01, len=8, value=42 LE) */
    *p++ = TLK_FIELD_INODE;
    put_u32_le(p, 8); p += 4;
    put_u64_le(p, 42); p += 8;
    /* FIELD_MODE (tag=0x03, len=1, value=0x01) */
    *p++ = TLK_FIELD_MODE;
    put_u32_le(p, 1); p += 4;
    *p++ = TLK_MODE_EXCLUSIVE;
    /* FIELD_TIMEOUT_MS (tag=0x06, len=8, value=30000 LE) */
    *p++ = TLK_FIELD_TIMEOUT_MS;
    put_u32_le(p, 8); p += 4;
    put_u64_le(p, 30000); p += 8;
    /* FIELD_CLIENT_ID (tag=0x09, len=8, value="client-A") */
    *p++ = TLK_FIELD_CLIENT_ID;
    put_u32_le(p, 8); p += 4;
    memcpy(p, "client-A", 8); p += 8;

    __u32 payload_len = (__u32)(p - expected_payload);
    __u8 expected_frame[256];
    size_t expected_frame_len;
    build_frame_manual(expected_frame, &expected_frame_len,
                       TLK_MSG_ACQUIRE, expected_payload, payload_len);

    TEST_CHECK((size_t)total == expected_frame_len);
    TEST_CHECK(memcmp(buf, expected_frame, expected_frame_len) == 0);

    /* Decode and verify fields round-trip correctly. */
    struct tlk_dec dec;
    __u8 msg_type;
    rc = tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_ACQUIRE);

    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 42);

    __u8 mode;
    TEST_CHECK(tlk_dec_u8(&dec, TLK_FIELD_MODE, &mode) == 0);
    TEST_CHECK(mode == TLK_MODE_EXCLUSIVE);

    __u64 timeout;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_TIMEOUT_MS, &timeout) == 0);
    TEST_CHECK(timeout == 30000);

    struct tlk_str client_id;
    TEST_CHECK(tlk_dec_str(&dec, TLK_FIELD_CLIENT_ID, &client_id) == 0);
    TEST_CHECK(client_id.len == 8);
    TEST_CHECK(memcmp(client_id.data, "client-A", 8) == 0);
}

/* 2. Verify byte layout matches the wire format spec: LE length, LE
 *    integer values. */
static void test_wire_format_byte_layout(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_GRANT);
    /* sn = 0x1122334455667788 — value chosen so each byte is
     * distinct, to catch endianness mistakes. */
    TEST_CHECK(tlk_enc_u64(&enc, TLK_FIELD_SN, 0x1122334455667788ULL) == 0);
    int total = tlk_enc_frame_finalize(&enc);

    /* Frame: [0x02][05 00 00 00][07 08 00 00 00][88 77 66 55 44 33 22 11] */
    TEST_CHECK(total == 5 + 5 + 8);
    TEST_CHECK(buf[0] == TLK_MSG_GRANT);
    /* payload_len = 13 = 0x0D */
    TEST_CHECK(buf[1] == 0x0D && buf[2] == 0x00 && buf[3] == 0x00 && buf[4] == 0x00);
    /* tag = SN */
    TEST_CHECK(buf[5] == TLK_FIELD_SN);
    /* len = 8 LE */
    TEST_CHECK(buf[6] == 0x08 && buf[7] == 0x00 && buf[8] == 0x00 && buf[9] == 0x00);
    /* value: 0x1122334455667788 in LE → 88 77 66 55 44 33 22 11 */
    TEST_CHECK(buf[10] == 0x88);
    TEST_CHECK(buf[11] == 0x77);
    TEST_CHECK(buf[12] == 0x66);
    TEST_CHECK(buf[13] == 0x55);
    TEST_CHECK(buf[14] == 0x44);
    TEST_CHECK(buf[15] == 0x33);
    TEST_CHECK(buf[16] == 0x22);
    TEST_CHECK(buf[17] == 0x11);
}

/* 3. Round-trip with field order independence: decode a frame whose
 *    fields are in a different order than the encoder would produce. */
static void test_field_order_independence(void)
{
    __u8 frame[64];
    __u8 *p = frame;
    *p++ = TLK_MSG_RELEASE;  /* msg_type */
    __u8 *len_pos = p;       /* reserve 4 bytes for payload_len */
    p += 4;
    __u8 *payload_start = p;

    /* Encode fields in REVERSE order vs. the Rust reference's
     * Release layout (which goes inode → token → client_id). */
    /* client_id first */
    *p++ = TLK_FIELD_CLIENT_ID; put_u32_le(p, 1); p += 4; *p++ = 'c';
    /* token second */
    *p++ = TLK_FIELD_TOKEN;    put_u32_le(p, 1); p += 4; *p++ = 't';
    /* inode third */
    *p++ = TLK_FIELD_INODE;    put_u32_le(p, 8); p += 4; put_u64_le(p, 99); p += 8;

    __u32 payload_len = (__u32)(p - payload_start);
    put_u32_le(len_pos, payload_len);
    size_t frame_len = (size_t)(p - frame);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_RELEASE);

    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 99);

    struct tlk_str token;
    TEST_CHECK(tlk_dec_str(&dec, TLK_FIELD_TOKEN, &token) == 0);
    TEST_CHECK(token.len == 1);
    TEST_CHECK(token.data[0] == 't');

    struct tlk_str client_id;
    TEST_CHECK(tlk_dec_str(&dec, TLK_FIELD_CLIENT_ID, &client_id) == 0);
    TEST_CHECK(client_id.len == 1);
    TEST_CHECK(client_id.data[0] == 'c');
}

/* 4. Duplicate tag detection (§2). */
static void test_duplicate_tag_rejected(void)
{
    __u8 frame[64];
    __u8 *p = frame;
    *p++ = TLK_MSG_ACQUIRE;
    __u8 *len_pos = p; p += 4;
    __u8 *payload_start = p;

    /* Two FIELD_INODE entries. */
    *p++ = TLK_FIELD_INODE; put_u32_le(p, 8); p += 4; put_u64_le(p, 1); p += 8;
    *p++ = TLK_FIELD_INODE; put_u32_le(p, 8); p += 4; put_u64_le(p, 2); p += 8;

    __u32 payload_len = (__u32)(p - payload_start);
    put_u32_le(len_pos, payload_len);
    size_t frame_len = (size_t)(p - frame);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    TEST_CHECK_MSG(rc == TLK_ERR_DUPLICATE, "duplicate tag should be rejected");
}

/* 5. Truncated frame header (frame_len < 5). */
static void test_truncated_header(void)
{
    __u8 short_buf[2] = {0x01, 0x00};
    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, short_buf, sizeof(short_buf), &msg_type);
    TEST_CHECK_MSG(rc == TLK_ERR_TRUNCATED, "short header should be truncated");
}

/* 6. Truncated field (payload ends mid-field). */
static void test_truncated_field(void)
{
    __u8 frame[16];
    __u8 *p = frame;
    *p++ = TLK_MSG_ACQUIRE;
    __u8 *len_pos = p; p += 4;
    __u8 *payload_start = p;

    /* tag + 4-byte len claiming 8 bytes of value, but we only supply 3. */
    *p++ = TLK_FIELD_INODE;
    put_u32_le(p, 8); p += 4;
    put_u64_le(p, 1); p += 8;  /* full 8-byte inode value */

    /* Now truncate: lie about payload_len so the decoder thinks the
     * payload ends inside this field. */
    __u32 real_payload_len = (__u32)(p - payload_start);
    __u32 lying_payload_len = real_payload_len - 4;  /* chop 4 bytes off end */
    put_u32_le(len_pos, lying_payload_len);
    size_t frame_len = (size_t)(p - frame);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    TEST_CHECK_MSG(rc == TLK_ERR_TRUNCATED, "truncated field should be detected");
}

/* 7. Payload_len larger than available buffer (overrun). */
static void test_payload_len_overrun(void)
{
    __u8 frame[16];
    frame[0] = TLK_MSG_ACQUIRE;
    /* claim 999 bytes of payload but provide only ~10 */
    put_u32_le(frame + 1, 999);
    memset(frame + 5, 0xAA, sizeof(frame) - 5);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, sizeof(frame), &msg_type);
    TEST_CHECK_MSG(rc == TLK_ERR_TRUNCATED, "payload_len > available should be truncated");
}

/* 8. Trailing bytes tolerated (forward compat, §1). */
static void test_trailing_bytes_tolerated(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_RELEASE_ACK);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 1);
    tlk_enc_u8 (&enc, TLK_FIELD_ERROR_CODE, TLK_ERR_OK);
    int total = tlk_enc_frame_finalize(&enc);

    /* Append 3 trailing bytes after the valid frame. */
    buf[total] = 0xAA;
    buf[total+1] = 0xBB;
    buf[total+2] = 0xCC;
    size_t with_trailer = (size_t)total + 3;

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, buf, with_trailer, &msg_type);
    TEST_CHECK_MSG(rc == 0, "trailing bytes should be tolerated");

    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 1);

    __u8 ec;
    TEST_CHECK(tlk_dec_u8(&dec, TLK_FIELD_ERROR_CODE, &ec) == 0);
    TEST_CHECK(ec == TLK_ERR_OK);
}

/* 9. Unknown tag silently skipped (forward compat, §2). */
static void test_unknown_tag_skipped(void)
{
    __u8 frame[64];
    __u8 *p = frame;
    *p++ = TLK_MSG_RELEASE;
    __u8 *len_pos = p; p += 4;
    __u8 *payload_start = p;

    /* Unknown tag 0x55 — must be silently skipped, not rejected. */
    *p++ = 0x55; put_u32_le(p, 4); p += 4;
    memcpy(p, "junk", 4); p += 4;

    /* Then a known FIELD_INODE. */
    *p++ = TLK_FIELD_INODE; put_u32_le(p, 8); p += 4; put_u64_le(p, 7); p += 8;

    __u32 payload_len = (__u32)(p - payload_start);
    put_u32_le(len_pos, payload_len);
    size_t frame_len = (size_t)(p - frame);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    TEST_CHECK_MSG(rc == 0, "unknown tags should be skipped");

    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 7);
}

/* 10. Absent field returns -ENODATA (graceful default path). */
static void test_absent_field_returns_enodata(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_REVOKE);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 5);
    tlk_enc_str(&enc, TLK_FIELD_TOKEN, "tok", 3);
    int total = tlk_enc_frame_finalize(&enc);

    struct tlk_dec dec;
    __u8 msg_type;
    tlk_dec_init(&dec, buf, (size_t)total, &msg_type);

    /* FIELD_SN is absent — should get -ENODATA. */
    __u64 sn;
    int rc = tlk_dec_u64(&dec, TLK_FIELD_SN, &sn);
    TEST_CHECK_MSG(rc == -ENODATA, "absent field should return -ENODATA");
}

/* 11. Encoder overflow detection (cap exhausted). */
static void test_encoder_overflow(void)
{
    __u8 tiny_buf[6];  /* fits frame header (5) + 1 byte */
    struct tlk_enc enc;
    tlk_enc_init(&enc, tiny_buf, sizeof(tiny_buf));
    tlk_enc_frame_start(&enc, TLK_MSG_ACQUIRE);

    /* Try to write a u64 (needs 5 + 8 = 13 bytes total — overflows. */
    int rc = tlk_enc_u64(&enc, TLK_FIELD_INODE, 1);
    TEST_CHECK_MSG(rc == TLK_ERR_NO_SPACE, "encoder should reject on overflow");
}

/* 12. Frame header must be started before any field write. */
static void test_field_without_frame_start(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    /* No frame_start call. */
    int rc = tlk_enc_u64(&enc, TLK_FIELD_INODE, 1);
    TEST_CHECK_MSG(rc == TLK_ERR_BAD_ENCODING,
                   "field write before frame_start should fail");
}

/* 13. finalize before frame_start should fail. */
static void test_finalize_without_start(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    int rc = tlk_enc_frame_finalize(&enc);
    TEST_CHECK_MSG(rc == TLK_ERR_BAD_ENCODING,
                   "finalize before frame_start should fail");
}

/* 14. Full Acquire with range-level lock (LockMode::Range semantics). */
static void test_roundtrip_acquire_range(void)
{
    __u8 buf[256];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_ACQUIRE);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 42);
    tlk_enc_u8 (&enc, TLK_FIELD_MODE, TLK_MODE_RANGE);
    tlk_enc_u64(&enc, TLK_FIELD_RANGE_START, 0);
    tlk_enc_u64(&enc, TLK_FIELD_RANGE_END, 4096);
    tlk_enc_u64(&enc, TLK_FIELD_TIMEOUT_MS, 5000);
    tlk_enc_str(&enc, TLK_FIELD_CLIENT_ID, "c", 1);
    int total = tlk_enc_frame_finalize(&enc);
    TEST_CHECK(total > 0);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_ACQUIRE);

    __u8 mode;
    TEST_CHECK(tlk_dec_u8(&dec, TLK_FIELD_MODE, &mode) == 0);
    TEST_CHECK(mode == TLK_MODE_RANGE);

    __u64 rs, re;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_RANGE_START, &rs) == 0);
    TEST_CHECK(rs == 0);
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_RANGE_END, &re) == 0);
    TEST_CHECK(re == 4096);
}

/* 15. EOF range: FIELD_RANGE_END absent (per Rust reference, EOF
 *     ranges omit the END field). */
static void test_roundtrip_eof_range(void)
{
    __u8 buf[256];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_INVALIDATE);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 1);
    tlk_enc_u64(&enc, TLK_FIELD_RANGE_START, 100);
    /* FIELD_RANGE_END intentionally omitted → "to EOF" */
    int total = tlk_enc_frame_finalize(&enc);

    struct tlk_dec dec;
    __u8 msg_type;
    tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(msg_type == TLK_MSG_INVALIDATE);

    __u64 rs;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_RANGE_START, &rs) == 0);
    TEST_CHECK(rs == 100);

    __u64 re;
    int rc = tlk_dec_u64(&dec, TLK_FIELD_RANGE_END, &re);
    TEST_CHECK_MSG(rc == -ENODATA, "EOF range should omit FIELD_RANGE_END");
}

/* 16. Grant with SN=0 should still round-trip (decoder returns -ENODATA
 *     for SN, caller defaults to 0). */
static void test_grant_sn_zero_default(void)
{
    __u8 buf[256];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_GRANT);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 1);
    tlk_enc_str(&enc, TLK_FIELD_TOKEN, "tok", 3);
    /* SN intentionally omitted (sn=0 in modularization phase). */
    tlk_enc_u64(&enc, TLK_FIELD_LEASE_MS, 30000);
    int total = tlk_enc_frame_finalize(&enc);

    struct tlk_dec dec;
    __u8 msg_type;
    tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(msg_type == TLK_MSG_GRANT);

    /* Caller pattern: SN optional, default to 0. */
    __u64 sn = 999;  /* poison */
    int rc = tlk_dec_u64(&dec, TLK_FIELD_SN, &sn);
    if (rc == -ENODATA)
        sn = 0;  /* default */
    TEST_CHECK(sn == 0);
}

/* 17. Field value length mismatch (e.g. claiming u64 but len=4). */
static void test_value_length_mismatch(void)
{
    __u8 frame[32];
    __u8 *p = frame;
    *p++ = TLK_MSG_ACQUIRE;
    __u8 *len_pos = p; p += 4;
    __u8 *payload_start = p;

    /* FIELD_INODE with len=4 (should be 8 for u64). */
    *p++ = TLK_FIELD_INODE;
    put_u32_le(p, 4); p += 4;
    put_u32_le(p, 0xDEADBEEF); p += 4;

    __u32 payload_len = (__u32)(p - payload_start);
    put_u32_le(len_pos, payload_len);
    size_t frame_len = (size_t)(p - frame);

    struct tlk_dec dec;
    __u8 msg_type;
    tlk_dec_init(&dec, frame, frame_len, &msg_type);

    __u64 inode;
    int rc = tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode);
    TEST_CHECK_MSG(rc == TLK_ERR_BAD_ENCODING,
                   "u64 field with len=4 should be bad encoding");
}

/* 18. Empty payload (frame_len == 5). */
static void test_empty_payload(void)
{
    __u8 frame[5] = {TLK_MSG_RELEASE, 0, 0, 0, 0};
    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, sizeof(frame), &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_RELEASE);

    /* All field lookups should return -ENODATA. */
    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == -ENODATA);
}

/* 19. Round-trip a Renew frame with all fields present. */
static void test_roundtrip_renew(void)
{
    __u8 buf[256];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_RENEW);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 7);
    tlk_enc_str(&enc, TLK_FIELD_TOKEN, "renew-tok", 9);
    tlk_enc_u64(&enc, TLK_FIELD_TIMEOUT_MS, 60000);
    tlk_enc_str(&enc, TLK_FIELD_CLIENT_ID, "client-B", 8);
    int total = tlk_enc_frame_finalize(&enc);
    TEST_CHECK(total > 0);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_RENEW);

    __u64 inode, timeout;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 7);
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_TIMEOUT_MS, &timeout) == 0);
    TEST_CHECK(timeout == 60000);

    struct tlk_str token, client_id;
    TEST_CHECK(tlk_dec_str(&dec, TLK_FIELD_TOKEN, &token) == 0);
    TEST_CHECK(token.len == 9);
    TEST_CHECK(memcmp(token.data, "renew-tok", 9) == 0);
    TEST_CHECK(tlk_dec_str(&dec, TLK_FIELD_CLIENT_ID, &client_id) == 0);
    TEST_CHECK(client_id.len == 8);
    TEST_CHECK(memcmp(client_id.data, "client-B", 8) == 0);
}

/* 20. Round-trip a RevokeAck frame. */
static void test_roundtrip_revoke_ack(void)
{
    __u8 buf[128];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_REVOKE_ACK);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 99);
    tlk_enc_str(&enc, TLK_FIELD_TOKEN, "rvk", 3);
    tlk_enc_str(&enc, TLK_FIELD_CLIENT_ID, "c", 1);
    int total = tlk_enc_frame_finalize(&enc);

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, buf, (size_t)total, &msg_type);
    TEST_CHECK(rc == 0);
    TEST_CHECK(msg_type == TLK_MSG_REVOKE_ACK);

    __u64 inode;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode) == 0);
    TEST_CHECK(inode == 99);
}

/* 21. Decoder must reject payload_len > frame_len - 5 even if the
 *     first few bytes look like a valid field header. (security
 *     regression: covers the off-by-one in early prototype.) */
static void test_payload_len_just_over(void)
{
    __u8 frame[16];
    frame[0] = TLK_MSG_ACQUIRE;
    /* payload_len = 11, but we only supply 10 bytes after header. */
    put_u32_le(frame + 1, 11);
    memset(frame + 5, 0, sizeof(frame) - 5);
    size_t frame_len = 5 + 10;  /* only 10 payload bytes present */

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    TEST_CHECK_MSG(rc == TLK_ERR_TRUNCATED,
                   "payload_len > available should be truncated");
}

/* 22. Encoder: double frame_start should fail. */
static void test_double_frame_start(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    TEST_CHECK(tlk_enc_frame_start(&enc, TLK_MSG_ACQUIRE) == 0);
    int rc = tlk_enc_frame_start(&enc, TLK_MSG_GRANT);
    TEST_CHECK_MSG(rc == TLK_ERR_BAD_ENCODING,
                   "double frame_start should fail");
}

/* 23. Encoder: u8 / u32 typed helpers produce the right bytes. */
static void test_typed_helpers(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_RELEASE_ACK);
    tlk_enc_u8 (&enc, TLK_FIELD_ERROR_CODE, 0xAB);
    tlk_enc_u32(&enc, TLK_FIELD_TIMEOUT_MS, 0x12345678);
    int total = tlk_enc_frame_finalize(&enc);

    /* Layout: msg_type=04, payload_len=0F 00 00 00
     *         FIELD_ERROR_CODE: 0A 01 00 00 00 AB        (6 bytes)
     *         FIELD_TIMEOUT_MS:  06 04 00 00 00 78 56 34 12  (9 bytes)
     * Total payload = 6 + 9 = 15 = 0x0F. */
    TEST_CHECK(buf[0] == TLK_MSG_RELEASE_ACK);
    TEST_CHECK(buf[1] == 0x0F);
    TEST_CHECK(buf[2] == 0x00);
    TEST_CHECK(buf[3] == 0x00);
    TEST_CHECK(buf[4] == 0x00);
    TEST_CHECK(buf[5] == TLK_FIELD_ERROR_CODE);
    TEST_CHECK(buf[6] == 0x01);
    TEST_CHECK(buf[7] == 0x00);
    TEST_CHECK(buf[8] == 0x00);
    TEST_CHECK(buf[9] == 0x00);
    TEST_CHECK(buf[10] == 0xAB);
    TEST_CHECK(buf[11] == TLK_FIELD_TIMEOUT_MS);
    TEST_CHECK(buf[12] == 0x04);
    TEST_CHECK(buf[13] == 0x00);
    TEST_CHECK(buf[14] == 0x00);
    TEST_CHECK(buf[15] == 0x00);
    TEST_CHECK(buf[16] == 0x78);
    TEST_CHECK(buf[17] == 0x56);
    TEST_CHECK(buf[18] == 0x34);
    TEST_CHECK(buf[19] == 0x12);
    (void)total;
}

/* 24. Repeated lookups of the same field work (one-pass index). */
static void test_repeated_lookup(void)
{
    __u8 buf[64];
    struct tlk_enc enc;
    tlk_enc_init(&enc, buf, sizeof(buf));
    tlk_enc_frame_start(&enc, TLK_MSG_GRANT);
    tlk_enc_u64(&enc, TLK_FIELD_INODE, 1234);
    int total = tlk_enc_frame_finalize(&enc);

    struct tlk_dec dec;
    __u8 msg_type;
    tlk_dec_init(&dec, buf, (size_t)total, &msg_type);

    __u64 inode1, inode2, inode3;
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode1) == 0);
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode2) == 0);
    TEST_CHECK(tlk_dec_u64(&dec, TLK_FIELD_INODE, &inode3) == 0);
    TEST_CHECK(inode1 == 1234);
    TEST_CHECK(inode2 == 1234);
    TEST_CHECK(inode3 == 1234);
}

int main(void)
{
    printf("=== tlk_codec round-trip tests ===\n");

    test_roundtrip_acquire();
    test_wire_format_byte_layout();
    test_field_order_independence();
    test_duplicate_tag_rejected();
    test_truncated_header();
    test_truncated_field();
    test_payload_len_overrun();
    test_trailing_bytes_tolerated();
    test_unknown_tag_skipped();
    test_absent_field_returns_enodata();
    test_encoder_overflow();
    test_field_without_frame_start();
    test_finalize_without_start();
    test_roundtrip_acquire_range();
    test_roundtrip_eof_range();
    test_grant_sn_zero_default();
    test_value_length_mismatch();
    test_empty_payload();
    test_roundtrip_renew();
    test_roundtrip_revoke_ack();
    test_payload_len_just_over();
    test_double_frame_start();
    test_typed_helpers();
    test_repeated_lookup();

    printf("\n");
    printf("=== Results: %d passed, %d failed ===\n", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
