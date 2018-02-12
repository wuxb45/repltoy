#include <stdio.h>
#include <stdlib.h>

  int
main(int argc, char ** argv)
{
  if (argc != 2) return 0;
  const int nr = atoi(argv[1]);
  unsigned int max = 0;
  for (int i = 0; i < nr; i++) {
    unsigned int v = (random() % 103) * (random() % 107) * (random() % 111) / 10000;
    if (max < v) max = v;
    fwrite(&v, sizeof(unsigned int), 1, stdout);
  }
  max++;
  fwrite(&max, sizeof(unsigned int), 1, stdout);
  return 0;
}
