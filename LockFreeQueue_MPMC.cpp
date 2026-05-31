
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#define _GNU_SOURCE  // Required for CPU affinity functions
#include <sched.h>   // Contains cpu_set_t definition
#include <pthread.h> // Required for pthread_setaffinity_np()
#include "LockFreeQueue_MPMC.hpp"

constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int NUMA_NODE_0 = 0;
constexpr int NUMA_NODE_1 = 1;

void pinThreadToCore(int threadIndex, int numaNode) {
    /*
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int cores_per_node = std::thread::hardware_concurrency();
    int core_id = (threadIndex % cores_per_node) + (numaNode * cores_per_node);
    assert(core_id < std::thread::hardware_concurrency());
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    */
}

int main() {
    LockFreeQueue<int> queue;
    std::vector<std::thread> threads;

    // Start producer threads
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([i, &queue]() {
            pinThreadToCore(i, NUMA_NODE_0);
            for (int j = 0; j < WORKLOAD; ++j) {
                queue.enqueue(j);
            }
        });
    }

    // Start consumer threads
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([i, &queue]() {
            pinThreadToCore(i, NUMA_NODE_1);
            int value;
            int pops = 0;
            while (pops < WORKLOAD) {
                if (queue.dequeue(value)) {
                    ++pops;
                }
            }
        });
    }

    // Join threads
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Completed.\n";
    return 0;
}
