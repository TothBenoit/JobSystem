#include "JobSystem.h"
#include <iostream>
#include <sstream>

void Test()
{
	Job::JobBuilder jobBuilder1;

	jobBuilder1.DispatchJob([]()
		{
			std::cout << "1) Job " << "\n";
			Job::JobBuilder jobBuilder;
			for (int i = 0; i < 10; i++)
			{
				jobBuilder.DispatchJob([=]()
					{
						std::stringstream sstream;
						sstream << "\tIntermediate job " << i << "\n";
						std::cout << sstream.str();
					});
			}
			Job::WaitForCounter(jobBuilder.ExtractWaitCounter());
			std::cout << "\n----------------\nFirst batch of job done\n----------------\n\n";
		}
	);

	Job::Counter counter{ jobBuilder1.ExtractWaitCounter() };
	{
		Job::JobBuilder jobBuilder2;
		jobBuilder2.DispatchWait(counter);
		jobBuilder2.DispatchJob<Job::Fence::None>([]()
			{
				std::cout << "2) Job " << "\n";
				Job::JobBuilder jobBuilder;
				for (int i = 0; i < 10; i++)
				{
					jobBuilder.DispatchJob<Job::Fence::None>([=]()
						{
							std::stringstream sstream;
							sstream << "\tIntermediate job " << i << "\n";
							std::cout << sstream.str();
						});
				}
				jobBuilder.DispatchExplicitFence();
				jobBuilder.DispatchJob([]()
					{
						std::cout << "\n----------------\nSecond batch of job done\n----------------\n\n";
					}
				);
			}
		);
	}

	Job::Wait();
	jobBuilder1.DispatchJob([]()
		{
			std::cout << "3) Job " << "\n";
			Job::JobBuilder jobBuilder;
			for (int i = 0; i < 10; i++)
			{
				jobBuilder.DispatchJob([=]()
					{
						std::stringstream sstream;
						sstream << "\tIntermediate job " << i << "\n";
						std::cout << sstream.str();
					});
			}
			Job::WaitForCounter(jobBuilder.ExtractWaitCounter());
		}
	);
	jobBuilder1.DispatchJob([]()
		{
			std::cout << "\n----------------\nLast batch of job done\n----------------\n\n";
		}
	);
}

int main()
{
	Job::Initialize();
	
	Test();

	Job::Wait();
	Job::Shutdown();
}
