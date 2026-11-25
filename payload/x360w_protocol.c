/* SPDX-License-Identifier: GPL-3.0-or-later
 * Xbox 360 PC Wireless Gaming Receiver protocol parser and ScePadData mapper.
 */
#include "x360w_protocol.h"

#include <string.h>

static int16_t
read_le16s(const uint8_t *p)
{
    uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
    return (int16_t)v;
}

int
x360w_is_known_vid_pid(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != X360W_USB_VID_MICROSOFT)
        return 0;
    return product_id == X360W_USB_PID_OFFICIAL ||
           product_id == X360W_USB_PID_ORIGINAL ||
           product_id == X360W_USB_PID_CLONE_A9 ||
           product_id == X360W_USB_PID_CLONE_AA ||
           product_id == X360W_USB_PID_CLONE_A1 ||
           product_id == X360W_USB_PID_CLONE_A0;
}

uint8_t
x360w_led_for_slot(unsigned slot_index)
{
    switch (slot_index) {
    case 0u: return X360W_LED_P1_SOLID;
    case 1u: return X360W_LED_P2_SOLID;
    case 2u: return X360W_LED_P3_SOLID;
    case 3u: return X360W_LED_P4_SOLID;
    default: return X360W_LED_OFF;
    }
}

void
x360w_pad_set_neutral(ScePadData *pad, uint8_t connected, uint8_t count,
                      uint64_t timestamp_us)
{
    if (!pad)
        return;
    memset(pad, 0, sizeof(*pad));
    pad->leftStick.x = 128u;
    pad->leftStick.y = 128u;
    pad->rightStick.x = 128u;
    pad->rightStick.y = 128u;
    pad->quat.w = 1.0f;
    pad->connected = connected ? 1u : 0u;
    pad->count = count;
    pad->timestamp = timestamp_us;
}

void
x360w_slot_init(X360WControllerSlot *slot, unsigned slot_index)
{
    if (!slot)
        return;
    memset(slot, 0, sizeof(*slot));
    slot->led_mode = x360w_led_for_slot(slot_index);
    x360w_pad_set_neutral(&slot->pad, 0u, 0u, 0u);
}

uint8_t
x360w_scale_axis(int16_t raw, int16_t deadzone, int invert_y)
{
    int32_t v = (int32_t)raw;
    if (deadzone > 0) {
        if (v > -deadzone && v < deadzone)
            return 128u;
    }
    int32_t scaled;
    if (invert_y) {
        /* Xbox 360: +32767 = UP, -32768 = DOWN.
         * PS5 ScePadData: 0 = UP, 128 = center, 255 = DOWN. */
        scaled = (32768 - v) >> 8;
    } else {
        /* Xbox 360: -32768 = LEFT, +32767 = RIGHT.
         * PS5 ScePadData: 0 = LEFT, 128 = center, 255 = RIGHT. */
        scaled = (v + 32768) >> 8;
    }
    if (scaled < 0)
        scaled = 0;
    else if (scaled > 255)
        scaled = 255;
    return (uint8_t)scaled;
}

static int g_x360w_guitar_mode = 0;

void
x360w_set_guitar_mode(int enabled)
{
    g_x360w_guitar_mode = enabled ? 1 : 0;
}

int
x360w_get_guitar_mode(void)
{
    return g_x360w_guitar_mode;
}

static uint8_t
extract_guitar_frets(uint8_t b_hi)
{
    uint8_t f = 0u;
    if (b_hi & 0x10u) f |= 0x01u; /* Green (A) */
    if (b_hi & 0x20u) f |= 0x02u; /* Red (B) */
    if (b_hi & 0x80u) f |= 0x04u; /* Yellow (Y) */
    if (b_hi & 0x40u) f |= 0x08u; /* Blue (X) */
    if (b_hi & 0x01u) f |= 0x10u; /* Orange (LB) */
    return f;
}

static uint32_t
map_guitar_buttons(uint8_t b_lo, uint8_t b_hi, uint8_t active_frets,
                   uint8_t *lt, uint8_t *rt)
{
    uint32_t out = 0u;

    /* D-Pad (Strum Up/Down + Left/Right), Start, Stick clicks */
    if (b_lo & 0x01u) out |= X360W_SCE_PAD_BUTTON_UP;
    if (b_lo & 0x02u) out |= X360W_SCE_PAD_BUTTON_DOWN;
    if (b_lo & 0x04u) out |= X360W_SCE_PAD_BUTTON_LEFT;
    if (b_lo & 0x08u) out |= X360W_SCE_PAD_BUTTON_RIGHT;
    if (b_lo & 0x10u) out |= X360W_SCE_PAD_BUTTON_OPTIONS;
    if (b_lo & 0x40u) out |= X360W_SCE_PAD_BUTTON_L3;
    if (b_lo & 0x80u) out |= X360W_SCE_PAD_BUTTON_R3;

    /* Menu shortcuts when Back (0x20) is held:
     *   Back + Green (A) -> Cross (Confirm)
     *   Back + Red   (B) -> Circle (Cancel/Back)
     *   Back + Guide     -> Create
     *   Back alone       -> Touchpad (Star Power / Select)
     */
    int back_held = (b_lo & 0x20u) != 0u;
    int menu_combo = 0;
    if (back_held) {
        if (b_hi & 0x10u) {
            out |= X360W_SCE_PAD_BUTTON_CROSS;
            menu_combo = 1;
        }
        if (b_hi & 0x20u) {
            out |= X360W_SCE_PAD_BUTTON_CIRCLE;
            menu_combo = 1;
        }
        if (!menu_combo) {
            if (b_hi & 0x04u)
                out |= X360W_SCE_PAD_BUTTON_CREATE;
            else
                out |= X360W_SCE_PAD_BUTTON_TOUCH_PAD;
        }
    }

    /* Strum-gated frets -> Guitar Hero Gamepad Mode (L2/L1/R1/R2/Cross) */
    if (!menu_combo) {
        if (active_frets & 0x01u) {
            out |= X360W_SCE_PAD_BUTTON_L2;
            *lt = 255u;
        }
        if (active_frets & 0x02u) out |= X360W_SCE_PAD_BUTTON_L1;
        if (active_frets & 0x04u) out |= X360W_SCE_PAD_BUTTON_R1;
        if (active_frets & 0x08u) {
            out |= X360W_SCE_PAD_BUTTON_R2;
            *rt = 255u;
        }
        if (active_frets & 0x10u) out |= X360W_SCE_PAD_BUTTON_CROSS;
    }

    if (b_hi & 0x02u) out |= X360W_SCE_PAD_BUTTON_CIRCLE;

    if (*lt >= X360W_TRIGGER_DIGITAL_THRESHOLD)
        out |= X360W_SCE_PAD_BUTTON_L2;
    if (*rt >= X360W_TRIGGER_DIGITAL_THRESHOLD)
        out |= X360W_SCE_PAD_BUTTON_R2;

    return out;
}

static uint32_t
map_x360_buttons(uint8_t b_lo, uint8_t b_hi, uint8_t *lt, uint8_t *rt)
{
    uint32_t out = 0u;

    /* Byte 0 (p[2] in wired / data[6] in wireless):
     *   bit 0: D-Pad Up      bit 4: Start
     *   bit 1: D-Pad Down    bit 5: Back
     *   bit 2: D-Pad Left    bit 6: Left Stick Click (L3)
     *   bit 3: D-Pad Right   bit 7: Right Stick Click (R3)
     */
    if (b_lo & 0x01u) out |= X360W_SCE_PAD_BUTTON_UP;
    if (b_lo & 0x02u) out |= X360W_SCE_PAD_BUTTON_DOWN;
    if (b_lo & 0x04u) out |= X360W_SCE_PAD_BUTTON_LEFT;
    if (b_lo & 0x08u) out |= X360W_SCE_PAD_BUTTON_RIGHT;
    if (b_lo & 0x10u) out |= X360W_SCE_PAD_BUTTON_OPTIONS;
    if (b_lo & 0x40u) out |= X360W_SCE_PAD_BUTTON_L3;
    if (b_lo & 0x80u) out |= X360W_SCE_PAD_BUTTON_R3;

    /* Standard Xbox 360 Gamepad -> PS5 layout:
     *   bit 0: LB            bit 4: A (Cross)
     *   bit 1: RB            bit 5: B (Circle)
     *   bit 2: Xbox Guide    bit 6: X (Square)
     *   bit 3: (unused)      bit 7: Y (Triangle)
     */
    if (b_hi & 0x01u) out |= X360W_SCE_PAD_BUTTON_L1;
    if (b_hi & 0x02u) out |= X360W_SCE_PAD_BUTTON_R1;
    if (b_hi & 0x10u) out |= X360W_SCE_PAD_BUTTON_CROSS;
    if (b_hi & 0x20u) out |= X360W_SCE_PAD_BUTTON_CIRCLE;
    if (b_hi & 0x40u) out |= X360W_SCE_PAD_BUTTON_SQUARE;
    if (b_hi & 0x80u) out |= X360W_SCE_PAD_BUTTON_TRIANGLE;

    /* Back -> Touchpad click (most PS5 games use Touchpad click for map/menu).
     * Guide + Back -> Create/Share button. */
    if (b_lo & 0x20u) {
        if (b_hi & 0x04u)
            out |= X360W_SCE_PAD_BUTTON_CREATE;
        else
            out |= X360W_SCE_PAD_BUTTON_TOUCH_PAD;
    }

    if (*lt >= X360W_TRIGGER_DIGITAL_THRESHOLD)
        out |= X360W_SCE_PAD_BUTTON_L2;
    if (*rt >= X360W_TRIGGER_DIGITAL_THRESHOLD)
        out |= X360W_SCE_PAD_BUTTON_R2;

    return out;
}

int
x360w_slot_tick(X360WControllerSlot *slot, uint64_t now_us)
{
    if (!slot || !slot->connected || !g_x360w_guitar_mode)
        return 0;
    if (slot->guitar_release_until_us == 0u || now_us < slot->guitar_release_until_us)
        return 0;

    slot->guitar_release_until_us = 0u;
    uint8_t lt = slot->guitar_raw_lt;
    uint8_t rt = slot->guitar_raw_rt;
    slot->pad.buttons = map_guitar_buttons(
        slot->guitar_raw_b_lo, slot->guitar_raw_b_hi,
        slot->guitar_latched_frets, &lt, &rt);
    slot->pad.analogButtons.l2 = lt;
    slot->pad.analogButtons.r2 = rt;
    slot->pad.timestamp = now_us;
    return 1;
}

X360WPacketEvent
x360w_process_packet(X360WControllerSlot *slot, unsigned slot_index,
                     const uint8_t *data, size_t len, uint64_t now_us)
{
    if (!slot || !data || len < 2u)
        return X360W_PKT_IGNORED;

    /* Presence / connection status notification:
     *   data[0] & 0x08: status change
     *   data[1] & 0x80: controller present
     *   data[1] & 0x40: headset present
     */
    if (data[0] & 0x08u) {
        uint8_t present = (data[1] & 0x80u) ? 1u : 0u;
        slot->headset_present = (data[1] & 0x40u) ? 1u : 0u;
        slot->last_packet_us = now_us;
        if (present) {
            if (!slot->connected) {
                slot->connected = 1u;
                slot->connect_count++;
                if (slot->connect_count == 0u)
                    slot->connect_count = 1u;
                slot->led_mode = x360w_led_for_slot(slot_index);
                slot->led_pending = 1u;
                x360w_pad_set_neutral(&slot->pad, 1u, slot->connect_count, now_us);
            }
            return X360W_PKT_CONNECT;
        } else {
            slot->connected = 0u;
            slot->led_pending = 0u;
            slot->rumble_large = 0u;
            slot->rumble_small = 0u;
            x360w_pad_set_neutral(&slot->pad, 0u, slot->connect_count, now_us);
            return X360W_PKT_DISCONNECT;
        }
    }

    /* Standard wireless controller input report:
     *   data[0] == 0x00, data[1] == 0x01 (or bit 0 set), length >= 18
     *   Payload starts at data + 4 (matches Linux xpad.c xpad360w_process_packet):
     *     p[0..1]: 0x00, 0x13/0x14 (report header)
     *     p[2]:    buttons low (D-pad, Start, Back, L3, R3)
     *     p[3]:    buttons high (LB, RB, Guide, A, B, X, Y)
     *     p[4]:    Left trigger (0..255)
     *     p[5]:    Right trigger (0..255)
     *     p[6..7]: Left stick X (int16 LE)
     *     p[8..9]: Left stick Y (int16 LE)
     *     p[10..11]: Right stick X (int16 LE)
     *     p[12..13]: Right stick Y (int16 LE)
     */
    if (data[0] == 0x00u && data[1] == 0x01u && len >= 18u) {
        const uint8_t *p = data + 4;
        if (!slot->connected) {
            slot->connected = 1u;
            slot->connect_count++;
            if (slot->connect_count == 0u)
                slot->connect_count = 1u;
            slot->led_mode = x360w_led_for_slot(slot_index);
            slot->led_pending = 1u;
        }

        uint8_t b_lo = p[2];
        uint8_t b_hi = p[3];
        uint8_t lt = p[4];
        uint8_t rt = p[5];
        int16_t lx = read_le16s(p + 6);
        int16_t ly = read_le16s(p + 8);
        int16_t rx = read_le16s(p + 10);
        int16_t ry = read_le16s(p + 12);

        ScePadData *pad = &slot->pad;
        if (g_x360w_guitar_mode) {
            uint8_t frets = extract_guitar_frets(b_hi);
            uint8_t strum_now = b_lo & 0x03u; /* D-Pad Up (0x01) or Down (0x02) */
            int strum_edge = (strum_now != 0u) && (strum_now != slot->guitar_prev_strum);
            slot->guitar_prev_strum = strum_now;
            slot->guitar_raw_b_lo = b_lo;
            slot->guitar_raw_b_hi = b_hi;
            slot->guitar_raw_lt = lt;
            slot->guitar_raw_rt = rt;

            if (strum_edge) {
                if ((frets & slot->guitar_latched_frets) != 0u) {
                    /* Re-strumming an already-held fret: pulse off briefly so
                     * repeated notes re-trigger in Gamepad GH mode. */
                    slot->guitar_release_until_us = now_us + X360W_GUITAR_RESTRUM_PULSE_US;
                } else {
                    slot->guitar_release_until_us = 0u;
                }
                slot->guitar_latched_frets = frets;
            } else {
                /* Keep sustains latched while frets stay held; release when lifted. */
                slot->guitar_latched_frets &= frets;
                if (slot->guitar_latched_frets == 0u)
                    slot->guitar_release_until_us = 0u;
            }

            if (slot->guitar_release_until_us != 0u &&
                now_us >= slot->guitar_release_until_us) {
                slot->guitar_release_until_us = 0u;
            }

            uint8_t active_frets = (slot->guitar_release_until_us != 0u)
                ? 0u : slot->guitar_latched_frets;
            pad->buttons = map_guitar_buttons(b_lo, b_hi, active_frets, &lt, &rt);
        } else {
            slot->guitar_prev_strum = 0u;
            slot->guitar_latched_frets = 0u;
            slot->guitar_release_until_us = 0u;
            pad->buttons = map_x360_buttons(b_lo, b_hi, &lt, &rt);
        }
        pad->analogButtons.l2 = lt;
        pad->analogButtons.r2 = rt;
        pad->leftStick.x = x360w_scale_axis(lx, X360W_DEFAULT_STICK_DEADZONE, 0);
        pad->leftStick.y = x360w_scale_axis(ly, X360W_DEFAULT_STICK_DEADZONE, 1);
        pad->rightStick.x = x360w_scale_axis(rx, X360W_DEFAULT_STICK_DEADZONE, 0);
        pad->rightStick.y = x360w_scale_axis(ry, X360W_DEFAULT_STICK_DEADZONE, 1);
        pad->quat.x = 0.0f;
        pad->quat.y = 0.0f;
        pad->quat.z = 0.0f;
        pad->quat.w = 1.0f;
        pad->connected = 1u;
        pad->count = slot->connect_count;
        pad->timestamp = now_us;

        slot->packets_received++;
        slot->last_packet_us = now_us;
        return X360W_PKT_INPUT;
    }

    return X360W_PKT_IGNORED;
}

void
x360w_build_inquiry_cmd(uint8_t out_cmd[X360W_CMD_LEN])
{
    memset(out_cmd, 0, X360W_CMD_LEN);
    out_cmd[0] = 0x08u;
    out_cmd[1] = 0x00u;
    out_cmd[2] = 0x0fu;
    out_cmd[3] = 0xc0u;
}

void
x360w_build_led_cmd(uint8_t out_cmd[X360W_CMD_LEN], uint8_t led_mode)
{
    memset(out_cmd, 0, X360W_CMD_LEN);
    out_cmd[0] = 0x00u;
    out_cmd[1] = 0x00u;
    out_cmd[2] = 0x08u;
    out_cmd[3] = (uint8_t)(0x40u + (led_mode & 0x0fu));
}

void
x360w_build_rumble_cmd(uint8_t out_cmd[X360W_CMD_LEN],
                       uint8_t large_motor, uint8_t small_motor)
{
    memset(out_cmd, 0, X360W_CMD_LEN);
    out_cmd[0] = 0x00u;
    out_cmd[1] = 0x01u;
    out_cmd[2] = 0x0fu;
    out_cmd[3] = 0xc0u;
    out_cmd[4] = 0x00u;
    out_cmd[5] = large_motor;
    out_cmd[6] = small_motor;
}
