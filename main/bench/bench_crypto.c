/* Crypto benchmark: hashing, AES, RSA-size modular exponentiation, ECDSA and
 * the hardware RNG through mbedTLS (which uses the on-chip accelerators when
 * the build enables them), plus a small software SHA-256 so the hardware
 * speed-up can be quantified and the digests cross-checked. */
#include "bench.h"
#include "caps.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"

#define BUF_BYTES   (16 * 1024)
#define MIN_TIME_US 120000

static volatile uint32_t s_sink;

/* ------------------------------------------------- software SHA-256 (reference) */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sw_sha256_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

/* len must be a multiple of 64 here; the final padding block is handled below */
static void sw_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t full = len & ~(size_t)63;
    for (size_t i = 0; i < full; i += 64) sw_sha256_block(st, data + i);

    uint8_t tail[128] = { 0 };
    size_t rem = len - full;
    memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tail_len; i += 64) sw_sha256_block(st, tail + i);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = st[i] >> 24; out[i * 4 + 1] = st[i] >> 16; out[i * 4 + 2] = st[i] >> 8; out[i * 4 + 3] = st[i];
    }
}

/* ----------------------------------------------------------------- helpers */

/* Runs op() repeatedly for at least MIN_TIME_US; returns MB/s over `bytes` per call. */
static double throughput(void (*op)(void *), void *arg, size_t bytes)
{
    uint32_t iters = 0;
    int64_t t0 = bench_now_us(), dt;
    do {
        op(arg);
        iters++;
        dt = bench_now_us() - t0;
    } while (dt < MIN_TIME_US);
    vTaskDelay(1);
    return (double)bytes * iters / (double)dt;
}

static int rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

typedef struct {
    uint8_t *in;
    uint8_t *out;
    mbedtls_aes_context aes;
    mbedtls_gcm_context gcm;
    uint8_t iv[16];
} crypto_arg_t;

static void op_sha1(void *p)
{
    crypto_arg_t *a = p;
    uint8_t digest[20];
    mbedtls_sha1(a->in, BUF_BYTES, digest);
    s_sink += digest[0];
}

static void op_sha256(void *p)
{
    crypto_arg_t *a = p;
    uint8_t digest[32];
    mbedtls_sha256(a->in, BUF_BYTES, digest, 0);
    s_sink += digest[0];
}

static void op_sha512(void *p)
{
    crypto_arg_t *a = p;
    uint8_t digest[64];
    mbedtls_sha512(a->in, BUF_BYTES, digest, 0);
    s_sink += digest[0];
}

static void op_sw_sha256(void *p)
{
    crypto_arg_t *a = p;
    uint8_t digest[32];
    sw_sha256(a->in, 4096, digest);
    s_sink += digest[0];
}

static void op_aes_cbc(void *p)
{
    crypto_arg_t *a = p;
    uint8_t iv[16];
    memcpy(iv, a->iv, 16);
    mbedtls_aes_crypt_cbc(&a->aes, MBEDTLS_AES_ENCRYPT, BUF_BYTES, iv, a->in, a->out);
}

static void op_gcm(void *p)
{
    crypto_arg_t *a = p;
    uint8_t tag[16];
    mbedtls_gcm_crypt_and_tag(&a->gcm, MBEDTLS_GCM_ENCRYPT, BUF_BYTES, a->iv, 12, NULL, 0, a->in, a->out, 16, tag);
}

static void op_rng(void *p)
{
    crypto_arg_t *a = p;
    esp_fill_random(a->out, BUF_BYTES);
}

/* ------------------------------------------------------------------- tests */

static void hash_tests(bench_ctx_t *ctx, crypto_arg_t *a)
{
    bench_metric(ctx, "SHA-1", throughput(op_sha1, a, BUF_BYTES), "MB/s");
    double hw = throughput(op_sha256, a, BUF_BYTES);
    bench_metric(ctx, "SHA-256", hw, "MB/s");
    bench_metric(ctx, "SHA-512", throughput(op_sha512, a, BUF_BYTES), "MB/s");

    double sw = throughput(op_sw_sha256, a, 4096);
    bench_metric(ctx, "SHA-256 software reference", sw, "MB/s");
    bench_metric(ctx, "SHA-256 mbedTLS vs software", hw / sw, "x");

    uint8_t d_hw[32], d_sw[32];
    mbedtls_sha256(a->in, BUF_BYTES - 5, d_hw, 0); /* odd length exercises padding */
    sw_sha256(a->in, BUF_BYTES - 5, d_sw);
    bool same = memcmp(d_hw, d_sw, 32) == 0;
    bench_text(ctx, "SHA-256 digest cross-check", "%s", same ? "PASS" : "FAIL");
    if (!same) bench_fail(ctx, "mbedTLS SHA-256 digest differs from the software reference");
}

static void aes_tests(bench_ctx_t *ctx, crypto_arg_t *a)
{
    uint8_t key[32];
    esp_fill_random(key, sizeof(key));
    esp_fill_random(a->iv, sizeof(a->iv));

    mbedtls_aes_init(&a->aes);
    mbedtls_aes_setkey_enc(&a->aes, key, 128);
    bench_metric(ctx, "AES-128-CBC encrypt", throughput(op_aes_cbc, a, BUF_BYTES), "MB/s");
    mbedtls_aes_setkey_enc(&a->aes, key, 256);
    bench_metric(ctx, "AES-256-CBC encrypt", throughput(op_aes_cbc, a, BUF_BYTES), "MB/s");
    mbedtls_aes_free(&a->aes);

    mbedtls_gcm_init(&a->gcm);
    if (mbedtls_gcm_setkey(&a->gcm, MBEDTLS_CIPHER_ID_AES, key, 128) == 0) {
        bench_metric(ctx, "AES-128-GCM encrypt", throughput(op_gcm, a, BUF_BYTES), "MB/s");
    }
    mbedtls_gcm_free(&a->gcm);

    /* Known-answer test (FIPS-197 appendix C.1) */
    static const uint8_t k[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t out[16];
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    mbedtls_aes_setkey_enc(&c, k, 128);
    mbedtls_aes_crypt_ecb(&c, MBEDTLS_AES_ENCRYPT, pt, out);
    mbedtls_aes_free(&c);
    bool ok = memcmp(out, ct, 16) == 0;
    bench_text(ctx, "AES known-answer test", "%s", ok ? "PASS" : "FAIL");
    if (!ok) bench_fail(ctx, "AES-128 known-answer test failed");
}

static void rsa_test(bench_ctx_t *ctx)
{
#if SOC_MPI_SUPPORTED && CONFIG_MBEDTLS_HARDWARE_MPI
    const size_t bits = 2048;
    const int rounds = 3;
#else
    /* Software big-number math is far slower, keep the run short. */
    const size_t bits = 1024;
    const int rounds = 1;
#endif
    const size_t len = bits / 8;
    uint8_t *tmp = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    if (!tmp) return;

    mbedtls_mpi N, A, E, X, RR;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&A); mbedtls_mpi_init(&E); mbedtls_mpi_init(&X); mbedtls_mpi_init(&RR);

    esp_fill_random(tmp, len);
    tmp[0] |= 0x80; tmp[len - 1] |= 1;
    mbedtls_mpi_read_binary(&N, tmp, len);
    esp_fill_random(tmp, len);
    tmp[0] &= 0x7F;
    mbedtls_mpi_read_binary(&A, tmp, len);
    esp_fill_random(tmp, len);
    tmp[0] &= 0x7F;
    mbedtls_mpi_read_binary(&E, tmp, len);

    int64_t t0 = bench_now_us();
    int rc = 0;
    for (int i = 0; i < rounds && rc == 0; i++) {
        rc = mbedtls_mpi_exp_mod(&X, &A, &E, &N, &RR);
        vTaskDelay(1);
    }
    int64_t dt = bench_now_us() - t0;
    if (rc == 0) {
        char name[48];
        snprintf(name, sizeof(name), "RSA-%u private-size modexp", (unsigned)bits);
        bench_metric(ctx, name, dt / 1000.0 / rounds, "ms/op");
    } else {
        bench_fail(ctx, "mbedtls_mpi_exp_mod failed (%d)", rc);
    }
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&A); mbedtls_mpi_free(&E); mbedtls_mpi_free(&X); mbedtls_mpi_free(&RR);
    heap_caps_free(tmp);
}

static void ecdsa_test(bench_ctx_t *ctx)
{
    mbedtls_ecdsa_context ec;
    mbedtls_ecdsa_init(&ec);
    uint8_t hash[32], sig[MBEDTLS_ECDSA_MAX_LEN];
    size_t slen = 0;
    esp_fill_random(hash, sizeof(hash));

    int64_t t0 = bench_now_us();
    int rc = mbedtls_ecdsa_genkey(&ec, MBEDTLS_ECP_DP_SECP256R1, rng, NULL);
    int64_t t_gen = bench_now_us() - t0;
    if (rc != 0) {
        bench_fail(ctx, "ECDSA key generation failed (-0x%04X)", -rc);
        mbedtls_ecdsa_free(&ec);
        return;
    }
    vTaskDelay(1);

    t0 = bench_now_us();
    rc = mbedtls_ecdsa_write_signature(&ec, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &slen, rng, NULL);
    int64_t t_sign = bench_now_us() - t0;
    vTaskDelay(1);

    t0 = bench_now_us();
    int vrc = rc == 0 ? mbedtls_ecdsa_read_signature(&ec, hash, sizeof(hash), sig, slen) : -1;
    int64_t t_verify = bench_now_us() - t0;

    bench_metric(ctx, "ECDSA P-256 key generation", t_gen / 1000.0, "ms");
    if (rc == 0) bench_metric(ctx, "ECDSA P-256 sign", t_sign / 1000.0, "ms");
    if (vrc == 0) bench_metric(ctx, "ECDSA P-256 verify", t_verify / 1000.0, "ms");
    bench_text(ctx, "ECDSA sign/verify round trip", "%s", (rc == 0 && vrc == 0) ? "PASS" : "FAIL");
    if (rc != 0 || vrc != 0) bench_fail(ctx, "ECDSA round trip failed (sign %d, verify %d)", rc, vrc);
    mbedtls_ecdsa_free(&ec);
}

/* Not built on CAP_DEFINED(): a macro argument is expanded before it reaches an
 * inner macro, which would defeat the "is it defined?" stringify check. */
#define YN(m) (__builtin_strcmp(#m, CAPS_STR(m)) != 0 ? "y" : "n")

static void acceleration_summary(bench_ctx_t *ctx)
{
    bench_text(ctx, "Hardware blocks present", "AES:%s SHA:%s RSA/MPI:%s ECC:%s HMAC:%s DS:%s",
               YN(SOC_AES_SUPPORTED), YN(SOC_SHA_SUPPORTED), YN(SOC_MPI_SUPPORTED),
               YN(SOC_ECC_SUPPORTED), YN(SOC_HMAC_SUPPORTED), YN(SOC_DIG_SIGN_SUPPORTED));
    bench_text(ctx, "mbedTLS uses hardware (this build)", "AES:%s SHA:%s MPI:%s ECC:%s",
               YN(CONFIG_MBEDTLS_HARDWARE_AES), YN(CONFIG_MBEDTLS_HARDWARE_SHA),
               YN(CONFIG_MBEDTLS_HARDWARE_MPI), YN(CONFIG_MBEDTLS_HARDWARE_ECC));
    bench_text(ctx, "Flash encryption", "%s", esp_flash_encryption_enabled() ? "enabled" : "disabled");
    bench_text(ctx, "Secure boot", "%s", esp_secure_boot_enabled() ? "enabled" : "disabled");
}

static void run(bench_ctx_t *ctx)
{
    acceleration_summary(ctx);

    crypto_arg_t *a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_8BIT);
    uint8_t *in = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint8_t *out = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!a || !in || !out) {
        bench_fail(ctx, "out of memory");
        goto cleanup;
    }
    a->in = in;
    a->out = out;
    esp_fill_random(in, BUF_BYTES);

    hash_tests(ctx, a);
    aes_tests(ctx, a);
    rsa_test(ctx);
    ecdsa_test(ctx);

    bench_metric(ctx, "Hardware RNG (esp_fill_random)", throughput(op_rng, a, BUF_BYTES), "MB/s");
    unsigned ones = 0;
    for (int i = 0; i < 4096; i++) ones += __builtin_popcount(out[i]);
    bench_metric(ctx, "RNG bit balance (ideal 0.500)", ones / (4096.0 * 8.0), "");
    bench_note(ctx, "Hardware RNG is only truly random while Wi-Fi/Bluetooth is active.");

cleanup:
    heap_caps_free(a);
    heap_caps_free(in);
    heap_caps_free(out);
}

const bench_def_t bench_def_crypto = {
    .id = "crypto",
    .name = "Crypto accelerators",
    .group = "Security",
    .desc = "SHA, AES (CBC/GCM), RSA-size modexp, ECDSA P-256 and the hardware RNG, with software SHA-256 reference and known-answer checks.",
    .available = NULL,
    .run = run,
};
