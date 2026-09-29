// SPDX-License-Identifier: MIT

#include "relay_protocol.h"

#include <string.h>

static const uint8_t s_magic[4] = {'E', 'X', 'A', 'R'};

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static bool frame_type_known(si_relay_frame_type_t type)
{
    switch (type) {
    case SI_RELAY_FRAME_HELLO:
    case SI_RELAY_FRAME_CHALLENGE:
    case SI_RELAY_FRAME_AUTH:
    case SI_RELAY_FRAME_READY:
    case SI_RELAY_FRAME_OPEN:
    case SI_RELAY_FRAME_OPEN_OK:
    case SI_RELAY_FRAME_DATA:
    case SI_RELAY_FRAME_WINDOW:
    case SI_RELAY_FRAME_CLOSE:
    case SI_RELAY_FRAME_PING:
    case SI_RELAY_FRAME_PONG:
    case SI_RELAY_FRAME_ERROR:
        return true;
    default:
        return false;
    }
}

static bool frame_is_session_scoped(si_relay_frame_type_t type)
{
    switch (type) {
    case SI_RELAY_FRAME_HELLO:
    case SI_RELAY_FRAME_CHALLENGE:
    case SI_RELAY_FRAME_AUTH:
    case SI_RELAY_FRAME_READY:
    case SI_RELAY_FRAME_PING:
    case SI_RELAY_FRAME_PONG:
        return true;
    default:
        return false;
    }
}

static bool frame_is_stream_scoped(si_relay_frame_type_t type)
{
    switch (type) {
    case SI_RELAY_FRAME_OPEN:
    case SI_RELAY_FRAME_OPEN_OK:
    case SI_RELAY_FRAME_DATA:
    case SI_RELAY_FRAME_WINDOW:
    case SI_RELAY_FRAME_CLOSE:
        return true;
    default:
        return false;
    }
}

static bool bytes_have_nonzero(const uint8_t *bytes, size_t size)
{
    uint8_t combined = 0U;
    for (size_t i = 0; i < size; ++i) {
        combined |= bytes[i];
    }
    return combined != 0U;
}

static bool device_id_valid(const uint8_t *device_id, size_t size)
{
    if (!device_id || size == 0U || size > SI_RELAY_MAX_DEVICE_ID_SIZE) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const uint8_t value = device_id[i];
        const bool alpha = value >= 'a' && value <= 'z';
        const bool digit = value >= '0' && value <= '9';
        if (!alpha && !digit && value != '-') {
            return false;
        }
        if ((i == 0U || i + 1U == size) && !alpha && !digit) {
            return false;
        }
    }
    return true;
}

static bool payload_size_valid(const si_relay_frame_header_t *header)
{
    switch (header->type) {
    case SI_RELAY_FRAME_HELLO:
        return header->payload_size >= 38U &&
               header->payload_size <= 37U + SI_RELAY_MAX_DEVICE_ID_SIZE;
    case SI_RELAY_FRAME_CHALLENGE:
        return header->payload_size == SI_RELAY_NONCE_SIZE;
    case SI_RELAY_FRAME_AUTH:
        return header->payload_size == SI_RELAY_AUTH_TAG_SIZE;
    case SI_RELAY_FRAME_READY:
        return header->payload_size == SI_RELAY_READY_PAYLOAD_SIZE;
    case SI_RELAY_FRAME_OPEN:
        return header->payload_size == 8U;
    case SI_RELAY_FRAME_OPEN_OK:
    case SI_RELAY_FRAME_WINDOW:
        return header->payload_size == 4U;
    case SI_RELAY_FRAME_DATA:
        return header->payload_size > 0U ||
               (header->flags & SI_RELAY_FLAG_FIN) != 0U;
    case SI_RELAY_FRAME_CLOSE:
    case SI_RELAY_FRAME_ERROR:
        return header->payload_size >= 2U && header->payload_size <= 130U;
    case SI_RELAY_FRAME_PING:
    case SI_RELAY_FRAME_PONG:
        return header->payload_size == 8U;
    default:
        return false;
    }
}

static si_relay_result_t validate_header(
    const si_relay_frame_header_t *header)
{
    if (!header) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }
    if (header->version != SI_RELAY_PROTOCOL_VERSION) {
        return SI_RELAY_RESULT_UNSUPPORTED_VERSION;
    }
    if (!frame_type_known(header->type)) {
        return SI_RELAY_RESULT_UNKNOWN_TYPE;
    }
    if ((header->type == SI_RELAY_FRAME_DATA &&
         (header->flags & (uint16_t)~SI_RELAY_FLAG_FIN) != 0U) ||
        (header->type != SI_RELAY_FRAME_DATA && header->flags != 0U)) {
        return SI_RELAY_RESULT_INVALID_FLAGS;
    }
    if ((frame_is_session_scoped(header->type) && header->stream_id != 0U) ||
        (frame_is_stream_scoped(header->type) && header->stream_id == 0U)) {
        return SI_RELAY_RESULT_INVALID_STREAM;
    }
    if (header->payload_size > SI_RELAY_MAX_PAYLOAD_SIZE) {
        return SI_RELAY_RESULT_PAYLOAD_TOO_LARGE;
    }
    if (!payload_size_valid(header)) {
        return SI_RELAY_RESULT_INVALID_PAYLOAD;
    }
    return SI_RELAY_RESULT_OK;
}

void si_relay_decoder_init(si_relay_decoder_t *decoder)
{
    if (decoder) {
        memset(decoder, 0, sizeof(*decoder));
    }
}

si_relay_result_t si_relay_encode_header(
    const si_relay_frame_header_t *header,
    uint8_t output[SI_RELAY_HEADER_SIZE])
{
    if (!output) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }
    si_relay_result_t result = validate_header(header);
    if (result != SI_RELAY_RESULT_OK) {
        return result;
    }

    memcpy(output, s_magic, sizeof(s_magic));
    output[4] = header->version;
    output[5] = (uint8_t)header->type;
    write_u16_be(&output[6], header->flags);
    write_u32_be(&output[8], header->stream_id);
    write_u32_be(&output[12], header->sequence);
    write_u32_be(&output[16], header->payload_size);
    return SI_RELAY_RESULT_OK;
}

si_relay_result_t si_relay_decode_header(
    const uint8_t input[SI_RELAY_HEADER_SIZE],
    si_relay_frame_header_t *header_out)
{
    if (!input || !header_out) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }
    if (memcmp(input, s_magic, sizeof(s_magic)) != 0) {
        return SI_RELAY_RESULT_BAD_MAGIC;
    }

    si_relay_frame_header_t header = {
        .version = input[4],
        .type = (si_relay_frame_type_t)input[5],
        .flags = read_u16_be(&input[6]),
        .stream_id = read_u32_be(&input[8]),
        .sequence = read_u32_be(&input[12]),
        .payload_size = read_u32_be(&input[16]),
    };
    si_relay_result_t result = validate_header(&header);
    if (result == SI_RELAY_RESULT_OK) {
        *header_out = header;
    }
    return result;
}

si_relay_result_t si_relay_validate_payload(
    const si_relay_frame_header_t *header, const uint8_t *payload)
{
    si_relay_result_t result = validate_header(header);
    if (result != SI_RELAY_RESULT_OK) {
        return result;
    }
    if (header->payload_size > 0U && !payload) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }

    switch (header->type) {
    case SI_RELAY_FRAME_HELLO: {
        const uint32_t capabilities = read_u32_be(payload);
        const size_t device_id_size = payload[36];
        if ((capabilities & ~SI_RELAY_KNOWN_CAPABILITIES) != 0U ||
            (capabilities & SI_RELAY_CAPABILITY_WEB_HTTP) == 0U ||
            !bytes_have_nonzero(&payload[4], SI_RELAY_NONCE_SIZE) ||
            device_id_size == 0U ||
            device_id_size != header->payload_size - 37U ||
            !device_id_valid(&payload[37], device_id_size)) {
            return SI_RELAY_RESULT_INVALID_PAYLOAD;
        }
        break;
    }
    case SI_RELAY_FRAME_CHALLENGE:
        if (!bytes_have_nonzero(payload, SI_RELAY_NONCE_SIZE)) {
            return SI_RELAY_RESULT_INVALID_PAYLOAD;
        }
        break;
    case SI_RELAY_FRAME_READY: {
        const uint32_t heartbeat_ms = read_u32_be(&payload[16]);
        const uint16_t max_streams = read_u16_be(&payload[20]);
        if (!bytes_have_nonzero(payload, SI_RELAY_SESSION_ID_SIZE) ||
            heartbeat_ms < 5000U || heartbeat_ms > 120000U ||
            max_streams == 0U || max_streams > 32U ||
            read_u16_be(&payload[22]) != 0U) {
            return SI_RELAY_RESULT_INVALID_PAYLOAD;
        }
        break;
    }
    case SI_RELAY_FRAME_OPEN: {
        const uint8_t service = payload[0];
        const uint32_t receive_window = read_u32_be(&payload[4]);
        if ((service != SI_RELAY_SERVICE_WEB_HTTP &&
             service != SI_RELAY_SERVICE_VIDEO_HTTP) ||
            payload[1] != 0U || payload[2] != 0U || payload[3] != 0U ||
            receive_window == 0U ||
            receive_window > SI_RELAY_MAX_WINDOW_SIZE) {
            return SI_RELAY_RESULT_INVALID_PAYLOAD;
        }
        break;
    }
    case SI_RELAY_FRAME_OPEN_OK:
    case SI_RELAY_FRAME_WINDOW:
        if (read_u32_be(payload) == 0U ||
            read_u32_be(payload) > SI_RELAY_MAX_WINDOW_SIZE) {
            return SI_RELAY_RESULT_INVALID_PAYLOAD;
        }
        break;
    default:
        break;
    }
    return SI_RELAY_RESULT_OK;
}

si_relay_result_t si_relay_decoder_consume(
    si_relay_decoder_t *decoder, const uint8_t *input, size_t input_size,
    uint8_t *payload_storage, size_t payload_capacity, size_t *consumed_out,
    si_relay_frame_header_t *header_out)
{
    if (!decoder || (!input && input_size > 0U) || !consumed_out ||
        !header_out) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }
    *consumed_out = 0U;

    if (!decoder->header_ready) {
        const size_t missing = SI_RELAY_HEADER_SIZE - decoder->header_used;
        const size_t copied = input_size < missing ? input_size : missing;
        if (copied > 0U) {
            memcpy(&decoder->header_bytes[decoder->header_used], input, copied);
            decoder->header_used += copied;
            *consumed_out += copied;
        }
        if (decoder->header_used < SI_RELAY_HEADER_SIZE) {
            return SI_RELAY_RESULT_NEED_MORE;
        }

        si_relay_result_t result =
            si_relay_decode_header(decoder->header_bytes, &decoder->current);
        if (result != SI_RELAY_RESULT_OK) {
            si_relay_decoder_init(decoder);
            return result;
        }
        decoder->header_ready = true;
        const bool storage_too_small =
            decoder->current.payload_size > payload_capacity;
        if (storage_too_small ||
            (decoder->current.payload_size > 0U && !payload_storage)) {
            si_relay_decoder_init(decoder);
            return storage_too_small ? SI_RELAY_RESULT_BUFFER_TOO_SMALL
                                     : SI_RELAY_RESULT_INVALID_ARGUMENT;
        }
        decoder->payload_storage = payload_storage;
        decoder->payload_capacity = payload_capacity;
    } else if (decoder->payload_storage != payload_storage ||
               decoder->payload_capacity != payload_capacity) {
        si_relay_decoder_init(decoder);
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }

    const size_t input_offset = *consumed_out;
    const size_t available = input_size - input_offset;
    const size_t missing = decoder->current.payload_size - decoder->payload_used;
    const size_t copied = available < missing ? available : missing;
    if (copied > 0U) {
        memcpy(&decoder->payload_storage[decoder->payload_used],
               &input[input_offset], copied);
        decoder->payload_used += copied;
        *consumed_out += copied;
    }
    if (decoder->payload_used < decoder->current.payload_size) {
        return SI_RELAY_RESULT_NEED_MORE;
    }

    si_relay_result_t result =
        si_relay_validate_payload(&decoder->current, decoder->payload_storage);
    if (result != SI_RELAY_RESULT_OK) {
        si_relay_decoder_init(decoder);
        return result;
    }
    *header_out = decoder->current;
    si_relay_decoder_init(decoder);
    return SI_RELAY_RESULT_FRAME_READY;
}

si_relay_result_t si_relay_build_auth_input(
    const uint8_t *hello_payload, size_t hello_size,
    const uint8_t challenge[SI_RELAY_NONCE_SIZE], uint8_t *output,
    size_t output_capacity, size_t *output_size)
{
    if (!hello_payload || !challenge || !output || !output_size ||
        hello_size < 38U ||
        hello_size > 37U + SI_RELAY_MAX_DEVICE_ID_SIZE) {
        return SI_RELAY_RESULT_INVALID_ARGUMENT;
    }
    const si_relay_frame_header_t hello_header = {
        .version = SI_RELAY_PROTOCOL_VERSION,
        .type = SI_RELAY_FRAME_HELLO,
        .payload_size = (uint32_t)hello_size,
    };
    if (si_relay_validate_payload(&hello_header, hello_payload) !=
            SI_RELAY_RESULT_OK ||
        !bytes_have_nonzero(challenge, SI_RELAY_NONCE_SIZE)) {
        return SI_RELAY_RESULT_INVALID_PAYLOAD;
    }
    const size_t required =
        SI_RELAY_AUTH_CONTEXT_SIZE + 1U + hello_size + SI_RELAY_NONCE_SIZE;
    if (output_capacity < required) {
        return SI_RELAY_RESULT_BUFFER_TOO_SMALL;
    }

    memcpy(output, SI_RELAY_AUTH_CONTEXT, SI_RELAY_AUTH_CONTEXT_SIZE);
    output[SI_RELAY_AUTH_CONTEXT_SIZE] = 0U;
    memcpy(&output[SI_RELAY_AUTH_CONTEXT_SIZE + 1U], hello_payload,
           hello_size);
    memcpy(&output[SI_RELAY_AUTH_CONTEXT_SIZE + 1U + hello_size], challenge,
           SI_RELAY_NONCE_SIZE);
    *output_size = required;
    return SI_RELAY_RESULT_OK;
}

const char *si_relay_result_name(si_relay_result_t result)
{
    switch (result) {
    case SI_RELAY_RESULT_OK:
        return "ok";
    case SI_RELAY_RESULT_NEED_MORE:
        return "need_more";
    case SI_RELAY_RESULT_FRAME_READY:
        return "frame_ready";
    case SI_RELAY_RESULT_INVALID_ARGUMENT:
        return "invalid_argument";
    case SI_RELAY_RESULT_BAD_MAGIC:
        return "bad_magic";
    case SI_RELAY_RESULT_UNSUPPORTED_VERSION:
        return "unsupported_version";
    case SI_RELAY_RESULT_UNKNOWN_TYPE:
        return "unknown_type";
    case SI_RELAY_RESULT_INVALID_FLAGS:
        return "invalid_flags";
    case SI_RELAY_RESULT_INVALID_STREAM:
        return "invalid_stream";
    case SI_RELAY_RESULT_PAYLOAD_TOO_LARGE:
        return "payload_too_large";
    case SI_RELAY_RESULT_INVALID_PAYLOAD:
        return "invalid_payload";
    case SI_RELAY_RESULT_BUFFER_TOO_SMALL:
        return "buffer_too_small";
    default:
        return "unknown_result";
    }
}
