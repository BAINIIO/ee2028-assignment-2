#include "nfc_ndef.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OUTPUT_BYTES = 256, GUARD_BYTES = 32, MAX_SUFFIX = 240 };

typedef struct {
    uint8_t before[GUARD_BYTES];
    uint8_t bytes[OUTPUT_BYTES];
    uint8_t after[GUARD_BYTES];
} GuardedOutput;

static unsigned checks;

#define CHECK(condition) do { assert(condition); ++checks; } while (0)

/* Exercise the caller's capacity as well as the encoder's physical bounds. */
static size_t build_guarded(const char *url, size_t capacity,
                            uint8_t result[OUTPUT_BYTES])
{
    GuardedOutput output;
    memset(&output, 0xa5, sizeof output);
    CHECK(capacity <= sizeof output.bytes);
    size_t length = NfcNdef_BuildUri(url, output.bytes, capacity);
    CHECK(length <= capacity);
    for (size_t i = 0; i < GUARD_BYTES; ++i) {
        CHECK(output.before[i] == 0xa5);
        CHECK(output.after[i] == 0xa5);
    }
    /* Failure must leave the destination alone; success owns only its result. */
    for (size_t i = length; i < sizeof output.bytes; ++i)
        CHECK(output.bytes[i] == 0xa5);
    if (result != NULL) memcpy(result, output.bytes, sizeof output.bytes);
    return length;
}

/* Decode independently using the NFC Forum Type 5 / URI record wire format. */
static void check_uri_record(const uint8_t *bytes, size_t length, const char *url)
{
    static const uint8_t capability_container[] = {0xe1, 0x40, 0x40, 0x00};
    CHECK(length >= 13u);
    CHECK(memcmp(bytes, capability_container, sizeof capability_container) == 0);
    CHECK(bytes[4] == 0x03);       /* NDEF Message TLV. */
    size_t record_length = bytes[5];
    CHECK(record_length != 0u && record_length != 0xffu);
    CHECK(record_length + 7u == length); /* CC, TLV header, record, terminator. */
    CHECK(bytes[6] == 0xd1);       /* MB | ME | SR, well-known type. */
    CHECK(bytes[7] == 1u);         /* One-byte record type. */
    size_t payload_length = bytes[8];
    CHECK(payload_length >= 2u);
    CHECK(record_length == 4u + payload_length);
    CHECK(bytes[9] == 0x55);       /* NFC well-known URI type "U". */
    const char *prefix = NULL;
    if (bytes[10] == 0x03) prefix = "http://";
    if (bytes[10] == 0x04) prefix = "https://";
    CHECK(prefix != NULL);
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = payload_length - 1u;
    CHECK(strlen(url) == prefix_length + suffix_length);
    CHECK(strncmp(url, prefix, prefix_length) == 0);
    CHECK(memcmp(bytes + 11, url + prefix_length, suffix_length) == 0);
    CHECK(bytes[length - 1u] == 0xfe); /* Type 5 TLV terminator, not URI data. */
}

static void test_golden_records(void)
{
    /* Literal vectors catch an internally consistent but nonstandard encoder. */
    static const uint8_t http_a[] = {
        0xe1, 0x40, 0x40, 0x00, 0x03, 0x06, 0xd1,
        0x01, 0x02, 0x55, 0x03, 0x61, 0xfe
    };
    static const uint8_t https_a[] = {
        0xe1, 0x40, 0x40, 0x00, 0x03, 0x06, 0xd1,
        0x01, 0x02, 0x55, 0x04, 0x61, 0xfe
    };
    uint8_t bytes[OUTPUT_BYTES];
    size_t length = build_guarded("http://a", sizeof http_a, bytes);
    CHECK(length == sizeof http_a);
    CHECK(memcmp(bytes, http_a, sizeof http_a) == 0);
    check_uri_record(bytes, length, "http://a");
    length = build_guarded("https://a", sizeof https_a, bytes);
    CHECK(length == sizeof https_a);
    CHECK(memcmp(bytes, https_a, sizeof https_a) == 0);
    check_uri_record(bytes, length, "https://a");
}

static void test_real_urls_and_capacity(void)
{
    static const char *const urls[] = {
        "http://192.168.1.20:8080/drone/GG01?mission=2",
        "https://garden.example/drone/GG01?mission=0&name=Last%20Hope",
        "http://localhost:8080/drone/GG01#status"
    };
    uint8_t bytes[OUTPUT_BYTES];
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; ++i) {
        char original[128];
        CHECK(strlen(urls[i]) < sizeof original);
        memcpy(original, urls[i], strlen(urls[i]) + 1u);
        size_t length = build_guarded(original, sizeof bytes, bytes);
        CHECK(length > 0u);
        check_uri_record(bytes, length, original);
        CHECK(strcmp(original, urls[i]) == 0);
        for (size_t capacity = 0; capacity <= length + 2u; ++capacity) {
            size_t written = build_guarded(original, capacity, bytes);
            if (capacity < length) CHECK(written == 0u);
            else {
                CHECK(written == length);
                check_uri_record(bytes, written, original);
            }
        }
    }
}

static void test_short_record_limits(void)
{
    char url[8 + MAX_SUFFIX + 2];
    uint8_t bytes[OUTPUT_BYTES];
    memcpy(url, "https://", 8);
    for (size_t suffix = 1; suffix <= MAX_SUFFIX; ++suffix) {
        memset(url + 8, 'a', suffix);
        if (suffix >= 2u) url[9] = '/';
        url[8u + suffix] = '\0';
        size_t expected = suffix + 12u;
        CHECK(build_guarded(url, expected - 1u, NULL) == 0u);
        size_t length = build_guarded(url, expected, bytes);
        CHECK(length == expected);
        check_uri_record(bytes, length, url);
    }
    /* The chosen bound stays in the short TLV / short NDEF record formats. */
    CHECK(bytes[5] == 245u);
    CHECK(bytes[8] == 241u);
    CHECK(bytes[251] == 0xfe);
    memset(url + 8, 'a', MAX_SUFFIX + 1u);
    url[9] = '/';
    url[8u + MAX_SUFFIX + 1u] = '\0';
    CHECK(build_guarded(url, sizeof bytes, NULL) == 0u);

    char oversized[1024];
    memset(oversized, 'a', sizeof oversized);
    memcpy(oversized, "http://a/", 9);
    oversized[sizeof oversized - 1u] = '\0';
    CHECK(build_guarded(oversized, sizeof bytes, NULL) == 0u);
}

static void test_invalid_input(void)
{
    static const char *const invalid[] = {
        "", "http://", "https://", "ftp://example.com", "mailto:a@example.com",
        "/drone/GG01", "example.com", "http:/example.com", "https:example.com",
        " http://example.com", "https://example.com/a b",
        "http://example.com/\t", "https://example.com/\r\n"
    };
    CHECK(NfcNdef_BuildUri("http://a", NULL, OUTPUT_BYTES) == 0u);
    CHECK(NfcNdef_BuildUri(NULL, NULL, 0) == 0u);
    CHECK(build_guarded(NULL, OUTPUT_BYTES, NULL) == 0u);
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        CHECK(build_guarded(invalid[i], OUTPUT_BYTES, NULL) == 0u);

    /* Every disallowed non-NUL byte must fail, with no partial header output. */
    for (unsigned value = 1; value <= 255u; ++value) {
        if (value > 0x20u && value < 0x7fu) continue;
        char url[] = "https://a/x";
        url[10] = (char)(uint8_t)value;
        CHECK(build_guarded(url, OUTPUT_BYTES, NULL) == 0u);
    }
}

int main(void)
{
    test_golden_records();
    test_real_urls_and_capacity();
    test_short_record_limits();
    test_invalid_input();
    printf("NFC NDEF tests passed (%u checks)\n", checks);
    return 0;
}
