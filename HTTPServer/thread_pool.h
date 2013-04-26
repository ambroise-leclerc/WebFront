#ifndef __THREAD_POOL_H_
#define __THREAD_POOL_H_
#include <memory>

class ThreadPool
{
public:
    ThreadPool( size_t number_of_threads);
    ThreadPool();                                           //< Create a thread pool with the maximum number of concurrent threads supported by the hardware
    ~ThreadPool();
    template <typename Function> void EnqueueTask(Function function);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};


#endif //__THREAD_POOL_H_