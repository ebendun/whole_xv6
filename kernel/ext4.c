#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Minimal ext4 reader (read-only) for simple probing and root listing.
// Limitations/assumptions:
// - block size == 4096 (BSIZE)
// - inode size == 128
// - single block group
// - only supports direct blocks for files/dirs

struct ext4_super {
  uint32 s_inodes_count;
  uint32 s_blocks_count_lo;
  uint32 s_log_block_size;
  uint32 s_blocks_per_group;
  uint32 s_inode_size; // we'll read if present
  uint16 s_magic;
  uint32 s_inodes_per_group;
};

static struct ext4_super sb;
static int ext4_present = 0;

static uint16
get_u16(uchar *p) {
  return (uint16)p[0] | ((uint16)p[1] << 8);
}
static uint32
get_u32(uchar *p) {
  return (uint32)p[0] | ((uint32)p[1]<<8) | ((uint32)p[2]<<16) | ((uint32)p[3]<<24);
}

int ext4_read_super(int dev)
{
  // superblock lives at byte offset 1024 from filesystem start.
  int sb_byte_off = 1024;
  int dev_blk = sb_byte_off / BSIZE;
  int off_in_blk = sb_byte_off % BSIZE;
  struct buf *b = bread(dev, dev_blk);
  if(!b) return -1;
  uchar *s = b->data + off_in_blk;

  sb.s_inodes_count = get_u32(s + 0);
  sb.s_blocks_count_lo = get_u32(s + 4);
  sb.s_log_block_size = get_u32(s + 24);
  sb.s_blocks_per_group = get_u32(s + 32);
  sb.s_inodes_per_group = get_u32(s + 40);
  sb.s_magic = get_u16(s + 56);
  // inode size is at offset 88 in ext2/3/4; if zero, default 128
  sb.s_inode_size = get_u16(s + 88);
  if(sb.s_inode_size == 0) sb.s_inode_size = 128;

  brelse(b);

  if(sb.s_magic != 0xEF53)
    return -1;
  // check block size
  uint32 fs_bsize = 1024u << sb.s_log_block_size;
  if(fs_bsize % BSIZE != 0){
    // only support filesystem block sizes that are multiples of kernel BSIZE
    return -1;
  }

  ext4_present = 1;
  return 0;
}

// Read group descriptor 0 and return inode table start block
// Read group descriptor for `group` (block group number) and return
// the inode table start as a filesystem-block number.
static uint32 ext4_get_inode_table_block(int dev, uint32 group)
{
  uint32 fs_bsize = 1024u << sb.s_log_block_size;
  uint32 ratio = fs_bsize / BSIZE; // device blocks per fs-block

  uint32 sb_fsblk = 1024 / fs_bsize; // superblock fs-block number
  uint32 gd_fsblk = sb_fsblk + 1; // first fs-block of group descriptors

  // size of a group descriptor; ext4 can have 64, use 64 which matches dumpe2fs
  uint32 gd_size = 64;

  // byte offset of desired descriptor within GDT
  uint64 desc_byte_off = (uint64)group * gd_size;
  uint32 desc_dev_blk = gd_fsblk * ratio + (desc_byte_off / BSIZE);
  uint32 off_in_blk = desc_byte_off % BSIZE;

  struct buf *b = bread(dev, desc_dev_blk);
  if(!b) return 0;
  uint32 inode_table = get_u32(b->data + off_in_blk + 8); // bg_inode_table (low 32 bits)
  brelse(b);
  return inode_table;
}

// Read raw inode into buf (caller supplies inode_no, starting from 1)
int ext4_read_inode(int dev, uint32 inode_no, uchar *out, uint32 outsz)
{
  if(!ext4_present) return -1;
  uint32 fs_bsize = 1024u << sb.s_log_block_size;
  uint32 ratio = fs_bsize / BSIZE;

  uint32 idx = inode_no - 1;
  uint32 group = idx / sb.s_inodes_per_group;
  uint32 itable_fs = ext4_get_inode_table_block(dev, group);
  if(itable_fs == 0) return -1;

  uint32 inodes_per_fs_block = fs_bsize / sb.s_inode_size;
  uint32 fs_block_offset = (idx % sb.s_inodes_per_group) / inodes_per_fs_block;
  uint32 within_fs_block = (idx % sb.s_inodes_per_group) % inodes_per_fs_block;
  uint32 offset_in_fs_block = within_fs_block * sb.s_inode_size;

  uint32 dev_block = (itable_fs + fs_block_offset) * ratio + (offset_in_fs_block / BSIZE);
  uint32 off = offset_in_fs_block % BSIZE;

  struct buf *b = bread(dev, dev_block);
  if(!b) return -1;
  uint32 tocopy = sb.s_inode_size;
  if(tocopy > outsz) tocopy = outsz;
  if(off + tocopy <= BSIZE){
    memmove(out, b->data + off, tocopy);
    brelse(b);
    return 0;
  }
  uint32 first = BSIZE - off;
  memmove(out, b->data + off, first);
  brelse(b);
  struct buf *b2 = bread(dev, dev_block + 1);
  if(!b2) return -1;
  memmove(out + first, b2->data, tocopy - first);
  brelse(b2);
  return 0;
}

// Read file data from inode into buffer, supports only direct blocks
int ext4_read_data(int dev, uchar *inode_buf, uchar *dst, uint32 len, uint32 offset)
{
  // inode layout: direct blocks start at offset 40 (15 * 4 bytes)
  uint32 direct[12];
  for(int i=0;i<12;i++)
    direct[i] = get_u32(inode_buf + 40 + i*4);
  uint32 fs_bsize = 1024u << sb.s_log_block_size;
  uint32 ratio = fs_bsize / BSIZE;

  uint32 blockno = offset / fs_bsize; // filesystem-block index within file
  uint32 off = offset % fs_bsize;
  uint32 copied = 0;
  while(copied < len){
    if(blockno >= 12) return copied; // no indirect support in this minimal impl
    uint32 phys = direct[blockno]; // phys is fs-block number
    if(phys == 0) break;
    // map filesystem block to device block
    uint32 dev_block = phys * ratio + (off / BSIZE);
    uint32 off_in_dev = off % BSIZE;
    struct buf *b = bread(dev, dev_block);
    if(!b) break;
    // amt from current device block
    uint32 amt = BSIZE - off_in_dev;
    if(amt > (len - copied)) amt = len - copied;
    memmove(dst + copied, b->data + off_in_dev, amt);
    brelse(b);
    copied += amt;
    blockno++;
    off = 0;
  }
  return copied;
}

// List root directory entries and print to console
void ext4_list_root(int dev)
{
  uchar inode_buf[256];
  if(ext4_read_inode(dev, 2, inode_buf, sizeof(inode_buf)) < 0){
    printf("ext4: failed read root inode\n");
    return;
  }
  // iterate direct blocks
  for(int di = 0; di < 12; di++){
    uint32 blk = get_u32(inode_buf + 40 + di*4);
    if(blk == 0) continue;
    uint32 fs_bsize = 1024u << sb.s_log_block_size;
    uint32 ratio = fs_bsize / BSIZE;
    if(ratio == 0 || fs_bsize > 8192) continue; // safety
    uchar *fsbuf = (uchar*)kalloc();
    if(!fsbuf) continue;
    // read full filesystem block (fs_bsize) by reading device blocks
    for(uint32 i = 0; i < ratio; i++){
      struct buf *b2 = bread(dev, blk * ratio + i);
      if(!b2) break;
      memmove(fsbuf + i * BSIZE, b2->data, BSIZE);
      brelse(b2);
    }
    uint32 off = 0;
    while(off < fs_bsize){
      uint32 inode = get_u32(fsbuf + off + 0);
      uint16 rec_len = get_u16(fsbuf + off + 4);
      uint8 name_len = fsbuf[off + 6];
      if(inode != 0 && name_len > 0){
        char name[256];
        uint32 cn = name_len; if(cn > 255) cn = 255;
        memmove(name, fsbuf + off + 8, cn);
        name[cn] = '\0';
        printf("ext4: |%s|\n", name);
      }
      if(rec_len == 0) break;
      off += rec_len;
    }
    kfree((void*)fsbuf);
  }
}

int ext4_read_file_by_path(int dev, const char *path, uchar *dst, uint32 len)
{
  if(!ext4_present) return -1;
  // only support absolute paths like /foo
  if(path[0] != '/') return -1;
  // tokenize single-level or nested paths
  char pcopy[256];
  safestrcpy(pcopy, path+1, sizeof(pcopy)); // skip leading '/'

  uint32 inode_no = 2; // start at root
  char *tok = pcopy;
  char *save;
  for(;;){
    char *component = tok;
    // find separator
    char *sep = 0;
    for(char *c = tok; *c; c++) if(*c == '/') { sep = c; break; }
    if(sep){ *sep = '\0'; save = sep+1; }
    else save = 0;

    // read inode for current inode_no
    uchar inode_buf[256];
    if(ext4_read_inode(dev, inode_no, inode_buf, sizeof(inode_buf)) < 0) return -1;
    // search directory entries for component
    int found = 0;
    for(int di=0; di<12; di++){
      uint32 blk = get_u32(inode_buf + 40 + di*4);
      if(blk == 0) continue;
      uint32 fs_bsize = 1024u << sb.s_log_block_size;
      uint32 ratio = fs_bsize / BSIZE;
      if(ratio == 0 || fs_bsize > 8192) continue;
      uchar *fsbuf = (uchar*)kalloc();
      if(!fsbuf) continue;
      for(uint32 i = 0; i < ratio; i++){
        struct buf *b = bread(dev, blk * ratio + i);
        if(!b) break;
        memmove(fsbuf + i * BSIZE, b->data, BSIZE);
        brelse(b);
      }
      uint32 off = 0;
      while(off < fs_bsize){
        uint32 ino = get_u32(fsbuf + off + 0);
        uint16 rec_len = get_u16(fsbuf + off + 4);
        uint8 name_len = fsbuf[off + 6];
        if(ino != 0 && name_len > 0){
          char name[256];
          uint32 cn = name_len; if(cn > 255) cn = 255;
          memmove(name, fsbuf + off + 8, cn);
          name[cn] = '\0';
          if(strncmp(name, component, cn) == 0 && strlen(component) == cn){
            inode_no = ino;
            found = 1;
            break;
          }
        }
        if(rec_len == 0) break;
        off += rec_len;
      }
      kfree((void*)fsbuf);
      if(found) break;
    }
    if(!found) return -1;
    if(!save) break; // last component
    tok = save;
  }
  // inode_no now the target file
  uchar inode_buf2[256];
  if(ext4_read_inode(dev, inode_no, inode_buf2, sizeof(inode_buf2)) < 0) return -1;
  // read file size at offset 4? ext2 i_size is at offset 4 (32-bit)
  uint32 isize = get_u32(inode_buf2 + 4);
  if(len > isize) len = isize;
  return ext4_read_data(dev, inode_buf2, dst, len, 0);
}

int ext4_is_present(void){ 
  return ext4_present;
}