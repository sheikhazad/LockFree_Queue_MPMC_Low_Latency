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
    // Enqueue operation – Michael & Scott MPMC queue
    // Assumes construction (dummy node) completes before any threads start
    void enqueue(T const& value) {
        Node* new_node = new Node(value);
        new_node->next.store(nullptr, std::memory_order_relaxed);

        while (true) {
            // 1. Acquire: ensures old_tail is fully initialized before dereferencing
            Node* old_tail = tail.load(std::memory_order_acquire);
            // 2. Acquire pairs with the release CAS that linked next
            Node* old_tail_next = old_tail->next.load(std::memory_order_acquire);

            // If tail is pointing to the real end
            if (old_tail_next == nullptr) {
                // 3. CAS to link new node at end of the list
                //    Publish new_node by linking it into old_tail->next
                if (old_tail->next.compare_exchange_weak(old_tail_next, new_node,
                        std::memory_order_release, // publish new_node: Writing old_tail->next not just swinging node
                        std::memory_order_relaxed)) // failure = retry
                {
                    //4. Try to swing tail to the new node (not mandatory but improves progress, so relax is enough)
                    // Advance tail (optimization; not part of correctness)
                    tail.compare_exchange_weak(old_tail, new_node,
                        std::memory_order_relaxed, 
                        std::memory_order_relaxed); 
                    return;
                }

                //if (old_tail->next.compare_exchange_weak(old_tail_next, new_node, ..) failed, 
                //it means another thread has made progress and appended a node. 
                //But the old_tail pointer we're working with is now stale—still referencing the previous tail.
                //we can reload old_tail immediately after failure so that:
                //1. ensure that we are always working with the latest tail.
                //2. we avoid redundant CAS attempts on outdated pointers.
                //3. help reduce unnecessary spinning (retries) and contention in tight loops.
                //Reloading old_tail after a failed link attempt ensures we're not swinging tail toward an already outdated node.
                // This is a common pattern in lock-free algorithms to ensure progress.

                // CAS failed → someone else appended; refresh tail hint
                // Optional: Not required for correctness. It is only a performance hint, not a correctness mechanism.
                old_tail = tail.load(std::memory_order_relaxed); // Reload tail after failure
            } else {
                // 5. Tail is behind → help advance it (optimization only)
                // Tail not pointing to actual end, try to advance it
                tail.compare_exchange_weak(old_tail, old_tail_next,
                    std::memory_order_relaxed, 
                    std::memory_order_relaxed);
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
            // 1. Load current head (dummy) with acquire so it's safe to dereference
            Node* old_head = head.load(std::memory_order_acquire);
            // 2. Read the next pointer; this is the real node we might consume
            Node* old_head_next = old_head->next.load(std::memory_order_acquire);

            if (old_head_next == nullptr) {
                // Queue is logically empty: only dummy present
                return false; 
            }

            //3. Attempt to swing head forward
            if (head.compare_exchange_weak(old_head, old_head_next,
                    // We use ACQUIRE (or ACQ_REL) so we synchronize with the producer that enqueued 'next'.
                    std::memory_order_acquire, // acquire on success: ensures we see next->data safely
                    std::memory_order_relaxed)) 
            {
                //4. Now we "own" old_head_next, safe to read its data
                //out = old_head_next->data; 
                out = std::move(old_head_next->data) //for complex T
                //delete old_head; // Node reclamation requires hazard pointers / epoch GC
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
