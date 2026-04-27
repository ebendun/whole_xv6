#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TOKMAX 256

static void
emit_if_match(char *tok, int toklen, int alldigits, int overflow)
{
  int n;

  if(toklen == 0 || !alldigits || overflow)
    return;

  tok[toklen] = '\0';
  n = atoi(tok);
  if(n % 5 == 0 || n % 6 == 0)
    printf("%d\n", n);
}

static int
is_sep(char c)
{
  return strchr(" -\r\t\n./,", c) != 0;
}

static void
sixfive(int fd)
{
  char c;
  char tok[TOKMAX];
  int n;
  int toklen;
  int alldigits;
  int overflow;

  toklen = 0;
  alldigits = 1;
  overflow = 0;

  while((n = read(fd, &c, 1)) == 1){
    if(is_sep(c)){
      emit_if_match(tok, toklen, alldigits, overflow);
      toklen = 0;
      alldigits = 1;
      overflow = 0;
      continue;
    }

    if(toklen < TOKMAX - 1)
      tok[toklen] = c;
    else
      overflow = 1;
    toklen++;

    if(c < '0' || c > '9')
      alldigits = 0;
  }

  if(n < 0){
    fprintf(2, "sixfive: read error\n");
    exit(1);
  }

  emit_if_match(tok, toklen, alldigits, overflow);
}

int
main(int argc, char *argv[])
{
  int i;
  int fd;

  if(argc < 2){
    fprintf(2, "Usage: sixfive file...\n");
    exit(1);
  }

  for(i = 1; i < argc; i++){
    fd = open(argv[i], O_RDONLY);
    if(fd < 0){
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      exit(1);
    }
    sixfive(fd);
    close(fd);
  }

  exit(0);
}
