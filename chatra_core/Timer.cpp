/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019 Chatra Project Team
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * author: Satoshi Hosokawa (chatra.hosokawa@gmail.com)
 */

#include "Timer.h"

namespace chatra {

Time Timer::getNextTime() {
	std::lock_guard<SpinLock> lock(lockMap);
	return timeQueue.empty() ? Time::max() : timeQueue.front();
}

void Timer::popAndInvokeHandlers() {
	std::unordered_map<unsigned, TimerHandler> handlers;
	{
		std::lock_guard<SpinLock> lock(lockMap);
		assert(!map.empty());

		auto tm = timeQueue.front();
		timeQueue.pop_front();

		auto it = map.find(tm);
		assert(it != map.cend());
		handlers = std::move(it->second.handlers);
		map.erase(it);
		for (auto& e : handlers) {
			idToTime.erase(e.first);
			recycledIds.emplace_back(e.first);
		}
	}

	for (auto& e : handlers) {
		try {
			e.second();
		}
		catch (...) {
			// nothing to do
		}
	}
}

unsigned Timer::submitHandler(Time tm, TimerHandler handler) {
	bool handlerInserted = false;
	unsigned id;
	{
		std::lock_guard<SpinLock> lock0(lockMap);

		if (recycledIds.empty())
			id = nextId++;
		else {
			id = recycledIds.back();
			recycledIds.pop_back();
		}

		idToTime.emplace(id, tm);

		auto it = map.find(tm);
		if (it == map.end()) {
			map[tm].handlers.emplace(id, std::move(handler));
			if (map.cbegin()->first == tm)
				handlerInserted = true;

			if (timeQueue.empty())
			    timeQueue.emplace_back(tm);
			else {
                timeQueue.insert(std::partition_point(timeQueue.cbegin(), timeQueue.cend(), [&](Time _tm) {
                    return _tm < tm;
                }), tm);
            }
		}
		else
			it->second.handlers.emplace(id, std::move(handler));
	}

	if (handlerInserted)
		onHandlerInserted();
	return id;
}

bool Timer::cancel(unsigned id) {
	std::lock_guard<SpinLock> lock0(lockMap);
	auto it = idToTime.find(id);
	if (it == idToTime.end())
		return false;
	map[it->second].handlers.erase(id);
	idToTime.erase(it);
	recycledIds.emplace_back(id);
	return true;
}

void Timer::cancelAll() {
	std::lock_guard<SpinLock> lock0(lockMap);
	timeQueue.clear();
	map.clear();
	idToTime.clear();
	recycledIds.clear();
	nextId = 0;
}


class SystemTimer : public Timer {
private:
	bool stop = false;
	std::mutex mt;
	std::condition_variable cv;
	std::thread th;

private:
	void run() {
		for (;;) {
			{
				std::unique_lock<std::mutex> lock0(mt);
				auto nextTime = getNextTime();
				auto nextPoint = (nextTime == Time::max() ? Clock::time_point::max() : Clock::time_point(nextTime));
				if (cv.wait_until(lock0, nextPoint, [&]() { return stop; }))
					return;
			}
			popAndInvokeHandlers();
		}
	}

protected:
	void onHandlerInserted() override {
		cv.notify_one();
	}

public:
	SystemTimer() noexcept {
		th = std::thread([this]() { run(); });
	}

	~SystemTimer() override {
		stop = true;
		cv.notify_one();
		th.join();
	}

	Time getTime() override {
		return std::chrono::duration_cast<Time>(Clock::now().time_since_epoch());
	}
};


class EmulatedTimer : public Timer {
public:
	~EmulatedTimer() override = default;

private:
	SpinLock lockTime;
	Time time;

protected:
	void onHandlerInserted() override {
		auto currentTime = getTime();
		while (getNextTime() <= currentTime)
			popAndInvokeHandlers();
	}

public:
	EmulatedTimer() noexcept : time(0) {
	}

	Time getTime() override {
		std::lock_guard<SpinLock> lock(lockTime);
		return time;
	}

	void increment(Time step) override {
		Time currentTime;
		{
			std::lock_guard<SpinLock> lock(lockTime);
			currentTime = (time += step);
		}
		while (getNextTime() <= currentTime)
			popAndInvokeHandlers();
	}
};


std::unique_ptr<Timer> newSystemTimer() {
	return std::unique_ptr<Timer>(new SystemTimer());
}

std::unique_ptr<Timer> newEmulatedTimer() {
	return std::unique_ptr<Timer>(new EmulatedTimer());
}

}  // namespace chatra
