/*
 * plugin.c
 *
 * Skeleton PS3 HEN plugin (concept):
 * - Hook USB read calls directed at a Skylanders portal.
 * - Present a chosen figure dump (read/write) instead of the real portal.
 * - Provide an in-game button combo to toggle the fake figure or cycle figures.
 *
 * !!! IMPORTANT !!!
 * - This is a reference skeleton. Hook-installation and exact syscall names
 *   must be implemented per your hooking technique (HEN/CFW hooking lib,
 *   game EBOOT patch, or prx injection library).
 * - Always test offline. Backup files before running anything that modifies saves.
 *
 * Build: use PSL1GHT or your preferred PS3 SDK/toolchain.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ppu-types.h>
#include <ppu-threads.h>
#include <sys/memory.h>
#include <sys/timer.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* --- PLACEHOLDERS YOU MUST SET --- */

/* Portal USB Vendor/Product ID (placeholder) */
#define PORTAL_VENDOR_ID  0x1234  /* REPLACE with real vendor id */
#define PORTAL_PRODUCT_ID 0x5678  /* REPLACE with real product id */

/* Path on PS3 where dumps are stored (placeholder) */
#define DUMP_FILE_PATH "/dev_hdd0/tmp/sky_figure_dump.bin"

/* Button combo to toggle emulation (placeholder: L3+R3+START) */
#define BTN_TOGGLE_L3  (1<<0)  /* replace bits according to pad API */
#define BTN_TOGGLE_R3  (1<<1)
#define BTN_TOGGLE_START (1<<2)

#define MAX_DUMP_SIZE (8192) /* typical small NFC dump — adjust to actual size */

/* --- Global state --- */
static int plugin_running = 0;
static int emulation_enabled = 1; /* start enabled by default */
static uint8_t *figure_dump = NULL;
static size_t figure_dump_size = 0;

/* Thread handles */
static sys_ppu_thread_t poll_thread = -1;

/* Forward declarations for hooking functions (implement per your env) */
int install_usb_hook(void);
int remove_usb_hook(void);

/* --- Utilities: file IO for dump (read/write) --- */

/* load_dump_from_disk: loads file into figure_dump buffer */
static int load_dump_from_disk(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_DUMP_SIZE) {
        fclose(f);
        return -2;
    }

    figure_dump = (uint8_t*)malloc(sz);
    if (!figure_dump) {
        fclose(f);
        return -3;
    }

    fread(figure_dump, 1, sz, f);
    fclose(f);
    figure_dump_size = (size_t)sz;
    return 0;
}

/* save_dump_to_disk: writes current buffer back to disk */
static int save_dump_to_disk(const char *path) {
    if (!figure_dump || figure_dump_size == 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fwrite(figure_dump, 1, figure_dump_size, f);
    fclose(f);
    return 0;
}

/* create_default_dump: simple fallback fill (for testing) */
static void create_default_dump(void) {
    if (figure_dump) free(figure_dump);
    figure_dump_size = 512; /* example size — set to real dump size */
    figure_dump = (uint8_t*)malloc(figure_dump_size);
    if (!figure_dump) return;
    memset(figure_dump, 0xAA, figure_dump_size); /* placeholder content */
    /* set a minimal expected header / ID so the game recognizes an actual figure */
    figure_dump[0] = 0x53; /* arbitrary */
}

/* --- USB read/write hook (conceptual) ---
 * This is the function you will register in place of the real USB read handler.
 *
 * The exact signature depends on the function you're hooking. Typical parameters:
 * - device handle / endpoint
 * - buffer pointer
 * - length
 * - timeout
 *
 * You must adapt the signature and hooking registration to the real target.
 */
typedef int (*real_usb_read_t)(int dev, void *buf, int len, int timeout);
static real_usb_read_t real_usb_read = NULL;

/* Example usb_read_hook: intercepts reads targeting portal VID/PID and returns dump */
int usb_read_hook(int dev_handle, void *buf, int len, int timeout) {
    /* Step 1: Check if emulation enabled */
    if (!emulation_enabled) {
        /* call original USB read (perform actual device I/O) */
        if (real_usb_read) return real_usb_read(dev_handle, buf, len, timeout);
        return -1; /* or appropriate error */
    }

    /* Step 2: Determine whether 'dev_handle' corresponds to the portal
     * This requires a mapping from dev_handle -> device descriptors.
     * Replace this check with your code that inspects the device's VID/PID.
     */
    int is_portal = 0;
    /* TODO: replace with actual check e.g., get_usb_device_vidpid(dev_handle) */
    /* Example pseudo-check:
     *    uint16_t vid = get_device_vendor(dev_handle);
     *    uint16_t pid = get_device_product(dev_handle);
     *    is_portal = (vid == PORTAL_VENDOR_ID && pid == PORTAL_PRODUCT_ID);
     */

    if (!is_portal) {
        /* not the portal: fall back to real USB behavior */
        if (real_usb_read) return real_usb_read(dev_handle, buf, len, timeout);
        return -1;
    }

    /* Step 3: Serve requested data from the loaded dump buffer:
     * The game may read various endpoints/offsets. You probably need to
     * implement logic to respond to the exact USB packet/frame the game expects.
     * For the skeleton, we just copy from the start of the dump.
     */

    if (!figure_dump || figure_dump_size == 0) {
        /* no dump loaded: optionally call original or return zeros */
        memset(buf, 0, len);
        return len; /* pretend we read 'len' bytes */
    }

    /* If the game requests more than the dump size, pad with zeros */
    size_t copy_sz = (len <= (int)figure_dump_size) ? len : figure_dump_size;
    memcpy(buf, figure_dump, copy_sz);
    if ((int)copy_sz < len) memset((uint8_t*)buf + copy_sz, 0, len - copy_sz);

    /* Return the number of bytes read */
    return (int)len;
}

/* Example usb_write_hook: intercept writes and persist into dump buffer */
typedef int (*real_usb_write_t)(int dev, const void *buf, int len, int timeout);
static real_usb_write_t real_usb_write = NULL;

int usb_write_hook(int dev_handle, const void *buf, int len, int timeout) {
    /* Similar device identity check as read hook */
    int is_portal = 0;
    /* TODO: detect portal device from dev_handle */

    if (!is_portal) {
        if (real_usb_write) return real_usb_write(dev_handle, buf, len, timeout);
        return -1;
    }

    /* Example: append or write into figure_dump at appropriate offset.
     * The actual write semantics depend on the portal protocol.
     * Here we simply overwrite the first len bytes.
     */
    if (!figure_dump) {
        /* allocate if needed */
        figure_dump = (uint8_t*)malloc(MAX_DUMP_SIZE);
        memset(figure_dump, 0, MAX_DUMP_SIZE);
        figure_dump_size = MAX_DUMP_SIZE;
    }

    size_t write_sz = (len <= (int)figure_dump_size) ? len : figure_dump_size;
    memcpy(figure_dump, buf, write_sz);

    /* Persist to disk */
    save_dump_to_disk(DUMP_FILE_PATH);

    /* Return bytes written */
    return (int)len;
}

/* --- Simple pad polling thread to detect button combo --- */
static void pad_poll_thread(uint64_t arg) {
    (void)arg;
    while (plugin_running) {
        /* Placeholder: replace with real pad polling API for the PS3 */
        /* Example pseudo call: uint32_t btn = pad_get_buttons(0); */
        uint32_t btn = 0; /* TODO: set actual read */

        /* Detect combo (placeholder bits) */
        if ((btn & (BTN_TOGGLE_L3 | BTN_TOGGLE_R3 | BTN_TOGGLE_START)) ==
            (BTN_TOGGLE_L3 | BTN_TOGGLE_R3 | BTN_TOGGLE_START)) {

            /* Debounce */
            sys_time_sleep(200);

            /* Toggle emulation */
            emulation_enabled = !emulation_enabled;

            /* Provide an audible beep or console log if desired (placeholder) */
            /* e.g., sys_speaker_beep(1); */
        }

        /* Sleep small amount to avoid busy loop */
        sys_time_sleep(50);
    }
    sys_ppu_thread_exit(0);
}

/* --- Module start/stop (plugin entry points) --- */

/* start_plugin: set up dump, install hooks, start poll thread */
int start_plugin(void) {
    int rc;

    /* Attempt to load dump; if missing, create default */
    rc = load_dump_from_disk(DUMP_FILE_PATH);
    if (rc != 0) {
        create_default_dump();
        /* we should also save the default for persistence */
        save_dump_to_disk(DUMP_FILE_PATH);
    }

    /* Install USB hooks (replace with real hooking code) */
    if (install_usb_hook() != 0) {
        /* If we cannot hook, abort start */
        return -1;
    }

    /* Start pad polling thread */
    plugin_running = 1;
    sys_ppu_thread_create(&poll_thread, pad_poll_thread, 0, 0x10000, 0x20, "pad_poll", 0);

    return 0;
}

/* stop_plugin: cleanup */
int stop_plugin(void) {
    /* stop thread */
    plugin_running = 0;
    if (poll_thread != -1) {
        sys_ppu_thread_join(poll_thread, NULL);
        poll_thread = -1;
    }

    /* Remove hooks */
    remove_usb_hook();

    /* Save dump once more */
    save_dump_to_disk(DUMP_FILE_PATH);

    /* free buffer */
    if (figure_dump) {
        free(figure_dump);
        figure_dump = NULL;
        figure_dump_size = 0;
    }

    return 0;
}

/* --- Hook installation placeholders ---
 * These functions must be implemented to:
 * 1) Find the symbol/address of the real USB read/write functions (e.g., in game binary)
 * 2) Save original pointers in `real_usb_read` / `real_usb_write`
 * 3) Replace them with usb_read_hook / usb_write_hook
 *
 * How to implement:
 * - Use your hooking library or HEN-shared hooking method
 * - On HEN you may use function-patching (write a branch at the function prologue)
 * - Or use VTable / import table patching depending on the target binary
 */

int install_usb_hook(void) {
    /* TODO: replace with actual implementation */
    /* Example pseudo:
     *   real_usb_read = (real_usb_read_t)lookup_symbol("sys_usb_read");
     *   if (real_usb_read) patch_function(real_usb_read, usb_read_hook);
     *   ...
     */
    return 0; /* return 0 for success in skeleton */
}

int remove_usb_hook(void) {
    /* TODO: restore originals */
    return 0;
}

/* Entrypoints expected by many plugin loaders; adapt names to your loader */
int module_start(uint64_t arg) {
    (void)arg;
    start_plugin();
    return 0;
}

int module_stop(void) {
    stop_plugin();
    return 0;
}
