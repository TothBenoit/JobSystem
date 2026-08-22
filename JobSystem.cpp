#include "JobSystem.h"

#include <windows.h>
#include <thread>   
#include <emmintrin.h>
#include <unordered_map>
#include <assert.h>

namespace Job
{
	struct CounterInstance;

	void UpdateWaitingJobs(const CounterInstance* pCounter);

	class SpinLock
	{
	public:
		void lock();
		void unlock();

	private:
		std::atomic<bool> m_lock{ false };
	};

	// Implementation inspired by WickedEngine blog
	template <typename T, size_t capacity>
	class ConcurrentRingBuffer
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
		CounterInstance* m_pFence;
		CounterInstance* m_pCounter;
	};

	struct WaitingListEntry
	{
		CounterInstance* m_pCounter;
		WaitingListEntry* m_pNext;
	};

	struct CounterInstance
	{
		uint32_t GetValue() const { return m_counter.load(); }

		void Decrement()
		{
			if (m_counter.fetch_sub(1) == 1)
			{
				if (m_hasWaitingJobs)
					UpdateWaitingJobs(this);
				m_waitingCountersLock.lock();
				WaitingListEntry* pCurrentWaitingCounter = m_pWaitingCounters;
				m_waitingCountersLock.unlock();
				while (pCurrentWaitingCounter) {
					pCurrentWaitingCounter->m_pCounter->Decrement();
					pCurrentWaitingCounter = pCurrentWaitingCounter->m_pNext;
				}
			}
		}

		void AddListener(CounterInstance& counter) const
		{
			counter.m_counter.fetch_add(1);
			m_waitingCountersLock.lock();
			bool shouldAdd = GetValue() > 0;
			if (shouldAdd)
			{
				counter.m_refCount.fetch_add(1);
				WaitingListEntry* pEntry = new WaitingListEntry();
				pEntry->m_pCounter = &counter;
				pEntry->m_pNext = m_pWaitingCounters;
				m_pWaitingCounters = pEntry;
			}
			m_waitingCountersLock.unlock();
			if (!shouldAdd)
				counter.Decrement();
		}

		~CounterInstance()
		{
			WaitingListEntry* pCurrentWaitingCounter = m_pWaitingCounters;
			while (pCurrentWaitingCounter) {
				if (pCurrentWaitingCounter->m_pCounter->m_refCount.fetch_sub(1) == 1)
					delete pCurrentWaitingCounter->m_pCounter;
				WaitingListEntry* pNext = pCurrentWaitingCounter->m_pNext;
				delete pCurrentWaitingCounter;
				pCurrentWaitingCounter = pNext;
			}
		}

		std::atomic<uint32_t> m_counter{ 0 };
		std::atomic<uint32_t> m_refCount{ 1 };
		mutable WaitingListEntry* m_pWaitingCounters{ nullptr };
		mutable SpinLock m_waitingCountersLock;
		mutable bool m_hasWaitingJobs{ false };
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
		ConcurrentRingBuffer<FiberDecl, g_fiberPerThread> freeFibers;
		ConcurrentRingBuffer<FiberDecl, g_fiberPerThread> sleepingFibers;
	};
	
	std::vector<Worker*> g_pWorkers;
	ConcurrentRingBuffer<JobInstance, 256> g_jobPool;

	SpinLock g_waitingFibersLock;
	std::unordered_map <const CounterInstance*, std::vector<FiberDecl>> g_waitingFibers;

#pragma optimize( "", off )
	thread_local uint32_t g_workerID;
	thread_local void* g_pMainFiber{ nullptr };
	thread_local FiberDecl g_pCurrentFiber{ nullptr, 0 };
#pragma optimize( "", on )

	std::atomic<uint64_t>   g_currentLabel{ 0 };
	std::atomic<uint64_t>   g_finishedLabel{ 0 };
	bool                    g_workerRunning{ false };

	void Switch_Fiber()
	{
		bool result = g_pWorkers[g_workerID]->freeFibers.push_back(g_pCurrentFiber);

		assert(result);
		if (!g_pWorkers[g_workerID]->sleepingFibers.pop_front(g_pCurrentFiber))
		{
			result = g_pWorkers[g_workerID]->freeFibers.pop_front(g_pCurrentFiber);
			assert(result);
		}
		assert(g_pCurrentFiber.pFiber);

		::SwitchToFiber(g_pCurrentFiber.pFiber);
	}

	void UpdateWaitingJobs(const CounterInstance* pCounter)
	{
		g_waitingFibersLock.lock();
		auto it = g_waitingFibers.find(pCounter);
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

	uint32_t GetWorkerID()
	{
		return g_workerID;
	}

	uint32_t GetWorkerCount()
	{
		return (uint32_t)g_pWorkers.size();
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

	void WaitForCounter_Fiber(const CounterInstance* pCounter)
	{
		pCounter->m_hasWaitingJobs = true;
		g_waitingFibersLock.lock();
		g_waitingFibers[pCounter].push_back(g_pCurrentFiber);
		g_waitingFibersLock.unlock();

		if (!g_pWorkers[g_workerID]->sleepingFibers.pop_front(g_pCurrentFiber))
			while (!g_pWorkers[g_workerID]->freeFibers.pop_front(g_pCurrentFiber)) { Switch_Fiber(); }
		assert(g_pCurrentFiber.pFiber);

		::SwitchToFiber(g_pCurrentFiber.pFiber);
	}

	void WorkerMainLoop(void* pData)
	{
		{
			JobInstance job;

			while (g_workerRunning)
			{
				if (g_jobPool.pop_front(job))
				{
					CounterInstance* pFence = job.m_pFence;
					if (pFence->GetValue() > 0)
					{
						WaitForCounter_Fiber(pFence);
					}

					(job.m_executable)();

					CounterInstance* pCounter = job.m_pCounter;
					pCounter->Decrement();
					if (pCounter->m_refCount.fetch_sub(1) == 1)
						delete job.m_pCounter;
					if (pFence->m_refCount.fetch_sub(1) == 1)
						delete pFence;

					g_finishedLabel.fetch_add(1);
				}

				Switch_Fiber();
			}
		}

		// Shutdown in progress
		// Every job must exit the previous scope to destroy the jobInstance

		if (!g_pWorkers[g_workerID]->sleepingFibers.pop_front(g_pCurrentFiber))
			if (!g_pWorkers[g_workerID]->freeFibers.pop_front(g_pCurrentFiber))
				g_pCurrentFiber.pFiber = g_pMainFiber;

		::SwitchToFiber(g_pCurrentFiber.pFiber);
	}

	void InitThread(uint32_t threadID)
	{
		assert(!g_pMainFiber);
		g_pMainFiber = ::ConvertThreadToFiber(nullptr);
		g_workerID = threadID;

		for (uint32_t i = 0; i < g_fiberPerThread; i++)
		{
			FiberDecl decl{ ::CreateFiber(64 * 1024, &WorkerMainLoop, nullptr), g_workerID };
			bool result = g_pWorkers[g_workerID]->freeFibers.push_back(decl);
			assert(result);
		}

		bool result = g_pWorkers[g_workerID]->freeFibers.pop_front(g_pCurrentFiber);
		assert(result);

		::SwitchToFiber(g_pCurrentFiber.pFiber);
	}

	void Initialize()
	{
		g_workerRunning = true;

		uint32_t numCores{ std::thread::hardware_concurrency() };

		uint32_t workerCount = (numCores == 0u) ? 1u : ((numCores > 8u) ? 8u : numCores);
		g_pWorkers.reserve(workerCount);
		for (uint32_t threadID = 0; threadID < workerCount; ++threadID)
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

		while (!g_pWorkers.empty())
		{
			Worker* pWorker{ g_pWorkers.back() };
			g_pWorkers.pop_back();
			delete(pWorker);
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
			WaitForCounter_Fiber(counter.m_pCounterInstance);
		}
		else
		{
			while (counter.GetValue() != 0) { std::this_thread::yield(); }
		}
	}

	void SpinLock::lock()
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

	void SpinLock::unlock()
	{
		m_lock.store(false);
	}

	Counter::Counter()
	{
		m_pCounterInstance = new CounterInstance();
	}

	Counter::Counter(const Counter& other)
	{
		if (&other == this)
			return;
		m_pCounterInstance = new CounterInstance();
		other.m_pCounterInstance->AddListener(*m_pCounterInstance);
	}

	Counter& Counter::operator=(const Counter& other)
	{
		if (&other == this)
			return *this;
		if (m_pCounterInstance->m_refCount.fetch_sub(1) == 1)
			delete m_pCounterInstance;
		m_pCounterInstance = new CounterInstance();
		other.m_pCounterInstance->AddListener(*m_pCounterInstance);
		return *this;
	}

	Counter::Counter(Counter&& other)
	{
		if (&other == this)
			return;
		m_pCounterInstance = other.m_pCounterInstance;
		m_pCounterInstance->m_refCount.fetch_add(1);
	}

	Counter& Counter::operator=(Counter&& other)
	{
		if (&other == this)
			return *this;
		if (m_pCounterInstance->m_refCount.fetch_sub(1) == 1)
			delete m_pCounterInstance;
		m_pCounterInstance = other.m_pCounterInstance;
		m_pCounterInstance->m_refCount.fetch_add(1);
		return *this;
	}

	Counter::~Counter()
	{
		if (m_pCounterInstance->m_refCount.fetch_sub(1) == 1)
			delete m_pCounterInstance;
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
		if (&other == this)
			return *this;
		CounterInstance* pNewInstance = new CounterInstance();
		m_pCounterInstance->AddListener(*pNewInstance);
		other.m_pCounterInstance->AddListener(*pNewInstance);
		if (m_pCounterInstance->m_refCount.fetch_sub(1) == 1)
			delete m_pCounterInstance;
		m_pCounterInstance = pNewInstance;
		return *this;
	}

	Counter Counter::operator+(const Counter& other)
	{
		Counter counter{ *this };
		counter += other;
		return counter;
	}

	uint32_t Counter::GetValue() const
	{ 
		return m_pCounterInstance->GetValue(); 
	}

	void JobBuilder::DispatchExplicitFence()
	{
		if (m_accumulateCounter.GetValue() > 0)
		{
			m_waitCounter = m_accumulateCounter;
			m_accumulateCounter = Counter();
		}
	}

	void JobBuilder::DispatchWait(const Counter& counter)
	{
		m_waitCounter += counter;
	}

	const Counter& JobBuilder::ExtractWaitCounter()
	{
		DispatchExplicitFence();
		return m_waitCounter;
	}

	void JobBuilder::DispatchJobInternal(const std::function<void()>& job)
	{
		m_accumulateCounter++;
		g_currentLabel.fetch_add(1);
		m_waitCounter.m_pCounterInstance->m_refCount.fetch_add(1);
		m_accumulateCounter.m_pCounterInstance->m_refCount.fetch_add(1);
		while (!g_jobPool.push_back(JobInstance{ job,  m_waitCounter.m_pCounterInstance, m_accumulateCounter.m_pCounterInstance })) { Switch(); }
	}
}
