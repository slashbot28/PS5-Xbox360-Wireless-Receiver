/* SPDX-License-Identifier: GPL-3.0-or-later
 * PS5 FreeBSD ugen USB driver for the Xbox 360 PC Wireless Gaming Receiver.
 */
#pragma once

#include <stdint.h>

#include "x360w_protocol.h"

typedef enum {
    X360W_TRANSPORT_NONE = 0,
    X360W_TRANSPORT_USB_FS = 1,
    X360W_TRANSPORT_DEV_EP = 2
} X360WTransportMode;

typedef struct {
    int                open;
    X360WTransportMode transport;
    uint8_t            bus;
    uint8_t            addr;
    uint16_t           vendor_id;
    uint16_t           product_id;
    int                ctl_fd;
    int                ep_fd[X360W_MAX_CONTROLLERS];
    uint8_t            in_ep[X360W_MAX_CONTROLLERS];
    uint8_t            out_ep[X360W_MAX_CONTROLLERS];
    uint8_t            out_busy[X360W_MAX_CONTROLLERS];
    uint8_t            in_buf[X360W_MAX_CONTROLLERS][X360W_PACKET_MAX_LEN];
    uint8_t            out_buf[X360W_MAX_CONTROLLERS][X360W_PACKET_MAX_LEN];
    void              *in_ptr[X360W_MAX_CONTROLLERS];
    void              *out_ptr[X360W_MAX_CONTROLLERS];
    uint32_t           in_len[X360W_MAX_CONTROLLERS];
    uint32_t           out_len[X360W_MAX_CONTROLLERS];
    void              *fs_endpoints; /* struct usb_fs_endpoint[8] */
    uint32_t           fatal_errors;
    X360WControllerSlot slots[X360W_MAX_CONTROLLERS];
} X360WReceiver;

/* Initialize receiver handle state (does not open any USB device). */
void x360w_usb_init(X360WReceiver *rx);

/* Scan /dev/usb for an Xbox 360 Wireless Receiver and open it.
 * Returns 0 on success, -1 if not found or unable to open. */
int x360w_usb_open(X360WReceiver *rx);

/* Close any open USB descriptors and reset controller slot states. */
void x360w_usb_close(X360WReceiver *rx);

/* Poll for incoming wireless packets for up to timeout_ms milliseconds.
 * Updates rx->slots[0..3] and automatically dispatches pending LED commands.
 * Returns:
 *   >= 0 : bitmask of slots that received an event (bits 0..3)
 *    -1  : receiver disconnected or fatal USB error (caller should close & rescan)
 */
int x360w_usb_poll(X360WReceiver *rx, int timeout_ms, uint64_t now_us);

/* Send a 12-byte wireless command (inquiry, LED, or rumble) to a slot (0..3).
 * Returns 0 on success, -1 on error. */
int x360w_usb_send_cmd(X360WReceiver *rx, unsigned slot_index,
                       const uint8_t cmd[X360W_CMD_LEN]);

/* Convenience: send rumble motors to a connected controller slot. */
int x360w_usb_set_rumble(X360WReceiver *rx, unsigned slot_index,
                         uint8_t large_motor, uint8_t small_motor);

/* Convenience: return number of currently connected wireless controllers (0..4). */
unsigned x360w_usb_connected_count(const X360WReceiver *rx);
