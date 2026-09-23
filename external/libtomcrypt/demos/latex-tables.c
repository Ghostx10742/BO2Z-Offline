/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */
/* print all PEM related infos */
#include "tomcrypt_private.h"

#if defined(LTC_PEM_SSH)
extern const struct blockcipher_info pem_dek_infos[];
extern const unsigned long pem_dek_infos_num;

extern const struct blockcipher_info ssh_ciphers[];
extern const unsigned long ssh_ciphers_num;

static const struct {
   const char *is, *should;
} cipher_name_map[] = {
   { "", "none" },
   { "aes", "AES" },
   { "blowfish", "Blowfish" },
   { "c20p1305", "ChaCha20Poly1305" },
   { "camellia", "Camellia" },
   { "cast5", "CAST5" },
   { "chacha20", "ChaCha20" },
   { "3des", "3DES (EDE)" },
   { "des", "DES" },
   { "desx", "DES-X" },
   { "idea", "IDEA" },
   { "rc5", "RC5" },
   { "rc2", "RC2" },
   { "seed", "SEED" },
   { "serpent", "Serpent" },
   { "twofish", "Twofish" },
};

static const char *s_map_cipher(const char *name)
{
   unsigned long n;
   for (n = 0; n < LTC_ARRAY_SIZE(cipher_name_map); ++n) {
      if (strcmp(name, cipher_name_map[n].is) == 0)
         return cipher_name_map[n].should;
   }
   fprintf(stderr, "Error: Can't map %s\n", name);
   exit(1);
}

static const struct {
   enum cipher_mode mode;
   const char *name;
} cipher_mode_map[] = {
   { cm_none,   "none",   },
   { cm_cbc,    "CBC",    },
   { cm_cfb,    "CFB",    },
   { cm_cfb1,   "CFB1",   },
   { cm_cfb8,   "CFB8",   },
   { cm_ctr,    "CTR",    },
   { cm_ofb,    "OFB",    },
   { cm_stream, "STREAM", },
   { cm_gcm,    "GCM",    },
};

static const char *s_map_mode(enum cipher_mode mode)
{
   size_t n;
   mode &= cm_modes | cm_1bit | cm_8bit;
   for (n = 0; n < LTC_ARRAY_SIZE(cipher_mode_map); ++n) {
      if (cipher_mode_map[n].mode == mode)
         return cipher_mode_map[n].name;
   }
   fprintf(stderr, "Error: Can't map cipher_mode %d\n", mode);
   exit(1);
}


static void LTC_NORETURN die(int status)
{
   FILE* o = status == EXIT_SUCCESS ? stdout : stderr;
   fprintf(o,
         "Usage: latex-tables [<-h>]\n\n"
         "Generate LaTeX tables from some library internal data.\n\n"
         "\t-h\tThe help you're looking at.\n"
   );
   exit(status);
}

static int s_to_lower(const char *in, char *out, unsigned long *outlen)
{
   unsigned long n;
   for (n = 0; n < *outlen && in[n]; ++n) {
      out[n] = tolower(in[n]);
   }
   if (n == *outlen)
      return CRYPT_BUFFER_OVERFLOW;
   out[n] = '\0';
   *outlen = n;
   return CRYPT_OK;
}

int main(int argc, char **argv)
{
   unsigned long n;
   if (argc > 1 && strstr(argv[1], "-h"))
      die(0);
   printf("PEM ciphers:\n\n");
   for (n = 0; n < pem_dek_infos_num; ++n) {
      char nbuf[32] = {0};
      size_t nlen = strlen(pem_dek_infos[n].name);
      memcpy(nbuf, pem_dek_infos[n].name, nlen);
      nbuf[nlen-1] = '}';
      printf("\\hline \\texttt{%-18s & %-15s & %-25ld & %-6s \\\\\n",
                               nbuf, s_map_cipher(pem_dek_infos[n].algo),
                                              pem_dek_infos[n].keylen * 8,
                                                       s_map_mode(pem_dek_infos[n].mode));
   }

   printf("\nSSH ciphers:\n\n");
   for (n = 0; n < ssh_ciphers_num; ++n) {
      char nbuf[32] = {0};
      size_t nlen = strlen(ssh_ciphers[n].name);
      memcpy(nbuf, ssh_ciphers[n].name, nlen);
      nbuf[nlen] = '}';
      printf("\\hline \\texttt{%-30s & %-16s & %-24ld & %-6s \\\\\n",
                               nbuf, s_map_cipher(ssh_ciphers[n].algo),
                               ssh_ciphers[n].keylen * 8,
                                                       s_map_mode(ssh_ciphers[n].mode));
   }

   printf("\nECC curves:\n\n");
   for (n = 0; ltc_ecc_curves[n].OID != NULL; ++n) {
      const char * const *names;
      char lower[32] = {0}, buf[64] = {0};
      unsigned long m, bufl = 0, lowerl;
      int err = ecc_get_curve_names(ltc_ecc_curves[n].OID, &names);
      if (err != CRYPT_OK) {
         printf("\\error: OID %s not found (%s)\n", ltc_ecc_curves[n].OID, error_to_string(err));
         return EXIT_FAILURE;
      }
      for (m = 1; names[m]; ++m) {
         const char *name = names[m];
         if (memcmp(name, "P-", 2) == 0 || memcmp(name, "ECC-", 4) == 0) {
            /* Use the original name */
         } else {
            lowerl = sizeof(lower);
            if ((err = s_to_lower(name, lower, &lowerl)) != CRYPT_OK) {
               printf("\\error: %s could not be converted to lowercase (%s)\n", name, error_to_string(err));
               return EXIT_FAILURE;
            }
            name = lower;
         }
         if (m == 1) {
            err = snprintf(buf + bufl, sizeof(buf) - bufl, "%s", name);
         } else {
            err = snprintf(buf + bufl, sizeof(buf) - bufl, ", %s", name);
         }
         if (err == -1 || (unsigned)err > sizeof(buf) - bufl) {
            printf("\\error: snprintf returned %d at %s\n", err, name);
            return EXIT_FAILURE;
         }
         bufl += err;
      }
      lower[0] = '{';
      lowerl = sizeof(lower) - 2;
      if ((err = s_to_lower(names[0], &lower[1], &lowerl)) != CRYPT_OK) {
         printf("\\error: %s could not be converted to lowercase (%s)\n", names[0], error_to_string(err));
         return EXIT_FAILURE;
      }
      lower[lowerl + 1] = '}';
      lower[lowerl + 2] = '\0';
      printf("\\hline \\texttt%-17s & %-36s & %-21s \\\\\n", lower, buf, ltc_ecc_curves[n].OID);
   }

   return 0;
}
#else
int main(void) { return EXIT_FAILURE; }
#endif
