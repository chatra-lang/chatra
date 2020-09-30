/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2020 Chatra Project Team
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

#ifndef CHATRA_CHATRA_DEBUGGER_H
#define CHATRA_CHATRA_DEBUGGER_H

#include "chatra.h"

namespace chatra {
namespace debugger {

enum class ThreadId : size_t {};
enum class FrameId : size_t {};
enum class BreakPointId : size_t {};

struct CodePoint {
	std::string packageName;
	std::string fileName;
	unsigned lineNo = 0;
};

enum class FrameType {
	Package,
	ScriptRoot,
	Class,
	Method,
	Block,
};

struct FrameState {
	ThreadId threadId;
	FrameId frameId;
	FrameType frameType;
	CodePoint current;
	std::vector<FrameId> scopeFrameIds;

	// TODO variables & methods
};

struct ThreadState {
	ThreadId threadId;
	std::vector<FrameId> frameIds;  // top to bottom
};

struct InstanceState {
	InstanceId instanceId;
	PackageId primaryPackageId;
	std::vector<ThreadId> threadIds;
};

enum class StepRunResult {
	NotTraceable,
	NotInRunning,
	Blocked,
	WaitingResources,
	Finished,
	Stopped,
	BreakPoint,
};

struct IDebugger {
	virtual ~IDebugger() = default;

	virtual void pause() = 0;
	virtual void resume() = 0;

	virtual StepRunResult stepOver(ThreadId threadId) = 0;
	virtual StepRunResult stepInto(ThreadId threadId) = 0;
	virtual StepRunResult stepOut(ThreadId threadId) = 0;

	// TODO BreakPoints

	virtual std::vector<InstanceState> getInstancesState() = 0;
	virtual ThreadState getThreadState(ThreadId threadId) = 0;
	virtual FrameState getFrameState(ThreadId threadId, FrameId frameId) = 0;
};


}  // namespace debugger

namespace d = debugger;

}  // namespace chatra


CHATRA_ENUM_HASH(chatra::debugger::ThreadId)
CHATRA_ENUM_HASH(chatra::debugger::FrameId)
CHATRA_ENUM_HASH(chatra::debugger::BreakPointId)


#endif //CHATRA_CHATRA_DEBUGGER_H
