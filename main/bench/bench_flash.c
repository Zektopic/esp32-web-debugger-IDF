/* Flash benchmark: chip identification, raw and memory-mapped read speed,
 * sector erase / page program speed with read-back verification (on the
 * dedicated `bench` partition only) and file-system throughput. */
#include "bench.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_random.h"

#define SECTOR 4096
#define BENCH_SUBTYPE ((esp_partition_subtype_t)0x40)

static const esp_partition_t *find_bench(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, BENCH_SUBTYPE, "bench");
}

static void chip_info(bench_ctx_t *ctx)
{
    uint32_t id = 0, phys = 0;
    esp_flash_read_id(NULL, &id);
    esp_flash_get_physical_size(NULL, &phys);
    bench_text(ctx, "JEDEC id", "0x%06X (mfr 0x%02X)", (unsigned)id, (unsigned)(id >> 16) & 0xFF);
    bench_metric(ctx, "Physical size", phys / (1024.0 * 1024.0), "MB");
    bench_text(ctx, "Firmware flash config", "%s, %s, %s", CONFIG_ESPTOOLPY_FLASHSIZE,
               CONFIG_ESPTOOLPY_FLASHMODE, CONFIG_ESPTOOLPY_FLASHFREQ);
    if (phys > 2u * 1024 * 1024 && strcmp(CONFIG_ESPTOOLPY_FLASHSIZE, "2MB") == 0) {
        bench_note(ctx, "Chip is larger than the 2 MB image configuration; the extra space is unused.");
    }
}

static void read_tests(bench_ctx_t *ctx)
{
    const esp_partition_t *app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (!app) {
        bench_note(ctx, "No app partition found for the read test.");
        return;
    }
    size_t total = app->size < 256 * 1024 ? app->size : 256 * 1024;
    uint8_t *buf = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;

    int64_t t0 = bench_now_us();
    for (size_t off = 0; off < total; off += SECTOR) {
        if (esp_partition_read(app, off, buf, SECTOR) != ESP_OK) {
            bench_fail(ctx, "raw read failed at offset 0x%X", (unsigned)off);
            break;
        }
    }
    int64_t dt = bench_now_us() - t0;
    bench_metric(ctx, "Raw read (esp_flash_read)", (double)total / (double)dt, "MB/s");
    heap_caps_free(buf);

    /* Cache-mapped access, as used when executing/reading constants from flash. */
    const void *ptr = NULL;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(app, 0, total, ESP_PARTITION_MMAP_DATA, &ptr, &h) == ESP_OK) {
        const volatile uint32_t *w = ptr;
        uint32_t acc = 0;
        t0 = bench_now_us();
        for (size_t i = 0; i < total / 4; i++) acc += w[i];
        dt = bench_now_us() - t0;
        (void)acc;
        bench_metric(ctx, "Memory-mapped read (cached)", (double)total / (double)dt, "MB/s");
        esp_partition_munmap(h);
    }
}

static void write_tests(bench_ctx_t *ctx)
{
    const esp_partition_t *part = find_bench();
    if (!part) {
        bench_note(ctx, "No `bench` partition in the partition table: erase/write test skipped.");
        return;
    }
    size_t size = (size_t)bench_param_int(ctx, "size_kb", 64) * 1024;
    size = (size / SECTOR) * SECTOR;
    if (size < SECTOR) size = SECTOR;
    if (size > part->size) size = part->size;

    uint8_t *buf = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *chk = heap_caps_malloc(SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf || !chk) {
        heap_caps_free(buf);
        heap_caps_free(chk);
        return;
    }
    esp_fill_random(buf, SECTOR);

    int64_t t0 = bench_now_us();
    esp_err_t err = esp_partition_erase_range(part, 0, size);
    int64_t t_erase = bench_now_us() - t0;
    if (err != ESP_OK) {
        bench_fail(ctx, "erase failed: %s", esp_err_to_name(err));
        goto done;
    }
    bench_metric(ctx, "Erase", (double)size / 1024.0 / ((double)t_erase / 1e6), "KB/s");
    bench_metric(ctx, "Erase time per 4 KB sector", (double)t_erase / 1000.0 / (size / SECTOR), "ms");

    t0 = bench_now_us();
    for (size_t off = 0; off < size; off += SECTOR) {
        buf[0] = (uint8_t)(off >> 12); /* make every sector unique */
        err = esp_partition_write(part, off, buf, SECTOR);
        if (err != ESP_OK) {
            bench_fail(ctx, "write failed at 0x%X: %s", (unsigned)off, esp_err_to_name(err));
            goto done;
        }
    }
    int64_t t_write = bench_now_us() - t0;
    bench_metric(ctx, "Write (page program)", (double)size / 1024.0 / ((double)t_write / 1e6), "KB/s");

    size_t bad = 0;
    t0 = bench_now_us();
    for (size_t off = 0; off < size; off += SECTOR) {
        buf[0] = (uint8_t)(off >> 12);
        if (esp_partition_read(part, off, chk, SECTOR) != ESP_OK || memcmp(buf, chk, SECTOR) != 0) bad++;
    }
    int64_t t_read = bench_now_us() - t0;
    bench_metric(ctx, "Read back", (double)size / 1024.0 / ((double)t_read / 1e6), "KB/s");
    bench_text(ctx, "Verify", "%s (%u sectors, %u bad)", bad ? "FAIL" : "PASS", (unsigned)(size / SECTOR), (unsigned)bad);
    if (bad) bench_fail(ctx, "%u sector(s) failed verification", (unsigned)bad);

    esp_partition_erase_range(part, 0, size); /* leave the scratch area clean */
done:
    heap_caps_free(buf);
    heap_caps_free(chk);
}

static void fs_tests(bench_ctx_t *ctx)
{
    const size_t total = 8 * 1024;
    uint8_t *buf = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;
    memset(buf, 0xA5, 1024);

    FILE *f = fopen("/spiffs/.bench.tmp", "wb");
    if (!f) {
        bench_note(ctx, "SPIFFS is not mounted or not writable: file-system test skipped.");
        heap_caps_free(buf);
        return;
    }
    int64_t t0 = bench_now_us();
    for (size_t i = 0; i < total; i += 1024) fwrite(buf, 1, 1024, f);
    fclose(f);
    int64_t dt = bench_now_us() - t0;
    bench_metric(ctx, "SPIFFS write", total / 1024.0 / ((double)dt / 1e6), "KB/s");

    f = fopen("/spiffs/.bench.tmp", "rb");
    if (f) {
        t0 = bench_now_us();
        while (fread(buf, 1, 1024, f) > 0) { }
        dt = bench_now_us() - t0;
        fclose(f);
        bench_metric(ctx, "SPIFFS read", total / 1024.0 / ((double)dt / 1e6), "KB/s");
    }
    unlink("/spiffs/.bench.tmp");
    heap_caps_free(buf);
}

static void run(bench_ctx_t *ctx)
{
    chip_info(ctx);
    read_tests(ctx);
    write_tests(ctx);
    fs_tests(ctx);
}

const bench_def_t bench_def_flash = {
    .id = "flash",
    .name = "Flash",
    .group = "Memory",
    .desc = "Flash identification, read speed (raw and cache-mapped), erase/program/verify on a scratch partition and SPIFFS throughput.",
    .available = NULL,
    .run = run,
};
