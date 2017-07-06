#ifndef QUICK_SORT_BASE_STACK_LIBRARY_H
#define QUICK_SORT_BASE_STACK_LIBRARY_H

#include <stack>
#include <mutex>
#include <memory>
#include <exception>
#include <future>   //  std::async  std::package_task
#include <functional> //  std::distance
#include <numeric>  //  std::accumulate
#include <thread>   // std::thread::hardware_concurremcy
#include <vector>
#include <list>
#include <atomic>
#include <algorithm>
#include <chrono>
/*******************************************************************************
 *    线程安全栈
 ******************************************************************************/
class empty_stack:std::exception
{
    const char* what() const throw();
};

template <typename T>
class thread_safe_stack
{
public:
    thread_safe_stack() {}
    thread_safe_stack(const thread_safe_stack& other)
    {
        std::lock_guard<std::mutex> lock(other.mtx);
        data = other.data;
    }
    thread_safe_stack& operator=(const thread_safe_stack& other) = delete;
    void push(T value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        data.push(std::move(value));
    }
    void pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(data.empty())
            throw empty_stack();
        value = std::move(data.top());
        data.pop();
    }
    std::shared_ptr<T> pop()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(data.empty())
            throw empty_stack();
        std::shared_ptr<T> const ret(std::make_shared(std::move(data.top())));
        data.pop();
        return ret;
    }
    bool empty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return data.empty();
    }
private:
    std::stack<T> data;
    mutable std::mutex  mtx;
};


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
/*******************************************************************************
 *   基于栈的并行快速排序
 ******************************************************************************/
template <typename T>
class parallel_quick_sort
 {
private:
    struct chunk_to_sort
    {
        std::list<T> data;
        std::promise<std::list<T> > promise;
    };
public:
    parallel_quick_sort():max_threads(std::thread::hardware_concurrency()),end_of_data(false),guard(threads) {};
    ~parallel_quick_sort() {end_of_data = true;}
    std::list<T> do_sort(std::list<T>& chunk_data)
    {
        if(chunk_data.empty()){
            return chunk_data;
        }
        std::list<T> result;
        result.splice(result.begin(),chunk_data,chunk_data.begin());
        T const& partition_val = *result.begin();
        typename std::list<T>::iterator divid = std::partition(chunk_data.begin(),chunk_data.end(),[&](T const& value){return value < partition_val;});
        chunk_to_sort new_low_chunk;
        new_low_chunk.data.splice(new_low_chunk.data.begin(),chunk_data,chunk_data.begin(),divid);
        std::future<std::list<T> > new_low = new_low_chunk.promise.get_future();
        chunks.push(std::move(new_low_chunk));
        if(threads.size() < max_threads)
        {
            threads.push_back(std::thread(std::bind(&parallel_quick_sort<T>::sort_thread),this));
        }
        std::list<T> new_high_chunk = do_sort(chunk_data);
        result.splice(result.end(),new_high_chunk);
        while(new_low.wait_for(std::chrono::microseconds(0)) != std::future_status::ready)
        {
            try_sort();
        }
        result.splice(result.begin(),new_low.get());
        return result;
    }

    void sort_thread()
    {
        while(!end_of_data)
        {
            try_sort();
            std::this_thread::yield();
        }
    };
    void try_sort()
    {
        std::shared_ptr<chunk_to_sort> chunk = chunks.pop();
        if(chunk)
        {
            sort_chunk(chunk);
        }
    }
    void sort_chunk(std::shared_ptr<chunk_to_sort> const& chunk)
    {
        chunk->promise.set_value(do_sort(chunk->data));
    }
private:
    unsigned long max_threads;
    thread_safe_stack<chunk_to_sort> chunks;
    std::atomic<bool> end_of_data;
    std::vector<std::thread> threads;
    thread_guard guard;
 };

/*******************************************************************************
 *    并行累加
 ******************************************************************************/
template <typename Iterator,typename T>
T parallel_accumulate_async(Iterator first,Iterator last,T init)
{
    unsigned long length = std::distance(first,last);
    unsigned long max_block_size = 25;
    if(length <= max_block_size)
    {
        return std::accumulate(first,last,init);
    }
    Iterator mid_point = first;
    std::advance(mid_point,length / 2);
    std::future<T> first_half_result = std::async(parallel_accumulate_async<Iterator,T>,first,mid_point,init);
    T second_half_result = parallel_accumulate_async(mid_point,last,T());
    return first_half_result.get() + second_half_result;
};



template <typename Iterator,typename T>
T accumulate_block(Iterator first,Iterator last)
{
        return std::accumulate(first,last,T());
};

template <typename Iterator,typename T>
T parallel_accumulate_package(Iterator first,Iterator last,T init)
{
    unsigned long length = std::distance(first,last);
    if(!length) {
        return init;
    }
    unsigned long max_chunk_size = 25;
    unsigned long max_thread = (length + max_chunk_size - 1) / max_chunk_size;
    unsigned long hardware_threads = std::thread::hardware_concurrency();
    unsigned long num_thread = std::min(hardware_threads == 0 ? 2 : hardware_threads,max_thread);
    unsigned long block_size = length / num_thread;
    std::vector<std::thread> threads(num_thread - 1);
    std::vector<std::future<T> > futures(num_thread - 1);
    thread_guard   thread_guards(threads);
    Iterator begin = first;
    for(int i = 0;i < num_thread - 1;++i)
    {
        Iterator end = begin;
        std::advance(end,block_size);
        std::packaged_task<T(Iterator,Iterator)> task(accumulate_block<Iterator,T>);
        futures[i] = task.get_future();
        threads[i] = std::thread(std::move(task),begin,end);
        begin = end;
    }
    T last_result = accumulate_block(begin,last);
    T result = init;
    for(typename std::vector<std::future<T> >::iterator it = futures.begin();it != futures.end();++it)
    {
        result += (*it).get();
    }
    result += last_result;
    return result;
};

/*******************************************************************************
 *    并行查找  std::promise
 ******************************************************************************/

template <typename Iterator,typename Mathtype>
Iterator parallel_find(Iterator first,Iterator last,Mathtype obj)
{
    struct find_element
    {
        void operator()(Iterator first,Iterator last,Mathtype math,std::promise<Iterator>& result,std::atomic<bool>& flag)
        {
            try
            {
                for(;(first != last) && (!flag.load());++first)
                {
                    if(*first == math)
                    {
                        result.set_value(first);
                        flag.store(true);
                        return ;
                    }
                }
            }
            catch(...)
            {
                try
                {
                    result.set_exception(std::current_exception());
                    flag.store(true);
                }
                catch(...)
                {}
            }

        }
    };
    unsigned long const length = std::distance(first,last);
    if(!length)
        return last;
    unsigned long const min_per_thread = 25;
    unsigned long const max_thread = (length + min_per_thread -1) / min_per_thread;
    unsigned long const hardware_thread = std::thread::hardware_concurrency();
    unsigned long const num_thread = std::min(hardware_thread != 0 ? hardware_thread : 2,max_thread);
    unsigned long const block_size = length / num_thread;
    std::promise<Iterator> result;
    std::atomic<bool> flag(false);
    std::vector<std::thread> threads(num_thread - 1);
    {
        thread_guard gurad(threads);
        Iterator start = first;
        for(unsigned long i = 0;i < num_thread - 1;++i)
        {
            Iterator end = start;
            std::advance(end,block_size);
            threads[i] = std::thread(find_element(),start,end,obj,result, flag);
            start = end;
        }
        find_element()(start,last,obj,result,flag);
    }
    if(!flag.load())
        return last;
    return result.get_future().get();
};


/*******************************************************************************
 *    并行查找  std::async
 ******************************************************************************/

template <typename Iterator,typename Mathtype>
Iterator parallel_find_impl(Iterator first,Iterator last,Mathtype math,std::atomic<bool>& flag)
{
    try
    {
        unsigned long length = std::distance(first,last);
        unsigned long max_per_thread = 25;
        if(length < (2 * max_per_thread))
        {
            for(;(first != last) && (!flag.load());++first)
            {
                if(*first == math)
                {
                    flag.store(true);
                    return first;
                }
            }
            return last;
        }
        else
        {
            Iterator mid = first + length / 2;
            std::future<Iterator> first_result = std::async(&parallel_find_impl<Iterator,Mathtype>,first,mid, std::ref(flag)); //flag引用传入,异步递归调用抛出异常,通过first_result.get()传播,
            Iterator second_result = parallel_find_impl(mid,last,math,flag); //直接递归调用抛出异常,first_result析构函数使异步线程提前结束
            return first_result.get() == mid ? second_result:first_result.get();  //first_result未找到,则second_result返回.还未找到,必定返回last
        }
    }
    catch(...)
    {  //仅捕获flag异常
        flag.store(true);
        throw;
    }
};

template <typename Iterator,typename Mathtype>
Iterator parallel_find_async(Iterator first,Iterator last,Mathtype math)
{
    std::atomic<bool> flag(true);
    return parallel_find_impl(first,last,math,flag);
};

/*******************************************************************************
 *    分块迭代累加  std::promise    std::future
 *    不可用 std::async ,线程间需要同步
 ******************************************************************************/
template <typename Iterator>
void parallel_block_partial_sun(Iterator first,Iterator last)
{
    typedef typename Iterator::value_type value_type;
    struct process_chunk
    {
        void oprerator()(Iterator first,Iterator last,std::promise<value_type>& block_end_value,std::future<value_type>& privious_end_value,
                            unsigned long block_size)
        {
            try {
                std::partial_sum(first,last,first);
                value_type added = privious_end_value.get();
                std::for_each(first,last,[added](value_type& item){item += added;});
                Iterator end_point = first + block_size - 1;
                block_end_value.set_value(*end_point);
            }
            catch(...)
            {
                block_end_value.set_exception(std::current_exception());
            }
        }
    };
    struct init_future
    {
        std::future<value_type > operator()()
        {
            std::promise<value_type> pro;
            pro.set_value(std::move(0));
            return pro.get_future();
        }
    };
    unsigned long const length = std::distance(first,last);
    if(!length)
        return last;
    unsigned long const min_per_thread = 25;
    unsigned long const max_thread = (length + min_per_thread -1) / min_per_thread;
    unsigned long const hardware_thread = std::thread::hardware_concurrency();
    unsigned long const num_thread = std::min(hardware_thread != 0 ? hardware_thread : 2,max_thread);
    unsigned long const block_size = length / num_thread;
    std::vector<std::thread > threads(num_thread - 1);
    std::vector<std::promise<value_type> > block_end_value(num_thread - 1);
    std::vector<std::future<value_type > > privious_end_value(num_thread - 1);
    thread_guard guard(threads);
    Iterator block_start = first;
    for(int i = 0;i < num_thread - 1;++i)
    {
        Iterator block_end = block_start;
        std::advance(block_end,block_size);
        threads[i] = std::thread(process_chunk(),block_start,block_end,block_end_value[i],(i == 0) ? init_future()() : privious_end_value[i - 1],block_size);
        block_start = block_end;
        privious_end_value[i] = block_end_value[i].get_future();
    }
    std::promise<value_type> last_promise;
    last_promise.set_value(0);
    auto func = std::function<void(Iterator,Iterator,std::promise<value_type>,std::future<value_type>,unsigned long)>;
    func(block_start,last,last_promise,(num_thread > 1) ? privious_end_value.back() : 0,block_size);
}

/*******************************************************************************
 *    栅栏同步机制，所有线程到达栅栏chu，方可继续操作，第一个到达的需要等待最后一个到达
 ******************************************************************************/
 class barrier
 {
 public:
     explicit barrier(const int count)
             :count_(count),space(count_.load()),generator(0)
     {}
     void wait()
     {
        const int old_generator = generator.load();
        if(!--space)
        {
            space = count_.load();
            ++generator;
        }
        else
        {
            while(generator.load() == old_generator)
                std::this_thread::yield();
        }
     }
     void done_waiting()
     {
         --count_;
         if(!--space)
         {
             space = count_.load();
             ++generator;
         }
     }
 private:
     std::atomic<int> count_;
     std::atomic<int> space;
     std::atomic<int> generator;
 };

/*******************************************************************************
 *    级数距离迭代累加
 ******************************************************************************/
template <typename Iterator>
void parallel_partial_sun(Iterator first,Iterator last)
{
    typedef typename Iterator::value_type value_type;
    struct process_element
    {
        static void process()(Iterator first,Iterator last,std::vector<value_type>& buff,int i,barrier& bar)
        {
            value_type& ith_element = *(first + i);
            bool update = false;
            for(int step = 0,stride = 1;stride <= i;stride *= 2,++step)
            {
                value_type const& source = (step % 2) ? buff[i] : ith_element;
                value_type& dest = (step % 2) ? ith_element : buff[i];
                value_type const& added = (step % 2) ? buff[i - stride] : *(first + i - stride);
                dest = source + added;
                update = !(step % 2);
                bar.wait();
            }
            if(update)
            {
                ith_element = buff[i];
            }
            bar.done_waiting();
        }
    };
    unsigned long const length = std::distance(first,last);
    if(!length)
        return;
    std::vector<value_type> buff(length);
    barrier bar(length);
    std::vector<std::thread> threads(length - 1);
    thread_guard guard(threads);
    for(int i = 0;i < length - 1;++i)
    {
        threads[i] = std::thread(std::mem_fn(&process_element::process),first,last,std::ref(buff),i,std::ref(bar));
    }
    process_element::process(first,last,std::ref(buff),length - 1,bar);
}



















































#endif