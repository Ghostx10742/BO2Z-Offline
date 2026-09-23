/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */

#include  <tomcrypt_test.h>

int pk_oid_test(void)
{
   const char *oid_str = "1.2.3.4.5";
   const unsigned long oid_ul[] = { 1, 2, 3, 4, 5 };
   char str[16];
   unsigned long buf[6], num = LTC_ARRAY_SIZE(oid_ul), strlen = sizeof(str), should_size = 0;

   SHOULD_FAIL_WITH(pk_oid_str_to_num(oid_str, NULL, &should_size), CRYPT_BUFFER_OVERFLOW);
   ENSURE(should_size == 5);

   DO(pk_oid_str_to_num(oid_str, buf, &num));
   ENSURE(num == 5);

   should_size = 1;
   SHOULD_FAIL_WITH(pk_oid_num_to_str(oid_ul, 5, str, &should_size), CRYPT_BUFFER_OVERFLOW);
   ENSURE(should_size == 10);
   should_size = 1;
   SHOULD_FAIL_WITH(pk_oid_num_to_str(oid_ul, 5, NULL, &should_size), CRYPT_BUFFER_OVERFLOW);
   ENSURE(should_size == 10);
   should_size = 16;
   SHOULD_FAIL_WITH(pk_oid_num_to_str(oid_ul, 5, NULL, &should_size), CRYPT_BUFFER_OVERFLOW);
   ENSURE(should_size == 10);

   XMEMSET(str, 'a', sizeof(str));
   DO(pk_oid_num_to_str(oid_ul, 5, str, &strlen));
   ENSURE(strlen == 10);
   ENSURE(XMEMCMP(str, oid_str, strlen) == 0);

   XMEMSET(str, 'a', sizeof(str));
   strlen = 10;
   DO(pk_oid_num_to_str(oid_ul, 5, str, &strlen));
   ENSURE(strlen == 10);
   ENSURE(XMEMCMP(str, oid_str, strlen) == 0);

   return 0;
}
