#pragma once
#include <functional>
#include <atomic>   

namespace Job
{
    void Initialize();
    void Wait();

    using Counter = std::atomic<uint64_t>;

    enum class Fence
    {
        None,
        With
    };

    class JobBuilder
    {
    public:
        JobBuilder();

        template<Fence fenceType = Fence::With>
        void DispatchJob(std::function<void()> job);
        void DispatchExplicitFence();

    private:
        void DispatchJobInternal(std::function<void()> job);

    private:
        Counter m_totalJob{ 0 };
        Counter m_jobFinished{ 0 };
        Counter m_fence{ 0 };

    };

    template<Fence fenceType>
    void JobBuilder::DispatchJob(std::function<void()> job)
    {
        if constexpr (fenceType == Fence::With)
        {
            DispatchExplicitFence();
        }
        DispatchJobInternal(job);
        m_totalJob.fetch_add(1);
    }
}