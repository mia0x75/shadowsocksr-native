/*
 * encrypt.c - Manage the global encryptor
 *
 * Copyright (C) 2013 - 2016, Max Lv <max.c.lv@gmail.com>
 *
 * This file is part of the shadowsocks-libev.
 *
 * shadowsocks-libev is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * shadowsocks-libev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with shadowsocks-libev; see the file COPYING. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <ctype.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(USE_CRYPTO_OPENSSL)

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/md5.h>
typedef EVP_CIPHER cipher_core_t;
typedef EVP_CIPHER_CTX cipher_core_ctx_t;
typedef EVP_MD digest_type_t;
#define MAX_KEY_LENGTH EVP_MAX_KEY_LENGTH
#define MAX_IV_LENGTH EVP_MAX_IV_LENGTH
#define MAX_MD_SIZE EVP_MAX_MD_SIZE

#include <openssl/md5.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>

#elif defined(USE_CRYPTO_MBEDTLS)

#include <mbedtls/md5.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>
#include <mbedtls/aes.h>
#define CIPHER_UNSUPPORTED "unsupported"

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <stdio.h>
#endif

#include <mbedtls/cipher.h>
#include <mbedtls/md.h>
typedef mbedtls_cipher_info_t cipher_core_t;
typedef mbedtls_cipher_context_t cipher_core_ctx_t;
typedef mbedtls_md_info_t digest_type_t;
#define MAX_KEY_LENGTH 64
#define MAX_IV_LENGTH MBEDTLS_MAX_IV_LENGTH
#define MAX_MD_SIZE MBEDTLS_MD_MAX_SIZE

/* we must have MBEDTLS_CIPHER_MODE_CFB defined */
#if !defined(MBEDTLS_CIPHER_MODE_CFB)
#error Cipher Feedback mode a.k.a CFB not supported by your mbed TLS.
#endif

#endif

#include <sodium.h>

#ifndef __MINGW32__
#include <arpa/inet.h>
#endif

#include "cache.h"
#include "encrypt.h"
#include "ssrutils.h"
#include "ssrbuffer.h"

#define OFFSET_ROL(p, o) ((uint64_t)(*(p + o)) << (8 * o))

struct cipher_env_t {
    uint8_t *enc_table;
    uint8_t *dec_table;
    uint8_t enc_key[MAX_KEY_LENGTH];
    int enc_key_len;
    int enc_iv_len;
    enum ss_cipher_type enc_method;
    struct cache *iv_cache;
};

struct cipher_wrapper {
    const cipher_core_t *core;
    size_t iv_len;
    size_t key_len;
};

struct cipher_ctx_t {
    cipher_core_ctx_t *core_ctx;
    uint8_t iv[MAX_IV_LENGTH];
};

struct enc_ctx {
    uint8_t init;
    uint64_t counter;
    struct cipher_ctx_t cipher_ctx;
};

#ifdef USE_CRYPTO_MBEDTLS

#define SS_CIPHERS_MBEDTLS_MAP(V)                               \
    V(ss_cipher_none,               "none"                  )   \
    V(ss_cipher_table,              "table"                 )   \
    V(ss_cipher_rc4,                "ARC4-128"              )   \
    V(ss_cipher_rc4_md5_6,          "ARC4-128"              )   \
    V(ss_cipher_rc4_md5,            "ARC4-128"              )   \
    V(ss_cipher_aes_128_cfb,        "AES-128-CFB128"        )   \
    V(ss_cipher_aes_192_cfb,        "AES-192-CFB128"        )   \
    V(ss_cipher_aes_256_cfb,        "AES-256-CFB128"        )   \
    V(ss_cipher_aes_128_ctr,        "AES-128-CTR"           )   \
    V(ss_cipher_aes_192_ctr,        "AES-192-CTR"           )   \
    V(ss_cipher_aes_256_ctr,        "AES-256-CTR"           )   \
    V(ss_cipher_bf_cfb,             "BLOWFISH-CFB64"        )   \
    V(ss_cipher_camellia_128_cfb,   "CAMELLIA-128-CFB128"   )   \
    V(ss_cipher_camellia_192_cfb,   "CAMELLIA-192-CFB128"   )   \
    V(ss_cipher_camellia_256_cfb,   "CAMELLIA-256-CFB128"   )   \
    V(ss_cipher_cast5_cfb,          CIPHER_UNSUPPORTED      )   \
    V(ss_cipher_des_cfb,            CIPHER_UNSUPPORTED      )   \
    V(ss_cipher_idea_cfb,           CIPHER_UNSUPPORTED      )   \
    V(ss_cipher_rc2_cfb,            CIPHER_UNSUPPORTED      )   \
    V(ss_cipher_seed_cfb,           CIPHER_UNSUPPORTED      )   \
    V(ss_cipher_salsa20,            "salsa20"               )   \
    V(ss_cipher_chacha20,           "chacha20"              )   \
    V(ss_cipher_chacha20ietf,       "chacha20-ietf"         )   \

static const char *
ss_mbedtls_cipher_name_by_type(enum ss_cipher_type index)
{
#define SS_CIPHER_MBEDTLS_GEN(name, text) case (name): return (text);
    switch (index) {
        SS_CIPHERS_MBEDTLS_MAP(SS_CIPHER_MBEDTLS_GEN)
    default:;  // Silence ss_cipher_max -Wswitch warning.
    }
#undef SS_CIPHER_MBEDTLS_GEN
    LOGE("Invalid index");
    return NULL; // "Invalid index";
}

#endif


#if DEBUG
//#define SHOW_DUMP
#endif

#ifdef SHOW_DUMP
static void
dump(char *tag, char *text, int len)
{
    int i;
    printf("%s: ", tag);
    for (i = 0; i < len; i++)
        printf("0x%02x ", (uint8_t)text[i]);
    printf("\n");
}

#endif

size_t ss_max_iv_length(void) {
    return MAX_IV_LENGTH;
}

size_t ss_max_key_length(void) {
    return MAX_KEY_LENGTH;
}

static int
crypto_stream_xor_ic(uint8_t *c, const uint8_t *m, uint64_t mlen,
                     const uint8_t *n, uint64_t ic, const uint8_t *k,
                     enum ss_cipher_type method)
{
    switch (method) {
    case ss_cipher_salsa20:
        return crypto_stream_salsa20_xor_ic(c, m, mlen, n, ic, k);
    case ss_cipher_chacha20:
        return crypto_stream_chacha20_xor_ic(c, m, mlen, n, ic, k);
    case ss_cipher_chacha20ietf:
        return crypto_stream_chacha20_ietf_xor_ic(c, m, mlen, n, (uint32_t)ic, k);
    default:
        break;
    }
    // always return 0
    return 0;
}

static int
random_compare(const void *_x, const void *_y, uint32_t i, uint64_t a)
{
    uint8_t x = *((uint8_t *)_x);
    uint8_t y = *((uint8_t *)_y);
    return (int) (a % (x + i) - a % (y + i));
}

static void
merge(uint8_t *left, int llength, uint8_t *right,
      int rlength, uint32_t salt, uint64_t key)
{
    uint8_t *ltmp = (uint8_t *)malloc((size_t)llength * sizeof(uint8_t));
    uint8_t *rtmp = (uint8_t *)malloc((size_t)rlength * sizeof(uint8_t));

    uint8_t *ll = ltmp;
    uint8_t *rr = rtmp;

    uint8_t *result = left;

    memcpy(ltmp, left, (size_t)llength * sizeof(uint8_t));
    memcpy(rtmp, right, (size_t)rlength * sizeof(uint8_t));

    while (llength > 0 && rlength > 0) {
        if (random_compare(ll, rr, salt, key) <= 0) {
            *result = *ll;
            ++ll;
            --llength;
        } else {
            *result = *rr;
            ++rr;
            --rlength;
        }
        ++result;
    }

    if (llength > 0) {
        while (llength > 0) {
            *result = *ll;
            ++result;
            ++ll;
            --llength;
        }
    } else {
        while (rlength > 0) {
            *result = *rr;
            ++result;
            ++rr;
            --rlength;
        }
    }

    ss_free(ltmp);
    ss_free(rtmp);
}

static void
merge_sort(uint8_t array[], int length, uint32_t salt, uint64_t key)
{
    uint8_t middle;
    uint8_t *left, *right;
    int llength;

    if (length <= 1) {
        return;
    }

    middle = (uint8_t)(length / 2);

    llength = length - middle;

    left  = array;
    right = array + llength;

    merge_sort(left, llength, salt, key);
    merge_sort(right, middle, salt, key);
    merge(left, llength, right, middle, salt, key);
}

int
enc_get_iv_len(struct cipher_env_t *env)
{
    return env->enc_iv_len;
}

uint8_t *
enc_get_key(struct cipher_env_t *env)
{
    return env->enc_key;
}

int
enc_get_key_len(struct cipher_env_t *env)
{
    return env->enc_key_len;
}

unsigned char *
enc_md5(const unsigned char *d, size_t n, unsigned char *md)
{
#if defined(USE_CRYPTO_OPENSSL)
    return MD5(d, n, md);
#elif defined(USE_CRYPTO_MBEDTLS)
    static unsigned char m[16];
    if (md == NULL) {
        md = m;
    }
    mbedtls_md5_ret(d, n, md);
    return md;
#endif
}

int
cipher_iv_size(const struct cipher_wrapper *cipher)
{
#if defined(USE_CRYPTO_OPENSSL)
    if (cipher->core == NULL) {
        return (int) cipher->iv_len;
    } else {
        return EVP_CIPHER_iv_length(cipher->core);
    }
#elif defined(USE_CRYPTO_MBEDTLS)
    if (cipher == NULL) {
        return 0;
    }
    return cipher->core->iv_size;
#endif
}

int
cipher_key_size(const struct cipher_wrapper *cipher)
{
#if defined(USE_CRYPTO_OPENSSL)
    if (cipher->core == NULL) {
        return (int) cipher->key_len;
    } else {
        return EVP_CIPHER_key_length(cipher->core);
    }
#elif defined(USE_CRYPTO_MBEDTLS)
    /*
    * Semi-API changes (technically public, morally private)
    * Renamed a few headers to include _internal in the name. Those headers are
    * not supposed to be included by users.
    * Changed md_info_t into an opaque structure (use md_get_xxx() accessors).
    * Changed pk_info_t into an opaque structure.
    * Changed cipher_base_t into an opaque structure.
    */
    if (cipher == NULL) {
        return 0;
    }
    /* From Version 1.2.7 released 2013-04-13 Default Blowfish keysize is now 128-bits */
    return cipher->core->key_bitlen / 8;
#endif
}

void
bytes_to_key_with_size(const char *pass, size_t len, uint8_t *md, size_t md_size)
{
    uint8_t result[128];
    enc_md5((const unsigned char *)pass, len, result);
    memcpy(md, result, 16);
    int i = 16;
    for (; i < (int)md_size; i += 16) {
        memcpy(result + 16, pass, len);
        enc_md5(result, 16 + len, result);
        memcpy(md + i, result, 16);
    }
}

int
bytes_to_key(const struct cipher_wrapper *cipher, const digest_type_t *md,
             const uint8_t *pass, uint8_t *key)
{
    size_t datal;
    datal = strlen((const char *)pass);

#if defined(USE_CRYPTO_OPENSSL)

    MD5_CTX c;
    unsigned char md_buf[MAX_MD_SIZE];
    int nkey;
    unsigned int i, mds;

    mds  = 16;
    nkey = 16;
    if (cipher != NULL) {
        nkey = cipher_key_size(cipher);
    }
    if (pass == NULL) {
        return nkey;
    }
    memset(&c, 0, sizeof(MD5_CTX));

    for (int j = 0, addmd = 0; j < nkey; addmd++) {
        MD5_Init(&c);
        if (addmd) {
            MD5_Update(&c, md_buf, mds);
        }
        MD5_Update(&c, pass, datal);
        MD5_Final(md_buf, &c);

        for (i = 0; i < mds; i++, j++) {
            if (j >= nkey) {
                break;
            }
            key[j] = md_buf[i];
        }
    }

    return nkey;

#elif defined(USE_CRYPTO_MBEDTLS)

    mbedtls_md_context_t c;
    unsigned char md_buf[MAX_MD_SIZE];
    int nkey;
    int addmd;
    unsigned int i, j, mds;

    nkey = 16;
    if (cipher != NULL) {
        nkey = cipher_key_size(cipher);
    }
    mds = mbedtls_md_get_size(md);
    memset(&c, 0, sizeof(mbedtls_md_context_t));

    if (pass == NULL)
        return nkey;
    if (mbedtls_md_setup(&c, md, 1))
        return 0;

    for (j = 0, addmd = 0; j < (unsigned int)nkey; addmd++) {
        mbedtls_md_starts(&c);
        if (addmd) {
            mbedtls_md_update(&c, md_buf, mds);
        }
        mbedtls_md_update(&c, pass, datal);
        mbedtls_md_finish(&c, &(md_buf[0]));

        for (i = 0; i < mds; i++, j++) {
            if (j >= (unsigned int)nkey) {
                break;
            }
            key[j] = md_buf[i];
        }
    }

    mbedtls_md_free(&c);
    return nkey;
#endif
}

int
rand_bytes(uint8_t *output, int len)
{
    randombytes_buf(output, (size_t)len);
    // always return success
    return 0;
}

const cipher_core_t *
get_cipher_of_type(enum ss_cipher_type method)
{
    if (method >= ss_cipher_salsa20) {
        return NULL;
    }

    if (method == ss_cipher_rc4_md5 || method == ss_cipher_rc4_md5_6) {
        method = ss_cipher_rc4;
    }

    const char *cipherName = ss_cipher_name_of_type(method);
    if (cipherName == NULL) {
        return NULL;
    }
#if defined(USE_CRYPTO_OPENSSL)
    return EVP_get_cipherbyname(cipherName);
#elif defined(USE_CRYPTO_MBEDTLS)
    const char *mbedtlsname = ss_mbedtls_cipher_name_by_type(method);
    if (strcmp(mbedtlsname, CIPHER_UNSUPPORTED) == 0) {
        LOGE("Cipher %s currently is not supported by mbed TLS library", mbedtlsname);
        return NULL;
    }
    return mbedtls_cipher_info_from_string(mbedtlsname);
#endif
}

const digest_type_t *
get_digest_type(const char *digest)
{
    if (digest == NULL) {
        LOGE("get_digest_type(): Digest name is null");
        return NULL;
    }

#if defined(USE_CRYPTO_OPENSSL)
    return EVP_get_digestbyname(digest);
#elif defined(USE_CRYPTO_MBEDTLS)
    return mbedtls_md_info_from_string(digest);
#endif
}

void
cipher_context_init(struct cipher_env_t *env, struct cipher_ctx_t *ctx, bool encrypt)
{
    enum ss_cipher_type method = env->enc_method;

    if (method >= ss_cipher_salsa20) {
//        enc_iv_len = ss_cipher_iv_size(method);
        return;
    }

    const char *cipherName = ss_cipher_name_of_type(method);
    if (cipherName == NULL) {
        return;
    }

    const cipher_core_t *cipher = get_cipher_of_type(method);

#if defined(USE_CRYPTO_OPENSSL)
    ctx->core_ctx = EVP_CIPHER_CTX_new();
    cipher_core_ctx_t *core_ctx = ctx->core_ctx;

    if (cipher == NULL) {
        LOGE("Cipher %s not found in OpenSSL library", cipherName);
        FATAL("Cannot initialize cipher");
    }
    if (!EVP_CipherInit_ex(core_ctx, cipher, NULL, NULL, NULL, encrypt)) {
        LOGE("Cannot initialize cipher %s", cipherName);
        exit(EXIT_FAILURE);
    }
    if (!EVP_CIPHER_CTX_set_key_length(core_ctx, env->enc_key_len)) {
        EVP_CIPHER_CTX_cleanup(core_ctx);
        LOGE("Invalid key length: %d", env->enc_key_len);
        exit(EXIT_FAILURE);
    }
    if (method > ss_cipher_rc4_md5) {
        EVP_CIPHER_CTX_set_padding(core_ctx, 1);
    }
#endif

#if defined(USE_CRYPTO_MBEDTLS)
    if (cipher == NULL) {
        LOGE("Cipher %s not found in mbed TLS library", cipherName);
        FATAL("Cannot initialize mbed TLS cipher");
    }
    cipher_core_ctx_t *core_ctx = calloc(1, sizeof(cipher_core_ctx_t));
    mbedtls_cipher_init(core_ctx);
    if (mbedtls_cipher_setup(core_ctx, cipher) != 0) {
        FATAL("Cannot initialize mbed TLS cipher context");
    }
    ctx->core_ctx = core_ctx;
#endif
}

void
cipher_context_set_iv(struct cipher_env_t *env, struct cipher_ctx_t *ctx, uint8_t *iv, size_t iv_len,
                      int enc)
{
    const unsigned char *true_key;

    if (iv == NULL) {
        LOGE("cipher_context_set_iv(): IV is null");
        return;
    }

    if (!enc) {
        memcpy(ctx->iv, iv, iv_len);
    }

    if (env->enc_method >= ss_cipher_salsa20) {
        return;
    }

    if (env->enc_method == ss_cipher_rc4_md5 || env->enc_method == ss_cipher_rc4_md5_6) {
        unsigned char key_iv[32];
        memcpy(key_iv, env->enc_key, 16);
        memcpy(key_iv + 16, iv, iv_len);
        true_key = enc_md5(key_iv, 16 + iv_len, NULL);
        iv_len   = 0;
    } else {
        true_key = env->enc_key;
    }
    cipher_core_ctx_t *core_ctx = ctx->core_ctx;
    if (core_ctx == NULL) {
        LOGE("cipher_context_set_iv(): Cipher context is null");
        return;
    }
#if defined(USE_CRYPTO_OPENSSL)
    if (!EVP_CipherInit_ex(core_ctx, NULL, NULL, true_key, iv, enc)) {
        EVP_CIPHER_CTX_cleanup(core_ctx);
        FATAL("Cannot set key and IV");
    }
#elif defined(USE_CRYPTO_MBEDTLS)
    if (mbedtls_cipher_setkey(core_ctx, true_key, env->enc_key_len * 8, enc) != 0) {
        mbedtls_cipher_free(core_ctx);
        FATAL("Cannot set mbed TLS cipher key");
    }

    if (mbedtls_cipher_set_iv(core_ctx, iv, iv_len) != 0) {
        mbedtls_cipher_free(core_ctx);
        FATAL("Cannot set mbed TLS cipher IV");
    }
    if (mbedtls_cipher_reset(core_ctx) != 0) {
        mbedtls_cipher_free(core_ctx);
        FATAL("Cannot finalize mbed TLS cipher context");
    }
#endif

#ifdef SHOW_DUMP
    dump("IV", (char *)iv, (int)iv_len);
#endif
}

void
cipher_context_release(struct cipher_env_t *env, struct cipher_ctx_t *ctx)
{
    if (env->enc_method >= ss_cipher_salsa20) {
        return;
    }
#if defined(USE_CRYPTO_OPENSSL)
    EVP_CIPHER_CTX_free(ctx->core_ctx);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_cipher_free(ctx->core_ctx);
    ss_free(ctx->core_ctx);
#endif
}

static int
cipher_context_update(struct cipher_ctx_t *ctx, uint8_t *output, size_t *olen,
                      const uint8_t *input, size_t ilen)
{
    cipher_core_ctx_t *core_ctx = ctx->core_ctx;
#if defined(USE_CRYPTO_OPENSSL)
    int err = 0, tlen = (int)*olen;
    err = EVP_CipherUpdate(core_ctx, (unsigned char *)output, &tlen,
                           (const unsigned char *)input, (int)ilen);
    *olen = (size_t)tlen;
    return err;
#elif defined(USE_CRYPTO_MBEDTLS)
    return !mbedtls_cipher_update(core_ctx, (const uint8_t *)input, ilen,
        (uint8_t *)output, olen);
#endif
}

int
ss_md5_hmac_with_key(char *auth, char *msg, int msg_len, uint8_t *auth_key, int key_len)
{
    uint8_t hash[MD5_BYTES];
#if defined(USE_CRYPTO_OPENSSL)
    HMAC(EVP_md5(), auth_key, key_len, (unsigned char *)msg, (size_t)msg_len, (unsigned char *)hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), auth_key, key_len, (uint8_t *)msg, msg_len, (uint8_t *)hash);
#endif
    memcpy(auth, hash, MD5_BYTES);

    return 0;
}

int
ss_md5_hash_func(char *auth, char *msg, int msg_len)
{
    uint8_t hash[MD5_BYTES];
#if defined(USE_CRYPTO_OPENSSL)
    MD5((unsigned char *)msg, (size_t)msg_len, (unsigned char *)hash);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), (uint8_t *)msg, msg_len, (uint8_t *)hash);
#endif
    memcpy(auth, hash, MD5_BYTES);

    return 0;
}

int
ss_sha1_hmac_with_key(char *auth, char *msg, int msg_len, uint8_t *auth_key, int key_len)
{
    uint8_t hash[SHA1_BYTES];
#if defined(USE_CRYPTO_OPENSSL)
    HMAC(EVP_sha1(), auth_key, key_len, (unsigned char *)msg, (size_t)msg_len, (unsigned char *)hash, NULL);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), auth_key, key_len, (uint8_t *)msg, msg_len, (uint8_t *)hash);
#endif
    memcpy(auth, hash, SHA1_BYTES);

    return 0;
}

int
ss_sha1_hash_func(char *auth, char *msg, int msg_len)
{
    uint8_t hash[SHA1_BYTES];
#if defined(USE_CRYPTO_OPENSSL)
    SHA1((unsigned char *)msg, (size_t)msg_len, (unsigned char *)hash);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), (uint8_t *)msg, msg_len, (uint8_t *)hash);
#endif
    memcpy(auth, hash, SHA1_BYTES);

    return 0;
}

int
ss_aes_128_cbc(char *encrypt, char *out_data, char *key)
{
    unsigned char iv[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#if defined(USE_CRYPTO_OPENSSL)
    AES_KEY aes;
    AES_set_encrypt_key((unsigned char*)key, 128, &aes);
    AES_cbc_encrypt((const unsigned char *)encrypt, (unsigned char *)out_data, 16, &aes, iv, AES_ENCRYPT);
#elif defined(USE_CRYPTO_MBEDTLS)
    mbedtls_aes_context aes;

    unsigned char output[16];

    mbedtls_aes_setkey_enc(&aes, (unsigned char *)key, 128);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, 16, iv, (unsigned char *)encrypt, output);

    memcpy(out_data, output, 16);
#endif
    return 0;
}

int
ss_encrypt_all(struct cipher_env_t *env, struct buffer_t *plain, size_t capacity)
{
    enum ss_cipher_type method = env->enc_method;
    if (method > ss_cipher_table) {
        struct cipher_ctx_t cipher_ctx;
        cipher_context_init(env, &cipher_ctx, 1);

        size_t iv_len = (size_t) env->enc_iv_len;
        int err       = 1;

        struct buffer_t *cipher = buffer_alloc(max(iv_len + plain->len, capacity));
        cipher->len = plain->len;

        uint8_t iv[MAX_IV_LENGTH];

        rand_bytes(iv, (int)iv_len);
        cipher_context_set_iv(env, &cipher_ctx, iv, iv_len, 1);
        memcpy(cipher->buffer, iv, iv_len);

        if (method >= ss_cipher_salsa20) {
            crypto_stream_xor_ic((uint8_t *)(cipher->buffer + iv_len),
                                 (const uint8_t *)plain->buffer, (uint64_t)(plain->len),
                                 (const uint8_t *)iv,
                                 0, env->enc_key, method);
        } else {
            err = cipher_context_update(&cipher_ctx, (uint8_t *)(cipher->buffer + iv_len),
                                        &cipher->len, (const uint8_t *)plain->buffer,
                                        plain->len);
        }

        if (!err) {
            cipher_context_release(env, &cipher_ctx);
            buffer_free(cipher);
            return -1;
        }

#ifdef SHOW_DUMP
        dump("PLAIN", plain->buffer, (int)plain->len);
        dump("CIPHER", cipher->buffer + iv_len, (int)cipher->len);
#endif

        cipher_context_release(env, &cipher_ctx);

        buffer_realloc(plain, max(iv_len + cipher->len, capacity));
        memcpy(plain->buffer, cipher->buffer, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        buffer_free(cipher);
        return 0;
    } else {
        if (env->enc_method == ss_cipher_table) {
            char *begin = plain->buffer;
            char *ptr   = plain->buffer;
            while (ptr < begin + plain->len) {
                *ptr = (char)env->enc_table[(uint8_t)*ptr];
                ptr++;
            }
        }

        return 0;
    }
}

int
ss_encrypt(struct cipher_env_t *env, struct buffer_t *plain, struct enc_ctx *ctx, size_t capacity)
{
    if (ctx != NULL) {
        int err       = 1;
        size_t iv_len = 0;
        if (!ctx->init) {
            iv_len = (size_t)env->enc_iv_len;
        }

        struct buffer_t *cipher = buffer_alloc(max(iv_len + plain->len, capacity));
        cipher->len = plain->len;

        if (!ctx->init) {
            cipher_context_set_iv(env, &ctx->cipher_ctx, ctx->cipher_ctx.iv, iv_len, 1);
            memcpy(cipher->buffer, ctx->cipher_ctx.iv, iv_len);
            ctx->counter = 0;
            ctx->init    = 1;
        }

        if (env->enc_method >= ss_cipher_salsa20) {
            size_t padding = (size_t)(ctx->counter % SODIUM_BLOCK_SIZE);
            buffer_realloc(cipher, max(iv_len + (padding + cipher->len) * 2, capacity));
            if (padding) {
                buffer_realloc(plain, max(plain->len + padding, capacity));
                memmove(plain->buffer + padding, plain->buffer, plain->len);
                sodium_memzero(plain->buffer, padding);
            }
            crypto_stream_xor_ic((uint8_t *)(cipher->buffer + iv_len),
                                 (const uint8_t *)plain->buffer,
                                 ((uint64_t)plain->len + padding),
                                 (const uint8_t *)ctx->cipher_ctx.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, env->enc_key,
                                 env->enc_method);
            ctx->counter += plain->len;
            if (padding) {
                memmove(cipher->buffer + iv_len,
                        cipher->buffer + iv_len + padding, cipher->len);
            }
        } else {
            err =
                cipher_context_update(&ctx->cipher_ctx,
                                      (uint8_t *)(cipher->buffer + iv_len),
                                      &cipher->len, (const uint8_t *)plain->buffer,
                                      plain->len);
            if (!err) {
                buffer_free(cipher);
                return -1;
            }
        }

#ifdef SHOW_DUMP
        dump("PLAIN", plain->buffer, (int)plain->len);
        dump("CIPHER", cipher->buffer + iv_len, (int)cipher->len);
#endif

        buffer_realloc(plain, max(iv_len + cipher->len, capacity));
        memcpy(plain->buffer, cipher->buffer, iv_len + cipher->len);
        plain->len = iv_len + cipher->len;

        buffer_free(cipher);
        return 0;
    } else {
        if (env->enc_method == ss_cipher_table) {
            char *begin = plain->buffer;
            char *ptr   = plain->buffer;
            while (ptr < begin + plain->len) {
                *ptr = (char)env->enc_table[(uint8_t)*ptr];
                ptr++;
            }
        }
        return 0;
    }
}

int
ss_decrypt_all(struct cipher_env_t *env, struct buffer_t *cipher, size_t capacity)
{
    enum ss_cipher_type method = env->enc_method;
    if (method > ss_cipher_table) {
        size_t iv_len = (size_t)env->enc_iv_len;
        int ret       = 1;

        if (cipher->len <= iv_len) {
            return -1;
        }

        struct cipher_ctx_t cipher_ctx;
        cipher_context_init(env, &cipher_ctx, 0);

        struct buffer_t *plain = buffer_alloc(max(cipher->len, capacity));
        plain->len = cipher->len - iv_len;

        uint8_t iv[MAX_IV_LENGTH];
        memcpy(iv, cipher->buffer, iv_len);
        cipher_context_set_iv(env, &cipher_ctx, iv, iv_len, 0);

        if (method >= ss_cipher_salsa20) {
            crypto_stream_xor_ic((uint8_t *)plain->buffer,
                                 (const uint8_t *)(cipher->buffer + iv_len),
                                 (uint64_t)(cipher->len - iv_len),
                                 (const uint8_t *)iv, 0, env->enc_key, method);
        } else {
            ret = cipher_context_update(&cipher_ctx, (uint8_t *)plain->buffer, &plain->len,
                                        (const uint8_t *)(cipher->buffer + iv_len),
                                        cipher->len - iv_len);
        }

        if (!ret) {
            cipher_context_release(env, &cipher_ctx);
            buffer_free(plain);
            return -1;
        }

#ifdef SHOW_DUMP
        dump("PLAIN", plain->buffer, (int)plain->len);
        dump("CIPHER", cipher->buffer + iv_len, (int)(cipher->len - iv_len));
#endif

        cipher_context_release(env, &cipher_ctx);

        buffer_realloc(cipher, max(plain->len, capacity));
        memcpy(cipher->buffer, plain->buffer, plain->len);
        cipher->len = plain->len;

        buffer_free(plain);
        return 0;
    } else {
        if (method == ss_cipher_table) {
            char *begin = cipher->buffer;
            char *ptr   = cipher->buffer;
            while (ptr < begin + cipher->len) {
                *ptr = (char)env->dec_table[(uint8_t)*ptr];
                ptr++;
            }
        }

        return 0;
    }
}

int
ss_decrypt(struct cipher_env_t *env, struct buffer_t *cipher, struct enc_ctx *ctx, size_t capacity)
{
    if (ctx != NULL) {
        size_t iv_len = 0;
        int err       = 1;

        struct buffer_t *plain = buffer_alloc(max(cipher->len, capacity));
        plain->len = cipher->len;

        if (!ctx->init) {
            uint8_t iv[MAX_IV_LENGTH];
            iv_len      = (size_t)env->enc_iv_len;
            plain->len -= iv_len;

            memcpy(iv, cipher->buffer, iv_len);
            cipher_context_set_iv(env, &ctx->cipher_ctx, iv, iv_len, 0);
            ctx->counter = 0;
            ctx->init    = 1;

            if (env->enc_method > ss_cipher_rc4) {
                if (cache_key_exist(env->iv_cache, (char *)iv, iv_len)) {
                    return -1;
                } else {
                    cache_insert(env->iv_cache, (char *)iv, iv_len, NULL);
                }
            }
        }

        if (env->enc_method >= ss_cipher_salsa20) {
            size_t padding = (size_t)(ctx->counter % SODIUM_BLOCK_SIZE);
            buffer_realloc(plain, max((plain->len + padding) * 2, capacity));

            if (padding) {
                buffer_realloc(cipher, max(cipher->len + padding, capacity));
                memmove(cipher->buffer + iv_len + padding, cipher->buffer + iv_len,
                        cipher->len - iv_len);
                sodium_memzero(cipher->buffer + iv_len, padding);
            }
            crypto_stream_xor_ic((uint8_t *)plain->buffer,
                                 (const uint8_t *)(cipher->buffer + iv_len),
                                 ((uint64_t)cipher->len - iv_len + padding),
                                 (const uint8_t *)ctx->cipher_ctx.iv,
                                 ctx->counter / SODIUM_BLOCK_SIZE, env->enc_key,
                                 env->enc_method);
            ctx->counter += cipher->len - iv_len;
            if (padding) {
                memmove(plain->buffer, plain->buffer + padding, plain->len);
            }
        } else {
            err = cipher_context_update(&ctx->cipher_ctx, (uint8_t *)plain->buffer, &plain->len,
                                        (const uint8_t *)(cipher->buffer + iv_len),
                                        cipher->len - iv_len);
        }

        if (!err) {
            buffer_free(plain);
            return -1;
        }

#ifdef SHOW_DUMP
        dump("PLAIN", plain->buffer, (int)plain->len);
        dump("CIPHER", cipher->buffer + iv_len, (int)(cipher->len - iv_len));
#endif

        buffer_realloc(cipher, max(plain->len, capacity));
        memcpy(cipher->buffer, plain->buffer, plain->len);
        cipher->len = plain->len;

        buffer_free(plain);
        return 0;
    } else {
        if(env->enc_method == ss_cipher_table) {
            char *begin = cipher->buffer;
            char *ptr   = cipher->buffer;
            while (ptr < begin + cipher->len) {
                *ptr = (char)env->dec_table[(uint8_t)*ptr];
                ptr++;
            }
        }
        return 0;
    }
}

int
ss_encrypt_buffer(struct cipher_env_t *env, struct enc_ctx *ctx, char *in, size_t in_size, char *out, size_t *out_size)
{
    struct buffer_t *cipher = buffer_alloc(in_size + 32);
    cipher->len = in_size;
    memcpy(cipher->buffer, in, in_size);
    int s = ss_encrypt(env, cipher, ctx, in_size + 32);
    if (s == 0) {
        *out_size = cipher->len;
        memcpy(out, cipher->buffer, cipher->len);
    }
    buffer_free(cipher);
    return s;
}

int
ss_decrypt_buffer(struct cipher_env_t *env, struct enc_ctx *ctx, char *in, size_t in_size, char *out, size_t *out_size)
{
    struct buffer_t *cipher = buffer_alloc(in_size + 32);
    cipher->len = in_size;
    memcpy(cipher->buffer, in, in_size);
    int s = ss_decrypt(env, cipher, ctx, in_size + 32);
    if (s == 0) {
        *out_size = cipher->len;
        memcpy(out, cipher->buffer, cipher->len);
    }
    buffer_free(cipher);
    return s;
}

const uint8_t * enc_ctx_get_iv(const struct enc_ctx *ctx) {
    return ctx->cipher_ctx.iv;
}

struct enc_ctx *
enc_ctx_new_instance(struct cipher_env_t *env, bool encrypt)
{
    struct enc_ctx *ctx = (struct enc_ctx *)calloc(1, sizeof(struct enc_ctx));
    sodium_memzero(ctx, sizeof(struct enc_ctx));
    cipher_context_init(env, &ctx->cipher_ctx, encrypt);

    if (encrypt) {
        rand_bytes(ctx->cipher_ctx.iv, env->enc_iv_len);
    }
    return ctx;
}

void
enc_ctx_release_instance(struct cipher_env_t *env, struct enc_ctx *ctx)
{
    cipher_context_release(env, &ctx->cipher_ctx);
    free(ctx);
}

void
enc_table_init(struct cipher_env_t * env, enum ss_cipher_type method, const char *pass)
{
    uint32_t i;
    uint64_t key = 0;
    uint8_t *digest;

    env->enc_table = ss_malloc(256);
    env->dec_table = ss_malloc(256);

    digest = enc_md5((const uint8_t *)pass, strlen(pass), NULL);

    for (i = 0; i < 8; i++) {
        key += OFFSET_ROL(digest, i);
    }
    for (i = 0; i < 256; ++i) {
        env->enc_table[i] = (uint8_t)i;
    }
    for (i = 1; i < 1024; ++i) {
        merge_sort(env->enc_table, 256, i, key);
    }
    for (i = 0; i < 256; ++i) {
        // gen decrypt table from encrypt table
        env->dec_table[env->enc_table[i]] = (uint8_t)i;
    }

    if (method == ss_cipher_table) {
        env->enc_key_len = (int) strlen(pass);
        memcpy(&env->enc_key, pass, env->enc_key_len);
    } else {
        const digest_type_t *md = get_digest_type("MD5");

        env->enc_key_len = bytes_to_key(NULL, md, (const uint8_t *)pass, env->enc_key);

        if (env->enc_key_len == 0) {
            FATAL("Cannot generate key and IV");
        }
    }

    env->enc_iv_len = 0;

    env->enc_method = method;
}

void
enc_key_init(struct cipher_env_t *env, enum ss_cipher_type method, const char *pass)
{
    if (method < ss_cipher_none || method >= ss_cipher_max) {
        LOGE("enc_key_init(): Illegal method");
        return;
    }

    // Initialize cache
    cache_create(&env->iv_cache, 256, NULL);

#if defined(USE_CRYPTO_OPENSSL)
    OpenSSL_add_all_algorithms();
#else
    cipher_core_t cipher_info = { 0 };
#endif

    struct cipher_wrapper *cipher = (struct cipher_wrapper *)calloc(1, sizeof(struct cipher_wrapper));

    // Initialize sodium for random generator
    if (sodium_init() == -1) {
        FATAL("Failed to initialize sodium");
    }

    if (method == ss_cipher_salsa20 || method == ss_cipher_chacha20 || method == ss_cipher_chacha20ietf) {
#if defined(USE_CRYPTO_OPENSSL)
        cipher->core    = NULL;
        cipher->key_len = (size_t) ss_cipher_key_size(method);
        cipher->iv_len  = (size_t) ss_cipher_iv_size(method);
#endif
#if defined(USE_CRYPTO_MBEDTLS)
        // XXX: key_length changed to key_bitlen in mbed TLS 2.0.0

        cipher_info.base = NULL;
        cipher_info.key_bitlen = (size_t)ss_cipher_key_size(method) * 8;
        cipher_info.iv_size = (size_t)ss_cipher_iv_size(method);

        cipher->core = &cipher_info;
#endif
    } else {
        cipher->core = get_cipher_of_type(method);
    }

    if (cipher->core == NULL && cipher->key_len == 0) {
        LOGE("Cipher %s not found in crypto library", ss_cipher_name_of_type(method));
        FATAL("Cannot initialize cipher");
    }

    const digest_type_t *md = get_digest_type("MD5");
    if (md == NULL) {
        FATAL("MD5 Digest not found in crypto library");
    }

    env->enc_key_len = bytes_to_key(cipher, md, (const uint8_t *)pass, env->enc_key);

    if (env->enc_key_len == 0) {
        FATAL("Cannot generate key and IV");
    }
    if (method == ss_cipher_rc4_md5 || method == ss_cipher_rc4_md5_6) {
        env->enc_iv_len = ss_cipher_iv_size(method);
    } else {
        env->enc_iv_len = cipher_iv_size(cipher);
    }
    env->enc_method = method;
    free(cipher);
}

struct cipher_env_t *
cipher_env_new_instance(const char *pass, const char *method)
{
    struct cipher_env_t *env = (struct cipher_env_t *)calloc(1, sizeof(struct cipher_env_t));
    enum ss_cipher_type m = ss_cipher_type_of_name(method);
    if (m <= ss_cipher_table) {
        enc_table_init(env, m, pass);
    } else {
        enc_key_init(env, m, pass);
    }
    env->enc_method = m;
    return env;
}

enum ss_cipher_type cipher_env_enc_method(const struct cipher_env_t *env) {
    return env->enc_method;
}

void
cipher_env_release(struct cipher_env_t *env)
{
    if (env->enc_method == ss_cipher_table) {
        ss_free(env->enc_table);
        ss_free(env->dec_table);
    } else {
        cache_delete(env->iv_cache, 0);
    }
    free(env);
}
