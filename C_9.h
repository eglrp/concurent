//
// Created by lenovo on 2017/5/24.
//

#ifndef QUICK_SORT_BASE_STACK_C_9_H
#define QUICK_SORT_BASE_STACK_C_9_H

#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <atomic>
#include <future>
#include <type_traits>
#include <list>
#include <chrono>
#include <algorithm>
#include <queue>
#include <exception>
#include <iostream>

/*******************************************************************************
 *    线程安全队列
 ******************************************************************************/
template <typename T>
class thread_safe_queue
{
public:
    thread_safe_queue() {}
    void wait_and_pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mtx);
        data_condition.wait(lock,[this](){ return !data_queue.empty();});
        value = std::move(data_queue.front());
        data_queue.pop();
    }
    std::shared_ptr<T> wait_and_pop()
    {
        std::unique_lock<std::mutex> lock(mtx);
        data_condition.wait(lock,[this](){ return !data_queue.empty();});
        std::shared_ptr<T> res = std::make_shared<T>(data_queue.front());
        data_queue.pop();
        return res;
    }
    bool try_pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mtx);
        if(data_queue.empty())
            return false;
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }
    std::shared_ptr<T> try_pop()
    {
        std::unique_lock<std::mutex> lock(mtx);
        if(data_queue.empty())
            return std::shared_ptr<T>();
        std::shared_ptr<T> res = std::make_shared<T>(data_queue.front());
        data_queue.pop();
        return res;
    }
    void push(T& value)
    {
        std::unique_lock<std::mutex> lock(mtx);
        std::shared_ptr<T> data(std::make_shared<T>(value));
        data_queue.push(data);
        data_condition.notify_one();
    }
private:
    std::queue<std::shared_ptr<T> > data_queue;
    std::condition_variable data_condition;
    std::mutex mtx;
};


/*******************************************************************************
 *    基于锁的任务窃取队列
 ******************************************************************************/
class work_steal_queue
{
public:
    work_steal_queue() {}
    work_steal_queue(const work_steal_queue& other) = delete;
    work_steal_queue& operator=(const work_steal_queue& other) = delete;
    void push(data_type& obj)
    {
        std::lock_guard<std::mutex> lock(mtx);
        work_queue.push_front(obj);
    }
    bool empty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return work_queue.empty();
    }
    bool try_pop(data_type& res)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(work_queue.empty())
            return false;
        res = std::move(work_queue.front());
        work_queue.pop_front();
        return true;
    }
    bool try_steal(data_type& res)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(work_queue.empty())
            return false;
        res = std::move(work_queue.back());
        work_queue.pop_back();
        return true;
    }
private:
    typedef function_wrapper data_type;
    std::deque<data_type> work_queue;
    std::mutex mtx;
};


/*******************************************************************************
 *    线程池
 ******************************************************************************/

struct thread_guard
{
    explicit thread_guard(std::vector<std::thread>& threads):threads_(threads) {}
    ~thread_guard()
    {
        for(unsigned long i = 0;i < threads_.size();++i)
        {
            if(threads_[i].joinable()){
                threads_[i].join();
            }
        }
    }
private:
    std::vector<std::thread> threads_;
};

template <typename Func>
class function_wrapper
{
private:
    struct impl_base
    {
        virtual void call() = 0;
        ~impl_base() {};
    };

    struct impl_type:impl_base
    {
        impl_type(Func&& func_):func(std::move(func_)) {}
        void call() { func();}
    private:
        Func func;
    };

public:
    function_wrapper(Func&& func):impl(new impl_type(std::move(func))) {}
    void operator()() {(*impl).call();}
    function_wrapper() = default;
    function_wrapper(function_wrapper&& other):impl(std::move(other.impl)) {}
    function_wrapper& operator=(function_wrapper&& other)
    {
        impl = std::move(other.impl);
        return *this;
    }
    function_wrapper(function_wrapper&) = delete;
    function_wrapper(const function_wrapper&) = delete;
    function_wrapper& operator=(const function_wrapper&) = delete;

private:
    std::unique_ptr<impl_base> impl;
};

class thread_pool
{
public:
    thread_pool():thread_count(std::thread::hardware_concurrency()),done(false),guard(threads)
    {
        try
        {
            for(int i = 0;i < thread_count;++i)
            {
                queues.push_back(std::unique_ptr<work_steal_queue>(new work_steal_queue));
                threads.push_back(std::thread(&thread_pool::work_thread,this,i));
            }
        }
        catch (...)
        {
            done.store(true);
            throw;
        }
    }

    template <typename Func>
    std::future<typename std::result_of<Func()>::type>
    submit(Func func)
    {
        typedef typename std::result_of<Func()>::type  result_type;
        std::packaged_task<result_type()> task(std::move(func));
        if(local_work_queue)
        {
            (*local_work_queue).push(std::move(function_wrapper<std::packaged_task<result_type()> >(std::move(task))));
        }
        else
        {
            work_queue.push(std::move(function_wrapper<std::packaged_task<result_type()> >(std::move(task))));
        }
        return task.get_future();
    }

    bool pop_task_from_local_thread(function_wrapper& obj)
    {
        return local_work_queue && (*local_work_queue).try_pop(obj);
    }

    bool pop_task_from_work_queue(function_wrapper& obj)
    {
        return work_queue.try_pop(obj);
    }

    bool pop_task_from_other_thread(function_wrapper& obj)
    {
        for(int i = 0;i < queues.size();++i)
        {
            const int index_ = (index + 1 + i) % queues.size();
            if(queues[index_].get()->try_steal(obj))
                return true;
        }
        return false;
    }
    void run_pending_task()
    {
        function_wrapper task;
        if(pop_task_from_local_thread(task) || pop_task_from_work_queue(task) || pop_task_from_other_thread(task))
        {
            task();
        }
        else
        {
            std::this_thread::yield();
        }
    }

    ~thread_pool()
    {
        done.store(true);
    }

private:
    void work_thread(int index_)
    {
        index = index_;
        local_work_queue = queues[index].get();
        while(!done)
            run_pending_task();
    }
private:
    const int thread_count;
    std::atomic_bool done;
    std::vector<std::thread> threads;
    thread_safe_queue<function_wrapper > work_queue;    //全局任务池
    std::vector<std::unique_ptr<work_steal_queue> > queues;   //线程本地任务池
    thread_guard guard;
    static thread_local work_steal_queue* local_work_queue;  //本地任务队列，解决队列中的任务竞争,unique_ptr防止非线程池的线程拥有任务队列
    static thread_local int index;
};

/*******************************************************************************
 *    基于线程池的快速排序
 ******************************************************************************/
template <typename T>
struct quick_sort
{
    std::list<T> do_sort(std::list<T> chunk_data)
    {
        if(chunk_data.empty()){
            return chunk_data;
        }
        std::list<T> result;
        result.splice(result.begin(),chunk_data,chunk_data.begin());
        const T& pivot = *result.begin();
        typename std::list<T>::iterator it = std::partition(chunk_data.begin(),chunk_data.end(),[&](const T& item){ return item < pivot;});
        std::list<T> new_low_chunk;
        new_low_chunk.splice(new_low_chunk.begin(),chunk_data,chunk_data.begin(),it);
        std::future<std::list<T> > new_low = pool.submit(std::bind(&quick_sort::do_sort,this,std::move(new_low_chunk)));
        std::list<T> new_high = do_sort(chunk_data);
        result.splice(result.end(),new_high,new_high.begin(),new_high.end());
        while(new_low.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            pool.run_pending_task();
        result.splice(result.begin(),new_low.get());
        return result;
    }
private:
    thread_pool pool;
};


/*******************************************************************************
 *    中断线程
 ******************************************************************************/
class interrupt_flag
 {
 public:

     void set();
     bool is_set() const;
 };

class thread_interrupted:std::exception
{
public:
    const char* what() const
    {
        std::cout << "interrupt point" << std::endl;
    }
};

thread_local interrupt_flag this_interrupt_flag;

template <typename Func>
class interruptible_thread
{
public:
    interruptible_thread(Func&& func)
    {
        std::promise<interrupt_flag*> p;
        internal_thread = std::thread([func,&p](){p.set_value(&this_interrupt_flag); func();});
        flag = p.get_future().get();
    }
    void join();
    bool joinable();
    void detach();
    void interrupt_point()
    {
        if(this_interrupt_flag.is_set())
            throw thread_interrupted();
    }
    void interrupt_wait(std::condition_variable& cv,std::unique_lock<std::mutex>& lock)
    {

    }
    void interrupt()
    {
        if(flag)
            flag->set();
    }

private:
    std::thread internal_thread;
    interrupt_flag* flag;
};























































#endif //QUICK_SORT_BASE_STACK_C_9_H
