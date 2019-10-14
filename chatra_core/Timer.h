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

#ifndef CHATRA_TIMER_H
#define CHATRA_TIMER_H

#include "Internal.h"

namespace chatra {

using Clock = std::chrono::steady_clock;
using Time = std::chrono::milliseconds;
using TimerHandler = std::function<void()>;


struct TimerElement {
	std::unordered_map<unsigned, TimerHandler> handlers;
};


class Timer {
public:
	virtual ~Timer() = default;

private:
	SpinLock lockMap;
	std::map<Time, TimerElement> map;
	std::unordered_map<unsigned, Time> idToTime;
	std::vector<unsigned> recycledIds;
	unsigned nextId = 0;

protected:
	virtual void onHandlerInserted() = 0;

	Time getNextTime();
	void popAndInvokeHandlers();

private:
	unsigned submitHandler(Time tm, TimerHandler handler);

public:
	virtual Time getTime() = 0;

	virtual void increment(Time step) { (void)step; }

	template <typename Process>
	unsigned at(Time time, Process process) {
		return submitHandler(time, process);
	}

	template <typename Process>
	unsigned delay(Time timeout, Process process) {
		return submitHandler(getTime() + timeout, process);
	}

	bool cancel(unsigned id);

	void cancelAll();
};

std::unique_ptr<Timer> newSystemTimer();
std::unique_ptr<Timer> newEmulatedTimer();

}  // namespace chatra

#endif //CHATRA_TIMER_H
