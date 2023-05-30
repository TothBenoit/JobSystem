#include "JobSystem.h"

#include <thread>   
#include <emmintrin.h>
#include <unordered_map>

namespace Job
{
    class CounterInstance;

    void WorkerMainLoop();
    void UpdateWaitingJobs(const CounterInstance* counterInstance);

    // Implementation inspired by WickedEngine blog
    template <typename T, size_t capacity>
    class ThreadSafeRingBuffer
    {
    public:

        inline bool push_back(const T& item)
        {
            bool result = false;
            lock.lock();
            size_t next = (head + 1) % capacity;
            if (next != tail)
            {
                data[head] = item;
                head = next;
                result = true;
            }
            lock.unlock();
            return result;
        }

        inline bool pop_front(T& item)
        {
            bool result = false;
            lock.lock();
            if (tail != head)
            {
                item = data[tail];
                tail = (tail + 1) % capacity;
                result = true;
            }
            lock.unlock();
            return result;
        }

    private:
        T data[capacity];
        size_t head = 0;
        size_t tail = 0;
        SpinLock lock;
    };

    struct JobInstance
    {
        std::function<void()> m_executable;
        Counter m_counter;
    };

    class CounterInstance
    {
    private:
        friend void UpdateWaitingJobs(const CounterInstance* counterInstance);
        friend void WorkerMainLoop();
        friend Counter;

        uint64_t GetValue() const { return m_counter.load(); }

        void Decrement()
        {
            m_counter.fetch_sub(1);
            UpdateWaitingJobs(this);

            for (auto& listeningCounter : m_listeningCounters)
            {
                listeningCounter->Decrement();
            }
        }

        void Addlistener(const Counter& counter) const
        {
            m_listenerLock.lock();
            m_listeningCounters.push_back(counter.m_pCounterInstance);
            m_listenerLock.unlock();
        }

    private:
        std::atomic<uint64_t> m_counter{ 0 };
        mutable std::vector<std::shared_ptr<CounterInstance>> m_listeningCounters;
        mutable SpinLock m_listenerLock;
    };

    uint32_t g_workerCount{ 0 };
    std::vector<std::thread> g_workers;
    ThreadSafeRingBuffer<JobInstance, 256> g_jobPool;

    SpinLock g_waitingJobsLock;
    std::unordered_map <const CounterInstance*, std::vector<JobInstance>> g_waitingJobs;

    std::atomic<uint64_t>   g_currentLabel{ 0 };
    std::atomic<uint64_t>   g_finishedLabel{ 0 };
    bool                    g_workerRunning{ false };

    void UpdateWaitingJobs(const CounterInstance* counterInstance)
    {
        if (counterInstance->GetValue() != 0)
            return;

        g_waitingJobsLock.lock();
        auto it = g_waitingJobs.find(counterInstance);
        if (it != g_waitingJobs.end())
        {
            for (JobInstance& job : it->second)
            {
                while (!g_jobPool.push_back(std::move(job))) {}
            }
            g_waitingJobs.erase(it);
        }
        g_waitingJobsLock.unlock();
    }

    void WorkerMainLoop()
    {
        JobInstance job;

        while (g_workerRunning)
        {
            if (g_jobPool.pop_front(job))
            {
                (job.m_executable)();

                Counter& counter = job.m_counter;
                counter--;
                UpdateWaitingJobs(counter.GetPtrValue());

                for (auto& listeningCounter : counter.GetPtrValue()->m_listeningCounters)
                {
                    UpdateWaitingJobs(listeningCounter.get());
                }

                g_finishedLabel.fetch_add(1);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }

    void Initialize()
    {
        g_workerRunning = true;

        unsigned int numCores{ std::thread::hardware_concurrency() };

        g_workerCount = (numCores == 0u) ? 1u : numCores;
        g_workers.reserve(g_workerCount);

        for (uint32_t threadID = 0; threadID < g_workerCount; ++threadID)
        {
            g_workers.emplace_back(&WorkerMainLoop);
        }
    }

    void Shutdown()
    {
        g_workerRunning = false;

        for (std::thread& worker : g_workers)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    void Wait()
    {
        while (g_finishedLabel.load() < g_currentLabel.load()) { std::this_thread::yield(); }
    }

    void WaitForCounter(const Counter& counter)
    {
        while (counter.GetValue() != 0) { std::this_thread::yield(); }
    }

    void SpinLock::lock()
    {
        while (g_workerRunning)
        {
            while (m_lock)
            {
                _mm_pause();
            }

            if (!m_lock.exchange(true))
                break;
        }
    }

    void SpinLock::unlock()
    {
        m_lock.store(false);
    }

    Counter::Counter()
    {
        m_pCounterInstance = std::shared_ptr<CounterInstance>{ new CounterInstance() };
    }

    Counter& Counter::operator++()
    {
        m_pCounterInstance->m_counter.fetch_add(1);
        return *this;
    }

    Counter& Counter::operator++(int)
    {
        return ++(*this);
    }

    Counter& Counter::operator--()
    {
        m_pCounterInstance->Decrement();
        return *this;
    }

    Counter& Counter::operator--(int)
    {
        return --(*this);
    }

    Counter& Counter::operator+=(const Counter& other)
    {
        other.m_pCounterInstance->Addlistener(*this);
        m_pCounterInstance->m_counter.fetch_add(other.GetValue());
        return *this;
    }

    uint64_t Counter::GetValue() const 
    { 
        return m_pCounterInstance->GetValue(); 
    }

    CounterInstance* Counter::GetPtrValue() const 
    { 
        return m_pCounterInstance.get(); 
    }

    JobBuilder::JobBuilder()
    {
        m_counter = Counter();
    }

    void JobBuilder::DispatchExplicitFence()
    {
        if (m_counter.GetValue() > 0)
        {
            m_fence = std::move(m_counter);
            m_counter = Counter();
        }
    }

    void JobBuilder::DispatchWait(const Counter& counter)
    {
        m_fence += counter;
    }

    const Counter& JobBuilder::ExtractWaitCounter()
    {
        return (m_counter.GetValue() == 0) ? m_fence : m_counter;
    }

    void JobBuilder::DispatchJobInternal(const std::function<void()>& job)
    {
        m_counter++;
        g_currentLabel.fetch_add(1);

        if ((m_fence.GetValue()) > 0)
        {
            g_waitingJobsLock.lock();
            g_waitingJobs[m_fence.GetPtrValue()].emplace_back(JobInstance{job, m_counter});
            g_waitingJobsLock.unlock();
        }
        else
        {
            while (!g_jobPool.push_back(JobInstance{ job, m_counter})) { std::this_thread::yield(); }
        }
    }
}