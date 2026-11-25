/* SPDX-License-Identifier: GPL-3.0-or-later
 * Prints current Xbox360Receiver bridge status to klog and stdout.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <ps5/klog.h>

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/x360w"
#endif

#define X360W_STATE_FILE POORDS4_DATA_DIR "/supervisor.txt"

int
main(void)
{
    int fd = open(X360W_STATE_FILE, O_RDONLY);
    if (fd < 0) {
        klog_printf("[X360W-status] Supervisor status file not found (%s)\n",
                    X360W_STATE_FILE);
        printf("status=not_running\n");
        return 1;
    }

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1u);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        klog_printf("[X360W-status]\n%s", buf);
        printf("%s", buf);
    }
    return 0;
}
