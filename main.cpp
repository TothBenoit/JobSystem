#include "JobSystem.h"
#include <iostream>


void Test()
{
	Job::JobBuilder jobBuilder1;

	for (int i = 0; i < 10; i++)
	{
		jobBuilder1.DispatchJob([=]()
			{
				std::cout << "1) Job " << i << "\n";
			}
		);
	}
	jobBuilder1.DispatchJob([]()
		{ 
			std::cout << "\n----------------\nFirst batch of job done\n----------------\n\n"; 
		}
	);

	Job::Counter counter{ jobBuilder1.ExtractWaitCounter() };
	
	{
		Job::JobBuilder jobBuilder2;
		jobBuilder2.DispatchWait(counter);
		for (int i = 10; i < 20; i++)
		{
			jobBuilder2.DispatchJob<Job::Fence::None>([=]()
				{
					std::cout << "2) Job " << i << "\n";
				}
			);
		}
		jobBuilder2.DispatchExplicitFence();
		jobBuilder2.DispatchJob([]()
			{
				std::cout << "\n----------------\nSecond batch of job done\n----------------\n\n";
			}
		);
		counter = jobBuilder2.ExtractWaitCounter();
	}

	jobBuilder1.DispatchWait(counter);
	for (int i = 20; i < 30; i++)
	{
		jobBuilder1.DispatchJob([=]()
			{
				std::cout << "3) Job " << i << "\n";
			}
		);
	}

	Job::Wait();
	std::cout << "\n----------------\nLast batch of job done\n----------------\n";
}

int main()
{
	Job::Initialize();
	
	Test();

	Job::Shutdown();
}
