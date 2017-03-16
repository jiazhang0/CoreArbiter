/* Copyright (c) 2015-2017 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef CORE_ARBITER_SERVER_H_
#define CORE_ARBITER_SERVER_H_

#include <atomic>
#include <deque>
#include <fstream>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <stdexcept>
#include <vector>

#include "CoreArbiterCommon.h"
#include "Syscall.h"

#define MAX_EPOLL_EVENTS 1000

namespace CoreArbiter {

class CoreArbiterServer {
  public:
    CoreArbiterServer(std::string socketPath,
                      std::string sharedMemPathPrefix,
                      std::vector<core_t> exclusiveCores={},
                      bool arbitrateImmediately=true);
    ~CoreArbiterServer();
    void startArbitration();
    void endArbitration();

    // Point at the most recently constructed instance of the
    // CoreArbiterServer.
    static CoreArbiterServer* volatile mostRecentInstance;

  private:
    struct ThreadInfo;
    struct ProcessInfo;
    struct CoreInfo;

    /**
     * Used to keep track of all the information for a core. There is a separate
     * CoreInfo instance for every core that the server has control over (both
     * exclusive and unmanaged). These structs are constructed when the server
     * starts up and exist for the server's entire lifetime.
     */
    struct CoreInfo {
        // The ID of this core. This ID matches what would be returned by a
        // process on this core that ran sched_getcpu().
        core_t id;

        // A pointer to the thread running exclusively on this core. NULL if
        // the core is available or unmanaged.
        struct ThreadInfo* exclusiveThread;

        // A stream pointing to the tasks file of this core's exclusive cpuset.
        std::ofstream cpusetFile;

        CoreInfo()
            : exclusiveThread(NULL)
        {}

        CoreInfo(core_t id)
            : id(id)
            , exclusiveThread(NULL)
        {}
    };

    /**
     * Used by ThreadInfo to keep track of a thread's state.
     */
    enum ThreadState {
        // Running on an exclusive core
        RUNNING_EXCLUSIVE,

        // Voluntarily running on the unmanaged core (this only happens before
        // the first call to blockUntilCoreAvailable())
        RUNNING_UNMANAGED,

        // Running on the unmanaged core because it was forceably preempted from
        // its excluisve core
        RUNNING_PREEMPTED,

        // Not running, waiting to be put on core
        BLOCKED
    };

    /**
     * Keeps track of all the information for a thread. A ThreadInfo instance
     * exists from the time that a new thread first connects with a server until
     * that connection closes.
     */
    struct ThreadInfo {
        // The ID of this thread (self-reported when the thread first
        // establishes a connection). All threads within a process are expected
        // to have unique IDs.
        pid_t id;

        // A pointer to the process that this thread belongs to.
        struct ProcessInfo* process;

        // The file descriptor for the socket used to communicate with this
        // thread.
        int socket;

        // A pointer to the core this thread is running exclusively on. NULL
        // if this thread is not running exclusively.
        struct CoreInfo* core;

        // The current state of this thread. When a thread first registers it
        // is assumed to be RUNNING_UNMANAGED.
        ThreadState state;

        ThreadInfo() {}

        ThreadInfo(pid_t threadId, struct ProcessInfo* process, int socket)
            : id(threadId)
            , process(process)
            , socket(socket)
            , core(NULL)
            , state(RUNNING_UNMANAGED)
        {}
    };

    /**
     * Keeps track of all the information for a process, including which threads
     * belong to this process. ProcessInfo instances are generated as-needed,
     * when a thread registers with a proces that we have not seen before. A
     * process is not deleted from memory until all of its threads' connections
     * have closed.
     */
    struct ProcessInfo {
        // The ID of this process (self-reported when a thread first establishes
        // a connection). All processes on this machine are expected to have
        // unique IDs.
        pid_t id;

        // The file descriptor that is mmapped into memory for communication
        // between the process and server (see coreReleaseRequestCount below).
        int sharedMemFd;

        // A monotonically increasing counter in shared memory of the number of
        // cores this process is expected to release. This value is compared
        // with coreReleaseCount to determine whether the process owes a core.
        // This value is only incremented by the server; nobody else should have
        // write access.
        std::atomic<uint64_t>* coreReleaseRequestCount;

        bool* threadPreempted;

        // A monotonically increasing counter of the number of cores this
        // process has owned and then released.
        uint64_t coreReleaseCount;

        // The number of cores that this process currently has threads running
        // exclusively on (at all priority levels).
        uint32_t totalCoresOwned;

        // How many cores this process desires at each priority level. Smaller
        // indexes mean higher priority.
        std::vector<uint32_t> desiredCorePriorities;

        // A map of ThreadState to the threads this process owns in that state.
        std::unordered_map<ThreadState, std::unordered_set<struct ThreadInfo*>,
                          std::hash<int>> threadStateToSet;

        ProcessInfo()
            : totalCoresOwned(0)
            , desiredCorePriorities(NUM_PRIORITIES)
        {}

        ProcessInfo(pid_t id, int sharedMemFd,
                    std::atomic<uint64_t>* coreReleaseRequestCount,
                    bool* threadPreempted)
            : id(id)
            , sharedMemFd(sharedMemFd)
            , coreReleaseRequestCount(coreReleaseRequestCount)
            , threadPreempted(threadPreempted)
            , coreReleaseCount(0)
            , totalCoresOwned(0)
            , desiredCorePriorities(NUM_PRIORITIES)
        {}
    };

    bool handleEvents();
    void acceptConnection(int listenSocket);
    void threadBlocking(int socket);
    void coresRequested(int socket);
    void countBlockedThreads(int socket);
    void timeoutThreadPreemption(int timerFd);
    void cleanupConnection(int socket);
    void distributeCores();
    void requestCoreRelease(struct CoreInfo* core);
    void totalAvailableCores(int socket);

    bool readData(int socket, void* buf, size_t numBytes, std::string err);
    bool sendData(int socket, void* buf, size_t numBytes, std::string err);

    void createCpuset(std::string dirName, std::string cores, std::string mems);
    void moveProcsToCpuset(std::string fromPath, std::string toPath);
    void removeOldCpusets(std::string arbiterCpusetPath);
    void moveThreadToExclusiveCore(struct ThreadInfo* thread,
                                   struct CoreInfo* core);
    void removeThreadFromExclusiveCore(struct ThreadInfo* thread);
    void changeThreadState(struct ThreadInfo* thread, ThreadState state);

    void installSignalHandler();

    // The path to the socket that the server is listening for new connections
    // on.
    std::string socketPath;

    // The file descriptor for the socket that the server is listening for new
    // connections on.
    int listenSocket;

    // The prefix that will be used to generate shared memory file paths for
    // each process. This can be either a file or directory.
    std::string sharedMemPathPrefix;

    // The file descriptor used to block on client requests.
    int epollFd;

    // A map of core preemption timers to the process that a core should be
    // retrieved from.
    std::unordered_map<int, pid_t> timerFdToProcessId;

    // The amount of time in milliseconds to wait before forceably preempting
    // a a thread from its exclusive core to the unmanaged core.
    uint64_t preemptionTimeout;

    // Maps thread socket file desriptors to their associated threads.
    std::unordered_map<int, struct ThreadInfo*> threadSocketToInfo;

    // Maps process IDs to their associated processes.
    std::unordered_map<pid_t, struct ProcessInfo*> processIdToInfo;

    // Contains the information for all the exclusive cores that this server
    // manages. This is set up upon server construction and does not change.
    std::vector<struct CoreInfo*> exclusiveCores;

    // A set of the threads currently running on cores in exclusiveCores.
    std::unordered_set<struct ThreadInfo*> exclusiveThreads;

    // Information about the unmanaged core. This is established on server
    // connection and does not change.
    struct CoreInfo unmanagedCore;

    // The smallest index in the vector is the highest priority and the first
    // entry in the deque is the process that requested a core at that priority
    // first
    std::vector<std::deque<struct ProcessInfo*>> corePriorityQueues;

    // When this file descriptor is written, the core arbiter will return from
    // startArbitration.
    volatile int terminationFd;

    // The path to the root cpuset directory.
    static std::string cpusetPath;

    // Wrap all system calls for easier testing.
    static Syscall* sys;

    // Used for testing to avoid unnecessary setup and code execution.
    static bool testingSkipCpusetAllocation;
    static bool testingSkipCoreDistribution;
    static bool testingSkipSend;
    static bool testingSkipMemoryDeallocation;
};

}

int ensureParents(const char *path, mode_t mode = S_IRWXU);

#endif // CORE_ARBITER_SERVER_H_
