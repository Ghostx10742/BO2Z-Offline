/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */
/* small demo app that just includes a cipher/hash/prng */
#include <tomcrypt.h>

int main(void)
{
#ifdef LTC_RIJNDAEL
#ifdef ENCRYPT_ONLY
   register_cipher(&rijndael_enc_desc);
#endif
#endif
   register_prng(&yarrow_desc);
   register_hash(&sha256_desc);
   return 0;
}
