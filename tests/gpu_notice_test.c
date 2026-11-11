#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gpu_notice.h"

static char base[256];
static int fails = 0;

static void put(const char *dev, const char *name, const char *value) {
    char path[512];
    snprintf(path, sizeof path, "%s/bus/pci/devices/%s/%s", base, dev, name);
    FILE *f = fopen(path, "w");
    fputs(value, f);
    fclose(f);
}

static void gpu(const char *dev, const char *vendor, const char *device, const char *driver, int card) {
    char path[512];
    snprintf(path, sizeof path, "mkdir -p %s/bus/pci/devices/%s %s/bus/pci/drivers", base, dev, base);
    if (system(path) != 0) exit(2);
    put(dev, "class", "0x030000\n");
    put(dev, "vendor", vendor);
    put(dev, "device", device);
    if (driver) {
        char target[512], link[512];
        snprintf(target, sizeof target, "../../../bus/pci/drivers/%s", driver);
        snprintf(link, sizeof link, "%s/bus/pci/devices/%s/driver", base, dev);
        if (symlink(target, link) != 0) exit(2);
    }
    if (card) {
        snprintf(path, sizeof path, "mkdir -p %s/bus/pci/devices/%s/drm/card0", base, dev);
        if (system(path) != 0) exit(2);
    }
}

static void reset(void) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s/bus", base);
    if (system(cmd) != 0) exit(2);
}

static void expect(const char *label, const char *needle) {
    const char *got = gpu_notice(base);
    int ok = needle ? got && strstr(got, needle) : got == NULL;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", label, ok ? "" : ": ", ok ? "" : (got ? got : "(none)"));
    if (!ok) fails++;
}

int main(void) {
    snprintf(base, sizeof base, "%s/gpu-notice-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : ".");
    if (!mkdtemp(base)) return 2;

    gpu("0000:01:00.0", "0x10de\n", "0x2182\n", "nouveau", 1);
    expect("GTX 1660 Ti on nouveau is fine", NULL);
    reset();

    gpu("0000:01:00.0", "0x10de\n", "0x2182\n", NULL, 0);
    expect("GTX 1660 Ti without driver", "without acceleration. Details");
    reset();

    gpu("0000:01:00.0", "0x10de\n", "0x2182\n", "nouveau", 0);
    expect("nouveau bound but no card, firmware missing", "without acceleration. Details");
    reset();

    gpu("0000:00:02.0", "0x8086\n", "0x3e92\n", "i915", 1);
    gpu("0000:01:00.0", "0x10de\n", "0x2520\n", NULL, 0);
    expect("hybrid laptop with Intel working", "other graphics card");
    reset();

    gpu("0000:01:00.0", "0x10de\n", "0x1b80\n", NULL, 0);
    expect("GTX 1080 without driver", "predates");
    reset();

    gpu("0000:00:02.0", "0x8086\n", "0x3e92\n", "i915", 1);
    expect("no NVIDIA card", NULL);
    reset();

    expect("no PCI tree", NULL);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (system(cmd) != 0) return 2;
    return fails ? 1 : 0;
}
