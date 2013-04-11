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
#include <stdlib.h>

// copy a range of bytes from one file to another

int main(int argc, char **argv)
{
  if(argc < 5) // file start end out
    return 1;

  FILE *in = fopen(argv[1], "rb");
  FILE *out = fopen(argv[4], "wb");
  int start = atoi(argv[2]), end = atoi(argv[3]);
  fseek(in, start, SEEK_SET);
  unsigned char *data = new unsigned char[end - start + 1];
  fread(data, 1, end - start + 1, in);
  fwrite(data, 1, end - start + 1, out);
  fclose(in);
  fclose(out);

  return 0;
}
