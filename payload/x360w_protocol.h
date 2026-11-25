/* SPDX-License-Identifier: GPL-3.0-or-later
 * Xbox 360 PC Wireless Gaming Receiver protocol parser and ScePadData mapper.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pad_types.h"

#define X360W_MAX_CONTROLLERS 4u
#define X360W_PACKET_MAX_LEN  32u
#define X360W_CMD_LEN         12u

/* Known USB Vendor/Product IDs for Microsoft Xbox 360 Wireless Receivers
 * and common third-party clones. */
#define X360W_USB_VID_MICROSOFT 0x045eu
#define X360W_USB_PID_OFFICIAL  0x0719u /* Xbox 360 Wireless Receiver for Windows */
#define X360W_USB_PID_ORIGINAL  0x0291u /* Original receiver & most clones */
#define X360W_USB_PID_CLONE_A9  0x02a9u /* Common third-party clone */
#define X360W_USB_PID_CLONE_AA  0x02aau /* Common third-party clone */
#define X360W_USB_PID_CLONE_A1  0x02a1u /* Variant clone */
#define X360W_USB_PID_CLONE_A0  0x02a0u /* Variant clone */

/* Vendor-specific USB interface class/subclass/protocol for Xbox 360 wireless
 * controller interfaces (interfaces 0, 2, 4, 6 on the receiver). */
#define X360W_USB_IFACE_CLASS    0xffu
#define X360W_USB_IFACE_SUBCLASS 0x5du
#define X360W_USB_IFACE_PROTOCOL 0x81u

/* Xbox 360 LED ring commands (byte 3 = 0x40 + led_mode). */
#define X360W_LED_OFF       0x00u
#define X360W_LED_BLINK_ALL 0x01u
#define X360W_LED_P1_SOLID  0x06u
#define X360W_LED_P2_SOLID  0x07u
#define X360W_LED_P3_SOLID  0x08u
#define X360W_LED_P4_SOLID  0x09u
#define X360W_LED_ROTATING  0x0au

/* PS5 / Orbis ScePadData button bitmasks. */
#define X360W_SCE_PAD_BUTTON_CREATE    0x00000001u
#define X360W_SCE_PAD_BUTTON_L3        0x00000002u
#define X360W_SCE_PAD_BUTTON_R3        0x00000004u
#define X360W_SCE_PAD_BUTTON_OPTIONS   0x00000008u
#define X360W_SCE_PAD_BUTTON_UP        0x00000010u
#define X360W_SCE_PAD_BUTTON_RIGHT     0x00000020u
#define X360W_SCE_PAD_BUTTON_DOWN      0x00000040u
#define X360W_SCE_PAD_BUTTON_LEFT      0x00000080u
#define X360W_SCE_PAD_BUTTON_L2        0x00000100u
#define X360W_SCE_PAD_BUTTON_R2        0x00000200u
#define X360W_SCE_PAD_BUTTON_L1        0x00000400u
#define X360W_SCE_PAD_BUTTON_R1        0x00000800u
#define X360W_SCE_PAD_BUTTON_TRIANGLE  0x00001000u
#define X360W_SCE_PAD_BUTTON_CIRCLE    0x00002000u
#define X360W_SCE_PAD_BUTTON_CROSS     0x00004000u
#define X360W_SCE_PAD_BUTTON_SQUARE    0x00008000u
#define X360W_SCE_PAD_BUTTON_TOUCH_PAD 0x00100000u

/* Trigger threshold for setting digital L2/R2 bits alongside analog values. */
#define X360W_TRIGGER_DIGITAL_THRESHOLD 32u

/* Default radial/axis deadzone in raw signed 16-bit stick units (~10%). */
#define X360W_DEFAULT_STICK_DEADZONE 3200

/* Re-strum release pulse duration (microseconds) so repeated notes re-trigger. */
#define X360W_GUITAR_RESTRUM_PULSE_US UINT64_C(20000)

typedef struct {
    uint8_t  connected;
    uint8_t  headset_present;
    uint8_t  connect_count;
    uint8_t  led_pending;
    uint8_t  led_mode;
    uint8_t  rumble_large;
    uint8_t  rumble_small;
    uint8_t  guitar_prev_strum;
    uint8_t  guitar_latched_frets;
    uint8_t  guitar_raw_b_lo;
    uint8_t  guitar_raw_b_hi;
    uint8_t  guitar_raw_lt;
    uint8_t  guitar_raw_rt;
    uint8_t  reserved[3];
    uint32_t packets_received;
    uint64_t last_packet_us;
    uint64_t guitar_release_until_us;
    ScePadData pad;
} X360WControllerSlot;

typedef enum {
    X360W_PKT_IGNORED = 0,
    X360W_PKT_CONNECT = 1,
    X360W_PKT_DISCONNECT = 2,
    X360W_PKT_INPUT = 3
} X360WPacketEvent;

/* Returns non-zero if (vendor_id, product_id) is a known Xbox 360 Wireless Receiver. */
int x360w_is_known_vid_pid(uint16_t vendor_id, uint16_t product_id);

/* Initialize a controller slot to a clean disconnected neutral ScePadData state. */
void x360w_slot_init(X360WControllerSlot *slot, unsigned slot_index);

/* Reset ScePadData to centered sticks, released buttons, preserving connected/count. */
void x360w_pad_set_neutral(ScePadData *pad, uint8_t connected, uint8_t count,
                           uint64_t timestamp_us);

/* Convert a signed 16-bit Xbox 360 stick axis to PS5 0..255 (center 128).
 * Set invert_y != 0 for vertical axes (Xbox Y+ is up; PS5 Y=0 is up). */
uint8_t x360w_scale_axis(int16_t raw, int16_t deadzone, int invert_y);

/* Enable or disable Strum-Gated Guitar Hero remap mode:
 * Holding frets + Strum Up/Down fires L2/L1/R1/R2/Cross (sustained until fret
 * release, pulsed on re-strum). Back+Green=Cross (Confirm), Back+Red=Circle. */
void x360w_set_guitar_mode(int enabled);
int x360w_get_guitar_mode(void);

/* Advance timed state (e.g. re-strum release pulse completion) between USB packets.
 * Returns 1 if slot->pad changed and should be republished immediately. */
int x360w_slot_tick(X360WControllerSlot *slot, uint64_t now_us);

/* Parse one raw USB Interrupt IN packet from an Xbox 360 wireless receiver slot.
 * Updates slot->connected, slot->pad, slot->led_pending, and returns the event kind. */
X360WPacketEvent x360w_process_packet(X360WControllerSlot *slot,
                                      unsigned slot_index,
                                      const uint8_t *data, size_t len,
                                      uint64_t now_us);

/* Build the 12-byte inquiry packet that asks the receiver for current controller presence. */
void x360w_build_inquiry_cmd(uint8_t out_cmd[X360W_CMD_LEN]);

/* Build the 12-byte LED command for a given X360W_LED_* mode (or player slot 0..3). */
void x360w_build_led_cmd(uint8_t out_cmd[X360W_CMD_LEN], uint8_t led_mode);
uint8_t x360w_led_for_slot(unsigned slot_index);

/* Build the 12-byte rumble motor command (large = left low-freq, small = right high-freq). */
void x360w_build_rumble_cmd(uint8_t out_cmd[X360W_CMD_LEN],
                            uint8_t large_motor, uint8_t small_motor);
