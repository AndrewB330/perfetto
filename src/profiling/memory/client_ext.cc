/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/profiling/memory/client_ext.h"

#include <android/fdsan.h>
#include <bionic/malloc.h>
#include <inttypes.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <tuple>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"

#include "src/profiling/common/proc_utils.h"
#include "src/profiling/memory/client.h"
#include "src/profiling/memory/scoped_spinlock.h"
#include "src/profiling/memory/unhooked_allocator.h"
#include "src/profiling/memory/wire_protocol.h"

using perfetto::profiling::ScopedSpinlock;
using perfetto::profiling::UnhookedAllocator;

namespace {
// Holds the active profiling client. Is empty at the start, or after we've
// started shutting down a profiling session. Hook invocations take shared_ptr
// copies (ensuring that the client stays alive until no longer needed), and do
// nothing if this primary pointer is empty.
//
// This shared_ptr itself is protected by g_client_lock. Note that shared_ptr
// handles are not thread-safe by themselves:
// https://en.cppreference.com/w/cpp/memory/shared_ptr/atomic
//
// To avoid on-destruction re-entrancy issues, this shared_ptr needs to be
// constructed with an allocator that uses the unhooked malloc & free functions.
// See UnhookedAllocator.
//
// We initialize this storage the first time GetClientLocked is called. We
// cannot use a static initializer because that leads to ordering problems
// of the ELF's constructors.

alignas(std::shared_ptr<perfetto::profiling::Client>) char g_client_arr[sizeof(
    std::shared_ptr<perfetto::profiling::Client>)];

bool g_client_init;

std::shared_ptr<perfetto::profiling::Client>* GetClientLocked() {
  if (!g_client_init) {
    new (g_client_arr) std::shared_ptr<perfetto::profiling::Client>;
    g_client_init = true;
  }
  return reinterpret_cast<std::shared_ptr<perfetto::profiling::Client>*>(
      &g_client_arr);
}

constexpr auto kMinHeapId = 1;

struct HeapprofdHeapInfoInternal {
  HeapprofdHeapInfo info;
  bool ready;
  bool enabled;
  uint32_t service_heap_id;
};

HeapprofdHeapInfoInternal g_heaps[256];

HeapprofdHeapInfoInternal& GetHeap(uint32_t id) {
  return g_heaps[id];
}

// Protects g_client, and serves as an external lock for sampling decisions (see
// perfetto::profiling::Sampler).
//
// We rely on this atomic's destuction being a nop, as it is possible for the
// hooks to attempt to acquire the spinlock after its destructor should have run
// (technically a use-after-destruct scenario).
std::atomic<bool> g_client_lock{false};

std::atomic<uint32_t> g_next_heap_id{kMinHeapId};

constexpr char kHeapprofdBinPath[] = "/system/bin/heapprofd";

int CloneWithoutSigchld() {
  auto ret = clone(nullptr, nullptr, 0, nullptr);
  if (ret == 0)
    android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
  return ret;
}

int ForklikeClone() {
  auto ret = clone(nullptr, nullptr, SIGCHLD, nullptr);
  if (ret == 0)
    android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
  return ret;
}

// Like daemon(), but using clone to avoid invoking pthread_atfork(3) handlers.
int Daemonize() {
  switch (ForklikeClone()) {
    case -1:
      PERFETTO_PLOG("Daemonize.clone");
      return -1;
      break;
    case 0:
      break;
    default:
      _exit(0);
      break;
  }
  if (setsid() == -1) {
    PERFETTO_PLOG("Daemonize.setsid");
    return -1;
  }
  // best effort chdir & fd close
  chdir("/");
  int fd = open("/dev/null", O_RDWR, 0);
  if (fd != -1) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
      close(fd);
  }
  return 0;
}

// Called only if |g_client_lock| acquisition fails, which shouldn't happen
// unless we're in a completely unexpected state (which we won't know how to
// recover from). Tries to abort (SIGABRT) the whole process to serve as an
// explicit indication of a bug.
//
// Doesn't use PERFETTO_FATAL as that is a single attempt to self-signal (in
// practice - SIGTRAP), while abort() tries to make sure the process has
// exited one way or another.
__attribute__((noreturn, noinline)) void AbortOnSpinlockTimeout() {
  PERFETTO_ELOG(
      "Timed out on the spinlock - something is horribly wrong. "
      "Aborting whole process.");
  abort();
}

std::string ReadSystemProperty(const char* key) {
  std::string prop_value;
  const prop_info* prop = __system_property_find(key);
  if (!prop) {
    return prop_value;  // empty
  }
  __system_property_read_callback(
      prop,
      [](void* cookie, const char* name, const char* value, uint32_t) {
        std::string* prop_value = reinterpret_cast<std::string*>(cookie);
        *prop_value = value;
      },
      &prop_value);
  return prop_value;
}

bool ForceForkPrivateDaemon() {
  // Note: if renaming the property, also update system_property.cc
  std::string mode = ReadSystemProperty("heapprofd.userdebug.mode");
  return mode == "fork";
}

std::shared_ptr<perfetto::profiling::Client> CreateClientForCentralDaemon(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  PERFETTO_LOG("Constructing client for central daemon.");
  using perfetto::profiling::Client;

  perfetto::base::Optional<perfetto::base::UnixSocketRaw> sock =
      Client::ConnectToHeapprofd(perfetto::profiling::kHeapprofdSocketFile);
  if (!sock) {
    PERFETTO_ELOG("Failed to connect to %s. This is benign on user builds.",
                  perfetto::profiling::kHeapprofdSocketFile);
    return nullptr;
  }
  return Client::CreateAndHandshake(std::move(sock.value()),
                                    unhooked_allocator);
}

std::shared_ptr<perfetto::profiling::Client> CreateClientAndPrivateDaemon(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  PERFETTO_LOG("Setting up fork mode profiling.");
  perfetto::base::UnixSocketRaw parent_sock;
  perfetto::base::UnixSocketRaw child_sock;
  std::tie(parent_sock, child_sock) = perfetto::base::UnixSocketRaw::CreatePair(
      perfetto::base::SockFamily::kUnix, perfetto::base::SockType::kStream);

  if (!parent_sock || !child_sock) {
    PERFETTO_PLOG("Failed to create socketpair.");
    return nullptr;
  }

  child_sock.RetainOnExec();

  // Record own pid and cmdline, to pass down to the forked heapprofd.
  pid_t target_pid = getpid();
  std::string target_cmdline;
  if (!perfetto::profiling::GetCmdlineForPID(target_pid, &target_cmdline)) {
    target_cmdline = "failed-to-read-cmdline";
    PERFETTO_ELOG(
        "Failed to read own cmdline, proceeding as this might be a by-pid "
        "profiling request (which will still work).");
  }

  // Prepare arguments for heapprofd.
  std::string pid_arg =
      std::string("--exclusive-for-pid=") + std::to_string(target_pid);
  std::string cmd_arg =
      std::string("--exclusive-for-cmdline=") + target_cmdline;
  std::string fd_arg =
      std::string("--inherit-socket-fd=") + std::to_string(child_sock.fd());
  const char* argv[] = {kHeapprofdBinPath, pid_arg.c_str(), cmd_arg.c_str(),
                        fd_arg.c_str(), nullptr};

  // Use fork-like clone to avoid invoking the host's pthread_atfork(3)
  // handlers. Also avoid sending the current process a SIGCHILD to further
  // reduce our interference.
  pid_t clone_pid = CloneWithoutSigchld();
  if (clone_pid == -1) {
    PERFETTO_PLOG("Failed to clone.");
    return nullptr;
  }
  if (clone_pid == 0) {  // child
    // Daemonize clones again, terminating the calling thread (i.e. the direct
    // child of the original process). So the rest of this codepath will be
    // executed in a new reparented process.
    if (Daemonize() == -1) {
      PERFETTO_PLOG("Daemonization failed.");
      _exit(1);
    }
    execv(kHeapprofdBinPath, const_cast<char**>(argv));
    PERFETTO_PLOG("Failed to execute private heapprofd.");
    _exit(1);
  }  // else - parent continuing the client setup

  child_sock.ReleaseFd().reset();  // close child socket's fd
  if (!parent_sock.SetTxTimeout(perfetto::profiling::kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set socket transmit timeout.");
    return nullptr;
  }

  if (!parent_sock.SetRxTimeout(perfetto::profiling::kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set socket receive timeout.");
    return nullptr;
  }

  // Wait on the immediate child to exit (allow for ECHILD in the unlikely case
  // we're in a process that has made its children unwaitable).
  int unused = 0;
  if (PERFETTO_EINTR(waitpid(clone_pid, &unused, __WCLONE)) == -1 &&
      errno != ECHILD) {
    PERFETTO_PLOG("Failed to waitpid on immediate child.");
    return nullptr;
  }

  return perfetto::profiling::Client::CreateAndHandshake(std::move(parent_sock),
                                                         unhooked_allocator);
}

// Note: android_mallopt(M_RESET_HOOKS) is mutually exclusive with
// heapprofd_initialize. Concurrent calls get discarded, which might be our
// unpatching attempt if there is a concurrent re-initialization running due to
// a new signal.
//
// Note: g_client can be reset by heapprofd_initialize without calling this
// function.

void DisableAllHeaps() {
  for (size_t i = kMinHeapId; i < g_next_heap_id.load(); ++i) {
    HeapprofdHeapInfoInternal& heap = GetHeap(i);
    if (!heap.ready)
      continue;
    if (heap.enabled) {
      heap.enabled = false;
      if (heap.info.callback)
        heap.info.callback(false);
    }
  }
}

void ShutdownLazy() {
  ScopedSpinlock s(&g_client_lock, ScopedSpinlock::Mode::Try);
  if (PERFETTO_UNLIKELY(!s.locked()))
    AbortOnSpinlockTimeout();

  if (!*GetClientLocked())  // other invocation already initiated shutdown
    return;

  DisableAllHeaps();
  // Clear primary shared pointer, such that later hook invocations become nops.
  GetClientLocked()->reset();

  if (!android_mallopt(M_RESET_HOOKS, nullptr, 0))
    PERFETTO_PLOG("Unpatching heapprofd hooks failed.");
}

// We're a library loaded into a potentially-multithreaded process, which might
// not be explicitly aware of this possiblity. Deadling with forks/clones is
// extremely complicated in such situations, but we attempt to handle certain
// cases.
//
// There are two classes of forking processes to consider:
//  * well-behaved processes that fork only when their threads (if any) are at a
//    safe point, and therefore not in the middle of our hooks/client.
//  * processes that fork with other threads in an arbitrary state. Though
//    technically buggy, such processes exist in practice.
//
// This atfork handler follows a crude lowest-common-denominator approach, where
// to handle the latter class of processes, we systematically leak any |Client|
// state (present only when actively profiling at the time of fork) in the
// postfork-child path.
//
// The alternative with acquiring all relevant locks in the prefork handler, and
// releasing the state postfork handlers, poses a separate class of edge cases,
// and is not deemed to be better as a result.
//
// Notes:
// * this atfork handler fires only for the |fork| libc entrypoint, *not*
//   |clone|. See client.cc's |IsPostFork| for some best-effort detection
//   mechanisms for clone/vfork.
// * it should be possible to start a new profiling session in this child
//   process, modulo the bionic's heapprofd-loading state machine being in the
//   right state.
// * we cannot avoid leaks in all cases anyway (e.g. during shutdown sequence,
//   when only individual straggler threads hold onto the Client).
void AtForkChild() {
  PERFETTO_LOG("heapprofd_client: handling atfork.");

  // A thread (that has now disappeared across the fork) could have been holding
  // the spinlock. We're now the only thread post-fork, so we can reset the
  // spinlock, though the state it protects (the |g_client| shared_ptr) might
  // not be in a consistent state.
  g_client_lock.store(false);

  DisableAllHeaps();

  // Leak the existing shared_ptr contents, including the profiling |Client| if
  // profiling was active at the time of the fork.
  // Note: this code assumes that the creation of the empty shared_ptr does not
  // allocate, which should be the case for all implementations as the
  // constructor has to be noexcept.
  new (g_client_arr) std::shared_ptr<perfetto::profiling::Client>();
}

}  // namespace

__attribute__((visibility("default"))) uint32_t heapprofd_register_heap(
    const HeapprofdHeapInfo* info,
    size_t n) {
  // For backwards compatibility, we handle HeapprofdHeapInfo that are shorter
  // than the current one (and assume all new fields are unset). If someone
  // calls us with a *newer* HeapprofdHeapInfo than this version of the library
  // understands, error out.
  if (n > sizeof(HeapprofdHeapInfo)) {
    return 0;
  }
  uint32_t next_id = g_next_heap_id.fetch_add(1);
  if (next_id >= perfetto::base::ArraySize(g_heaps)) {
    return 0;
  }
  HeapprofdHeapInfoInternal& heap = GetHeap(next_id);
  memcpy(&heap.info, info, n);
  heap.ready = true;
  return next_id;
}

__attribute__((visibility("default"))) bool
heapprofd_report_allocation(uint32_t heap_id, uint64_t id, uint64_t size) {
  const HeapprofdHeapInfoInternal& heap = GetHeap(heap_id);
  if (!heap.enabled) {
    return false;
  }
  size_t sampled_alloc_sz = 0;
  std::shared_ptr<perfetto::profiling::Client> client;
  {
    ScopedSpinlock s(&g_client_lock, ScopedSpinlock::Mode::Try);
    if (PERFETTO_UNLIKELY(!s.locked()))
      AbortOnSpinlockTimeout();

    auto* g_client_ptr = GetClientLocked();
    if (!*g_client_ptr)  // no active client (most likely shutting down)
      return false;

    sampled_alloc_sz = (*g_client_ptr)->GetSampleSizeLocked(size);
    if (sampled_alloc_sz == 0)  // not sampling
      return false;

    client = *g_client_ptr;  // owning copy
  }                          // unlock

  if (!client->RecordMalloc(heap.service_heap_id, sampled_alloc_sz, size, id)) {
    ShutdownLazy();
  }
  return true;
}

__attribute__((visibility("default"))) void heapprofd_report_free(
    uint32_t heap_id,
    uint64_t id) {
  const HeapprofdHeapInfoInternal& heap = GetHeap(heap_id);
  if (!heap.enabled) {
    return;
  }
  std::shared_ptr<perfetto::profiling::Client> client;
  {
    ScopedSpinlock s(&g_client_lock, ScopedSpinlock::Mode::Try);
    if (PERFETTO_UNLIKELY(!s.locked()))
      AbortOnSpinlockTimeout();

    client = *GetClientLocked();  // owning copy (or empty)
  }

  if (client) {
    if (!client->RecordFree(heap.service_heap_id, id))
      ShutdownLazy();
  }
}

__attribute__((visibility("default"))) bool heapprofd_init_session(
    void* (*malloc_fn)(size_t),
    void (*free_fn)(void*)) {
  static bool first_init = true;
  // Install an atfork handler to deal with *some* cases of the host forking.
  // The handler will be unpatched automatically if we're dlclosed.
  if (first_init && pthread_atfork(/*prepare=*/nullptr, /*parent=*/nullptr,
                                   &AtForkChild) != 0) {
    PERFETTO_PLOG("%s: pthread_atfork failed, not installing hooks.",
                  getprogname());
    return false;
  }
  first_init = false;

  // TODO(fmayer): Check other destructions of client and make a decision
  // whether we want to ban heap objects in the client or not.
  std::shared_ptr<perfetto::profiling::Client> old_client;
  {
    ScopedSpinlock s(&g_client_lock, ScopedSpinlock::Mode::Try);
    if (PERFETTO_UNLIKELY(!s.locked()))
      AbortOnSpinlockTimeout();

    auto* g_client_ptr = GetClientLocked();
    if (*g_client_ptr && (*g_client_ptr)->IsConnected()) {
      PERFETTO_LOG("%s: Rejecting concurrent profiling initialization.",
                   getprogname());
      return true;  // success as we're in a valid state
    }
    old_client = *g_client_ptr;
    g_client_ptr->reset();
  }

  old_client.reset();

  // The dispatch table never changes, so let the custom allocator retain the
  // function pointers directly.
  UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator(malloc_fn,
                                                                    free_fn);

  // These factory functions use heap objects, so we need to run them without
  // the spinlock held.
  std::shared_ptr<perfetto::profiling::Client> client;
  if (!ForceForkPrivateDaemon())
    client = CreateClientForCentralDaemon(unhooked_allocator);
  if (!client)
    client = CreateClientAndPrivateDaemon(unhooked_allocator);

  if (!client) {
    PERFETTO_LOG("%s: heapprofd_client not initialized, not installing hooks.",
                 getprogname());
    return false;
  }
  const perfetto::profiling::ClientConfiguration& cli_config =
      client->client_config();

  for (size_t j = kMinHeapId; j < g_next_heap_id.load(); ++j) {
    HeapprofdHeapInfoInternal& heap = GetHeap(j);
    if (!heap.ready)
      continue;

    bool matched = false;
    for (size_t i = 0; i < cli_config.num_heaps; ++i) {
      static_assert(sizeof(g_heaps[0].info.heap_name) == HEAPPROFD_HEAP_NAME_SZ,
                    "correct heap name size");
      static_assert(sizeof(cli_config.heaps[0]) == HEAPPROFD_HEAP_NAME_SZ,
                    "correct heap name size");
      if (strncmp(&cli_config.heaps[i][0], &heap.info.heap_name[0],
                  HEAPPROFD_HEAP_NAME_SZ) == 0) {
        heap.service_heap_id = i;
        if (!heap.enabled && heap.info.callback)
          heap.info.callback(true);
        heap.enabled = true;
        matched = true;
        break;
      }
    }
    if (!matched && heap.enabled) {
      heap.enabled = false;
      if (heap.info.callback)
        heap.info.callback(false);
    }
  }
  PERFETTO_LOG("%s: heapprofd_client initialized.", getprogname());
  {
    ScopedSpinlock s(&g_client_lock, ScopedSpinlock::Mode::Try);
    if (PERFETTO_UNLIKELY(!s.locked()))
      AbortOnSpinlockTimeout();

    // This cannot have been set in the meantime. There are never two concurrent
    // calls to this function, as Bionic uses atomics to guard against that.
    PERFETTO_DCHECK(*GetClientLocked() == nullptr);
    *GetClientLocked() = std::move(client);
  }
  return true;
}
