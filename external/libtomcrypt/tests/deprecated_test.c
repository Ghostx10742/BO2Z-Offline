/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */
#ifndef LTC_NO_DEPRECATED_APIS
#define LTC_DEPRECATED(x)
#include  <tomcrypt_test.h>

#ifdef LTC_MECC
static void s_ecc_test(void)
{
   const ltc_ecc_curve* dp;
   unsigned char buf[128];
   unsigned long len;
   ecc_key key;
   int stat;
   unsigned char data16[16] = { 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1, 0xd1 };

   /* We need an MPI provider for ECC */
   if (ltc_mp.name == NULL) return;

   ENSURE(ltc_ecc_curves[0].OID != NULL);

   DO(ecc_find_curve(ltc_ecc_curves[0].OID, &dp));
   DO(ecc_make_key_ex(&yarrow_prng, find_prng ("yarrow"), &key, dp));

   len = sizeof(buf);
   DO(ecc_sign_hash(data16, 16, buf, &len, &yarrow_prng, find_prng ("yarrow"), &key));
   stat = 0;
   DO(ecc_verify_hash(buf, len, data16, 16, &stat, &key));

   SHOULD_FAIL(ecc_verify_hash_rfc7518(buf, len, data16, 16, &stat, &key));

   len = sizeof(buf);
   DO(ecc_sign_hash_rfc7518(data16, 16, buf, &len, &yarrow_prng, find_prng ("yarrow"), &key));
   stat = 0;
   DO(ecc_verify_hash_rfc7518(buf, len, data16, 16, &stat, &key));

   SHOULD_FAIL(ecc_verify_hash(buf, len, data16, 16, &stat, &key));

   ecc_free(&key);
}
#endif

#ifdef LTC_MRSA
extern const unsigned char ltc_rsa_private_test_key[];
extern const unsigned long ltc_rsa_private_test_key_sz;
extern const unsigned char ltc_openssl_public_rsa[];
extern const unsigned long ltc_openssl_public_rsa_sz;
static void s_rsa_test(void)
{
   rsa_key key, pubkey;
   int stat;
   const unsigned char tv[] = "test";
   unsigned char buf0[1024], buf1[1024];
   unsigned long buf0len, buf1len;

   /* We need an MPI provider for RSA */
   if (ltc_mp.name == NULL) return;

   DO(rsa_import(ltc_rsa_private_test_key, ltc_rsa_private_test_key_sz, &key));
   DO(rsa_import(ltc_openssl_public_rsa, ltc_openssl_public_rsa_sz, &pubkey));

   buf0len = sizeof(buf0);
   DO(rsa_sign_hash_ex(tv, 4, buf0, &buf0len, LTC_PKCS_1_PSS, &yarrow_prng, find_prng("yarrow"), find_hash("sha1"), 8, &key));
   buf1len = sizeof(buf1);
   DO(rsa_verify_hash_ex(buf0, buf0len, tv, 4, LTC_PKCS_1_PSS, find_hash("sha1"), 8, &stat, &pubkey));
   ENSURE(stat == 1);

   buf0len = sizeof(buf0);
   DO(rsa_encrypt_key_ex(tv, 4, buf0, &buf0len, NULL, 0, &yarrow_prng, find_prng("yarrow"), find_hash("sha1"), LTC_PKCS_1_OAEP, &pubkey));
   buf1len = sizeof(buf1);
   DO(rsa_decrypt_key_ex(buf0, buf0len, buf1, &buf1len, NULL, 0, find_hash("sha1"), LTC_PKCS_1_OAEP, &stat, &key));
   ENSURE(stat == 1);
   COMPARE_TESTVECTOR(buf1, buf1len, tv, 4, "s_rsa_test", 0);

   rsa_free(&pubkey);
   rsa_free(&key);
}
#endif
#endif /* LTC_NO_DEPRECATED_APIS */

int deprecated_test(void)
{
#ifndef LTC_NO_DEPRECATED_APIS
#ifdef LTC_MECC
   s_ecc_test();
#endif
#ifdef LTC_MRSA
   s_rsa_test();
#endif
#endif /* LTC_NO_DEPRECATED_APIS */
   return 0;
}
