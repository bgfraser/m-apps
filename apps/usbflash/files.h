/* SPDX-License-Identifier: MIT */
/* The firmware files on the drive: .fwsc packages and .bin flash images in /, /FIRMWARE and /DOCK, in that order and
 * in name order within each folder. Portable: tests/usbflash_host.c runs it on a drive image. */
#ifndef USBFLASH_FILES_H
#define USBFLASH_FILES_H
#include "fat.h"
#include "package.h"
#include <stdbool.h>
#define FILES_MOST 40u

struct item {
    struct fat_entry file;
    enum pkg_kind kind;
    const char *folder;
    char name[FAT_NAME_BYTES];
};

struct file_list {
    struct item items[FILES_MOST];
    unsigned count;
    /* More files than fit. */
    bool more;
};

/* After fat_mount(). 0 on success, otherwise what to show. */
const char *files_scan(struct file_list *list);
#endif
