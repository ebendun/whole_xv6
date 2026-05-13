// Minimal on-disk ext4 format used by the read-only ext4 probe.

#define EXT4_ROOTINO        2
#define EXT4_INODE_BUFSIZE  256

#define EXT4_SUPER_MAGIC    0xEF53
#define EXT4_EXTENTS_FL     0x00080000
#define EXT4_EXT_MAGIC      0xF30A

#define EXT4_MODE_DIR       0x4000
#define EXT4_MODE_REG       0x8000

struct ext4_superblock {
  uint32 s_inodes_count;
  uint32 s_blocks_count_lo;
  uint32 s_log_block_size;
  uint32 s_blocks_per_group;
  uint32 s_inode_size;
  uint16 s_magic;
  uint32 s_inodes_per_group;
};

// The first 128 bytes are the fixed ext2/3/4 inode layout. ext4 filesystems
// commonly use 256-byte inodes, so keep room for the bytes we do not decode.
struct ext4_inode {
  uint16 i_mode;
  uint16 i_uid;
  uint32 i_size_lo;
  uint32 i_atime;
  uint32 i_ctime;
  uint32 i_mtime;
  uint32 i_dtime;
  uint16 i_gid;
  uint16 i_links_count;
  uint32 i_blocks_lo;
  uint32 i_flags;
  uint32 i_osd1;
  uchar  i_block[60];
  uint32 i_generation;
  uint32 i_file_acl_lo;
  uint32 i_size_high;
  uint32 i_obso_faddr;
  uchar  i_osd2[12];
  uchar  i_extra[128];
} __attribute__((packed));

struct ext4_dir_entry_header {
  uint32 inode;
  uint16 rec_len;
  uint8 name_len;
  uint8 file_type;
} __attribute__((packed));

