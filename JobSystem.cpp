#include "JobSystem.h"

#include <thread>   
#include <condition_variable> 
#include <array>
#include <map>

namespace Job
{
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
        std::mutex lock;
    };

    struct JobInstance
    {
        std::function<void()> m_executable;
        Counter* m_pCounter{ nullptr };
    };

    uint32_t numThreads = 0;
    ThreadSafeRingBuffer<JobInstance, 256> jobPool;

    std::mutex waitingJobsMutex;
    std::map < Counter*, std::vector<std::pair<JobInstance, uint32_t>>> waitingJobs;

    std::condition_variable wakeCondition;
    std::mutex wakeMutex;
    std::atomic<uint64_t> currentLabel = 0;
    std::atomic<uint64_t> finishedLabel;
   
    inline void poll()
    {
        wakeCondition.notify_one();
        std::this_thread::yield();
    }

    void UpdateWaitingJob(Counter * pCounter)
    {
        waitingJobsMutex.lock();
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
                    while (!jobPool.push_back(jobInstance)) { poll(); }
                    if (itJobs + 1 == jobs.end())
                    {
                        jobs.pop_back();
                        break;
                    }

                    *itJobs = jobs.back();
                    jobs.pop_back();
                    wakeCondition.notify_one();
                    continue;
                }
                ++itJobs;
            }
            if (it->second.empty())
            {
                waitingJobs.erase(it);
            }
        }
        waitingJobsMutex.unlock();
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
                        std::unique_lock<std::mutex> lock(wakeMutex);
                        wakeCondition.wait(lock);
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
            waitingJobsMutex.lock();
            waitingJobs[&m_jobFinished].emplace_back(JobInstance{ job, &m_jobFinished }, m_fence.load());
            waitingJobsMutex.unlock();
        }
        else
        {
            while (!jobPool.push_back(JobInstance{ job, &m_jobFinished })) { poll(); }
        }

        wakeCondition.notify_one();
    }
}