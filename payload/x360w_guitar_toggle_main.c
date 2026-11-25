/* SPDX-License-Identifier: GPL-3.0-or-later
 * Toggles Guitar Hero fret-to-gamepad remap mode for Xbox360Receiver.elf.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/klog.h>

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/x360w"
#endif

#define X360W_GUITAR_FILE POORDS4_DATA_DIR "/guitar-mode"

int
main(void)
{
    mkdir(POORDS4_DATA_DIR, 0755);
    if (access(X360W_GUITAR_FILE, F_OK) == 0) {
        (void)unlink(X360W_GUITAR_FILE);
        klog_printf("[X360W-guitar] Guitar Hero remap mode DISABLED\n");
        printf("guitar_mode=0\n");
    } else {
        int fd = open(X360W_GUITAR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            close(fd);
        klog_printf("[X360W-guitar] Guitar Hero remap mode ENABLED\n");
        printf("guitar_mode=1\n");
    }
    return 0;
}
