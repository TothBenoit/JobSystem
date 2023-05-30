#include "JobSystem.h"

#include <thread>   
#include <condition_variable> 
#include <emmintrin.h>
#include <map>

namespace Job
{
    class SpinLock
    {
    public:
        void lock()
        {
            while (true)
            {
                while (m_lock)
                {
                    _mm_pause();
                }

                if (!m_lock.exchange(true))
                    break;
            }
        }

        void unlock()
        {
            m_lock.store(false);
        }

    private:
        std::atomic<bool> m_lock{ false };
    };

    // Implementation from WickedEngine blog
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
        Counter* m_pCounter{ nullptr };
    };

    uint32_t numThreads = 0;
    ThreadSafeRingBuffer<JobInstance, 256> jobPool;

    SpinLock waitingJobsLock;
    std::map < Counter*, std::vector<std::pair<JobInstance, uint32_t>>> waitingJobs;

    std::atomic<uint64_t> currentLabel = 0;
    std::atomic<uint64_t> finishedLabel;
   
    inline void poll()
    {
        std::this_thread::yield();
    }

    void UpdateWaitingJob(Counter * pCounter)
    {
        waitingJobsLock.lock();
        auto it = waitingJobs.find(pCounter);
        if (it != waitingJobs.end())
        {
            std::vector < std::pair<JobInstance, uint32_t>>& jobs{ it->second };
            auto itJobs = jobs.begin();
            for (; itJobs != jobs.end(); )
            {
                auto& [jobInstance, fenceValue] = *itJobs;
                if (fenceValue <= pCounter->load())
                {
                    while (!jobPool.push_back(jobInstance)) {}
                    if (itJobs + 1 == jobs.end())
                    {
                        jobs.pop_back();
                        break;
                    }

                    *itJobs = jobs.back();
                    jobs.pop_back();
                    continue;
                }
                ++itJobs;
            }
            if (it->second.empty())
            {
                waitingJobs.erase(it);
            }
        }
        waitingJobsLock.unlock();
    }

    void Initialize()
    {
        finishedLabel.store(0);

        unsigned int numCores = std::thread::hardware_concurrency();

        numThreads = (numCores == 0u) ? 1u : numCores;

        for (uint32_t threadID = 0; threadID < numThreads; ++threadID)
        {
            std::thread worker([] {

                JobInstance job;

                while (true)
                {
                    if (jobPool.pop_front(job))
                    {
                        (job.m_executable)();
                        (*job.m_pCounter).fetch_add(1);
                        
                        UpdateWaitingJob(job.m_pCounter);

                        finishedLabel.fetch_add(1);
                    }
                    else
                    {
                        poll();
                    }
                }

                });

            worker.detach();
        }
    }

    bool IsBusy()
    {
        return finishedLabel.load() < currentLabel.load();
    }

    void Wait()
    {
        while (IsBusy()) { poll(); }
    }

    JobBuilder::JobBuilder()
    {}

    void JobBuilder::DispatchExplicitFence()
    {
        m_fence.store(m_totalJob);
    }

    void JobBuilder::DispatchJobInternal(const std::function<void()>& job)
    {
        currentLabel.fetch_add(1);

        if (m_fence > m_jobFinished)
        {
            waitingJobsLock.lock();
            waitingJobs[&m_jobFinished].emplace_back(JobInstance{ job, &m_jobFinished }, m_fence.load());
            waitingJobsLock.unlock();
        }
        else
        {
            while (!jobPool.push_back(JobInstance{ job, &m_jobFinished })) { poll(); }
        }
    }
}