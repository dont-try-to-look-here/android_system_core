/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <backtrace/Backtrace.h>
#include <backtrace/BacktraceMap.h>

// For the THREAD_SIGNAL definition.
#include "BacktraceThread.h"

#include <cutils/atomic.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "thread_utils.h"

// Number of microseconds per milliseconds.
#define US_PER_MSEC             1000

// Number of nanoseconds in a second.
#define NS_PER_SEC              1000000000ULL

// Number of simultaneous dumping operations to perform.
#define NUM_THREADS  40

// Number of simultaneous threads running in our forked process.
#define NUM_PTRACE_THREADS 5

struct thread_t {
  pid_t tid;
  int32_t state;
  pthread_t threadId;
  void* data;
};

struct dump_thread_t {
  thread_t thread;
  Backtrace* backtrace;
  int32_t* now;
  int32_t done;
};

extern "C" {
// Prototypes for functions in the test library.
int test_level_one(int, int, int, int, void (*)(void*), void*);

int test_recursive_call(int, void (*)(void*), void*);
}

uint64_t NanoTime() {
  struct timespec t = { 0, 0 };
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<uint64_t>(t.tv_sec * NS_PER_SEC + t.tv_nsec);
}

void DumpFrames(Backtrace* backtrace) {
  if (backtrace->NumFrames() == 0) {
    printf("    No frames to dump\n");
    return;
  }

  for (size_t i = 0; i < backtrace->NumFrames(); i++) {
    printf("    %s\n", backtrace->FormatFrameData(i).c_str());
  }
}

void WaitForStop(pid_t pid) {
  uint64_t start = NanoTime();

  siginfo_t si;
  while (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) < 0 && (errno == EINTR || errno == ESRCH)) {
    if ((NanoTime() - start) > NS_PER_SEC) {
      printf("The process did not get to a stopping point in 1 second.\n");
      break;
    }
    usleep(US_PER_MSEC);
  }
}

bool ReadyLevelBacktrace(Backtrace* backtrace) {
  // See if test_level_four is in the backtrace.
  bool found = false;
  for (Backtrace::const_iterator it = backtrace->begin(); it != backtrace->end(); ++it) {
    if (it->func_name == "test_level_four") {
      found = true;
      break;
    }
  }

  return found;
}

void VerifyLevelDump(Backtrace* backtrace) {
  ASSERT_GT(backtrace->NumFrames(), static_cast<size_t>(0));
  ASSERT_LT(backtrace->NumFrames(), static_cast<size_t>(MAX_BACKTRACE_FRAMES));

  // Look through the frames starting at the highest to find the
  // frame we want.
  size_t frame_num = 0;
  for (size_t i = backtrace->NumFrames()-1; i > 2; i--) {
    if (backtrace->GetFrame(i)->func_name == "test_level_one") {
      frame_num = i;
      break;
    }
  }
  ASSERT_LT(static_cast<size_t>(0), frame_num);
  ASSERT_LE(static_cast<size_t>(3), frame_num);

  ASSERT_EQ(backtrace->GetFrame(frame_num)->func_name, "test_level_one");
  ASSERT_EQ(backtrace->GetFrame(frame_num-1)->func_name, "test_level_two");
  ASSERT_EQ(backtrace->GetFrame(frame_num-2)->func_name, "test_level_three");
  ASSERT_EQ(backtrace->GetFrame(frame_num-3)->func_name, "test_level_four");
}

void VerifyLevelBacktrace(void*) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyLevelDump(backtrace.get());
}

bool ReadyMaxBacktrace(Backtrace* backtrace) {
  return (backtrace->NumFrames() == MAX_BACKTRACE_FRAMES);
}

void VerifyMaxDump(Backtrace* backtrace) {
  ASSERT_EQ(backtrace->NumFrames(), static_cast<size_t>(MAX_BACKTRACE_FRAMES));
  // Verify that the last frame is our recursive call.
  ASSERT_EQ(backtrace->GetFrame(MAX_BACKTRACE_FRAMES-1)->func_name,
            "test_recursive_call");
}

void VerifyMaxBacktrace(void*) {
  std::unique_ptr<Backtrace> backtrace(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyMaxDump(backtrace.get());
}

void ThreadSetState(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);
  android_atomic_acquire_store(1, &thread->state);
  volatile int i = 0;
  while (thread->state) {
    i++;
  }
}

void VerifyThreadTest(pid_t tid, void (*VerifyFunc)(Backtrace*)) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), tid));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyFunc(backtrace.get());
}

bool WaitForNonZero(int32_t* value, uint64_t seconds) {
  uint64_t start = NanoTime();
  do {
    if (android_atomic_acquire_load(value)) {
      return true;
    }
  } while ((NanoTime() - start) < seconds * NS_PER_SEC);
  return false;
}

TEST(libbacktrace, local_trace) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelBacktrace, nullptr), 0);
}

void VerifyIgnoreFrames(
    Backtrace* bt_all, Backtrace* bt_ign1,
    Backtrace* bt_ign2, const char* cur_proc) {
  EXPECT_EQ(bt_all->NumFrames(), bt_ign1->NumFrames() + 1);
  EXPECT_EQ(bt_all->NumFrames(), bt_ign2->NumFrames() + 2);

  // Check all of the frames are the same > the current frame.
  bool check = (cur_proc == nullptr);
  for (size_t i = 0; i < bt_ign2->NumFrames(); i++) {
    if (check) {
      EXPECT_EQ(bt_ign2->GetFrame(i)->pc, bt_ign1->GetFrame(i+1)->pc);
      EXPECT_EQ(bt_ign2->GetFrame(i)->sp, bt_ign1->GetFrame(i+1)->sp);
      EXPECT_EQ(bt_ign2->GetFrame(i)->stack_size, bt_ign1->GetFrame(i+1)->stack_size);

      EXPECT_EQ(bt_ign2->GetFrame(i)->pc, bt_all->GetFrame(i+2)->pc);
      EXPECT_EQ(bt_ign2->GetFrame(i)->sp, bt_all->GetFrame(i+2)->sp);
      EXPECT_EQ(bt_ign2->GetFrame(i)->stack_size, bt_all->GetFrame(i+2)->stack_size);
    }
    if (!check && bt_ign2->GetFrame(i)->func_name == cur_proc) {
      check = true;
    }
  }
}

void VerifyLevelIgnoreFrames(void*) {
  std::unique_ptr<Backtrace> all(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(all.get() != nullptr);
  ASSERT_TRUE(all->Unwind(0));

  std::unique_ptr<Backtrace> ign1(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));

  std::unique_ptr<Backtrace> ign2(
      Backtrace::Create(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));

  VerifyIgnoreFrames(all.get(), ign1.get(), ign2.get(), "VerifyLevelIgnoreFrames");
}

TEST(libbacktrace, local_trace_ignore_frames) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelIgnoreFrames, nullptr), 0);
}

TEST(libbacktrace, local_max_trace) {
  ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, VerifyMaxBacktrace, nullptr), 0);
}

void VerifyProcTest(pid_t pid, pid_t tid, bool share_map,
                    bool (*ReadyFunc)(Backtrace*),
                    void (*VerifyFunc)(Backtrace*)) {
  pid_t ptrace_tid;
  if (tid < 0) {
    ptrace_tid = pid;
  } else {
    ptrace_tid = tid;
  }
  uint64_t start = NanoTime();
  bool verified = false;
  do {
    usleep(US_PER_MSEC);
    if (ptrace(PTRACE_ATTACH, ptrace_tid, 0, 0) == 0) {
      // Wait for the process to get to a stopping point.
      WaitForStop(ptrace_tid);

      std::unique_ptr<BacktraceMap> map;
      if (share_map) {
        map.reset(BacktraceMap::Create(pid));
      }
      std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, tid, map.get()));
      ASSERT_TRUE(backtrace->Unwind(0));
      ASSERT_TRUE(backtrace.get() != nullptr);
      if (ReadyFunc(backtrace.get())) {
        VerifyFunc(backtrace.get());
        verified = true;
      }

      ASSERT_TRUE(ptrace(PTRACE_DETACH, ptrace_tid, 0, 0) == 0);
    }
    // If 5 seconds have passed, then we are done.
  } while (!verified && (NanoTime() - start) <= 5 * NS_PER_SEC);
  ASSERT_TRUE(verified);
}

TEST(libbacktrace, ptrace_trace) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyLevelBacktrace, VerifyLevelDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

TEST(libbacktrace, ptrace_trace_shared_map) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }

  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, true, ReadyLevelBacktrace, VerifyLevelDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

TEST(libbacktrace, ptrace_max_trace) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyMaxBacktrace, VerifyMaxDump);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

void VerifyProcessIgnoreFrames(Backtrace* bt_all) {
  std::unique_ptr<Backtrace> ign1(Backtrace::Create(bt_all->Pid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));

  std::unique_ptr<Backtrace> ign2(Backtrace::Create(bt_all->Pid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));

  VerifyIgnoreFrames(bt_all, ign1.get(), ign2.get(), nullptr);
}

TEST(libbacktrace, ptrace_ignore_frames) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }
  VerifyProcTest(pid, BACKTRACE_CURRENT_THREAD, false, ReadyLevelBacktrace, VerifyProcessIgnoreFrames);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

// Create a process with multiple threads and dump all of the threads.
void* PtraceThreadLevelRun(void*) {
  EXPECT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
  return nullptr;
}

void GetThreads(pid_t pid, std::vector<pid_t>* threads) {
  // Get the list of tasks.
  char task_path[128];
  snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);

  DIR* tasks_dir = opendir(task_path);
  ASSERT_TRUE(tasks_dir != nullptr);
  struct dirent* entry;
  while ((entry = readdir(tasks_dir)) != nullptr) {
    char* end;
    pid_t tid = strtoul(entry->d_name, &end, 10);
    if (*end == '\0') {
      threads->push_back(tid);
    }
  }
  closedir(tasks_dir);
}

TEST(libbacktrace, ptrace_threads) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    for (size_t i = 0; i < NUM_PTRACE_THREADS; i++) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

      pthread_t thread;
      ASSERT_TRUE(pthread_create(&thread, &attr, PtraceThreadLevelRun, nullptr) == 0);
    }
    ASSERT_NE(test_level_one(1, 2, 3, 4, nullptr, nullptr), 0);
    _exit(1);
  }

  // Check to see that all of the threads are running before unwinding.
  std::vector<pid_t> threads;
  uint64_t start = NanoTime();
  do {
    usleep(US_PER_MSEC);
    threads.clear();
    GetThreads(pid, &threads);
  } while ((threads.size() != NUM_PTRACE_THREADS + 1) &&
      ((NanoTime() - start) <= 5 * NS_PER_SEC));
  ASSERT_EQ(threads.size(), static_cast<size_t>(NUM_PTRACE_THREADS + 1));

  ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);
  WaitForStop(pid);
  for (std::vector<int>::const_iterator it = threads.begin(); it != threads.end(); ++it) {
    // Skip the current forked process, we only care about the threads.
    if (pid == *it) {
      continue;
    }
    VerifyProcTest(pid, *it, false, ReadyLevelBacktrace, VerifyLevelDump);
  }
  ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

  kill(pid, SIGKILL);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
}

void VerifyLevelThread(void*) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), gettid()));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyLevelDump(backtrace.get());
}

TEST(libbacktrace, thread_current_level) {
  ASSERT_NE(test_level_one(1, 2, 3, 4, VerifyLevelThread, nullptr), 0);
}

void VerifyMaxThread(void*) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), gettid()));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyMaxDump(backtrace.get());
}

TEST(libbacktrace, thread_current_max) {
  ASSERT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, VerifyMaxThread, nullptr), 0);
}

void* ThreadLevelRun(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);

  thread->tid = gettid();
  EXPECT_NE(test_level_one(1, 2, 3, 4, ThreadSetState, data), 0);
  return nullptr;
}

TEST(libbacktrace, thread_level_trace) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  // Make sure that the thread signal used is not visible when compiled for
  // the target.
#if !defined(__GLIBC__)
  ASSERT_LT(THREAD_SIGNAL, SIGRTMIN);
#endif

  // Save the current signal action and make sure it is restored afterwards.
  struct sigaction cur_action;
  ASSERT_TRUE(sigaction(THREAD_SIGNAL, nullptr, &cur_action) == 0);

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyLevelDump(backtrace.get());

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);

  // Verify that the old action was restored.
  struct sigaction new_action;
  ASSERT_TRUE(sigaction(THREAD_SIGNAL, nullptr, &new_action) == 0);
  EXPECT_EQ(cur_action.sa_sigaction, new_action.sa_sigaction);
  // The SA_RESTORER flag gets set behind our back, so a direct comparison
  // doesn't work unless we mask the value off. Mips doesn't have this
  // flag, so skip this on that platform.
#ifdef SA_RESTORER
  cur_action.sa_flags &= ~SA_RESTORER;
  new_action.sa_flags &= ~SA_RESTORER;
#endif
  EXPECT_EQ(cur_action.sa_flags, new_action.sa_flags);
}

TEST(libbacktrace, thread_ignore_frames) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  std::unique_ptr<Backtrace> all(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(all.get() != nullptr);
  ASSERT_TRUE(all->Unwind(0));

  std::unique_ptr<Backtrace> ign1(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(ign1.get() != nullptr);
  ASSERT_TRUE(ign1->Unwind(1));

  std::unique_ptr<Backtrace> ign2(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(ign2.get() != nullptr);
  ASSERT_TRUE(ign2->Unwind(2));

  VerifyIgnoreFrames(all.get(), ign1.get(), ign2.get(), nullptr);

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);
}

void* ThreadMaxRun(void* data) {
  thread_t* thread = reinterpret_cast<thread_t*>(data);

  thread->tid = gettid();
  EXPECT_NE(test_recursive_call(MAX_BACKTRACE_FRAMES+10, ThreadSetState, data), 0);
  return nullptr;
}

TEST(libbacktrace, thread_max_trace) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadMaxRun, &thread_data) == 0);

  // Wait for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);
  ASSERT_TRUE(backtrace->Unwind(0));

  VerifyMaxDump(backtrace.get());

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);
}

void* ThreadDump(void* data) {
  dump_thread_t* dump = reinterpret_cast<dump_thread_t*>(data);
  while (true) {
    if (android_atomic_acquire_load(dump->now)) {
      break;
    }
  }

  // The status of the actual unwind will be checked elsewhere.
  dump->backtrace = Backtrace::Create(getpid(), dump->thread.tid);
  dump->backtrace->Unwind(0);

  android_atomic_acquire_store(1, &dump->done);

  return nullptr;
}

TEST(libbacktrace, thread_multiple_dump) {
  // Dump NUM_THREADS simultaneously.
  std::vector<thread_t> runners(NUM_THREADS);
  std::vector<dump_thread_t> dumpers(NUM_THREADS);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (size_t i = 0; i < NUM_THREADS; i++) {
    // Launch the runners, they will spin in hard loops doing nothing.
    runners[i].tid = 0;
    runners[i].state = 0;
    ASSERT_TRUE(pthread_create(&runners[i].threadId, &attr, ThreadMaxRun, &runners[i]) == 0);
  }

  // Wait for tids to be set.
  for (std::vector<thread_t>::iterator it = runners.begin(); it != runners.end(); ++it) {
    ASSERT_TRUE(WaitForNonZero(&it->state, 30));
  }

  // Start all of the dumpers at once, they will spin until they are signalled
  // to begin their dump run.
  int32_t dump_now = 0;
  for (size_t i = 0; i < NUM_THREADS; i++) {
    dumpers[i].thread.tid = runners[i].tid;
    dumpers[i].thread.state = 0;
    dumpers[i].done = 0;
    dumpers[i].now = &dump_now;

    ASSERT_TRUE(pthread_create(&dumpers[i].thread.threadId, &attr, ThreadDump, &dumpers[i]) == 0);
  }

  // Start all of the dumpers going at once.
  android_atomic_acquire_store(1, &dump_now);

  for (size_t i = 0; i < NUM_THREADS; i++) {
    ASSERT_TRUE(WaitForNonZero(&dumpers[i].done, 30));

    // Tell the runner thread to exit its infinite loop.
    android_atomic_acquire_store(0, &runners[i].state);

    ASSERT_TRUE(dumpers[i].backtrace != nullptr);
    VerifyMaxDump(dumpers[i].backtrace);

    delete dumpers[i].backtrace;
    dumpers[i].backtrace = nullptr;
  }
}

TEST(libbacktrace, thread_multiple_dump_same_thread) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  thread_t runner;
  runner.tid = 0;
  runner.state = 0;
  ASSERT_TRUE(pthread_create(&runner.threadId, &attr, ThreadMaxRun, &runner) == 0);

  // Wait for tids to be set.
  ASSERT_TRUE(WaitForNonZero(&runner.state, 30));

  // Start all of the dumpers at once, they will spin until they are signalled
  // to begin their dump run.
  int32_t dump_now = 0;
  // Dump the same thread NUM_THREADS simultaneously.
  std::vector<dump_thread_t> dumpers(NUM_THREADS);
  for (size_t i = 0; i < NUM_THREADS; i++) {
    dumpers[i].thread.tid = runner.tid;
    dumpers[i].thread.state = 0;
    dumpers[i].done = 0;
    dumpers[i].now = &dump_now;

    ASSERT_TRUE(pthread_create(&dumpers[i].thread.threadId, &attr, ThreadDump, &dumpers[i]) == 0);
  }

  // Start all of the dumpers going at once.
  android_atomic_acquire_store(1, &dump_now);

  for (size_t i = 0; i < NUM_THREADS; i++) {
    ASSERT_TRUE(WaitForNonZero(&dumpers[i].done, 30));

    ASSERT_TRUE(dumpers[i].backtrace != nullptr);
    VerifyMaxDump(dumpers[i].backtrace);

    delete dumpers[i].backtrace;
    dumpers[i].backtrace = nullptr;
  }

  // Tell the runner thread to exit its infinite loop.
  android_atomic_acquire_store(0, &runner.state);
}

// This test is for UnwindMaps that should share the same map cursor when
// multiple maps are created for the current process at the same time.
TEST(libbacktrace, simultaneous_maps) {
  BacktraceMap* map1 = BacktraceMap::Create(getpid());
  BacktraceMap* map2 = BacktraceMap::Create(getpid());
  BacktraceMap* map3 = BacktraceMap::Create(getpid());

  Backtrace* back1 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map1);
  EXPECT_TRUE(back1->Unwind(0));
  delete back1;
  delete map1;

  Backtrace* back2 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map2);
  EXPECT_TRUE(back2->Unwind(0));
  delete back2;
  delete map2;

  Backtrace* back3 = Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD, map3);
  EXPECT_TRUE(back3->Unwind(0));
  delete back3;
  delete map3;
}

TEST(libbacktrace, fillin_erases) {
  BacktraceMap* back_map = BacktraceMap::Create(getpid());

  backtrace_map_t map;

  map.start = 1;
  map.end = 3;
  map.flags = 1;
  map.name = "Initialized";
  back_map->FillIn(0, &map);
  delete back_map;

  ASSERT_FALSE(BacktraceMap::IsValid(map));
  ASSERT_EQ(static_cast<uintptr_t>(0), map.start);
  ASSERT_EQ(static_cast<uintptr_t>(0), map.end);
  ASSERT_EQ(0, map.flags);
  ASSERT_EQ("", map.name);
}

TEST(libbacktrace, format_test) {
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), BACKTRACE_CURRENT_THREAD));
  ASSERT_TRUE(backtrace.get() != nullptr);

  backtrace_frame_data_t frame;
  frame.num = 1;
  frame.pc = 2;
  frame.sp = 0;
  frame.stack_size = 0;
  frame.func_offset = 0;

  // Check no map set.
  frame.num = 1;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000000000002  <unknown>",
#else
  EXPECT_EQ("#01 pc 00000002  <unknown>",
#endif
            backtrace->FormatFrameData(&frame));

  // Check map name empty, but exists.
  frame.map.start = 1;
  frame.map.end = 1;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000000000001  <unknown>",
#else
  EXPECT_EQ("#01 pc 00000001  <unknown>",
#endif
            backtrace->FormatFrameData(&frame));


  // Check relative pc is set and map name is set.
  frame.pc = 0x12345679;
  frame.map.name = "MapFake";
  frame.map.start =  1;
  frame.map.end =  1;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake",
#endif
            backtrace->FormatFrameData(&frame));

  // Check func_name is set, but no func offset.
  frame.func_name = "ProcFake";
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake (ProcFake)",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake (ProcFake)",
#endif
            backtrace->FormatFrameData(&frame));

  // Check func_name is set, and func offset is non-zero.
  frame.func_offset = 645;
#if defined(__LP64__)
  EXPECT_EQ("#01 pc 0000000012345678  MapFake (ProcFake+645)",
#else
  EXPECT_EQ("#01 pc 12345678  MapFake (ProcFake+645)",
#endif
            backtrace->FormatFrameData(&frame));
}

struct map_test_t {
  uintptr_t start;
  uintptr_t end;
};

bool map_sort(map_test_t i, map_test_t j) {
  return i.start < j.start;
}

void VerifyMap(pid_t pid) {
  char buffer[4096];
  snprintf(buffer, sizeof(buffer), "/proc/%d/maps", pid);

  FILE* map_file = fopen(buffer, "r");
  ASSERT_TRUE(map_file != nullptr);
  std::vector<map_test_t> test_maps;
  while (fgets(buffer, sizeof(buffer), map_file)) {
    map_test_t map;
    ASSERT_EQ(2, sscanf(buffer, "%" SCNxPTR "-%" SCNxPTR " ", &map.start, &map.end));
    test_maps.push_back(map);
  }
  fclose(map_file);
  std::sort(test_maps.begin(), test_maps.end(), map_sort);

  std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(pid));

  // Basic test that verifies that the map is in the expected order.
  std::vector<map_test_t>::const_iterator test_it = test_maps.begin();
  for (BacktraceMap::const_iterator it = map->begin(); it != map->end(); ++it) {
    ASSERT_TRUE(test_it != test_maps.end());
    ASSERT_EQ(test_it->start, it->start);
    ASSERT_EQ(test_it->end, it->end);
    ++test_it;
  }
  ASSERT_TRUE(test_it == test_maps.end());
}

TEST(libbacktrace, verify_map_remote) {
  pid_t pid;

  if ((pid = fork()) == 0) {
    while (true) {
    }
    _exit(0);
  }
  ASSERT_LT(0, pid);

  ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);

  // Wait for the process to get to a stopping point.
  WaitForStop(pid);

  // The maps should match exactly since the forked process has been paused.
  VerifyMap(pid);

  ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);
}

void* ThreadReadTest(void* data) {
  thread_t* thread_data = reinterpret_cast<thread_t*>(data);

  thread_data->tid = gettid();

  // Create two map pages.
  // Mark the second page as not-readable.
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
  uint8_t* memory;
  if (posix_memalign(reinterpret_cast<void**>(&memory), pagesize, 2 * pagesize) != 0) {
    return reinterpret_cast<void*>(-1);
  }

  if (mprotect(&memory[pagesize], pagesize, PROT_NONE) != 0) {
    return reinterpret_cast<void*>(-1);
  }

  // Set up a simple pattern in memory.
  for (size_t i = 0; i < pagesize; i++) {
    memory[i] = i;
  }

  thread_data->data = memory;

  // Tell the caller it's okay to start reading memory.
  android_atomic_acquire_store(1, &thread_data->state);

  // Loop waiting for everything
  while (thread_data->state) {
  }

  free(memory);

  android_atomic_acquire_store(1, &thread_data->state);

  return nullptr;
}

void RunReadTest(Backtrace* backtrace, uintptr_t read_addr) {
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));

  // Create a page of data to use to do quick compares.
  uint8_t* expected = new uint8_t[pagesize];
  for (size_t i = 0; i < pagesize; i++) {
    expected[i] = i;
  }
  uint8_t* data = new uint8_t[2*pagesize];
  // Verify that we can only read one page worth of data.
  size_t bytes_read = backtrace->Read(read_addr, data, 2 * pagesize);
  ASSERT_EQ(pagesize, bytes_read);
  ASSERT_TRUE(memcmp(data, expected, pagesize) == 0);

  // Verify unaligned reads.
  for (size_t i = 1; i < sizeof(word_t); i++) {
    bytes_read = backtrace->Read(read_addr + i, data, 2 * sizeof(word_t));
    ASSERT_EQ(2 * sizeof(word_t), bytes_read);
    ASSERT_TRUE(memcmp(data, &expected[i], 2 * sizeof(word_t)) == 0)
        << "Offset at " << i << " failed";
  }
  delete data;
  delete expected;
}

TEST(libbacktrace, thread_read) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  thread_t thread_data = { 0, 0, 0, nullptr };
  ASSERT_TRUE(pthread_create(&thread, &attr, ThreadReadTest, &thread_data) == 0);

  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 10));

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(getpid(), thread_data.tid));
  ASSERT_TRUE(backtrace.get() != nullptr);

  RunReadTest(backtrace.get(), reinterpret_cast<uintptr_t>(thread_data.data));

  android_atomic_acquire_store(0, &thread_data.state);

  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 10));
}

volatile uintptr_t g_ready = 0;
volatile uintptr_t g_addr = 0;

void ForkedReadTest() {
  // Create two map pages.
  size_t pagesize = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
  uint8_t* memory;
  if (posix_memalign(reinterpret_cast<void**>(&memory), pagesize, 2 * pagesize) != 0) {
    perror("Failed to allocate memory\n");
    exit(1);
  }

  // Mark the second page as not-readable.
  if (mprotect(&memory[pagesize], pagesize, PROT_NONE) != 0) {
    perror("Failed to mprotect memory\n");
    exit(1);
  }

  // Set up a simple pattern in memory.
  for (size_t i = 0; i < pagesize; i++) {
    memory[i] = i;
  }

  g_addr = reinterpret_cast<uintptr_t>(memory);
  g_ready = 1;

  while (1) {
    usleep(US_PER_MSEC);
  }
}

TEST(libbacktrace, process_read) {
  pid_t pid;
  if ((pid = fork()) == 0) {
    ForkedReadTest();
    exit(0);
  }
  ASSERT_NE(-1, pid);

  bool test_executed = false;
  uint64_t start = NanoTime();
  while (1) {
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) == 0) {
      WaitForStop(pid);

      std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, pid));

      uintptr_t read_addr;
      size_t bytes_read = backtrace->Read(reinterpret_cast<uintptr_t>(&g_ready),
                                          reinterpret_cast<uint8_t*>(&read_addr),
                                          sizeof(uintptr_t));
      ASSERT_EQ(sizeof(uintptr_t), bytes_read);
      if (read_addr) {
        // The forked process is ready to be read.
        bytes_read = backtrace->Read(reinterpret_cast<uintptr_t>(&g_addr),
                                     reinterpret_cast<uint8_t*>(&read_addr),
                                     sizeof(uintptr_t));
        ASSERT_EQ(sizeof(uintptr_t), bytes_read);

        RunReadTest(backtrace.get(), read_addr);

        test_executed = true;
        break;
      }
      ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);
    }
    if ((NanoTime() - start) > 5 * NS_PER_SEC) {
      break;
    }
    usleep(US_PER_MSEC);
  }
  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);

  ASSERT_TRUE(test_executed);
}

#if defined(ENABLE_PSS_TESTS)
#include "GetPss.h"

#define MAX_LEAK_BYTES 32*1024UL

void CheckForLeak(pid_t pid, pid_t tid) {
  // Do a few runs to get the PSS stable.
  for (size_t i = 0; i < 100; i++) {
    Backtrace* backtrace = Backtrace::Create(pid, tid);
    ASSERT_TRUE(backtrace != nullptr);
    ASSERT_TRUE(backtrace->Unwind(0));
    delete backtrace;
  }
  size_t stable_pss = GetPssBytes();

  // Loop enough that even a small leak should be detectable.
  for (size_t i = 0; i < 4096; i++) {
    Backtrace* backtrace = Backtrace::Create(pid, tid);
    ASSERT_TRUE(backtrace != nullptr);
    ASSERT_TRUE(backtrace->Unwind(0));
    delete backtrace;
  }
  size_t new_pss = GetPssBytes();
  size_t abs_diff = (new_pss > stable_pss) ? new_pss - stable_pss : stable_pss - new_pss;
  // As long as the new pss is within a certain amount, consider everything okay.
  ASSERT_LE(abs_diff, MAX_LEAK_BYTES);
}

TEST(libbacktrace, check_for_leak_local) {
  CheckForLeak(BACKTRACE_CURRENT_PROCESS, BACKTRACE_CURRENT_THREAD);
}

TEST(libbacktrace, check_for_leak_local_thread) {
  thread_t thread_data = { 0, 0, 0, nullptr };
  pthread_t thread;
  ASSERT_TRUE(pthread_create(&thread, nullptr, ThreadLevelRun, &thread_data) == 0);

  // Wait up to 2 seconds for the tid to be set.
  ASSERT_TRUE(WaitForNonZero(&thread_data.state, 2));

  CheckForLeak(BACKTRACE_CURRENT_PROCESS, thread_data.tid);

  // Tell the thread to exit its infinite loop.
  android_atomic_acquire_store(0, &thread_data.state);

  ASSERT_TRUE(pthread_join(thread, nullptr) == 0);
}

TEST(libbacktrace, check_for_leak_remote) {
  pid_t pid;

  if ((pid = fork()) == 0) {
    while (true) {
    }
    _exit(0);
  }
  ASSERT_LT(0, pid);

  ASSERT_TRUE(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0);

  // Wait for the process to get to a stopping point.
  WaitForStop(pid);

  CheckForLeak(pid, BACKTRACE_CURRENT_THREAD);

  ASSERT_TRUE(ptrace(PTRACE_DETACH, pid, 0, 0) == 0);

  kill(pid, SIGKILL);
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);
}
#endif
