/** Memory allocator implementation -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2014, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/config.h"
#include "Galois/Runtime/Support.h"
#include "Galois/Runtime/mm/Mem.h"
#include "Galois/Runtime/ll/gio.h"

#include <fstream>
#include <limits>
#include <vector>

#ifdef GALOIS_USE_NUMA
#include <numa.h>
#include <numaif.h>
#endif

#ifdef GALOIS_USE_NUMA
static int isNumaAvailable;
#endif

//! Define to get version of library that does not depend on Galois thread primatives
//#define GALOIS_FORCE_STANDALONE

void Galois::Runtime::MM::printInterleavedStats(int minPages) {
  std::ifstream f("/proc/self/numa_maps");

  if (!f) {
    LL::gInfo("No NUMA support");
    return;
  }

  char line[2048];
  LL::gInfo("INTERLEAVED STATS BEGIN");
  while (f.getline(line, sizeof(line)/sizeof(*line))) { 
    // Chomp \n
    size_t len = strlen(line);
    if (len && line[len-1] == '\n')
      line[len-1] = '\0';

    char* start;
    if (strstr(line, "interleave") != 0) {
      LL::gInfo(line);
    } else if ((start = strstr(line, "anon=")) != 0) {
      int pages;
      if (sscanf(start, "anon=%d", &pages) == 1 && pages >= minPages) {
        LL::gInfo(line);
      }
    } else if ((start = strstr(line, "mapped=")) != 0) {
      int pages;
      if (sscanf(start, "mapped=%d", &pages) == 1 && pages >= minPages) {
        LL::gInfo(line);
      }
    }
  }
  LL::gInfo("INTERLEAVED STATS END");
}

static int numNumaPagesFor(unsigned nodeid) {
  std::ifstream f("/proc/self/numa_maps");
  if (!f) {
    return 0;
  }

  char format[2048];
  char search[2048];
  snprintf(format, sizeof(format), "N%u=%%d", nodeid);
  snprintf(search, sizeof(search), "N%u=", nodeid);

  char line[2048];
  int totalPages = 0;
  while (f.getline(line, sizeof(line)/sizeof(*line))) {
    char* start;
    if ((start = strstr(line, search)) != 0) {
      int pages;
      if (sscanf(start, format, &pages) == 1) {
        totalPages += pages;
      }
    }
  }

  return totalPages;
}

static int checkNuma() {
#ifdef GALOIS_USE_NUMA
  if (isNumaAvailable == 0) {
    isNumaAvailable = numa_available() == -1 ? -1 : 1;
    if (isNumaAvailable == -1)
      Galois::Runtime::LL::gWarn("NUMA configured but not available");
  }
  return isNumaAvailable == 1;
#else
  return false;
#endif
}

int Galois::Runtime::MM::numNumaAllocForNode(unsigned nodeid) {
  return numNumaPagesFor(nodeid);
}

int Galois::Runtime::MM::numNumaNodes() {
  if (!checkNuma())
    return 1;
#ifdef GALOIS_USE_NUMA
  return numa_num_configured_nodes();
#else
  return 1;
#endif
}

static inline int getNumaNode(unsigned tid) {
  if (!checkNuma())
    return 0;

#ifdef GALOIS_FORCE_STANDALONE
  unsigned proc = 0;
#else
  unsigned proc = Galois::Runtime::LL::getProcessorForThread(tid);
#endif

#if defined(GALOIS_USE_NUMA_OLD)
  // Assume block distribution from physical processors to numa nodes
  int numNodes = numa_num_configured_nodes();
  return proc / numNodes;
#elif defined(GALOIS_USE_NUMA)
  return numa_node_of_cpu(proc);
#else
  proc = 0; // get rid of unused warning
  return proc;
#endif
}

#ifdef GALOIS_USE_NUMA
static void *allocInterleaved(size_t len, unsigned num) {
  void* data = 0;
# if defined(GALOIS_USE_NUMA_OLD)
  nodemask_t nm = numa_no_nodes;
  for (unsigned i = 0; i < num; ++i) {
    nodemask_set(&nm, getNumaNode(i));
  }
  data = numa_alloc_interleaved_subset(len, &nm);
  // NB(ddn): Some strange bugs when empty interleaved mappings are
  // coalesced. Eagerly fault in interleaved pages to circumvent.
  if (data)
    Galois::Runtime::MM::pageIn(data, len, Galois::Runtime::MM::pageSize);
# else
  bitmask* nm = numa_allocate_nodemask();
  for (unsigned i = 0; i < num; ++i) {
    numa_bitmask_setbit(nm, getNumaNode(i));
  }
  data = numa_alloc_interleaved_subset(len, nm);
  numa_free_nodemask(nm);
  // NB(ddn): Some strange bugs when empty interleaved mappings are
  // coalesced. Eagerly fault in interleaved pages to circumvent.
  if (data)
    Galois::Runtime::MM::pageIn(data, len, Galois::Runtime::MM::pageSize);
# endif
  return data;
}
#endif

#ifdef GALOIS_USE_NUMA
static bool checkIfInterleaved(void* data, size_t len, unsigned total) {
  // Assume small allocations are interleaved properly
  if (len < Galois::Runtime::MM::hugePageSize * Galois::Runtime::MM::numNumaNodes())
    return true;

  union { void* as_vptr; char* as_cptr; uintptr_t as_uint; } d = { data };
  size_t pageSize = Galois::Runtime::MM::pageSize;
  int numNodes = Galois::Runtime::MM::numNumaNodes();

  std::vector<size_t> hist(numNodes);
  for (size_t i = 0; i < len; i += pageSize) {
    int node;
    char *mem = d.as_cptr + i;
    if (get_mempolicy(&node, NULL, 0, mem, MPOL_F_NODE|MPOL_F_ADDR) < 0) {
      //Galois::Runtime::LL::gInfo("unknown status[", mem, "]: ", strerror(errno));
    } else {
      hist[node] += 1;
    }
  }

  size_t least = std::numeric_limits<size_t>::max();
  size_t greatest = std::numeric_limits<size_t>::min();
  for (unsigned i = 0; i < total; ++i) {
    int node = getNumaNode(i);
    least = std::min(least, hist[node]);
    greatest = std::max(greatest, hist[node]);
  }

  return !total || least / (double) greatest > 0.5;
}
#endif

#ifndef GALOIS_FORCE_STANDALONE
// Figure out which subset of threads will participate in pageInInterleaved
static void createMapping(std::vector<int>& mapping, unsigned& uniqueNodes) {
  std::vector<bool> hist(Galois::Runtime::MM::numNumaNodes());
  uniqueNodes = 0;
  for (unsigned i = 0; i < mapping.size(); ++i) {
    int node = getNumaNode(i);
    if (hist[node])
      continue;
    hist[node] = true;
    uniqueNodes += 1;
    mapping[i] = node + 1;
  }
}
#endif

#ifndef GALOIS_FORCE_STANDALONE
static void pageInInterleaved(void* data, size_t len, std::vector<int>& mapping, unsigned numNodes) {
  // XXX Don't know whether memory is backed by hugepages or not, so stick with
  // smaller page size
  size_t blockSize = Galois::Runtime::MM::pageSize;
#ifdef GALOIS_FORCE_STANDALONE
  unsigned tid = 0;
#else
  unsigned tid = Galois::Runtime::LL::getTID();
#endif
  int id = mapping[tid] - 1;
  if (id < 0)
    return;
  size_t start = id * blockSize;
  size_t stride = numNodes * blockSize;
  if (len <= start)
    return;
  union { void* as_vptr; char* as_cptr; } d = { data };

  Galois::Runtime::MM::pageIn(d.as_cptr + start, len - start, stride);
}
#endif

static inline bool isNumaAlloc(void* data, size_t len) {
  union { void* as_vptr; char* as_cptr; } d = { data };
  return d.as_cptr[len-1] != 0;
}

static inline void setNumaAlloc(void* data, size_t len, bool isNuma) {
  union { void* as_vptr; char* as_cptr; } d = { data };
  d.as_cptr[len-1] = isNuma;
}

void* Galois::Runtime::MM::largeInterleavedAlloc(size_t len, bool full) {
  void* data;
#ifdef GALOIS_FORCE_STANDALONE
  unsigned total = 1;
  bool inForEach = false;
#else
  unsigned total = full ? Galois::Runtime::LL::getMaxCores() : activeThreads;
  bool inForEach = inGaloisForEach;
#endif
  bool numaAlloc = false;

  len += 1; // space for allocation metadata

  if (inForEach) {
    if (checkNuma()) {
#ifdef GALOIS_USE_NUMA
      data = allocInterleaved(len, total);
      numaAlloc = true;
#else
      data = largeAlloc(len, false);
#endif
    } else {
      data = largeAlloc(len, false);
    }
  } else {
    // DDN: Depend on first-touch policy to place memory rather than libnuma
    // calls because numa_alloc_interleaved seems to have issues properly
    // interleaving memory.
    data = largeAlloc(len, false);
#ifndef GALOIS_FORCE_STANDALONE
    unsigned uniqueNodes;
    std::vector<int> mapping(total);
    createMapping(mapping, uniqueNodes);
    getSystemThreadPool().run(total, std::bind(pageInInterleaved, data, len, std::ref(mapping), uniqueNodes));
#endif
  }

  if (!data)
    abort();

  setNumaAlloc(data, len, numaAlloc);

#ifdef GALOIS_USE_NUMA
  // numa_alloc_interleaved sometimes fails to interleave pages
  if (numaAlloc && !checkIfInterleaved(data, len, total))
    Galois::Runtime::LL::gWarn("NUMA interleaving failed: ", data, " size: ", len);
#endif

  return data;
}

void Galois::Runtime::MM::largeInterleavedFree(void* data, size_t len) {
  len += 1; // space for allocation metadata

#ifdef GALOIS_USE_NUMA
  if (isNumaAlloc(data, len)) {
    numa_free(data, len);
    return;
  }
#endif
  largeFree(data, len);
}
