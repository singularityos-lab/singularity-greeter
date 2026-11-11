#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpu_notice.h"

static unsigned long read_hex(const char *dir, const char *name) {
    char path[512], buf[32] = "";
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) buf[0] = '\0';
    fclose(f);
    return strtoul(buf, NULL, 16);
}

static void driver_of(const char *dir, char *out, size_t len) {
    char path[512], link[512];
    snprintf(path, sizeof path, "%s/driver", dir);
    ssize_t n = readlink(path, link, sizeof link - 1);
    out[0] = '\0';
    if (n <= 0) return;
    link[n] = '\0';
    const char *base = strrchr(link, '/');
    snprintf(out, len, "%s", base ? base + 1 : link);
}

static bool has_card(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/drm", dir);
    DIR *d = opendir(path);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strncmp(e->d_name, "card", 4) == 0) found = true;
    closedir(d);
    return found;
}

const char *gpu_notice(const char *sysfs) {
    char root[512];
    snprintf(root, sizeof root, "%s/bus/pci/devices", sysfs);
    DIR *d = opendir(root);
    if (!d) return NULL;
    bool other_works = false, broken = false, broken_modern = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[768];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        if ((read_hex(dir, "class") >> 16) != 0x03) continue;
        bool card = has_card(dir);
        if (read_hex(dir, "vendor") != 0x10de) {
            if (card) other_works = true;
            continue;
        }
        char driver[64];
        driver_of(dir, driver, sizeof driver);
        bool started = card && (strcmp(driver, "nouveau") == 0 || strcmp(driver, "nvidia") == 0);
        if (started) continue;
        broken = true;
        if (read_hex(dir, "device") >= 0x1e00) broken_modern = true;
    }
    closedir(d);
    if (!broken) return NULL;
    if (other_works) return "The NVIDIA graphics card did not start, so this computer uses its other graphics card. Details are in Logs.";
    if (broken_modern) return "The NVIDIA graphics card did not start, so the screen runs without acceleration. Details are in Logs.";
    return "This NVIDIA graphics card predates the GeForce 16 and RTX series and did not start, so the screen runs without acceleration.";
}
