#pragma once
#include <atomic>
#include <functional>
#include <stdint.h>

#pragma warning( disable : 4530)

/*************************/
// TODO :
// - Add different priority queues
/*************************/

namespace Job
{
	struct CounterInstance;
	class Counter;

	void Initialize();
	void Shutdown();
	void Wait();
	void WaitForCounter(const Counter& counter);
	uint32_t GetWorkerID();
	uint32_t GetWorkerCount();


	class Counter
	{
	public:
		Counter();
		Counter(const Counter&);
		Counter& operator=(const Counter&);
		Counter(Counter&&);
		Counter& operator=(Counter&&);
		~Counter();

		Counter& operator++();
		Counter& operator++(int);
		Counter& operator--();
		Counter& operator--(int);
		Counter& operator+=(const Counter& other);
		Counter  operator+(const Counter& other);

		uint32_t GetValue() const;

	private:
		friend class JobBuilder;
		friend void WaitForCounter(const Counter&);
		friend CounterInstance;

		CounterInstance* m_pCounterInstance{ nullptr };
	};

	enum class Fence
	{
		None,
		With
	};

	class JobBuilder
	{
	public:
		template<Fence fenceType = Fence::With>
		void DispatchJob(const std::function<void()>& job);
		void DispatchExplicitFence();
		void DispatchWait(const Counter& counter);
		const Counter& ExtractWaitCounter();

	private:
		void DispatchJobInternal(const std::function<void()>& job);

	private:
		Counter     m_accumulateCounter;
		Counter     m_waitCounter;
	};

	template<Fence fenceType>
	void JobBuilder::DispatchJob(const std::function<void()>& job)
	{
		DispatchJobInternal(job);
		if constexpr (fenceType == Fence::With)
		{
			DispatchExplicitFence();
		}
	}
}
