#include <iostream>
#include <setjmp.h>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <sys/time.h>
#include <cassert>
#include <signal.h>
#include <vector>

#if defined(__x86_64__)
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g"(ret)
                 : "0"(addr));
    return ret;
}

#elif defined(__aarch64__)
/* code for 64 bit ARM (Apple Silicon) arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation function for ARM64 architecture.
   Adjust as necessary for your specific use case. */
address_t translate_address(address_t addr)
{
    address_t ret;
    // Placeholder for translation mechanism on ARM64
    // For now, we'll just return the address unchanged
    ret = addr;
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5

#endif

#define LIB_ERROR "thread library error: "
#define SYS_ERROR "system error: "
#define SECOND 1000000
#define MAX_THREAD_NUM 100 /* maximal number of threads */
#define STACK_SIZE 4096    /* stack size per thread (in bytes) */

/* External interface */
std::deque<int> *readyQueue;
std::vector<int> *sleepingVector;
int current_thread = 0;
int quantumUsecs;
int realtime = 0;
typedef void (*thread_entry_point)(void);

struct Thread
{
    sigjmp_buf env;
    char *stack;
    int virtualtime;
    bool blocked;
    int sleeptimer;
};

Thread *threads[MAX_THREAD_NUM];

void block_sig(int sig)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unblock_sig(int sig)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

void __timer_setup(int quantum_usecs);

void __advance_time()
{
    realtime++;
    for (int i = 0; i < (int)sleepingVector->size(); ++i)
    {
        int tid = sleepingVector->at(i);
        threads[tid]->sleeptimer--;
        if (threads[tid]->sleeptimer == 0)
        {
            threads[tid]->sleeptimer = -1;
            if (threads[tid]->blocked == false)
            {
                readyQueue->push_back(tid);
            }
            sleepingVector->erase(sleepingVector->begin() + i);
            i--;
        }
    }
}
/**
 * @brief Saves the current thread's context and switches to another thread.
 *
 * This function saves the context of the current thread, switches to the specified
 * thread, and resumes its execution. If the context save is successful, the thread
 * switch occurs; otherwise, it returns from the function call.
 *
 * @param tid The thread ID to switch to.
 */
void __yield(int tid, bool reset_timer = false)
{
    int ret_val = sigsetjmp(threads[current_thread]->env, 1);
    bool did_just_save_bookmark = ret_val == 0;
    if (did_just_save_bookmark)
    {
        current_thread = tid;
        __advance_time();
        threads[tid]->virtualtime++;
        if (reset_timer)
        {
            __timer_setup(quantumUsecs);
        }

        siglongjmp(threads[tid]->env, 1);
    }
    unblock_sig(SIGVTALRM);
}

/**
 * @brief Sets up a thread with the given stack and entry point.
 *
 * This function initializes a thread's environment, including its stack and entry
 * point function. It prepares the thread to be scheduled and run.
 *
 * @param tid The thread ID to set up.
 * @param stack The stack allocated for the thread.
 * @param entry_point The function to run when the thread starts.
 */
void __setup_thread(int tid, char *stack, thread_entry_point entry_point)
{
    threads[tid] = new Thread;
    // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
    // siglongjmp to jump into the thread.
    address_t sp = (address_t)stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t)entry_point;
    threads[tid]->virtualtime = 0;
    threads[tid]->sleeptimer = -1;
    threads[tid]->blocked = false;

    sigsetjmp(threads[tid]->env, 1);
    threads[tid]->env->__jmpbuf[JB_SP] = translate_address(sp);
    threads[tid]->env->__jmpbuf[JB_PC] = translate_address(pc);
    sigemptyset(&threads[tid]->env->__saved_mask);
}

/**
 * @brief Finds an available thread ID.
 *
 * This function searches for an available thread ID that is not currently in use.
 *
 * @return The available thread ID, or -1 if no available ID is found.
 */
int __find_available_tid(void)
{
    for (int i = 0; i < MAX_THREAD_NUM; i++)
    {
        if (threads[i] == nullptr)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Pops the next thread from the ready queue and switches to it.
 *
 * This function removes the front thread from the ready queue and switches to it,
 * effectively yielding execution to the next thread.
 */
void __thread_popper(bool reset_timer = false)
{
    int tid = readyQueue->front();
    readyQueue->pop_front();
    __yield(tid, reset_timer);
}

/**
 * @brief Switches to the next thread after terminating the current one.
 *
 * This function is used to handle the termination of a thread. It switches execution
 * to the next thread in the ready queue.
 */
void __terminate_jump()
{
    int tid = readyQueue->front();
    readyQueue->pop_front();
    current_thread = tid;
    __advance_time();
    __timer_setup(quantumUsecs);
    std::cout << "debug tid " << tid << std::endl;
    std::cout << "debug queue " << readyQueue->front() << " size: " << readyQueue->size() << std::endl;
    siglongjmp(threads[tid]->env, 1);
}

/**
 * @brief Signal handler for the virtual timer.
 *
 * This function handles the virtual timer signal (SIGVTALRM). It adds the current
 * thread to the ready queue and switches to the next thread.
 *
 * @param sig The signal number (should be SIGVTALRM).
 */
void __time_handler(int sig)
{
    readyQueue->push_back(current_thread);
    __thread_popper();
}

/**
 * @brief Frees the resources allocated for a thread.
 *
 * This function releases the resources (stack and thread structure) allocated for
 * the specified thread.
 *
 * @param tid The thread ID to free.
 */
void __free_thread(int tid)
{
    if (threads[tid] != nullptr)
    {
        delete threads[tid]->stack;
        delete threads[tid];
        threads[tid] = nullptr;
    }
}

/**
 * @brief Removes a thread from the ready queue.
 *
 * This function finds and removes the specified thread from the ready queue.
 *
 * @param tid The thread ID to remove.
 */
void __remove_from_database(int tid)
{
    auto it = std::find(readyQueue->begin(), readyQueue->end(), tid);

    // Check if the element was found

    if (it != readyQueue->end())
    {
        readyQueue->erase(it);
    }

    for (int i = 0; i < (int)sleepingVector->size(); ++i)
    {
        if (sleepingVector->at(i) == tid)
        {
            sleepingVector->erase(sleepingVector->begin() + i);
        }
    }
}
/**
 * @brief Sets up the virtual timer for thread scheduling.
 *
 * This function configures and starts a virtual timer to manage the scheduling of
 * threads based on the specified quantum time.
 *
 * @param quantum_usecs The length of a quantum in microseconds.
 */
void __timer_setup(int quantum_usecs)
{
    struct sigaction sa = {0};
    struct itimerval timer;

    // Install __time_handler as the signal handler for SIGVTALRM.
    sa.sa_handler = &__time_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        std::cerr << SYS_ERROR << "sigaction error.\n";
    }

    // Configure the timer to expire after quantum_usecs sec... */
    timer.it_value.tv_sec = 0;              // first time interval, seconds part
    timer.it_value.tv_usec = quantum_usecs; // first time interval, microseconds part

    // configure the timer to expire every quantum_usecs sec after that.
    timer.it_interval.tv_sec = 0;              // following time intervals, seconds part
    timer.it_interval.tv_usec = quantum_usecs; // following time intervals, microseconds part

    if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
    {
        std::cerr << SYS_ERROR << "set timer failed.\n";
    }
}

// TODO: understand how to malloc in cpp.

/**
 * @brief initializes the thread library.
 *
 * Once this function returns, the main thread (tid == 0) will be set as RUNNING. There is no need to
 * provide an entry_point or to create a stack for the main thread - it will be using the "regular" stack and PC.
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
 *
 * Note: probebly need to add check for memory alloc.
 */
int uthread_init(int quantum_usecs)
{
    readyQueue = new std::deque<int>;
    sleepingVector = new std::vector<int>;
    if (quantum_usecs <= 0)
    {
        std::cerr << LIB_ERROR << "invalid quantum_usecs\n";
        return -1;
    }
    quantumUsecs = quantum_usecs;
    __setup_thread(0, nullptr, nullptr);
    __timer_setup(quantum_usecs);
    return 0;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 * It is an error to call this function with a null entry_point.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
 */
int uthread_spawn(thread_entry_point entry_point)
{
    block_sig(SIGVTALRM);
    if (readyQueue->size() == MAX_THREAD_NUM)
    {
        std::cerr << LIB_ERROR << "thread overflow\n";
        unblock_sig(SIGVTALRM);
        return -1;
    }
    // TODO maybe nullpointer.
    if (entry_point == NULL)
    {
        std::cerr << LIB_ERROR << "null entry point\n";
        unblock_sig(SIGVTALRM);
        return -1;
    }
    char *stack = new char[STACK_SIZE];
    int tid = __find_available_tid();
    __setup_thread(tid, stack, entry_point);
    readyQueue->push_back(tid);
    unblock_sig(SIGVTALRM);
    return tid;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
 */
int uthread_terminate(int tid)
{
    block_sig(SIGVTALRM);
    if (tid == 0)
    {
        for (int i = 0; i < MAX_THREAD_NUM; i++)
        {
            __free_thread(i);
        }
        exit(0);
    }
    __remove_from_database(tid);
    __free_thread(tid);
    if (tid == current_thread)
    {
        __terminate_jump();
    }
    return 0;
}

/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_block(int tid)
{
    block_sig(SIGVTALRM);
    if ((threads[tid] == nullptr) || (tid == 0))
    {
        std::cerr << LIB_ERROR << "no thread with this ID tid exists or the ID is 0\n";
        unblock_sig(SIGVTALRM);
        return -1;
    }

    threads[tid]->blocked = true;
    __remove_from_database(tid);
    if (tid == current_thread)
    {
        __thread_popper(true);
    }
    else
    {
        unblock_sig(SIGVTALRM);
    }
    return 0;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_resume(int tid)
{
    block_sig(SIGVTALRM);
    if (threads[tid] == nullptr)
    {
        std::cerr << LIB_ERROR << "no thread with this ID tid exists\n";
        unblock_sig(SIGVTALRM);
        return -1;
    }
    if (threads[tid]->blocked)
    {
        threads[tid]->blocked = false;
        if (threads[tid]->sleeptimer == -1)
        {
            readyQueue->push_back(tid);
        }
    }
    unblock_sig(SIGVTALRM);
    return 0;
}
/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY queue.
 * If the thread which was just RUNNING should also be added to the READY queue, or if multiple threads wake up
 * at the same time, the order in which they're added to the end of the READY queue doesn't matter.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid == 0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
 */
int uthread_sleep(int num_quantums)
{
    block_sig(SIGVTALRM);
    if (current_thread == 0)
    {
        unblock_sig(SIGVTALRM);
        return -1;
    }
    threads[current_thread]->sleeptimer = num_quantums;
    sleepingVector->push_back(current_thread);
    __thread_popper(true);
    return 0;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
 */
int uthread_get_tid()
{
    return current_thread;
}

/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
 */
int uthread_get_total_quantums()
{
    return realtime;
}

/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
 */
int uthread_get_quantums(int tid)
{
    block_sig(SIGVTALRM);
    if (threads[tid] == nullptr)
    {
        unblock_sig(SIGVTALRM);
        return -1;
    }
    unblock_sig(SIGVTALRM);
    return threads[tid]->virtualtime;
}
