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

#include <stdio.h>
#include <thread>
#include <atomic>

#include "CoreArbiterClient.h"
#include "Logger.h"

using namespace CoreArbiter;

#define NUM_TRIALS 100

/**
  * This thread will get unblocked when a core is allocated, and will block
  * itself again when the number of cores is decreased.
  */
void coreExec(CoreArbiterClient& client) {
    client.setNumCores({1,0,0,0,0,0,0,0});
    client.blockUntilCoreAvailable();
    client.setNumCores({0,0,0,0,0,0,0,0});
    while (!client.mustReleaseCore());
    client.unregisterThread();
}

int main(){
    Logger::setLogLevel(DEBUG);
    CoreArbiterClient& client =
        CoreArbiterClient::getInstance("/tmp/CoreArbiter/testsocket");
    std::thread coreThread(coreExec, std::ref(client));

    coreThread.join();
    printf("There are %lu cores available\n", client.getNumUnoccupiedCores());
}