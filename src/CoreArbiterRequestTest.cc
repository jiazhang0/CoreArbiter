#include <stdio.h>
#include <atomic>
#include <thread>

#include "CoreArbiterClient.h"
#include "Logger.h"
#include "PerfUtils/Cycles.h"
#include "PerfUtils/Stats.h"
#include "PerfUtils/TimeTrace.h"
#include "PerfUtils/Util.h"

/**
 * This benchmark will rapidly increase and decrease the number of cores
 * requested, to stress the core arbiter's allocation and deallocation
 * mechanism.
 */

using CoreArbiter::CoreArbiterClient;
using PerfUtils::Cycles;
using PerfUtils::TimeTrace;
using namespace CoreArbiter;

#define NUM_TRIALS 100

std::atomic<bool> end(false);

/**
 * This thread will block and unblock on the Core Arbiter's command.
 */
void
coreExec(CoreArbiterClient* client) {
    while (!end) {
        client->blockUntilCoreAvailable();
        while (!client->mustReleaseCore())
            ;
    }
}

/**
 * This thread will request an increasing number of cores and then a decreasing
 * number of cores.
 */
int
main(int argc, const char** argv) {
    const int MAX_CORES = std::thread::hardware_concurrency() - 1;
    CoreArbiterClient* client = CoreArbiterClient::getInstance();

    // Start up several threads to actually ramp up and down
    for (int i = 0; i < MAX_CORES; i++)
        (new std::thread(coreExec, std::ref(client)))->detach();

    std::vector<uint32_t> coreRequest = {0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < NUM_TRIALS; i++) {
        // First go up and then go down.
        int j;
        for (j = 1; j < MAX_CORES; j++) {
            coreRequest[0] = j;
            client->setRequestedCores(coreRequest);
        }
        for (; j > 0; j--) {
            coreRequest[0] = j;
            client->setRequestedCores(coreRequest);
        }
    }
    coreRequest[0] = MAX_CORES;
    client->setRequestedCores(coreRequest);
    end.store(true);
}