/* SPDX-License-Identifier: GPL-3.0-or-later
 * Xbox 360 PC Wireless Gaming Receiver -> native PS5 game ScePadData bridge.
 *
 * Reads up to 4 wireless Xbox 360 controllers over USB (FreeBSD ugen) and
 * injects their inputs into running PS5 games using PoorDS4's libScePad
 * import bridge, with vibration feedback relayed back to the controllers.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/payload.h>

#include "pad_types.h"
#include "version.h"
#include "wireless_ds4.h"
#include "x360w_protocol.h"
#include "x360w_usb.h"

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/x360w"
#endif

#define X360W_STOP_FILE   POORDS4_DATA_DIR "/stop-bridge"
#define X360W_GUITAR_FILE POORDS4_DATA_DIR "/guitar-mode"
#define X360W_LOG_FILE    POORDS4_DATA_DIR "/bridge.log"
#define X360W_LOCK_FILE  POORDS4_DATA_DIR "/supervisor.lock"
#define X360W_STATE_FILE POORDS4_DATA_DIR "/supervisor.txt"
#define X360W_LOG_LIMIT  (512u * 1024u)

typedef struct {
    int32_t user_id[4];
} SceUserServiceLoginUserIdList;

extern int sceUserServiceInitialize(void *params);
extern int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList *list);
extern int sceUserServiceGetInitialUser(int32_t *user_id);

extern int sceKernelOpenEventFlag(intptr_t *event_flag, const char *name);
extern int sceKernelPollEventFlag(intptr_t event_flag, uint64_t bits,
                                  unsigned int wait_mode,
                                  uint64_t *result_pattern);
extern int sceKernelCloseEventFlag(intptr_t event_flag);

#define X360W_EVENT_WAITMODE_OR 2u
#define SYSTEM_STATE_SHUTDOWN_ON_GOING 100u
#define SYSTEM_STATE_SUSPEND_ON_GOING  300u
#define SYSTEM_STATE_MAIN_ON_STANDBY   500u

static int g_lock_fd = -1;
static size_t g_log_bytes;
static volatile sig_atomic_t g_shutdown_requested;
static intptr_t g_system_state_info_flag = -1;
static uint64_t g_last_monotonic_ms;
static uint64_t g_last_wall_ms;

static uint64_t
monotonic_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)ts.tv_nsec / UINT64_C(1000);
}

static uint64_t
wall_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000) +
           (uint64_t)ts.tv_nsec / UINT64_C(1000000);
}

void
poords4_log(const char *format, ...)
{
    char buf[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n <= 0)
        return;

    klog_printf("%s", buf);

    if (g_log_bytes > X360W_LOG_LIMIT)
        return;
    int fd = open(X360W_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1u;
        ssize_t w = write(fd, buf, len);
        if (w > 0)
            g_log_bytes += (size_t)w;
        close(fd);
    }
}

static void
on_signal(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

static int
acquire_instance_lock(void)
{
    mkdir(POORDS4_DATA_DIR, 0755);
    int fd = open(X360W_LOCK_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    char pid_str[32];
    int len = snprintf(pid_str, sizeof(pid_str), "%d\n", (int)getpid());
    if (len > 0) {
        (void)ftruncate(fd, 0);
        (void)lseek(fd, 0, SEEK_SET);
        (void)write(fd, pid_str, (size_t)len);
    }
    g_lock_fd = fd;
    return 0;
}

static void
release_instance_lock(void)
{
    if (g_lock_fd >= 0) {
        (void)flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

static int
check_stop_or_suspend(void)
{
    if (g_shutdown_requested)
        return 1;
    if (access(X360W_STOP_FILE, F_OK) == 0) {
        (void)unlink(X360W_STOP_FILE);
        poords4_log("[X360W] Stop file detected; shutting down cleanly\n");
        return 1;
    }
    if (g_system_state_info_flag >= 0) {
        uint64_t pat = 0;
        if (sceKernelPollEventFlag(g_system_state_info_flag, UINT64_MAX,
                                   X360W_EVENT_WAITMODE_OR, &pat) == 0) {
            unsigned state = (unsigned)(pat & 0xffffu);
            if (state == SYSTEM_STATE_SHUTDOWN_ON_GOING ||
                state == SYSTEM_STATE_SUSPEND_ON_GOING ||
                state == SYSTEM_STATE_MAIN_ON_STANDBY) {
                poords4_log("[X360W] System state=%u detected; quiescing\n", state);
                return 2;
            }
        }
    }
    uint64_t now_mono = monotonic_us() / 1000u;
    uint64_t now_wall = wall_ms();
    if (g_last_monotonic_ms > 0u && g_last_wall_ms > 0u) {
        int64_t d_mono = (int64_t)(now_mono - g_last_monotonic_ms);
        int64_t d_wall = (int64_t)(now_wall - g_last_wall_ms);
        if (d_wall - d_mono > 15000) {
            poords4_log("[X360W] Resume gap detected; shutting down cleanly\n");
            return 2;
        }
    }
    g_last_monotonic_ms = now_mono;
    g_last_wall_ms = now_wall;
    return 0;
}

static uint32_t
collect_ps5_users(int32_t out_users[X360W_MAX_CONTROLLERS])
{
    for (unsigned i = 0; i < X360W_MAX_CONTROLLERS; ++i)
        out_users[i] = -1;

    (void)sceUserServiceInitialize(NULL);

    uint32_t count = 0;
    int32_t initial = -1;
    if (sceUserServiceGetInitialUser(&initial) == 0 && initial > 0)
        out_users[count++] = initial;

    SceUserServiceLoginUserIdList list;
    memset(&list, 0xff, sizeof(list));
    if (sceUserServiceGetLoginUserIdList(&list) == 0) {
        for (unsigned i = 0; i < 4u && count < X360W_MAX_CONTROLLERS; ++i) {
            int32_t uid = list.user_id[i];
            if (uid <= 0)
                continue;
            int dup = 0;
            for (unsigned j = 0; j < count; ++j) {
                if (out_users[j] == uid) {
                    dup = 1;
                    break;
                }
            }
            if (!dup)
                out_users[count++] = uid;
        }
    }
    return count;
}

static void
write_status_file(const X360WReceiver *rx, pid_t game_pid, intptr_t bridge_args,
                  uint32_t user_count, const int32_t users[X360W_MAX_CONTROLLERS])
{
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", X360W_STATE_FILE);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;

    char buf[1024];
    int len = snprintf(
        buf, sizeof(buf),
        "version=%s\n"
        "pid=%d\n"
        "receiver_open=%d\n"
        "receiver_bus=%u\n"
        "receiver_addr=%u\n"
        "receiver_vid=0x%04x\n"
        "receiver_pid=0x%04x\n"
        "transport=%s\n"
        "connected_controllers=%u\n"
        "guitar_mode=%d\n"
        "game_pid=%d\n"
        "bridge_args=0x%lx\n"
        "ps5_users=%u (0x%08x,0x%08x,0x%08x,0x%08x)\n"
        "slot0_connected=%u slot0_packets=%u slot0_buttons=0x%08x\n"
        "slot1_connected=%u slot1_packets=%u slot1_buttons=0x%08x\n"
        "slot2_connected=%u slot2_packets=%u slot2_buttons=0x%08x\n"
        "slot3_connected=%u slot3_packets=%u slot3_buttons=0x%08x\n",
        X360W_VERSION,
        (int)getpid(),
        rx ? rx->open : 0,
        rx ? (unsigned)rx->bus : 0u,
        rx ? (unsigned)rx->addr : 0u,
        rx ? (unsigned)rx->vendor_id : 0u,
        rx ? (unsigned)rx->product_id : 0u,
        (rx && rx->open)
            ? (rx->transport == X360W_TRANSPORT_USB_FS ? "usb_fs" : "dev_ep")
            : "none",
        x360w_usb_connected_count(rx),
        x360w_get_guitar_mode(),
        (int)game_pid,
        (unsigned long)bridge_args,
        user_count,
        (uint32_t)users[0], (uint32_t)users[1],
        (uint32_t)users[2], (uint32_t)users[3],
        rx ? (unsigned)rx->slots[0].connected : 0u,
        rx ? rx->slots[0].packets_received : 0u,
        rx ? rx->slots[0].pad.buttons : 0u,
        rx ? (unsigned)rx->slots[1].connected : 0u,
        rx ? rx->slots[1].packets_received : 0u,
        rx ? rx->slots[1].pad.buttons : 0u,
        rx ? (unsigned)rx->slots[2].connected : 0u,
        rx ? rx->slots[2].packets_received : 0u,
        rx ? rx->slots[2].pad.buttons : 0u,
        rx ? (unsigned)rx->slots[3].connected : 0u,
        rx ? rx->slots[3].packets_received : 0u,
        rx ? rx->slots[3].pad.buttons : 0u);
    if (len > 0)
        (void)write(fd, buf, (size_t)len);
    close(fd);
    (void)rename(tmp_path, X360W_STATE_FILE);
}

static void
relay_game_rumble(X360WReceiver *rx, pid_t game_pid, intptr_t bridge_args,
                  uint64_t last_rumble_pub[X360W_MAX_CONTROLLERS])
{
    if (!rx || !rx->open || game_pid <= 0 || !bridge_args)
        return;
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        if (!rx->slots[s].connected)
            continue;
        PoorDS4GameOutputCommand cmd;
        uint64_t published = 0;
        if (wireless_ds4_game_rumble_request(game_pid, bridge_args, s,
                                             &cmd, &published) == 0 &&
            published > last_rumble_pub[s]) {
            last_rumble_pub[s] = published;
            (void)x360w_usb_set_rumble(rx, s,
                                       cmd.vibration.large_motor,
                                       cmd.vibration.small_motor);
        }
    }
}

static void
teardown_game_bridge(pid_t *game_pid, intptr_t *bridge_args, int suspend_safe)
{
    if (*game_pid > 0 && *bridge_args) {
        if (wireless_ds4_game_bridge_image_state(*game_pid) > 0) {
            (void)wireless_ds4_game_bridge_abandon();
        } else if (suspend_safe) {
            (void)wireless_ds4_game_bridge_quiesce(*game_pid, *bridge_args);
            (void)wireless_ds4_game_bridge_abandon();
        } else {
            (void)wireless_ds4_game_bridge_remove(*game_pid, *bridge_args);
        }
    }
    *game_pid = -1;
    *bridge_args = 0;
}

int
main(void)
{
    if (acquire_instance_lock() != 0) {
        klog_printf("[X360W] Another Xbox360Receiver instance is already running\n");
        return 0;
    }

    (void)unlink(X360W_STOP_FILE);
    (void)signal(SIGTERM, on_signal);
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGHUP, SIG_IGN);

    intptr_t info_flag = -1;
    if (sceKernelOpenEventFlag(&info_flag, "SceSystemStateMgrInfo") == 0)
        g_system_state_info_flag = info_flag;

    poords4_log("[X360W] Xbox 360 PC Wireless Gaming Receiver Bridge v%s started (pid=%d)\n",
                X360W_VERSION, (int)getpid());

    X360WReceiver rx;
    x360w_usb_init(&rx);

    int32_t ps5_users[X360W_MAX_CONTROLLERS] = {-1, -1, -1, -1};
    uint32_t user_count = collect_ps5_users(ps5_users);
    poords4_log("[X360W] Logged-in PS5 users=%u primary=0x%08x\n",
                user_count, (uint32_t)ps5_users[0]);

    pid_t game_pid = -1;
    intptr_t bridge_args = 0;
    uint8_t slot_published[X360W_MAX_CONTROLLERS] = {0, 0, 0, 0};
    uint64_t last_rumble_pub[X360W_MAX_CONTROLLERS] = {0, 0, 0, 0};
    uint64_t last_usb_scan_us = 0;
    uint64_t last_game_scan_us = 0;
    uint64_t last_status_us = 0;
    uint64_t last_keepalive_us = 0;
    int stop_reason = 0;

    while ((stop_reason = check_stop_or_suspend()) == 0) {
        uint64_t now_us = monotonic_us();

        /* 1. Maintain USB connection to the Xbox 360 Wireless Receiver. */
        if (!rx.open) {
            if (now_us - last_usb_scan_us >= UINT64_C(1000000)) {
                last_usb_scan_us = now_us;
                if (x360w_usb_open(&rx) == 0) {
                    poords4_log("[X360W] Receiver ready; waiting for wireless controllers\n");
                }
            }
        } else {
            int poll_rc = x360w_usb_poll(&rx, 4, now_us);
            if (poll_rc < 0) {
                poords4_log("[X360W] Receiver disconnected; closing USB handle\n");
                x360w_usb_close(&rx);
                if (game_pid > 0 && bridge_args) {
                    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
                        if (slot_published[s]) {
                            (void)wireless_ds4_game_bridge_deactivate_slot(
                                game_pid, bridge_args, s);
                            slot_published[s] = 0;
                        }
                    }
                }
            }
        }

        /* 2. Watch running PS5 game lifecycle and attach bridge when ready. */
        if (game_pid > 0 && bridge_args) {
            int img_state = wireless_ds4_game_bridge_image_state(game_pid);
            if (img_state > 0 || kill(game_pid, 0) != 0) {
                poords4_log("[X360W] Game pid=%d exited or changed image; detaching\n",
                            (int)game_pid);
                teardown_game_bridge(&game_pid, &bridge_args, 0);
                memset(slot_published, 0, sizeof(slot_published));
                memset(last_rumble_pub, 0, sizeof(last_rumble_pub));
            }
        }

        if (game_pid <= 0 &&
            now_us - last_game_scan_us >= UINT64_C(500000)) {
            last_game_scan_us = now_us;
            pid_t found_pid = -1;
            int find_rc = wireless_ds4_game_bridge_find_target(&found_pid);
            if (find_rc == 1 && found_pid > 0) {
                user_count = collect_ps5_users(ps5_users);
                if (user_count > 0u) {
                    wireless_ds4_set_external_users(ps5_users, user_count);
                    PoorDS4PadSource src;
                    src.user_id = ps5_users[0];
                    src.pad_index = 0;
                    src.pad_handle = -1;
                    src.ds4_connected = 1;
                    pid_t installed_pid = -1;
                    intptr_t installed_args = 0;
                    int rc = wireless_ds4_game_bridge_install(
                        &src, &installed_pid, &installed_args);
                    if (rc == 1 && installed_pid > 0 && installed_args != 0) {
                        game_pid = installed_pid;
                        bridge_args = installed_args;
                        memset(slot_published, 0, sizeof(slot_published));
                        memset(last_rumble_pub, 0, sizeof(last_rumble_pub));
                        poords4_log("[X360W] Installed PS5 pad bridge into game pid=%d args=0x%lx\n",
                                    (int)game_pid, (unsigned long)bridge_args);
                    } else {
                        poords4_log("[X360W] Bridge install pending/failed for pid=%d rc=%d\n",
                                    (int)found_pid, rc);
                    }
                }
            }
        }

        /* 3. Publish connected Xbox 360 wireless controllers to the PS5 game.
         * Refresh leases at ~120 Hz (every 8 ms) or immediately on new packets. */
        if (game_pid > 0 && bridge_args && rx.open) {
            int keepalive_due = (now_us - last_keepalive_us) >= UINT64_C(8000);
            if (keepalive_due)
                last_keepalive_us = now_us;

            for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
                if (rx.slots[s].connected) {
                    int ticked = x360w_slot_tick(&rx.slots[s], now_us);
                    if (keepalive_due || !slot_published[s] || ticked ||
                        rx.slots[s].last_packet_us == now_us) {
                        rx.slots[s].pad.timestamp = now_us;
                        if (wireless_ds4_game_bridge_update_slot(
                                game_pid, bridge_args, s,
                                &rx.slots[s].pad, sizeof(rx.slots[s].pad),
                                /*is_simulated=*/0, /*is_dualsense=*/0) == 0) {
                            if (!slot_published[s]) {
                                PoorDS4RumbleStatus rsrc;
                                memset(&rsrc, 0, sizeof(rsrc));
                                rsrc.session = 1u;
                                rsrc.epoch = 1u;
                                rsrc.handle = (int32_t)(s + 1u);
                                rsrc.user = (s < user_count) ? ps5_users[s] : ps5_users[0];
                                rsrc.connected = 1u;
                                (void)wireless_ds4_game_rumble_route(
                                    game_pid, bridge_args, s,
                                    (uint64_t)rx.slots[s].connect_count,
                                    &rsrc, 1);
                            }
                            slot_published[s] = 1u;
                        }
                    }
                } else if (slot_published[s]) {
                    (void)wireless_ds4_game_rumble_route(
                        game_pid, bridge_args, s, 0u, NULL, 0);
                    (void)wireless_ds4_game_bridge_deactivate_slot(
                        game_pid, bridge_args, s);
                    slot_published[s] = 0u;
                }
            }

            relay_game_rumble(&rx, game_pid, bridge_args, last_rumble_pub);
        }

        /* 4. Check live guitar-mode toggle and export status file. */
        if (now_us - last_status_us >= UINT64_C(1000000)) {
            last_status_us = now_us;
            int want_guitar = (access(X360W_GUITAR_FILE, F_OK) == 0) ? 1 : 0;
            if (want_guitar != x360w_get_guitar_mode()) {
                x360w_set_guitar_mode(want_guitar);
                poords4_log("[X360W] Guitar Hero remap mode %s\n",
                            want_guitar ? "ENABLED (A/B/Y/X/LB -> L2/L1/R1/R2/Cross)"
                                        : "DISABLED (standard gamepad)");
            }
            write_status_file(&rx, game_pid, bridge_args, user_count, ps5_users);
        }

        if (!rx.open)
            usleep(20000);
    }

    /* Clean shutdown: zero rumble on all connected controllers, detach bridge, close USB. */
    if (rx.open) {
        for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
            if (rx.slots[s].connected)
                (void)x360w_usb_set_rumble(&rx, s, 0u, 0u);
        }
    }
    teardown_game_bridge(&game_pid, &bridge_args, stop_reason == 2);
    x360w_usb_close(&rx);
    write_status_file(&rx, -1, 0, user_count, ps5_users);

    if (g_system_state_info_flag >= 0) {
        (void)sceKernelCloseEventFlag(g_system_state_info_flag);
        g_system_state_info_flag = -1;
    }

    poords4_log("[X360W] Supervisor exited cleanly (reason=%d)\n", stop_reason);
    release_instance_lock();
    return 0;
}
