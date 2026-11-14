/* Applied on top of mbedTLS's default config (MBEDTLS_USER_CONFIG_FILE). */
#if defined(__EMSCRIPTEN__) || defined(__native_client__) || defined(__pnacl__)
/* fastcomp (Samsung's Emscripten 1.39) and PNaCl do not support target-specific inline-asm
 * barriers in mbedTLS's constant-time code. Plain C fallbacks are used without MBEDTLS_HAVE_ASM. */
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#if defined(__native_client__) || defined(__pnacl__)
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#endif
#endif
