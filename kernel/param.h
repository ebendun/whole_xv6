#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE      128  // open files per process
#define NFILE       256  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define FIRSTDEV      1  // xv6 ext4 device number of file system root disk
#define SECONDDEV     2  // init xv6 fs device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       10000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
#define USERSTACK    2     // user stack pages
#define NVMA 16            // user vma
#define SUPERPGNUM 16       // the num of super page
