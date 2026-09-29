/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h> // For memcpy

#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-isoc";

static void isoc_complete_frame(uvc_stream_t *uvc_stream)
{
    bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

    UVC_ENTER_CRITICAL();
    uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL; // Stop writing more data to this frame

    const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame &&
                                     !uvc_stream->single_thread.skip_current_frame);
    UVC_EXIT_CRITICAL();

    if (invoke_fb_callback) {
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (return_frame && this_frame) {
        uvc_host_frame_return(uvc_stream, this_frame);
    }
}

static void isoc_emit_frame_event(uvc_stream_t *uvc_stream, enum uvc_host_dev_event event_type)
{
    uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
    if (stream_cb) {
        const uvc_host_stream_event_data_t event = {
            .type = event_type,
        };
        stream_cb(&event, uvc_stream->constant.cb_arg);
    }
}

static uvc_host_frame_t *isoc_get_next_frame(uvc_stream_t *uvc_stream)
{
    uvc_host_frame_t *frame = uvc_frame_get_empty(uvc_stream);
    if (!frame) {
        uvc_stream->single_thread.skip_current_frame = true;
        isoc_emit_frame_event(uvc_stream, UVC_HOST_FRAME_BUFFER_UNDERFLOW);
        return NULL;
    }

    UVC_ENTER_CRITICAL();
    uvc_stream->dynamic.current_frame = frame;
    UVC_EXIT_CRITICAL();
    return frame;
}

static bool isoc_add_fixed_size_frame_data(uvc_stream_t *uvc_stream,
                                           const uint8_t *payload_data,
                                           size_t payload_data_len)
{
    size_t offset = 0;
    while (offset < payload_data_len) {
        uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
        if (!current_frame) {
            current_frame = isoc_get_next_frame(uvc_stream);
            if (!current_frame) {
                return false;
            }
        }

        if (current_frame->data_len >= current_frame->data_buffer_len) {
            isoc_complete_frame(uvc_stream);
            continue;
        }

        const size_t space = current_frame->data_buffer_len - current_frame->data_len;
        const size_t remaining = payload_data_len - offset;
        const size_t chunk = remaining < space ? remaining : space;
        esp_err_t ret = uvc_frame_add_data(current_frame, payload_data + offset, chunk);
        if (ret != ESP_OK) {
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_emit_frame_event(uvc_stream, UVC_HOST_FRAME_BUFFER_OVERFLOW);
            return false;
        }

        offset += chunk;
        if (current_frame->data_len >= current_frame->data_buffer_len) {
            isoc_complete_frame(uvc_stream);
        }
    }

    return true;
}

static const uint8_t *isoc_find_jpeg_marker(const uint8_t *data, size_t len, uint8_t marker)
{
    if (!data || len < 2) {
        return NULL;
    }

    for (size_t i = 1; i < len; i++) {
        if (data[i - 1] == JPEG_MARKER && data[i] == marker) {
            return data + i - 1;
        }
    }
    return NULL;
}

static bool isoc_add_mjpeg_frame_data(uvc_stream_t *uvc_stream,
                                      const uint8_t *payload_data,
                                      size_t payload_data_len)
{
    size_t offset = 0;
    while (offset < payload_data_len) {
        uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
        if (!current_frame) {
            const uint8_t *soi = isoc_find_jpeg_marker(payload_data + offset,
                                                       payload_data_len - offset,
                                                       JPEG_SOI);
            if (!soi) {
                return true;
            }

            current_frame = isoc_get_next_frame(uvc_stream);
            if (!current_frame) {
                return false;
            }
            uvc_stream->single_thread.skip_current_frame = false;
            offset = (size_t)(soi - payload_data);
        }

        const uint8_t *data = payload_data + offset;
        size_t remaining = payload_data_len - offset;
        size_t chunk = remaining;
        bool complete = false;

        if (current_frame->data_len > 0 &&
            current_frame->data[current_frame->data_len - 1] == JPEG_MARKER &&
            remaining > 0 && data[0] == 0xD9) {
            chunk = 1;
            complete = true;
        } else {
            const uint8_t *eoi = isoc_find_jpeg_marker(data, remaining, 0xD9);
            if (eoi) {
                chunk = (size_t)(eoi - data) + 2U;
                complete = true;
            }
        }

        esp_err_t ret = uvc_frame_add_data(current_frame, data, chunk);
        if (ret != ESP_OK) {
            uvc_stream->single_thread.skip_current_frame = true;
            isoc_emit_frame_event(uvc_stream, UVC_HOST_FRAME_BUFFER_OVERFLOW);
            return false;
        }

        offset += chunk;
        if (complete) {
            isoc_complete_frame(uvc_stream);
        }
    }

    return true;
}

/**
 * @brief Callback function for handling Isochronous USB transfers from a UVC camera.
 *
 * This function processes isochronous transfer packets, which may contain video frame data. The following key points
 * are handled in the transfer:
 *
 * - **Start of Frame (SoF)**: Detected by a change in Frame ID, which toggles between 0 and 1.
 * - **End of Frame (EoF)**: Signaled in the packet header.
 * - **Transfer Characteristics**:
 *   - **No CRC**: Data corruption is possible.
 *   - **No ACK**: Packets can be missed.
 *   - **Packet Header**: Each packet includes a header used to detect errors, missed packets, and other issues.
 *
 * The callback performs the following tasks:
 * 1. Checks the status of each isochronous packet and handles various USB transfer statuses (e.g., completed,
 *    error, device disconnected).
 * 2. Parses packet headers to detect the start of new frames, handles errors, and manages frame buffers.
 * 3. Aggregates valid data into a frame buffer, ensuring no buffer overflow occurs.
 * 4. Signals the end of a frame and invokes user-defined callbacks if necessary.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void isoc_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    // USB_TRANSFER_STATUS_NO_DEVICE is set in transfer->status.
    // Other error codes are saved in status of each ISOC packet descriptor
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
        default:
            assert(false);
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {
            ESP_LOGD(TAG, "invalid UVC payload header, %02x, %02x, len:%d", payload[0], payload[1], isoc_desc->actual_num_bytes);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        const size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;

        if (payload_data_len == 0) {
            // This is a zero-length packet, skip it
            ESP_LOGD(TAG, "zero-length packet, skipping");
            goto next_isoc_packet;
        }

        const bool is_mjpeg = (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG);
        if (is_mjpeg) {
            if (payload_header->bmHeaderInfo.error) {
                ESP_LOGW(TAG, "MJPEG packet error");
                uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
                if (current_frame) {
                    uvc_frame_reset(current_frame);
                }
                goto next_isoc_packet;
            }
            (void)isoc_add_mjpeg_frame_data(uvc_stream, payload_data, payload_data_len);
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            ESP_LOGW(TAG, "frame error");
            uvc_stream->single_thread.skip_current_frame = true;
        }

        const bool payload_starts_soi = payload_data_len >= 2 &&
                                        payload_data[0] == JPEG_MARKER &&
                                        payload_data[1] == JPEG_SOI;
        const bool has_current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame) != NULL;
        if (is_mjpeg && !has_current_frame && !payload_starts_soi) {
            uvc_stream->single_thread.current_frame_id = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }

        const bool start_of_frame =
            (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id) ||
            (is_mjpeg && !has_current_frame && payload_starts_soi);

        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;

            // Check mjpeg frame start
            if (is_mjpeg && !payload_starts_soi) {
                // We received frame with invalid frame, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;
                ESP_LOGD(TAG, "invalid MJPEG SOI");
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->single_thread.skip_current_frame = true;

                    // Inform the user about the underflow
                    isoc_emit_frame_event(uvc_stream, UVC_HOST_FRAME_BUFFER_UNDERFLOW);
                    goto next_isoc_packet;
                }
            } else {
                // We received SoF but current_frame is not NULL: We missed EoF - reset the frame buffer
                ESP_EARLY_LOGD(TAG, "missed EoF");
                uvc_stream->single_thread.skip_current_frame = true;
                uvc_frame_reset(uvc_stream->dynamic.current_frame);
                UVC_EXIT_CRITICAL();
            }
        }

        // Add received data to frame buffer
        if (!uvc_stream->single_thread.skip_current_frame) {
            uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);

            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_YUY2) {
                (void)isoc_add_fixed_size_frame_data(uvc_stream, payload_data, payload_data_len);
                goto maybe_end_of_frame;
            }

            esp_err_t ret = uvc_frame_add_data(current_frame, payload_data, payload_data_len);
            if (ret != ESP_OK) {
                // Frame buffer overflow, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;

                // Inform the user about the overflow
                isoc_emit_frame_event(uvc_stream, UVC_HOST_FRAME_BUFFER_OVERFLOW);
                goto next_isoc_packet;
            }
        }

maybe_end_of_frame:
        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            isoc_complete_frame(uvc_stream);
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
