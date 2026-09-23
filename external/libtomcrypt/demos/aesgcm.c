/* LibTomCrypt, modular cryptographic library -- Tom St Denis */
/* SPDX-License-Identifier: Unlicense */

/**
  @file aesgcm.c
  AES128-GCM demo - file en-&decryption, Steffen Jaeckel
  Uses the format: |ciphertext|tag-16-bytes|
*/

#define _GNU_SOURCE

#include <tomcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef LTC_GCM_MODE
int main(void)
{
   return -1;
}
#else

#include "gcm-file/gcm_filehandle.c"
#include "gcm-file/gcm_file.c"


static off_t fsize(const char *filename)
{
   struct stat st;

   if (stat(filename, &st) == 0) return st.st_size;

   return -1;
}

#if defined(__linux__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 14)
#define HAS_SYNCFS
#endif
#endif

static int mv(const char *old_name, const char *new_name)
{
   int fd;
   if (rename(old_name, new_name) == -1) return -1;
   fd = open(new_name, 0);
   if (fd == -1) return -1;
#if !defined(_WIN32)
   if (fsync(fd) != 0) goto OUT;
#if defined(HAS_SYNCFS)
   syncfs(fd);
#else
   sync();
#endif
OUT:
#endif
   close(fd);
   return 0;
}

static void LTC_NORETURN die(int ret)
{
   fprintf(stderr, "Usage: aesgcm <-e|-d> <infile> <outfile> <88|96 char hex-string 'IV | key'>\n");
   exit(ret);
}

int main(int argc, char **argv)
{
   int ret = 0, err, arg, direction, res, tmp;
   size_t keylen;
   uint8_t keybuf[48] = {0};
   char *out = NULL;
   const char *mode, *in_file, *out_file, *key_string;
   unsigned long ivlen, key_len;

   if (argc < 5) {
      if (argc > 1 && strstr(argv[1], "-h"))
         die(0);
      else
         die(__LINE__);
   }

   arg = 1;
   mode = argv[arg++];
   in_file = argv[arg++];
   out_file = argv[arg++];
   key_string = argv[arg++];

   if(strcmp(mode, "-d") == 0) direction = GCM_DECRYPT;
   else if(strcmp(mode, "-e") == 0) direction = GCM_ENCRYPT;
   else die(__LINE__);

   if (fsize(in_file) <= 0) die(__LINE__);

   keylen = XSTRLEN(key_string);
   if (keylen != 88 && keylen != 96) die(__LINE__);

   key_len = sizeof(keybuf);
   if ((err = base16_decode(key_string, keylen, keybuf, &key_len)) != CRYPT_OK) {
      fprintf(stderr, "boooh %s\n", error_to_string(err));
      die(__LINE__);
   }

   register_all_ciphers();

   if(asprintf(&out, "%s-XXXXXX", out_file) < 0) die(__LINE__);
   if((tmp = mkstemp(out)) == -1) {
      ret = __LINE__;
      goto cleanup;
   }
   close(tmp);
   ivlen = keylen/2 - 32;
   if((err = gcm_file(find_cipher("aes"), &keybuf[ivlen], 32, keybuf, ivlen, NULL, 0, in_file, out, 16, direction, &res)) != CRYPT_OK) {
      fprintf(stderr, "boooh %s\n", error_to_string(err));
      ret = __LINE__;
      goto cleanup;
   }

   if(res != 1) {
      ret = __LINE__;
   }
   else
   {
      if (mv(out, out_file) != 0) ret = __LINE__;
   }

cleanup:
   if(ret != 0) unlink(out);
   free(out);


   return ret;
}
#endif
