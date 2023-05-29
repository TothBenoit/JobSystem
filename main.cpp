#include "JobSystem.h"
#include <iostream>
#include <chrono>
#include <thread>

int main()
{
	Job::Initialize();

	Job::JobBuilder jobBuilder1;
	for (int i = 1; i < 10000; i++)
	{
		jobBuilder1.DispatchJob<Job::Fence::None>([i]()
			{
				std::cout << "Job " << i << "\n  - ThreadID :" << std::this_thread::get_id() << "\n";
			}
		);
	}

	Job::Wait();
}