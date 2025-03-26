#pragma once
#include <atomic>
#include <functional>

#pragma warning( disable : 4530)

/*************************/
// TODO :
// - Add different priority queues
/*************************/

namespace Job
{
    class CounterInstance;
    class Counter;

    void Initialize();
    void Shutdown();
    void Wait();
    void WaitForCounter(const Counter& counter);


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

        uint32_t GetValue() const;

    private:
        friend void WaitForCounter_Fiber(const Counter&);
        friend CounterInstance;

        CounterInstance* m_pCounterInstance;
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
        Counter     m_counter;
        Counter     m_fence;
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