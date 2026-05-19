#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "virtio.h"

#define NET_Q_RX 0
#define NET_Q_TX 1
#define NET_QUEUES 2

struct virtio_net_hdr {
  uint8 flags;
  uint8 gso_type;
  uint16 hdr_len;
  uint16 gso_size;
  uint16 csum_start;
  uint16 csum_offset;
} __attribute__((packed));

struct netq {
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  char free[NUM];
  uint16 used_idx;
};

static struct {
  struct spinlock lock;
  uint64 base;
  uint32 version;
  struct netq q[NET_QUEUES];
  char *rxbuf[NUM];
  char *txhdr[NUM];
  char *txbuf[NUM];
} vnet;

static void
netq_init(int qid)
{
  struct netq *q = &vnet.q[qid];

  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_SEL) = qid;
  uint32 max = *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max < NUM)
    panic("virtio net queue too short");

  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_NUM) = NUM;

  if(vnet.version == 1){
    q->desc = superalloc();
    if(q->desc == 0)
      panic("virtio net superalloc");
    memset(q->desc, 0, SUPERPGSIZE);
    q->avail = (struct virtq_avail *)((char *)q->desc + NUM * sizeof(struct virtq_desc));
    q->used = (struct virtq_used *)((char *)q->desc + PGSIZE);

    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_PFN) = ((uint64)q->desc) >> PGSHIFT;
  } else {
    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    if(q->desc == 0 || q->avail == 0 || q->used == 0)
      panic("virtio net kalloc");
    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);

    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)q->desc;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)q->desc >> 32;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)q->avail;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)q->avail >> 32;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)q->used;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)q->used >> 32;
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_READY) = 1;
  }

  for(int i = 0; i < NUM; i++)
    q->free[i] = 1;
}

static int
alloc_desc(struct netq *q)
{
  for(int i = 0; i < NUM; i++){
    if(q->free[i]){
      q->free[i] = 0;
      return i;
    }
  }
  return -1;
}

static void
free_desc(struct netq *q, int i)
{
  q->desc[i].addr = 0;
  q->desc[i].len = 0;
  q->desc[i].flags = 0;
  q->desc[i].next = 0;
  q->free[i] = 1;
}

static void
submit_desc(int qid, int head)
{
  struct netq *q = &vnet.q[qid];
  q->avail->ring[q->avail->idx % NUM] = head;
  __sync_synchronize();
  q->avail->idx++;
  __sync_synchronize();
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_QUEUE_NOTIFY) = qid;
}

static void
reclaim_tx(void)
{
  struct netq *q = &vnet.q[NET_Q_TX];

  while(q->used_idx != q->used->idx){
    __sync_synchronize();
    int head = q->used->ring[q->used_idx % NUM].id;
    int data = q->desc[head].next;
    if(vnet.txhdr[head]){
      kfree(vnet.txhdr[head]);
      vnet.txhdr[head] = 0;
    }
    if(vnet.txbuf[head]){
      kfree(vnet.txbuf[head]);
      vnet.txbuf[head] = 0;
    }
    free_desc(q, data);
    free_desc(q, head);
    q->used_idx++;
  }
}

static void
refill_rx(int i)
{
  struct netq *q = &vnet.q[NET_Q_RX];
  char *buf = kalloc();
  if(buf == 0)
    panic("virtio net rx kalloc");
  vnet.rxbuf[i] = buf;
  q->desc[i].addr = (uint64)buf;
  q->desc[i].len = PGSIZE;
  q->desc[i].flags = VRING_DESC_F_WRITE;
  q->desc[i].next = 0;
  q->free[i] = 0;
  submit_desc(NET_Q_RX, i);
}

void
virtio_net_init(void)
{
  uint32 status = 0;

  initlock(&vnet.lock, "virtio_net");
  for(int i = 0; i < VIRTIO_COUNT; i++){
    uint64 base = VIRTIO0 + i * PGSIZE;
    uint32 magic = *(volatile uint32 *)(base + VIRTIO_MMIO_MAGIC_VALUE);
    uint32 version = *(volatile uint32 *)(base + VIRTIO_MMIO_VERSION);
    uint32 device_id = *(volatile uint32 *)(base + VIRTIO_MMIO_DEVICE_ID);
    uint32 vendor = *(volatile uint32 *)(base + VIRTIO_MMIO_VENDOR_ID);
    if(magic == 0x74726976 && (version == 1 || version == 2) &&
       device_id == 1 && vendor == 0x554d4551){
      vnet.base = base;
      vnet.version = version;
      break;
    }
  }
  if(vnet.base == 0)
    panic("could not find virtio net");

  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) = 0;
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) = status;
  status |= VIRTIO_CONFIG_S_DRIVER;
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) = status;

  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_DRIVER_FEATURES) = 0;
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) = status;
  if((*(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK) == 0)
    panic("virtio net FEATURES_OK unset");

  netq_init(NET_Q_RX);
  netq_init(NET_Q_TX);

  for(int i = 0; i < NUM; i++)
    refill_rx(i);

  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_STATUS) = status;
}

int
virtio_net_transmit(char *buf, int len)
{
  struct netq *q = &vnet.q[NET_Q_TX];
  char *hdr;

  acquire(&vnet.lock);
  reclaim_tx();

  int h = alloc_desc(q);
  int d = alloc_desc(q);
  if(h < 0 || d < 0){
    if(h >= 0)
      free_desc(q, h);
    if(d >= 0)
      free_desc(q, d);
    release(&vnet.lock);
    return -1;
  }

  hdr = kalloc();
  if(hdr == 0){
    free_desc(q, d);
    free_desc(q, h);
    release(&vnet.lock);
    return -1;
  }
  memset(hdr, 0, sizeof(struct virtio_net_hdr));

  q->desc[h].addr = (uint64)hdr;
  q->desc[h].len = sizeof(struct virtio_net_hdr);
  q->desc[h].flags = VRING_DESC_F_NEXT;
  q->desc[h].next = d;
  q->desc[d].addr = (uint64)buf;
  q->desc[d].len = len;
  q->desc[d].flags = 0;
  q->desc[d].next = 0;
  vnet.txhdr[h] = hdr;
  vnet.txbuf[h] = buf;

  submit_desc(NET_Q_TX, h);
  release(&vnet.lock);
  return 0;
}

void
virtio_net_intr(void)
{
  struct netq *q = &vnet.q[NET_Q_RX];

  acquire(&vnet.lock);
  *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_INTERRUPT_ACK) =
    *(volatile uint32 *)(vnet.base + VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
  reclaim_tx();

  while(q->used_idx != q->used->idx){
    __sync_synchronize();
    int id = q->used->ring[q->used_idx % NUM].id;
    int len = q->used->ring[q->used_idx % NUM].len;
    char *dma = vnet.rxbuf[id];
    vnet.rxbuf[id] = 0;
    free_desc(q, id);
    q->used_idx++;

    if(len > sizeof(struct virtio_net_hdr)){
      int plen = len - sizeof(struct virtio_net_hdr);
      char *pkt = kalloc();
      if(pkt){
        memmove(pkt, dma + sizeof(struct virtio_net_hdr), plen);
        kfree(dma);
        refill_rx(id);
        release(&vnet.lock);
        net_rx(pkt, plen);
        acquire(&vnet.lock);
        continue;
      }
    }
    kfree(dma);
    refill_rx(id);
  }
  release(&vnet.lock);
}
