#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "fs.h"

// Probe ext4 on FIRSTDEV and do a minimal root listing.
void ext4_init(void)
{
  if(ext4_read_super(FIRSTDEV) < 0){
    printf("ext4: no ext4 filesystem on device %d or unsupported format\n", FIRSTDEV);
    return;
  }
  //printf("ext4: detected ext4 on device %d\n", FIRSTDEV);
  //ext4_list_root(FIRSTDEV);
}
