// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/stat.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10
#define HISTMAX 16

struct linux_dirent64 {
  uint64 d_ino;
  uint64 d_off;
  ushort d_reclen;
  uchar d_type;
  char d_name[1];
};

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

static char history[HISTMAX][100];
static int nhistory;

int
hasprefix(char *s, char *prefix)
{
  while(*prefix){
    if(*s++ != *prefix++)
      return 0;
  }
  return 1;
}

void
redraw(char *buf, int len)
{
  write(2, "\r$ ", 3);
  if(len > 0)
    write(2, buf, len);
  write(2, "\033[K", 3);
}

void
remember(char *buf)
{
  int len, i;

  len = strlen(buf);
  if(len > 0 && buf[len-1] == '\n')
    len--;
  if(len == 0)
    return;
  if(nhistory > 0 && strlen(history[nhistory-1]) == len &&
     memcmp(history[nhistory-1], buf, len) == 0)
    return;
  if(nhistory == HISTMAX){
    for(i = 1; i < HISTMAX; i++)
      strcpy(history[i-1], history[i]);
    nhistory--;
  }
  memmove(history[nhistory], buf, len);
  history[nhistory][len] = 0;
  nhistory++;
}

int
match_dir(char *dir, char *prefix, char *match, int msz)
{
  char dbuf[512];
  int fd, nread, bpos, nmatch;

  nmatch = 0;
  fd = open(dir, O_RDONLY);
  if(fd < 0)
    return 0;
  while((nread = __sys_getdents64(fd, dbuf, sizeof(dbuf))) > 0){
    for(bpos = 0; bpos < nread; ){
      struct linux_dirent64 *d = (struct linux_dirent64 *)(dbuf + bpos);
      bpos += d->d_reclen;
      if(d->d_ino == 0 || d->d_name[0] == '.')
        continue;
      if(hasprefix(d->d_name, prefix)){
        if(nmatch == 0){
          if(strlen(d->d_name) < msz)
            strcpy(match, d->d_name);
        } else {
          if(nmatch == 1)
            write(2, "\n", 1);
          write(2, d->d_name, strlen(d->d_name));
          write(2, "  ", 2);
        }
        nmatch++;
      }
    }
  }
  close(fd);
  if(nmatch > 1)
    write(2, "\n", 1);
  return nmatch;
}

void
complete(char *buf, int *len, int nbuf)
{
  char dir[64], prefix[64], match[64];
  int start, slash, i, nmatch, oldlen;

  start = *len;
  while(start > 0 && buf[start-1] != ' ' && buf[start-1] != '\t')
    start--;

  slash = -1;
  for(i = start; i < *len; i++){
    if(buf[i] == '/')
      slash = i;
  }

  if(slash >= 0){
    oldlen = slash - start;
    if(oldlen == 0){
      strcpy(dir, "/");
    } else if(oldlen < sizeof(dir)){
      memmove(dir, buf + start, oldlen);
      dir[oldlen] = 0;
    } else {
      return;
    }
    oldlen = *len - slash - 1;
    if(oldlen >= sizeof(prefix))
      return;
    memmove(prefix, buf + slash + 1, oldlen);
    prefix[oldlen] = 0;
  } else {
    oldlen = *len - start;
    if(oldlen >= sizeof(prefix))
      return;
    memmove(prefix, buf + start, oldlen);
    prefix[oldlen] = 0;
    if(start == 0)
      strcpy(dir, "/bin");
    else
      strcpy(dir, ".");
  }

  match[0] = 0;
  nmatch = match_dir(dir, prefix, match, sizeof(match));
  if(nmatch == 1){
    oldlen = strlen(prefix);
    for(i = oldlen; match[i] && *len < nbuf - 2; i++){
      buf[(*len)++] = match[i];
      write(2, &match[i], 1);
    }
    if(*len < nbuf - 2){
      buf[(*len)++] = ' ';
      write(2, " ", 1);
    }
    buf[*len] = 0;
  } else if(nmatch > 1) {
    redraw(buf, *len);
  }
}

int
looks_elf(char *path)
{
  int fd;
  char magic[4];

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return 0;
  if(read(fd, magic, sizeof(magic)) != sizeof(magic)){
    close(fd);
    return 0;
  }
  close(fd);
  return magic[0] == 0x7f && magic[1] == 'E' &&
         magic[2] == 'L' && magic[3] == 'F';
}

void
runline(char *buf)
{
  char *cmd = buf;
  while (*cmd == ' ' || *cmd == '\t')
    cmd++;
  if (*cmd == '\n' || *cmd == 0 || *cmd == '#')
    return;
  if(cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' '){
    cmd[strlen(cmd)-1] = 0;
    if(chdir(cmd+3) < 0)
      fprintf(2, "cannot cd %s\n", cmd+3);
  } else {
    if(fork1() == 0)
      runcmd(parsecmd(cmd));
    wait(0);
  }
}

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    if(looks_elf(ecmd->argv[0])){
      fprintf(2, "exec %s failed\n", ecmd->argv[0]);
      break;
    }
    char *sargv[MAXARGS];
    int i;
    sargv[0] = "sh";
    sargv[1] = ecmd->argv[0];
    for(i = 1; ecmd->argv[i] && i + 1 < MAXARGS - 1; i++)
      sargv[i + 1] = ecmd->argv[i];
    sargv[i + 1] = 0;
    exec("sh", sargv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    close(rcmd->fd);
    if(open(rcmd->file, rcmd->mode) < 0){
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  char c;
  int len, nav;

  memset(buf, 0, nbuf);
  len = 0;
  nav = nhistory;
  write(2, "$ ", 2);
  for(;;){
    if(read(0, &c, 1) != 1)
      return -1;
    if(c == 4)
      return -1;
    if(c == '\n' || c == '\r'){
      write(2, "\n", 1);
      if(len < nbuf - 1)
        buf[len++] = '\n';
      buf[len] = 0;
      remember(buf);
      return 0;
    }
    if(c == '\t'){
      complete(buf, &len, nbuf);
      nav = nhistory;
      continue;
    }
    if(c == 21){
      len = 0;
      buf[0] = 0;
      redraw(buf, len);
      nav = nhistory;
      continue;
    }
    if(c == 8 || c == 127){
      if(len > 0){
        len--;
        buf[len] = 0;
        write(2, "\b \b", 3);
      }
      nav = nhistory;
      continue;
    }
    if(c == '\033'){
      char seq[2];
      if(read(0, &seq[0], 1) != 1 || read(0, &seq[1], 1) != 1)
        continue;
      if(seq[0] == '[' && seq[1] == 'A' && nhistory > 0){
        if(nav > 0)
          nav--;
        strcpy(buf, history[nav]);
        len = strlen(buf);
        redraw(buf, len);
      } else if(seq[0] == '[' && seq[1] == 'B' && nhistory > 0){
        if(nav < nhistory)
          nav++;
        if(nav == nhistory){
          len = 0;
          buf[0] = 0;
        } else {
          strcpy(buf, history[nav]);
          len = strlen(buf);
        }
        redraw(buf, len);
      }
      continue;
    }
    if(c >= ' ' && c < 0x7f && len < nbuf - 2){
      buf[len++] = c;
      buf[len] = 0;
      write(2, &c, 1);
      nav = nhistory;
    }
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  static char buf[100];
  int fd;
  int n;
  char c;

  if(argc > 1){
    if((fd = open(argv[1], O_RDONLY)) < 0){
      fprintf(2, "sh: cannot open %s\n", argv[1]);
      exit(1);
    }
    n = 0;
    while(read(fd, &c, 1) == 1){
      if(n < sizeof(buf) - 1)
        buf[n++] = c;
      if(c == '\n'){
        buf[n] = 0;
        runline(buf);
        n = 0;
      }
    }
    if(n > 0){
      buf[n++] = '\n';
      buf[n] = 0;
      runline(buf);
    }
    close(fd);
    exit(0);
  }

  // Ensure that three file descriptors are open.
  while((fd = open("/dev/console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    runline(buf);
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
