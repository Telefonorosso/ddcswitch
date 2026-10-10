/*
 * DDCSwitch 0.5 - native Emu68 / Raspberry Pi 0-3 HDMI DDC/CI POC
 *
 * AmigaOS CLI utility. No Emu68 kernel modification, no network, no PC helper.
 * Sends MCCS commands directly over the BCM283x BSC2 controller used by HDMI
 * DDC. Hardware-validated for Set VCP 0x60 on Raspberry Pi 3A+ / Emu68.
 *
 * Usage:
 *   DDCSwitch 16
 *   DDCSwitch 17
 *   DDCSwitch 0x10
 *   DDCSwitch 0x11
 *   DDCSwitch LIST
 *   DDCSwitch CURRENT
 *   DDCSwitch SWITCH 16 17
 *
 * LIST reads the monitor DDC/CI capabilities string (F3/E3), extracts the
 * values advertised for VCP feature 0x60 (Input Source), and prints them.
 * Capabilities are advisory: some monitors report incomplete/wrong lists.
 */

#include <stdio.h>
#include <stdint.h>
#include <proto/dos.h>
#include <dos/rdargs.h>

static const char ver[] = "$VER: DDCSwitch 0.5 (10.10.2026)";

#define BSC2_BASE       0xF2805000UL

#define BSC_C           0x00UL
#define BSC_S           0x04UL
#define BSC_DLEN        0x08UL
#define BSC_A           0x0cUL
#define BSC_FIFO        0x10UL

#define BSC_C_I2CEN      (1UL << 15)
#define BSC_C_ST         (1UL << 7)
#define BSC_C_CLEAR_1    (1UL << 4)
#define BSC_C_CLEAR_MASK (3UL << 4)
#define BSC_C_READ       (1UL << 0)

#define BSC_S_CLKT      (1UL << 9)
#define BSC_S_ERR       (1UL << 8)
#define BSC_S_RXD       (1UL << 5)
#define BSC_S_DONE      (1UL << 1)
#define BSC_S_TA        (1UL << 0)

#define DDC_CI_ADDR_7BIT    0x37U
#define DDC_CI_ADDR_WRITE   0x6eU
#define DDC_CI_REPLY_XOR    0x50U
#define VCP_INPUT_SOURCE    0x60U

#define DDC_VCP_REQUEST     0x01U
#define DDC_VCP_REPLY       0x02U
#define DDC_CAPS_REQUEST    0xf3U
#define DDC_CAPS_REPLY      0xe3U
#define DDC_CAPS_DATA_MAX   32U
#define DDC_CAPS_PAYLOAD_MAX (3U + DDC_CAPS_DATA_MAX)
#define DDC_CAPS_RAW_READ   (DDC_CAPS_PAYLOAD_MAX + 3U)
#define DDC_CAPS_STRING_MAX 4095U

static char g_caps[DDC_CAPS_STRING_MAX + 1U];

static inline uint32_t bswap32(uint32_t v)
{
    return ((v & 0x000000ffUL) << 24) |
           ((v & 0x0000ff00UL) <<  8) |
           ((v & 0x00ff0000UL) >>  8) |
           ((v & 0xff000000UL) >> 24);
}

static inline uint32_t bsc_rd(uint32_t off)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(BSC2_BASE + off);
    return bswap32(*p);
}

static inline void bsc_wr(uint32_t off, uint32_t value)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(BSC2_BASE + off);
    *p = bswap32(value);
}

static uint8_t ddc_checksum(const uint8_t *body, unsigned n)
{
    uint8_t c = DDC_CI_ADDR_WRITE;
    unsigned i;

    for (i = 0; i < n; ++i)
        c ^= body[i];

    return c;
}

/* Preserve the already hardware-validated Set VCP transaction unchanged. */
static int ddc_set_vcp(uint8_t vcp, uint16_t value, uint32_t *status_out)
{
    uint8_t pkt[7];
    uint32_t old_c, old_a, old_dlen;
    uint32_t s;
    unsigned i;
    unsigned tick;
    int rc = 0;

    s = bsc_rd(BSC_S);
    if (s & BSC_S_TA) {
        if (status_out) *status_out = s;
        return -10;
    }

    old_c    = bsc_rd(BSC_C);
    old_a    = bsc_rd(BSC_A);
    old_dlen = bsc_rd(BSC_DLEN);

    pkt[0] = 0x51;
    pkt[1] = 0x84;
    pkt[2] = 0x03;
    pkt[3] = vcp;
    pkt[4] = (uint8_t)(value >> 8);
    pkt[5] = (uint8_t)value;
    pkt[6] = ddc_checksum(pkt, 6);

    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_CLEAR_1);
    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, DDC_CI_ADDR_7BIT);
    bsc_wr(BSC_DLEN, sizeof(pkt));

    for (i = 0; i < sizeof(pkt); ++i)
        bsc_wr(BSC_FIFO, pkt[i]);

    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_ST);

    s = 0;
    for (tick = 0; tick < 50; ++tick) {
        s = bsc_rd(BSC_S);
        if (s & (BSC_S_DONE | BSC_S_ERR | BSC_S_CLKT))
            break;
        Delay(1);
    }

    if (s & BSC_S_CLKT)
        rc = -3;
    else if (s & BSC_S_ERR)
        rc = -2;
    else if (!(s & BSC_S_DONE))
        rc = -1;

    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, old_a);
    bsc_wr(BSC_DLEN, old_dlen);
    bsc_wr(BSC_C, old_c & ~(BSC_C_ST | BSC_C_CLEAR_MASK));

    if (status_out) *status_out = s;
    return rc;
}

/* Small write helper used only by LIST. Packet already includes checksum. */
static int bsc_write_packet(const uint8_t *pkt, unsigned bytes, uint32_t *status_out)
{
    uint32_t old_c, old_a, old_dlen;
    uint32_t s;
    unsigned i, tick;
    int rc = 0;

    if (!pkt || !bytes || bytes > 16U)
        return -20;

    s = bsc_rd(BSC_S);
    if (s & BSC_S_TA) {
        if (status_out) *status_out = s;
        return -10;
    }

    old_c    = bsc_rd(BSC_C);
    old_a    = bsc_rd(BSC_A);
    old_dlen = bsc_rd(BSC_DLEN);

    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_CLEAR_1);
    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, DDC_CI_ADDR_7BIT);
    bsc_wr(BSC_DLEN, bytes);

    for (i = 0; i < bytes; ++i)
        bsc_wr(BSC_FIFO, pkt[i]);

    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_ST);

    s = 0;
    for (tick = 0; tick < 50U; ++tick) {
        s = bsc_rd(BSC_S);
        if (s & (BSC_S_DONE | BSC_S_ERR | BSC_S_CLKT))
            break;
        Delay(1);
    }

    if (s & BSC_S_CLKT) rc = -3;
    else if (s & BSC_S_ERR) rc = -2;
    else if (!(s & BSC_S_DONE)) rc = -1;

    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, old_a);
    bsc_wr(BSC_DLEN, old_dlen);
    bsc_wr(BSC_C, old_c & ~(BSC_C_ST | BSC_C_CLEAR_MASK));

    if (status_out) *status_out = s;
    return rc;
}

/*
 * Read exactly 'bytes' from DDC/CI slave 0x37.
 * A DDC capabilities reply is at most 38 bytes including framing/checksum.
 * The BSC FIFO is only 16 bytes, so RXD must be drained while the transfer is
 * active; using DOS Delay() here would be far too slow and could overflow it.
 */
static int bsc_read_packet(uint8_t *buf, unsigned bytes, uint32_t *status_out)
{
    uint32_t old_c, old_a, old_dlen;
    uint32_t s;
    unsigned got = 0;
    unsigned long guard = 4000000UL;
    int rc = 0;

    if (!buf || !bytes)
        return -20;

    s = bsc_rd(BSC_S);
    if (s & BSC_S_TA) {
        if (status_out) *status_out = s;
        return -10;
    }

    old_c    = bsc_rd(BSC_C);
    old_a    = bsc_rd(BSC_A);
    old_dlen = bsc_rd(BSC_DLEN);

    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_CLEAR_1);
    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, DDC_CI_ADDR_7BIT);
    bsc_wr(BSC_DLEN, bytes);
    bsc_wr(BSC_C, BSC_C_I2CEN | BSC_C_ST | BSC_C_READ);

    s = 0;
    while (guard--) {
        s = bsc_rd(BSC_S);

        while (got < bytes && (s & BSC_S_RXD)) {
            buf[got++] = (uint8_t)bsc_rd(BSC_FIFO);
            s = bsc_rd(BSC_S);
        }

        if (s & (BSC_S_ERR | BSC_S_CLKT))
            break;

        if ((s & BSC_S_DONE) && !(s & BSC_S_RXD))
            break;
    }

    /* Drain anything left in FIFO after DONE. */
    s = bsc_rd(BSC_S);
    while (got < bytes && (s & BSC_S_RXD)) {
        buf[got++] = (uint8_t)bsc_rd(BSC_FIFO);
        s = bsc_rd(BSC_S);
    }

    if (s & BSC_S_CLKT) rc = -3;
    else if (s & BSC_S_ERR) rc = -2;
    else if (got != bytes) rc = -4;
    else if (!(s & BSC_S_DONE)) rc = -1;

    bsc_wr(BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
    bsc_wr(BSC_A, old_a);
    bsc_wr(BSC_DLEN, old_dlen);
    bsc_wr(BSC_C, old_c & ~(BSC_C_ST | BSC_C_CLEAR_MASK | BSC_C_READ));

    if (status_out) *status_out = s;
    return rc;
}


/* Read one VCP value. This is much faster than LIST: one request + one reply.
 * Reply payload: 02 RC CP T MH ML SH SL. For simple non-continuous VCP 0x60
 * the active input is the 16-bit SH:SL current value (normally only SL used).
 */
static int ddc_get_vcp(uint8_t vcp, uint16_t *current, uint16_t *maximum,
                       uint8_t *type_out)
{
    uint8_t req[5];
    uint8_t raw[11];
    uint32_t status;
    uint8_t x;
    unsigned i, attempt;
    int rc;

    if (!current || !maximum)
        return -20;

    req[0] = 0x51;
    req[1] = 0x82;
    req[2] = DDC_VCP_REQUEST;
    req[3] = vcp;
    req[4] = ddc_checksum(req, 4);

    for (attempt = 0; attempt < 3U; ++attempt) {
        rc = bsc_write_packet(req, sizeof(req), &status);
        if (rc != 0) {
            Delay(3);
            continue;
        }

        /* DDC/CI Get VCP reply must not be read immediately. */
        Delay(3);

        rc = bsc_read_packet(raw, sizeof(raw), &status);
        if (rc != 0) {
            Delay(2);
            continue;
        }

        /* Some displays return a busy/no-reply byte before a real answer. */
        if (raw[0] == 0x51U || raw[0] == 0xffU) {
            Delay(2);
            continue;
        }

        if (raw[0] != DDC_CI_ADDR_WRITE || raw[1] != 0x88U ||
            raw[2] != DDC_VCP_REPLY || raw[4] != vcp) {
            Delay(2);
            continue;
        }

        x = DDC_CI_REPLY_XOR;
        for (i = 0; i < sizeof(raw); ++i)
            x ^= raw[i];
        if (x != 0U) {
            Delay(2);
            continue;
        }

        /* RC != 0 means unsupported/invalid VCP feature. */
        if (raw[3] != 0U)
            return -40;

        if (type_out) *type_out = raw[5];
        *maximum = (uint16_t)(((uint16_t)raw[6] << 8) | raw[7]);
        *current = (uint16_t)(((uint16_t)raw[8] << 8) | raw[9]);
        return 0;
    }

    return -30;
}

/* Read one F3/E3 capabilities fragment at the requested string offset. */
static int ddc_caps_chunk(uint16_t offset, uint8_t *data, unsigned *data_len)
{
    uint8_t req[6];
    uint8_t raw[DDC_CAPS_RAW_READ];
    uint32_t status;
    unsigned payload_len, total, i;
    uint8_t x;
    int rc;
    unsigned attempt;

    if (!data || !data_len)
        return -20;

    *data_len = 0;

    req[0] = 0x51;
    req[1] = 0x83;
    req[2] = DDC_CAPS_REQUEST;
    req[3] = (uint8_t)(offset >> 8);
    req[4] = (uint8_t)offset;
    req[5] = ddc_checksum(req, 5);

    for (attempt = 0; attempt < 3U; ++attempt) {
        rc = bsc_write_packet(req, sizeof(req), &status);
        if (rc != 0) {
            Delay(3);
            continue;
        }

        /* DDC/CI requires >=40 ms before reading a capabilities reply. */
        Delay(3);  /* 50-60 ms at 60/50 Hz */

        rc = bsc_read_packet(raw, sizeof(raw), &status);
        if (rc != 0) {
            Delay(3);
            continue;
        }

        /* 0x51/0xff are commonly observed DDC/CI "busy/no reply yet" bytes. */
        if (raw[0] == 0x51U || raw[0] == 0xffU) {
            Delay(3);
            continue;
        }

        if (raw[0] != DDC_CI_ADDR_WRITE || !(raw[1] & 0x80U)) {
            Delay(3);
            continue;
        }

        payload_len = raw[1] & 0x7fU;
        if (payload_len < 3U || payload_len > DDC_CAPS_PAYLOAD_MAX) {
            Delay(3);
            continue;
        }

        total = payload_len + 3U; /* source + length + payload + checksum */
        if (total > sizeof(raw)) {
            Delay(3);
            continue;
        }

        x = DDC_CI_REPLY_XOR;
        for (i = 0; i < total; ++i)
            x ^= raw[i];
        if (x != 0U) {
            Delay(3);
            continue;
        }

        if (raw[2] != DDC_CAPS_REPLY ||
            raw[3] != (uint8_t)(offset >> 8) ||
            raw[4] != (uint8_t)offset) {
            Delay(3);
            continue;
        }

        *data_len = payload_len - 3U;
        for (i = 0; i < *data_len; ++i)
            data[i] = raw[5U + i];

        return 0;
    }

    return -30;
}

static int ddc_read_capabilities(char *out, unsigned out_size)
{
    uint8_t chunk[DDC_CAPS_DATA_MAX];
    unsigned chunk_len;
    unsigned used = 0;
    uint16_t offset = 0;
    unsigned i;
    int rc;

    if (!out || out_size < 2U)
        return -20;

    for (;;) {
        rc = ddc_caps_chunk(offset, chunk, &chunk_len);
        if (rc != 0)
            return rc;

        if (chunk_len == 0U)
            break;

        for (i = 0; i < chunk_len; ++i) {
            if (chunk[i] == 0U) {
                out[used] = '\0';
                return (int)used;
            }
            if (used + 1U >= out_size)
                return -31;
            out[used++] = (char)chunk[i];
        }

        offset = (uint16_t)(offset + chunk_len);

        /* Standard says wait >=50 ms before the next capabilities request. */
        Delay(3);
    }

    out[used] = '\0';
    return (int)used;
}

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int str_eq_ci(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b))
            return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static unsigned hex_val(int c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

static int starts_ci(const char *p, const char *word)
{
    while (*word) {
        if (!*p || ascii_tolower((unsigned char)*p) !=
                   ascii_tolower((unsigned char)*word))
            return 0;
        ++p; ++word;
    }
    return 1;
}

static const char *input_name(unsigned v)
{
    switch (v) {
        case 0x01: return "VGA-1";
        case 0x02: return "VGA-2";
        case 0x03: return "DVI-1";
        case 0x04: return "DVI-2";
        case 0x05: return "Composite-1";
        case 0x06: return "Composite-2";
        case 0x07: return "S-Video-1";
        case 0x08: return "S-Video-2";
        case 0x09: return "Tuner-1";
        case 0x0a: return "Tuner-2";
        case 0x0b: return "Tuner-3";
        case 0x0c: return "Component-1";
        case 0x0d: return "Component-2";
        case 0x0e: return "Component-3";
        case 0x0f: return "DisplayPort-1";
        case 0x10: return "DisplayPort-2";
        case 0x11: return "HDMI-1";
        case 0x12: return "HDMI-2";
        default:   return "vendor/unknown";
    }
}

static void print_value(unsigned v)
{
    printf("  0x%02lx  %3lu  %s\n",
           (unsigned long)v, (unsigned long)v, input_name(v));
}

/* Print one run of hex digits from inside 60(...). */
static unsigned print_hex_run(const char *p, unsigned n)
{
    unsigned count = 0;
    unsigned i = 0;

    if (n == 0U) return 0;

    if (n <= 2U) {
        unsigned v = 0;
        while (i < n) v = (v << 4) | hex_val((unsigned char)p[i++]);
        print_value(v);
        return 1;
    }

    /* Some monitors omit spaces entirely: e.g. 60(010F1112). */
    while (i + 1U < n) {
        unsigned v = (hex_val((unsigned char)p[i]) << 4) |
                     hex_val((unsigned char)p[i + 1U]);
        print_value(v);
        ++count;
        i += 2U;
    }

    /* Be permissive with a trailing one-digit value. */
    if (i < n) {
        print_value(hex_val((unsigned char)p[i]));
        ++count;
    }

    return count;
}

/* Find vcp(...), then feature 60(...), handling nested value parentheses. */
static int list_inputs_from_caps(const char *caps)
{
    const char *p = caps;
    const char *vcp = 0;
    const char *end;
    int depth;

    while (*p) {
        if (starts_ci(p, "vcp(")) {
            vcp = p + 4;
            break;
        }
        ++p;
    }

    if (!vcp) {
        puts("Capabilities string has no vcp(...) section.");
        return 0;
    }

    depth = 1;
    end = vcp;
    while (*end && depth) {
        if (*end == '(') ++depth;
        else if (*end == ')') --depth;
        ++end;
    }
    if (depth != 0) {
        puts("Malformed capabilities string (unterminated vcp section).");
        return 0;
    }

    p = vcp;
    while (p < end - 1) {
        unsigned code;
        unsigned digits;
        const char *q;

        while (p < end - 1 && !is_hex((unsigned char)*p)) ++p;
        if (p >= end - 1) break;

        q = p;
        digits = 0;
        while (q < end - 1 && is_hex((unsigned char)*q) && digits < 2U) {
            ++q; ++digits;
        }
        if (!digits) { ++p; continue; }

        code = hex_val((unsigned char)p[0]);
        if (digits == 2U)
            code = (code << 4) | hex_val((unsigned char)p[1]);
        p = q;

        while (p < end - 1 && (unsigned char)*p <= ' ') ++p;

        if (code == VCP_INPUT_SOURCE) {
            unsigned count = 0;

            if (p >= end - 1 || *p != '(') {
                puts("Monitor advertises VCP 0x60 (Input Source), but does not list values.");
                return 1;
            }

            ++p;
            puts("Input sources advertised by monitor:");
            while (p < end - 1 && *p != ')') {
                const char *r;
                unsigned n;

                while (p < end - 1 && !is_hex((unsigned char)*p) && *p != ')') ++p;
                if (p >= end - 1 || *p == ')') break;

                r = p;
                while (r < end - 1 && is_hex((unsigned char)*r)) ++r;
                n = (unsigned)(r - p);
                count += print_hex_run(p, n);
                p = r;
            }

            if (!count)
                puts("  (no enumerated values reported)");

            return 1;
        }

        /* Skip a value list belonging to some other VCP code. */
        if (p < end - 1 && *p == '(') {
            int sub = 1;
            ++p;
            while (p < end - 1 && sub) {
                if (*p == '(') ++sub;
                else if (*p == ')') --sub;
                ++p;
            }
        }
    }

    puts("Monitor capabilities do not advertise VCP 0x60 (Input Source).");
    return 0;
}

static int do_list(void)
{
    int n;

    puts("Reading DDC/CI capabilities...");
    n = ddc_read_capabilities(g_caps, sizeof(g_caps));
    if (n < 0) {
        printf("Capabilities read failed (rc=%ld).\n", (long)n);
        puts("The monitor may still support input switching; capabilities replies are optional/unreliable.");
        return 20;
    }

    printf("Capabilities: %lu bytes\n", (unsigned long)n);
    list_inputs_from_caps(g_caps);
    puts("Note: capabilities are advisory; some monitors report incomplete or incorrect values.");
    return 0;
}

static void usage(void)
{
    puts("DDCSwitch 0.5 - Emu68 native HDMI DDC/CI POC");
    puts("Usage: DDCSwitch <input|LIST|CURRENT|SWITCH a b>");
    puts("Examples: DDCSwitch 16");
    puts("          DDCSwitch LIST");
    puts("          DDCSwitch CURRENT");
    puts("          DDCSwitch SWITCH 16 17");
}

static int parse_u16(const char *s, uint16_t *out)
{
    unsigned long v = 0;
    unsigned base = 10;
    unsigned digits = 0;

    if (!s || !out) return 0;

    while (*s && (unsigned char)*s <= ' ') ++s;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    while (*s) {
        unsigned c = (unsigned char)*s;
        unsigned d;

        if (c <= ' ') {
            do { ++s; } while (*s && (unsigned char)*s <= ' ');
            if (*s != '\0' || digits == 0) return 0;
            *out = (uint16_t)v;
            return 1;
        }

        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;

        if (d >= base) return 0;
        if (v > (0xffffUL - d) / base) return 0;

        v = v * base + d;
        ++digits;
        ++s;
    }

    if (!digits) return 0;
    *out = (uint16_t)v;
    return 1;
}

int main(void)
{
    LONG opts[3] = { 0, 0, 0 };
    struct RDArgs *rda;
    const char *action, *arg1, *arg2;
    uint16_t input16, a, b, cur, maxv;
    uint8_t type = 0;
    unsigned long input;
    uint32_t status = 0;
    int rc;

    (void)ver;

    rda = ReadArgs("ACTION/A,VALUE1,VALUE2", opts, NULL);
    if (!rda) {
        usage();
        PrintFault(IoErr(), (STRPTR)"DDCSwitch");
        return 10;
    }

    action = (const char *)(uintptr_t)(ULONG)opts[0];
    arg1   = (const char *)(uintptr_t)(ULONG)opts[1];
    arg2   = (const char *)(uintptr_t)(ULONG)opts[2];

    if (action && str_eq_ci(action, "LIST")) {
        FreeArgs(rda);
        return do_list();
    }

    if (action && (str_eq_ci(action, "CURRENT") || str_eq_ci(action, "GET"))) {
        FreeArgs(rda);
        rc = ddc_get_vcp(VCP_INPUT_SOURCE, &cur, &maxv, &type);
        if (rc != 0) {
            printf("Get VCP 0x60 failed (rc=%ld).\n", (long)rc);
            return 20;
        }
        printf("Current input: 0x%02lx (%lu) %s\n",
               (unsigned long)cur, (unsigned long)cur, input_name(cur));
        return 0;
    }

    if (action && str_eq_ci(action, "SWITCH")) {
        if (!arg1 || !arg2 || !parse_u16(arg1, &a) || !parse_u16(arg2, &b)) {
            puts("Usage: DDCSwitch SWITCH <input1> <input2>");
            FreeArgs(rda);
            return 10;
        }
        FreeArgs(rda);

        rc = ddc_get_vcp(VCP_INPUT_SOURCE, &cur, &maxv, &type);
        if (rc != 0) {
            printf("Get VCP 0x60 failed (rc=%ld).\n", (long)rc);
            return 20;
        }

        printf("Current input: 0x%02lx (%lu) %s\n",
               (unsigned long)cur, (unsigned long)cur, input_name(cur));

        if (cur == a)
            input16 = b;
        else if (cur == b)
            input16 = a;
        else {
            printf("Current input is neither %lu nor %lu; no change.\n",
                   (unsigned long)a, (unsigned long)b);
            return 5;
        }

        printf("Switching to: 0x%02lx (%lu) %s\n",
               (unsigned long)input16, (unsigned long)input16,
               input_name(input16));

        rc = ddc_set_vcp(VCP_INPUT_SOURCE, input16, &status);
        if (rc == 0)
            return 0;

        if (rc == -10)
            printf("BSC2 busy (TA=1), untouched. status=0x%08lx\n",
                   (unsigned long)status);
        else if (rc == -3)
            printf("BSC2 clock-stretch timeout (CLKT). status=0x%08lx\n",
                   (unsigned long)status);
        else if (rc == -2)
            printf("BSC2 NACK/error (ERR). status=0x%08lx\n",
                   (unsigned long)status);
        else
            printf("BSC2 software timeout waiting for DONE. status=0x%08lx\n",
                   (unsigned long)status);
        return 20;
    }

    /* Legacy/direct form: DDCSwitch 16 */
    if (!action || arg1 || arg2 || !parse_u16(action, &input16)) {
        printf("Invalid input/action: [%s]\n", action ? action : "(null)");
        usage();
        FreeArgs(rda);
        return 10;
    }

    input = (unsigned long)input16;
    FreeArgs(rda);

    printf("DDC/CI: VCP 0x60 <- 0x%04lx (%lu)\n", input, input);

    rc = ddc_set_vcp(VCP_INPUT_SOURCE, input16, &status);
    if (rc == 0) {
        printf("BSC2 DONE (status=0x%08lx). Command sent.\n",
               (unsigned long)status);
        return 0;
    }

    if (rc == -10)
        printf("BSC2 busy (TA=1), untouched. status=0x%08lx\n",
               (unsigned long)status);
    else if (rc == -3)
        printf("BSC2 clock-stretch timeout (CLKT). status=0x%08lx\n",
               (unsigned long)status);
    else if (rc == -2)
        printf("BSC2 NACK/error (ERR). status=0x%08lx\n",
               (unsigned long)status);
    else
        printf("BSC2 software timeout waiting for DONE. status=0x%08lx\n",
               (unsigned long)status);

    return 20;
}
