//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// the address of virtio mmio register r for a given base.
#define VIRTIO_STEP 0x1000

// support up to two virtio mmio devices (sdcard + fs.img)
#define VDISKS 2

static inline volatile uint32 *Rbase(uint64 base, uint32 r) {
  return (volatile uint32 *)(base + (r));
}

static struct disk {
  // a set (not a ring) of DMA descriptors, with which the
  // driver tells the device where to read and write individual
  // disk operations. there are NUM descriptors.
  // most commands consist of a "chain" (a linked list) of a couple of
  // these descriptors.
  struct virtq_desc *desc;

  // a ring in which the driver writes descriptor numbers
  // that the driver would like the device to process.  it only
  // includes the head descriptor of each chain. the ring has
  // NUM elements.
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
  // there are NUM used ring entries.
  struct virtq_used *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  // track info about in-flight operations,
  // for use when completion interrupt arrives.
  // indexed by first descriptor index of chain.
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // disk command headers.
  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;

} disks[VDISKS];

// number of virtio devices initialized
static int vdisk_inited = 0;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  // initialize per-device structures
  vdisk_inited = 0;
  for(int devidx = 0; devidx < VDISKS; devidx++){
    uint64 base = VIRTIO0 + devidx * VIRTIO_STEP;
    struct disk *d = &disks[devidx];
    initlock(&d->vdisk_lock, "virtio_disk");

    uint32 magic = *(volatile uint32*)(base + VIRTIO_MMIO_MAGIC_VALUE);
    uint32 version = *(volatile uint32*)(base + VIRTIO_MMIO_VERSION);
    uint32 device_id = *(volatile uint32*)(base + VIRTIO_MMIO_DEVICE_ID);
    uint32 vendor = *(volatile uint32*)(base + VIRTIO_MMIO_VENDOR_ID);
    if(magic != 0x74726976 || (version != 1 && version != 2) || device_id != 2 || vendor != 0x554d4551){
      panic("could not find virtio disk");
    }

    // reset device
    *(volatile uint32*)(base + VIRTIO_MMIO_STATUS) = status;

    // set ACKNOWLEDGE status bit
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *(volatile uint32*)(base + VIRTIO_MMIO_STATUS) = status;

    // set DRIVER status bit
    status |= VIRTIO_CONFIG_S_DRIVER;
    *(volatile uint32*)(base + VIRTIO_MMIO_STATUS) = status;

    // negotiate features
    uint64 features = *(volatile uint32*)(base + VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *(volatile uint32*)(base + VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // tell device that feature negotiation is complete.
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *(volatile uint32*)(base + VIRTIO_MMIO_STATUS) = status;

    // re-read status to ensure FEATURES_OK is set.
    status = *(volatile uint32*)(base + VIRTIO_MMIO_STATUS);
    if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
      panic("virtio disk FEATURES_OK unset");

    // initialize queue 0.
    *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_SEL) = 0;

    // check maximum queue size.
    uint32 max = *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_NUM_MAX);
    if(max == 0)
      panic("virtio disk has no queue 0");
    if(max < NUM)
      panic("virtio disk max queue too short");

    // set queue size.
    *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_NUM) = NUM;

    if(version == 1){
      d->desc = superalloc();
      if(!d->desc)
        panic("virtio disk superalloc");
      memset(d->desc, 0, SUPERPGSIZE);
      d->avail = (struct virtq_avail *)((char*)d->desc + NUM*sizeof(struct virtq_desc));
      d->used = (struct virtq_used *)((char*)d->desc + PGSIZE);

      *(volatile uint32*)(base + VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;
      *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;
      *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_PFN) = ((uint64)d->desc) >> PGSHIFT;
    } else {
      d->desc = kalloc();
      d->avail = kalloc();
      d->used = kalloc();
      if(!d->desc || !d->avail || !d->used)
        panic("virtio disk kalloc");
      memset(d->desc, 0, PGSIZE);
      memset(d->avail, 0, PGSIZE);
      memset(d->used, 0, PGSIZE);

      *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)d->desc;
      *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)d->desc >> 32;
      *(volatile uint32*)(base + VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)d->avail;
      *(volatile uint32*)(base + VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)d->avail >> 32;
      *(volatile uint32*)(base + VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)d->used;
      *(volatile uint32*)(base + VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)d->used >> 32;

      *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_READY) = 0x1;
    }

    for(int i = 0; i < NUM; i++)
      d->free[i] = 1;

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *(volatile uint32*)(base + VIRTIO_MMIO_STATUS) = status;

    vdisk_inited++;
  }

  if(vdisk_inited == 0)
    panic("could not find virtio disk");
}


// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc(struct disk *d)
{
  for(int i = 0; i < NUM; i++){
    if(d->free[i]){
      d->free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(struct disk *d, int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(d->free[i])
    panic("free_desc 2");
  d->desc[i].addr = 0;
  d->desc[i].len = 0;
  d->desc[i].flags = 0;
  d->desc[i].next = 0;
  d->free[i] = 1;
  wakeup(&d->free[0]);
}

// free a chain of descriptors.
static void
free_chain(struct disk *d, int i)
{
  while(1){
    int flag = d->desc[i].flags;
    int nxt = d->desc[i].next;
    free_desc(d, i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(struct disk *d, int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc(d);
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(d, idx[j]);
      return -1;
    }
  }
  return 0;
}

//
// check that there are at most NBUF distinct
// struct buf's, which the lock lab requires.
//
static struct buf *xbufs[NBUF];
static void
checkbuf(struct buf *b)
{
  for(int i = 0; i < NBUF; i++){
    if(xbufs[i] == b){
      return;
    }
    if(xbufs[i] == 0){
      xbufs[i] = b;
      return;
    }
  }
  panic("more than NBUF bufs");
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  // map b->dev (1-based) to disks index (0-based)
  int devidx = 0;
  if(b->dev >= 1 && b->dev <= VDISKS)
    devidx = (int)b->dev - 1;
  struct disk *d = &disks[devidx];
  uint64 base = VIRTIO0 + devidx * VIRTIO_STEP;

  acquire(&d->vdisk_lock);

  checkbuf(b);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(d, idx) == 0) {
      break;
    }
    sleep(&d->free[0], &d->vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &d->ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  d->desc[idx[0]].addr = (uint64) buf0;
  d->desc[idx[0]].len = sizeof(struct virtio_blk_req);
  d->desc[idx[0]].flags = VRING_DESC_F_NEXT;
  d->desc[idx[0]].next = idx[1];

  d->desc[idx[1]].addr = (uint64) b->data;
  d->desc[idx[1]].len = BSIZE;
  if(write)
    d->desc[idx[1]].flags = 0; // device reads b->data
  else
    d->desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  d->desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  d->desc[idx[1]].next = idx[2];

  d->info[idx[0]].status = 0xff; // device writes 0 on success
  d->desc[idx[2]].addr = (uint64) &d->info[idx[0]].status;
  d->desc[idx[2]].len = 1;
  d->desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  d->desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  d->info[idx[0]].b = b;

  // tell the device the first index in our chain of descriptors.
  d->avail->ring[d->avail->idx % NUM] = idx[0];

  __sync_synchronize();

  // tell the device another avail ring entry is available.
  d->avail->idx += 1; // not % NUM ...

  __sync_synchronize();

  *(volatile uint32*)(base + VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &d->vdisk_lock);
  }

  d->info[idx[0]].b = 0;
  free_chain(d, idx[0]);

  release(&d->vdisk_lock);
}

void
virtio_disk_intr()
{
  // handle interrupts for all initialized virtio devices
  for(int devidx = 0; devidx < VDISKS; devidx++){
    struct disk *d = &disks[devidx];

    uint64 base = VIRTIO0 + devidx * VIRTIO_STEP;

    acquire(&d->vdisk_lock);

    // ack this device's interrupt
    *(volatile uint32*)(base + VIRTIO_MMIO_INTERRUPT_ACK) = *(volatile uint32*)(base + VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    __sync_synchronize();

    while(d->used_idx != d->used->idx){
      __sync_synchronize();
      int id = d->used->ring[d->used_idx % NUM].id;

      if(d->info[id].status != 0)
        panic("virtio_disk_intr status");

      struct buf *b = d->info[id].b;
      if(b){
        b->disk = 0;   // disk is done with buf
        wakeup(b);
      } 
      // else {
      //   // wake up any waiter sleeping on the status word
      //   wakeup(&d->info[id].status);
      // }

      d->used_idx += 1;
    }

    release(&d->vdisk_lock);
  }
}
