/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
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
 */

#include "katana/PagePool.h"

#include "katana/Logging.h"

static katana::internal::PageAllocState<>* PA;

void
katana::internal::setPagePoolState(PageAllocState<>* pa) {
  KATANA_LOG_VASSERT(!(PA && pa), "double Initialization of PageAllocState");
  PA = pa;
}

int
katana::numPagePoolAllocTotal() {
  return PA->countAll();
}

int
katana::numPagePoolAllocForThread(unsigned tid) {
  return PA->count(tid);
}

void*
katana::pagePoolAlloc() {
  return PA->pageAlloc();
}

void
katana::pagePoolPreAlloc(unsigned num) {
  while (num--) {
    PA->pagePreAlloc();
  }
}

void
katana::pagePoolEnsurePreallocated(unsigned num) {
  auto tid = katana::ThreadPool::getTID();
  while (PA->freeCount(tid) < num) {
    PA->pagePreAlloc();
  }
}

void
katana::pagePoolFree(void* ptr) {
  PA->pageFree(ptr);
}
