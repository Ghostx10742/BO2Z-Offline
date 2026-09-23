/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */
#include <tomcrypt_test.h>

#if defined(LTC_PKCS_1)

#include "../notes/rsa-testvectors/pkcs1v15crypt-vectors.c"



int pkcs_1_eme_test(void)
{
  struct ltc_prng_descriptor* no_prng_desc = no_prng_desc_get();
  ltc_rsa_op_parameters rsa_params = {
                                      .wprng = register_prng(no_prng_desc),
                                      .prng = (void*)no_prng_desc,
                                      .params.hash_idx = -1,
                                      .padding = LTC_PKCS_1_V1_5
  };
  unsigned int i, j;
  rsa_params.params.hash_idx = find_hash("sha1");

  if (ltc_mp.name == NULL) return CRYPT_NOP;

  for (i = 0; i < LTC_ARRAY_SIZE(testcases_eme); ++i) {
    testcase_t* t = &testcases_eme[i];
    rsa_key k, *key = &k;
    DOX(rsa_init(key), t->name);

    DOX(ltc_mp_read_unsigned_bin(key->e, t->rsa.e, t->rsa.e_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->d, t->rsa.d, t->rsa.d_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->N, t->rsa.n, t->rsa.n_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->dQ, t->rsa.dQ, t->rsa.dQ_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->dP, t->rsa.dP, t->rsa.dP_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->qP, t->rsa.qInv, t->rsa.qInv_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->q, t->rsa.q, t->rsa.q_l), t->name);
    DOX(ltc_mp_read_unsigned_bin(key->p, t->rsa.p, t->rsa.p_l), t->name);
    key->type = PK_PRIVATE;

    for (j = 0; j < LTC_ARRAY_SIZE(t->data); ++j) {
        rsaData_t* s = &t->data[j];
        unsigned char buf[256], obuf[256];
        unsigned long buflen = sizeof(buf), obuflen = sizeof(obuf);
        int stat;
        prng_descriptor[rsa_params.wprng].add_entropy(s->o2, s->o2_l, (void*)rsa_params.prng);
        DOX(rsa_encrypt_key_v2(s->o1, s->o1_l, obuf, &obuflen, &rsa_params, key), s->name);
        COMPARE_TESTVECTOR(obuf, obuflen, s->o3, s->o3_l,s->name, j);
        DOX(rsa_decrypt_key_v2(obuf, obuflen, buf, &buflen, &rsa_params, &stat, key), s->name);
        ENSUREX(stat == 1, s->name);
    } /* for */

    ltc_mp_deinit_multi(key->d,  key->e, key->N, key->dQ, key->dP, key->qP, key->p, key->q, LTC_NULL);
  } /* for */

  unregister_prng(no_prng_desc);
  no_prng_desc_free(no_prng_desc);

  return 0;
}

#else
LTC_NOP_TEST(pkcs_1_eme_test)
#endif

