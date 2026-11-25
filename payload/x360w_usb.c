/* SPDX-License-Identifier: GPL-3.0-or-later
 * PS5 FreeBSD ugen USB driver for the Xbox 360 PC Wireless Gaming Receiver.
 */
#include "x360w_usb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <ps5/kernel.h>
#endif

extern void poords4_log(const char *format, ...);

#define X360W_FS_ENDPOINT_COUNT (X360W_MAX_CONTROLLERS * 2u)

#if defined(__PROSPERO__)

typedef struct {
    int      elevated;
    uint64_t orig_authid;
    uint8_t  orig_caps[16];
} X360WPrivGuard;

static void
x360w_priv_enter(X360WPrivGuard *g)
{
    static const uint8_t full_caps[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    memset(g, 0, sizeof(*g));
    pid_t pid = getpid();
    g->orig_authid = kernel_get_ucred_authid(pid);
    if (!g->orig_authid)
        return;
    if (kernel_get_ucred_caps(pid, g->orig_caps) != 0)
        return;
    if (kernel_set_ucred_authid(pid, UINT64_C(0x4800000000010003)) != 0)
        return;
    if (kernel_set_ucred_caps(pid, full_caps) != 0) {
        (void)kernel_set_ucred_authid(pid, g->orig_authid);
        return;
    }
    g->elevated = 1;
}

static void
x360w_priv_leave(X360WPrivGuard *g)
{
    if (!g->elevated)
        return;
    pid_t pid = getpid();
    (void)kernel_set_ucred_caps(pid, g->orig_caps);
    (void)kernel_set_ucred_authid(pid, g->orig_authid);
    g->elevated = 0;
}

static int
x360w_open_elevated(const char *path, int flags)
{
    X360WPrivGuard g;
    x360w_priv_enter(&g);
    int fd = open(path, flags);
    int saved = errno;
    x360w_priv_leave(&g);
    errno = saved;
    return fd;
}

static int
x360w_ioctl_elevated(int fd, unsigned long req, void *arg)
{
    X360WPrivGuard g;
    x360w_priv_enter(&g);
    int rc = ioctl(fd, req, arg);
    int saved = errno;
    x360w_priv_leave(&g);
    errno = saved;
    return rc;
}

/* Inspect full USB configuration descriptor for vendor-specific Xbox 360
 * wireless interfaces (class 0xFF, subclass 0x5D, protocol 0x81). */
static int
x360w_match_config_desc(int ctl_fd)
{
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    struct usb_gen_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.ugd_data = buf;
    desc.ugd_maxlen = (uint16_t)sizeof(buf);
    desc.ugd_config_index = 0;

    if (x360w_ioctl_elevated(ctl_fd, USB_GET_FULL_DESC, &desc) != 0)
        return 0;

    size_t len = desc.ugd_actlen;
    if (len > sizeof(buf))
        len = sizeof(buf);

    unsigned matching_ifaces = 0;
    size_t pos = 0;
    while (pos + 2u <= len) {
        uint8_t blen = buf[pos];
        uint8_t btype = buf[pos + 1u];
        if (blen < 2u || pos + blen > len)
            break;
        /* USB Interface Descriptor (type 0x04, length >= 9) */
        if (btype == 0x04u && blen >= 9u) {
            uint8_t iclass = buf[pos + 5u];
            uint8_t isub = buf[pos + 6u];
            uint8_t iproto = buf[pos + 7u];
            if (iclass == X360W_USB_IFACE_CLASS &&
                isub == X360W_USB_IFACE_SUBCLASS &&
                iproto == X360W_USB_IFACE_PROTOCOL) {
                matching_ifaces++;
            }
        }
        pos += blen;
    }
    return matching_ifaces > 0u;
}

static int
x360w_try_setup_usb_fs(X360WReceiver *rx)
{
    struct usb_fs_endpoint *eps = calloc(X360W_FS_ENDPOINT_COUNT, sizeof(*eps));
    if (!eps)
        return -1;

    /* Claim controller interfaces 0, 2, 4, 6 (detach any existing driver first). */
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        int iface = (int)(s * 2u);
        (void)x360w_ioctl_elevated(rx->ctl_fd, USB_IFACE_DRIVER_DETACH, &iface);
        (void)x360w_ioctl_elevated(rx->ctl_fd, USB_CLAIM_INTERFACE, &iface);
    }

    struct usb_fs_init init_req;
    memset(&init_req, 0, sizeof(init_req));
    init_req.pEndpoints = eps;
    init_req.ep_index_max = (uint8_t)X360W_FS_ENDPOINT_COUNT;

    if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_INIT, &init_req) != 0) {
        free(eps);
        return -1;
    }

    /* Open 4 Interrupt IN endpoints (ep_index 0..3 -> 0x81, 0x83, 0x85, 0x87)
     * and 4 Interrupt OUT endpoints (ep_index 4..7 -> 0x01, 0x03, 0x05, 0x07). */
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        struct usb_fs_open open_in;
        memset(&open_in, 0, sizeof(open_in));
        open_in.max_bufsize = X360W_PACKET_MAX_LEN;
        open_in.max_frames = 1u;
        open_in.ep_index = (uint8_t)s;
        open_in.ep_no = rx->in_ep[s];
        if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_OPEN, &open_in) != 0) {
            struct usb_fs_uninit u = {0};
            (void)x360w_ioctl_elevated(rx->ctl_fd, USB_FS_UNINIT, &u);
            free(eps);
            return -1;
        }

        struct usb_fs_open open_out;
        memset(&open_out, 0, sizeof(open_out));
        open_out.max_bufsize = X360W_PACKET_MAX_LEN;
        open_out.max_frames = 1u;
        open_out.ep_index = (uint8_t)(X360W_MAX_CONTROLLERS + s);
        open_out.ep_no = rx->out_ep[s];
        if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_OPEN, &open_out) != 0) {
            struct usb_fs_uninit u = {0};
            (void)x360w_ioctl_elevated(rx->ctl_fd, USB_FS_UNINIT, &u);
            free(eps);
            return -1;
        }

        rx->in_ptr[s] = rx->in_buf[s];
        rx->in_len[s] = X360W_PACKET_MAX_LEN;
        eps[s].ppBuffer = &rx->in_ptr[s];
        eps[s].pLength = &rx->in_len[s];
        eps[s].nFrames = 1u;
        eps[s].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        eps[s].timeout = USB_FS_TIMEOUT_NONE;

        rx->out_ptr[s] = rx->out_buf[s];
        rx->out_len[s] = X360W_CMD_LEN;
        eps[X360W_MAX_CONTROLLERS + s].ppBuffer = &rx->out_ptr[s];
        eps[X360W_MAX_CONTROLLERS + s].pLength = &rx->out_len[s];
        eps[X360W_MAX_CONTROLLERS + s].nFrames = 1u;
        eps[X360W_MAX_CONTROLLERS + s].flags = 0u;
        eps[X360W_MAX_CONTROLLERS + s].timeout = 100u;
        rx->out_busy[s] = 0u;
    }

    rx->fs_endpoints = eps;
    rx->transport = X360W_TRANSPORT_USB_FS;

    /* Start continuous Interrupt IN polling on all 4 controller slots. */
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        struct usb_fs_start start_req;
        start_req.ep_index = (uint8_t)s;
        (void)x360w_ioctl_elevated(rx->ctl_fd, USB_FS_START, &start_req);
    }

    return 0;
}

static int
x360w_try_setup_dev_ep(X360WReceiver *rx)
{
    unsigned opened = 0;
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        char path[64];
        unsigned ep_num = (s * 2u) + 1u; /* 1, 3, 5, 7 */
        snprintf(path, sizeof(path), "/dev/usb/%u.%u.%u",
                 (unsigned)rx->bus, (unsigned)rx->addr, ep_num);
        int fd = x360w_open_elevated(path, O_RDWR | O_NONBLOCK);
        rx->ep_fd[s] = fd;
        if (fd >= 0) {
            int short_xfer = 1;
            int timeout_ms = 10;
            (void)x360w_ioctl_elevated(fd, USB_SET_RX_SHORT_XFER, &short_xfer);
            (void)x360w_ioctl_elevated(fd, USB_SET_RX_TIMEOUT, &timeout_ms);
            (void)x360w_ioctl_elevated(fd, USB_SET_TX_TIMEOUT, &timeout_ms);
            opened++;
        }
    }
    if (opened == 0u)
        return -1;
    rx->transport = X360W_TRANSPORT_DEV_EP;
    return 0;
}

static int g_usb_diag_logged = 0;

static int
x360w_probe_device_node(X360WReceiver *rx, uint8_t bus, uint8_t addr)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/usb/%u.%u.0",
             (unsigned)bus, (unsigned)addr);
    int fd = x360w_open_elevated(path, O_RDWR);
    if (fd < 0) {
        if (!g_usb_diag_logged && errno != ENOENT) {
            poords4_log("[X360W] probe %s open failed errno=%d\n", path, errno);
        }
        return -1;
    }

    uint16_t vid = 0, pid = 0;
    uint8_t dev_class = 0, dev_sub = 0, dev_proto = 0;
    uint8_t cfg_no = 1;
    struct usb_device_descriptor ddesc;
    memset(&ddesc, 0, sizeof(ddesc));
    if (x360w_ioctl_elevated(fd, USB_GET_DEVICE_DESC, &ddesc) == 0) {
        vid = (uint16_t)ddesc.idVendor[0] | ((uint16_t)ddesc.idVendor[1] << 8u);
        pid = (uint16_t)ddesc.idProduct[0] | ((uint16_t)ddesc.idProduct[1] << 8u);
        dev_class = ddesc.bDeviceClass;
        dev_sub = ddesc.bDeviceSubClass;
        dev_proto = ddesc.bDeviceProtocol;
        int cur_cfg = 0;
        if (x360w_ioctl_elevated(fd, USB_GET_CONFIG, &cur_cfg) == 0)
            cfg_no = (uint8_t)cur_cfg;
    } else {
        int desc_err = errno;
        struct usb_device_info udi;
        memset(&udi, 0, sizeof(udi));
        if (x360w_ioctl_elevated(fd, USB_GET_DEVICEINFO, &udi) != 0) {
            if (!g_usb_diag_logged) {
                poords4_log("[X360W] probe %s USB_GET_DEVICE_DESC err=%d USB_GET_DEVICEINFO err=%d\n",
                            path, desc_err, errno);
            }
            close(fd);
            return -1;
        }
        vid = udi.udi_vendorNo;
        pid = udi.udi_productNo;
        dev_class = udi.udi_class;
        dev_sub = udi.udi_subclass;
        dev_proto = udi.udi_protocol;
        cfg_no = udi.udi_config_no;
    }

    if (!g_usb_diag_logged) {
        poords4_log("[X360W] USB device %s vid=0x%04x pid=0x%04x class=0x%02x sub=0x%02x proto=0x%02x cfg=%u\n",
                    path, (unsigned)vid, (unsigned)pid,
                    (unsigned)dev_class, (unsigned)dev_sub,
                    (unsigned)dev_proto, (unsigned)cfg_no);
    }

    int match = x360w_is_known_vid_pid(vid, pid);
    if (!match)
        match = x360w_match_config_desc(fd);
    if (!match) {
        close(fd);
        return -1;
    }

    /* Ensure configuration 1 is active if the OS left the device unconfigured. */
    if (cfg_no == 0u) {
        int cfg = 0; /* config index 0 -> configuration 1 */
        int cfg_rc = x360w_ioctl_elevated(fd, USB_SET_CONFIG, &cfg);
        poords4_log("[X360W] USB_SET_CONFIG(0) rc=%d errno=%d\n", cfg_rc, cfg_rc ? errno : 0);
    }

    rx->ctl_fd = fd;
    rx->bus = bus;
    rx->addr = addr;
    rx->vendor_id = vid;
    rx->product_id = pid;
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        rx->in_ep[s] = (uint8_t)(0x81u + s * 2u);  /* 0x81, 0x83, 0x85, 0x87 */
        rx->out_ep[s] = (uint8_t)(0x01u + s * 2u); /* 0x01, 0x03, 0x05, 0x07 */
        rx->ep_fd[s] = -1;
        x360w_slot_init(&rx->slots[s], s);
    }

    if (x360w_try_setup_usb_fs(rx) != 0) {
        poords4_log("[X360W] USB_FS setup failed (errno=%d); trying dev_ep fallback\n", errno);
        if (x360w_try_setup_dev_ep(rx) != 0) {
            poords4_log("[X360W] dev_ep setup also failed (errno=%d)\n", errno);
            close(fd);
            rx->ctl_fd = -1;
            return -1;
        }
    }

    rx->open = 1;
    rx->fatal_errors = 0u;
    poords4_log("[X360W] Opened Xbox 360 Wireless Receiver at /dev/usb/%u.%u.0 "
                "vid=0x%04x pid=0x%04x transport=%s\n",
                (unsigned)bus, (unsigned)addr,
                (unsigned)rx->vendor_id, (unsigned)rx->product_id,
                rx->transport == X360W_TRANSPORT_USB_FS ? "usb_fs" : "dev_ep");

    /* Send presence inquiry to all 4 slots so already-synced pads respond. */
    uint8_t inquiry[X360W_CMD_LEN];
    x360w_build_inquiry_cmd(inquiry);
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s)
        (void)x360w_usb_send_cmd(rx, s, inquiry);

    return 0;
}

#endif /* __PROSPERO__ */

void
x360w_usb_init(X360WReceiver *rx)
{
    if (!rx)
        return;
    memset(rx, 0, sizeof(*rx));
    rx->ctl_fd = -1;
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        rx->ep_fd[s] = -1;
        x360w_slot_init(&rx->slots[s], s);
    }
}

int
x360w_usb_open(X360WReceiver *rx)
{
    if (!rx)
        return -1;
    if (rx->open)
        return 0;

#if !defined(__PROSPERO__)
    return -1;
#else
    /* First try listing /dev/usb for *.0 control nodes. */
    X360WPrivGuard g;
    x360w_priv_enter(&g);
    DIR *dir = opendir("/dev/usb");
    x360w_priv_leave(&g);

    if (dir) {
        if (!g_usb_diag_logged)
            poords4_log("[X360W] Scanning /dev/usb directory...\n");
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            unsigned bus = 0, addr = 0, ep = 0;
            if (sscanf(ent->d_name, "%u.%u.%u", &bus, &addr, &ep) == 3 &&
                ep == 0u && bus <= 255u && addr >= 1u && addr <= 255u) {
                if (x360w_probe_device_node(rx, (uint8_t)bus, (uint8_t)addr) == 0) {
                    closedir(dir);
                    return 0;
                }
            }
        }
        closedir(dir);
    } else {
        if (!g_usb_diag_logged)
            poords4_log("[X360W] opendir(/dev/usb) failed errno=%d; probing bus 0..7 addr 1..32\n", errno);
        /* Fallback: direct probe of common bus/address ranges. */
        for (unsigned bus = 0; bus < 8u; ++bus) {
            for (unsigned addr = 1; addr <= 32u; ++addr) {
                if (x360w_probe_device_node(rx, (uint8_t)bus, (uint8_t)addr) == 0)
                    return 0;
            }
        }
    }
    if (!g_usb_diag_logged) {
        poords4_log("[X360W] Initial USB scan complete: no Xbox 360 Wireless Receiver found yet\n");
        g_usb_diag_logged = 1;
    }
    return -1;
#endif
}

void
x360w_usb_close(X360WReceiver *rx)
{
    if (!rx)
        return;
#if defined(__PROSPERO__)
    if (rx->transport == X360W_TRANSPORT_USB_FS && rx->ctl_fd >= 0) {
        struct usb_fs_uninit u = {0};
        (void)x360w_ioctl_elevated(rx->ctl_fd, USB_FS_UNINIT, &u);
    }
    if (rx->fs_endpoints) {
        free(rx->fs_endpoints);
        rx->fs_endpoints = NULL;
    }
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        if (rx->ep_fd[s] >= 0) {
            close(rx->ep_fd[s]);
            rx->ep_fd[s] = -1;
        }
        x360w_slot_init(&rx->slots[s], s);
    }
    if (rx->ctl_fd >= 0) {
        close(rx->ctl_fd);
        rx->ctl_fd = -1;
    }
#endif
    rx->open = 0;
    rx->transport = X360W_TRANSPORT_NONE;
    rx->fatal_errors = 0u;
}

int
x360w_usb_send_cmd(X360WReceiver *rx, unsigned slot_index,
                   const uint8_t cmd[X360W_CMD_LEN])
{
    if (!rx || !rx->open || !cmd || slot_index >= X360W_MAX_CONTROLLERS)
        return -1;

#if !defined(__PROSPERO__)
    return -1;
#else
    if (rx->transport == X360W_TRANSPORT_USB_FS) {
        if (!rx->fs_endpoints || rx->out_busy[slot_index])
            return -1;
        struct usb_fs_endpoint *eps = (struct usb_fs_endpoint *)rx->fs_endpoints;
        unsigned out_idx = X360W_MAX_CONTROLLERS + slot_index;
        memcpy(rx->out_buf[slot_index], cmd, X360W_CMD_LEN);
        rx->out_len[slot_index] = X360W_CMD_LEN;
        eps[out_idx].nFrames = 1u;
        eps[out_idx].status = 0;
        struct usb_fs_start start_req;
        start_req.ep_index = (uint8_t)out_idx;
        if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_START, &start_req) != 0)
            return -1;
        rx->out_busy[slot_index] = 1u;
        return 0;
    }

    if (rx->transport == X360W_TRANSPORT_DEV_EP) {
        int fd = rx->ep_fd[slot_index];
        if (fd < 0)
            return -1;
        ssize_t written = write(fd, cmd, X360W_CMD_LEN);
        return (written == (ssize_t)X360W_CMD_LEN) ? 0 : -1;
    }

    return -1;
#endif
}

int
x360w_usb_set_rumble(X360WReceiver *rx, unsigned slot_index,
                     uint8_t large_motor, uint8_t small_motor)
{
    if (!rx || !rx->open || slot_index >= X360W_MAX_CONTROLLERS)
        return -1;
    if (!rx->slots[slot_index].connected)
        return 0;
    uint8_t cmd[X360W_CMD_LEN];
    x360w_build_rumble_cmd(cmd, large_motor, small_motor);
    int rc = x360w_usb_send_cmd(rx, slot_index, cmd);
    if (rc == 0) {
        rx->slots[slot_index].rumble_large = large_motor;
        rx->slots[slot_index].rumble_small = small_motor;
    }
    return rc;
}

int
x360w_usb_poll(X360WReceiver *rx, int timeout_ms, uint64_t now_us)
{
    if (!rx || !rx->open)
        return -1;

#if !defined(__PROSPERO__)
    (void)timeout_ms;
    (void)now_us;
    return -1;
#else
    int changed_mask = 0;

    if (rx->transport == X360W_TRANSPORT_USB_FS) {
        struct pollfd pfd;
        pfd.fd = rx->ctl_fd;
        pfd.events = POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM;
        pfd.revents = 0;
        int prc = poll(&pfd, 1, timeout_ms);
        if (prc < 0) {
            if (errno == EINTR)
                return 0;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;

        struct usb_fs_endpoint *eps = (struct usb_fs_endpoint *)rx->fs_endpoints;
        for (unsigned iter = 0; iter < 32u; ++iter) {
            struct usb_fs_complete comp;
            memset(&comp, 0, sizeof(comp));
            if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_COMPLETE, &comp) != 0) {
                if (errno == EBUSY || errno == EAGAIN)
                    break;
                return -1;
            }

            uint8_t idx = comp.ep_index;
            if (idx < X360W_MAX_CONTROLLERS) {
                int st = eps[idx].status;
                if (st == 0 && eps[idx].aFrames > 0u && rx->in_len[idx] > 0u) {
                    X360WPacketEvent ev = x360w_process_packet(
                        &rx->slots[idx], idx,
                        rx->in_buf[idx], rx->in_len[idx], now_us);
                    if (ev != X360W_PKT_IGNORED)
                        changed_mask |= (1 << idx);
                    rx->fatal_errors = 0u;
                } else if (st != 0) {
                    rx->fatal_errors++;
                    if (rx->fatal_errors >= 16u)
                        return -1;
                }

                /* Rearm Interrupt IN transfer immediately. */
                rx->in_len[idx] = X360W_PACKET_MAX_LEN;
                eps[idx].nFrames = 1u;
                eps[idx].aFrames = 0u;
                eps[idx].status = 0;
                struct usb_fs_start start_req;
                start_req.ep_index = idx;
                if (x360w_ioctl_elevated(rx->ctl_fd, USB_FS_START, &start_req) != 0)
                    return -1;
            } else if (idx < X360W_FS_ENDPOINT_COUNT) {
                rx->out_busy[idx - X360W_MAX_CONTROLLERS] = 0u;
            }
        }
    } else if (rx->transport == X360W_TRANSPORT_DEV_EP) {
        struct pollfd pfds[X360W_MAX_CONTROLLERS];
        nfds_t count = 0;
        unsigned map[X360W_MAX_CONTROLLERS];
        for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
            if (rx->ep_fd[s] >= 0) {
                pfds[count].fd = rx->ep_fd[s];
                pfds[count].events = POLLIN;
                pfds[count].revents = 0;
                map[count] = s;
                count++;
            }
        }
        if (count == 0u)
            return -1;
        int prc = poll(pfds, count, timeout_ms);
        if (prc < 0)
            return (errno == EINTR) ? 0 : -1;
        for (nfds_t i = 0; i < count; ++i) {
            unsigned s = map[i];
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                return -1;
            if (pfds[i].revents & POLLIN) {
                uint8_t buf[X360W_PACKET_MAX_LEN];
                ssize_t n = read(pfds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    X360WPacketEvent ev = x360w_process_packet(
                        &rx->slots[s], s, buf, (size_t)n, now_us);
                    if (ev != X360W_PKT_IGNORED)
                        changed_mask |= (1 << s);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    return -1;
                }
            }
        }
    }

    /* Dispatch any pending LED assignments for newly connected wireless pads. */
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        if (rx->slots[s].connected && rx->slots[s].led_pending) {
            uint8_t led_cmd[X360W_CMD_LEN];
            x360w_build_led_cmd(led_cmd, rx->slots[s].led_mode);
            if (x360w_usb_send_cmd(rx, s, led_cmd) == 0) {
                rx->slots[s].led_pending = 0u;
                poords4_log("[X360W] Slot %u connected -> assigned Player %u LED\n",
                            s, s + 1u);
            }
        }
    }

    return changed_mask;
#endif
}

unsigned
x360w_usb_connected_count(const X360WReceiver *rx)
{
    if (!rx || !rx->open)
        return 0u;
    unsigned n = 0u;
    for (unsigned s = 0; s < X360W_MAX_CONTROLLERS; ++s) {
        if (rx->slots[s].connected)
            n++;
    }
    return n;
}
