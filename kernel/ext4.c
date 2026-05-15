#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "ext4.h"
#include "buf.h"
#include "proc.h"

// Read-only ext4 reader used by ext4 probe path.

static struct ext4_superblock sb;
static int ext4_present = 0;

//turn the data to Little-Endian
static uint16
get_u16(uchar *p) {
  return (uint16)p[0] | ((uint16)p[1] << 8);
}
static uint32
get_u32(uchar *p) {
  return (uint32)p[0] | ((uint32)p[1]<<8) | ((uint32)p[2]<<16) | ((uint32)p[3]<<24);
}

static uint64
get_u64_from_pair(uint32 lo, uint32 hi) {
  return (uint64)lo | ((uint64)hi << 32);
}

static uint32
ext4_fs_bsize(void)
{
  return 1024u << sb.s_log_block_size;
}

static int
ext4_inode_mode_is_dir(struct ext4_inode *inode)
{
  uint16 mode = get_u16((uchar *)&inode->i_mode);
  return (mode & EXT4_MODE_DIR) == EXT4_MODE_DIR;
}

static int
ext4_inode_mode_is_reg(struct ext4_inode *inode)
{
  uint16 mode = get_u16((uchar *)&inode->i_mode);
  return (mode & EXT4_MODE_REG) == EXT4_MODE_REG;
}

static uint64
ext4_inode_size(struct ext4_inode *inode)
{
  uint32 lo = get_u32((uchar *)&inode->i_size_lo);
  uint32 hi = get_u32((uchar *)&inode->i_size_high);
  return get_u64_from_pair(lo, hi);
}

static uint32
ext4_inode_flags(struct ext4_inode *inode)
{
  return get_u32((uchar *)&inode->i_flags);
}

static int
ext4_read_fs_block(int dev, uint32 fsblk, uchar *dst)
{
  uint32 ratio = ext4_fs_bsize() / BSIZE;
  uint32 i;
  for(i = 0; i < ratio; i++){
    struct buf *b = bread(dev, fsblk * ratio + i);
    if(!b)
      return -1;
    memmove(dst + i * BSIZE, b->data, BSIZE);
    brelse(b);
  }
  return 0;
}

static int
ext4_read_ptr_entry(int dev, uint32 ptrblk, uint32 idx, uint32 *out)
{
  uint32 ratio = ext4_fs_bsize() / BSIZE;
  uint32 byte_off = idx * 4;
  uint32 dev_block = ptrblk * ratio + (byte_off / BSIZE);
  uint32 off = byte_off % BSIZE;
  struct buf *b = bread(dev, dev_block);
  if(!b)
    return -1;
  *out = get_u32(b->data + off);
  brelse(b);
  return 0;
}

static int
ext4_lookup_extent_block(int dev, struct ext4_inode *inode, uint32 lbn, uint32 *phys)
{
  uchar *node;
  uchar *base;

  *phys = 0;
  node = (uchar *)kalloc();
  if(!node)
    return -1;

  base = inode->i_block;
  for(;;){
    uint16 magic = get_u16(base + 0);
    uint16 entries = get_u16(base + 2);
    uint16 depth = get_u16(base + 6);
    uint32 i;

    if(magic != EXT4_EXT_MAGIC)
      break;
    if(entries == 0)
      break;

    if(depth == 0){
      for(i = 0; i < entries; i++){
        uchar *ex = base + 12 + i * 12;
        uint32 ee_block = get_u32(ex + 0);
        uint16 ee_len_raw = get_u16(ex + 4);
        uint32 ee_len = (uint32)(ee_len_raw & 0x7FFF);
        uint64 ee_start = get_u64_from_pair(get_u32(ex + 8), get_u16(ex + 6));
        if(ee_len == 0)
          continue;
        if(lbn >= ee_block && lbn < ee_block + ee_len){
          *phys = (uint32)(ee_start + (lbn - ee_block));
          kfree(node);
          return 0;
        }
      }
      kfree(node);
      return 0;
    } else {
      int found = 0;
      uint64 child = 0;
      for(i = 0; i < entries; i++){
        uchar *ix = base + 12 + i * 12;
        uint32 ei_block = get_u32(ix + 0);
        if(ei_block > lbn)
          break;
        child = get_u64_from_pair(get_u32(ix + 4), get_u16(ix + 8));
        found = 1;
      }
      if(!found)
        break;
      if(ext4_read_fs_block(dev, (uint32)child, node) < 0)
        break;
      base = node;
    }
  }

  kfree(node);
  return -1;
}

static int
ext4_lookup_legacy_block(int dev, struct ext4_inode *inode, uint32 lbn, uint32 *phys)
{
  uint32 ptrs = ext4_fs_bsize() / 4;
  uint32 blk;

  *phys = 0;
  if(lbn < 12){
    *phys = get_u32(inode->i_block + lbn * 4);
    return 0;
  }

  lbn -= 12;
  blk = get_u32(inode->i_block + 12 * 4);
  if(lbn < ptrs){
    if(blk == 0)
      return 0;
    return ext4_read_ptr_entry(dev, blk, lbn, phys);
  }

  lbn -= ptrs;
  blk = get_u32(inode->i_block + 13 * 4);
  if((uint64)lbn < (uint64)ptrs * ptrs){
    uint32 i1 = lbn / ptrs;
    uint32 i2 = lbn % ptrs;
    uint32 lvl1 = 0;
    if(blk == 0)
      return 0;
    if(ext4_read_ptr_entry(dev, blk, i1, &lvl1) < 0)
      return -1;
    if(lvl1 == 0)
      return 0;
    return ext4_read_ptr_entry(dev, lvl1, i2, phys);
  }

  lbn -= ptrs * ptrs;
  blk = get_u32(inode->i_block + 14 * 4);
  if((uint64)lbn < (uint64)ptrs * ptrs * ptrs){
    uint32 i1 = lbn / (ptrs * ptrs);
    uint32 rem = lbn % (ptrs * ptrs);
    uint32 i2 = rem / ptrs;
    uint32 i3 = rem % ptrs;
    uint32 lvl1 = 0;
    uint32 lvl2 = 0;
    if(blk == 0)
      return 0;
    if(ext4_read_ptr_entry(dev, blk, i1, &lvl1) < 0)
      return -1;
    if(lvl1 == 0)
      return 0;
    if(ext4_read_ptr_entry(dev, lvl1, i2, &lvl2) < 0)
      return -1;
    if(lvl2 == 0)
      return 0;
    return ext4_read_ptr_entry(dev, lvl2, i3, phys);
  }

  return 0;
}

static int
ext4_inode_lookup_block(int dev, struct ext4_inode *inode, uint32 lbn, uint32 *phys)
{
  if(ext4_inode_flags(inode) & EXT4_EXTENTS_FL)
    return ext4_lookup_extent_block(dev, inode, lbn, phys);
  return ext4_lookup_legacy_block(dev, inode, lbn, phys);
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

  //read the data from superblock
  sb.s_inodes_count = get_u32(s + 0);
  sb.s_blocks_count_lo = get_u32(s + 4);
  sb.s_log_block_size = get_u32(s + 24);
  sb.s_blocks_per_group = get_u32(s + 32);
  sb.s_inodes_per_group = get_u32(s + 40);
  sb.s_magic = get_u16(s + 56);
  sb.s_inode_size = get_u16(s + 88);
  
  brelse(b);

  if(sb.s_magic != EXT4_SUPER_MAGIC)
    return -1;
  // check block size; the block have to be aligned, otherwise it will be hard to handle.
  uint32 fs_bsize = ext4_fs_bsize();
  if(fs_bsize % BSIZE != 0 || fs_bsize > PGSIZE){
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
static int ext4_read_inode(int dev, uint32 inode_no, struct ext4_inode *out)
{
  if(!ext4_present) return -1;
  if(inode_no == 0) return -1;
  memset(out, 0, sizeof(*out));
  uint32 fs_bsize = ext4_fs_bsize();
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
  if(tocopy > sizeof(*out)) tocopy = sizeof(*out);
  if(off + tocopy <= BSIZE){
    memmove((uchar *)out, b->data + off, tocopy);
    brelse(b);
    return 0;
  }
  uint32 first = BSIZE - off;
  memmove((uchar *)out, b->data + off, first);
  brelse(b);
  struct buf *b2 = bread(dev, dev_block + 1);
  if(!b2) return -1;
  memmove((uchar *)out + first, b2->data, tocopy - first);
  brelse(b2);
  return 0;
}

// Read file data from inode into buffer, supports only direct blocks
static int ext4_read_data(int dev, struct ext4_inode *inode, uchar *dst, uint32 len, uint32 offset)
{
  uint32 fs_bsize = ext4_fs_bsize();
  uint32 ratio = fs_bsize / BSIZE;
  uint32 copied = 0;

  uint64 file_size = ext4_inode_size(inode);
  if((uint64)offset >= file_size)
    return 0;
  if((uint64)len > file_size - offset)
    len = (uint32)(file_size - offset);

  while(copied < len){
    uint32 foff = offset + copied;
    uint32 lbn = foff / fs_bsize;
    uint32 off_in_fs = foff % fs_bsize;
    uint32 phys = 0;
    uint32 dev_block;
    uint32 off_in_dev;
    uint32 amt;

    if(ext4_inode_lookup_block(dev, inode, lbn, &phys) < 0)
      break;
    if(phys == 0)
      break;

    dev_block = phys * ratio + (off_in_fs / BSIZE);
    off_in_dev = off_in_fs % BSIZE;
    struct buf *b = bread(dev, dev_block);
    if(!b)
      break;

    amt = BSIZE - off_in_dev;
    if(amt > (len - copied))
      amt = len - copied;
    if(amt > fs_bsize - off_in_fs)
      amt = fs_bsize - off_in_fs;
    memmove(dst + copied, b->data + off_in_dev, amt);
    brelse(b);

    copied += amt;
  }
  return copied;
}

static int
ext4_lookup_name_in_dir(int dev, struct ext4_inode *dir_inode, const char *name, uint32 *out_ino)
{
  uint64 dir_size = ext4_inode_size(dir_inode);
  uint64 off = 0;

  while(off + 8 <= dir_size){
    struct ext4_dir_entry_header hdr;
    uint32 ino;
    uint16 rec_len;
    uint8 name_len;

    if(ext4_read_data(dev, dir_inode, (uchar *)&hdr, sizeof(hdr), (uint32)off) != sizeof(hdr))
      break;
    ino = get_u32((uchar *)&hdr.inode);
    rec_len = get_u16((uchar *)&hdr.rec_len);
    name_len = hdr.name_len;

    if(rec_len < 8)
      break;
    if(off + rec_len > dir_size)
      break;

    if(ino != 0 && name_len > 0 && name_len < 255){
      char nm[256];
      if(ext4_read_data(dev, dir_inode, (uchar *)nm, name_len, (uint32)off + 8) != name_len)
        return -1;
      nm[name_len] = '\0';
      if(strlen(name) == name_len && strncmp(name, nm, name_len) == 0){
        *out_ino = ino;
        return 0;
      }
    }

    off += rec_len;
  }

  return -1;
}

// List root directory entries and print to console
void ext4_list_root(int dev)
{
  struct ext4_inode inode;
  uint64 off;
  if(ext4_read_inode(dev, EXT4_ROOTINO, &inode) < 0){
    printf("ext4: failed read root inode\n");
    return;
  }

  off = 0;
  while(off + 8 <= ext4_inode_size(&inode)){
    struct ext4_dir_entry_header hdr;
    uint32 ino;
    uint16 rec_len;
    uint8 name_len;
    if(ext4_read_data(dev, &inode, (uchar *)&hdr, sizeof(hdr), (uint32)off) != sizeof(hdr))
      break;
    ino = get_u32((uchar *)&hdr.inode);
    rec_len = get_u16((uchar *)&hdr.rec_len);
    name_len = hdr.name_len;
    if(rec_len < 8)
      break;
    if(ino != 0 && name_len > 0 && name_len < 255){
      char name[256];
      if(ext4_read_data(dev, &inode, (uchar *)name, name_len, (uint32)off + 8) != name_len)
        break;
      name[name_len] = '\0';
      printf("ext4: |%s|\n", name);
    }
    off += rec_len;
  }
}

static int
ext4_lookup_path(int dev, const char *path, struct ext4_inode *out)
{
  char pcopy[256];
  char *tok;
  uint32 inode_no = EXT4_ROOTINO;

  if(!ext4_present) return -1;
  if(path[0] != '/') return -1;
  if(path[1] == '\0'){
    return ext4_read_inode(dev, EXT4_ROOTINO, out);
  }

  safestrcpy(pcopy, path+1, sizeof(pcopy)); // skip leading '/'
  tok = pcopy;
  while(*tok){
    char *component;
    char *sep;
    struct ext4_inode inode;

    while(*tok == '/')
      tok++;
    if(*tok == '\0')
      break;

    component = tok;
    sep = tok;
    while(*sep && *sep != '/')
      sep++;
    if(*sep){
      *sep = '\0';
      tok = sep + 1;
    } else {
      tok = sep;
    }

    if(ext4_read_inode(dev, inode_no, &inode) < 0)
      return -1;
    if(!ext4_inode_mode_is_dir(&inode))
      return -1;
    if(ext4_lookup_name_in_dir(dev, &inode, component, &inode_no) < 0)
      return -1;
  }

  return ext4_read_inode(dev, inode_no, out);
}

int
ext4_read_file_by_path_at(int dev, const char *path, uchar *dst, uint32 len, uint32 off)
{
  struct ext4_inode inode;

  if(ext4_lookup_path(dev, path, &inode) < 0) return -1;
  if(!ext4_inode_mode_is_reg(&inode))
    return -1;
  return ext4_read_data(dev, &inode, dst, len, off);
}

int
ext4_read_file_by_path(int dev, const char *path, uchar *dst, uint32 len)
{
  return ext4_read_file_by_path_at(dev, path, dst, len, 0);
}

uint64
ext4_file_size_by_path(int dev, const char *path)
{
  struct ext4_inode inode;
  if(ext4_lookup_path(dev, path, &inode) < 0) return 0;
  if(!ext4_inode_mode_is_reg(&inode)) return 0;
  return ext4_inode_size(&inode);
}

int
ext4_path_is_dir(int dev, const char *path)
{
  struct ext4_inode inode;
  if(ext4_lookup_path(dev, path, &inode) < 0) return 0;
  return ext4_inode_mode_is_dir(&inode);
}

int
ext4_path_is_reg(int dev, const char *path)
{
  struct ext4_inode inode;
  if(ext4_lookup_path(dev, path, &inode) < 0) return 0;
  return ext4_inode_mode_is_reg(&inode);
}

static int
ext4_name_has_suffix(const char *name, const char *suffix)
{
  int nlen = strlen(name);
  int slen = strlen(suffix);
  if(nlen < slen)
    return 0;
  return strncmp(name + nlen - slen, suffix, slen) == 0;
}

static void
ext4_walk_all_files(int dev, uint32 inode_no, const char *path, int depth)
{
  struct ext4_inode *inode;
  uint64 off;
  uint64 dsize;

  if(depth > 16)
    return;
  inode = (struct ext4_inode *)kalloc();
  if(!inode)
    return;

  if(ext4_read_inode(dev, inode_no, inode) < 0){
    kfree(inode);
    return;
  }

  if(ext4_inode_mode_is_reg(inode)){
    int is_sh = ext4_name_has_suffix(path, "testcode.sh");
    if(is_sh)
      printf("ext4: script %s\n", path);
    kfree(inode);
    return;
  }

  if(!ext4_inode_mode_is_dir(inode)){
    kfree(inode);
    return;
  }

  dsize = ext4_inode_size(inode);
  if(dsize > (16ULL << 20))
    dsize = (16ULL << 20);

  off = 0;
  while(off + 8 <= dsize){
    struct ext4_dir_entry_header hdr;
    uint32 child_ino;
    uint16 rec_len;
    uint8 name_len;

    if(ext4_read_data(dev, inode, (uchar *)&hdr, sizeof(hdr), (uint32)off) != sizeof(hdr))
      break;

    child_ino = get_u32((uchar *)&hdr.inode);
    rec_len = get_u16((uchar *)&hdr.rec_len);
    name_len = hdr.name_len;

    if(rec_len < 8)
      break;

    if(child_ino != 0 && name_len > 0 && name_len < 255){
      char *name = (char *)kalloc();
      if(!name)
        break;
      if(ext4_read_data(dev, inode, (uchar *)name, name_len, (uint32)off + 8) != name_len)
      {
        kfree(name);
        break;
      }
      name[name_len] = '\0';

      if(strncmp(name, ".", 1) != 0 || strlen(name) != 1){
        if(strncmp(name, "..", 2) != 0 || strlen(name) != 2){
          char *child_path = (char *)kalloc();
          if(!child_path){
            kfree(name);
            break;
          }
          if(strlen(path) == 1 && path[0] == '/')
            snprintf(child_path, PGSIZE, "/%s", name);
          else
            snprintf(child_path, PGSIZE, "%s/%s", path, name);

          ext4_walk_all_files(dev, child_ino, child_path, depth + 1);
          kfree(child_path);
        }
      }
      kfree(name);
    }

    off += rec_len;
  }

  kfree(inode);
}

void
ext4_print_sh_scripts(int dev)
{
  if(!ext4_present)
    return;
  struct ext4_inode root_inode;
  uint32 ino;
  if(ext4_read_inode(dev, EXT4_ROOTINO, &root_inode) < 0)
    return;
  if(ext4_lookup_name_in_dir(dev, &root_inode, "glibc", &ino) == 0)
    ext4_walk_all_files(dev, ino, "/glibc", 0);
  if(ext4_lookup_name_in_dir(dev, &root_inode, "musl", &ino) == 0)
    ext4_walk_all_files(dev, ino, "/musl", 0);
}


void
ext4_join_path(char *out, int outsz, char *cwd, char *path)
{
  int oi = 0;
  char tmp[MAXPATH];
  char *p;

  if(path[0] == '/')
    safestrcpy(tmp, path, sizeof(tmp));
  else if(cwd[0] == '/' && cwd[1] == 0)
    snprintf(tmp, sizeof(tmp), "/%s", path);
  else
    snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);

  out[oi++] = '/';
  p = tmp;
  while(*p && oi < outsz - 1){
    char elem[MAXPATH];
    int n = 0;
    while(*p == '/')
      p++;
    if(*p == 0)
      break;
    while(*p && *p != '/' && n < sizeof(elem) - 1)
      elem[n++] = *p++;
    elem[n] = 0;
    while(*p && *p != '/')
      p++;
    if(n == 1 && elem[0] == '.')
      continue;
    if(n == 2 && elem[0] == '.' && elem[1] == '.'){
      if(oi > 1){
        oi--;
        while(oi > 1 && out[oi-1] != '/')
          oi--;
      }
      out[oi] = 0;
      continue;
    }
    if(oi > 1 && oi < outsz - 1)
      out[oi++] = '/';
    for(int i = 0; i < n && oi < outsz - 1; i++)
      out[oi++] = elem[i];
    out[oi] = 0;
  }
  out[oi] = 0;
}

int
resolve_ext4_path(char *path, char *out, int outsz)
{
  struct proc *p = myproc();

  if(path[0] == '/'){
    ext4_join_path(out, outsz, "/", path);
    return 1;
  }
  if(p->cwd_is_ext4){
    ext4_join_path(out, outsz, p->ext4_cwd, path);
    return 1;
  }
  return 0;
}