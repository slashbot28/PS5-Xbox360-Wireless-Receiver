/* SPDX-License-Identifier: GPL-3.0-or-later
 * Requests clean shutdown of the running Xbox360Receiver supervisor.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/klog.h>

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/x360w"
#endif

#define X360W_STOP_FILE POORDS4_DATA_DIR "/stop-bridge"

int
main(void)
{
    mkdir(POORDS4_DATA_DIR, 0755);
    int fd = open(X360W_STOP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        klog_printf("[X360W-stop] Failed to create %s\n", X360W_STOP_FILE);
        return 1;
    }
    close(fd);
    klog_printf("[X360W-stop] Stop request written to %s\n", X360W_STOP_FILE);
    return 0;
}
