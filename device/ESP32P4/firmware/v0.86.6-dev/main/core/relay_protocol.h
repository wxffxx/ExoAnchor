// SPDX-License-Identifier: MIT
#pragma once

/*
 * ExoAnchor Relay Protocol (EXAR/1) framing.
 *
 * This is an original, project-specific protocol. It is deliberately not
 * wire-compatible with FRP or any other reverse-proxy implementation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_RELAY_PROTOCOL_VERSION 1U
#define SI_RELAY_HEADER_SIZE 20U
#define SI_RELAY_MAX_PAYLOAD_SIZE (16U * 1024U)
#define SI_RELAY_MAX_DEVICE_ID_SIZE 63U
#define SI_RELAY_NONCE_SIZE 32U
#define SI_RELAY_AUTH_TAG_SIZE 32U
#define SI_RELAY_SESSION_ID_SIZE 16U
#define SI_RELAY_READY_PAYLOAD_SIZE 24U
#define SI_RELAY_MAX_WINDOW_SIZE (4U * 1024U * 1024U)
#define SI_RELAY_AUTH_CONTEXT "EXAR-AUTH-V1"
#define SI_RELAY_AUTH_CONTEXT_SIZE (sizeof(SI_RELAY_AUTH_CONTEXT) - 1U)
#define SI_RELAY_AUTH_INPUT_MAX_SIZE                                      \
    (SI_RELAY_AUTH_CONTEXT_SIZE + 1U + 37U +                              \
     SI_RELAY_MAX_DEVICE_ID_SIZE + SI_RELAY_NONCE_SIZE)

#define SI_RELAY_CAPABILITY_WEB_HTTP (1UL << 0U)
#define SI_RELAY_CAPABILITY_VIDEO_HTTP (1UL << 1U)
#define SI_RELAY_KNOWN_CAPABILITIES                                      \
    (SI_RELAY_CAPABILITY_WEB_HTTP | SI_RELAY_CAPABILITY_VIDEO_HTTP)

#define SI_RELAY_FLAG_FIN 0x0001U

typedef enum {
    SI_RELAY_FRAME_HELLO = 0x01,
    SI_RELAY_FRAME_CHALLENGE = 0x02,
    SI_RELAY_FRAME_AUTH = 0x03,
    SI_RELAY_FRAME_READY = 0x04,
    SI_RELAY_FRAME_OPEN = 0x10,
    SI_RELAY_FRAME_OPEN_OK = 0x11,
    SI_RELAY_FRAME_DATA = 0x12,
    SI_RELAY_FRAME_WINDOW = 0x13,
    SI_RELAY_FRAME_CLOSE = 0x14,
    SI_RELAY_FRAME_PING = 0x20,
    SI_RELAY_FRAME_PONG = 0x21,
    SI_RELAY_FRAME_ERROR = 0x22,
} si_relay_frame_type_t;

typedef enum {
    SI_RELAY_SERVICE_WEB_HTTP = 1,
    SI_RELAY_SERVICE_VIDEO_HTTP = 2,
} si_relay_service_t;

typedef enum {
    SI_RELAY_RESULT_OK = 0,
    SI_RELAY_RESULT_NEED_MORE = 1,
    SI_RELAY_RESULT_FRAME_READY = 2,
    SI_RELAY_RESULT_INVALID_ARGUMENT = -1,
    SI_RELAY_RESULT_BAD_MAGIC = -2,
    SI_RELAY_RESULT_UNSUPPORTED_VERSION = -3,
    SI_RELAY_RESULT_UNKNOWN_TYPE = -4,
    SI_RELAY_RESULT_INVALID_FLAGS = -5,
    SI_RELAY_RESULT_INVALID_STREAM = -6,
    SI_RELAY_RESULT_PAYLOAD_TOO_LARGE = -7,
    SI_RELAY_RESULT_INVALID_PAYLOAD = -8,
    SI_RELAY_RESULT_BUFFER_TOO_SMALL = -9,
} si_relay_result_t;

typedef struct {
    uint8_t version;
    si_relay_frame_type_t type;
    uint16_t flags;
    uint32_t stream_id;
    uint32_t sequence;
    uint32_t payload_size;
} si_relay_frame_header_t;

/*
 * Streaming decoder state. Payload storage stays caller-owned so the decoder
 * itself remains small and the firmware can place large buffers in PSRAM.
 */
typedef struct {
    uint8_t header_bytes[SI_RELAY_HEADER_SIZE];
    size_t header_used;
    si_relay_frame_header_t current;
    size_t payload_used;
    uint8_t *payload_storage;
    size_t payload_capacity;
    bool header_ready;
} si_relay_decoder_t;

void si_relay_decoder_init(si_relay_decoder_t *decoder);

si_relay_result_t si_relay_encode_header(
    const si_relay_frame_header_t *header,
    uint8_t output[SI_RELAY_HEADER_SIZE]);

si_relay_result_t si_relay_decode_header(
    const uint8_t input[SI_RELAY_HEADER_SIZE],
    si_relay_frame_header_t *header_out);

si_relay_result_t si_relay_validate_payload(
    const si_relay_frame_header_t *header, const uint8_t *payload);

/*
 * Consumes at most one complete frame. The same payload_storage pointer must
 * be supplied until that frame completes. Repeat with the unconsumed suffix
 * when FRAME_READY is returned and the network read contained another frame.
 */
si_relay_result_t si_relay_decoder_consume(
    si_relay_decoder_t *decoder, const uint8_t *input, size_t input_size,
    uint8_t *payload_storage, size_t payload_capacity, size_t *consumed_out,
    si_relay_frame_header_t *header_out);

/*
 * Produces the canonical bytes passed to HMAC-SHA256. The crypto operation is
 * intentionally supplied by the platform (mbedTLS in firmware).
 */
si_relay_result_t si_relay_build_auth_input(
    const uint8_t *hello_payload, size_t hello_size,
    const uint8_t challenge[SI_RELAY_NONCE_SIZE], uint8_t *output,
    size_t output_capacity, size_t *output_size);

const char *si_relay_result_name(si_relay_result_t result);
