/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-native unit tests for Xbox 360 Wireless Receiver protocol and mapping.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "x360w_protocol.h"

static void
put_le16s(uint8_t *p, int16_t v)
{
    uint16_t u = (uint16_t)v;
    p[0] = (uint8_t)(u & 0xffu);
    p[1] = (uint8_t)((u >> 8u) & 0xffu);
}

static void
test_vid_pid(void)
{
    assert(x360w_is_known_vid_pid(0x045eu, 0x0719u) == 1);
    assert(x360w_is_known_vid_pid(0x045eu, 0x0291u) == 1);
    assert(x360w_is_known_vid_pid(0x045eu, 0x02a9u) == 1);
    assert(x360w_is_known_vid_pid(0x045eu, 0x02aau) == 1);
    assert(x360w_is_known_vid_pid(0x054cu, 0x0ce6u) == 0); /* DualSense */
    assert(x360w_is_known_vid_pid(0x054cu, 0x09ccu) == 0); /* DualShock 4 */
    assert(x360w_is_known_vid_pid(0x045eu, 0x028eu) == 0); /* Wired 360 */
}

static void
test_connect_disconnect(void)
{
    X360WControllerSlot slot;
    x360w_slot_init(&slot, 1u); /* Player 2 */
    assert(slot.connected == 0u);
    assert(slot.pad.connected == 0u);
    assert(slot.pad.leftStick.x == 128u);
    assert(slot.pad.leftStick.y == 128u);

    uint8_t conn_pkt[2] = {0x08u, 0x80u};
    X360WPacketEvent ev = x360w_process_packet(&slot, 1u, conn_pkt, sizeof(conn_pkt), 1000u);
    assert(ev == X360W_PKT_CONNECT);
    assert(slot.connected == 1u);
    assert(slot.connect_count == 1u);
    assert(slot.led_pending == 1u);
    assert(slot.led_mode == X360W_LED_P2_SOLID);
    assert(slot.pad.connected == 1u);
    assert(slot.pad.count == 1u);

    uint8_t disc_pkt[2] = {0x08u, 0x00u};
    ev = x360w_process_packet(&slot, 1u, disc_pkt, sizeof(disc_pkt), 2000u);
    assert(ev == X360W_PKT_DISCONNECT);
    assert(slot.connected == 0u);
    assert(slot.pad.connected == 0u);
    assert(slot.pad.buttons == 0u);
    assert(slot.pad.leftStick.x == 128u);
    assert(slot.pad.leftStick.y == 128u);
}

static void
test_buttons_and_triggers(void)
{
    X360WControllerSlot slot;
    x360w_slot_init(&slot, 0u);

    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00u;
    pkt[1] = 0x01u;
    pkt[2] = 0x00u;
    pkt[3] = 0xf0u;
    pkt[4] = 0x00u;
    pkt[5] = 0x13u;
    /* b_lo: D-Pad Up (0x01) + Start (0x10) + Back (0x20) + L3 (0x40) */
    pkt[6] = 0x01u | 0x10u | 0x20u | 0x40u;
    /* b_hi: LB (0x01) + A (0x10) + Y (0x80) */
    pkt[7] = 0x01u | 0x10u | 0x80u;
    /* Triggers: LT = 200 (above threshold), RT = 10 (below threshold) */
    pkt[8] = 200u;
    pkt[9] = 10u;

    X360WPacketEvent ev = x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 5000u);
    assert(ev == X360W_PKT_INPUT);
    assert(slot.connected == 1u);
    assert(slot.pad.connected == 1u);
    assert(slot.pad.analogButtons.l2 == 200u);
    assert(slot.pad.analogButtons.r2 == 10u);

    uint32_t expected = X360W_SCE_PAD_BUTTON_UP |
                        X360W_SCE_PAD_BUTTON_OPTIONS |
                        X360W_SCE_PAD_BUTTON_TOUCH_PAD |
                        X360W_SCE_PAD_BUTTON_L3 |
                        X360W_SCE_PAD_BUTTON_L1 |
                        X360W_SCE_PAD_BUTTON_CROSS |
                        X360W_SCE_PAD_BUTTON_TRIANGLE |
                        X360W_SCE_PAD_BUTTON_L2;
    assert(slot.pad.buttons == expected);

    /* Guide + Back -> Create instead of Touchpad */
    pkt[6] = 0x20u;
    pkt[7] = 0x04u;
    pkt[8] = 0u;
    pkt[9] = 255u;
    ev = x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 6000u);
    assert(ev == X360W_PKT_INPUT);
    /* Announce / serial packet (data[1] == 0x0f) must be ignored, not decoded as buttons */
    uint8_t announce_pkt[32];
    memset(announce_pkt, 0xff, sizeof(announce_pkt));
    announce_pkt[0] = 0x00u;
    announce_pkt[1] = 0x0fu;
    ev = x360w_process_packet(&slot, 0u, announce_pkt, sizeof(announce_pkt), 7000u);
    assert(ev == X360W_PKT_IGNORED);
    assert(slot.pad.buttons == (X360W_SCE_PAD_BUTTON_CREATE | X360W_SCE_PAD_BUTTON_R2));
}

static void
test_sticks_and_deadzone(void)
{
    /* Deadzone centers small drift to 128 */
    assert(x360w_scale_axis(0, X360W_DEFAULT_STICK_DEADZONE, 0) == 128u);
    assert(x360w_scale_axis(1500, X360W_DEFAULT_STICK_DEADZONE, 0) == 128u);
    assert(x360w_scale_axis(-1500, X360W_DEFAULT_STICK_DEADZONE, 1) == 128u);

    /* Full X range: -32768 -> 0 (left), +32767 -> 255 (right) */
    assert(x360w_scale_axis(-32768, X360W_DEFAULT_STICK_DEADZONE, 0) == 0u);
    assert(x360w_scale_axis(32767, X360W_DEFAULT_STICK_DEADZONE, 0) == 255u);

    /* Full Y range (inverted): +32767 (UP) -> 0, -32768 (DOWN) -> 255 */
    assert(x360w_scale_axis(32767, X360W_DEFAULT_STICK_DEADZONE, 1) == 0u);
    assert(x360w_scale_axis(-32768, X360W_DEFAULT_STICK_DEADZONE, 1) == 255u);

    /* Verify end-to-end packet stick decoding */
    X360WControllerSlot slot;
    x360w_slot_init(&slot, 0u);
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00u;
    pkt[1] = 0x01u;
    put_le16s(pkt + 10, -32768); /* LX full left */
    put_le16s(pkt + 12, 32767);  /* LY full up */
    put_le16s(pkt + 14, 32767);  /* RX full right */
    put_le16s(pkt + 16, -32768); /* RY full down */

    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 9000u) == X360W_PKT_INPUT);
    assert(slot.pad.leftStick.x == 0u);
    assert(slot.pad.leftStick.y == 0u);
    assert(slot.pad.rightStick.x == 255u);
    assert(slot.pad.rightStick.y == 255u);
}

static void
test_commands(void)
{
    uint8_t cmd[X360W_CMD_LEN];

    x360w_build_inquiry_cmd(cmd);
    assert(cmd[0] == 0x08u && cmd[1] == 0x00u && cmd[2] == 0x0fu && cmd[3] == 0xc0u);

    x360w_build_led_cmd(cmd, x360w_led_for_slot(0u));
    assert(cmd[0] == 0x00u && cmd[2] == 0x08u && cmd[3] == 0x46u);

    x360w_build_led_cmd(cmd, x360w_led_for_slot(3u));
    assert(cmd[3] == 0x49u);

    x360w_build_rumble_cmd(cmd, 0xaau, 0x55u);
    assert(cmd[0] == 0x00u && cmd[1] == 0x01u && cmd[2] == 0x0fu &&
           cmd[3] == 0xc0u && cmd[5] == 0xaau && cmd[6] == 0x55u);
}

static void
test_guitar_hero_remap(void)
{
    X360WControllerSlot slot;
    x360w_slot_init(&slot, 0u);
    x360w_set_guitar_mode(1);
    assert(x360w_get_guitar_mode() == 1);

    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00u;
    pkt[1] = 0x01u;

    /* 1. Holding frets WITHOUT strumming must NOT fire fret notes yet */
    pkt[6] = 0x00u;
    pkt[7] = 0x10u | 0x20u | 0x80u | 0x40u | 0x01u;
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 10000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == 0u);
    assert(slot.pad.analogButtons.l2 == 0u);
    assert(slot.pad.analogButtons.r2 == 0u);

    /* 2. Strum Down (D-Pad Down = 0x02) with all 5 frets held fires all 5 */
    pkt[6] = 0x02u;
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 12000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == (X360W_SCE_PAD_BUTTON_DOWN |
                                X360W_SCE_PAD_BUTTON_L2 |
                                X360W_SCE_PAD_BUTTON_L1 |
                                X360W_SCE_PAD_BUTTON_R1 |
                                X360W_SCE_PAD_BUTTON_R2 |
                                X360W_SCE_PAD_BUTTON_CROSS));
    assert(slot.pad.analogButtons.l2 == 255u);
    assert(slot.pad.analogButtons.r2 == 255u);

    /* 3. Releasing Strum while holding Green keeps Green sustained */
    pkt[6] = 0x00u;
    pkt[7] = 0x10u; /* only Green held */
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 15000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == X360W_SCE_PAD_BUTTON_L2);
    assert(slot.pad.analogButtons.l2 == 255u);

    /* 4. Re-strumming Down while still holding Green triggers 20ms release pulse */
    pkt[6] = 0x02u;
    pkt[7] = 0x10u;
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 20000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == X360W_SCE_PAD_BUTTON_DOWN);
    assert(slot.pad.analogButtons.l2 == 0u);

    /* 5. After 20ms, x360w_slot_tick re-asserts Green automatically */
    assert(x360w_slot_tick(&slot, 45000u) == 1);
    assert(slot.pad.buttons == (X360W_SCE_PAD_BUTTON_DOWN | X360W_SCE_PAD_BUTTON_L2));
    assert(slot.pad.analogButtons.l2 == 255u);

    /* 6. Menu shortcuts: Back + Green -> Cross (Confirm), Back + Red -> Circle (Cancel) */
    pkt[6] = 0x20u;
    pkt[7] = 0x10u;
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 50000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == X360W_SCE_PAD_BUTTON_CROSS);

    pkt[6] = 0x20u;
    pkt[7] = 0x20u;
    assert(x360w_process_packet(&slot, 0u, pkt, sizeof(pkt), 55000u) == X360W_PKT_INPUT);
    assert(slot.pad.buttons == X360W_SCE_PAD_BUTTON_CIRCLE);

    x360w_set_guitar_mode(0);
    assert(x360w_get_guitar_mode() == 0);
}

int
main(void)
{
    test_vid_pid();
    test_connect_disconnect();
    test_buttons_and_triggers();
    test_sticks_and_deadzone();
    test_commands();
    test_guitar_hero_remap();
    printf("All x360w_protocol tests passed!\n");
    return 0;
}
