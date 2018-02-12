/*
 * Copyright (c) 2015  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

// includes {{{
// C headers
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>

// POSIX headers
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

// Linux headers
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
// }}} includes
// types {{{
typedef int_least8_t            s8;
typedef int_least16_t           s16;
typedef int_least32_t           s32;
typedef int_least64_t           s64;

typedef uint_least8_t           u8;
typedef uint_least16_t          u16;
typedef uint_least32_t          u32;
typedef uint_least64_t          u64;
// }}} types
// bitmap {{{
struct bitmap {
  u64 bits;
  u64 ones;
  u64 bm[];
};

  static inline struct bitmap *
bitmap_create(const u64 bits)
{
  struct bitmap * const bm = (typeof(bm))calloc(1, sizeof(*bm) + (sizeof(u64) * ((bits + 63) >> 6)));
  bm->bits = bits;
  bm->ones = 0;
  return bm;
}

  static inline bool
bitmap_test(const struct bitmap * const bm, const u64 idx)
{
  return ((idx < bm->bits) && (bm->bm[idx >> 6] & (UINT64_C(1) << (idx & UINT64_C(0x3f))))) ? true : false;
}

  static inline bool
bitmap_test_all1(struct bitmap * const bm)
{
  return bm->ones == bm->bits ? true : false;
}

  static inline bool
bitmap_test_all0(struct bitmap * const bm)
{
  return bm->ones == 0 ? true : false;
}

  static inline void
bitmap_set1(struct bitmap * const bm, const u64 idx)
{
  if (idx < bm->bits && bitmap_test(bm, idx) == false) {
    bm->bm[idx >> 6] |= (UINT64_C(1) << (idx & UINT64_C(0x3f)));
    bm->ones++;
  }
}

  static inline void
bitmap_set0(struct bitmap * const bm, const u64 idx)
{
  if (idx < bm->bits && bitmap_test(bm, idx) == true) {
    bm->bm[idx >> 6] &= ~(UINT64_C(1) << (idx & UINT64_C(0x3f)));
    bm->ones--;
  }
}

  static inline u64
bitmap_count(struct bitmap * const bm)
{
  return bm->ones;
}

  static inline void
bitmap_set_all1(struct bitmap * const bm)
{
  memset(bm->bm, 0xff, (sizeof(u64) * ((bm->bits + 63) >> 6)));
  bm->ones = bm->bits;
}

  static inline void
bitmap_set_all0(struct bitmap * const bm)
{
  memset(bm->bm, 0, (sizeof(u64) * ((bm->bits + 63) >> 6)));
  bm->ones = 0;
}

  static inline void
bitmap_static_init(struct bitmap * const bm, const u64 bits)
{
  bm->bits = bits;
  bitmap_set_all0(bm);
}
// }}} bitmap
 // hash {{{
#define XXH_PRIVATE_API
#include "xxhash.h"
#undef XXH_PRIVATE_API

  static inline u32
xxhash32(const void * const ptr, const size_t size)
{
  return XXH32(ptr, size, 0);
}

  static inline u64
xxhash64(const void * const ptr, const size_t size)
{
  return XXH64(ptr, size, 0);
}
// }}} hash
// timing {{{
  static inline u64
time_nsec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

  static inline u64
time_usec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec * UINT64_C(1000000) + (ts.tv_nsec / UINT64_C(1000));
}

  static inline u64
time_sec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec;
}

  static inline double
time_sec_double(void)
{
  const u64 nsec = time_nsec();
  return ((double)nsec) / 1000000000.0;
}

  static inline u64
time_diff_nsec(const u64 last)
{
  return time_nsec() - last;
}

  static inline u64
time_diff_usec(const u64 last)
{
  return time_usec() - last;
}

  static inline u64
time_diff_sec(const u64 last)
{
  return time_sec() - last;
}

  static inline double
time_diff_sec_double(const double last)
{
  return time_sec_double() - last;
}
// }}} timing

  static inline void
debug_assert(const bool v)
{
  if (!v) {
    fprintf(stderr, "error!\n");
    exit(0);
  }
}

#define OUTSIDE ((UINT32_C(0xffffffff)))

struct common_stat {
  char name[12];
  u32 nr_keys;
  u64 cap;
  u64 size;
  u64 hit;
  u64 miss;
};

struct rep_api {
  void * (*new)(const u32 nr_keys, const u64 max_cap);
  void   (*destory)(void * const rep);
  void   (*op_set)(void * const rep, const u32 key);
  void   (*op_get)(void * const rep, const u32 key);
  void   (*op_del)(void * const rep, const u32 key);
  void   (*collect_stat)(void * const rep, struct common_stat * const out);
  void   (*clean_stat)(void * const rep);
};

static bool __set_on_miss = true;

struct lru {
  u32 nr_keys;
  u32 cur_keys;
  u64 max_cap;
  u64 cur_cap;
  // stat
  u64 nr_set;
  u64 nr_add;
  u64 nr_get;
  u64 nr_del;
  // internal
  u64 nr_rmv; // really delete
  u64 nr_hit; // get-hit
  u64 nr_mis; // get-miss
  u64 nr_evi; // eviction

  u64 * bitmap;
  u64 nr_bm;
  struct {
    u32 prev;
    u32 next;
  } arr[];
};

  static void *
lru_new(const u32 nr_keys, const u64 max_cap)
{
  const size_t sz = sizeof(struct lru) + ((nr_keys + 1) * sizeof(((struct lru *)NULL)->arr[0]));
  struct lru * const lru = (typeof(lru))calloc(1, sz);
  debug_assert(lru);
  lru->nr_keys = nr_keys;
  lru->cur_keys = 0;
  lru->max_cap = max_cap;
  lru->cur_cap = 0;
  const u64 nr_bm = (nr_keys>>6) + 1;
  lru->bitmap = (typeof(lru->bitmap))calloc(1, nr_bm * sizeof(lru->bitmap[0]));
  debug_assert(lru->bitmap);
  lru->nr_bm = nr_bm;
  for (u64 i = 0; i < nr_keys; i++) {
    lru->arr[i].prev = OUTSIDE;
    lru->arr[i].next = OUTSIDE;
  }
  lru->arr[nr_keys].prev = nr_keys;
  lru->arr[nr_keys].next = nr_keys;
  return (void *)lru;
}

  static void
lru_destory(void * const lru)
{
  free(((struct lru *)lru)->bitmap);
  free(lru);
}

  static inline bool
lru_in(struct lru * const lru, const u32 key)
{
  debug_assert(key < lru->nr_keys);
  if (lru->arr[key].prev == OUTSIDE) {
    debug_assert(lru->arr[key].next == OUTSIDE);
    return false;
  } else {
    return true;
  }
}

  static inline void
lru_remove(struct lru * const lru, const u32 key)
{
  debug_assert(key < lru->nr_keys);
  if (lru_in(lru, key)) { // in here
    const u32 next = lru->arr[key].next;
    const u32 prev = lru->arr[key].prev;

    debug_assert(lru->cur_keys);
    lru->cur_cap -= 1;
    lru->cur_keys--;
    lru->arr[prev].next = next;
    lru->arr[next].prev = prev;
    // clean up
    lru->arr[key].prev = OUTSIDE;
    lru->arr[key].next = OUTSIDE;
    lru->bitmap[key>>6] &= (~(UINT64_C(1) << (key & 0x3fu)));
  }
}

  static inline void
lru_insert(void * const ptr, const u32 key)
{
  struct lru * const lru = (typeof(lru))ptr;
  const u32 nr_keys = lru->nr_keys;
  debug_assert(false == lru_in(lru, key));
  const u32 head0 = lru->arr[nr_keys].next;
  lru->arr[key].next = head0;
  lru->arr[key].prev = nr_keys;
  lru->arr[head0].prev = key;
  lru->arr[nr_keys].next = key;
  lru->cur_cap += 1;
  lru->cur_keys++;
  debug_assert(lru->cur_keys <= nr_keys);
  lru->bitmap[key>>6] |= (UINT64_C(1) << (key & 0x3fu));
}

  static void
lru_evict1(struct lru * const lru)
{
  debug_assert(lru->nr_keys > 0);
  const u32 nr_keys = lru->nr_keys;
  const u32 tail0 = lru->arr[nr_keys].prev;
  lru_remove(lru, tail0);
  lru->nr_evi++;
}

  static void
lru_set(void * const ptr, const u32 key)
{
  struct lru * const lru = (typeof(lru))ptr;
  debug_assert(key < lru->nr_keys);
  lru->nr_set++;

  if (lru_in(lru, key)) {
    lru_remove(lru, key);
  }
  lru_insert(lru, key);
  // eviction
  while (lru->cur_cap > lru->max_cap) {
    lru_evict1(lru);
  }
}

  static void
lru_get(void * const ptr, const u32 key)
{
  struct lru * const lru = (typeof(lru))ptr;
  debug_assert(key < lru->nr_keys);
  lru->nr_get++;

  if (lru_in(lru, key)) {
    lru->nr_hit++;
    lru_remove(lru, key);
    lru_insert(lru, key);
  } else {
    lru->nr_mis++;
    if (__set_on_miss) {
      lru_set(ptr, key);
    }
  }
}

  static void
lru_del(void * const ptr, const u32 key)
{
  struct lru * const lru = (typeof(lru))ptr;
  debug_assert(key < lru->nr_keys);
  lru->nr_del++;

  if (lru_in(lru, key)) {
    lru->nr_rmv++;
    lru_remove(lru, key);
  }
}

  static void
lru_collect_stat(void * const ptr, struct common_stat * const out)
{
  struct lru * const lru = (typeof(lru))ptr;
  strcpy(out->name, "LRU");
  out->nr_keys = lru->nr_keys;
  out->cap = lru->max_cap;
  out->size = lru->cur_cap;
  out->hit = lru->nr_hit;
  out->miss = lru->nr_mis;
}

  static void
lru_clean_stat(void * const ptr)
{
  struct lru * const lru = (typeof(lru))ptr;
  lru->nr_set = 0;
  lru->nr_add = 0;
  lru->nr_get = 0;
  lru->nr_del = 0;

  lru->nr_rmv = 0;
  lru->nr_hit = 0;
  lru->nr_mis = 0;
  lru->nr_evi = 0;
}

static struct rep_api lru_api = {
  .new = lru_new,
  .destory = lru_destory,
  .op_set = lru_set,
  .op_get = lru_get,
  .op_del = lru_del,
  .collect_stat = lru_collect_stat,
  .clean_stat = lru_clean_stat,
};

//// ARC
#define ARC_T1 ((0))
#define ARC_B1 ((1))
#define ARC_T2 ((2))
#define ARC_B2 ((3))

struct arc {
  u32 nr_keys;
  u64 max_cap;

  u64 p;
  u64 caps[4];

  // stat
  u64 nr_set;
  u64 nr_add;
  u64 nr_get;
  u64 nr_del;
  // internal
  u64 nr_rmv; // really delete
  u64 nr_hit; // get-hit
  u64 nr_mis; // get-miss
  u64 nr_evi; // eviction

  struct {
    struct {
      u32 prev;
      u32 next;
    } node[4];
  } __attribute__((packed)) arr[];
};

  static void *
arc_new(const u32 nr_keys, const u64 max_cap)
{
  const size_t sz = sizeof(struct arc) + ((nr_keys + 1) * sizeof(((struct arc *)NULL)->arr[0]));
  struct arc * const arc = (typeof(arc))calloc(1, sz);
  debug_assert(arc);
  arc->nr_keys = nr_keys;
  arc->max_cap = max_cap;
  arc->caps[0] = 0;
  arc->caps[1] = 0;
  arc->caps[2] = 0;
  arc->caps[3] = 0;
  for (u64 i = 0; i <= nr_keys; i++) {
    for (u64 j = 0; j < 4; j++) {
      arc->arr[i].node[j].prev = (i<nr_keys)?OUTSIDE:nr_keys;
      arc->arr[i].node[j].next = (i<nr_keys)?OUTSIDE:nr_keys;
    }
  }
  return (void *)arc;
}

  static void
arc_destory(void * const arc)
{
  free(arc);
}

  static inline bool
arc_in(const u32 where, struct arc * const arc, const u32 key)
{
  debug_assert(where < 4);
  debug_assert(key < arc->nr_keys);
  if (arc->arr[key].node[where].prev == OUTSIDE) {
    debug_assert(arc->arr[key].node[where].next == OUTSIDE);
    return false;
  } else {
    return true;
  }
}

  static inline void
arc_remove(const u32 where, struct arc * const arc, const u32 key)
{
  debug_assert(where < 4);
  debug_assert(key < arc->nr_keys);
  if (arc_in(where, arc, key)) {
    const u32 next = arc->arr[key].node[where].next;
    const u32 prev = arc->arr[key].node[where].prev;
    arc->arr[next].node[where].prev = prev;
    arc->arr[prev].node[where].next = next;
    arc->arr[key].node[where].next = OUTSIDE;
    arc->arr[key].node[where].prev = OUTSIDE;
    // clean
    arc->caps[where] -= 1;
  }
}

  static inline void
arc_insert(const u32 where, struct arc * const arc, const u32 key)
{
  const u32 nr_keys = arc->nr_keys;
  debug_assert(where < 4);
  debug_assert(false == arc_in(where, arc, key));
  const u32 head0 = arc->arr[nr_keys].node[where].next;

  arc->arr[key].node[where].prev = nr_keys;
  arc->arr[key].node[where].next = head0;
  arc->arr[head0].node[where].prev = key;
  arc->arr[nr_keys].node[where].next = key;
  arc->caps[where] += 1;
}

  static inline u32
arc_lru(const u32 where, struct arc * const arc)
{
  const u32 nr_keys = arc->nr_keys;
  const u32 lru = arc->arr[nr_keys].node[where].prev;
  debug_assert(lru < nr_keys);
  return lru;
}

  static inline void
arc_remove_lru(const u32 where, struct arc * const arc)
{
  const u32 victim = arc_lru(where, arc);
  arc_remove(where, arc, victim);
}

  static inline bool
arc_resident(struct arc * const arc, const u32 key)
{
  return (arc_in(ARC_T1, arc, key) || arc_in(ARC_T2, arc, key)) ? true : false;
}

  static inline void
arc_move(const u32 fromwhere, const u32 towhere, struct arc * const arc, const u32 key)
{
  debug_assert(arc_in(fromwhere, arc, key));
  debug_assert(key < arc->nr_keys);
  arc_remove(fromwhere, arc, key);
  arc_insert(towhere, arc, key);
}

  static inline void
arc_replace(struct arc * const arc)
{
  while (((arc->caps[ARC_T1] + arc->caps[ARC_T2]) > arc->max_cap) && arc->caps[ARC_T1] > arc->p) {
    const u32 victim = arc_lru(ARC_T1, arc);
    arc_move(ARC_T1, ARC_B1, arc, victim);
  }
  while (((arc->caps[ARC_T1] + arc->caps[ARC_T2]) > arc->max_cap) && arc->caps[ARC_T2] > (arc->max_cap - arc->p)) {
    const u32 victim = arc_lru(ARC_T2, arc);
    arc_move(ARC_T2, ARC_B2, arc, victim);
  }
}

  static inline void
arc_set(void * const ptr, const u32 key)
{
  struct arc * const arc = (typeof(arc))ptr;
  debug_assert(key < arc->nr_keys);
  if (arc_in(ARC_T1, arc, key)) { // case I.1
    arc_remove(ARC_T1, arc, key);
    arc_insert(ARC_T2, arc, key);

  } else if (arc_in(ARC_T2, arc, key)) { // Case I.1
    arc_remove(ARC_T2, arc, key);
    arc_insert(ARC_T2, arc, key);

  } else if (arc_in(ARC_B1, arc, key)) { // Case II
    const u64 d1 = (arc->caps[ARC_B1] > arc->caps[ARC_B2])?1:(arc->caps[ARC_B2]/arc->caps[ARC_B1]);
    const u64 pp = arc->p + d1;
    arc->p = (pp < arc->max_cap) ? pp : arc->max_cap;
    arc_remove(ARC_B1, arc, key);
    arc_insert(ARC_T2, arc, key);
    arc_replace(arc);

  } else if (arc_in(ARC_B2, arc, key)) { // Case III
    const u64 d2 = (arc->caps[ARC_B2] > arc->caps[ARC_B1])?1:(arc->caps[ARC_B1]/arc->caps[ARC_B2]);
    const u64 pp = arc->p - d2;
    arc->p = (arc->p < d2) ? 0 : pp;
    arc_remove(ARC_B2, arc, key);
    arc_insert(ARC_T2, arc, key);
    arc_replace(arc);

  } else { // Case IV
    arc_insert(ARC_T1, arc, key);
    arc_replace(arc);
    while ((arc->caps[ARC_T1] + arc->caps[ARC_B1]) > arc->max_cap) { // balance L1
      if (arc->caps[ARC_B1] > 0) {
        arc_remove_lru(ARC_B1, arc);
      } else {
        arc_remove_lru(ARC_T1, arc);
      }
    }

    while ((arc->caps[ARC_T1] + arc->caps[ARC_B1] + arc->caps[ARC_T2] + arc->caps[ARC_B2]) > (arc->max_cap << 1)) {
      if (arc->caps[ARC_B2] > 0) {
        arc_remove_lru(ARC_B2, arc);
      } else {
        arc_remove_lru(ARC_T2, arc);
      }
    }
  }
}

  static inline void
arc_get(void * const ptr, const u32 key)
{
  struct arc * const arc = (typeof(arc))ptr;
  if (arc_resident(arc, key)) {
    arc->nr_hit++;
    arc_set(ptr, key);
  } else {
    arc->nr_mis++;
    if (__set_on_miss) {
      arc_set(ptr, key);
    }
  }
}

  static inline void
arc_del(void * const ptr, const u32 key)
{
  struct arc * const arc = (typeof(arc))ptr;
  arc_remove(ARC_T1, arc, key);
  arc_remove(ARC_T2, arc, key);
}

  static void
arc_collect_stat(void * const ptr, struct common_stat * const out)
{
  struct arc * const arc = (typeof(arc))ptr;
  strcpy(out->name, "ARC");
  const u64 cur_cap = arc->caps[ARC_T1] + arc->caps[ARC_T2];
  out->nr_keys = arc->nr_keys;
  out->cap = arc->max_cap;
  out->size = cur_cap;
  out->hit = arc->nr_hit;
  out->miss = arc->nr_mis;
}

  static void
arc_clean_stat(void * const ptr)
{
  struct arc * const arc = (typeof(arc))ptr;
  arc->nr_hit = 0;
  arc->nr_mis = 0;
}

static struct rep_api arc_api = {
  .new = arc_new,
  .destory = arc_destory,
  .op_set = arc_set,
  .op_get = arc_get,
  .op_del = arc_del,
  .collect_stat = arc_collect_stat,
  .clean_stat = arc_clean_stat,
};

struct replay_info {
  const u32 * tracemap;
  u64 na;
  void * rep;
  const struct rep_api * api;
  u32 nr_keys;
  u64 ecap;
  double srate;
  double lb;
  double ub;
};

#define RAND64_MAX   ((UINT64_C(0xffffffffffffffff)))
#define RAND64_MAX_D ((double)(RAND64_MAX))
  static void
fullpass(const struct replay_info * const info)
{
  info->api->clean_stat(info->rep);
  const u32 nr_keys = info->nr_keys;
  const u32 * const v = info->tracemap;
  const u64 na = info->na;

  for (u64 i = 0; i < na; i++) {
    if (v[i] < nr_keys) {
      const double r = ((double)xxhash64(&v[i], sizeof(v[i])))/RAND64_MAX_D;
      if (r >= info->lb && r <= info->ub) {
        info->api->op_get(info->rep, v[i]);
      }
    }
  }
}

  static u64
getfilesize(const char * const file)
{
  FILE * const f = fopen(file, "r");
  if (f == NULL) {
    fprintf(stderr, "open %s failed\n", file);
    exit(1);
  }

  fseek(f, 0l, SEEK_END);
  const u64 na = (u64)ftell(f);
  fclose(f);
  return na;
}

  static double
runtrace106(const u32 nr_keys, const u32 * const tracemap, const u64 na,
    const u64 max_cap, const struct rep_api * const api, const double srate)
{
  double lb = 0.0;
  //if (srate < 0.8) {
  //  lb = random_double() * (1.0 - srate);
  //  debug_assert((lb + srate) <= 1.0);
  //}
  const  double ub = lb + srate;
  const u64 runcap = (u64)(((double)max_cap) * srate);
  void * const rep = api->new(nr_keys, runcap);
  if (rep == NULL) {
    fprintf(stderr, "allocation failed\n");
    return 0.0;
  }

  struct replay_info info = {
    .tracemap = tracemap,
    .na = na,
    .rep = rep,
    .api = api,
    .nr_keys = nr_keys,
    .ecap = max_cap,
    .srate = srate,
    .lb = lb,
    .ub = ub,
  };

  fullpass(&info);

  struct common_stat st;
  api->collect_stat(rep, &st);
  //const double all = (double)(st.hit + st.miss);
  // 3.1.1 Error Reduction
  const double all = ((double)(na - 1)) * srate;
  const double miss = (double)(st.miss);
  api->destory(rep);
  return miss / all;
}

struct mrpoint {
  u64 cap;
  double mr;
  double dt;
};

  int
main(int argc, char ** argv)
{
  if (argc < 5) {
    printf("usage: %s <algo> <trace> <sampling-rate(0 to 1)> <#probes>\n", argv[0]);
    exit(0);
  }

  // argv[1]
  struct rep_api * api = NULL;
  if (strcmp(argv[1], "lru") == 0) {
    api = &lru_api;
  } else if (strcmp(argv[1], "arc") == 0) {
    api = &arc_api;
  } else {
    fprintf(stderr, "invalid api\n");
    exit(0);
  }

  // argv[2]
  const char * const filename = argv[2];

  // argv[3]
  double srate = 1.0;
  sscanf(argv[3], "%lf", &srate);
  if (srate <= 0.0 || srate > 1.0) {
    fprintf(stderr, "invalid srate\n");
    exit(0);
  }

  // argv[4]
  u64 probes = strtoull(argv[4], NULL, 10);
  struct mrpoint mrc[probes + 2];

  const double gmul = 1.0 / (1024.0 * 1024.0 * 1024.0);
  const u64 filesize = getfilesize(filename);
  const u64 na = filesize / sizeof(u32);
  const int mfd = open(filename, O_RDONLY);
  u32 * const tracemap = mmap(NULL, filesize, PROT_READ, MAP_SHARED, mfd, 0);
  if (tracemap == MAP_FAILED) {
    fprintf(stderr, "map failed\n");
    exit(1);
  }
  const u32 nr_keys = tracemap[na - 1];
  // compute real max_cap
  struct bitmap *bm = bitmap_create(nr_keys);
  for (u64 i = 0; i < na; i++) {
    bitmap_set1(bm, tracemap[i]);
  }
  const u32 used_keys = bitmap_count(bm);
  free(bm);
  const u64 max_cap = (u64)used_keys;
  const u64 unit_cap = max_cap / probes;
  for (u64 i = 0; i < probes; i++) {
    mrc[i].cap = unit_cap * (i + 1);
    const double t0 = time_sec_double();
    const double mr = runtrace106(nr_keys, tracemap, na, mrc[i].cap, api, srate);
    const double dt = time_diff_sec_double(t0);
    mrc[i].mr = mr;
    mrc[i].dt = dt;
    printf("%s %s %s %s PROBE %lu CAP %12lu %.9lf MR %.6lf DT %.6lf\n",
        argv[1], argv[2], argv[3], argv[4],
        i, mrc[i].cap, ((double)mrc[i].cap) * gmul, mrc[i].mr, mrc[i].dt);
  }

  close(mfd);
  munmap(tracemap, filesize);
  return 0;
}
