/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef PROFILER_H_
#define PROFILER_H_

#include <string>
#include <chrono>

namespace vc4c
{
#if DEBUG_MODE
#define PROFILE(func, ...) \
		profiler::ProfilingResult profile##func{#func, __FILE__, __LINE__, profiler::Clock::now()}; \
		func(__VA_ARGS__); \
		profiler::endFunctionCall(profile##func)

#define PROFILE_START(name) profiler::ProfilingResult profile##name{#name, __FILE__, __LINE__, profiler::Clock::now()}
#define PROFILE_END(name) profiler::endFunctionCall(profile##name)

#define PROFILE_START_DYNAMIC(name) profiler::ProfilingResult profile{name, __FILE__, __LINE__, profiler::Clock::now()}
#define PROFILE_END_DYNAMIC(name) profiler::endFunctionCall(profile)

#define PROFILE_COUNTER(index, name, value) profiler::increaseCounter(index, name, value, __FILE__, __LINE__)
#define PROFILE_COUNTER_WITH_PREV(index, name, value, prevIndex) profiler::increaseCounter(index, name, value, __FILE__, __LINE__, prevIndex)

#define PROFILE_RESULTS() profiler::dumpProfileResults();
#else
#define PROFILE(func, ...) func(__VA_ARGS__)

#define PROFILE_START(name)
#define PROFILE_END(name)

#define PROFILE_START_DYNAMIC(name)
#define PROFILE_END_DYNAMIC(name)

#define PROFILE_COUNTER(index, name, value)
#define PROFILE_COUNTER_WITH_PREV(index, name, value, prevIndex)

#define PROFILE_RESULTS()
#endif

	namespace profiler
	{
		using Clock = std::chrono::system_clock;
		using Duration = std::chrono::microseconds;

		struct ProfilingResult
		{
			std::string name;
			std::string fileName;
			std::size_t lineNumber;
			Clock::time_point startTime;
		};

		void endFunctionCall(const ProfilingResult& result);

		void dumpProfileResults(bool writeAsWarning = false);

		void increaseCounter(const std::size_t index, const std::string& name, const std::size_t value, const std::string& file, const std::size_t line, const std::size_t prevIndex = SIZE_MAX);
	}
}



#endif /* PROFILER_H_ */
