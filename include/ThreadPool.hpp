#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>
#include <memory>

class ThreadPool {
public:
    // 构造函数，传入线程数量
    ThreadPool(size_t threads);
    
    // 析构函数，等待所有任务完成并销毁线程
    ~ThreadPool();

    // 提交任务到任务队列
    // F: 函数类型, Args: 函数参数类型
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;

private:
    // 存储工作线程的容器
    std::vector<std::thread> workers;
    // 任务队列
    std::queue<std::function<void()>> tasks;

    // 同步机制
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// 构造函数实现
inline ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for(size_t i = 0; i < threads; ++i) {
        // emplace_back 直接在容器尾部构造对象，比 push_back 更高效
        workers.emplace_back([this] {
            // 每个工作线程的执行逻辑
            for(;;) {
                std::function<void()> task;
                {
                    // 使用 unique_lock 来管理互斥锁
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    // 等待条件：stop为true 或 任务队列不为空
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    
                    // 如果线程池停止且任务队列为空，则线程退出
                    if(this->stop && this->tasks.empty())
                        return;
                    
                    // 从队列中取出一个任务
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                } // lock 在这里自动释放
                
                // 执行任务
                task();
            }
        });
    }
}

// 提交任务的实现
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> {
    
    using return_type = typename std::result_of<F(Args...)>::type;

    // 使用 packaged_task 包装任务，以便获取 future
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
        
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // 不允许在线程池停止后继续添加任务
        if(stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        // 将任务放入队列
        tasks.emplace([task](){ (*task)(); });
    }
    // 通知一个等待的线程
    condition.notify_one();
    return res;
}

// 析构函数实现
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    // 通知所有线程，唤醒它们以检查 stop 标志
    condition.notify_all();
    // 等待所有工作线程执行完毕
    for(std::thread &worker: workers)
        worker.join();
}

#endif