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
#include <unordered_map>

namespace chatra {
namespace debugger {

enum class ThreadId : size_t {};
enum class FrameId : size_t {};
enum class ScopeId : size_t {};
enum class ObjectId : size_t {};
enum class BreakPointId : size_t {};

CHATRA_DEFINE_EXCEPTION(IllegalRuntimeStateException, UnsupportedOperationException);
CHATRA_DEFINE_EXCEPTION(ParserErrorAroundBreakPointException, UnsupportedOperationException);
CHATRA_DEFINE_EXCEPTION(NonBreakableStatementException, UnsupportedOperationException);

struct CodePoint {
	std::string packageName;
	std::string fileName;
	unsigned lineNo;
};

struct PackageState {
	PackageId packageId;
	std::string packageName;
	std::vector<Script> scripts;
};

enum class ValueType {
	Null,
	Bool,
	Int,
	Float,
	String,
	Object,
	Method,
};

struct Value {
	ValueType valueType;
	union {
		bool vBool;
		int64_t vInt;
		double vFloat;
		ObjectId objectId;
	};
	std::string vString;
	std::string className;
	std::string methodName;
	std::string methodArgs;
};

enum class ScopeType {
	Package,
	ScriptRoot,
	Class,
	Method,
	Block,
};

struct ScopeState {
	ThreadId threadId;
	ScopeId scopeId;
	ScopeType scopeType;
	std::unordered_map<std::string, Value> values;
};

using FrameType = ScopeType;

struct FrameState {
	ThreadId threadId;
	FrameId frameId;
	FrameType frameType;
	CodePoint current;
	std::vector<ScopeId> scopeIds;
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

struct ObjectState {
	ObjectId objectId;
	std::string className;
	std::unordered_map<std::string, Value> values;
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

struct IDebuggerHost {
	virtual ~IDebuggerHost() = default;

	virtual void onBreakPoint(BreakPointId breakPointId) { (void)breakPointId; }
};

struct IDebugger {
	virtual ~IDebugger() = default;

	virtual void pause() = 0;
	virtual void resume() = 0;
	virtual bool isPaused() = 0;

	virtual StepRunResult stepOver(ThreadId threadId) = 0;
	virtual StepRunResult stepInto(ThreadId threadId) = 0;
	virtual StepRunResult stepOut(ThreadId threadId) = 0;

	virtual BreakPointId addBreakPoint(const CodePoint& point) = 0;
	virtual void removeBreakPoint(BreakPointId breakPointId) = 0;

	virtual std::vector<PackageState> getPackagesState() = 0;
	virtual std::vector<InstanceState> getInstancesState() = 0;
	virtual ThreadState getThreadState(ThreadId threadId) = 0;
	virtual FrameState getFrameState(ThreadId threadId, FrameId frameId) = 0;
	virtual ScopeState getScopeState(ThreadId threadId, ScopeId scopeId) = 0;
	virtual ObjectState getObjectState(ObjectId objectId) = 0;
};


}  // namespace debugger

namespace d = debugger;

}  // namespace chatra


CHATRA_ENUM_HASH(chatra::debugger::ThreadId)
CHATRA_ENUM_HASH(chatra::debugger::FrameId)
CHATRA_ENUM_HASH(chatra::debugger::ScopeId)
CHATRA_ENUM_HASH(chatra::debugger::ObjectId)
CHATRA_ENUM_HASH(chatra::debugger::BreakPointId)


#endif //CHATRA_CHATRA_DEBUGGER_H
