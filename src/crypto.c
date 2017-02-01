/*
 * crypto.c - Manage the global crypto
 *
 * Copyright (C) 2013 - 2017, Max Lv <max.c.lv@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <sodium.h>
#include <mbedtls/md5.h>

#include "cache.h"
#include "crypto.h"
#include "stream.h"
#include "aead.h"
#include "utils.h"

struct cache *nonce_cache;

int
balloc(buffer_t *ptr, size_t capacity)
{
    sodium_memzero(ptr, sizeof(buffer_t));
    ptr->data     = ss_malloc(capacity);
    ptr->capacity = capacity;
    return capacity;
}

int
brealloc(buffer_t *ptr, size_t len, size_t capacity)
{
    if (ptr == NULL)
        return -1;
    size_t real_capacity = max(len, capacity);
    if (ptr->capacity < real_capacity) {
        ptr->data     = ss_realloc(ptr->data, real_capacity);
        ptr->capacity = real_capacity;
    }
    return real_capacity;
}

void
bfree(buffer_t *ptr)
{
    if (ptr == NULL)
        return;
    ptr->idx      = 0;
    ptr->len      = 0;
    ptr->capacity = 0;
    if (ptr->data != NULL) {
        ss_free(ptr->data);
    }
}

int
bprepend(buffer_t *dst, buffer_t *src, size_t capacity)
{
    brealloc(dst, dst->len + src->len, capacity);
    memmove(dst->data + src->len, dst->data, dst->len);
    memcpy(dst->data, src->data, src->len);
    dst->len = dst->len + src->len;
    return dst->len;
}

int
rand_bytes(void *output, int len)
{
    randombytes_buf(output, len);
    // always return success
    return 0;
}

unsigned char *
crypto_md5(const unsigned char *d, size_t n, unsigned char *md)
{
    static unsigned char m[16];
    if (md == NULL) {
        md = m;
    }
    mbedtls_md5(d, n, md);
    return md;
}

int
crypto_derive_key(const cipher_t *cipher, const char *pass,
                  uint8_t *key, size_t nkey, int version)
{
    if (version == 2) {
        const unsigned char salt[crypto_pwhash_SALTBYTES] = {
            's', 'h', 'a', 'd', 'o', 'w', 's', 'o',
            'c', 'k', 's', ' ', 'h', 'a', 's', 'h'
        };
        int err = crypto_pwhash (key, nkey, (char*)pass, strlen(pass), salt,
                crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                crypto_pwhash_ALG_DEFAULT);
        if (err)
            FATAL("Out of memory when doing password hashing");
        else
            return nkey;
    }

    size_t datal;
    datal = strlen((const char *)pass);

    const digest_type_t *md = mbedtls_md_info_from_string("MD5");
    if (md == NULL) {
        FATAL("MD5 Digest not found in crypto library");
    }

    mbedtls_md_context_t c;
    unsigned char md_buf[MAX_MD_SIZE];
    int addmd;
    unsigned int i, j, mds;

    mds = mbedtls_md_get_size(md);
    memset(&c, 0, sizeof(mbedtls_md_context_t));

    if (pass == NULL)
        return nkey;
    if (mbedtls_md_setup(&c, md, 1))
        return 0;

    for (j = 0, addmd = 0; j < nkey; addmd++) {
        mbedtls_md_starts(&c);
        if (addmd) {
            mbedtls_md_update(&c, md_buf, mds);
        }
        mbedtls_md_update(&c, (uint8_t *)pass, datal);
        mbedtls_md_finish(&c, &(md_buf[0]));

        for (i = 0; i < mds; i++, j++) {
            if (j >= nkey)
                break;
            key[j] = md_buf[i];
        }
    }

    mbedtls_md_free(&c);
    return nkey;
}

crypto_t *
crypto_init(const char *password, const char *method)
{
    int i, m = -1;

    // Initialize sodium for random generator
    if (sodium_init() == -1) {
        FATAL("Failed to initialize sodium");
    }

    // Initialize NONCE cache
    cache_create(&nonce_cache, 1024, NULL);

    if (method != NULL) {
        for (i = 0; i < STREAM_CIPHER_NUM; i++)
            if (strcmp(method, supported_stream_ciphers[i]) == 0) {
                m = i;
                break;
            }
        if (m != -1) {
            cipher_t *cipher = stream_init(password, method);
            if (cipher == NULL)
                return NULL;
            crypto_t *crypto = (crypto_t *)malloc(sizeof(crypto_t));
            crypto_t tmp     = {
                .cipher      = cipher,
                .encrypt_all = &stream_encrypt_all,
                .decrypt_all = &stream_decrypt_all,
                .encrypt     = &stream_encrypt,
                .decrypt     = &stream_decrypt,
                .ctx_init    = &stream_ctx_init,
                .ctx_release = &stream_ctx_release
            };
            memcpy(crypto, &tmp, sizeof(crypto_t));
            return crypto;
        }

        for (i = 0; i < AEAD_CIPHER_NUM; i++)
            if (strcmp(method, supported_aead_ciphers[i]) == 0) {
                m = i;
                break;
            }
        if (m != -1) {
            cipher_t *cipher = aead_init(password, method);
            if (cipher == NULL)
                return NULL;
            crypto_t *crypto = (crypto_t *)ss_malloc(sizeof(crypto_t));
            crypto_t tmp     = {
                .cipher      = cipher,
                .encrypt_all = &aead_encrypt_all,
                .decrypt_all = &aead_decrypt_all,
                .encrypt     = &aead_encrypt,
                .decrypt     = &aead_decrypt,
                .ctx_init    = &aead_ctx_init,
                .ctx_release = &aead_ctx_release
            };
            memcpy(crypto, &tmp, sizeof(crypto_t));
            return crypto;
        }
    }

    LOGE("invalid cipher name: %s", method);
    return NULL;
}
