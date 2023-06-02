#include "JobSystem.h"

#include <windows.h>
#include <thread>   
#include <emmintrin.h>
#include <unordered_map>
#include <assert.h>

namespace Job
{
    class CounterInstance;

    void WorkerMainLoop();
    void UpdateWaitingJobs(const CounterInstance* counterInstance);
    void WaitForCounter_Fiber(const Counter& counter);
    void InitThread(uint32_t threadID);

    // Implementation inspired by WickedEngine blog
    template <typename T, size_t capacity>
    class ThreadSafeRingBuffer
    {
    public:
        inline bool push_back(const T& item)
        {
            bool result = false;
            lock.lock();
            size_t next = (head + 1) % (capacity+1);
            if(next != tail)
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
                tail = (tail + 1) % (capacity+1);
                result = true;
            }
            lock.unlock();
            return result;
        }

    private:
        T data[capacity+1];
        size_t head = 0;
        size_t tail = 0;
        SpinLock lock;
    };

    struct JobInstance
    {
        std::function<void()> m_executable;
        Counter m_counter;
        Counter m_fence;
    };

    class CounterInstance
    {
    private:
        friend void UpdateWaitingJobs(const CounterInstance* counterInstance);
        friend void WorkerMainLoop(void * pData);
        friend Counter;

        uint64_t GetValue() const { return m_counter.load(); }

        void Decrement()
        {
            m_counter.fetch_sub(1);
            UpdateWaitingJobs(this);

            for (auto& waitingCounter : m_waitingCounters)
            {
                waitingCounter->Decrement();
            }
        }

        void Addlistener(Counter& counter) const
        {
            m_waitingCountersLock.lock();
            m_waitingCounters.push_back(counter.GetPtrValue());
            m_waitingCountersLock.unlock();
        }

    private:
        std::atomic<uint64_t> m_counter{ 0 };
        mutable std::vector<CounterInstance*> m_waitingCounters;
        mutable SpinLock m_waitingCountersLock;
    };

	struct FiberDecl
	{
		void* pFiber;
		uint32_t threadIndex;
	};

    const uint32_t g_fiberPerThread{ 32 };
    struct Worker
    {
        Worker(void (*pFunc)(uint32_t))
            : pEntryPoint{ pFunc }
        {}
        
        void Run(uint32_t threadID)
        {
            thread = std::thread(pEntryPoint, threadID);
        }

        void (*pEntryPoint)(uint32_t);
        std::thread thread;
        ThreadSafeRingBuffer<FiberDecl, g_fiberPerThread> freeFibers;
        ThreadSafeRingBuffer<FiberDecl, g_fiberPerThread> sleepingFibers;
    };
    
    uint32_t g_workerCount{ 0 };
    std::vector<Worker*> g_pWorkers;
    ThreadSafeRingBuffer<JobInstance, 256> g_jobPool;

    SpinLock g_waitingFibersLock;
    std::unordered_map <const CounterInstance*, std::vector<FiberDecl>> g_waitingFibers;

#pragma optimize( "", off )
    thread_local uint32_t g_threadIndex;
	thread_local void* g_pMainFiber{ nullptr };
	thread_local FiberDecl g_pCurrentFiber{ nullptr, 0 };
#pragma optimize( "", on )

    std::atomic<uint64_t>   g_currentLabel{ 0 };
    std::atomic<uint64_t>   g_finishedLabel{ 0 };
    bool                    g_workerRunning{ false };

	void Switch_Fiber()
	{
		bool result = g_pWorkers[g_threadIndex]->freeFibers.push_back(g_pCurrentFiber);

		assert(result);
		if (!g_pWorkers[g_threadIndex]->sleepingFibers.pop_front(g_pCurrentFiber))
		{
			result = g_pWorkers[g_threadIndex]->freeFibers.pop_front(g_pCurrentFiber);
			assert(result);
		}
		assert(g_pCurrentFiber.pFiber);

		::SwitchToFiber(g_pCurrentFiber.pFiber);
	}

	void Switch()
	{
		if (g_pCurrentFiber.pFiber)
		{
			Switch_Fiber();
		}
		else
		{
			std::this_thread::yield();
		}
	}

    void UpdateWaitingJobs(const CounterInstance* counterInstance)
    {
        if (counterInstance->GetValue() != 0)
            return;

        g_waitingFibersLock.lock();
        auto it = g_waitingFibers.find(counterInstance);
        if (it != g_waitingFibers.end())
        {
            for (FiberDecl& fiber : it->second)
            {
                while (!g_pWorkers[fiber.threadIndex]->sleepingFibers.push_back(fiber)) { Switch_Fiber(); }
            }
            g_waitingFibers.erase(it);
        }
        g_waitingFibersLock.unlock();
    }

    void WorkerMainLoop(void* pData)
    {
        JobInstance job;

        while (g_workerRunning)
        {
            if (g_jobPool.pop_front(job))
            {
                if (job.m_fence.GetValue() > 0)
                {
                    WaitForCounter_Fiber(job.m_fence);
                }

                (job.m_executable)();

                Counter& counter = job.m_counter;
                counter--;

                g_finishedLabel.fetch_add(1);
            }

            Switch_Fiber();
		}
        ::SwitchToFiber(g_pMainFiber);
    }

    void InitThread(uint32_t threadID)
    {
        assert(!g_pMainFiber);
        g_pMainFiber = ::ConvertThreadToFiber(nullptr);
        g_threadIndex = threadID;

        for (uint32_t i = 0; i < g_fiberPerThread; i++)
        {
            FiberDecl decl{ ::CreateFiber(64 * 1024, &WorkerMainLoop, nullptr), g_threadIndex };
            bool result = g_pWorkers[g_threadIndex]->freeFibers.push_back(decl);
            assert(result);
        }

        bool result = g_pWorkers[g_threadIndex]->freeFibers.pop_front(g_pCurrentFiber);
        assert(result);

        ::SwitchToFiber(g_pCurrentFiber.pFiber);
    }

    void Initialize()
    {
        g_workerRunning = true;

        unsigned int numCores{ std::thread::hardware_concurrency() };

        g_workerCount = (numCores == 0u) ? 1u : ((numCores > 8u) ? 8u : numCores);
        g_pWorkers.reserve(g_workerCount);
        for (uint32_t threadID = 0; threadID < g_workerCount; ++threadID)
        {
            g_pWorkers.push_back(new Worker(&InitThread));
            g_pWorkers.back()->Run(threadID);
        }
    }

    void Shutdown()
    {
        g_workerRunning = false;

        for (Worker* pWorker : g_pWorkers)
        {
            if (pWorker->thread.joinable())
                pWorker->thread.join();
        }
    }

    void Wait()
    {
        assert(!g_pCurrentFiber.pFiber); // Can't wait for all jobs to finish within a job
        while (g_finishedLabel.load() < g_currentLabel.load()) { std::this_thread::yield(); }
    }

    void WaitForCounter(const Counter& counter)
    {
        if (g_pCurrentFiber.pFiber)
        {
            WaitForCounter_Fiber(counter);
        }
        else
        {
            while (counter.GetValue() != 0) { std::this_thread::yield(); }
        }
    }

    void WaitForCounter_Fiber(const Counter& counter)
    {
		g_waitingFibersLock.lock();
		g_waitingFibers[counter.GetPtrValue()].push_back(g_pCurrentFiber);
		g_waitingFibersLock.unlock();

        if (!g_pWorkers[g_threadIndex]->sleepingFibers.pop_front(g_pCurrentFiber))
		    while (!g_pWorkers[g_threadIndex]->freeFibers.pop_front(g_pCurrentFiber)) { Switch_Fiber(); }
		assert(g_pCurrentFiber.pFiber);

		::SwitchToFiber(g_pCurrentFiber.pFiber);
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
        DispatchExplicitFence();
        return m_fence;
    }

    void JobBuilder::DispatchJobInternal(const std::function<void()>& job)
    {
        m_counter++;
        g_currentLabel.fetch_add(1);

        while (!g_jobPool.push_back(JobInstance{ job, m_counter, m_fence })) { Switch(); }
    }
}