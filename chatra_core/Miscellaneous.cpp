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

void TupleAssignmentMap::Element::save(Writer& w) const {
	w.out(destIndex);
	w.out(sourceIndexes);
	w.out(type);
}

void TupleAssignmentMap::Element::restore(Reader& r) {
	r.in(destIndex);
	r.inArray(sourceIndexes);
	r.in(type);
}

bool TupleAssignmentMap::save(Writer& w) const {
	w.out(map);
	return true;
}

TupleAssignmentMap::TupleAssignmentMap(Reader& r) noexcept : ObjectBase(typeId_TupleAssignmentMap, getClassStatic()) {
	(void)r;
}

bool TupleAssignmentMap::restore(Reader& r) {
	r.inArray(map);
	return true;
}

static size_t functionObjectRefCount(Thread& thread, size_t frameIndex) {
	size_t count = 0;
	while (frameIndex != SIZE_MAX) {
		auto& f = thread.frames[frameIndex];
		auto scopeType = f.scope->getScopeType();
		if (scopeType == ScopeType::Package)
			return count;
		chatra_assert(scopeType != ScopeType::Thread && scopeType != ScopeType::Global);
		count++;
		frameIndex = f.parentIndex;
	}
	return count;
}

FunctionObject::FunctionObject(Storage& storage, Thread& thread, Package* package, size_t parentIndex,
		const MethodTable* methodTable, StringId name) noexcept
		: ObjectBase(storage, typeId_FunctionObject, getClassStatic(), functionObjectRefCount(thread, parentIndex), thread.getId()),
		package(package), scopeType(parentIndex == SIZE_MAX ? ScopeType::Method : ScopeType::InnerMethod),
		methodTable(methodTable), name(name) {

	scopes.reserve(size());
	size_t position = 0;
	while (parentIndex != SIZE_MAX) {
		auto& f = thread.frames[parentIndex];
		auto scopeType = f.scope->getScopeType();
		if (scopeType == ScopeType::Package)
			break;

		scopes.emplace_back(scopeType, f.node, f.cl);
		if (scopeType == ScopeType::Class)
			ref(position).set(f.scope->ref(StringId::Self));
		else
			ref(position).capture(*f.scope);

		position++;
		parentIndex = f.parentIndex;
	}
	chatra_assert(scopes.size() == size());
}

FunctionObject::FunctionObject(Storage& storage, Thread& thread, Package* package, Reference ref,
		const MethodTable* methodTable, StringId name) noexcept
		: ObjectBase(storage, typeId_FunctionObject, getClassStatic(), 1, thread.getId()),
		package(package), scopeType(ScopeType::Method), methodTable(methodTable), name(name) {

	this->ref(0).set(ref);
	scopes.emplace_back(ScopeType::Class, nullptr, ref.deref<ObjectBase>().getClass());
}

bool FunctionObject::save(Writer& w) const {
	w.out(package);
	w.out(scopes, [&](const decltype(scopes)::value_type& e) {
		w.out(std::get<0>(e));
		w.out(std::get<1>(e));
		w.out(std::get<2>(e));
	});
	w.out(scopeType);
	w.out(methodTable);
	w.out(name);
	return false;
}

FunctionObject::FunctionObject(Reader& r) noexcept
		: ObjectBase(typeId_FunctionObject, getClassStatic()),
		package(nullptr), scopeType(ScopeType::Method), methodTable(nullptr), name(StringId::Invalid) {
	(void)r;
}

bool FunctionObject::restore(Reader& r) {
	package = r.read<Package*>();
	r.inList([&]() {
		scopes.emplace_back(r.read<ScopeType>(), r.read<Node*>(), r.read<Class*>());
	});
	r.in(scopeType);
	r.in(methodTable);
	r.in(name);
	return false;
}

WaitContext::WaitContext(Storage& storage, Reference self, Thread& thread, size_t targetCount) noexcept
		: Object(storage, typeId_WaitContext, targetCount, Object::ElementsAreExclusive()), thread(thread), self(std::move(self)) {
}

void WaitContext::deploy(unsigned waitingId, size_t callerFrame,
		std::vector<std::tuple<size_t, std::string, Reference>> targets) {

	this->waitingId = waitingId;
	this->callerFrame = callerFrame;

	remainingCount = targets.size();

	size_t index = 0;
	for (auto& e : targets) {
		auto refEv = std::get<2>(e);
		ref(index).set(refEv);

		this->targets.emplace_back(new Target(std::get<0>(e), std::move(std::get<1>(e)), derefAsEventObject(refEv)));
		auto& t = this->targets.back();
		t->ct = this;
		t->eventObject->registerWatcher(t.get(), WaitContextEventWatcher);
		index++;
	}

	for (auto& target : this->targets)
		target->eventObject->activateWatcher(target.get());
}

void WaitContext::saveThread(Writer& w) const {
	w.out(&thread);
}

bool WaitContext::save(Writer& w) const {
	w.out(waitingId);
	w.out(callerFrame);
	w.out(targets, [&](const std::unique_ptr<Target>& target) {
		w.out(target->index);
		w.out(target->key);
	});
	w.out(remainingCount);

	return false;
}

void WaitContext::saveReferences(Writer& w) const {
	w.out(self);
}

WaitContext::WaitContext(Thread& thread, Reader& r) noexcept
		: Object(typeId_WaitContext), thread(thread), self(Reference::SetNull()) {
	(void)r;
}

bool WaitContext::restore(Reader& r) {
	r.in(waitingId);
	r.in(callerFrame);
	r.inList([&]() {
		auto index = r.read<size_t>();
		auto key = r.read<std::string>();
		targets.emplace_back(new Target(index, std::move(key), nullptr));
	});
	r.in(remainingCount);

	return false;
}

void WaitContext::restoreReferences(Reader& r) {
	self = r.readReference();

	for (size_t i = 0; i < targets.size(); i++) {
		auto& t = targets[i];
		t->ct = this;
		t->eventObject = derefAsEventObject(ref(i));
		t->eventObject->registerWatcher(t.get(), WaitContextEventWatcher);
		t->eventObject->activateWatcher(t.get());
	}
}

bool WaitContextEventWatcher(void* tag) {
	auto* target = static_cast<WaitContext::Target*>(tag);
	auto* ct = target->ct;
	auto& thread = ct->thread;
	auto& runtime = thread.runtime;

	if (runtime.attemptToShutdown)
		return false;

	bool processed = false;
	bool safeToRemove;
	{
		std::lock_guard<SpinLock> lock(ct->lockTriggered);
		if (ct->remainingCount-- == ct->targets.size()) {
			for (auto& t : ct->targets) {
				if (t->eventObject->unregisterWatcher(t.get()))
					ct->remainingCount--;
			}

			auto ret = thread.frames[ct->callerFrame].values.back()->getRef();
			if (target->index != SIZE_MAX)
				ret.setInt(static_cast<int64_t>(target->index));
			else
				ret.allocateWithoutLock<String>().setValue(std::move(target->key));

			runtime.resume(ct->waitingId);
			processed = true;
		}
		safeToRemove = (ct->remainingCount == 0);
	}

	if (safeToRemove) {
		auto ref = ct->self;
		ref.setNull();
		std::lock_guard<SpinLock> lock(runtime.lockScope);
		runtime.recycledRefs.emplace_back(ref);
	}

	return processed;
}

EventObject* derefAsEventObject(Reference ref) {
	if (ref.valueType() != ReferenceValueType::Object || ref.isNull())
		throw RuntimeException(StringId::IllegalArgumentException);

	auto* cl = ref.deref<ObjectBase>().getClass();
	EventObject* ret = nullptr;
	if (cl == Async::getClassStatic())
		ret = &ref.deref<Async>();

	if (ret != nullptr)
		return ret;

	throw RuntimeException(StringId::IllegalArgumentException);
}


#ifdef CHATRA_DEBUG_LOCK

static std::atomic<unsigned> lockId = {0};

void Lock::setFrame(Frame* currentFramePtr) {
	thread = &currentFramePtr->thread;
	frameIndex = static_cast<size_t>(std::distance(thread->frames.cbegin(), 
			std::find_if(thread->frames.cbegin(), thread->frames.cend(), [&](const Frame& f) { return &f == currentFramePtr; })));
	id = lockId++;
}

void Lock::outputHeader(std::FILE* fp) {
	chatra_assert(thread != nullptr);
	std::fprintf(fp, "Lock(thread=%u, frame#%u): ",
			static_cast<unsigned>(thread->getId()), static_cast<unsigned>(frameIndex));
}

void Lock::outputReference(std::FILE* fp, Reference ref, LockType lockType) {
	const char* const lockTypeNames[] = {"hard", "soft", "temp"};
	std::fprintf(fp, "node@%p[%s]",
			static_cast<void*>(ref.___nodePtr()), lockTypeNames[static_cast<unsigned>(lockType)]);

	auto requester = ref.lockedBy();
	if (requester == InvalidRequester)
		std::fprintf(fp, "(unlocked)");
	else
		std::fprintf(fp, "(locked by #%u)", static_cast<unsigned>(requester));

	if (!ref.isNullWithoutLock() && ref.valueTypeWithoutLock() == ReferenceValueType::Object) {
		std::fprintf(fp, "->@%p", static_cast<void*>(&ref.derefWithoutLock()));
	}
}

void Lock::outputLog(const char* verb, Reference ref, LockType lockType) {
	chatra_assert(thread != nullptr);
	outputHeader(stdout);
	std::printf("%s: ", verb);
	outputReference(stdout, ref, lockType);
	std::printf("\n");
	std::fflush(stdout);
}

void Lock::outputLog(const char* verb) {
	chatra_assert(thread != nullptr);
	outputHeader(stdout);
	std::printf("%s\n", verb);
	std::fflush(stdout);
}

void Lock::audit() {
	chatra_assert(thread != nullptr);
	std::vector<std::tuple<Reference, LockType>> failedList;
	for (auto lockType : {LockType::Hard, LockType::Soft, LockType::Temporary}) {
		for (auto& ref : refs[static_cast<size_t>(lockType)]) {
			if (ref.lockedBy() == thread->getId())
				continue;
			if (methodCall && lockType == LockType::Temporary && suspendedRefs.count(ref) == 1)
				continue;
			failedList.emplace_back(ref, lockType);
		}
	}
	if (!failedList.empty()) {
		FILE* stream = stdout;  // stderr
		outputHeader(stream);
		std::fprintf(stream, "audit: unexpected unlocked references:");
		for (auto& e : failedList) {
			std::fprintf(stream, " ");
			outputReference(stream, std::get<0>(e), std::get<1>(e));
		}
		std::fprintf(stream, "\n");
		std::fflush(stream);
		chatra_assert(false);
	}
}

#define CHATRA_DEBUG_LOCK_AUDIT  audit()
#define CHATRA_DEBUG_LOCK_LOG0(verb)  outputLog(verb)
#define CHATRA_DEBUG_LOCK_LOG1(verb, ref, lockType)  outputLog(verb, ref, lockType)

#else // CHATRA_DEBUG_LOCK

#define CHATRA_DEBUG_LOCK_AUDIT  (void)0
#define CHATRA_DEBUG_LOCK_LOG0(verb)  (void)0
#define CHATRA_DEBUG_LOCK_LOG1(verb, ref, lockType)  (void)0

#endif // CHATRA_DEBUG_LOCK

#ifdef CHATRA_DEBUG_LOCK
Lock::~Lock() {
	if (saved)
		return;
	chatra_assert(!methodCall);
	for (auto& e : refs)
		chatra_assert(e.empty());
}
#endif

bool Lock::has(Reference ref, LockType lockType) const {
	return refs[static_cast<size_t>(lockType)].count(ref) != 0;
}

void Lock::add(Reference ref, LockType lockType) {
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG1("add", ref, lockType);

#ifdef CHATRA_DEBUG_LOCK
	if (methodCall && lockType == LockType::Temporary) {
		chatra_assert(refs[static_cast<size_t>(LockType::Temporary)].count(ref) == 1);
		chatra_assert(suspendedRefs.count(ref) == 1);
		suspendedRefs.erase(ref);
	}
	else
		chatra_assert(refs[static_cast<size_t>(lockType)].count(ref) == 0);
	for (auto _lockType : {LockType::Hard, LockType::Soft, LockType::Temporary})
		chatra_assert(_lockType == lockType || refs[static_cast<size_t>(_lockType)].count(ref) == 0);
#endif

	refs[static_cast<size_t>(lockType)].emplace(ref);
}

void Lock::moveFromSoftToTemporary(Reference ref) {
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG1("move_from", ref, LockType::Soft);
	CHATRA_DEBUG_LOCK_LOG1("move_to", ref, LockType::Temporary);

#ifdef CHATRA_DEBUG_LOCK
	chatra_assert(refs[static_cast<size_t>(LockType::Hard)].count(ref) == 0);
	chatra_assert(refs[static_cast<size_t>(LockType::Soft)].count(ref) +
			refs[static_cast<size_t>(LockType::Temporary)].count(ref) == 1);
#endif

	if (refs[static_cast<size_t>(LockType::Soft)].erase(ref) == 0) {
		CHATRA_DEBUG_LOCK_LOG0("move_target_does_not_exist");
		return;
	}
	CHATRA_DEBUG_LOCK_LOG0("move_target_exists");
	refs[static_cast<size_t>(LockType::Temporary)].emplace(ref);
}

void Lock::release(Reference ref, LockType lockType) {
	chatra_assert(lockType != LockType::Temporary || !methodCall);
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG1("release", ref, lockType);
	if (refs[static_cast<size_t>(lockType)].erase(ref) == 0)
		throw InternalError();
	ref.unlock();
}

void Lock::releaseIfExists(Reference ref, LockType lockType) {
	chatra_assert(!methodCall);
	CHATRA_DEBUG_LOCK_AUDIT;
	if (refs[static_cast<size_t>(lockType)].erase(ref) != 0) {
		CHATRA_DEBUG_LOCK_LOG1("releaseIfExists_exists", ref, lockType);
		ref.unlock();
	}
	else
		CHATRA_DEBUG_LOCK_LOG1("releaseIfExists_does_not_exist", ref, lockType);
}

void Lock::releaseAll() {
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG0("releaseAll");

	if (methodCall)
		refs[static_cast<size_t>(LockType::Temporary)].clear();

	for (auto& e : refs) {
		for (auto& ref : e) {
#ifdef CHATRA_DEBUG_LOCK
			chatra_assert(ref.lockedBy() == thread->getId());
#endif
			ref.unlock();
		}
		// Omitted e.clear() for performance reason
	}

#ifdef CHATRA_DEBUG_LOCK
	for (auto& e : refs)
		e.clear();
#endif
}

void Lock::releaseIfExists(Reference ref) {
	chatra_assert(!methodCall);
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG0("releaseIfExists");

#ifdef CHATRA_DEBUG_LOCK
	auto requester = ref.lockedBy();
	if (requester == InvalidRequester) {
		for (auto& e : refs)
			chatra_assert(e.count(ref) == 0);
		return;
	}
	chatra_assert(ref.lockedBy() == thread->getId());
	if (methodCall && refs[static_cast<size_t>(LockType::Temporary)].count(ref) != 0)
		return;
	size_t count = 0;
	for (auto& e : refs)
		count += e.erase(ref);
	chatra_assert(count <= 1);
	if (count != 0)
		ref.unlock();
#else
	for (auto& e : refs)
		e.erase(ref);
	// Omitted ref.unlock() for performance reason
#endif
}

void Lock::moveTemporaryToSoftLock() {
	chatra_assert(!methodCall);
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG0("moveTemporaryToSoftLock");

	auto& temp = refs[static_cast<size_t>(LockType::Temporary)];
	refs[static_cast<size_t>(LockType::Soft)].insert(temp.cbegin(), temp.cend());
	temp.clear();
}

void Lock::prepareToMethodCall() {
	chatra_assert(!methodCall);
	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG0("prepareToMethodCall");

	methodCall = true;

	auto& soft = refs[static_cast<size_t>(LockType::Soft)];
	for (auto ref : soft)
		ref.unlock();
	soft.clear();

	auto& temp = refs[static_cast<size_t>(LockType::Temporary)];
	for (auto ref : temp)
		ref.unlock();

#ifdef CHATRA_DEBUG_LOCK
	chatra_assert(suspendedRefs.empty());
	suspendedRefs.insert(temp.cbegin(), temp.cend());
#endif
}

bool Lock::isInMethodCallState() const {
	return methodCall;
}

std::vector<Reference> Lock::getTemporary() const {
	auto& temp = refs[static_cast<size_t>(LockType::Temporary)];
	return std::vector<Reference>(temp.cbegin(), temp.cend());
}

void Lock::temporaryLockRestored() {
	chatra_assert(methodCall);

#ifdef CHATRA_DEBUG_LOCK
	chatra_assert(suspendedRefs.empty());
#endif

	methodCall = false;

	CHATRA_DEBUG_LOCK_AUDIT;
	CHATRA_DEBUG_LOCK_LOG0("temporaryLockRestored");
}

void Lock::save(Writer& w) const {
	for (auto& refSet : refs)
		w.out(refSet, [&](const Reference& ref) { w.out(ref); });
	w.out(methodCall);

#ifdef CHATRA_DEBUG_LOCK
	w.out(suspendedRefs, [&](const Reference& ref) { w.out(ref); });
	saved = true;
#endif
}

void Lock::restore(Reader& r) {
	for (auto& refSet : refs) {
		r.inList([&]() { refSet.emplace(r.readReference()); });
	}
	r.in(methodCall);

#ifdef CHATRA_DEBUG_LOCK
	r.inList([&]() { suspendedRefs.emplace(r.readReference()); });
#endif
}

TemporaryObject* Frame::allocateTemporary() {
	return thread.allocateTemporary();
}

TemporaryObject* Frame::pushTemporary() {
	values.emplace_back(allocateTemporary());
	return values.back();
}

void Frame::push(TemporaryObject* value) {
	values.emplace_back(value);
}

void Frame::push(size_t position, TemporaryObject* value) {
	values.insert(values.cbegin() + position, value);
}

void Frame::pushEmptyTuple() {
	values.emplace_back(allocateTemporary());
	values.back()->setReference(thread.scope->ref(StringId::EmptyTuple));
}

void Frame::pop() {
	chatra_assert(!values.empty());
	thread.recycleTemporary(values.back());
	values.pop_back();
}

void Frame::pop(size_t first, size_t last) {
	chatra_assert(first <= last && last <= values.size());
	thread.recycleTemporary(values.cbegin() + first, values.cbegin() + last);
	values.erase(values.cbegin() + first, values.cbegin() + last);
}

void Frame::popAll() {
	thread.recycleTemporary(values.cbegin(), values.cend());
	values.clear();
}

void Frame::recycle(TemporaryObject* value) {
	thread.recycleTemporary(value);
}

void Frame::duplicateTop() {
	auto* value = values.back();
	if (value->hasArguments()) {
		auto* argValue = *(values.end() - 2);
		pushTemporary()->copyFrom(*argValue);
	}
	pushTemporary()->copyFrom(*value);
}

TemporaryTuple* Frame::pushTemporaryTuple() {
	auto* ret = thread.allocateTemporaryTuple();
	temporaryTuples.emplace_back(ret);
	return ret;
}

void Frame::popTemporaryTuple() {
	thread.recycleTemporaryTuple(temporaryTuples.back());
	temporaryTuples.pop_back();
}

void Frame::clearAllTemporaries() {
	popAll();
	thread.recycleTemporaryTuple(temporaryTuples.cbegin(), temporaryTuples.cend());
	temporaryTuples.clear();
}

Reference Frame::getSelf() const {
	chatra_assert(scope->getScopeType() == ScopeType::Class);
	return scope->ref(StringId::Self);
}

Node* Frame::getExceptionNode() const {
	for (auto it = stack.crbegin(); it != stack.crend(); it++) {
		auto* stackNode = it->first;
		if (!stackNode->tokens.empty())
			return stackNode;
	}
	if (node != nullptr && phase != 0 && phase <= node->blockNodes.size()) {
		auto& n = node->blockNodes[phase - 1];
		if (!n->tokens.empty())
			return n.get();
	}
	return node == nullptr ? nullptr : node->tokens.empty() ? nullptr : node;
}

Frame::Frame(Thread& thread, Package& package, size_t parentIndex,
		Scope* scope, size_t popCount) noexcept
		: thread(thread), package(package), parentIndex(parentIndex), scope(scope), popCount(popCount) {}

Frame::Frame(Thread& thread, Package& package, size_t parentIndex,
		ScopeType type, Node* node, size_t popCount) noexcept
		: thread(thread), package(package), parentIndex(parentIndex), frameScope(thread.runtime.storage->add(type)),
		popCount(popCount) {

	scope = frameScope.get();
	if (type == ScopeType::Block)
		lock = thread.frames[parentIndex].lock;
	else {
		frameLock.reset(new Lock());
		frameLock->setFrame(this);
		lock = frameLock.get();
	}

	this->node = node;

	if (node != nullptr && node->type != NodeType::ScriptRoot && node->type != NodeType::Class)
		hasMethods = true;
}

Frame::Frame(ForInteractive, Thread& thread, Package& package, size_t parentIndex,
		Scope* scope, Node* node, size_t popCount) noexcept
		: thread(thread), package(package), parentIndex(parentIndex), scope(scope), popCount(popCount) {

	chatra_assert(node->type == NodeType::ScriptRoot);

	frameLock.reset(new Lock());
	frameLock->setFrame(this);
	lock = frameLock.get();

	this->node = node;
}

Frame::Frame(Thread& thread, Package& package, size_t parentIndex,
		const Class* cl, Reference instanceRef, size_t popCount) noexcept
		: thread(thread), package(package), parentIndex(parentIndex),
		frameScope(thread.runtime.storage->add(ScopeType::Class)), cl(cl), popCount(popCount) {

	scope = frameScope.get();
	scope->addConst(StringId::Self).set(instanceRef);
}

void Frame::save(Writer& w) const {
	w.out(parentIndex);
	w.CHATRA_OUT_POINTER(scope, Scope);
	w.CHATRA_OUT_POINTER(frameScope.get(), Scope);
	w.out(cl);
	w.out(popCount);
	w.out(captured);

	w.out(isLoop);
	w.out(label);
	w.out(phaseOnContinue);

	w.out(frameLock != nullptr);
	if (frameLock) {
		w.out(*frameLock);
		w.CHATRA_REGISTER_POINTER(frameLock.get(), Lock);
	}
	w.CHATRA_OUT_POINTER(lock, Lock);

	w.out(node);

	auto objectMap = thread.scope->getObjectMap();

	w.out(phase);
	w.out(stack, [&](const std::pair<Node*, size_t>& e) {
		w.out(e.first);
		w.out(e.second);
	});
	w.out(subStack);
	w.out(values, [&](const TemporaryObject* value) {
		w.out(objectMap.at(value));
	});
	w.out(exception != nullptr);
	if (exception != nullptr)
		w.out(objectMap.at(exception));
	w.out(exceptionNode);
	w.out(stackTrace);
	w.out(caughtException != nullptr);
	if (caughtException != nullptr)
		w.out(objectMap.at(caughtException));

	w.out(temporaryTuples, [&](const TemporaryTuple* value) {
		w.out(objectMap.at(value));
	});
}

Frame::Frame(Thread& thread, Package& package, Reader& r) noexcept
		: thread(thread), package(package), parentIndex(SIZE_MAX), scope(nullptr), popCount(0) {
	(void)r;
}

void Frame::restore(Reader& r) {
	r.in(parentIndex);
	scope = r.CHATRA_READ_RAW(Scope);
	frameScope = r.CHATRA_READ_UNIQUE(Scope);
	r.in(cl);
	r.in(popCount);
	r.in(captured);

	r.in(isLoop);
	r.in(label);
	r.in(phaseOnContinue);

	if (r.read<bool>()) {
		frameLock.reset(r.allocate<Lock>());
		frameLock->setFrame(this);
		r.CHATRA_REGISTER_POINTER(frameLock.get(), Lock);
	}
	lock = r.CHATRA_READ_RAW(Lock);

	r.in(node);
	if (node != nullptr && node->type != NodeType::ScriptRoot && node->type != NodeType::Class)
		hasMethods = true;

	r.in(phase);
	r.inList([&]() {
		auto* node = r.read<Node*>();
		auto nodePhase = r.read<size_t>();
		stack.emplace_back(std::make_pair(node, nodePhase));
	});
	r.inArray(subStack);
	r.inList([&]() {
		values.emplace_back(thread.restoreTemporary(r));
	});
	if (r.read<bool>())
		exception = thread.restoreTemporary(r);
	exceptionNode = r.read<Node*>();
	stackTrace = r.read<std::string>();
	if (r.read<bool>())
		caughtException = thread.restoreTemporary(r);

	r.inList([&]() {
		temporaryTuples.emplace_back(thread.restoreTemporaryTuple(r));
	});
}

void TransferRequest::save(Writer& w) const {
	w.out(requester);
	w.out(ref);
	w.out(lockType);
}

TransferRequest::TransferRequest(Reader& r) noexcept
		: requester(InvalidRequester), ref(Reference::SetNull()), lockType(LockType::Soft) {
	(void)r;
}

void TransferRequest::restore(Reader& r) {
	r.in(requester);
	ref = r.readReference();
	r.in(lockType);
}

}  // namespace chatra
