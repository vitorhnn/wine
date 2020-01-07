#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#ifdef HAVE_GNUTLS_CIPHER_INIT
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/abstract.h>
#ifdef HAVE_LIBGCRYPT
#include <libgcrypt/libgcrypt.h>
#endif
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "bcrypt.h"

#include "bcrypt_internal.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/library.h"
#include "wine/unicode.h"

#if defined(HAVE_GNUTLS_CIPHER_INIT) &&  defined(SONAME_LIBGCRYPT)
WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static void *libgcrypt_handle;
#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gcry_sexp_build);
MAKE_FUNCPTR(gcry_pk_encrypt);
MAKE_FUNCPTR(gcry_sexp_extract_param);
MAKE_FUNCPTR(gcry_mpi_point_new);
MAKE_FUNCPTR(gcry_mpi_ec_decode_point);
MAKE_FUNCPTR(gcry_mpi_new);
MAKE_FUNCPTR(gcry_mpi_point_snatch_get);
MAKE_FUNCPTR(gcry_mpi_print);
MAKE_FUNCPTR(gcry_sexp_release);
MAKE_FUNCPTR(gcry_mpi_release);
MAKE_FUNCPTR(gcry_mpi_point_release);
MAKE_FUNCPTR(gcry_strsource);
MAKE_FUNCPTR(gcry_strerror);
#undef MAKE_FUNCPTR

BOOL gcrypt_initialize(void)
{
    if (!(libgcrypt_handle = wine_dlopen( SONAME_LIBGCRYPT, RTLD_NOW, NULL, 0 )))
    {
        ERR_(winediag)( "failed to load libgcrypt, no support for diffie hellman key exchange\n" );
        return FALSE;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = wine_dlsym( libgcrypt_handle, #f, NULL, 0 ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(gcry_sexp_build);
    LOAD_FUNCPTR(gcry_pk_encrypt);
    LOAD_FUNCPTR(gcry_sexp_extract_param);
    LOAD_FUNCPTR(gcry_mpi_point_new);
    LOAD_FUNCPTR(gcry_mpi_ec_decode_point);
    LOAD_FUNCPTR(gcry_mpi_new);
    LOAD_FUNCPTR(gcry_mpi_point_snatch_get);
    LOAD_FUNCPTR(gcry_mpi_print);
    LOAD_FUNCPTR(gcry_sexp_release);
    LOAD_FUNCPTR(gcry_mpi_release);
    LOAD_FUNCPTR(gcry_mpi_point_release);
    LOAD_FUNCPTR(gcry_strsource);
    LOAD_FUNCPTR(gcry_strerror);
#undef LOAD_FUNCPTR

    return TRUE;

fail:
    wine_dlclose( libgcrypt_handle, NULL, 0 );
    libgcrypt_handle = NULL;
    return FALSE;
}


void gcrypt_uninitialize(void)
{
    wine_dlclose( libgcrypt_handle, NULL, 0 );
    libgcrypt_handle = NULL;
}

/* this is necessary since GNUTLS doesn't support ECDH public key encryption, maybe we can replace this when it does:
   https://github.com/gnutls/gnutls/blob/cdc4fc288d87f91f974aa23b6e8595a53970ce00/lib/nettle/pk.c#L495 */
NTSTATUS compute_secret_ecc (struct key *privkey_in, struct key *pubkey_in, struct secret *secret)
{
    const char *pubkey_format;
    DWORD key_size;
    gcry_sexp_t pubkey = NULL;
    gcry_sexp_t privkey = NULL;
    gcry_sexp_t xchg_result = NULL;
    gcry_mpi_t result_mpi = NULL;
    gcry_mpi_point_t result_point = NULL;
    gcry_mpi_t secret_key = NULL;
    gcry_error_t err;
    NTSTATUS status = STATUS_SUCCESS;

    switch (pubkey_in->alg_id)
    {
        case ALG_ID_ECDH_P256:
            pubkey_format = "NIST P-256";
            key_size = 32;
            break;
        default:
            FIXME("Unsupported algorithm id: %u\n", pubkey_in->alg_id);
            return STATUS_INTERNAL_ERROR;
    }

    /* import public key -
       copy public key into temporary buffer so we can prepend 0x04 (to indicate it is uncompressed) */
    {
        UCHAR *public_key_raw = heap_alloc((key_size * 2) + 1);
        public_key_raw[0] = 0x04;
        memcpy(public_key_raw + 1, pubkey_in->u.a.pubkey + sizeof(BCRYPT_ECCKEY_BLOB), key_size * 2);

        err = pgcry_sexp_build(&pubkey, NULL,
                            "(key-data(public-key(ecdh(curve %s)(q %b))))",
                            pubkey_format,
                            (key_size * 2) + 1,
                            public_key_raw);

        heap_free(public_key_raw);
    }

    if (err)
    {
        ERR("Failed to build gcrypt public key\n");
        goto done;
    }

    /* import private key */
    /* extract private key from blob structure */
    {
        UCHAR *private_key_raw = heap_alloc(key_size);
        UCHAR *key_blob;
        ULONG blob_size;

        status = key_export_ecc( privkey_in, NULL, 0, &blob_size );
        if (status)
            goto done;

        key_blob = heap_alloc(blob_size);
        status = key_export_ecc( privkey_in, key_blob, blob_size, &blob_size);
        if (status)
        {
            heap_free(private_key_raw);
            heap_free(key_blob);
            goto done;
        }

        memcpy(private_key_raw, key_blob + sizeof(BCRYPT_ECCKEY_BLOB) + key_size * 2, key_size);
        heap_free(key_blob);

        err = pgcry_sexp_build(&privkey, NULL,
                            "(data(flags raw)(value %b))",
                            key_size,
                            private_key_raw);

        heap_free(private_key_raw);
    }

    if (err)
    {
        ERR("Failed to build gcrypt private key data\n");
        goto done;
    }

    if ((err = pgcry_pk_encrypt(&xchg_result, privkey, pubkey)))
    {
        ERR("Failed to perform key exchange\n");
        goto done;
    }

    if ((err = pgcry_sexp_extract_param(xchg_result, "", "s", &result_mpi, NULL)))
    {
        ERR("Failed to extract result of key exchange\n");
        goto done;
    }

    result_point = pgcry_mpi_point_new(0);

    if ((err = pgcry_mpi_ec_decode_point(result_point, result_mpi, NULL)))
    {
        ERR("Failed decoding point from result\n");
        goto done;
    }

    secret_key = pgcry_mpi_new(0);
    if (!secret_key)
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    /* releases result_point, (we don't need to free it later) */
    pgcry_mpi_point_snatch_get(secret_key, NULL, NULL, result_point);

    {
        size_t secret_size;
        UCHAR *tmp_buffer;

        if ((err = pgcry_mpi_print(GCRYMPI_FMT_STD, NULL, 0, &secret_size, secret_key)))
        {
            goto done;
        }

        if (secret_size != key_size && secret_size != key_size + 1)
        {
            ERR("got secret size %lu, expected %u\n", secret_size, key_size);
            status = STATUS_INTERNAL_ERROR;
            goto done;
        }

        tmp_buffer = heap_alloc(secret_size);
        if ((err = pgcry_mpi_print(GCRYMPI_FMT_STD, tmp_buffer, secret_size, NULL, secret_key)))
        {
            goto done;
        }

        if (secret_size % 2 != 0) /* remove prepended 0 */
        {
            UCHAR *secret_buffer = heap_alloc(secret_size - 1);
            memcpy(secret_buffer, tmp_buffer + 1, secret_size - 1);
            heap_free(tmp_buffer);
            secret->data = secret_buffer;
            secret->len = secret_size - 1;
        }
        else
        {
            secret->data = tmp_buffer;
            secret->len = secret_size;
        }
    }

    done:
    pgcry_sexp_release(pubkey);
    pgcry_sexp_release(privkey);
    pgcry_sexp_release(xchg_result);
    pgcry_mpi_release(result_mpi);
    pgcry_mpi_release(secret_key);

    if (status)
    {
        return status;
    }
    if (err)
    {
        ERR("Error = %s/%s\n", pgcry_strsource (err), pgcry_strerror (err));
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}
#endif
