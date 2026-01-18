
#define _GNU_SOURCE

#include "vtpc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef VTPC_PAGE_SIZE
#define VTPC_PAGE_SIZE 4096u
#endif

#ifndef VTPC_CACHE_PAGES
#define VTPC_CACHE_PAGES 256u
#endif


#define HT_FACTOR 4u
#define HT_SIZE (VTPC_CACHE_PAGES * HT_FACTOR)


typedef struct {
  int fd;
  uint64_t page_no;
} page_key_t;


typedef struct {
  int in_use;           
  int dirty;            
  int key_valid;        
  page_key_t key;       
  unsigned char *data;  
} page_slot_t;

typedef enum { HT_EMPTY = 0, HT_USED = 1, HT_TOMB = 2 } ht_state_t;

typedef struct {
  ht_state_t st;
  page_key_t key;
  int slot_index;
} ht_entry_t;

typedef struct {
  int used;
  int fd;
  off_t offset;
  off_t file_size;
} fd_state_t;


static page_slot_t g_pages[VTPC_CACHE_PAGES];
static ht_entry_t g_ht[HT_SIZE];

static fd_state_t g_fds[1024];


static unsigned int g_rng = 0xC0FFEEu;

static uint64_t hash_u64(uint64_t x);
static uint64_t key_hash(page_key_t k);
static int key_eq(page_key_t a, page_key_t b);

static int ht_find_index(page_key_t key, int *out_found);
static int ht_lookup(page_key_t key);
static void ht_insert(page_key_t key, int slot_index);
static void ht_erase(page_key_t key);

static int fdstate_ensure(int fd);
static void fdstate_remove(int fd);

static int alloc_page_data(unsigned char **out);
static int alloc_aligned(void **out);

static int flush_slot(int slot_index);
static void evict_slot(int slot_index);
static int find_free_slot(void);
static int random_victim(void);

static int load_page_into_slot(int slot_index, int fd, uint64_t page_no, off_t file_size);
static int get_slot_for_page(int fd, uint64_t page_no, int for_write, int full_overwrite, off_t file_size);


static uint64_t hash_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

static uint64_t key_hash(page_key_t k) {
  uint64_t x = ((uint64_t)(uint32_t)k.fd << 32) ^ k.page_no;
  return hash_u64(x);
}

static int key_eq(page_key_t a, page_key_t b) {
  return (a.fd == b.fd) && (a.page_no == b.page_no);
}

static int ht_find_index(page_key_t key, int *out_found) {
  uint64_t h = key_hash(key);
  uint32_t start = (uint32_t)(h % HT_SIZE);

  int first_tomb = -1;
  for (uint32_t i = 0; i < HT_SIZE; i++) {
    uint32_t idx = (start + i) % HT_SIZE;
    ht_entry_t *e = &g_ht[idx];

    if (e->st == HT_EMPTY) {
      if (out_found) *out_found = 0;
      return (first_tomb >= 0) ? first_tomb : (int)idx;
    }
    if (e->st == HT_TOMB) {
      if (first_tomb < 0) first_tomb = (int)idx;
      continue;
    }
    if (key_eq(e->key, key)) {
      if (out_found) *out_found = 1;
      return (int)idx;
    }
  }

  if (out_found) *out_found = 0;
  return -1;
}

static int ht_lookup(page_key_t key) {
  int found = 0;
  int idx = ht_find_index(key, &found);
  if (idx < 0 || !found) return -1;
  return g_ht[idx].slot_index;
}

static void ht_insert(page_key_t key, int slot_index) {
  int found = 0;
  int idx = ht_find_index(key, &found);
  if (idx < 0) return;
  g_ht[idx].st = HT_USED;
  g_ht[idx].key = key;
  g_ht[idx].slot_index = slot_index;
}

static void ht_erase(page_key_t key) {
  int found = 0;
  int idx = ht_find_index(key, &found);
  if (idx < 0 || !found) return;
  g_ht[idx].st = HT_TOMB;
}

static int fdstate_ensure(int fd) {
  if (fd < 0 || fd >= (int)(sizeof(g_fds) / sizeof(g_fds[0]))) {
    errno = EBADF;
    return -1;
  }
  if (g_fds[fd].used) return 0;

  struct stat st;
  if (fstat(fd, &st) != 0) return -1;

  g_fds[fd].used = 1;
  g_fds[fd].fd = fd;
  g_fds[fd].offset = 0;
  g_fds[fd].file_size = (off_t)st.st_size;
  return 0;
}

static void fdstate_remove(int fd) {
  if (fd < 0 || fd >= (int)(sizeof(g_fds) / sizeof(g_fds[0]))) return;
  g_fds[fd].used = 0;
  g_fds[fd].fd = -1;
  g_fds[fd].offset = 0;
  g_fds[fd].file_size = 0;
}


static int alloc_page_data(unsigned char **out) {
  unsigned char *p = (unsigned char *)malloc(VTPC_PAGE_SIZE);
  if (!p) {
    errno = ENOMEM;
    return -1;
  }
  *out = p;
  return 0;
}
static int alloc_aligned(void **out) {
  void *p = NULL;
  int rc = posix_memalign(&p, VTPC_PAGE_SIZE, VTPC_PAGE_SIZE);
  if (rc != 0) {
    errno = rc;
    return -1;
  }
  *out = p;
  return 0;
}

static int flush_slot(int slot_index) {
  page_slot_t *s = &g_pages[slot_index];
  if (!s->in_use || !s->dirty) return 0;

  void *tmp = NULL;
  if (alloc_aligned(&tmp) != 0) return -1;

  memcpy(tmp, s->data, VTPC_PAGE_SIZE);

  off_t off = (off_t)(s->key.page_no * (uint64_t)VTPC_PAGE_SIZE);
  ssize_t wr = pwrite(s->key.fd, tmp, VTPC_PAGE_SIZE, off);

  int saved = errno;
  free(tmp);

  if (wr < 0) {
    errno = saved;
    return -1;
  }
  if ((size_t)wr != VTPC_PAGE_SIZE) {
    errno = EIO;
    return -1;
  }

  s->dirty = 0;
  return 0;
}

static void evict_slot(int slot_index) {
  page_slot_t *s = &g_pages[slot_index];
  if (!s->in_use) return;

  (void)flush_slot(slot_index);

  if (s->key_valid) ht_erase(s->key);

  s->in_use = 0;
  s->dirty = 0;
  s->key_valid = 0;
}

static int find_free_slot(void) {
  for (int i = 0; i < (int)VTPC_CACHE_PAGES; i++) {
    if (!g_pages[i].in_use) return i;
  }
  return -1;
}

static int random_victim(void) {
  g_rng = g_rng * 1103515245u + 12345u;
  return (int)(g_rng % VTPC_CACHE_PAGES);
}

static int load_page_into_slot(int slot_index, int fd, uint64_t page_no, off_t file_size) {
  page_slot_t *s = &g_pages[slot_index];

  s->key.fd = fd;
  s->key.page_no = page_no;
  s->key_valid = 1;
  s->in_use = 1;
  s->dirty = 0;

  if (!s->data) {
    if (alloc_page_data(&s->data) != 0) return -1;
  }

  off_t off = (off_t)(page_no * (uint64_t)VTPC_PAGE_SIZE);

  if (off >= file_size) {
    memset(s->data, 0, VTPC_PAGE_SIZE);
    ht_insert(s->key, slot_index);
    return 0;
  }

  void *tmp = NULL;
  if (alloc_aligned(&tmp) != 0) return -1;

  ssize_t rd = pread(fd, tmp, VTPC_PAGE_SIZE, off);
  int saved = errno;

  if (rd < 0) {
    free(tmp);
    errno = saved;
    return -1;
  }

  if (rd == 0) {
    memset(s->data, 0, VTPC_PAGE_SIZE);
  } else {
    memcpy(s->data, tmp, (size_t)rd);
    if ((size_t)rd < VTPC_PAGE_SIZE) {
      memset(s->data + rd, 0, VTPC_PAGE_SIZE - (size_t)rd);
    }
  }

  free(tmp);

  ht_insert(s->key, slot_index);
  return 0;
}

static int get_slot_for_page(int fd, uint64_t page_no, int for_write, int full_overwrite, off_t file_size) {
  page_key_t key = {.fd = fd, .page_no = page_no};

  int slot = ht_lookup(key);
  if (slot >= 0) {
    return slot;
  }

  slot = find_free_slot();
  if (slot < 0) {
    slot = random_victim();
    evict_slot(slot);
  }

  if (for_write && full_overwrite) {
    page_slot_t *s = &g_pages[slot];
    if (!s->data) {
      if (alloc_page_data(&s->data) != 0) return -1;
    }
    s->key = key;
    s->key_valid = 1;
    s->in_use = 1;
    s->dirty = 0;
    memset(s->data, 0, VTPC_PAGE_SIZE);
    ht_insert(key, slot);
    return slot;
  }

  if (load_page_into_slot(slot, fd, page_no, file_size) != 0) return -1;
  return slot;
}


int vtpc_open(const char *path, int mode, int access) {
#ifdef O_DIRECT
  mode |= O_DIRECT;
#endif

  int fd = open(path, mode, access);
  if (fd < 0) return -1;

  if (fdstate_ensure(fd) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }

  return fd;
}

int vtpc_close(int fd) {
  if (fdstate_ensure(fd) != 0) return -1;

  for (int i = 0; i < (int)VTPC_CACHE_PAGES; i++) {
    if (g_pages[i].in_use && g_pages[i].key_valid && g_pages[i].key.fd == fd) {
      if (flush_slot(i) != 0) {
        int saved = errno;
        evict_slot(i);
        (void)close(fd);
        fdstate_remove(fd);
        errno = saved;
        return -1;
      }
      evict_slot(i);
    }
  }

  fdstate_remove(fd);
  return close(fd);
}

ssize_t vtpc_read(int fd, void *buf, size_t count) {
  if (count == 0) return 0;
  if (!buf) {
    errno = EINVAL;
    return -1;
  }
  if (fdstate_ensure(fd) != 0) return -1;

  fd_state_t *st = &g_fds[fd];

  if (st->offset >= st->file_size) return 0;

  unsigned char *out = (unsigned char *)buf;
  size_t done = 0;

  while (count > 0) {
    if (st->offset >= st->file_size) break;

    uint64_t page_no = (uint64_t)(st->offset / (off_t)VTPC_PAGE_SIZE);
    size_t in_page = (size_t)(st->offset % (off_t)VTPC_PAGE_SIZE);

    size_t can_take = VTPC_PAGE_SIZE - in_page;
    size_t need = (count < can_take) ? count : can_take;

    off_t remain = st->file_size - st->offset;
    if ((off_t)need > remain) need = (size_t)remain;

    int slot = get_slot_for_page(fd, page_no, 0, 0, st->file_size);
    if (slot < 0) return (done > 0) ? (ssize_t)done : -1;

    memcpy(out, g_pages[slot].data + in_page, need);

    out += need;
    st->offset += (off_t)need;
    done += need;
    count -= need;
  }

  return (ssize_t)done;
}

ssize_t vtpc_write(int fd, const void *buf, size_t count) {
  if (count == 0) return 0;
  if (!buf) {
    errno = EINVAL;
    return -1;
  }
  if (fdstate_ensure(fd) != 0) return -1;

  fd_state_t *st = &g_fds[fd];

  const unsigned char *in = (const unsigned char *)buf;
  size_t done = 0;

  while (count > 0) {
    uint64_t page_no = (uint64_t)(st->offset / (off_t)VTPC_PAGE_SIZE);
    size_t in_page = (size_t)(st->offset % (off_t)VTPC_PAGE_SIZE);

    size_t can_put = VTPC_PAGE_SIZE - in_page;
    size_t need = (count < can_put) ? count : can_put;

    int full_overwrite = (in_page == 0 && need == VTPC_PAGE_SIZE) ? 1 : 0;

    int slot = get_slot_for_page(fd, page_no, 1, full_overwrite, st->file_size);
    if (slot < 0) return (done > 0) ? (ssize_t)done : -1;

    memcpy(g_pages[slot].data + in_page, in, need);
    g_pages[slot].dirty = 1;

    in += need;
    st->offset += (off_t)need;
    done += need;
    count -= need;

    if (st->offset > st->file_size) st->file_size = st->offset;
  }

  return (ssize_t)done;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  if (fdstate_ensure(fd) != 0) return (off_t)-1;

  if (whence != SEEK_SET) {
    errno = EINVAL;
    return (off_t)-1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return (off_t)-1;
  }

  g_fds[fd].offset = offset;
  return offset;
}

int vtpc_fsync(int fd) {
  if (fdstate_ensure(fd) != 0) return -1;

  for (int i = 0; i < (int)VTPC_CACHE_PAGES; i++) {
    if (g_pages[i].in_use && g_pages[i].key_valid && g_pages[i].key.fd == fd) {
      if (flush_slot(i) != 0) return -1;
    }
  }


  return fsync(fd);
}
