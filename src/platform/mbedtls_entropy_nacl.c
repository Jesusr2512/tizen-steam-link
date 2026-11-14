/*
 * mbedTLS hardware entropy source for NaCl / PNaCl.
 *
 * Reads from /dev/urandom provided by nacl_io (backed by NaCl's IRT random interface),
 * with a fallback xorshift64* mixer seeded by clock_gettime(CLOCK_REALTIME / CLOCK_MONOTONIC)
 * so mbedtls_hardware_poll never returns 0 bytes.
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void) data;
    size_t filled = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (filled < len) {
            ssize_t r = read(fd, output + filled, len - filled);
            if (r <= 0) break;
            filled += (size_t) r;
        }
        close(fd);
    }
    if (filled < len) {
        struct timespec ts_real = {0, 0}, ts_mono = {0, 0};
        clock_gettime(CLOCK_REALTIME, &ts_real);
        clock_gettime(CLOCK_MONOTONIC, &ts_mono);
        static uint64_t state = 0x853c49e6748fea9bULL;
        state ^= (uint64_t) ts_real.tv_sec ^ ((uint64_t) ts_real.tv_nsec << 32);
        state ^= (uint64_t) ts_mono.tv_nsec ^ (uintptr_t) output;
        while (filled < len) {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            uint64_t val = state * 0x2545F4914F6CDD1DULL;
            for (int i = 0; i < 8 && filled < len; ++i) {
                output[filled++] = (unsigned char) (val & 0xff);
                val >>= 8;
            }
        }
    }
    *olen = len;
    return 0;
}

int64_t mbedtls_ms_time(void) {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000);
}

void mbedtls_platform_zeroize(void *buf, size_t len) {
    if (len > 0 && buf != NULL) {
        volatile unsigned char *p = (volatile unsigned char *) buf;
        while (len--) {
            *p++ = 0;
        }
    }
}
