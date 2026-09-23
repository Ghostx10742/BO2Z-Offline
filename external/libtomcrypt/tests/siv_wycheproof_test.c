/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */
/* test SIV */
#include <tomcrypt_test.h>

#if defined(LTC_TEST) && defined(LTC_SIV_MODE)
#include "siv_wycheproof.h"

/* typedef struct
 * {
 *    int tcId;
 *    const char *comment;
 *    uint8_t key[64];
 *    size_t keyLen;
 *    const uint8_t *aad;
 *    size_t aadLen;
 *    const uint8_t *msg;
 *    size_t msgLen;
 *    const uint8_t *ct;
 *    size_t ctLen;
 *    ltc_aes_siv_test_case_result result;
 * } aes_siv_test_case;
 */
int siv_wycheproof_test(void)
{
   unsigned long n, buflen;
   unsigned char buf[MAXBLOCKSIZE];
   const unsigned char *aad[2] = {0};
   unsigned long aadlen[2] = {0};
   int cipher;
   cipher = find_cipher("aes");
   for (n = 0; n < LTC_ARRAY_SIZE(aes_siv_tests); ++n) {
      XMEMSET(buf, 0, sizeof(buf));
      buflen = sizeof(buf);
      if (aes_siv_tests[n].result == LTC_TEST_CASE_SHOULD_FAIL) {
         SHOULD_FAIL(siv_memory(cipher, LTC_DECRYPT,
                                aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                                aes_siv_tests[n].ct, aes_siv_tests[n].ctLen,
                                buf, &buflen,
                                1,
                                aes_siv_tests[n].aad, aes_siv_tests[n].aadLen,
                                LTC_NULL));
         XMEMSET(buf, 0, sizeof(buf));
         buflen = sizeof(buf);
         aad[0] = aes_siv_tests[n].aad;
         aadlen[0] = aes_siv_tests[n].aadLen;
         SHOULD_FAIL(siv_decrypt_memory(cipher,
                                        aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                                        1,
                                        (const unsigned char **)aad, aadlen,
                                        aes_siv_tests[n].ct, aes_siv_tests[n].ctLen,
                                        buf, &buflen));
      } else {
         DO(siv_memory(cipher, LTC_ENCRYPT,
                       aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                       aes_siv_tests[n].msg, aes_siv_tests[n].msgLen,
                       buf, &buflen,
                       1,
                       aes_siv_tests[n].aad, aes_siv_tests[n].aadLen,
                       LTC_NULL));
         COMPARE_TESTVECTOR(buf, buflen, aes_siv_tests[n].ct, aes_siv_tests[n].ctLen, aes_siv_tests[n].comment, aes_siv_tests[n].tcId);
         XMEMSET(buf, 0, sizeof(buf));
         buflen = sizeof(buf);
         DO(siv_memory(cipher, LTC_DECRYPT,
                       aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                       aes_siv_tests[n].ct, aes_siv_tests[n].ctLen,
                       buf, &buflen,
                       1,
                       aes_siv_tests[n].aad, aes_siv_tests[n].aadLen,
                       LTC_NULL));
         COMPARE_TESTVECTOR(buf, buflen, aes_siv_tests[n].msg, aes_siv_tests[n].msgLen, aes_siv_tests[n].comment, -aes_siv_tests[n].tcId);

         XMEMSET(buf, 0, sizeof(buf));
         buflen = sizeof(buf);
         aad[0] = aes_siv_tests[n].aad;
         aadlen[0] = aes_siv_tests[n].aadLen;
         DO(siv_encrypt_memory(cipher,
                               aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                               1,
                               (const unsigned char **)aad, aadlen,
                               aes_siv_tests[n].msg, aes_siv_tests[n].msgLen,
                               buf, &buflen));
         COMPARE_TESTVECTOR(buf, buflen, aes_siv_tests[n].ct, aes_siv_tests[n].ctLen, aes_siv_tests[n].comment, aes_siv_tests[n].tcId);
         buflen = sizeof(buf);
         DO(siv_decrypt_memory(cipher,
                               aes_siv_tests[n].key, aes_siv_tests[n].keyLen,
                               1,
                               (const unsigned char **)aad, aadlen,
                               aes_siv_tests[n].ct, aes_siv_tests[n].ctLen,
                               buf, &buflen));
         COMPARE_TESTVECTOR(buf, buflen, aes_siv_tests[n].msg, aes_siv_tests[n].msgLen, aes_siv_tests[n].comment, -aes_siv_tests[n].tcId);
      }
   }
   return CRYPT_OK;
}

#else
LTC_NOP_TEST(siv_wycheproof_test)
#endif
