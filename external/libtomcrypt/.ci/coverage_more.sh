#!/bin/bash

set -e

function pdiv() {
  printf "\n====== %s ======\n" "$*"
}

if [ "$#" = "1" -a "$(echo $1 | grep 'gmp')" != "" ]; then
   pdiv "Test GMP"
   ./test t gmp
fi

pdiv "Sizes"
./sizes
pdiv "Constants"
./constants

pdiv "Generate hashsum_tv.txt"
for i in $(for j in $(echo $(./hashsum -h | awk '/Algorithms/,EOF' | tail -n +2)); do echo $j; done | sort); do
  echo -n "$i: " && ./hashsum -a $i tests/test.key
done > hashsum_tv.txt

pdiv "Compare hashsum_tv.txt"
difftroubles=$(diff -i -w -B hashsum_tv.txt notes/hashsum_tv.txt | grep '^<') || true
if [ -n "$difftroubles" ]; then
  echo "FAILURE: hashsum_tv.tx"
  diff -i -w -B hashsum_tv.txt notes/hashsum_tv.txt
  echo "hashsum failed"
  exit 1
else
  echo "hashsum okay"
fi

pdiv "aesgcm"
./hashsum -a sha256 aesgcm > aesgcm.sha256sum
./aesgcm -e aesgcm aesgcm.enc0 "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"
./aesgcm -d aesgcm.enc0 aesgcm.dec0 "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"
./aesgcm -e aesgcm aesgcm.enc1 "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899AABB"
./aesgcm -d aesgcm.enc1 aesgcm.dec1 "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899AABB"
for i in {0..1}; do
  rm aesgcm
  mv aesgcm.dec${i} aesgcm
  ./hashsum -c aesgcm.sha256sum
done
chmod +x aesgcm

exit 0
