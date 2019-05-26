#ifndef LEVELDB_CONCURRENT_THREADPOOL_H
#define LEVELDB_CONCURRENT_THREADPOOL_H

#include <atomic>
#include <thread>
#include <mutex>
#include <array>
#include <vector>
#include <list>
#include <functional>
#include <future>
#include <condition_variable>
#include "util/debug.h"

namespace leveldb {

/**
 *  Simple ThreadPool that creates `ThreadCount` threads upon its creation,
 *  and pulls from a queue to get new jobs. The default is 10 threads.
 *
 *  This class requires a number of c++11 features be present in your compiler.
 */
class ThreadPool {
    
    std::vector<std::thread> threads;
    std::list<std::function<void()>> queue;

    std::atomic_int         jobs_left;
    std::atomic_bool        bailout;
    std::atomic_bool        finished;
    std::condition_variable job_available_var;
    std::condition_variable wait_var;
    std::mutex              wait_mutex;
    std::mutex              queue_mutex;
    int                     threadcount;

    /**
     *  Take the next job in the queue and run it.
     *  Notify the main thread that a job has completed.
     */
    void Task() {//线程的执行体
        while( !bailout ) {
            DEBUG_T("to handle job\n");
            next_job()();//next_job()函数就是获取想要运行的函数对象，然后执行()表示开始运行
            DEBUG_T("after handle job\n");
            --jobs_left;//剩下的还未完成的任务数量 
            wait_var.notify_one();//唤醒正在等待任务完成的线程，表示有一个任务已经完成了
        }
    }

    /**
     *  Get the next job; pop the first item in the queue, 
     *  otherwise wait for a signal from the main thread.
     */
    std::function<void()> next_job() {
        std::function<void()> res;//表示获取的下一个函数体
        std::unique_lock<std::mutex> job_lock( queue_mutex );//unique_lock能够自动解锁 

        // Wait for a job if we don't have any.
        // 如果后面的条件为false,那就处于阻塞状态，否则不进入阻塞状态 
        job_available_var.wait( job_lock, [this]() ->bool { return queue.size() || bailout; } );
        
        // Get job from the queue
        if( !bailout ) {
            DEBUG_T("have get job from queue\n");
            res = queue.front();//获取第一个job
            queue.pop_front();//并将其从线程池中pop出去
        }
        else { // If we're bailing out, 'inject' a job into the queue to keep jobs_left accurate.
            res = []{};
            ++jobs_left;
        }
        return res;
    }

public:
    ThreadPool(int threadcount)
        : jobs_left( 0 )
        , bailout( false )
        , finished( false ) 
        , threadcount(threadcount)
    {//线程池的构造函数
        threads.resize(threadcount);
        for( unsigned i = 0; i < threadcount; ++i )//线程的数目
            threads[ i ] = std::thread( [this]{ this->Task(); } );//创建线程，执行Task函数
    }

    /**
     *  JoinAll on deconstruction
     */
    ~ThreadPool() {
        JoinAll();
    }

    /**
     *  Get the number of threads in this pool
     */
    inline unsigned Size() const {//线程的数目
        return threadcount;
    }

    /**
     *  Get the number of jobs left in the queue.
     */
    inline unsigned JobsRemaining() {//剩余的任务数量
        std::lock_guard<std::mutex> guard( queue_mutex );//上锁
        return queue.size();//返回队列中任务的数目
    }

    /**
     *  Add a new job to the pool. If there are no jobs in the queue,
     *  a thread is woken up to take the job. If all threads are busy,
     *  the job is added to the end of the queue.
     */
    template<class F, class... Args>
    auto AddJob(F&& f, Args&&... args)->std::future<decltype(f(args...))> {
        std::lock_guard<std::mutex> guard( queue_mutex );
        using RetType = decltype(f(args...));
        auto job = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<RetType> future = job->get_future();
        {
            queue.emplace_back([job]{
                (*job)();
                });//将任务加入到队列中 
        }
        ++jobs_left;//剩下的还未完成的任务数量
        job_available_var.notify_one();//条件变量唤醒
        return future;
    }

    /**
     *  Join with all threads. Block until all threads have completed.
     *  Params: WaitForAll: If true, will wait for the queue to empty 
     *          before joining with threads. If false, will complete
     *          current jobs, then inform the threads to exit.
     *  The queue will be empty after this call, and the threads will
     *  be done. After invoking `ThreadPool::JoinAll`, the pool can no
     *  longer be used. If you need the pool to exist past completion
     *  of jobs, look to use `ThreadPool::WaitAll`.
     */
    void JoinAll( bool WaitForAll = true ) {
        //阻塞，直到所有的任务都完成,完成之后会销毁线程池，如果不想销毁，
        //可以调用函数waitall,只是单纯地等待任务完成
        if( !finished ) {
            if( WaitForAll ) {
                WaitAll();
            }

            // note that we're done, and wake up any thread that's
            // waiting for a new job
            bailout = true;//表示任务已经完成了 
            job_available_var.notify_all();//表示 

            for( auto &x : threads )//遍历线程  
                if( x.joinable() )
                    x.join();
            finished = true;//表示结束了
        }
    }

    /**
     *  Wait for the pool to empty before continuing. 
     *  This does not call `std::thread::join`, it only waits until
     *  all jobs have finshed executing.
     */
    void WaitAll() {
        if( jobs_left > 0 ) {
            std::unique_lock<std::mutex> lk( wait_mutex );//加锁
            //直到所有的任务都完成 
            wait_var.wait( lk, [this]{ return this->jobs_left == 0; } );
            //条件变量才能调用wait函数，表示
            lk.unlock();
        }
    }
};

} // namespace leveldb 

#endif //LEVELDB_CONCURRENT_THREADPOOL_H
