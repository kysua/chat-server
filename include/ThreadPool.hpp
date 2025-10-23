#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool {
public:
    // 构造函数：创建固定数量的线程和指定容量的任务队列
    ThreadPool(size_t threadCount, size_t queueCapacity);
    
    // 析构函数：优雅地停止线程池
    ~ThreadPool();

    // 提交任务到任务队列
    // F: 可调用对象类型 (如 lambda)
    // 如果成功将任务放入队列，返回 true；如果队列已满或线程池已停止，返回 false。
    template<class F>
    bool enqueue(F&& task);

private:
    // 存储工作线程的容器
    std::vector<std::thread> workers_;
    // 任务队列
    std::queue<std::function<void()>> tasks_;

    // 任务队列的最大容量
    const size_t queueCapacity_;

    // 同步机制
    std::mutex queueMutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
};

// 构造函数实现
inline ThreadPool::ThreadPool(size_t threadCount, size_t queueCapacity)
    : queueCapacity_(queueCapacity), stop_(false) {
    // 启动指定数量的工作线程
    for (size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] {
            // 每个工作线程的执行逻辑
            for (;;) {
                std::function<void()> task;
                {
                    // 使用 unique_lock 管理互斥锁，以等待任务
                    std::unique_lock<std::mutex> lock(this->queueMutex_);
                    
                    // 等待条件：线程池被停止 或 任务队列不为空
                    this->condition_.wait(lock, [this] { 
                        return this->stop_.load() || !this->tasks_.empty(); 
                    });
                    
                    // 如果线程池停止了，并且任务队列也空了，那么工作线程就可以安全退出了
                    if (this->stop_.load() && this->tasks_.empty()) {
                        return;
                    }
                    
                    // 从队列中取出一个任务来执行
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                } // 锁在此处自动释放
                
                // 执行任务
                task();
            }
        });
    }
}

// 提交任务的实现
template<class F>
bool ThreadPool::enqueue(F&& task) {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);

        // 如果线程池已经停止，或者任务队列已达到容量上限，则拒绝新任务
        if (stop_.load() || tasks_.size() >= queueCapacity_) {
            return false;
        }

        // 将任务放入队列
        // 使用 std::forward 完美转发任务
        tasks_.emplace(std::forward<F>(task));
    }
    
    // 任务成功入队后，通知一个正在等待的工作线程
    condition_.notify_one();
    return true;
}

// 析构函数实现
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        // 设置停止标志
        stop_.store(true);
    }
    
    // 通知所有正在等待的线程，唤醒它们以检查停止标志并退出
    condition_.notify_all();
    
    // 等待所有工作线程执行完剩余任务并安全退出
    for (std::thread &worker : workers_) {
        worker.join();
    }
}

#endif // THREADPOOL_H
