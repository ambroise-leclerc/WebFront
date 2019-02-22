#include "thread_pool.h"
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <iostream>

class ThreadPool::Impl
{
public:
    Impl(size_t number_of_threads) : running_(true)
    {
        for (size_t thread =0; thread < number_of_threads; ++thread)
            workers_.push_back(std::thread(&Impl::Worker, this));
    }

    ~Impl()
    {
        std::cout << "ThreadPool destructor" << std::endl;
        running_ = false;
        tasks_condition_.notify_all();

        // Wait for threads termination
        for (auto &thread : workers_)
        {
            thread.join();
        }
    }

    template <typename Function> void EnqueueTask(Function function)
    {
        {
            std::unique_lock<std::mutex> lock(tasks_mutex_);
            tasks_.push_back(std::function<void()>(function));
        }
        tasks_condition_.notify_one();
    }

private:
    bool running_;
    std::vector<std::thread> workers_;          //< workers threads
    std::deque<std::function<void()>> tasks_;   //< task queue
    std::mutex tasks_mutex_;
    std::condition_variable tasks_condition_;

private:
    void Worker()
    {
        std::function<void()> task;
        while (running_)
        {
            {
                // acquire a lock for the condition variable
                std::unique_lock<std::mutex> lock(tasks_mutex_);

                // wait for a new task (in a loop in case of spurious wakeup)
                while (tasks_.empty() && running_)
                    tasks_condition_.wait(lock);

                if (running_)
                {
                    task = tasks_.front();
                    tasks_.pop_front();
                }
            }
            if (running_)
            {
                task();
            }
        }
    }
};

ThreadPool::ThreadPool(size_t number_of_threads) : pimpl_(new Impl(number_of_threads)) {}
ThreadPool::ThreadPool() : pimpl_(new Impl(std::thread::hardware_concurrency())) {}
ThreadPool::~ThreadPool() {}
template <typename Function> void ThreadPool::EnqueueTask(Function function) { pimpl_->EnqueueTask(function); }

template void ThreadPool::EnqueueTask(std::function<void()>);
