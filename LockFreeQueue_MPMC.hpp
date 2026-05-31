#include <atomic>
#include <cassert>
#include <thread>
#include <immintrin.h> // Required for _mm_pause()

#ifndef hardware_destructive_interference_size
#define hardware_destructive_interference_size 64
#endif

constexpr size_t CACHE_LINE_SIZE = hardware_destructive_interference_size;


//Michael & Scott queue
template <typename T>
class LockFreeQueue {
private:
    struct alignas(CACHE_LINE_SIZE) Node 
   {
        T data;
        std::atomic<Node*> next;

        explicit Node(T const& value) : data(value), next(nullptr) {}
        Node() : next(nullptr) {} // Dummy node constructor for simplified logic
    };

    alignas(CACHE_LINE_SIZE) std::atomic<Node*> head; // Points to dummy or oldest unconsumed node
    alignas(CACHE_LINE_SIZE) std::atomic<Node*> tail; // Points to newest node for enqueue

public:
    
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    LockFreeQueue() 
    {
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
                        std::memory_order_release, // Publish new_node by linking it into the queue.
                                                   // Consumers can reach new_node only after this release CAS succeeds.
                        std::memory_order_relaxed)) // failure = retry
                {
                    //4. Try to swing tail to the new node (not mandatory but improves progress, so relax is enough)
                    // Advance tail (optimization; not part of correctness)
                    //compare_exchange_weak may fail spuriously and need looping, so use compare_exchange_strong for simplicity here
                    tail.compare_exchange_strong(old_tail, new_node,
                        std::memory_order_relaxed, 
                        std::memory_order_relaxed); 
                    return;
                }
                
                //CAS failed → means another thread has made progress and appended a node. 
                
            } else {
                // 5. Someone already appended a node, but tail still points to an older node.
                // Tail is behind → help advance it (optimization only)
                //compare_exchange_weak may fail spuriously and need looping, so use compare_exchange_strong for simplicity here
                tail.compare_exchange_strong(old_tail, old_tail_next,
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
            //head is never null as dummy node guarantees
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
                out = std::move(old_head_next->data); //for complex T
                //Without hazard pointers, epoch reclamation, RCU, etc., this queue leaks memory.
                //ABA is not triggered in practice as we do not reclaim and memory will not be reused
                //delete old_head; 
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
};
