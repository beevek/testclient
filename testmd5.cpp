/*
  Copyright 2008-2013 Kristopher R Beevers and Internap Network
  Services Corporation.

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <stdio.h>
#include <openssl/evp.h>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>


void md5_compute(FILE *f, int start, int end, std::string &md5)
{
  // initialize openssl md5 digest
  static unsigned char data[102400]; // 100K buffer to read from file
  static unsigned char md_val[EVP_MAX_MD_SIZE];
  unsigned int md_len;
  EVP_MD_CTX mdctx;
  const EVP_MD *md = EVP_md5();
  EVP_MD_CTX_init(&mdctx);
  EVP_DigestInit_ex(&mdctx, md, NULL);

  // read data in chunks from the file and update the digest
  size_t rv;
  fseek(f, start, SEEK_SET);
  do {
    rv = end - start + 1;
    if(rv > 102400)
      rv = 102400;
    rv = fread(data, 1, rv, f);
    if(rv == 0)
      break;
    EVP_DigestUpdate(&mdctx, data, rv);
    start += rv;
  } while(start <= end);

  // finalize the digest
  EVP_DigestFinal_ex(&mdctx, md_val, &md_len);
  EVP_MD_CTX_cleanup(&mdctx);

  // output
  md5.reserve(64);
  char digit[3];
  for(unsigned int i = 0; i < md_len; ++i) {
    sprintf(digit, "%02x", md_val[i]);
    md5.append(digit);
  }
}

int main(int argc, char **argv)
{
  if(argc < 2)
    return 1;
  FILE *f = fopen(argv[1], "rb");
  if(!f)
    return 1;
  struct stat st;
  fstat(fileno(f), &st);
  std::string md5;
  if(argc > 3)
    md5_compute(f, atoi(argv[2]), atoi(argv[3]), md5);
  else
    md5_compute(f, 0, st.st_size-1, md5);
  printf("%s\n", md5.c_str());
  return 0;
}
