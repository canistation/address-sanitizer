//===-- asan_thread_registry.cc ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
//
// AsanThreadRegistry-related code. AsanThreadRegistry is a container
// for summaries of all created threads.
//===----------------------------------------------------------------------===//

#include "asan_stack.h"
#include "asan_thread.h"
#include "asan_thread_registry.h"

namespace __asan {

static AsanThreadRegistry asan_thread_registry(__asan::LINKER_INITIALIZED);

AsanThreadRegistry &asanThreadRegistry() {
  return asan_thread_registry;
}

static void DestroyAsanTsd(void *tsd) {
  AsanThread *t = (AsanThread*)tsd;
  if (t != asanThreadRegistry().GetMain()) {
    delete t;
  }
}

AsanThreadRegistry::AsanThreadRegistry(LinkerInitialized x)
    : main_thread_(x),
      main_thread_summary_(x),
      accumulated_stats_(x) { }

void AsanThreadRegistry::Init() {
  CHECK(0 == pthread_key_create(&tls_key_, DestroyAsanTsd));
  tls_key_created_ = true;
  SetCurrent(&main_thread_);
  main_thread_.set_summary(&main_thread_summary_);
  main_thread_summary_.set_thread(&main_thread_);
  thread_summaries_[0] = &main_thread_summary_;
  n_threads_ = 1;
}

void AsanThreadRegistry::RegisterThread(AsanThread *thread, int parent_tid,
                                        AsanStackTrace *stack) {
  ScopedLock lock(&mu_);
  CHECK(n_threads_ > 0);
  int tid = n_threads_;
  n_threads_++;
  CHECK(n_threads_ < kMaxNumberOfThreads);
  AsanThreadSummary *summary = new AsanThreadSummary(tid, parent_tid, stack);
  summary->set_thread(thread);
  thread_summaries_[tid] = summary;
  thread->set_summary(summary);
}

void AsanThreadRegistry::UnregisterThread(AsanThread *thread) {
  ScopedLock lock(&mu_);
  thread->stats().FlushToStats(&accumulated_stats_);
  AsanThreadSummary *summary = thread->summary();
  CHECK(summary);
  summary->set_thread(NULL);
}

AsanThread *AsanThreadRegistry::GetMain() {
  return &main_thread_;
}

AsanThread *AsanThreadRegistry::GetCurrent() {
  CHECK(tls_key_created_);
  AsanThread *thread = (AsanThread*)pthread_getspecific(tls_key_);
  return thread;
}

void AsanThreadRegistry::SetCurrent(AsanThread *t) {
  CHECK(0 == pthread_setspecific(tls_key_, t));
  CHECK(pthread_getspecific(tls_key_) == t);
}

AsanStats *AsanThreadRegistry::GetCurrentThreadStats() {
  AsanThread *t = GetCurrent();
  return (t) ? &(t->stats()) : &main_thread_.stats();
}

AsanStats AsanThreadRegistry::GetAccumulatedStats() {
  ScopedLock lock(&mu_);
  UpdateAccumulatedStatsUnlocked();
  return accumulated_stats_;
}

size_t AsanThreadRegistry::GetCurrentAllocatedBytes() {
  ScopedLock lock(&mu_);
  UpdateAccumulatedStatsUnlocked();
  return accumulated_stats_.malloced - accumulated_stats_.freed;
}

AsanThreadSummary *AsanThreadRegistry::FindByTid(int tid) {
  CHECK(tid >= 0);
  CHECK(tid < n_threads_);
  CHECK(thread_summaries_[tid]);
  return thread_summaries_[tid];
}

AsanThread *AsanThreadRegistry::FindThreadByStackAddress(uintptr_t addr) {
  ScopedLock lock(&mu_);
  for (int tid = 0; tid < n_threads_; tid++) {
    AsanThread *t = thread_summaries_[tid]->thread();
    if (!t) continue;
    if (t->FakeStack().AddrIsInFakeStack(addr) || t->AddrIsInStack(addr)) {
      return t;
    }
  }
  return 0;
}

void AsanThreadRegistry::UpdateAccumulatedStatsUnlocked() {
  for (int tid = 0; tid < n_threads_; tid++) {
    AsanThread *t = thread_summaries_[tid]->thread();
    if (t != NULL) {
      t->stats().FlushToStats(&accumulated_stats_);
    }
  }
}

}  // namespace __asan