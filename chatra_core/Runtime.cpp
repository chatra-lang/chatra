/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2019-2020 Chatra Project Team
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

constexpr unsigned currentVersion = 200;  // major(XX).minor(XX).revision(XX)

static std::atomic<size_t> lastRuntimeId = {0};

Package::Package(RuntimeImp& runtime, std::string name, PackageInfo packageInfo, bool fromHost) noexcept
		: PackageInfo(std::move(packageInfo)),
		runtime(runtime), name(std::move(name)), scope(runtime.storage->add(ScopeType::Package)),
		fromHost(fromHost) {

	scope->add(StringId::PackageInitializer);
	postInitialize();
}

void Package::postInitialize() {
	node = std::make_shared<Node>();
	node->type = NodeType::ScriptRoot;
	node->flags |= NodeFlags::InitialNode;

	if (!interface)
		interface = std::make_shared<IPackage>();
}

static bool parseBlockNodes(ParserWorkingSet& ws, IErrorReceiverBridge& errorReceiverBridge,
		std::shared_ptr<StringTable>& sTable, Node* node) {

	try {
		if (!errorReceiverBridge.hasError() && node->blockNodesState == NodeState::Grouped)
			structureInnerNode(errorReceiverBridge, sTable, node, false);

		if (!errorReceiverBridge.hasError() && node->blockNodesState == NodeState::Structured) {
			parseInnerNode(ws, errorReceiverBridge, sTable, node, false);
			if (!errorReceiverBridge.hasError())
				return true;
		}
	}
	catch (const AbortCompilingException&) {
		errorAtNode(errorReceiverBridge, ErrorLevel::Error, node, "abort compiling", {});
	}

	node->type = ntParserError;
	node->blockNodes.clear();
	node->symbols.clear();
	return false;
}

std::shared_ptr<Node> Package::parseNode(IErrorReceiver& errorReceiver, Node* node) {
	std::shared_ptr<Node> scriptNode;
	IErrorReceiverBridge errorReceiverBridge(errorReceiver);

	if (!grouped) {
		grouped = true;

		for (auto& script : scripts) {
			try {
				auto fileLines = parseLines(errorReceiverBridge, runtime.primarySTable, script.name, 1, script.script);
				auto fileNode = groupScript(errorReceiverBridge, runtime.primarySTable, fileLines);

				// Keep reference to Line for avoiding automatic deletion by shared_ptr.
				lines.insert(lines.end(), fileLines.cbegin(), fileLines.cend());

				if (scriptNode) {
					scriptNode->blockNodes.insert(scriptNode->blockNodes.end(), fileNode->blockNodes.cbegin(), fileNode->blockNodes.cend());
					scriptNode->symbols.insert(scriptNode->symbols.end(), fileNode->symbols.cbegin(), fileNode->symbols.cend());
				}
				else
					scriptNode = std::move(fileNode);
			}
			catch (AbortCompilingException&) {
				// nothing to do
			}
		}

		if (!scriptNode) {
			scriptNode = std::make_shared<Node>();
			scriptNode->type = NodeType::ScriptRoot;
		}

		node = scriptNode.get();
	}

	if (parseBlockNodes(runtime.parserWs, errorReceiverBridge, runtime.primarySTable, node) &&
			node->type == NodeType::ScriptRoot) {
		for (auto& n : node->symbols) {
			if (n->type == NodeType::Class) {
				parseBlockNodes(runtime.parserWs, errorReceiverBridge, runtime.primarySTable, n.get());
				n->blockNodesState = NodeState::Parsed;
			}
		}
	}

	// set NodesState as Parsed even if compilation failed; this is required for suppressing further Thread::parse() call
	node->blockNodesState = NodeState::Parsed;

	return scriptNode;
}

void Package::distributeScriptNode(std::shared_ptr<Node> scriptNode) {
	std::lock_guard<SpinLock> lock0(lockNode);
	node = std::move(scriptNode);
	auto* nNew = node.get();
	for (auto* thread : threadsWaitingForNode)
		thread->frames.back().node = nNew;
	threadsWaitingForNode.clear();
}

bool Package::requiresProcessImport(IErrorReceiver& errorReceiver, const StringTable* sTable, Node* node,
		bool warnIfDuplicates) const {
	chatra_assert(node->type == NodeType::Import);

	auto sid = node->subNodes[SubNode::Import_Package]->sid;
	if (imports.count(sid) != 0) {
		if (warnIfDuplicates) {
			errorAtNode(errorReceiver, ErrorLevel::Warning, node->subNodes[SubNode::Import_Package].get(),
					"duplicated imports; ignored", {});
		}
		return false;
	}

	auto name = sTable->ref(sid);
	if (name == this->name)
		return false;

	if (node->subNodes[SubNode::Import_Alias]) {
		auto alias = node->subNodes[SubNode::Import_Alias]->sid;
		if (importsByName.count(alias) != 0) {
			errorAtNode(errorReceiver, ErrorLevel::Error, node->subNodes[SubNode::Import_Alias].get(), "duplicated alias name", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
	}

	return true;
}

Package& Package::import(Node* node, PackageId targetPackageId) {
	chatra_assert(node->type == NodeType::Import);

	auto sid = node->subNodes[SubNode::Import_Package]->sid;

	auto* targetPackage = runtime.packageIds.lockAndRef(targetPackageId);
	imports.emplace(sid);
	if (node->subNodes[SubNode::Import_Alias]) {
		auto alias = node->subNodes[SubNode::Import_Alias]->sid;
		importsByName.emplace(alias, targetPackage);
	}
	else {
		importsByName.emplace(sid, targetPackage);
		anonymousImports.emplace_back(targetPackage);
	}

	return *targetPackage;
}

static StringId getStringIdOrThrow(const StringTable* sTable, const std::string& name) {
	if (name.length() == 0)
		return StringId::Invalid;
	auto sid = sTable->find(name);
	if (sid == StringId::Invalid)
		throw RuntimeException(StringId::IllegalArgumentException);
	return sid;
}

static std::vector<NativeMethod> filterNativeMethods(
		const StringTable* sTable, const std::vector<NativeCallHandlerInfo>& handlers, StringId sidClass = StringId::Invalid) {

	std::vector<NativeMethod> ret;
	for (auto& e : handlers) {
		try {
			if (sidClass == getStringIdOrThrow(sTable, e.className)) {
				ret.emplace_back(getStringIdOrThrow(sTable, e.name), getStringIdOrThrow(sTable, e.subName),
						nativeCall, e.handler);
			}
		}
		catch (RuntimeException&) {
			// do nothing
		}
	}
	return ret;
}

void Package::build(IErrorReceiver& errorReceiver, const StringTable* sTable) {
	chatra_assert(node->blockNodesState == NodeState::Parsed);

	// Performs two-path initialization to allow cross references in
	// constructor arguments or package-global defs
	std::unordered_map<StringId, Node*> classNames;
	std::vector<Class*> classList;
	for (auto& n : node->symbols) {
		if (n->type != NodeType::Class)
			continue;

		auto it = classNames.find(n->sid);
		if (it != classNames.cend()) {
			errorAtNode(errorReceiver, ErrorLevel::Error, n.get(), "duplicated class name", {});
			errorAtNode(errorReceiver, ErrorLevel::Error, it->second, "previous declaration is here", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
		classNames.emplace(n->sid, n.get());

		classList.push_back(classes.emplace(this, n.get()));
	}

	// Register variables, classes and operator overrides
	chatra_assert(!clPackage);
	clPackage.reset(new Class(errorReceiver, sTable, *this, this, node.get(), nullptr, filterNativeMethods(sTable, handlers)));

	for (auto* cl : classList)
		cl->initialize(errorReceiver, sTable, *this, nullptr, filterNativeMethods(sTable, handlers, cl->getName()));

	for (auto& n : node->symbols) {
		if (n->type == NodeType::DefOperator) {
			addOperatorMethod(runtime.operators, errorReceiver, sTable, *this, this, n.get());
			hasDefOperator = true;
		}
	}
}

void Package::allocatePackageObject() const {
	scope->addConst(StringId::PackageObject).allocateWithoutLock<PackageObject>(clPackage.get());
}

const Class* Package::findClass(StringId name) {
	auto* cl = classes.find(name);
	if (cl != nullptr)
		return cl;

	cl = runtime.classes.find(name);
	if (cl != nullptr)
		return cl;

	for (auto* package : anonymousImports) {
		cl = package->classes.find(name);
		if (cl != nullptr)
			return cl;
	}
	return nullptr;
}

const Class* Package::findPackageClass(StringId packageName, StringId name) {
	auto it = importsByName.find(packageName);
	if (it == importsByName.end())
		return nullptr;
	return it->second->classes.find(name);
}

Package* Package::findPackage(StringId name) {
	auto it = importsByName.find(name);
	return it == importsByName.end() ? nullptr : it->second;
}

const std::vector<Package*>& Package::refAnonymousImports() const {
	return anonymousImports;
}

void Package::pushNodeFrame(Thread& thread, Package& package, size_t parentIndex, ScopeType type, size_t popCount) {
	std::lock_guard<SpinLock> lock(lockNode);
	if (node->blockNodesState != NodeState::Parsed)
		threadsWaitingForNode.emplace_back(&thread);
	thread.frames.emplace_back(thread, package, parentIndex, type, node.get(), popCount);
}

RuntimeId Package::runtimeId() const {
	return runtime.runtimeId;
}

std::vector<uint8_t> Package::saveEvent(NativeEventObject* event) const {
	if (event == nullptr)
		throw IllegalArgumentException();
	auto t = static_cast<uint64_t>(static_cast<NativeEventObjectImp*>(event)->getWaitingId());
	auto* ptr = reinterpret_cast<const uint8_t*>(&t);
	return std::vector<uint8_t>(ptr, ptr + 8);
}

NativeEventObject* Package::restoreEvent(const std::vector<uint8_t>& stream) const {
	if (stream.size() != 8)
		throw IllegalArgumentException();
	auto t = *reinterpret_cast<const uint64_t*>(stream.data());
	return new NativeEventObjectImp(runtime, static_cast<unsigned>(t));
}

IDriver* Package::getDriver(DriverType driverType) const {
	return runtime.getDriver(driverType);
}

void Package::saveScripts(Writer& w) const {
	w.out(initialized.load());
	w.out(fromHost);
	w.out(grouped);

	if (fromHost) {
		w.out(name);
		return;
	}

	w.out(scripts, [&](const Script& file) {
		// TODO Compression and removing comments
		w.out(file.name);
		w.out(file.script);
	});
}

Package::Package(RuntimeImp& runtime, Reader& r) noexcept : runtime(runtime) {
	(void)r;
}

void Package::restoreScripts(chatra::Reader& r) {
	temporaryInitialized = r.read<bool>();
	r.in(fromHost);
	r.in(grouped);

	if (fromHost) {
		r.in(name);

		auto p = runtime.host->queryPackage(name);
		if (p.scripts.empty())
			throw PackageNotFoundException();

		scripts = std::move(p.scripts);
		handlers = std::move(p.handlers);
		interface = std::move(p.interface);
	}
	else {
		r.inList([&]() {
			auto name = r.read<std::string>();
			auto script = r.read<std::string>();
			scripts.emplace_back(std::move(name), std::move(script));
		});
	}

	postInitialize();
}

void Package::save(Writer& w) const {
	w.CHATRA_OUT_POINTER(scope.get(), Scope);
	w.out(hasDefOperator);

	w.out(threadsWaitingForNode, [&](Thread* thread) {
		w.out(thread);
	});
}

void Package::restore(Reader& r) {
	scope = r.CHATRA_READ_UNIQUE(Scope);
	r.in(hasDefOperator);

	r.inList([&]() {
		threadsWaitingForNode.emplace_back(r.read<Thread*>());
	});
}

void RuntimeImp::launchStorage() {
	// Start pseudo GC thread
	storage = Storage::newInstance(this);
	gcInstance = instanceIds.allocate();
	gcThread = threadIds.allocate(*this, *gcInstance);
	chatra_assert(gcInstance->getId() == static_cast<InstanceId>(0));
	chatra_assert(gcThread->getId() == static_cast<Requester>(0));

	// Register pseudo package for finalizer
	auto _finalizerPackageId = loadPackage({std::string("(finalizer)"), ""});
	chatra_assert(_finalizerPackageId == finalizerPackageId);
	(void)_finalizerPackageId;

	auto* finalizerPackage = packageIds.ref(finalizerPackageId);
	finalizerPackage->initialized = true;

	auto& n0 = finalizerPackage->node;
	n0->blockNodesState = NodeState::Parsed;
	n0->blockNodes.emplace_back(std::make_shared<Node>());
	auto& n1 = n0->blockNodes.back();
	n1->type = ntFinalizer;

	storage->deploy();
}

void RuntimeImp::launchFinalizerThread() {
	auto _finalizerInstanceId = run(finalizerPackageId);  // global scope will be restored later
	chatra_assert(_finalizerInstanceId == finalizerInstanceId);
	(void)_finalizerInstanceId;

	finalizerThread = instanceIds.ref(finalizerInstanceId)->threads.begin()->second.get();
	chatra_assert(finalizerThread->getId() == finalizerThreadId);

	scope = storage->add(ScopeType::Global);
	scope->add(StringId::Parser);

	auto r0 = scope->addConst(StringId::FinalizerObjects);
	r0.allocate<Array>();
	scope->addExclusive(StringId::FinalizerTemporary);

	finalizerThread->frames[0].scope = scope.get();
}

void RuntimeImp::launchSystem(unsigned initialThreadCount) {
	timers.emplace("", newSystemTimer());

	multiThread = (initialThreadCount != std::numeric_limits<unsigned>::max());
	if (multiThread)
		setWorkers(initialThreadCount);
}

void RuntimeImp::shutdownThreads() {
	if (!multiThread)
		return;

	setWorkers(0);

	{
		std::unique_lock<std::mutex> lock(mtQueue);
		if (!workerThreads.empty())
			cvShutdown.wait(lock, [&]() { return workerThreads.empty(); });
	}
}

void RuntimeImp::shutdownTimers() {
	std::lock_guard<SpinLock> lock(lockTimers);
	for (auto& e : timers)
		e.second->cancelAll();
}

// ScriptRoot node -> script index (>=0 for packageList, <0 for -StringId)
static std::unordered_map<const Node*, ptrdiff_t> getEmbeddedRootMap() {
	std::unordered_map<const Node*, ptrdiff_t> rootMap;
	for (auto& e : refNodeMapForEmbeddedFunctions())
		rootMap.emplace(e.second, -static_cast<ptrdiff_t>(e.first));
	for (auto& e : refNodeMapForEmbeddedClasses())
		rootMap.emplace(e.second, -static_cast<ptrdiff_t>(e.first));
	return rootMap;
}

static void addToParentMap(std::unordered_map<const Node*, const Node*>& parentMap, std::deque<const Node*>& nodes,
		const Node* node, const std::vector<std::shared_ptr<Node>>& childNodes) {

	for (auto& n : childNodes) {
		if (!n)
			continue;
		chatra_assert(parentMap.count(n.get()) == 0);
		parentMap.emplace(n.get(), node);
		nodes.emplace_back(n.get());
	}
}

std::unordered_map<const Class*, size_t> RuntimeImp::getClassMap() const {
	std::vector<const Class*> classList;
	std::unordered_map<const Class*, size_t> classMap;

	for (auto& ec : classes.refClassMap()) {
		if (ec.second->getNode() == nullptr)
			classList.emplace_back(ec.second);
	}
	classList.emplace_back(Tuple::getClassStatic());

	std::sort(classList.begin(), classList.end(), [](const Class* a, const Class* b) {
		return static_cast<std::underlying_type<StringId>::type>(a->getName())
				< static_cast<std::underlying_type<StringId>::type>(b->getName());
	});
	for (auto* cl : classList)
		classMap.emplace(cl, classMap.size());

	return classMap;
}

void RuntimeImp::saveEntityFrames(Writer& w) {
	w.out(packageIds, [&](const Package& package) {
		(void)package;
		return true;
	}, [&](const Package& package) {
		auto packageId = package.getId();
		w.out(packageId);

		if (packageId != finalizerPackageId) {
			package.saveScripts(w);
			w.out(package.interface->savePackage(const_cast<Package&>(package)));
		}

		w.out(package.instances, [&](const decltype(package.instances)::value_type& ei) {
			w.out(ei.first);
			auto& instance = ei.second;
			w.out(instance->threads, [&](const decltype(instance->threads)::value_type& et) {
				w.out(et.first);
			});
		});
	});
}

void RuntimeImp::saveEntityMap(Writer& w) {
	// classes (only embedded classes which do not have a Node pointer)
	auto classMap = getClassMap();

	// nodes
	auto rootMap = getEmbeddedRootMap();
	packageIds.forEach([&](Package& package) {
		rootMap.emplace(package.node.get(), static_cast<ptrdiff_t>(package.getId()));
	});

	std::unordered_map<const Node*, const Node*> parentMap;  // child -> parent
	std::deque<const Node*> nodes;
	for (auto& e : rootMap)
		nodes.emplace_back(e.first);
	while (!nodes.empty()) {
		auto* node = nodes.front();
		nodes.pop_front();
		addToParentMap(parentMap, nodes, node, node->blockNodes);
		addToParentMap(parentMap, nodes, node, node->subNodes);
		addToParentMap(parentMap, nodes, node, node->annotations);
	}

	w.setEntityMap(std::move(classMap), std::move(rootMap), std::move(parentMap));
}

#define CHATRA_WRITE_OBJECT(className)  case typeId_##className:  \
		return static_cast<className*>(object)->save(w)
#define CHATRA_WRITE_OBJECT_WITH_THREAD(className)  case typeId_##className:  \
		static_cast<className*>(object)->saveThread(w); \
		return static_cast<className*>(object)->save(w)
#define CHATRA_WRITE_OBJECT_REFS(className)  case typeId_##className:  \
		static_cast<className*>(object)->saveReferences(w);  break

void RuntimeImp::saveStorage(Writer& w) const {
	storage->save(w, [&](TypeId typeId, Object* object) {
		w.saveResync(static_cast<int>(typeId));
		switch (typeId) {
		CHATRA_WRITE_OBJECT(UserObjectBase);
		case typeId_Exception:  return dynamic_cast<ExceptionBase*>(object)->save(w);
		CHATRA_WRITE_OBJECT(Tuple);
		CHATRA_WRITE_OBJECT(Async);
		CHATRA_WRITE_OBJECT(String);
		CHATRA_WRITE_OBJECT(ContainerBody);
		CHATRA_WRITE_OBJECT(Array);
		CHATRA_WRITE_OBJECT(Dict);

		CHATRA_WRITE_OBJECT_WITH_THREAD(TemporaryObject);
		CHATRA_WRITE_OBJECT_WITH_THREAD(TemporaryTuple);
		CHATRA_WRITE_OBJECT(TupleAssignmentMap);
		CHATRA_WRITE_OBJECT(FunctionObject);
		CHATRA_WRITE_OBJECT_WITH_THREAD(WaitContext);
		CHATRA_WRITE_OBJECT(PackageObject);

		default:
			throw InternalError();
		}
	}, [&](TypeId typeId, Object* object) {
		w.saveResync(static_cast<int>(typeId));
		switch (typeId) {
		CHATRA_WRITE_OBJECT_REFS(TemporaryObject);
		CHATRA_WRITE_OBJECT_REFS(TemporaryTuple);
		CHATRA_WRITE_OBJECT_REFS(WaitContext);
		default:
			break;
		}
	});
}

void RuntimeImp::saveState(Writer& w) const {

	w.out(packageIds, [&](const Package& package) {
		return package.getId() != finalizerPackageId;
	}, [&](const Package& package) {
		w.out(package.getId());
		package.save(w);
	});

	w.out(threadIds, [&](const Thread& thread) {
		return &thread != gcThread.get();
	}, [&](const Thread& thread) {
		w.out(thread.getId());
		thread.save(w);
	});

	w.CHATRA_OUT_POINTER(scope.get(), Scope);
	w.out(recycledRefs, [&](const Reference& ref) { w.out(ref); });

	w.out(packageIdByName, [&](const std::pair<std::string, PackageId>& e) {
		w.out(e.first);
		w.out(e.second);
	});

	w.out(queue, [&](const Thread* thread) {
		w.out(thread);
	});

	w.out(waitingThreads, [&](const std::pair<unsigned, Thread*>& e) {
		w.out(e.first);
		w.out(e.second);
	});

	std::unordered_map<const Timer*, std::string> timerToName;
	w.out(timers, [&](const std::pair<const std::string, std::unique_ptr<Timer>>& e) {
		w.out(e.first);
		if (e.first.empty())
			return;
		w.out(e.second->getTime().count());
		timerToName.emplace(e.second.get(), e.first);
	});
	w.out(idToTimer, [&](const Timer* timer) {
		w.out(timerToName.at(timer));
	});
	w.out(sleepRequests, [&](const std::pair<unsigned, std::tuple<std::string, int64_t>>& e) {
		w.out(e.first);
		w.out(std::get<0>(e.second));
		w.out(std::get<1>(e.second));
	});
}

void RuntimeImp::restoreEntityFrames(Reader& r) {
	r.inList([&]() {
		auto packageId = r.read<PackageId>();

		Package* package;
		if (packageId == finalizerPackageId)
			package = packageIds.lockAndRef(packageId);
		else {
			auto packagePtr = packageIds.allocateWithId(packageId, *this, r);
			package = packagePtr.get();

			package->restoreScripts(r);
			package->interface->restorePackage(*package, r.read<std::vector<uint8_t>>());

			if (!package->name.empty())
				packageIdByName.emplace(package->name, packageId);
			packages.emplace(packageId, std::move(packagePtr));
		}

		r.inList([&]() {
			auto instanceId = r.read<InstanceId>();
			auto instancePtr = instanceIds.allocateWithId(instanceId, packageId);
			auto& instance = *instancePtr;

			package->instances.emplace(instanceId, std::move(instancePtr));

			r.inList([&]() {
				auto threadId = r.read<Requester>();
				auto threadPtr = threadIds.allocateWithId(threadId, *this, instance, r);

				instance.threads.emplace(threadId, std::move(threadPtr));
			});
		});
	});
}

void RuntimeImp::restoreEntityMap(Reader& r) {
	auto classMap = getClassMap();
	auto rootMap = getEmbeddedRootMap();

	// system classes which *has* a node pointer
	std::vector<const Class*> systemClasses;
	for (auto& ec : classes.refClassMap()) {
		if (ec.second->getNode() != nullptr)
			systemClasses.emplace_back(ec.second);
	}

	// system methods
	std::vector<const Method*> systemMethods;
	methods.forEach([&](const Method& method) {
		systemMethods.emplace_back(&method);
	});

	r.setEntityMap(classMap, rootMap, systemClasses, systemMethods);
}

void RuntimeImp::restoreEntities(Reader& r, PackageId packageId, Node* node) {
	auto* package = packageIds.ref(packageId);

	if (node->type == NodeType::ScriptRoot) {
		// Find dependency chain and initialize all of corresponding packages;
		// This is partially same logic which Thread::initializePackage() does.
		std::vector<std::tuple<Node*, PackageId, PackageId>> targetPackages;
		std::deque<PackageId> hostPackages(1, packageId);
		while (!hostPackages.empty()) {
			auto hostPackageId = hostPackages.front();
			hostPackages.pop_front();

			targetPackages.emplace_back(nullptr, hostPackageId, hostPackageId);

			for (auto& n : restorePackageNode(r, hostPackageId, false)->blockNodes) {
				if (n->type != NodeType::Import)
					continue;

				auto sid = n->subNodes[SubNode::Import_Package]->sid;
				auto it = packageIdByName.find(primarySTable->ref(sid));
				if (it == packageIdByName.cend())
					break;
				auto targetPackageId = it->second;

				auto* hostPackage = package;
				if (hostPackageId != packageId)
					hostPackage = packageIds.ref(hostPackageId);
				if (!hostPackage->requiresProcessImport(*this, primarySTable.get(), n.get(), false))
					continue;

				targetPackages.emplace_back(n.get(), hostPackageId, targetPackageId);
				hostPackages.emplace_back(targetPackageId);
			}
		}

		for (auto it = targetPackages.crbegin(); it != targetPackages.crend(); it++) {
			auto hostPackageId = std::get<1>(*it);
			auto targetPackageId = std::get<2>(*it);

			auto* hostPackage = packageIds.ref(hostPackageId);

			if (hostPackageId != targetPackageId) {
				hostPackage->import(std::get<0>(*it), targetPackageId);
				continue;
			}

			if (!hostPackage->temporaryInitialized)
				continue;
			if (hostPackage->initialized)
				continue;

			hostPackage->build(*this, primarySTable.get());
			hostPackage->initialized = true;

			r.add(hostPackage->clPackage.get());
			for (auto& e : hostPackage->classes.refClassMap())
				r.add(e.second);
		}

		return;
	}
}

#define CHATRA_READ_OBJECT(className)  case typeId_##className:  \
		return static_cast<className*>(*object = new className(r))->restore(r)
#define CHATRA_READ_OBJECT_WITH_THREAD(className)  case typeId_##className:  \
		return static_cast<className*>(*object = new className(*r.readValidThread(), r))->restore(r)
#define CHATRA_READ_OBJECT_REFS(className)  case typeId_##className:  \
		static_cast<className*>(object)->restoreReferences(r);  break

template <class Type>
struct RestoreExceptionPredicate final {
	void operator()(Object** object, Storage& storage) {
		*object = new Type(storage);
	}
};

void RuntimeImp::restoreStorage(Reader& r) const {
	storage->restore(r, [&](TypeId typeId, Object** object) {
		r.restoreResync(static_cast<int>(typeId));
		switch (typeId) {
		case typeId_UserObjectBase: {
			auto* userObjectBase = new UserObjectBase(r.read<Class*>());
			*object = userObjectBase;
			return userObjectBase->restore(r);
		}

		case typeId_Exception:
			switchException<RestoreExceptionPredicate>(r.read<StringId>(), object, *storage);
			return true;

		CHATRA_READ_OBJECT(Tuple);
		CHATRA_READ_OBJECT(Async);
		CHATRA_READ_OBJECT(String);
		CHATRA_READ_OBJECT(ContainerBody);
		CHATRA_READ_OBJECT(Array);
		CHATRA_READ_OBJECT(Dict);

		CHATRA_READ_OBJECT_WITH_THREAD(TemporaryObject);
		CHATRA_READ_OBJECT_WITH_THREAD(TemporaryTuple);
		CHATRA_READ_OBJECT(TupleAssignmentMap);
		CHATRA_READ_OBJECT(FunctionObject);
		CHATRA_READ_OBJECT_WITH_THREAD(WaitContext);

		case typeId_PackageObject:
			*object = new PackageObject(r.read<Class*>());
			return false;

		default:
			throw InternalError();
		}
	}, [&](TypeId typeId, Object* object) {
		r.restoreResync(static_cast<int>(typeId));
		switch (typeId) {
		CHATRA_READ_OBJECT_REFS(UserObjectBase);
		CHATRA_READ_OBJECT_REFS(TemporaryObject);
		CHATRA_READ_OBJECT_REFS(TemporaryTuple);
		CHATRA_READ_OBJECT_REFS(WaitContext);
		default:
			break;
		}
	});
}

void RuntimeImp::restoreState(Reader& r) {

	r.inList([&]() {
		packageIds.ref(r.read<PackageId>())->restore(r);
	});

	r.inList([&]() {
		auto* thread = threadIds.ref(r.read<Requester>());
		thread->restore(r);
		thread->postInitialize(r);
	});

	scope = r.CHATRA_READ_UNIQUE(Scope);
	r.inList([&]() { recycledRefs.emplace_back(r.readReference()); });

	r.inList([&]() {
		auto name = r.read<std::string>();
		auto packageId = r.read<PackageId>();
		packageIdByName.emplace(std::move(name), packageId);
	});

	r.inList([&]() {
		queue.emplace_back(r.read<Thread*>());
	});

	unsigned maxWaitingId = 0;
	r.inList([&]() {
		auto waitingId = r.read<unsigned>();
		auto* thread = r.read<Thread*>();
		waitingThreads.emplace(waitingId, thread);
		maxWaitingId = std::max(maxWaitingId, waitingId);
	});
	if (!waitingThreads.empty()) {
		hasWaitingThreads = true;
		std::vector<bool> usedWaitingIds(static_cast<size_t>(maxWaitingId) + 1, false);
		for (auto& e : waitingThreads)
			usedWaitingIds[e.first] = true;
		for (size_t i = 0; i < usedWaitingIds.size(); i++) {
			if (!usedWaitingIds[i])
				recycledWaitingIds.push_back(static_cast<unsigned>(i));
		}
	}

	r.inList([&]() {
		auto name = r.read<std::string>();
		if (name.empty())
			return;
		auto count = r.read<Time::rep>();

		auto timer = newEmulatedTimer();
		timer->increment(Time(count));
		timers.emplace(std::move(name), std::move(timer));
	});
	r.inList([&]() {
		idToTimer.emplace_back(timers.at(r.read<std::string>()).get());
	});
	r.inList([&]() {
		auto waitingId = r.read<unsigned>();
		auto name = r.read<std::string>();
		auto timeout = r.read<int64_t>();
		sleepRequests.emplace(waitingId, std::make_tuple(std::move(name), timeout));
	});

	packageIds.forEach([&](Package& package) {
		if (!package.initialized && package.hasDefOperator)
			restorePackageNode(r, package.getId(), false);
	});
}

void RuntimeImp::reactivateFinalizerThread() {
	finalizerThread = threadIds.lockAndRef(finalizerThreadId);
}

void RuntimeImp::reactivateThreads() {
	std::deque<Thread*> queueCopy;
	std::swap(queue, queueCopy);
	for (auto* thread : queueCopy)
		enqueue(thread);
}

void RuntimeImp::reactivateTimers() {
	decltype(sleepRequests) sleepRequestsCopy(sleepRequests.cbegin(), sleepRequests.cend());
	for (auto& e : sleepRequestsCopy)
		issueTimer(e.first, *timers.at(std::get<0>(e.second)), Time(std::get<1>(e.second)));
}

void RuntimeImp::checkGc() {
	if (gcWaitCount-- != 0)
		return;

	{
		std::lock_guard<std::mutex> lock(mtGc);
		if (storage->tidy(*this))
			storage->collect(gcThread->getId());
	}

	gcWaitCount = 100 * (multiThread ? std::max(1U, targetWorkerThreads) : 1);
}

void RuntimeImp::fullGc() {
	std::lock_guard<std::mutex> lock(mtGc);

	// Should run tidy()-collect() cycle TWICE because some objects might not be marked as unused
	// if the objects was on Scope which was created after current tidy() process starts (initiated by checkGc()).
	for (size_t i = 0; i < 2; i++) {
		while (!storage->tidy(*this))
			;
		storage->collect(gcThread->getId());
	}
}

size_t RuntimeImp::stepCountForScope(size_t totalScopeCount) {
	// printf("gc_scope %u\n", static_cast<unsigned>(totalScopeCount));
	return std::max<size_t>(16, totalScopeCount >> 4U);
}

size_t RuntimeImp::stepCountForMarking(size_t totalObjectCount) {
	// printf("gc_mark %u\n", static_cast<unsigned>(totalObjectCount));
	return std::max<size_t>(64, totalObjectCount >> 4U);
}

size_t RuntimeImp::stepCountForSweeping(size_t totalObjectCount) {
	// printf("gc_sweep %u\n", static_cast<unsigned>(totalObjectCount));
	return std::max<size_t>(64, totalObjectCount >> 4U);
}

bool RuntimeImp::distributeStringTable(unsigned oldVersion) {
	if (oldVersion != UINT_MAX && oldVersion == primarySTable->getVersion())
		return false;

	primarySTable->clearDirty();
	{
		std::lock_guard<SpinLock> lock0(lockSTable);
		distributedSTable = primarySTable->copy();
	}
	threadIds.forEach([&](Thread& thread) {
		thread.hasNewSTable = true;
	});

	return true;
}

Thread& RuntimeImp::createThread(Instance& instance, Package& package, Node* node) {
	auto thread = threadIds.lockAndAllocate(*this, instance);
	thread->postInitialize();
	thread->frames.reserve(16);
	thread->frames.emplace_back(*thread, package, SIZE_MAX, scope.get());
	thread->frames.emplace_back(*thread, package, 0, thread->scope.get());
	thread->frames.emplace_back(*thread, package, 1, package.scope.get());

	// Keep global and thread frame (=residentialFrameCount)
	if (node == nullptr)
		package.pushNodeFrame(*thread, package, 2, ScopeType::ScriptRoot, 2);
	else
		thread->frames.emplace_back(*thread, package, 2, ScopeType::ScriptRoot, node, 2);

	auto& f = thread->frames.back();
	f.phase = Phase::ScriptRoot_Initial;

	Thread& ret = *thread;
	{
		std::lock_guard<SpinLock> lock(instance.lockThreads);
		instance.threads.emplace(thread->getId(), std::move(thread));
	}
	return ret;
}

void RuntimeImp::enqueue(Thread* thread) {
	chatra_assert(thread != nullptr);

	/*std::printf("enqueue: instanceId %u, threadId = %u; frames.size = %u\n",
			static_cast<unsigned>(thread->instance.getId()),
			static_cast<unsigned>(thread->getId()),
			static_cast<unsigned>(thread->frames.size()));
	std::fflush(stdout);*/

	/*{
		static unsigned count = 0;
		static constexpr unsigned breakCount = std::numeric_limits<unsigned>::max();  // std::numeric_limits<unsigned>::max()
		std::unique_lock<std::mutex> lock(mtQueue);
		count++;
		if (count == breakCount)
			chatra_assert(false);
		for (auto* t : queue)
			chatra_assert(t != thread);
	}*/

	if (multiThread) {
		std::unique_lock<std::mutex> lock(mtQueue);
		queue.push_back(thread);
		cvQueue.notify_one();
	}
	else {
		std::lock_guard<SpinLock> lock(lockQueue);
		queue.push_back(thread);
	}
}

unsigned RuntimeImp::pause(Thread* thread) {
	hasWaitingThreads = true;
	std::lock_guard<SpinLock> lock(lockWaitingThreads);
	unsigned id;
	if (recycledWaitingIds.empty())
		id = static_cast<unsigned>(waitingThreads.size());
	else {
		id = recycledWaitingIds.back();
		recycledWaitingIds.pop_back();
	}
	waitingThreads.emplace(id, thread);
	return id;
}

void RuntimeImp::resume(unsigned waitingId) {
	Thread* thread;
	{
		std::lock_guard<SpinLock> lock(lockWaitingThreads);
		auto it = waitingThreads.find(waitingId);
		chatra_assert(it != waitingThreads.end());
		thread = it->second;
	}

	// see Thread::invokeNativeMethod()
	if (std::this_thread::get_id() == thread->nativeCallThreadId)
		thread->pauseRequested = false;
	else {
		std::lock_guard<SpinLock> lock(thread->lockNative);
		enqueue(thread);
	}

	{
		std::lock_guard<SpinLock> lock(lockWaitingThreads);
		waitingThreads.erase(waitingId);
		recycledWaitingIds.emplace_back(waitingId);
		hasWaitingThreads = !waitingThreads.empty();
	}
}

void RuntimeImp::issueTimer(unsigned waitingId, Timer& timer, Time time) {
	auto* runtime = this;
	timer.at(time, [runtime, waitingId]() {
		if (runtime->attemptToShutdown)
			return;

		{
			std::lock_guard<SpinLock> lock(runtime->lockTimers);
			runtime->sleepRequests.erase(waitingId);
		}

		runtime->resume(waitingId);
	});
}

IDriver* RuntimeImp::getDriver(DriverType driverType) {
	std::lock_guard<SpinLock> lock(lockDrivers);
	auto it = drivers.find(driverType);
	IDriver* driver = nullptr;
	if (it == drivers.cend())
		driver = drivers.emplace(driverType, host->queryDriver(driverType)).first->second.get();
	else
		driver = it->second.get();
	if (driver == nullptr)
		throw UnsupportedOperationException();
	return driver;
}

std::string RuntimeImp::formatOrigin(const std::string& fileName, unsigned lineNo) {
	std::string out;
	if (fileName.empty())
		out.append("unknown file");
	else {
		out.append(fileName);
		switch (lineNo) {
		case WholeFile:  break;
		case UnknownLine:  out.append(":unknown line");  break;
		default:  out.append(":").append(std::to_string(lineNo));  break;
		}
	}
	return out;
}

std::string RuntimeImp::formatError(ErrorLevel level,
		const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
		const std::string& message, const std::vector<std::string>& args, bool outputExtraMessagePlaceholder) {

	std::string out = formatOrigin(fileName, lineNo);
	out.append(":");

	std::string outMessage = message;
	if (!args.empty()) {
		for (size_t i = 0; i < args.size(); i++) {
			std::string target = "${" + std::to_string(i) + "}";
			size_t index = 0;
			while (index < outMessage.size()) {
				auto position = outMessage.find(target, index);
				if (position == std::string::npos)
					break;
				outMessage.replace(position, target.size(), args[i]);
				index = position + target.size();
			}
		}
	}

	out.append(" ");
	switch (level) {
	case ErrorLevel::Info:  out.append("info");  break;
	case ErrorLevel::Warning:  out.append("warning");  break;
	case ErrorLevel::Error:  out.append("error");  break;
	}

	out.append(": ").append(outMessage);
	if (outputExtraMessagePlaceholder)
		out.append("${0}");  // Placeholder for "+extra N errors"

	if (!line.empty()) {
		out.append(":\n\t");

		auto skip = static_cast<size_t>(std::distance(line.cbegin(), std::find_if(line.cbegin(), line.cend(), isNotSpace)));

		if (first == SIZE_MAX || last == SIZE_MAX)
			out.append(line.substr(skip));
		else {
			chatra_assert(first <= last && last <= line.size());
			skip = std::min(skip, first);

			auto validLength = line.find("//", last);
			if (validLength == std::string::npos)
				validLength = line.size();

			bool hasTailingText = (line.cbegin() + validLength
					!= std::find_if(line.cbegin() + last, line.cbegin() + validLength, isNotSpace));

			if (first == skip && !hasTailingText)
				out.append(line.substr(skip));
			else {
				out.append(line.substr(skip, first - skip))
						.append(" *HERE* ->").append(line.substr(first, last - first)).append("<- ")
						.append(line.substr(last));
			}
		}
	}

	out.append("\n");
	return out;
}

void RuntimeImp::error(ErrorLevel level,
		const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
		const std::string& message, const std::vector<std::string>& args) {
	outputError(formatError(level, fileName, lineNo, line, first, last, message, args));
}

void RuntimeImp::outputError(const std::string& message) const {
	host->console(message);
}

Thread* RuntimeImp::popDebuggableThread(debugger::ThreadId threadId) {
	if (!paused)
		throw debugger::IllegalRuntimeStateException("Runtime is not paused");

	auto requester = static_cast<Requester>(static_cast<size_t>(threadId));

	Thread* thread = threadIds.lockAndRef(requester);
	if (thread == nullptr)
		throw IllegalArgumentException("specified threadId(%zu) is not found", static_cast<size_t>(threadId));

	{
		std::lock_guard<SpinLock> lock0(lockQueue);
		auto it = std::find(queue.cbegin(), queue.cend(), thread);
		if (it == queue.cend())
			return nullptr;
		queue.erase(it);
	}

	return thread;
}

template <typename ContinueCond>
debugger::StepRunResult RuntimeImp::stepRun(Thread* thread, ContinueCond continueCond) {
	auto threadId = thread->getId();

	checkGc();
	thread->captureStringTable();

	while (continueCond()) {
		switch (thread->stepRun()) {
		case Thread::StepRunResult::Abort:
			thread->finish();
			return debugger::StepRunResult::Finished;
		case Thread::StepRunResult::Continue:
			continue;
		case Thread::StepRunResult::Next:
			break;
		case Thread::StepRunResult::BreakPoint:
			return debugger::StepRunResult::BreakPoint;
		}

		if (threadIds.lockAndRef(threadId) == nullptr)
			return debugger::StepRunResult::Finished;

		{
			std::lock_guard<SpinLock> lock0(lockQueue);
			auto it = std::find(queue.cbegin(), queue.cend(), thread);
			if (it != queue.cend()) {
				queue.erase(it);
				continue;
			}
		}

		{
			std::lock_guard<SpinLock> lock1(lockWaitingThreads);
			auto it = std::find_if(waitingThreads.cbegin(), waitingThreads.cend(), [&](const std::pair<unsigned, Thread*>& e) {
				return e.second == thread;
			});
			if (it != waitingThreads.cend())
				return debugger::StepRunResult::Blocked;
		}

		return debugger::StepRunResult::WaitingResources;
	}

	enqueue(thread);
	return debugger::StepRunResult::Stopped;
}

debugger::CodePoint RuntimeImp::nodeToCodePoint(PackageId packageId, Node* node) {
	if (node == nullptr)
		return debugger::CodePoint();

	auto line = (node->tokens.empty() ? node->line : node->tokens[0]->line.lock());
	if (!line)
		return debugger::CodePoint();

	debugger::CodePoint ret;

	{
		std::lock_guard<decltype(packageIds)> lock0(packageIds);
		if (!packageIds.has(packageId))
			throw IllegalArgumentException();
		ret.packageName = packageIds.ref(packageId)->name;
	}

	ret.fileName = line->fileName;
	ret.lineNo = line->lineNo;
	return ret;
}

static bool findNode(const debugger::CodePoint& point, Node* node, std::vector<Node*>& path_r) {
	auto line = (node->tokens.empty() ? node->line : node->tokens[0]->line.lock());
	if (line && line->lineNo == point.lineNo && line->fileName == point.fileName) {
		path_r.emplace_back(node);
		return true;
	}

	for (auto& n : node->blockNodes) {
		if (findNode(point, n.get(), path_r)) {
			path_r.emplace_back(node);
			return true;
		}
	}

	return false;
}

void RuntimeImp::adjustFramePhase(Node* node, size_t baseIndex, ptrdiff_t delta) const {
	threadIds.forEach([&](Thread& thread) {
		for (auto& f : thread.frames) {
			if (f.node == node && f.phase <= f.node->blockNodes.size() && f.phase >= baseIndex)
				f.phase += delta;
		}
	});
}

void RuntimeImp::applyBreakPoint(BreakPoint& bp) {
	Package* p = nullptr;
	packageIds.forEach([&](Package& package) {
		if (package.name != bp.point.packageName)
			return;
		for (auto& s : package.scripts) {
			if (s.name == bp.point.fileName) {
				p = &package;
				break;
			}
		}
	});
	if (p == nullptr)
		throw IllegalArgumentException();

	IErrorReceiverBridge errorReceiverBridge(*this);
	unsigned sTableVersion = primarySTable->getVersion();

	if (!p->grouped) {
		auto scriptNode = p->parseNode(errorReceiverBridge, nullptr);
		if (errorReceiverBridge.hasError())
			throw debugger::ParserErrorAroundBreakPointException();
		p->distributeScriptNode(std::move(scriptNode));
	}

	Node* node;
	{
		std::lock_guard<SpinLock> lock0(p->lockNode);
		node = p->node.get();
	}

	std::vector<Node*> path;
	for (;;) {
		path.clear();
		if (!findNode(bp.point, node, path)) {
			distributeStringTable(sTableVersion);
			throw debugger::NonBreakableStatementException();
		}

		auto it = std::find_if(path.crbegin(), path.crend(), [&](Node* n) {
			return n->blockNodesState != NodeState::Parsed;
		});
		if (it == path.crend())
			break;

		p->parseNode(errorReceiverBridge, *it);
		if (errorReceiverBridge.hasError())
			throw debugger::ParserErrorAroundBreakPointException();
	}

	distributeStringTable(sTableVersion);

	switch (path[0]->type) {
	case NodeType::Expression:
	case NodeType::Touch:
	case NodeType::Switch:
	case NodeType::Do:
	case NodeType::Break:
	case NodeType::Continue:
	case NodeType::Return:
	case NodeType::Throw: {
		bp.type = BreakPointType::Statement;
		bp.node0 = path[0];
		bp.node1 = path[1];

		auto it = std::find_if(path[1]->blockNodes.cbegin(), path[1]->blockNodes.cend(), [&](const std::shared_ptr<Node>& n) {
			return n.get() == path[0];
		});
		chatra_assert(it != path[1]->blockNodes.cend());

		adjustFramePhase(path[1], std::distance(path[1]->blockNodes.cbegin(), it), +1);

		auto* n = path[1]->blockNodes.insert(it, std::make_shared<Node>())->get();
		n->type = ntBreakPointStatement;
		n->blockNodesState = NodeState::Parsed;
		break;
	}

	case NodeType::Def:
	case NodeType::DefOperator:
	case NodeType::Class:
	case NodeType::Case:
	case NodeType::Default:
	case NodeType::Catch:
	case NodeType::Finally: {
		bp.type = BreakPointType::InnerBlock;
		bp.node0 = path[0];
		chatra_assert(path[0]->blockNodesState == NodeState::Parsed);

		adjustFramePhase(path[0], 0, +1);

		auto* n = path[0]->blockNodes.insert(path[0]->blockNodes.cbegin(), std::make_shared<Node>())->get();
		n->type = ntBreakPointInnerBlock;
		n->blockNodesState = NodeState::Parsed;
		break;
	}

	case NodeType::If:
	case NodeType::ElseIf:
	case NodeType::Else: {
		bp.type = BreakPointType::IfGroup;
		bp.node0 = path[0];
		bp.node1 = path[1];  // IfGroup

		switch (path[0]->type) {
		case NodeType::If:  path[1]->type = ntBreakPointIfGroup;  break;
		case NodeType::ElseIf:  path[0]->type = ntBreakPointElseIf;  break;
		case NodeType::Else:  path[0]->type = ntBreakPointElse;  break;
		default:
			throw InternalError();
		}
		break;
	}

	default: {
		NodeType nodeType;
		switch (path[0]->type) {
		case NodeType::For:  bp.type = BreakPointType::For;  nodeType = ntBreakPointFor;  break;
		case NodeType::While:  bp.type = BreakPointType::While;  nodeType = ntBreakPointWhile;  break;
		default:
			throw debugger::NonBreakableStatementException();
		}
		bp.node0 = path[0];
		path[0]->type = nodeType;
		break;
	}
	}
}

void RuntimeImp::cancelBreakPoint(BreakPoint& bp) const {
	switch (bp.type) {
	case BreakPointType::Invalid:
		return;
	case BreakPointType::Statement: {
		auto it = std::find_if(bp.node1->blockNodes.cbegin(), bp.node1->blockNodes.cend(), [&](const std::shared_ptr<Node>& n) {
			return n.get() == bp.node0;
		});
		chatra_assert(it != bp.node1->blockNodes.cbegin() && it != bp.node1->blockNodes.cend());

		adjustFramePhase(bp.node1, std::distance(bp.node1->blockNodes.cbegin(), it), -1);

		bp.node1->blockNodes.erase(it - 1);
		break;
	}

	case BreakPointType::InnerBlock: {
		adjustFramePhase(bp.node0, 1, -1);
		bp.node0->blockNodes.erase(bp.node0->blockNodes.cbegin());
		break;
	}

	case BreakPointType::IfGroup: {
		if (bp.node0->type == NodeType::If)
			bp.node1->type = NodeType::IfGroup;
		else if (bp.node0->type == ntBreakPointElseIf)
			bp.node0->type = NodeType::ElseIf;
		else if (bp.node0->type == ntBreakPointElse)
			bp.node0->type = NodeType::Else;
		else
			throw InternalError();
		break;
	}

	default: {
		NodeType nodeType;
		switch (bp.type) {
		case BreakPointType::For:  nodeType = NodeType::For;  break;
		case BreakPointType::While:  nodeType = NodeType::While;  break;
		default:
			throw InternalError();
		}
		bp.node0->type = nodeType;
		break;
	}
	}
}

void RuntimeImp::debugBreak(Node* node0, Thread* thread) {
	debugger::BreakPointId breakPointId;
	debugger::IDebuggerHost* debuggerHost;
	{
		std::lock_guard<std::mutex> lock0(lockDebugger);
		if (paused)
			throw debugger::IllegalRuntimeStateException();

		auto it = std::find_if(breakPoints.cbegin(), breakPoints.cend(), [&](decltype(breakPoints)::const_reference e) {
			return e.second.node0 == node0;
		});
		if (it == breakPoints.cend())
			throw InternalError();
		breakPointId = it->first;

		if (multiThread) {
			{
				std::unique_lock<std::mutex> lock1(mtQueue);
				previousWorkerThreads = targetWorkerThreads;
			}

			if (previousWorkerThreads != 1) {
				setWorkers(1);
				{
					std::unique_lock<std::mutex> lock(mtQueue);
					if (!workerThreads.empty())
						cvShutdown.wait(lock, [&]() { return workerThreads.size() == 1; });
				}
			}
			setWorkers(0);
		}
		paused = true;
		debuggerHost = this->debuggerHost.get();
	}

	chatra_assert(thread != nullptr);
	enqueue(thread);

	if (debuggerHost != nullptr)
		debuggerHost->onBreakPoint(breakPointId);
}


RuntimeImp::RuntimeImp(std::shared_ptr<IHost> host) noexcept
		: runtimeId(static_cast<RuntimeId>(lastRuntimeId.fetch_add(1))),
		host(std::move(host)), methods(MethodTable::ForEmbeddedMethods()) {
}

void RuntimeImp::setSelf(std::shared_ptr<RuntimeImp>& self) {
	this->self = self;
}

void RuntimeImp::initialize(unsigned initialThreadCount) {
	registerEmbeddedClasses(classes);
	registerEmbeddedFunctions(methods, operators);

	primarySTable = StringTable::newInstance();
	distributedSTable = primarySTable->copy();

	launchStorage();
	launchFinalizerThread();
	launchSystem(initialThreadCount);
}

void RuntimeImp::initialize(unsigned initialThreadCount, const std::vector<uint8_t>& savedState) {
	multiThread = (initialThreadCount != std::numeric_limits<unsigned>::max());

	registerEmbeddedClasses(classes);
	registerEmbeddedFunctions(methods, operators);

	Reader r(*this);
	r.parse(currentVersion, savedState);

	try {
		primarySTable = StringTable::newInstance(r.select("sTable"));
		distributedSTable = primarySTable->copy();

		// Restore the world
		launchStorage();

		restoreEntityFrames(r.select("entity"));
		restoreEntityMap(r.select("map"));
		restoreStorage(r.select("storage"));
		restoreState(r.select("state"));
	}
	catch (...) {
		throw IllegalArgumentException();
	}

	chatra_assert(storage->audit());
	chatra_assert(!primarySTable->isDirty());

	reactivateFinalizerThread();

	launchSystem(initialThreadCount);
	reactivateThreads();
	reactivateTimers();
}

Node* RuntimeImp::restorePackageNode(Reader& r, PackageId packageId, bool initialNode) {
	auto* package = packageIds.ref(packageId);
	auto* node = package->node.get();
	if (initialNode || node->blockNodesState == NodeState::Parsed)
		return node;

	if (!package->grouped)
		return node;
	package->grouped = false;

	IErrorReceiverBridge errorReceiverBridge(*this);
	auto scriptNode = package->parseNode(errorReceiverBridge, node);
	if (errorReceiverBridge.hasError())
		throw IllegalArgumentException();

	chatra_assert(scriptNode);
	package->node = std::move(scriptNode);
	restoreEntities(r, packageId, package->node.get());
	return package->node.get();
}

void RuntimeImp::restoreNode(Reader& r, PackageId packageId, Node* node) {
	if (node->blockNodesState == NodeState::Parsed)
		return;

	IErrorReceiverBridge errorReceiverBridge(*this);
	try {
		auto n = packageIds.ref(packageId)->parseNode(errorReceiverBridge, node);
		n.reset();
		if (errorReceiverBridge.hasError())
			throw AbortCompilingException();
	}
	catch (AbortCompilingException&) {
		errorAtNode(*this, ErrorLevel::Error, node, "an unrecoverable parser error encountered during restoration", {});
		throw IllegalArgumentException();
	}

	restoreEntities(r, packageId, node);
}

const MethodTable* RuntimeImp::restoreMethodTable(PackageId packageId, Node* node) {
	auto* package = packageIds.ref(packageId);
	return methodTableCache.scanInnerFunctions(this, primarySTable.get(), *package, node);
}

std::vector<uint8_t> RuntimeImp::doShutdown(bool save) {
	if (closed)
		throw UnsupportedOperationException();
	closed = true;

	if (save && parserErrorRaised) {
		errorAtNode(*this, ErrorLevel::Warning, nullptr, "ParserErrorException raised during execution; "
				"sometimes this breaks serialization mechanism and will cause fatal error during restoration", {});
	}

	if (save)
		fullGc();

	shutdownThreads();

	packageIds.forEach([&](Package& package) {
		package.interface->attemptToShutdown(package, save);
	});

	attemptToShutdown = true;

	shutdownTimers();

	if (save) {
		chatra_assert(storage->audit());

		Writer w(*this);
		primarySTable->save(w.select("sTable"));

		saveEntityFrames(w.select("entity"));
		saveEntityMap(w.select("map"));
		saveStorage(w.select("storage"));
		saveState(w.select("state"));

		return w.getBytes(currentVersion);
	}

	return {};
}

RuntimeImp::~RuntimeImp() {
	if (!closed)
		(void)doShutdown(false);

	gcThread.reset();
	gcInstance.reset();
}

std::vector<uint8_t> RuntimeImp::shutdown(bool save) {
	return doShutdown(save);
}

void RuntimeImp::setWorkers(unsigned threadCount) {
	if (!multiThread)
		return;

	std::unique_lock<std::mutex> lock0(mtQueue);
	targetWorkerThreads = threadCount;
	if (workerThreads.size() > threadCount) {
		cvQueue.notify_all();
		return;
	}

	for (; workerThreads.size() < threadCount; nextId++) {
		workerThreads.emplace(nextId, std::unique_ptr<std::thread>(new std::thread([&, this](unsigned wsId) {
			auto detach = [&]() {
				auto it = workerThreads.find(wsId);
				it->second->detach();
				workerThreads.erase(it);
				cvShutdown.notify_one();
			};

			for (;;) {
				Thread* thread;
				{
					std::unique_lock<std::mutex> lock1(mtQueue);
					cvQueue.wait(lock1, [&]() {
						return !queue.empty() || workerThreads.size() > targetWorkerThreads;
					});
					if (workerThreads.size() > targetWorkerThreads) {
						detach();
						return;
					}
					thread = queue.front();
					queue.pop_front();
				}

				try {
					if (thread->run() == Thread::RunResult::Stop) {
						detach();
						return;
					}
				}
				catch (...) {
					outputError("fatal: uncaught exception raised\n");
				}
			}
		}, nextId)));
	}
	cvQueue.notify_all();
}

bool RuntimeImp::handleQueue() {
	if (multiThread)
		return false;
	Thread* thread;
	{
		std::lock_guard<SpinLock> lock(lockQueue);
		if (queue.empty())
			return hasWaitingThreads;
		thread = queue.front();
		queue.pop_front();
	}
	thread->run();
	return true;
}

void RuntimeImp::loop() {
	while (handleQueue())
		;
}

PackageId RuntimeImp::loadPackage(const Script& script) {
	std::vector<Script> scripts = {script};
	return loadPackage(scripts);
}

PackageId RuntimeImp::loadPackage(const std::vector<Script>& scripts) {
	auto package = packageIds.lockAndAllocate(*this, "", PackageInfo{scripts, {}, nullptr}, false);
	auto packageId = package->getId();

	std::lock_guard<SpinLock> lock(lockPackages);
	packages.emplace(packageId, std::move(package));
	return packageId;
}

PackageId RuntimeImp::loadPackage(const std::string& packageName) {
	std::lock_guard<std::mutex> lock0(mtLoadPackage);
	{
		std::lock_guard<SpinLock> lock1(lockPackages);
		auto it = packageIdByName.find(packageName);
		if (it != packageIdByName.end())
			return it->second;
	}
	auto p = host->queryPackage(packageName);
	if (p.scripts.empty())
		throw PackageNotFoundException();

	auto package = packageIds.lockAndAllocate(*this, packageName, std::move(p), true);
	auto packageId = package->getId();

	std::lock_guard<SpinLock> lock(lockPackages);
	packages.emplace(packageId, std::move(package));
	packageIdByName.emplace(packageName, packageId);
	return packageId;
}

InstanceId RuntimeImp::run(PackageId packageId) {
	auto* package = packageIds.lockAndRef(packageId);
	if (package == nullptr)
		throw PackageNotFoundException();

	auto instance = instanceIds.lockAndAllocate(packageId);
	auto instanceId = instance->getId();

	// Boot primary thread
	auto& thread = createThread(*instance, *package);

	{
		std::lock_guard<SpinLock> lock(package->lockInstances);
		package->instances.emplace(instanceId, std::move(instance));
	}
	enqueue(&thread);
	return instanceId;
}

bool RuntimeImp::isRunning(InstanceId instanceId) {
	auto* instance = instanceIds.lockAndRef(instanceId);
	if (instance == nullptr)
		throw IllegalArgumentException();

	std::lock_guard<SpinLock> lock(instance->lockThreads);
	return !instance->threads.empty();
}

void RuntimeImp::stop(InstanceId instanceId) {
	auto* instance = instanceIds.lockAndRef(instanceId);
	if (instance == nullptr)
		throw IllegalArgumentException();

	auto primaryPackageId = instance->primaryPackageId;
	if (primaryPackageId == enum_max<PackageId>::value)
		throw IllegalArgumentException();

	{
		std::lock_guard<SpinLock> lock0(instance->lockThreads);
		if (!instance->threads.empty())
			throw UnsupportedOperationException();
	}

	auto* package = packageIds.lockAndRef(instance->primaryPackageId);
	chatra_assert(package != nullptr);
	{
		std::lock_guard<SpinLock> lock1(package->lockInstances);
		package->instances.erase(instanceId);
	}
}

TimerId RuntimeImp::addTimer(const std::string& name) {
	if (name.length() == 0)
		throw IllegalArgumentException();
	auto timer = newEmulatedTimer();
	auto* timerPtr = timer.get();

	std::lock_guard<SpinLock> lock(lockTimers);
	auto it = timers.find(name);
	if (it != timers.cend()) {
		return static_cast<TimerId>(std::distance(idToTimer.cbegin(),
				std::find(idToTimer.cbegin(), idToTimer.cend(), it->second.get())));
	}

	timers.emplace(name, std::move(timer));
	idToTimer.emplace_back(timerPtr);
	return static_cast<TimerId>(idToTimer.size() - 1);
}

void RuntimeImp::increment(TimerId timerId, int64_t step) {
	if (step < 0)
		throw IllegalArgumentException();

	Timer* timer;
	{
		std::lock_guard<SpinLock> lock(lockTimers);
		auto index = static_cast<size_t>(timerId);
		if (index >= idToTimer.size())
			throw IllegalArgumentException();
		timer = idToTimer[index];
	}
	timer->increment(std::chrono::milliseconds(step));
}

std::shared_ptr<debugger::IDebugger> RuntimeImp::getDebugger(
		std::shared_ptr<debugger::IDebuggerHost> debuggerHost) {
	std::lock_guard<std::mutex> lock0(lockDebugger);
	this->debuggerHost = std::move(debuggerHost);
	return std::static_pointer_cast<debugger::IDebugger>(self.lock());
}

void RuntimeImp::pause() {
	std::lock_guard<std::mutex> lock0(lockDebugger);
	if (paused)
		throw debugger::IllegalRuntimeStateException("Runtime is already paused");

	if (multiThread) {
		{
			std::unique_lock<std::mutex> lock1(mtQueue);
			previousWorkerThreads = targetWorkerThreads;
		}
		shutdownThreads();
	}
	paused = true;
}

void RuntimeImp::resume() {
	std::lock_guard<std::mutex> lock0(lockDebugger);
	if (!paused)
		throw debugger::IllegalRuntimeStateException("Runtime is not paused");

	if (multiThread)
		setWorkers(previousWorkerThreads);
	paused = false;
}

bool RuntimeImp::isPaused() {
	std::lock_guard<std::mutex> lock0(lockDebugger);
	return paused;
}

#define CHATRA_POP_DEBUGGABLE_THREAD  \
		std::lock_guard<std::mutex> lock0(lockDebugger);  \
		auto* thread = popDebuggableThread(threadId);  \
		if (thread == nullptr)  \
			return debugger::StepRunResult::NotInRunning

debugger::StepRunResult RuntimeImp::stepOver(debugger::ThreadId threadId) {
	CHATRA_POP_DEBUGGABLE_THREAD;

	auto framesCount = thread->frames.size();
	auto* node = thread->currentNode();
	if (node == nullptr) {
		enqueue(thread);
		return debugger::StepRunResult::NotTraceable;
	}

	return stepRun(thread, [&]() {
		return framesCount <= thread->frames.size()
				&& node == thread->currentNode(framesCount - 1);
	});
}

debugger::StepRunResult RuntimeImp::stepInto(debugger::ThreadId threadId) {
	CHATRA_POP_DEBUGGABLE_THREAD;

	auto framesCount = thread->frames.size();
	auto* node = thread->currentNode();
	if (node == nullptr) {
		enqueue(thread);
		return debugger::StepRunResult::NotTraceable;
	}

	return stepRun(thread, [&]() {
		return framesCount == thread->frames.size() && node == thread->currentNode();
	});
}

debugger::StepRunResult RuntimeImp::stepOut(debugger::ThreadId threadId) {
	CHATRA_POP_DEBUGGABLE_THREAD;

	auto targetFrameIndex = thread->frames.size() - 1;
	for (;;) {
		auto& f = thread->frames[targetFrameIndex];
		targetFrameIndex -= f.popCount;
		if (f.scope->getScopeType() != ScopeType::Block)
			break;
	}

	return stepRun(thread, [&]() {
		return targetFrameIndex + 1 < thread->frames.size();
	});
}

#define CHATRA_CHECK_RUNTIME_PAUSED  \
		std::lock_guard<std::mutex> lock0(lockDebugger);  \
		if (!paused)  \
			throw debugger::IllegalRuntimeStateException();

debugger::BreakPointId RuntimeImp::addBreakPoint(const debugger::CodePoint& point) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto it = std::find_if(breakPoints.cbegin(), breakPoints.cend(), [&](decltype(breakPoints)::const_reference e) {
		auto& bp = e.second;
		return bp.point.packageName == point.packageName && bp.point.fileName == point.fileName
				&& bp.point.lineNo == point.lineNo;
	});
	if (it != breakPoints.cend())
		throw IllegalArgumentException();

	BreakPoint bp;
	bp.point = point;
	applyBreakPoint(bp);

	auto breakPointId = static_cast<debugger::BreakPointId>(nextBreakPointId++);
	breakPoints.emplace(breakPointId, std::move(bp));
	return breakPointId;
}

void RuntimeImp::removeBreakPoint(debugger::BreakPointId breakPointId) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto it = breakPoints.find(breakPointId);
	if (it == breakPoints.cend())
		throw IllegalArgumentException();

	cancelBreakPoint(it->second);
	breakPoints.erase(it);
}

std::vector<debugger::PackageState> RuntimeImp::getPackagesState() {
	CHATRA_CHECK_RUNTIME_PAUSED;

	std::vector<debugger::PackageState> ret;
	packageIds.forEach([&](const Package& package) {
		if (package.getId() == finalizerPackageId)
			return;

		ret.emplace_back();
		auto& v = ret.back();

		v.packageId = package.getId();
		v.packageName = package.name;
		v.scripts = package.scripts;
	});
	return ret;
}

std::vector<debugger::InstanceState> RuntimeImp::getInstancesState() {
	CHATRA_CHECK_RUNTIME_PAUSED;

	std::vector<debugger::InstanceState> ret;
	instanceIds.forEach([&](const Instance& instance) {
		auto instanceId = instance.getId();
		if (instanceId == finalizerInstanceId || instanceId == gcInstance->getId())
			return;

		ret.emplace_back();
		auto& v = ret.back();

		v.instanceId = instance.getId();
		v.primaryPackageId = instance.primaryPackageId;

		std::lock_guard<SpinLock> lock1(instance.lockThreads);
		v.threadIds.reserve(instance.threads.size());
		for (auto& e : instance.threads)
			v.threadIds.emplace_back(static_cast<debugger::ThreadId>(static_cast<size_t>(e.first)));
	});
	return ret;
}

debugger::ThreadState RuntimeImp::getThreadState(debugger::ThreadId threadId) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto requester = static_cast<Requester>(static_cast<size_t>(threadId));

	std::lock_guard<decltype(threadIds)> lock1(threadIds);
	if (!threadIds.has(requester))
		throw IllegalArgumentException("specified threadId(%zu) is not found", static_cast<size_t>(threadId));
	auto* thread = threadIds.ref(requester);

	debugger::ThreadState ret;
	ret.threadId = threadId;

	for (size_t frameIndex = thread->frames.size() - 1; frameIndex + 1 > residentialFrameCount;
			frameIndex -= thread->frames[frameIndex].popCount) {
		ret.frameIds.emplace_back(static_cast<debugger::FrameId>(frameIndex));
	}

	return ret;
}

static debugger::ScopeType convertScopeType(ScopeType scopeType) {
	switch (scopeType) {
	case ScopeType::Package:  return debugger::FrameType::Package;
	case ScopeType::ScriptRoot:  return debugger::FrameType::ScriptRoot;
	case ScopeType::Class:  return debugger::FrameType::Class;
	case ScopeType::Method:  return debugger::FrameType::Method;
	case ScopeType::InnerMethod:  return debugger::FrameType::Method;
	case ScopeType::Block:  return debugger::FrameType::Block;
	default:
		throw IllegalArgumentException();
	}
}

debugger::FrameState RuntimeImp::getFrameState(debugger::ThreadId threadId, debugger::FrameId frameId) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto requester = static_cast<Requester>(static_cast<size_t>(threadId));
	auto frameIndex = static_cast<size_t>(frameId);

	std::lock_guard<decltype(threadIds)> lock1(threadIds);
	if (!threadIds.has(requester))
		throw IllegalArgumentException("specified threadId(%zu) is not found", static_cast<size_t>(threadId));
	auto* thread = threadIds.ref(requester);

	if (frameIndex >= thread->frames.size())
		throw IllegalArgumentException("specified frameId(%zu, threadId = %zu) is not found",
				frameIndex, static_cast<size_t>(threadId));
	auto& f = thread->frames[frameIndex];

	debugger::FrameState ret;
	ret.threadId = threadId;
	ret.frameId = frameId;
	ret.frameType = convertScopeType(f.scope->getScopeType());
	ret.current = nodeToCodePoint(f.package.getId(), thread->currentNode(frameIndex));

	for (; frameIndex != SIZE_MAX; frameIndex = thread->frames[frameIndex].parentIndex) {
		auto scopeType = thread->frames[frameIndex].scope->getScopeType();
		if (scopeType == ScopeType::Thread || scopeType == ScopeType::Global)
			continue;
		ret.scopeIds.emplace_back(static_cast<debugger::ScopeId>(frameIndex));
	}

	return ret;
}

static std::string getClassNameForDebugger(const StringTable* sTable, Object& object) {
	switch (object.getTypeId()) {
	case TypeId::CapturedScope:
	case TypeId::CapturedScopeObject:
	case typeId_TemporaryObject:
	case typeId_WaitContext:
		return "systemObject";

	case typeId_PackageObject:
		return "packageObject";

	default:
		auto* cl = static_cast<ObjectBase*>(&object)->getClass();
		auto className = sTable->ref(cl->getName());
		auto* p = cl->getPackage();
		if (p != nullptr && !p->name.empty())
			className = p->name + "." + className;
		return className;
	}
}

static debugger::Value getValueForDebugger(const StringTable* sTable, Reference ref) {
	debugger::Value v;
	switch (ref.valueTypeWithoutLock()) {
	case ReferenceValueType::Bool:
		v.valueType = debugger::ValueType::Bool;
		v.vBool = ref.getBoolWithoutLock();
		break;
	case ReferenceValueType::Int:
		v.valueType = debugger::ValueType::Int;
		v.vInt = ref.getIntWithoutLock();
		break;
	case ReferenceValueType::Float:
		v.valueType = debugger::ValueType::Float;
		v.vFloat = ref.getFloatWithoutLock();
		break;

	case ReferenceValueType::Object: {
		if (ref.isNullWithoutLock())
			v.valueType = debugger::ValueType::Null;
		else if (ref.derefWithoutLock<ObjectBase>().getClass() == String::getClassStatic()) {
			v.valueType = debugger::ValueType::String;
			v.vString = ref.derefWithoutLock<String>().getValue();
		}
		else {
			v.valueType = debugger::ValueType::Object;
			auto& object = ref.derefWithoutLock();
			v.objectId = static_cast<debugger::ObjectId>(object.getObjectIndex());
			v.className = getClassNameForDebugger(sTable, object);
		}
		break;
	}
	}
	return v;
}

static debugger::Value getValueForDebugger(const StringTable* sTable, const Method* method) {
	chatra_assert(method->position == SIZE_MAX);

	debugger::Value v;
	v.valueType = debugger::ValueType::Method;
	v.methodName = sTable->ref(method->name);
	if (method->subName != StringId::Invalid)
		v.methodName += "." + sTable->ref(method->subName);
	v.methodArgs = method->getSignature(sTable);
	return v;
}

class ValueWriterForDebugger {
private:
	std::unordered_map<std::string, debugger::Value>& values;
	std::unordered_map<std::string, unsigned> keys;

public:
	explicit ValueWriterForDebugger(std::unordered_map<std::string, debugger::Value>& values) : values(values) {}

	void addValue(std::string key, debugger::Value v) {
		auto it = keys.find(key);
		if (it == keys.cend()) {
			keys.emplace(key, 1);
			values.emplace(std::move(key), std::move(v));
			return;
		}

		if (it->second == 1) {
			auto vFirst = values[key];
			values.erase(key);
			values.emplace(key + "[1]", std::move(vFirst));
		}

		auto count = it->second + 1;
		values.emplace(key + "[" + std::to_string(count) + "]", std::move(v));
		keys.emplace(std::move(key), count);
	}

	void addRef(const StringTable* sTable, StringId name, Reference ref) {
		addValue(sTable->ref(name), getValueForDebugger(sTable, ref));
	}

	void addMethod(const StringTable* sTable, const Method* method) {
		auto v = getValueForDebugger(sTable, method);
		auto key = v.methodName;
		addValue(std::move(key), std::move(v));
	}
};

debugger::ScopeState RuntimeImp::getScopeState(debugger::ThreadId threadId, debugger::ScopeId scopeId) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto requester = static_cast<Requester>(static_cast<size_t>(threadId));
	auto frameIndex = static_cast<size_t>(scopeId);

	std::lock_guard<decltype(threadIds)> lock1(threadIds);
	if (!threadIds.has(requester))
		throw IllegalArgumentException("specified threadId(%zu) is not found", static_cast<size_t>(threadId));
	auto* thread = threadIds.ref(requester);

	if (frameIndex >= thread->frames.size())
		throw IllegalArgumentException("specified scopeId(%zu, threadId = %zu) is not found",
				frameIndex, static_cast<size_t>(threadId));
	auto& f = thread->frames[frameIndex];

	debugger::ScopeState ret;
	ret.threadId = threadId;
	ret.scopeId = scopeId;
	ret.scopeType = convertScopeType(f.scope->getScopeType());

	ValueWriterForDebugger writer(ret.values);

	for (auto& e : f.scope->getAllRefsWithKey())
		writer.addRef(primarySTable.get(), e.first, e.second);

	if (f.methods != nullptr) {
		for (auto* method : f.methods->getAllMethods())
			writer.addMethod(primarySTable.get(), method);
	}

	return ret;
}

debugger::ObjectState RuntimeImp::getObjectState(debugger::ObjectId objectId) {
	CHATRA_CHECK_RUNTIME_PAUSED;

	auto& object = storage->derefDirect(static_cast<size_t>(objectId));

	debugger::ObjectState ret;
	ret.objectId = objectId;
	ret.className = getClassNameForDebugger(primarySTable.get(), object);

	ValueWriterForDebugger writer(ret.values);

	switch (object.getTypeId()) {
	case TypeId::CapturedScope:
	case TypeId::CapturedScopeObject:
	case typeId_TemporaryObject:
	case typeId_WaitContext:
		break;

	default: {
		auto* cl = storage->derefDirect<ObjectBase>(static_cast<size_t>(objectId)).getClass();

		for (auto* method : cl->refMethods().getAllMethods()) {
			if (method->position != SIZE_MAX)
				writer.addRef(primarySTable.get(), method->name, object.ref(method->position));
			else
				writer.addMethod(primarySTable.get(), method);
		}

		for (auto* method : cl->refConstructors().getAllMethods())
			writer.addMethod(primarySTable.get(), method);
	}
	}

	return ret;
}


static std::atomic<bool> initialized = {false};
static std::mutex mtInitialize;

std::shared_ptr<Runtime> Runtime::newInstance(std::shared_ptr<IHost> host,
		const std::vector<uint8_t>& savedState,
		unsigned initialThreadCount) {

	if (!initialized) {
		std::lock_guard<std::mutex> lock(mtInitialize);
		if (!initialized) {
			try {
				initializeLexicalAnalyzer();
				initializeParser();
				initializeEmbeddedClasses();
				initializeEmbeddedFunctions();
			}
			catch (...) {
#ifndef CHATRA_NDEBUG
				std::fprintf(stderr, "fatal: failed to initialize some subsystems!");
#endif
				return nullptr;
			}
			initialized = true;
		}
	}

	auto runtime = std::make_shared<RuntimeImp>(std::move(host));
	runtime->setSelf(runtime);
	if (savedState.empty())
		runtime->initialize(initialThreadCount);
	else
		runtime->initialize(initialThreadCount, savedState);

	return std::static_pointer_cast<Runtime>(runtime);
}


void UserObjectBase::onDestroy(void* tag, Requester requester, Reference& ref) {
	(void)requester;

	auto& runtime = *static_cast<RuntimeImp*>(tag);

	chatra_assert(ref.deref<ObjectBase>().getTypeId() == typeId_UserObjectBase);
	auto& object = ref.deref<UserObjectBase>();
	if (object.deinitCalled)
		return;

	nativePtr.reset();

	if (object.getClass()->refMethods().find(nullptr, StringId::Deinit, StringId::Invalid, {}, {}) != nullptr) {
		std::lock_guard<SpinLock> lock(runtime.lockFinalizerTemporary);
		auto r0 = runtime.scope->ref(StringId::FinalizerObjects);
		auto r1 = runtime.scope->ref(StringId::FinalizerTemporary);

		auto& copy = r1.allocate<UserObjectBase>(object.getClass());
		copy.deinitCalled = true;
		for (auto sid : object.keys())
			copy.ref(sid).setWithoutBothLock(object.ref(sid));
		auto size = object.size();
		for (size_t i = 0; i < size; i++)
			copy.ref(i).setWithoutBothLock(object.ref(i));

		r0.deref<Array>().add(r1);
		r1.setNull();

		runtime.enqueue(runtime.finalizerThread);
	}
}


#ifndef CHATRA_NDEBUG
void Package::dump(const std::shared_ptr<StringTable>& sTable) {
	if (scope) {
		printf("scope:\n");
		scope->dump(sTable);
	}
	printf("package-class:\n");
	if (clPackage)
		clPackage->dump(sTable);
	printf("classes:\n");
	classes.dump(sTable);
	printf("imports:");
	for (StringId sid : imports)
		printf(" %s", sTable->ref(sid).c_str());
	printf("\n");
}

void RuntimeImp::dump() {
	{
		std::lock_guard<SpinLock> lock(lockSTable);
		printf("storage:\n");
		storage->dump(distributedSTable);
		printf("packages:\n");
		for (auto& e : packages)
			e.second->dump(distributedSTable);
		printf("classes:\n");
		classes.dump(distributedSTable);
		printf("methods:\n");
		methods.dump(distributedSTable);
		printf("operators:\n");
		operators.dump(distributedSTable);
	}
	{
		std::lock_guard<SpinLock> lock(lockQueue);
		printf("%u threads running\n", static_cast<unsigned>(queue.size()));
	}
}
#endif // !CHATRA_NDEBUG

}  // namespace chatra
