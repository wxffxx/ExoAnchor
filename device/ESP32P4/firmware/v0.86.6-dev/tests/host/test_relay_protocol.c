// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "relay_protocol.h"

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static size_t make_hello(uint8_t *payload, const char *device_id)
{
    const size_t id_size = strlen(device_id);
    write_u32_be(payload, SI_RELAY_CAPABILITY_WEB_HTTP |
                              SI_RELAY_CAPABILITY_VIDEO_HTTP);
    for (size_t i = 0; i < SI_RELAY_NONCE_SIZE; ++i) {
        payload[4U + i] = (uint8_t)(0xa0U + i);
    }
    payload[36] = (uint8_t)id_size;
    memcpy(&payload[37], device_id, id_size);
    return 37U + id_size;
}

static void test_canonical_header(void)
{
    const si_relay_frame_header_t header = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_DATA,
        .flags = SI_RELAY_FLAG_FIN,
        .stream_id = 0x01020304U,
        .sequence = 0x10203040U,
        .payload_size = 3U,
    };
    const uint8_t expected[SI_RELAY_HEADER_SIZE] = {
        'E', 'X', 'A', 'R', 1, 0x12, 0, 1,
        1,   2,   3,   4,   0x10, 0x20, 0x30, 0x40,
        0,   0,   0,   3,
    };
    uint8_t encoded[SI_RELAY_HEADER_SIZE];
    assert(si_relay_encode_header(&header, encoded) == SI_RELAY_RESULT_OK);
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    si_relay_frame_header_t decoded = {0};
    assert(si_relay_decode_header(encoded, &decoded) == SI_RELAY_RESULT_OK);
    assert(decoded.version == header.version);
    assert(decoded.type == header.type);
    assert(decoded.flags == header.flags);
    assert(decoded.stream_id == header.stream_id);
    assert(decoded.sequence == header.sequence);
    assert(decoded.payload_size == header.payload_size);
}

static void test_header_rejections(void)
{
    si_relay_frame_header_t header = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_PING,
        .payload_size = 8U,
    };
    uint8_t encoded[SI_RELAY_HEADER_SIZE];
    assert(si_relay_encode_header(&header, encoded) == SI_RELAY_RESULT_OK);

    encoded[0] = 'F';
    assert(si_relay_decode_header(encoded, &header) == SI_RELAY_RESULT_BAD_MAGIC);
    encoded[0] = 'E';
    encoded[4] = 2U;
    assert(si_relay_decode_header(encoded, &header) ==
           SI_RELAY_RESULT_UNSUPPORTED_VERSION);
    encoded[4] = SI_RELAY_PROTOCOL_VERSION;
    encoded[5] = 0xffU;
    assert(si_relay_decode_header(encoded, &header) ==
           SI_RELAY_RESULT_UNKNOWN_TYPE);

    header = (si_relay_frame_header_t){
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_DATA,
        .stream_id = 0U,
        .payload_size = 1U,
    };
    assert(si_relay_encode_header(&header, encoded) ==
           SI_RELAY_RESULT_INVALID_STREAM);
    header.stream_id = 7U;
    header.flags = 0x8000U;
    assert(si_relay_encode_header(&header, encoded) ==
           SI_RELAY_RESULT_INVALID_FLAGS);
    header.flags = 0U;
    header.payload_size = SI_RELAY_MAX_PAYLOAD_SIZE + 1U;
    assert(si_relay_encode_header(&header, encoded) ==
           SI_RELAY_RESULT_PAYLOAD_TOO_LARGE);
}

static void test_fragmented_hello(void)
{
    uint8_t hello[100];
    const size_t hello_size = make_hello(hello, "ea-p4-123456");
    const si_relay_frame_header_t header = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_HELLO,
        .sequence = 9U,
        .payload_size = (uint32_t)hello_size,
    };
    uint8_t wire[SI_RELAY_HEADER_SIZE + sizeof(hello)];
    assert(si_relay_encode_header(&header, wire) == SI_RELAY_RESULT_OK);
    memcpy(&wire[SI_RELAY_HEADER_SIZE], hello, hello_size);

    si_relay_decoder_t decoder;
    si_relay_decoder_init(&decoder);
    uint8_t payload[100] = {0};
    si_relay_frame_header_t decoded = {0};
    const size_t wire_size = SI_RELAY_HEADER_SIZE + hello_size;
    for (size_t i = 0; i < wire_size; ++i) {
        size_t consumed = 99U;
        const si_relay_result_t result = si_relay_decoder_consume(
            &decoder, &wire[i], 1U, payload, sizeof(payload), &consumed,
            &decoded);
        assert(consumed == 1U);
        assert(result == (i + 1U == wire_size
                              ? SI_RELAY_RESULT_FRAME_READY
                              : SI_RELAY_RESULT_NEED_MORE));
    }
    assert(decoded.type == SI_RELAY_FRAME_HELLO);
    assert(decoded.sequence == 9U);
    assert(decoded.payload_size == hello_size);
    assert(memcmp(payload, hello, hello_size) == 0);
}

static void test_coalesced_frames(void)
{
    uint8_t wire[2U * (SI_RELAY_HEADER_SIZE + 8U)] = {0};
    const si_relay_frame_header_t ping = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_PING,
        .sequence = 1U,
        .payload_size = 8U,
    };
    const si_relay_frame_header_t pong = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_PONG,
        .sequence = 2U,
        .payload_size = 8U,
    };
    assert(si_relay_encode_header(&ping, wire) == SI_RELAY_RESULT_OK);
    memset(&wire[SI_RELAY_HEADER_SIZE], 0x11, 8U);
    const size_t second = SI_RELAY_HEADER_SIZE + 8U;
    assert(si_relay_encode_header(&pong, &wire[second]) == SI_RELAY_RESULT_OK);
    memset(&wire[second + SI_RELAY_HEADER_SIZE], 0x22, 8U);

    si_relay_decoder_t decoder;
    si_relay_decoder_init(&decoder);
    uint8_t payload[8];
    si_relay_frame_header_t decoded;
    size_t consumed = 0U;
    assert(si_relay_decoder_consume(
               &decoder, wire, sizeof(wire), payload, sizeof(payload),
               &consumed, &decoded) == SI_RELAY_RESULT_FRAME_READY);
    assert(consumed == second);
    assert(decoded.type == SI_RELAY_FRAME_PING);
    assert(payload[0] == 0x11U);

    size_t second_consumed = 0U;
    assert(si_relay_decoder_consume(
               &decoder, &wire[consumed], sizeof(wire) - consumed, payload,
               sizeof(payload), &second_consumed,
               &decoded) == SI_RELAY_RESULT_FRAME_READY);
    assert(second_consumed == second);
    assert(decoded.type == SI_RELAY_FRAME_PONG);
    assert(payload[0] == 0x22U);
}

static void test_payload_validation_and_capacity(void)
{
    uint8_t open_payload[8] = {SI_RELAY_SERVICE_WEB_HTTP, 0, 0, 0,
                               0, 1, 0, 0};
    si_relay_frame_header_t open = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_OPEN,
        .stream_id = 1U,
        .payload_size = sizeof(open_payload),
    };
    assert(si_relay_validate_payload(&open, open_payload) ==
           SI_RELAY_RESULT_OK);
    open_payload[0] = 99U;
    assert(si_relay_validate_payload(&open, open_payload) ==
           SI_RELAY_RESULT_INVALID_PAYLOAD);

    uint8_t hello[100];
    const size_t hello_size = make_hello(hello, "ea-p4-123456");
    hello[36]--;
    si_relay_frame_header_t hello_header = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_HELLO,
        .payload_size = (uint32_t)hello_size,
    };
    assert(si_relay_validate_payload(&hello_header, hello) ==
           SI_RELAY_RESULT_INVALID_PAYLOAD);
    hello[36]++;
    hello[37] = 'E';
    assert(si_relay_validate_payload(&hello_header, hello) ==
           SI_RELAY_RESULT_INVALID_PAYLOAD);
    hello[37] = 'e';

    uint8_t wire[SI_RELAY_HEADER_SIZE];
    const si_relay_frame_header_t data = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_DATA,
        .stream_id = 3U,
        .payload_size = 64U,
    };
    assert(si_relay_encode_header(&data, wire) == SI_RELAY_RESULT_OK);
    si_relay_decoder_t decoder;
    si_relay_decoder_init(&decoder);
    uint8_t payload[8];
    si_relay_frame_header_t decoded;
    size_t consumed = 0U;
    assert(si_relay_decoder_consume(
               &decoder, wire, sizeof(wire), payload, sizeof(payload),
               &consumed, &decoded) == SI_RELAY_RESULT_BUFFER_TOO_SMALL);
    assert(consumed == SI_RELAY_HEADER_SIZE);
}

static void test_decoder_rejects_storage_switch(void)
{
    const si_relay_frame_header_t ping = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_PING,
        .payload_size = 8U,
    };
    uint8_t wire[SI_RELAY_HEADER_SIZE + 8U] = {0};
    assert(si_relay_encode_header(&ping, wire) == SI_RELAY_RESULT_OK);
    memset(&wire[SI_RELAY_HEADER_SIZE], 0x33, 8U);

    si_relay_decoder_t decoder;
    si_relay_decoder_init(&decoder);
    uint8_t first_storage[8] = {0};
    uint8_t second_storage[8] = {0};
    si_relay_frame_header_t decoded;
    size_t consumed = 0U;
    assert(si_relay_decoder_consume(
               &decoder, wire, SI_RELAY_HEADER_SIZE + 1U, first_storage,
               sizeof(first_storage), &consumed,
               &decoded) == SI_RELAY_RESULT_NEED_MORE);
    assert(consumed == SI_RELAY_HEADER_SIZE + 1U);

    consumed = 0U;
    assert(si_relay_decoder_consume(
               &decoder, &wire[SI_RELAY_HEADER_SIZE + 1U], 7U,
               second_storage, sizeof(second_storage), &consumed,
               &decoded) == SI_RELAY_RESULT_INVALID_ARGUMENT);
    assert(consumed == 0U);
}

static void test_auth_transcript(void)
{
    uint8_t hello[100];
    const size_t hello_size = make_hello(hello, "ea-p4-123456");
    uint8_t challenge[SI_RELAY_NONCE_SIZE];
    memset(challenge, 0x5a, sizeof(challenge));
    uint8_t output[SI_RELAY_AUTH_INPUT_MAX_SIZE];
    size_t output_size = 0U;
    assert(si_relay_build_auth_input(hello, hello_size, challenge, output,
                                     sizeof(output), &output_size) ==
           SI_RELAY_RESULT_OK);
    assert(output_size == SI_RELAY_AUTH_CONTEXT_SIZE + 1U + hello_size +
                              SI_RELAY_NONCE_SIZE);
    assert(memcmp(output, SI_RELAY_AUTH_CONTEXT,
                  SI_RELAY_AUTH_CONTEXT_SIZE) == 0);
    assert(output[SI_RELAY_AUTH_CONTEXT_SIZE] == 0U);
    assert(memcmp(&output[SI_RELAY_AUTH_CONTEXT_SIZE + 1U], hello,
                  hello_size) == 0);
    assert(memcmp(&output[output_size - SI_RELAY_NONCE_SIZE], challenge,
                  SI_RELAY_NONCE_SIZE) == 0);

    assert(si_relay_build_auth_input(hello, hello_size, challenge, output,
                                     output_size - 1U, &output_size) ==
           SI_RELAY_RESULT_BUFFER_TOO_SMALL);
}

int main(void)
{
    test_canonical_header();
    test_header_rejections();
    test_fragmented_hello();
    test_coalesced_frames();
    test_payload_validation_and_capacity();
    test_decoder_rejects_storage_switch();
    test_auth_transcript();
    assert(strcmp(si_relay_result_name(SI_RELAY_RESULT_BAD_MAGIC),
                  "bad_magic") == 0);
    puts("relay protocol tests: PASS");
    return 0;
}
