#include <atomic>
#include <memory>
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#define _GNU_SOURCE  // Required for CPU affinity functions
#include <sched.h>   // Contains cpu_set_t definition
#include <pthread.h> // Required for pthread_setaffinity_np()
//#include <emmintrin.h> // Required for _mm_pause()

#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif

constexpr int NUM_PRODUCERS = 4;
constexpr int NUM_CONSUMERS = 4;
constexpr int WORKLOAD = 1000;
constexpr int NUMA_NODE_0 = 0;
constexpr int NUMA_NODE_1 = 1;
constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;


//Michael & Scott queue
template <typename T>
class LockFreeQueue {
private:
    struct alignas(CACHE_LINE_SIZE) Node {
        T data;
        std::atomic<Node*> next;

        explicit Node(T const& value) : data(value), next(nullptr) {}
        Node() : next(nullptr) {} // Dummy node constructor for simplified logic
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head; // Points to dummy or oldest unconsumed node
    alignas(CACHE_LINE_SIZE) std::atomic<Node*> tail; // Points to newest node for enqueue

public:
    LockFreeQueue() {
        // Initialize queue with dummy node to simplify empty queue handling
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    // Enqueue operation - Append at tail
    void enqueue(T const& value) {
        Node* new_node = new Node(value);
        new_node->next.store(nullptr, std::memory_order_relaxed);

        while (true) {
            Node* old_tail = tail.load(std::memory_order_acquire);
            Node* next = old_tail->next.load(std::memory_order_acquire);

            if (next == nullptr) {
                // CAS to link new node at end of the list
                if (old_tail->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    // Try to swing tail to the new node (not mandatory but improves progress)
                    tail.compare_exchange_weak(old_tail, new_node,
                        std::memory_order_release, std::memory_order_relaxed); // May fail unnecessarily
                    return;
                }

                //if (old_tail->next.compare_exchange_weak(next, new_node, ..) failed, 
                //it means another thread has made progress and appended a node. 
                //But the old_tail pointer we're working with is now stale—still referencing the previous tail.
                //we can reload old_tail immediately after failure so that:
                //1. ensure that we are always working with the latest tail.
                //2. we avoid redundant CAS attempts on outdated pointers.
                //3. help reduce unnecessary spinning (retries) and contention in tight loops.
                //Reloading old_tail after a failed link attempt ensures we're not swinging tail toward an already outdated node.
                // This is a common pattern in lock-free algorithms to ensure progress.
                old_tail = tail.load(std::memory_order_relaxed); // Reload tail after failure
            } else {
                // Tail not pointing to actual end, try to advance it
                tail.compare_exchange_weak(old_tail, next,
                    std::memory_order_release, std::memory_order_relaxed);
            }

            // Optional: reduce contention with brief pause
            #ifdef __x86_64__
            _mm_pause();  // On x86, better than yielding
            #else
            std::this_thread::yield();
            #endif
        }
    }

    // Dequeue operation - Remove from head
    bool dequeue(T& out) {
        while (true) {
            //old_head can't be outside loop in queue (unlike stack). 
            //Because, In a stack pop, all correctness depends on one shared pointer (head).
            //In a queue dequeue(), correctness depends on two shared pointers (head and head->next).
            Node* old_head = head.load(std::memory_order_acquire);
            Node* next = old_head->next.load(std::memory_order_acquire);

            if (next == nullptr) {
                return false; // Queue is empty
            }

            // Attempt to swing head forward
            if (head.compare_exchange_weak(old_head, next,
                    std::memory_order_release, std::memory_order_relaxed)) {
                out = next->data; // Or std::move(next->data) for complex T
                delete old_head; // Safe to reclaim dummy or consumed node
                return true;
            }

            // Optional: reduce contention
            #ifdef __x86_64__
            _mm_pause();
            #else
            std::this_thread::yield();
            #endif
        }
    }

    // Optional: relaxed check for emptiness
    bool empty() const {

        //empty() is a speculative check and doesn’t need strong ordering.
        //empty() is a speculative check, typically used to avoid expensive operations like dequeue() when the queue 
        //appears empty. In this context, visibility and 
        //synchronization are not critical—the operation doesn’t modify state and is inherently best-effort.
        //Using relaxed memory order is sufficient for this purpose, as it allows the compiler and CPU to optimize
        //the operation without enforcing unnecessary synchronization.
        //Using acquire memory order here would be overkill, as it would enforce a full memory
        //barrier, which is not needed for a simple check like this.
        //Acquire memory order is typically used when you need to ensure that all previous operations are visible
        //before the current operation, which is not the case here.
        
       //Node* h = head.load(std::memory_order_acquire);
        Node* h = head.load(std::memory_order_relaxed);
        //return h->next.load(std::memory_order_acquire) == nullptr;
        return h->next.load(std::memory_order_relaxed) == nullptr;


    }

    ~LockFreeQueue() {
        //Node* current = head.exchange(nullptr, std::memory_order_acquire);
        //Destructor is single-threaded by contract: 
        //since no other threads should be accessing the queue during destruction,
        //We can safely use relaxed memory order here because we are not concerned with visibility or synchronization
        //of the head pointer across threads at this point.
        
        Node* current = head.load(std::memory_order_relaxed);
        while (current) {
           //Node* next = current->next.load(std::memory_order_acquire);
           Node* next = current->next.load(std::memory_order_relaxed);
           delete current;
           current = next;
       }
    }

    // Disable copy/move operations
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;
};

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
