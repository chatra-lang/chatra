/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019-2021 Chatra Project Team
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

#include "Runtime.h"

namespace chatra {

static MethodTable embeddedMethods {MethodTable::ForEmbeddedMethods()};
static AsyncOperatorTable embeddedOperators;
static std::forward_list<std::shared_ptr<Node>> embeddedMethodsNode;
static std::unordered_map<StringId, Node*> nodeMap;

static const char* initFunctions = R"***(
def log(a0: String) as native
def dump() as native
def dump(a0) as native
def gc() as native

def _native_time(clock) as native
def time(clock: '')
	return _native_time(clock)

def _native_sleep(timeout, clock, relative) as native
def sleep(timeout: Int, clock: '', relative: true)
	_native_sleep(timeout, clock, relative)

def _native_wait(a0, a1) as native
def wait(a0...; a1...)
	t0 = _native_wait(a0, a1)
	return t0, t0 is Int ? a0[t0] : a1[t0]

def type(a0) as native
def objectId(a0) as native

def _native_compile(a0: String, a1: Int) as native

def operator(a0 == a1)
	return type(a0).equals(type(a1)) ? a0.equals(a1) : false
def operator(a0 != a1)
	return !(type(a0).equals(type(a1)) ? a0.equals(a1) : false)

def operator(a0: String + a1)
	return a0.clone().append(a1)
def operator(a0: Array + a1)
	return a0.clone().append(a1)
)***"
#ifndef CHATRA_NDEBUG
R"***(
def _check(a0: String, a1: Bool) as native
def _checkCmd(a0: String) as native
def _incrementTestTimer(a0: Int) as native
)***"
#endif // !CHATRA_NDEBUG
;

void initializeEmbeddedFunctions() {
	auto& classes = refEmbeddedClassTable();

	INullErrorReceiver nullErrorReceiver;
	IAssertionNullErrorReceiver assertionNullErrorReceiver;
	IErrorReceiverBridge errorReceiver(assertionNullErrorReceiver);

	std::shared_ptr<StringTable> sTable = StringTable::newInstance();

	struct ClassFinder : public IClassFinder {
		ClassTable& classes;
		explicit ClassFinder(ClassTable& classes) : classes(classes) {}
		const Class* findClass(StringId name) override {
			return classes.find(name);
		}
		const Class* findPackageClass(StringId packageName, StringId name) override {
			(void)packageName;
			(void)name;
			return nullptr;
		}
	} classFinder(classes);

	auto lines = parseLines(errorReceiver, sTable, "(internal-functions)", 1, initFunctions);
	auto node = groupScript(errorReceiver, sTable, lines);
	structureInnerNode(errorReceiver, sTable, node.get(), true);
	parseInnerNode(errorReceiver, sTable, node.get(), true);
	nodeMap.emplace(StringId::EmbeddedFunctions, node.get());

	unsigned defOperatorCount = 0;
	for (auto& n : node->symbols) {
		if (n->type == NodeType::Def) {
			embeddedMethods.add(n.get(), nullptr, n->sid, StringId::Invalid,
					tupleToArguments(errorReceiver, sTable.get(), classFinder, n->subNodes[SubNode::Def_Parameter].get()), {});
		}
		else if (n->type == NodeType::DefOperator) {
			if (defOperatorCount++ < 2)  // == and !=, without any class restrictions
				addOperatorMethod(embeddedOperators, nullErrorReceiver, sTable.get(), classFinder, nullptr, n.get());
			else
				addOperatorMethod(embeddedOperators, errorReceiver, sTable.get(), classFinder, nullptr, n.get());
		}
	}

	embeddedMethodsNode.emplace_front(std::move(node));

	if (errorReceiver.hasError())
		throw InternalError();

	if (sTable->getVersion() != 0) {
		#ifndef CHATRA_NDEBUG
			printf("Additional strings:");
			for (auto i = static_cast<size_t>(StringId::PredefinedStringIds); i < sTable->validIdCount(); i++)
				printf(" %s", sTable->ref(static_cast<StringId>(i)).c_str());
			printf("\n");
		#endif
		throw InternalError();
	}

	embeddedMethods.add(NativeMethod(StringId::log, StringId::Invalid, native_log));
	embeddedMethods.add(NativeMethod(StringId::dump, StringId::Invalid, native_dump));
	embeddedMethods.add(NativeMethod(StringId::gc, StringId::Invalid, native_gc));
	embeddedMethods.add(NativeMethod(StringId::_native_time, StringId::Invalid, native_time));
	embeddedMethods.add(NativeMethod(StringId::_native_sleep, StringId::Invalid, native_sleep));
	embeddedMethods.add(NativeMethod(StringId::_native_wait, StringId::Invalid, native_wait));
	embeddedMethods.add(NativeMethod(StringId::type, StringId::Invalid, native_type));
	embeddedMethods.add(NativeMethod(StringId::objectId, StringId::Invalid, native_objectId));
	embeddedMethods.add(NativeMethod(StringId::_native_compile, StringId::Invalid, native_compile));
	embeddedMethods.add(NativeMethod(StringId::_check, StringId::Invalid, native_check));
	embeddedMethods.add(NativeMethod(StringId::_checkCmd, StringId::Invalid, native_checkCmd));
	embeddedMethods.add(NativeMethod(StringId::_incrementTestTimer, StringId::Invalid, native_incrementTestTimer));
}

void registerEmbeddedFunctions(MethodTable& methods, AsyncOperatorTable& operators) {
	methods = embeddedMethods;
	operators.import(embeddedOperators);
}

const std::unordered_map<StringId, Node*>& refNodeMapForEmbeddedFunctions() {
	return nodeMap;
}

void outputLog(Thread& thread, const std::string& message) {
	auto str = message;
	if (!str.empty() && str.back() != '\n')
		str.append("\n");
	thread.runtime.outputError(str);
}

void native_log(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
	auto message = args.ref(0);
	outputLog(thread, message.isNull() ? std::string("null") : message.deref<String>().getValue());
}

void native_dump(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
#ifndef CHATRA_NDEBUG
	if (args.size() == 0) {
		thread.runtime.dump();
		return;
	}
	if (args.ref(0).valueType() != ReferenceValueType::Object || args.ref(0).isNull())
		return;

	if (getReferClass(args.ref(0)) == String::getClassStatic()) {
		std::lock_guard<SpinLock> lock(thread.runtime.lockSTable);
		auto& sTable = thread.runtime.distributedSTable;
		auto key = args.ref(0).deref<String>().getValue();

		if (key == "package")
			thread.frames.back().package.dump(sTable);
		else if (key == "package_scope")
			thread.frames.back().package.scope->ref(StringId::PackageObject).deref().dump(sTable);
		else if (key == "scope")
			thread.frames.back().scope->dump(sTable);
		return;
	}

	{
		std::lock_guard<SpinLock> lock(thread.runtime.lockSTable);
		args.ref(0).deref<ObjectBase>().dump(thread.runtime.distributedSTable);
	}
#endif // !CHATRA_NDEBUG
}

void native_gc(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
	thread.runtime.fullGc();
}

void native_time(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
	auto clock = (args.ref(0).isNull() ? "" : args.ref(0).deref<String>().getValue());

	RuntimeImp* runtime = &thread.runtime;
	std::lock_guard<SpinLock> lock(runtime->lockTimers);
	auto it = runtime->timers.find(clock);
	auto* timer = (it == runtime->timers.end() ? runtime->timers.at("") : it->second).get();
	ret.setInt(static_cast<int64_t>(timer->getTime().count()));
}

void native_sleep(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
	if (args.ref(0).isNull() || args.ref(1).isNull() || args.ref(2).isNull())
		throw RuntimeException(StringId::IllegalArgumentException);

	int64_t timeout = args.ref(0).getInt();
	auto clock = args.ref(1).deref<String>().getValue();
	bool relative = args.ref(2).getBool();

	RuntimeImp* runtime = &thread.runtime;
	unsigned waitingId = thread.requestToPause();
	Timer* timer;
	Time time;

	{
		std::lock_guard<SpinLock> lock(runtime->lockTimers);

		auto it = runtime->timers.find(clock);
		if (it == runtime->timers.end())
			throw RuntimeException(StringId::IllegalArgumentException);
		timer = it->second.get();
		time = std::chrono::milliseconds(timeout);
		if (relative)
			time += timer->getTime();

		runtime->sleepRequests.emplace(waitingId, std::make_tuple(clock, static_cast<int64_t>(time.count())));
	}

	runtime->issueTimer(waitingId, *timer, time);
}

void native_wait(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;

	auto& argList = args.ref(0).deref<Array>();
	auto& argDict = args.ref(1).deref<Dict>();

	std::vector<std::tuple<size_t, std::string, Reference>> targets;
	targets.reserve(argList.size() + argDict.size());

	{
		std::lock_guard<SpinLock> lock(argList.lockValue);
		for (size_t i = 0; i < argList.length; i++)
			targets.emplace_back(i, "", argList.container().ref(i));
	}

	{
		std::lock_guard<SpinLock> lock(argDict.lockValue);
		for (auto& e : argDict.keyToIndex)
			targets.emplace_back(SIZE_MAX, e.first, argDict.container().ref(e.second));
	}

	if (targets.empty())
		throw RuntimeException(StringId::IllegalArgumentException);
	for (auto& e : targets)
		derefAsEventObject(std::get<2>(e));

	WaitContext* ct;
	{
		auto& runtime = thread.runtime;
		std::lock_guard<SpinLock> lock(runtime.lockScope);
		if (runtime.recycledRefs.empty()) {
			auto ref = runtime.scope->addExclusive();
			ct = &ref.allocate<WaitContext>(ref, thread, targets.size());
		}
		else {
			auto ref = runtime.recycledRefs.back();
			runtime.recycledRefs.pop_back();
			ct = &ref.allocate<WaitContext>(ref, thread, targets.size());
		}
	}
	ct->deploy(thread.requestToPause(), thread.callerFrame, std::move(targets));
}

void native_type(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;

	auto* cl = getReferClass(args.ref(0));
	if (cl == nullptr) {
		ret.setNull();
		return;
	}

	auto& sTable = thread.runtime.distributedSTable;
	auto value = sTable->ref(cl->getName());

	auto* package = cl->getPackage();
	if (package != nullptr)
		value = package->name + "." + value;

	ret.allocate<String>().setValue(value);
}

void native_objectId(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;

	auto ref = args.ref(0);
	if (ref.valueType() != ReferenceValueType::Object || ref.isNull()) {
		ret.setInt(-1);
		return;
	}

	auto id = ref.deref().getObjectIndex();
	chatra_assert(id != SIZE_MAX);
	ret.setInt(static_cast<int64_t>(id));
}

static void buildReflectNode(Reference sharedRef, Reference ref, Node* node);

static void buildReflectNodeCallRecursive(Reference sharedRef, ReflectNode& reflectNode, Node* node) {
	auto& blocks = reflectNode.ref(1).allocateWithoutLock<Array>();
	for (auto& n : node->blockNodes)
		buildReflectNode(sharedRef, blocks.add(), n.get());

	auto& subs = reflectNode.ref(2).allocateWithoutLock<Array>();
	for (auto& n : node->subNodes) {
		auto ref = subs.add();
		if (n)
			buildReflectNode(sharedRef, ref, n.get());
	}
}

static void buildReflectNode(Reference sharedRef, Reference ref, Node* node) {
	sharedRef.derefWithoutLock<ReflectNodeShared>().addNode(*node);

	auto& reflectNode = ref.allocateWithoutLock<ReflectNode>(node);
	reflectNode.ref(0).setWithoutBothLock(sharedRef);

	buildReflectNodeCallRecursive(sharedRef, reflectNode, node);
}

void native_compile(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;

	auto arg0 = args.ref(0);
	if (arg0.isNull())
		throw RuntimeException(StringId::IllegalArgumentException);

	auto script = arg0.deref<String>().getValue();

	auto popCount = static_cast<size_t>(args.ref(1).getInt());
	size_t targetSize = thread.frames.size();
	for (size_t i = 0; i < popCount; i++) {
		if (targetSize <= residentialFrameCount)
			throw RuntimeException(StringId::IllegalArgumentException);
		targetSize -= thread.frames[targetSize - 1].popCount;
	}
	auto* callerNode = thread.frames[targetSize - 1].getExceptionNode();
	std::shared_ptr<Line> callerLine;
	if (callerNode != nullptr && !callerNode->tokens.empty())
		callerLine = callerNode->tokens.front()->line.lock();

	std::string callerName = (callerLine ?
			RuntimeImp::formatOrigin(callerLine->fileName, callerLine->lineNo) : "unknown file")
			+ ":compile";

	std::vector<std::shared_ptr<Line>> lines;
	std::shared_ptr<Node> node;
	{
		auto& runtime = thread.runtime;
		auto& sTable = runtime.primarySTable;

		std::lock_guard<std::mutex> lock(runtime.mtParser);
		unsigned sTableVersion = sTable->getVersion();

		// This may update StringTable, but current thread doesn't have to capture its updates;
		// because Node object generated by this function forces any thread to capture latest StringTable
		// prior to access it.
		try {
			IErrorReceiverBridge errorReceiverBridge(thread);
			auto check = [&]() {
				if (errorReceiverBridge.hasError())
					throw RuntimeException(StringId::IllegalArgumentException);
			};

			lines = parseLines(errorReceiverBridge, sTable, callerName, 1, script);
			node = groupScript(errorReceiverBridge, sTable, lines);
			check();

			chatra_assert(node->blockNodesState == NodeState::Grouped);
			structureInnerNode(errorReceiverBridge, sTable, node.get(), true);
			check();

			chatra_assert(node->blockNodesState == NodeState::Structured);
			parseInnerNode(errorReceiverBridge, sTable, node.get(), true);
			check();

			node->blockNodesState = NodeState::Parsed;
		}
		catch (const RuntimeException&) {
			runtime.distributeStringTable(sTableVersion);
			throw;
		}

		runtime.distributeStringTable(sTableVersion);
	}

	auto& reflectNode = ret.allocate<ReflectNode>(node.get());
	chatra_assert(ReflectNode::getClassStatic()->refMethods().find(nullptr, StringId::_shared, StringId::Invalid, {}, {})->position == 0);
	chatra_assert(ReflectNode::getClassStatic()->refMethods().find(nullptr, StringId::_block, StringId::Invalid, {}, {})->position == 1);
	chatra_assert(ReflectNode::getClassStatic()->refMethods().find(nullptr, StringId::_sub, StringId::Invalid, {}, {})->position == 2);

	auto sharedRef = reflectNode.ref(0);
	auto& shared = sharedRef.allocateWithoutLock<ReflectNodeShared>();
	shared.lines = std::move(lines);
	shared.addNode(*node);

	buildReflectNodeCallRecursive(sharedRef, reflectNode, node.get());
}

#ifndef CHATRA_NDEBUG
static bool stdoutEnabled = true;
static std::string testMode;
static bool checkFinished = false;
static unsigned checkPassedCount = 0;
static unsigned checkFailedCount = 0;
static std::mutex mtFinished;
static std::condition_variable cvFinished;

void enableStdout(bool enabled) {
	stdoutEnabled = enabled;
}

void setTestMode(const std::string& _testMode) {
	testMode = _testMode;
}

void beginCheckScript() {
	checkFinished = false;
}

void endCheckScript() {
	if (!checkFinished) {
		std::fprintf(stderr, "failed: aborted running script\n");
		std::fflush(stderr);
		checkFailedCount++;
	}
}

bool showResults() {
	auto* stream = (checkFailedCount != 0 ? stderr : stdout);
	if (stdoutEnabled || stream != stdout) {
		std::fprintf(stream, "results: passed %u, failed %u\n", checkPassedCount, checkFailedCount);
		std::fflush(stream);
	}
	return checkFailedCount == 0;
}

void waitUntilFinished() {
	std::unique_lock<std::mutex> lock(mtFinished);
	cvFinished.wait(lock, [&]{
		return checkFinished;
	});

#ifdef CHATRA_TRACE_TEMPORARY_ALLOCATION
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif
}

#endif // !CHATRA_NDEBUG

void native_check(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
#ifndef CHATRA_NDEBUG
	ret.setBool(false);

	if (getReferClass(args.ref(0)) != String::getClassStatic()) {
		std::fprintf(stderr, "failed: argument 0 of _check() is not String\n");
		std::fflush(stderr);
		return;
	}
	auto subject = args.ref(0).deref<String>().getValue();

	auto cl1 = getReferClass(args.ref(1));
	if (cl1 != Bool::getClassStatic()) {
		std::lock_guard<SpinLock> lock(thread.runtime.lockSTable);
		std::fprintf(stderr, "failed: %s; argument 1 of _check() is not Bool (found %s)\n", subject.data(),
				thread.runtime.distributedSTable->ref(cl1->getName()).data());
		std::fflush(stderr);
		return;
	}
	if (args.ref(1).getBool()) {
		if (stdoutEnabled) {
			std::printf("passed: %s\n", subject.data());
			std::fflush(stdout);
		}
		checkPassedCount++;
		ret.setBool(true);
	}
	else {
		std::fprintf(stderr, "failed: %s\n", subject.data());
		std::fflush(stderr);
		checkFailedCount++;
	}
#endif // !CHATRA_NDEBUG
}

void native_checkCmd(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
#ifndef CHATRA_NDEBUG
	if (getReferClass(args.ref(0)) != String::getClassStatic()) {
		std::fprintf(stderr, "failed: argument 0 of _check() is not String\n");
		std::fflush(stderr);
		return;
	}
	auto verb = args.ref(0).deref<String>().getValue();

	if (verb == "finished") {
		checkFinished = true;
		if (endsWith(testMode.cbegin(), testMode.cend(), "_mt"))
			cvFinished.notify_one();
		return;
	}
	if (verb == "abort") {
		chatra_assert(false);
		return;
	}
	if (verb == "testMode") {
		ret.allocate<String>().setValue(testMode);
		return;
	}
	throw InternalError();
#endif // !CHATRA_NDEBUG
}

void native_incrementTestTimer(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_ARGS_CAPTURE;
#ifndef CHATRA_NDEBUG
	auto timerId = thread.runtime.addTimer("test");
	thread.runtime.increment(timerId, args.ref(0).getInt());
#endif // !CHATRA_NDEBUG
}

}  // namespace chatra
