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

#define CHATRA_EVALUATE_VALUE  do {   \
	switch (evaluateValue(EvaluateValueMode::Value)) {  \
	case EvaluateValueResult::Suspend:  return false;  \
	case EvaluateValueResult::StackChanged:  return true;  \
	default:  break;  \
	}} while(0);  \
	CHATRA_FALLTHROUGH

#define CHATRA_EVALUATE_VALUE_MODE(mode)  do {  \
	switch (evaluateValue(EvaluateValueMode::mode)) {  \
	case EvaluateValueResult::Suspend:  return false;  \
	case EvaluateValueResult::StackChanged:  return true;  \
	default:  break;  \
	}} while(0);  \
	CHATRA_FALLTHROUGH

static constexpr size_t binaryOperatorProcessPhase = 4;
static constexpr size_t assignmentOperatorEvaluationPhase = 2;
static constexpr size_t assignmentOperatorProcessPhase = 6;


bool Thread::initialized = false;
Node Thread::expressionMarkerNode;
Node Thread::initializePackageNode;
Node Thread::evaluateTupleNode;
Node Thread::defaultConstructorNode;
Node Thread::initializeVarsNode;
Node Thread::popValueNode;
Node Thread::postConstructorNode;
Node Thread::postPrefixUnaryOperatorNode;
Node Thread::restoreTemporaryLockNode;
Node Thread::returnFromAsyncNode;
Node Thread::processFor_CallNextNode;
Node Thread::varStatementMarkerNode;
std::array<Node, NumberOfOperators> Thread::operatorNode;
std::unordered_map<NodeType, Node*> Thread::specialNodes0;
std::unordered_map<Operator, Node*> Thread::specialNodes1;

void Thread::set(Node& node, NodeType type) {
	node.type = type;
	node.flags |= NodeFlags::ThreadSpecial;
	specialNodes0.emplace(type, &node);
}

void Thread::set(Node& node, Operator op) {
	node.type = NodeType::Operator;
	node.op = op;
	node.flags |= NodeFlags::ThreadSpecial;
	specialNodes1.emplace(op, &node);
}

void Thread::captureStringTable() {
	if (!hasNewSTable)
		return;
	hasNewSTable = false;
	std::lock_guard<SpinLock> lock(runtime.lockSTable);
	sTableHolder = runtime.distributedSTable;
	sTable = sTableHolder.get();
}

void Thread::scanInnerFunctions(IErrorReceiver* errorReceiver, const StringTable* sTable) {
	auto& f = frames.back();
	if (f.hasMethods && f.methods == nullptr)
		f.methods = methodTableCache.scanInnerFunctions(errorReceiver, sTable, f.package, f.node);
}

bool Thread::parse() {
	auto& f = frames.back();

	if (f.node->blockNodesState == NodeState::Parsed) {
		scanInnerFunctions(nullptr, sTable);
		return true;
	}

	auto parser = runtime.scope->ref(StringId::Parser);
	if (!lockReference(parser, LockType::Soft))
		return false;

	if (f.node->blockNodesState == NodeState::Parsed) {
		scanInnerFunctions(nullptr, sTable);
		return true;
	}

	IErrorReceiverBridge errorReceiverBridge(*this);
	unsigned sTableVersion = runtime.primarySTable->getVersion();

	auto scriptNode = f.package.parseNode(errorReceiverBridge, f.node);
	scanInnerFunctions(&errorReceiverBridge, runtime.primarySTable.get());

	if (runtime.primarySTable->getVersion() != sTableVersion) {
		runtime.primarySTable->clearDirty();
		{
			std::lock_guard<SpinLock> lock(runtime.lockSTable);
			runtime.distributedSTable = runtime.primarySTable->copy();
		}
		runtime.threadIds.forEach([&](Thread& thread) {
			thread.hasNewSTable = true;
		});
		captureStringTable();
	}

	// Defer distributing node to each threads because new thread creation during parseBlockNodes() causes MemberNotFoundException
	if (scriptNode) {
		std::lock_guard<SpinLock> lock(f.package.lockNode);
		f.package.node = std::move(scriptNode);
		auto* nNew = f.package.node.get();
		for (auto* thread : f.package.threadsWaitingForNode)
			thread->frames.back().node = nNew;
		f.package.threadsWaitingForNode.clear();
	}

	f.lock->release(parser, LockType::Soft);

	if (errorReceiverBridge.hasError())
		throw RuntimeException(StringId::ParserErrorException);

	checkTransferReq();
	return true;
}

Node* Thread::getExceptionNode() {
	auto& f = frames.back();
	for (auto it = f.stack.crbegin(); it != f.stack.crend(); it++) {
		auto* node = it->first;
		if (!node->tokens.empty())
			return node;
	}
	if (f.node != nullptr && f.phase != 0 && f.phase <= f.node->blockNodes.size()) {
		auto& n = f.node->blockNodes[f.phase - 1];
		if (!n->tokens.empty())
			return n.get();
	}
	return f.node == nullptr ? nullptr : f.node->tokens.empty() ? nullptr : f.node;
}

template <class Type>
struct AllocateExceptionPredicate {
	void operator()(Reference ref) {
		ref.allocate<Type>();
	}
};

void Thread::raiseException(const RuntimeException& ex) {
	Node* exceptionNode = getExceptionNode();

	auto& f = frames.back();
	assert(f.exception == nullptr);
	f.stack.clear();
	f.exception = f.allocateTemporary();
	switchException<AllocateExceptionPredicate>(ex.name, f.exception->setRvalue());
	f.exceptionNode = exceptionNode;

	if (ex.name == StringId::ParserErrorException)
		runtime.parserErrorRaised = true;
}

void Thread::raiseException(TemporaryObject* exValue) {
	assert(exValue != nullptr && exValue->hasRef());
	if (exValue->getRef().isNull()) {
		errorAtNode(*this, ErrorLevel::Error, getExceptionNode(), "throw statement cannot handle null value", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}

	Node* exceptionNode = getExceptionNode();

	auto& f = frames.back();
	assert(f.exception == nullptr);
	f.stack.clear();
	f.exception = f.allocateTemporary();
	f.exception->setRvalue().set(exValue->getRef());
	f.exceptionNode = exceptionNode;
}

void Thread::consumeException() {
	auto& f = frames.back();

	const Class* exCl = getReferClass(f.exception->getRef());

	auto it0 = f.node->blockNodes.cbegin();

	// consumeException() searches Catch handler starting from "current" frame,
	// which may indicate a block statement and its block can have Catch handlers.
	// In that case, consumeException() inappropriately detects a Catch handler inside the block
	// as a primary exception handler.
	// To avoid this invalid behavior, skipping current frame from handler search.
	if (f.phase > f.node->blockNodes.size())
		it0 = f.node->blockNodes.cend();
	else if (!f.node->blockNodes.empty() && f.phase != 0){
		switch (f.node->blockNodes[f.phase - 1]->type) {
		case NodeType::Catch:
			it0 = f.node->blockNodes.cend();
			if (f.node->blockNodes.back()->type == NodeType::Finally)
				it0--;
			break;

		case NodeType::Finally:
			it0 = f.node->blockNodes.cend();
			break;

		default:
			break;
		}
	}

	auto it = std::find_if(it0, f.node->blockNodes.cend(), [&](const std::shared_ptr<Node>& n) {
		if (n->type == NodeType::Finally)
			return true;
		if (n->type != NodeType::Catch)
			return false;
		auto& nClassList = n->subNodes[SubNode::Catch_ClassList];
		auto itCl = std::find_if(nClassList->subNodes.cbegin(), nClassList->subNodes.cend(), [&](const std::shared_ptr<Node>& nCl) {
			auto* cl = (nCl->type == NodeType::Name ?
					f.package.findClass(nCl->sid) : f.package.findPackageClass(nCl->subNodes[0]->sid, nCl->subNodes[1]->sid));
			return cl != nullptr && cl->isAssignableFrom(exCl);
		});
		return itCl != nClassList->subNodes.cend();
	});

	if (it == f.node->blockNodes.cend() || (*it)->type == NodeType::Finally) {
		auto& f0 = frames[frames.size() - f.popCount - 1];
		try {
			if (f0.exception != nullptr) {
				errorAtNode(*this, ErrorLevel::Error, f.exceptionNode,"unhandled exception raised inside catch block", {});
				errorAtNode(*this, ErrorLevel::Error, f0.exceptionNode,"primary exception was raised from here", {});
				emitError(StringId::UnsupportedOperationException, f.exceptionNode);
				throw AbortThreadException();
			}

			if (!f0.stack.empty() && f0.stack.back().first->op == opInitializePackage) {
				errorAtNode(*this, ErrorLevel::Error, f.exceptionNode,"unhandled exception raised during package initialization", {});
				emitError(getReferClass(f.exception->getRef())->getName(), f.exceptionNode);
				throw AbortThreadException();
			}
		}
		catch (const AbortThreadException&) {
			// To suppress emitting error messages in finish()
			f.recycle(f.exception);
			f.exception = nullptr;
			throw;
		}

		f0.exception = f0.allocateTemporary();
		f0.exception->setRvalue().set(f.exception->getRef());
		f0.exceptionNode = f.exceptionNode;
		f0.stack.clear();

		f.recycle(f.exception);
		f.exception = nullptr;

		if (f.caughtException != nullptr) {
			f.recycle(f.caughtException);
			f.caughtException = nullptr;
		}

		if (it == f.node->blockNodes.cend())
			pop();
		else {
			pushBlock(it->get(), 0);
			frames.back().popCount += (frames.end() - 2)->popCount;
		}
		return;
	}

	auto& n = *it;
	assert(n->type == NodeType::Catch);
	f.phase = static_cast<size_t>(std::distance(f.node->blockNodes.cbegin(), it)) + 1;
	pushBlock(n.get(), 0);

	auto& f0 = *(frames.end() - 2);
	auto& f1 = *(frames.end() - 1);

	f1.caughtException = f1.allocateTemporary();
	f1.caughtException->setRvalue().set(f0.exception->getRef());

	if (n->subNodes[SubNode::Catch_Label]) {
		auto ref = f1.scope->add(n->subNodes[SubNode::Catch_Label]->sid, getId());
		f1.lock->add(ref, LockType::Soft);
		ref.set(f0.exception->getRef());
	}

	f0.recycle(f0.exception);
	f0.exception = nullptr;
	emitError(exCl->getName(), f0.exceptionNode);
}

void Thread::checkIsValidNode(Node* node) {
	if (node->type != ntParserError)
		return;
	errorAtNode(*this, ErrorLevel::Error, node, "unrecoverable parser error", {});
	throw RuntimeException(StringId::ParserErrorException);
}

void Thread::sendTransferReq(Requester requester, Reference ref, LockType lockType, Requester holder) {
	assert(holder != requester);
	auto* holderThread = runtime.threadIds.ref(holder);

	std::lock_guard<SpinLock> lock(holderThread->lockTransferReq);
	holderThread->hasTransferReq = true;
	holderThread->transferReq.emplace_back(requester, ref, lockType);

	// std::printf("sendTransferReq: requester %u -> holder %u, %p\n", static_cast<unsigned>(getId()),
	// 		static_cast<unsigned>(holder), ref.___nodePtr());
	std::fflush(stdout);
}

bool Thread::lockReference(Reference ref, LockType lockType) {
	if (!ref.requiresLock())
		return true;

	auto& f = frames.back();
	assert(f.lock != nullptr);

	if (ref.lockedBy() == getId()) {
		if (lockType == LockType::Temporary)
			f.lock->moveFromSoftToTemporary(ref);
		assert(lockType == LockType::Hard || f.lock->has(ref, lockType));
		return true;
	}

	if (ref.lockOnce(getId())) {
		f.lock->add(ref, lockType);
		return true;
	}

	// Note that some other threads can be created or acquire/release lock at the same time.
	for (;;) {
		std::lock_guard<decltype(runtime.threadIds)> lock0(runtime.threadIds);

		auto holder = ref.lockedBy();
		if (holder == InvalidRequester) {
			if (ref.lockOnce(getId())) {
				f.lock->add(ref, lockType);
				return true;
			}
			continue;
		}

		sendTransferReq(getId(), ref, lockType, holder);
		return false;
	}
}

bool Thread::lockReference(TemporaryObject* value) {
	if (value->getType() != TemporaryObject::Type::ObjectRef)
		return lockReference(value->getRef(), LockType::Temporary);
	size_t primaryIndex = value->getPrimaryIndex();
	if (primaryIndex == SIZE_MAX)
		return lockReference(value->getRef(), LockType::Temporary);
	return lockReference(value->getSourceRef().deref<ObjectBase>().ref(primaryIndex), LockType::Hard);
}

void Thread::checkTransferReq() {
	if (!hasTransferReq)
		return;
	{
		std::lock_guard<SpinLock> lock(lockTransferReq);
		std::swap(transferReq, transferReqCopy);
		hasTransferReq = false;
	}
	for (auto& r : transferReqCopy) {
		for (;;) {
			if (r.ref.lockOnce(r.requester)) {
				auto* thread = runtime.threadIds.lockAndRef(r.requester);
				thread->frames.back().lock->add(r.ref, r.lockType);
				runtime.enqueue(thread);
				// std::printf("checkTransferReq: requester %u <- holder %u, %p\n", static_cast<unsigned>(r.requester),
				// 		static_cast<unsigned>(getId()), r.ref.___nodePtr());
				break;
			}
			auto holder = r.ref.lockedBy();
			if (holder == InvalidRequester)
				continue;
			sendTransferReq(r.requester, r.ref, r.lockType, holder);
			break;
		}
	}
	transferReqCopy.clear();
}

Reference Thread::allocateReference(StringId sid) {
	auto& f = frames.back();
	assert(f.lock != nullptr);
	auto ref = f.scope->add(sid, getId());
	f.lock->add(ref, LockType::Soft);
	return ref;
}

TemporaryObject* Thread::allocateTemporary() {
	// temporaryObjectPool.setBreakPoint(23, 39401);
	return temporaryObjectPool.allocate([&]() {
		return &scope->add<TemporaryObject>(getId(), *this);
	});
}

TemporaryTuple* Thread::allocateTemporaryTuple() {
	// temporaryTuplePool.setBreakPoint(1, 2);
	return temporaryTuplePool.allocate([&]() {
		return &scope->add<TemporaryTuple>(getId(), *this);
	});
}

void Thread::recycleTemporary(TemporaryObject* value) {
	temporaryObjectPool.recycle(value);
}

void Thread::recycleTemporaryTuple(TemporaryTuple* value) {
	auto objects = value->exportAll();
	recycleTemporary(objects.cbegin(), objects.cend());
	temporaryTuplePool.recycle(value);
}

void Thread::checkStackOverflow(Node* node) {
	if (frames.size() > 100) {
		errorAtNode(*this, ErrorLevel::Error, node, "stack overflow", {});
		emitError(StringId::RuntimeException, node);
		throw AbortThreadException();
	}
	if (frames.size() > 50 && !stackOverflowWarned) {
		stackOverflowWarned = true;
		errorAtNode(*this, ErrorLevel::Warning, node, "stack grows too large", {});
	}
}

Frame& Thread::pushBlock(Node* node, size_t phase) {
	checkStackOverflow(node);
	frames.back().lock->moveTemporaryToSoftLock();
	frames.emplace_back(*this, frames.back().package, frames.size() - 1, ScopeType::Block, node);
	auto& ret = frames.back();
	ret.phase = phase;
	return ret;
}

Frame& Thread::pushBlock(Node* node, size_t phase, StringId label, size_t phaseOnContinue) {
	auto& ret = pushBlock(node, phase);
	ret.isLoop = true;
	ret.label = label;
	ret.phaseOnContinue = phaseOnContinue;
	return ret;
}

void Thread::pop() {
	size_t popCount = frames.back().popCount;
	bool checkRequired = false;
	for (size_t i = 0; i < popCount; i++) {
		auto& f = frames.back();

		if (f.lock != nullptr) {
			if (f.lock == f.frameLock.get())
				f.lock->releaseAll();
			else {
				// Release lock for variables on this scope since the scope will be removed at the end of this loop
				// and references on the scope are no longer valid (except captured case).
				for (auto ref : f.scope->getAllRefs())
					f.lock->releaseIfExists(ref);
			}
		}

		switch (f.scope->getScopeType()) {
		case ScopeType::ScriptRoot:
		case ScopeType::Method:
		case ScopeType::InnerMethod:
			checkRequired = true;
			break;
		default:
			break;
		}

		f.clearAllTemporaries();
		assert(f.exception == nullptr);
		assert(f.caughtException == nullptr);

		frames.pop_back();
	}

	if (checkRequired)
		checkTransferReq();

	auto& f = frames.back();
	if (f.lock != nullptr && f.lock->isInMethodCallState())
		f.stack.emplace_back(&restoreTemporaryLockNode, 0);
}

void Thread::pop(size_t targetSize, FinallyBlockScheme scheme) {
	if (scheme == FinallyBlockScheme::Through) {
		while (targetSize < frames.size())
			pop();
		return;
	}

	while (targetSize < frames.size()) {
		auto& f = frames.back();
		if (f.node == nullptr || f.node->blockNodes.empty() || f.node->blockNodes.back()->type != NodeType::Finally)
			pop();
		else
			break;
	}

	size_t frameCount = frames.size();
	size_t startIndex = targetSize - (scheme == FinallyBlockScheme::IncludesLast ? 1 : 0);
	for (size_t i = startIndex; i < frameCount; i++) {
		auto& f = frames[i];
		if (f.node == nullptr || f.node->blockNodes.empty() || f.node->blockNodes.back()->type != NodeType::Finally)
			continue;

		frames.emplace_back(*this, frames.back().package, i, ScopeType::Block,
				f.node->blockNodes.back().get(), frames.size() == frameCount ? 1 + frameCount - targetSize : 1);
		frames.back().phase = 0;
	}
}

void Thread::constructFrames(Package* package, Node* node, size_t parentIndex) {
	if (parentIndex != SIZE_MAX) {
		assert(package != nullptr);
		frames.emplace_back(*this, *package, parentIndex, ScopeType::InnerMethod, node, 1);
		return;
	}

	if (package == nullptr)
		package = &frames[residentialFrameCount].package;

	// Note: in the case of calling method of other imported package,
	// argument "package" can be different from frames[parentIndex].package.
	frames.emplace_back(*this, *package, residentialFrameCount - 1, package->scope.get());
	frames.emplace_back(*this, *package, frames.size() - 1, ScopeType::Method, node, 2);
}

void Thread::constructFrames(Package* package, Node* node, const Class* cl, Reference ref) {
	if (package == nullptr)
		package = &frames[residentialFrameCount].package;

	frames.emplace_back(*this, *package, residentialFrameCount - 1, package->scope.get());
	frames.emplace_back(*this, *package, frames.size() - 1, cl, ref);
	frames.emplace_back(*this, *package, frames.size() - 1, ScopeType::Method, node, 3);
}

void Thread::buildPrimitive(Node* node, const Class* cl, Reference rSource, Reference rTarget) {
	// These can be native method, but for efficiency at runtime, it is implemented as embedded functions
	if (cl == Bool::getClassStatic()) {
		switch (rSource.valueType()) {
		case ReferenceValueType::Bool:  rTarget.set(rSource);  return;
		case ReferenceValueType::Int:  rTarget.setBool(rSource.getInt() != 0);  return;
		default:
			if (!rSource.isNull() && rSource.deref<ObjectBase>().getClass()->getName() == StringId::String) {
				auto value = rSource.deref<String>().getValue();
				if (value == "true")
					rTarget.setBool(true);
				else if (value == "false")
					rTarget.setBool(false);
				else
					throw RuntimeException(StringId::IllegalArgumentException);
				return;
			}
			break;
		}
	}
	else if (cl == Int::getClassStatic()) {
		switch (rSource.valueType()) {
		case ReferenceValueType::Bool:  rTarget.setInt(rSource.getBool() ? 1 : 0);  return;
		case ReferenceValueType::Int:  rTarget.set(rSource);  return;
		case ReferenceValueType::Float:  rTarget.setInt(static_cast<int64_t>(rSource.getFloat()));  return;
		default:
			if (!rSource.isNull() && rSource.deref<ObjectBase>().getClass()->getName() == StringId::String) {
				try {
					rTarget.setInt(static_cast<int64_t>(std::stoll(rSource.deref<String>().getValue())));
					return;
				}
				catch (std::invalid_argument&) {
					errorAtNode(*this, ErrorLevel::Error, node, "parameter value cannot be parsed as Int", {});
				}
				catch (std::out_of_range&) {
					errorAtNode(*this, ErrorLevel::Error, node, "parameter value is out of range for Int", {});
				}
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			break;
		}
	}
	else if (cl == Float::getClassStatic()) {
		switch (rSource.valueType()) {
		case ReferenceValueType::Bool:  rTarget.setFloat(rSource.getBool() ? 1.0 : 0.0);  return;
		case ReferenceValueType::Int:  rTarget.setFloat(static_cast<double>(rSource.getInt()));  return;
		case ReferenceValueType::Float:  rTarget.set(rSource);  return;
		default:
			if (!rSource.isNull() && rSource.deref<ObjectBase>().getClass()->getName() == StringId::String) {
				try {
					rTarget.setFloat(std::stod(rSource.deref<String>().getValue()));
					return;
				}
				catch (std::invalid_argument&) {
					errorAtNode(*this, ErrorLevel::Error, node, "parameter value cannot be parsed as Float", {});
				}
				catch (std::out_of_range&) {
					errorAtNode(*this, ErrorLevel::Error, node, "parameter value is out of range for Float", {});
				}
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			break;
		}
	}

	auto* clSource = getReferClass(rSource);
	errorAtNode(*this, ErrorLevel::Error, node, "conversion from ${0} to ${1} is not supported",
			{getClassName(clSource), sTable->ref(cl->getName())});
	throw RuntimeException(StringId::IllegalArgumentException);
}

void Thread::methodCall(bool hasSetArg) {
	auto& f = frames.back();

	size_t valueIndex = f.values.size() - 1;
	auto* setArgValue = (hasSetArg ? f.values[valueIndex--] : nullptr);
	auto* methodValue = f.values[valueIndex--];
	auto* argsValue = (methodValue->methodHasArguments() ? f.values[valueIndex--] : nullptr);

	assert(f.lock != nullptr);
	checkStackOverflow(methodValue->getNode());

	auto* method = (hasSetArg ? methodValue->getSetMethod() : methodValue->getRefMethod());
	assert(method != nullptr);

	size_t callerFrame = frames.size() - 1;
	auto methodValueType = methodValue->getType();

	auto* methodNode = method->node;
	if (methodNode == nullptr) {
		assert(methodValueType == TemporaryObject::Type::Constructor ||
				methodValueType == TemporaryObject::Type::ConstructorCall);
		methodNode = &defaultConstructorNode;
	}

	if (methodValueType == TemporaryObject::Type::Constructor) {
		assert(!hasSetArg);
		auto ref = f.pushTemporary()->setRvalue();
		auto* cl = methodValue->getClass();

		if (cl == Bool::getClassStatic() || cl == Int::getClassStatic() || cl == Float::getClassStatic()) {
			assert(argsValue != nullptr);
			buildPrimitive(methodValue->getNode(), cl, argsValue->getRef().deref<Tuple>().ref(0), ref);
			f.pop(valueIndex + 1, f.values.size() - 1);
			runtime.enqueue(this);
			return;
		}

		auto objectBuilder = cl->getObjectBuilder();
		if (objectBuilder == nullptr)
			ref.allocate<UserObjectBase>(cl);
		else
			(*objectBuilder)(cl, ref);

		f.lock->prepareToMethodCall();
		f.stack.emplace_back(&postConstructorNode, 0);

		constructFrames(cl->getPackage(), methodNode, cl, ref);
		auto& f1 = frames.back();
		f1.stack.emplace_back(&initializeVarsNode, 0);

		runtime.enqueue(this);
		return;
	}

	if (methodNode->subNodes[SubNode::Def_Delete]) {
		errorAtNode(*this, ErrorLevel::Error, methodValue->getNode(), "attempt to call deleted function or method", {});
		errorAtNode(*this, ErrorLevel::Error, methodNode, "definition is here", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}

	if (methodValue->getNativeMethod() != nullptr) {
		auto isNative = invokeNativeMethod(callerFrame,
				methodValue->hasSource() ? &methodValue->getSourceRef().deref<ObjectBase>() : nullptr,
				methodValue, argsValue, [&]() { f.pop(valueIndex + 1, f.values.size() - 1); });
		if (isNative)
			return;
	}

	switch (methodValue->getType()) {
	case TemporaryObject::Type::ConstructorCall: {
		auto ref = methodValue->getSourceRef();
		auto& object = ref.derefWithoutLock<ObjectBase>();
		if (object.getTypeId() == typeId_UserObjectBase &&
				static_cast<UserObjectBase*>(&object)->refUninitializedParents().erase(method->cl) == 0) {
			f.pop(valueIndex + 1, f.values.size());
			f.pushTemporary()->setRvalue();
			runtime.enqueue(this);
			return;
		}
		constructFrames(methodValue->getClass()->getPackage(), methodNode, method->cl, ref);
		break;
	}

	case TemporaryObject::Type::FrameMethod:
		constructFrames(methodValue->getPackage(), methodNode, methodValue->getFrameIndex());
		break;

	case TemporaryObject::Type::ObjectMethod:
		constructFrames(method->cl->getPackage(), methodNode, method->cl, methodValue->getSourceRef());
		break;

	default:
		throw InternalError();
	}

	importMethodArguments(method, argsValue, setArgValue);

	auto& f0 = frames[callerFrame];
	size_t popCount = f0.values.size() - 1 - valueIndex;
	for (size_t i = 0; i < popCount; i++)
		f0.pop();
	f0.lock->prepareToMethodCall();

	checkTransferReq();
	runtime.enqueue(this);
}

void Thread::operatorMethodCall(const OperatorMethod* method) {

	size_t callerFrame = frames.size() - 1;
	constructFrames(method->package, method->node, SIZE_MAX);

	auto& f0 = frames[callerFrame];
	for (size_t i = method->args.size(); i-- > 0; ) {
		allocateReference(method->args[i].name).set(f0.values.back()->getRef());
		f0.pop();
	}
	f0.lock->prepareToMethodCall();

	checkTransferReq();
	runtime.enqueue(this);
}

void Thread::methodCallViaFunctionObject(bool hasSetArg, const Method* method) {
	auto& f = frames.back();

	size_t valueIndex = f.values.size() - 1;
	auto* setArgValue = (hasSetArg ? f.values[valueIndex--] : nullptr);
	auto* methodValue = f.values[valueIndex--];

	if (!methodValue->hasArguments()) {
		errorAtNode(*this, ErrorLevel::Error, methodValue->getNode(), "calling via function object requires arguments", {});
		throw RuntimeException(StringId::IncompleteExpressionException);
	}

	auto* argsValue = f.values[valueIndex--];
	auto& object = methodValue->getRef().deref<FunctionObject>();
	auto& tuple = argsValue->getRef().deref<Tuple>();

	if (method == nullptr) {
		if (hasSetArg) {
			method = object.methodTable->find(nullptr, object.name, StringId::Set, tuple.toArgs(),
					{{StringId::Invalid, getReferClass(setArgValue->getRef())}});
		}
		else
			method = object.methodTable->find(nullptr, object.name, StringId::Invalid, tuple.toArgs(), {});

		if (method == nullptr) {
			errorAtNode(*this, ErrorLevel::Error, methodValue->getNode(), "arguments mismatch", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}
	}

	size_t callerFrame = frames.size() - 1;

	auto* package = object.package;
	if (package == nullptr)
		package = &frames[residentialFrameCount].package;

	frames.emplace_back(*this, *package, residentialFrameCount - 1, package->scope.get());

	for (size_t i = object.scopes.size(); i-- > 0; ) {
		auto scopeType = std::get<0>(object.scopes[i]);
		if (scopeType == ScopeType::Class) {
			frames.emplace_back(*this, *package, frames.size() - 1, std::get<2>(object.scopes[i]), object.ref(i));
			continue;
		}

		frames.emplace_back(*this, *package, frames.size() - 1, scopeType, std::get<1>(object.scopes[i]));
		frames.back().captured = true;
		frames.back().scope->add(StringId::Captured, getId()).set(object.ref(i));
	}

	frames.emplace_back(*this, *package, frames.size() - 1, object.scopeType,
			method->node, frames.size() - callerFrame);

	importMethodArguments(method, argsValue, setArgValue);

	auto& f0 = frames[callerFrame];
	size_t popCount = f0.values.size() - 1 - valueIndex;
	for (size_t i = 0; i < popCount; i++)
		f0.pop();
	f0.lock->prepareToMethodCall();

	checkTransferReq();
	runtime.enqueue(this);
}

void Thread::importMethodArguments(const Method* method, TemporaryObject* argsValue,
		TemporaryObject* setArgValue) {

	assert(method != nullptr);
	auto& f = frames.back();
	auto& emptyTuple = *this->emptyTuple;
	auto& tuple = (argsValue == nullptr ? emptyTuple : argsValue->getRef().deref<Tuple>());
	method->mapArgs(tuple,
			[&](size_t destIndex, const ArgumentDef& arg, size_t srcIndex) {
				(void)destIndex;
				allocateReference(arg.name).set(tuple.ref(srcIndex));

			}, [&](size_t destIndex, const ArgumentDef& arg, const std::vector<size_t>& srcIndexes) {
				(void)destIndex;
				auto ref = allocateReference(arg.name);

				if (arg.type == ArgumentDef::Type::ListVarArg) {
					auto& values = ref.allocate<Array>();
					for (size_t position : srcIndexes)
						values.add(tuple.ref(position));
				}
				else {
					auto& values = ref.allocate<Dict>();
					for (size_t position : srcIndexes) {
						auto& key = tuple.key(position);
						values.add(key.sid == StringId::Invalid ? key.str : sTable->ref(key.sid), tuple.ref(position));
					}
				}
			}, [&](const std::vector<size_t>& destIndexes) {
				for (size_t destIndex : destIndexes) {
					auto& arg = method->args[destIndex];
					allocateReference(arg.name);
					f.stack.emplace_back(&popValueNode, 0);
					f.stack.emplace_back(arg.defaultOp, assignmentOperatorEvaluationPhase);
					f.pushTemporary()->setName(arg.defaultOp->subNodes[0].get(), arg.name);
					f.stack.emplace_back(arg.defaultOp->subNodes[1].get(), 0);
				}
			});

	if (setArgValue != nullptr) {
		auto& subArgs = method->refSubArgs();
		assert(subArgs.size() == 1);
		allocateReference(subArgs[0].name).set(setArgValue->getRef());
	}
}

#define CHATRA_CATCH_NATIVE_EXCEPTION(name)  \
	catch (name& ex) {  \
		errorAtNode(*this, ErrorLevel::Error, method->node, ex.message, {});  \
		throw RuntimeException(StringId::name);  \
	}

template <typename PostProcess>
bool Thread::invokeNativeMethod(size_t callerFrame, ObjectBase* object,
		TemporaryObject* methodValue, TemporaryObject* argsValue, PostProcess postProcess) {

	auto* nativeMethod = methodValue->getNativeMethod();
	if (nativeMethod == nullptr)
		return false;

	// This is required for avoiding conflicts with resuming this thread and postProcess()
	nativeCallThreadId = std::this_thread::get_id();
	std::lock_guard<SpinLock> lock(lockNative);

	auto* method = methodValue->getRefMethod();
	auto ret = frames[callerFrame].pushTemporary()->setRvalue();

	this->callerFrame = callerFrame;  // This is used by native_wait(); not a smart way, but simple
	pauseRequested = false;
	try {
		auto& emptyTuple = *this->emptyTuple;
		(*nativeMethod->method)(nativeMethod->handler, *this, object, method->name, method->subName,
				argsValue == nullptr ? emptyTuple : argsValue->getRef().deref<Tuple>(), ret);
	}
	catch (RuntimeException&) {
		errorAtNode(*this, ErrorLevel::Error, method->node, "exception raised from native method", {});
		throw;
	}
	CHATRA_CATCH_NATIVE_EXCEPTION(PackageNotFoundException)
	CHATRA_CATCH_NATIVE_EXCEPTION(IllegalArgumentException)
	CHATRA_CATCH_NATIVE_EXCEPTION(UnsupportedOperationException)
	CHATRA_CATCH_NATIVE_EXCEPTION(NativeException)
	catch (...) {
		errorAtNode(*this, ErrorLevel::Error, method->node, "exception raised from native method", {});
		throw RuntimeException(StringId::NativeException);
	}

	nativeCallThreadId = std::thread::id();

	postProcess();

	if (pauseRequested) {
		auto& f = frames.back();
		f.lock->prepareToMethodCall();
		checkTransferReq();
		if (!f.stack.empty())
			f.stack.emplace_back(&restoreTemporaryLockNode, 0);
	}
	else
		runtime.enqueue(this);
	return true;
}

void Thread::returnStatement(TemporaryObject* returnValue, bool runFinallyBlock) {
	size_t targetSize = frames.size();
	for (;;) {
		auto& f = frames[targetSize - 1];
		if (f.node->type == NodeType::Finally) {
			errorAtNode(*this, ErrorLevel::Error, frames.back().node, "cannot place return statement inside finally block", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		targetSize -= f.popCount;
		if (f.scope->getScopeType() != ScopeType::Block)
			break;
	}

	auto* value = frames[targetSize - 1].pushTemporary();
	if (returnValue == nullptr)
		value->setRvalue().setNull();
	else {
		assert(returnValue->hasRef());
		value->setRvalue().set(returnValue->getRef());
	}

	pop(targetSize, runFinallyBlock ? FinallyBlockScheme::Run : FinallyBlockScheme::Through);
	runtime.enqueue(this);
}

void Thread::breakStatement(Node* node) {
	auto label = (node->subNodes[0] ? node->subNodes[0]->sid : StringId::Invalid);

	size_t frameIndex;
	for (frameIndex = frames.size() - 1; frameIndex >= residentialFrameCount; ) {
		auto& f = frames[frameIndex];

		if (f.node->type == NodeType::Finally) {
			errorAtNode(*this, ErrorLevel::Error, node, "cannot jump outside finally block with break statement", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		if (f.node->type == NodeType::Switch && label == StringId::Invalid)
			break;
		if (f.isLoop && (label == StringId::Invalid || label == f.label))
			break;

		frameIndex -= f.popCount;
	}
	frameIndex -= frames[frameIndex].popCount;

	if (frameIndex >= residentialFrameCount) {
		pop(frameIndex + 1, FinallyBlockScheme::Run);
		return;
	}

	if (label == StringId::Invalid) {
		errorAtNode(*this, ErrorLevel::Error, node, "\"break\" statement is not in loop or switch statement", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	else {
		errorAtNode(*this, ErrorLevel::Error, node, "label not found", {});
		throw RuntimeException(StringId::IllegalArgumentException);
	}
}

void Thread::continueStatement(Node* node) {
	auto label = (node->subNodes[0] ? node->subNodes[0]->sid : StringId::Invalid);

	size_t frameIndex;
	for (frameIndex = frames.size() - 1; frameIndex >= residentialFrameCount; ) {
		auto& f = frames[frameIndex];

		if (f.node->type == NodeType::Finally) {
			errorAtNode(*this, ErrorLevel::Error, node, "cannot jump outside finally block with continue statement", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		if (f.isLoop && (label == StringId::Invalid || label == f.label))
			break;

		frameIndex -= f.popCount;
	}

	if (frameIndex >= residentialFrameCount) {
		pop(frameIndex + 1, FinallyBlockScheme::IncludesLast);
		frames[frameIndex].phase = frames[frameIndex].phaseOnContinue;
		return;
	}

	if (label == StringId::Invalid) {
		errorAtNode(*this, ErrorLevel::Error, node, "\"continue\" statement is not in loop statement", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	else {
		errorAtNode(*this, ErrorLevel::Error, node, "label not found", {});
		throw RuntimeException(StringId::IllegalArgumentException);
	}
}

void Thread::convertTopToScalar(Node* node) {
	auto* value = frames.back().values.back();
	if (!value->hasRef()) {
		errorAtNode(*this, ErrorLevel::Error, node, "expected expression", {});
		throw RuntimeException(StringId::TypeMismatchException);
	}

	if (getReferClass(value->getRef()) != Tuple::getClassStatic())
		return;

	auto& tuple = value->getRef().deref<Tuple>();
	if (tuple.tupleSize() != 1) {
		errorAtNode(*this, ErrorLevel::Error, node, "expected scalar expression; founds multiple-value Tuple", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	value->getRef().set(tuple.ref(0));
}

void Thread::convertTopToScalar(Node* node, size_t subNodeIndex) {
	convertTopToScalar(node->subNodes[subNodeIndex].get());
}

bool Thread::isNameOrEvaluate(Node* node, size_t subNodeIndex) {
	auto& f = frames.back();
	auto& nodePhase = f.stack.back().second;
	auto& subNode = node->subNodes[subNodeIndex];

	if (subNode->type == NodeType::Name)
		return true;
	else if (subNode->op == Operator::Tuple) {  // a op (b)
		if (subNode->subNodes.size() != 1) {
			errorAtNode(*this, ErrorLevel::Error, subNode.get(), "expected name or String expression", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}
		nodePhase++;
		f.stack.emplace_back(subNode->subNodes[0].get(), 0);
	}
	else {  // a op "b"
		nodePhase++;
		f.stack.emplace_back(subNode.get(), 0);
	}
	return false;
}

bool Thread::getBoolOrThrow(Node* node, size_t subNodeIndex) {
	convertTopToScalar(node, subNodeIndex);
	auto* value = frames.back().values.back();
	if (!value->hasRef() || value->getRef().valueType() != ReferenceValueType::Bool) {
		errorAtNode(*this, ErrorLevel::Error, node->subNodes[subNodeIndex].get(), "expected Bool expression", {});
		throw RuntimeException(StringId::TypeMismatchException);
	}
	return value->getRef().getBool();
}

std::string Thread::getStringOrThrow(Node* node, size_t subNodeIndex) {
	auto& subNode = node->subNodes[subNodeIndex];

	convertTopToScalar(node, subNodeIndex);
	auto* value = frames.back().values.back();
	if (!value->hasRef()) {
		errorAtNode(*this, ErrorLevel::Error, subNode.get(), "expected name or String expression", {});
		throw RuntimeException(StringId::TypeMismatchException);
	}

	auto ref = value->getRef();
	if (ref.isNull() || ref.valueType() != ReferenceValueType::Object
			|| ref.deref<ObjectBase>().getClass() != String::getClassStatic()) {
		errorAtNode(*this, ErrorLevel::Error, subNode.get(), "expected name or String expression", {});
		throw RuntimeException(StringId::TypeMismatchException);
	}

	return ref.deref<String>().getValue();
}

StringId Thread::getStringIdOrThrow(Node* node, size_t subNodeIndex) {
	auto& subNode = node->subNodes[subNodeIndex];
	auto str = getStringOrThrow(node, subNodeIndex);
	auto sid = sTable->find(str);
	if (sid == StringId::Invalid) {
		errorAtNode(*this, ErrorLevel::Error, subNode.get(), R"*(no method or member named "${0}")*", {str});
		throw RuntimeException(StringId::MemberNotFoundException);
	}
	return sid;
}

const Class* Thread::isClassOrEvaluate(Node* node, size_t subNodeIndex) {
	auto& f = frames.back();
	auto& subNode = node->subNodes[subNodeIndex];

	if (subNode->op == Operator::ElementSelection) {
		auto& subNode0 = subNode->subNodes[0];
		auto& subNode1 = subNode->subNodes[1];
		if (subNode0->type == NodeType::Name && subNode1->type == NodeType::Name) {
			auto* cl = f.package.findPackageClass(subNode0->sid, subNode1->sid);
			if (cl != nullptr)
				return cl;
		}
	}

	if (!isNameOrEvaluate(node, subNodeIndex))
		return nullptr;

	auto* cl = f.package.findClass(subNode->sid);
	if (cl == nullptr) {
		errorAtNode(*this, ErrorLevel::Error, subNode.get(), "expected class name", {});
		throw RuntimeException(StringId::ClassNotFoundException);
	}
	return cl;
}

const Class* Thread::getClassOrThrow(Node* node, size_t subNodeIndex) {
	auto& f = frames.back();
	auto& subNode = node->subNodes[subNodeIndex];
	auto str = getStringOrThrow(node, subNodeIndex);

	StringId sidPackage = StringId::Invalid;
	StringId sidName = StringId::Invalid;

	size_t offset = str.find_first_of('.');
	if (offset == std::string::npos)
		sidName = sTable->find(str);
	else {
		sidPackage = sTable->find(str.substr(0, offset));
		if (sidPackage != StringId::Invalid)
			sidName = sTable->find(str.substr(offset + 1));
	}

	const Class* cl = nullptr;
	if (sidName != StringId::Invalid) {
		cl = (sidPackage == StringId::Invalid ?
				f.package.findClass(sidName) : f.package.findPackageClass(sidPackage, sidName));
	}

	if (cl == nullptr) {
		errorAtNode(*this, ErrorLevel::Error, subNode.get(), R"*(no class named "${0}")*", {str});
		throw RuntimeException(StringId::ClassNotFoundException);
	}

	return cl;
}

void Thread::checkTupleAsArgOrThrow(Node* node, const Tuple& tuple) {
	bool hasKey = false;
	for (size_t i = 0; i < tuple.size(); i++) {
		auto& key = tuple.key(i);
		if (!hasKey && !key.isNull())
			hasKey = true;
		else if (hasKey && key.isNull()) {
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[i].get(), "expected parameter with key", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}
	}
}

void Thread::checkTupleAsContainerValueOrThrow(Node* node, const Tuple& tuple) {
	if (tuple.size() == 0)
		return;
	bool hasKey = !tuple.key(0).isNull();
	for (size_t i = 1; i < tuple.size(); i++) {
		if (hasKey != !tuple.key(i).isNull()) {
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[i].get(),
					hasKey ? "expected container initializer value with key" : "expected container initializer value without key", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}
	}
}

Thread::EvaluateValueResult Thread::evaluateTemporaryObject(TemporaryObject* value, bool allocateIfNotFound) {
	if (value->hasRef()) {
		auto ref = value->getRef();
		assert(!ref.requiresLock() || ref.lockedBy() == getId());
		if (value->hasArguments() && getReferClass(ref) == FunctionObject::getClassStatic())
			return EvaluateValueResult::FunctionObject;
	}

	while (value->requiresEvaluate()) {
		auto result = value->evaluate(!allocateIfNotFound);
		if (result == TemporaryObject::EvaluationResult::Failed) {
			allocateReference(value->getName());
			// printf("allocated \"%s\"(%u)\n", sTable->ref(value->getName()).c_str(), static_cast<unsigned>(value->getName()));
			result = value->evaluate();
			assert(result != TemporaryObject::EvaluationResult::Partial);
		}

		if (result == TemporaryObject::EvaluationResult::Succeeded)
			return EvaluateValueResult::Next;

		if (value->hasRef()) {
			if (!lockReference(value))
				return EvaluateValueResult::Suspend;
			if (value->hasArguments() && getReferClass(value->getRef()) == FunctionObject::getClassStatic())
				return EvaluateValueResult::FunctionObject;
		}
	}

	return EvaluateValueResult::Next;
}

Thread::EvaluateValueResult Thread::evaluateValue(EvaluateValueMode mode) {
	auto& f = frames.back();
	auto& nodePhase = f.stack.back().second;
	auto* value = f.values.back();

	auto result = evaluateTemporaryObject(value, false);
	if (result != EvaluateValueResult::Next) {
		if (result == EvaluateValueResult::FunctionObject) {
			nodePhase++;
			methodCallViaFunctionObject(false);
			return EvaluateValueResult::Suspend;
		}
		return result;
	}

	if (!value->isComplete()) {
		switch (mode) {
		case EvaluateValueMode::Class:
			if (value->getType() == TemporaryObject::Type::Class) {
				nodePhase++;
				return EvaluateValueResult::Next;
			}
			break;

		case EvaluateValueMode::ElementSelectionLeft:
			switch (value->getType()) {
			case TemporaryObject::Type::Package:
			case TemporaryObject::Type::Class:
			case TemporaryObject::Type::Super:
			case TemporaryObject::Type::ExplicitSuper:
				nodePhase++;
				return EvaluateValueResult::Next;
			default:
				break;
			}
			break;

		default:
			break;
		}

		errorAtNode(*this, ErrorLevel::Error, value->getNode(), "incomplete expression", {});
		throw RuntimeException(StringId::IncompleteExpressionException);
	}

	if (value->getType() == TemporaryObject::Type::Rvalue) {
		nodePhase++;
		if (getReferClass(value->getRef()) == TemporaryTuple::getClassStatic()) {
			f.stack.emplace_back(&evaluateTupleNode, 0);
			return EvaluateValueResult::StackChanged;
		}
		return EvaluateValueResult::Next;
	}

	if (value->hasRef()) {
		if (!lockReference(value))
			return EvaluateValueResult::Suspend;

		nodePhase++;
		return EvaluateValueResult::Next;
	}

	nodePhase++;
	methodCall(false);
	return EvaluateValueResult::Suspend;
}

Thread::EvaluateValueResult Thread::evaluateTuple() {
	auto& f = frames.back();

	auto& nodePhase = f.stack.back().second;
	auto position = nodePhase / 2;
	auto phase = nodePhase % 2;

	auto& tmp = (*(f.values.end() - 1 - (position + phase)))->getRef().deref<TemporaryTuple>();

	if (tmp.tupleSize() == 0) {
		f.pop();
		f.popTemporaryTuple();
		f.pushEmptyTuple();
		f.stack.pop_back();
		return EvaluateValueResult::StackChanged;
	}

	if (position < tmp.tupleSize()) {
		if (phase == 0) {
			if (position != 0)
				convertTopToScalar(tmp.getNode());
			tmp.extract(position);
			nodePhase++;
		}
		return evaluateValue(EvaluateValueMode::Value);
	}
	convertTopToScalar(tmp.getNode());

	std::vector<std::pair<Tuple::Key, Reference>> args;
	args.reserve(tmp.tupleSize());
	for (size_t i = 0; i < tmp.tupleSize(); i++)
		args.emplace_back(tmp.key(i), (*(f.values.end() - tmp.tupleSize() + i))->getRef());

	auto* value = f.allocateTemporary();
	value->setRvalue().allocate<Tuple>(sTable, args, getId());

	f.pop(f.values.size() - tmp.tupleSize() - 1, f.values.size());
	f.popTemporaryTuple();
	f.push(value);
	f.stack.pop_back();
	return EvaluateValueResult::StackChanged;
}

Thread::EvaluateValueResult Thread::evaluateAndAllocateForAssignment(Node* node, TemporaryObject* value) {
	auto& f = frames.back();
	auto& nodePhase = f.stack.back().second;

	auto result = evaluateTemporaryObject(value, value->getType() == TemporaryObject::Type::Empty && !value->hasArguments());
	if (result == EvaluateValueResult::FunctionObject) {
		nodePhase++;
		return EvaluateValueResult::Next;
	}

	if (result != EvaluateValueResult::Next) {
		assert(result == EvaluateValueResult::Suspend);
		return result;
	}

	if (!value->isComplete()) {
		errorAtNode(*this, ErrorLevel::Error, node, "incomplete expression", {});
		throw RuntimeException(StringId::IncompleteExpressionException);
	}

	nodePhase++;
	return EvaluateValueResult::Next;
}

std::string Thread::getClassName(const Class* cl) {
	if (cl == nullptr)
		return "null";
	auto name = sTable->ref(cl->getName());
	auto* package = cl->getPackage();
	auto packageName = (package == nullptr ? "" : package->name);
	return (packageName.length() == 0 ? "" : packageName + ".") + name;
}

template <typename PrBool, typename PrInt, typename PrFloat>
bool Thread::standardUnaryOperator(PrBool prBool, PrInt prInt, PrFloat prFloat) {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	default: {
		auto r0 = (*(f.values.end() - 1))->getRef();
		auto r0Type = r0.valueType();

		if (r0Type != ReferenceValueType::Object) {
			auto* value = f.allocateTemporary();
			auto result = value->setRvalue();
			try {
				switch (r0Type) {
				case ReferenceValueType::Bool:  result.setBool(prBool(r0.getBool()));  break;
				case ReferenceValueType::Int:  result.setInt(prInt(r0.getInt()));  break;
				case ReferenceValueType::Float:  result.setFloat(prFloat(r0.getFloat()));  break;
				default:
					throw InternalError();
				}
			}
			catch (const OperatorNotSupportedException&) {
				f.recycle(value);
				throw RuntimeException(StringId::UnsupportedOperationException);
			}
			catch (...) {
				f.recycle(value);
				throw;
			}

			f.pop();
			f.push(value);
			f.stack.pop_back();
			return true;
		}

		auto* cl0 = getReferClass(r0);
		auto* method = runtime.operators.find(node->op, cl0);
		if (method == nullptr) {
			Node* errorNode = node;
			if (errorNode->tokens.empty())
				errorNode = (f.stack.size() >= 2 ? (f.stack.end() - 2)->first : nullptr);

			errorAtNode(*this, ErrorLevel::Error, errorNode, "${0} operator with operand ${1} is not defined",
					{getOpDescription(node->op), getClassName(cl0)});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		f.stack.pop_back();
		operatorMethodCall(method);
		return false;
	}
	}
}

template <typename PrBool, typename PrInt, typename PrFloat>
bool Thread::unaryAssignmentOperator(bool prefix, PrBool prBool, PrInt prInt, PrFloat prFloat) {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  nodePhase++;  f.duplicateTop();  CHATRA_FALLTHROUGH;
	case 2:  CHATRA_EVALUATE_VALUE;
	case 3: {
		auto r1 = f.values.back()->getRef();
		auto r1Type = r1.valueType();

		if (r1Type != ReferenceValueType::Object) {
			nodePhase = assignmentOperatorProcessPhase;

			if (!prefix) {
				auto* v1Copy = f.allocateTemporary();
				v1Copy->setRvalue().set(r1);
				f.push(f.values.size() - ((*(f.values.end() - 2))->hasArguments() ? 3 : 2), v1Copy);
				f.stack.emplace(f.stack.end() - 1, &postPrefixUnaryOperatorNode, 0);
			}

			switch (r1.valueType()) {
			case ReferenceValueType::Bool:  r1.setBool(prBool(r1.getBool()));  break;
			case ReferenceValueType::Int:  r1.setInt(prInt(r1.getInt()));  break;
			case ReferenceValueType::Float:  r1.setFloat(prFloat(r1.getFloat()));  break;
			default:
				throw InternalError();
			}

			return assignmentOperator();
		}

		auto* cl1 = getReferClass(r1);
		auto* method = runtime.operators.find(node->op, cl1);
		if (method == nullptr) {
			Node* errorNode = node;
			if (errorNode->tokens.empty())
				errorNode = (f.stack.size() >= 2 ? (f.stack.end() - 2)->first : nullptr);

			errorAtNode(*this, ErrorLevel::Error, errorNode, "${0} operator with operand ${1} is not defined",
					{getOpDescription(node->op), getClassName(cl1)});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		f.pop(f.values.size() - ((*(f.values.end() - 2))->hasArguments() ? 3 : 2), f.values.size() - 1);
		f.stack.pop_back();
		operatorMethodCall(method);
		return false;
	}
	default:
		return assignmentOperator();
	}
}

template <typename Process>
bool Thread::binaryOperator(Process process) {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	case 2:  nodePhase++;  f.stack.emplace_back(node->subNodes[1].get(), 0);  return true;
	case 3:  CHATRA_EVALUATE_VALUE;
	default: {
		assert(nodePhase == binaryOperatorProcessPhase);
		auto r0 = (*(f.values.end() - 2))->getRef();
		auto r1 = (*(f.values.end() - 1))->getRef();

		auto r0Type = r0.valueType();
		auto r1Type = r1.valueType();

		try {
			auto* value = process(f, r0, r0Type, r1, r1Type);
			if (value != nullptr) {
				f.pop();
				f.pop();
				f.push(value);
				f.stack.pop_back();
				return true;
			}
		}
		catch (const OperatorNotSupportedException&) {
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		auto* cl0 = getReferClass(r0);
		auto* cl1 = getReferClass(r1);
		auto* method = runtime.operators.find(node->op, cl0, cl1);
		if (method == nullptr) {
			Node* errorNode = node;
			if (errorNode->tokens.empty())
				errorNode = (f.stack.size() >= 2 ? (f.stack.end() - 2)->first : nullptr);

			errorAtNode(*this, ErrorLevel::Error, errorNode, "${0} operator with operands ${1} and ${2} is not defined",
					{getOpDescription(node->op), getClassName(cl0), getClassName(cl1)});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		f.stack.pop_back();
		operatorMethodCall(method);
		return false;
	}
	}
}

template <typename PrBool, typename PrInt, typename PrFloat>
bool Thread::standardBinaryOperator(PrBool prBool, PrInt prInt, PrFloat prFloat) {
	return binaryOperator([&](Frame& f, Reference r0, ReferenceValueType r0Type,
			Reference r1, ReferenceValueType r1Type) -> TemporaryObject* {

		if (r0Type != r1Type || r0Type == ReferenceValueType::Object)
			return nullptr;

		auto* value = f.allocateTemporary();
		auto result = value->setRvalue();
		try {
			switch (r0Type) {
			case ReferenceValueType::Bool:  result.setBool(prBool(r0.getBool(), r1.getBool()));  break;
			case ReferenceValueType::Int:  result.setInt(prInt(r0.getInt(), r1.getInt()));  break;
			case ReferenceValueType::Float:  result.setFloat(prFloat(r0.getFloat(), r1.getFloat()));  break;
			default:
				throw InternalError();
			}
		}
		catch (...) {
			f.recycle(value);
			throw;
		}
		return value;
	});
}

template <typename PrInt>
bool Thread::shiftOrBitwiseOperator(PrInt prInt) {
	return binaryOperator([&](Frame& f, Reference r0, ReferenceValueType r0Type,
			Reference r1, ReferenceValueType r1Type) -> TemporaryObject* {

		if (r0Type != ReferenceValueType::Int || r1Type != ReferenceValueType::Int)
			return nullptr;

		auto* value = f.allocateTemporary();
		try {
			value->setRvalue().setInt(prInt(r0.getInt(), r1.getInt()));
		}
		catch (...) {
			f.recycle(value);
			throw;
		}
		return value;
	});
}

template <typename PrBool, typename PrInt, typename PrFloat>
bool Thread::comparisonOperator(PrBool prBool, PrInt prInt, PrFloat prFloat) {
	return binaryOperator([&](Frame& f, Reference r0, ReferenceValueType r0Type,
			Reference r1, ReferenceValueType r1Type) -> TemporaryObject* {

		if (r0Type != r1Type || r0Type == ReferenceValueType::Object)
			return nullptr;

		auto* value = f.allocateTemporary();
		auto result = value->setRvalue();
		try {
			switch (r0Type) {
			case ReferenceValueType::Bool:  result.setBool(prBool(r0.getBool(), r1.getBool()));  break;
			case ReferenceValueType::Int:  result.setBool(prInt(r0.getInt(), r1.getInt()));  break;
			case ReferenceValueType::Float:  result.setBool(prFloat(r0.getFloat(), r1.getFloat()));  break;
			default:
				throw InternalError();
			}
		}
		catch (...) {
			f.recycle(value);
			throw;
		}
		return value;
	});
}

template <bool equal, class Type>
static constexpr bool equality(Type a, Type b) {
	return equal ? a == b : a != b;
}

template <bool equal>
bool Thread::equalityOperator() {
	return binaryOperator([&](Frame& f, Reference r0, ReferenceValueType r0Type,
			Reference r1, ReferenceValueType r1Type) -> TemporaryObject* {

		if (r0.isNull() || r1.isNull()) {
			auto* value = f.allocateTemporary();
			value->setRvalue().setBool(equality<equal>(r0.isNull(), r1.isNull()));
			return value;
		}

		if (r0Type != r1Type || r0Type == ReferenceValueType::Object)
			return nullptr;

		auto* value = f.allocateTemporary();
		auto result = value->setRvalue();
		switch (r0Type) {
		case ReferenceValueType::Bool:  result.setBool(equality<equal>(r0.getBool(), r1.getBool()));  break;
		case ReferenceValueType::Int:  result.setBool(equality<equal>(r0.getInt(), r1.getInt()));  break;
		case ReferenceValueType::Float:  result.setBool(equality<equal>(r0.getFloat(), r1.getFloat()));  break;
		default:
			throw InternalError();
		}

		return value;
	});
}

template <typename PrEvaluateRight, typename PrBoolLeft, typename PrBoolRight>
bool Thread::logicalOperator(PrEvaluateRight prEvaluateRight, PrBoolLeft prBoolLeft, PrBoolRight prBoolRight) {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	case 2: {
		auto r0 = (*(f.values.end() - 1))->getRef();
		auto r0Type = r0.valueType();
		if (r0Type != ReferenceValueType::Bool) {
			errorAtNode(*this, ErrorLevel::Error, node, "${0} operator can only take Bool operands; left operand is ${1}",
					{getOpDescription(node->op), getClassName(getReferClass(r0))});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		if (prEvaluateRight(r0.getBool())) {
			nodePhase++;
			f.stack.emplace_back(node->subNodes[1].get(), 0);
		}
		else {
			auto* value = f.allocateTemporary();
			value->setRvalue().setBool(prBoolLeft(r0.getBool()));
			f.pop();
			f.push(value);
			f.stack.pop_back();
		}
		return true;
	}

	case 3:  CHATRA_EVALUATE_VALUE;
	default: {
		auto r0 = (*(f.values.end() - 2))->getRef();
		auto r1 = (*(f.values.end() - 1))->getRef();
		auto r1Type = r1.valueType();
		if (r1Type != ReferenceValueType::Bool) {
			errorAtNode(*this, ErrorLevel::Error, node, "${0} operator can only take Bool operands; right operand is ${1}",
					{getOpDescription(node->op), getClassName(getReferClass(r1))});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}

		auto* value = f.allocateTemporary();
		value->setRvalue().setBool(prBoolRight(r0.getBool(), r1.getBool()));
		f.pop();
		f.pop();
		f.push(value);
		f.stack.pop_back();
		return true;
	}
	}
}

template <typename PrBool>
bool Thread::logicalOperator(PrBool prBool) {
	return binaryOperator([&](Frame& f, Reference r0, ReferenceValueType r0Type,
			Reference r1, ReferenceValueType r1Type) -> TemporaryObject* {

		if (r0Type != ReferenceValueType::Bool || r1Type != ReferenceValueType::Bool)
			return nullptr;

		auto* value = f.allocateTemporary();
		value->setRvalue().setBool(prBool(r0.getBool(), r1.getBool()));
		return value;
	});

}

bool Thread::expressionOperator() {
	auto& f = frames.back();
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  CHATRA_EVALUATE_VALUE;
	default:
		f.stack.pop_back();
		return true;
	}
}

bool Thread::containerOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	// extract container values
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[SubNode::Container_Values].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	case 2:
		nodePhase++;
		checkTupleAsContainerValueOrThrow(node->subNodes[SubNode::Container_Values].get(),
				f.values.back()->getRef().deref<Tuple>());
		CHATRA_FALLTHROUGH;

	// extract constructor argument if present
	case 3:
		if (!node->subNodes[SubNode::Container_Args]) {
			nodePhase = 6;
			f.pushEmptyTuple();
			return true;
		}
		nodePhase++;
		f.stack.emplace_back(node->subNodes[SubNode::Container_Args].get(), 0);
		return true;
	case 4:  CHATRA_EVALUATE_VALUE;
	case 5:
		nodePhase++;
		checkTupleAsArgOrThrow(node->subNodes[SubNode::Container_Args].get(),
				f.values.back()->getRef().deref<Tuple>());
		CHATRA_FALLTHROUGH;

	// call constructor
	case 6: {
		auto& nCl = node->subNodes[SubNode::Container_Class];
		nodePhase++;
		if (!nCl) {
			assert(!node->subNodes[SubNode::Container_Args]);
			auto& tuple = (*(f.values.end() - 2))->getRef().deref<Tuple>();
			f.pushTemporary()->setName(node, tuple.size() == 0 || tuple.key(0).isNull() ? StringId::Array : StringId::Dict);
		}
		else if (nCl->type == NodeType::Name && nCl->sid == StringId::a)
			f.pushTemporary()->setName(nCl.get(), StringId::Array);
		else if (nCl->type == NodeType::Name && nCl->sid == StringId::d)
			f.pushTemporary()->setName(nCl.get(), StringId::Dict);
		else {
			f.stack.emplace_back(nCl.get(), 0);
			return true;
		}
		CHATRA_FALLTHROUGH;
	}
	case 7: {
		auto& nArgs = node->subNodes[SubNode::Container_Args];
		nodePhase++;
		f.values.back()->addArgument(nArgs ? nArgs.get() : node, (*(f.values.end() - 2))->getRef().deref<Tuple>().toArgs());
		CHATRA_FALLTHROUGH;
	}
	case 8:  CHATRA_EVALUATE_VALUE;

	// call add() method
	default: {
		auto* v0 = *(f.values.end() - 2);
		auto* v1 = *(f.values.end() - 1);
		v1->selectElement(node, StringId::add);
		v1->addArgument(node->subNodes[SubNode::Container_Values].get(), v0->getRef().deref<Tuple>().toArgs());
		f.stack.pop_back();
		return true;
	}
	}
}

bool Thread::callOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[1].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	case 2:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 3:
		// To evaluate the case of "foo(...)(...)" which "foo(...)" returns an object
		if (!f.values.back()->hasArguments())
			nodePhase++;
		else
			CHATRA_EVALUATE_VALUE;
	default: {
		auto& tuple = (*(f.values.end() - 2))->getRef().deref<Tuple>();
		checkTupleAsArgOrThrow(node->subNodes[1].get(), tuple);
		f.values.back()->addArgument(node->subNodes[1].get(), tuple.toArgs());
		f.stack.pop_back();
		return true;
	}
	}
}

bool Thread::elementSelectionOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE_MODE(ElementSelectionLeft);
	case 2:
		if (isNameOrEvaluate(node, 1)) {
			f.values.back()->selectElement(node->subNodes[1].get(), node->subNodes[1]->sid);
			f.stack.pop_back();
		}
		return true;
	case 3:  CHATRA_EVALUATE_VALUE;
	default: {
		auto sid = getStringIdOrThrow(node, 1);
		f.pop();
		f.values.back()->selectElement(node->subNodes[1].get(), sid);
		f.stack.pop_back();
		return true;
	}
	}
}

bool Thread::functionObjectOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  nodePhase++;  f.values.back()->addArgument(nullptr, {StringId::AnyArgs});  CHATRA_FALLTHROUGH;
	default:  {
		auto* value = f.values.back();
		auto result = evaluateTemporaryObject(value, false);
		if (result != EvaluateValueResult::Next) {
			if (result == EvaluateValueResult::FunctionObject) {
				errorAtNode(*this, ErrorLevel::Error, node, "cannot take reference to function or method reference", {});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			return false;
		}

		auto* method = value->getRefMethod();
		if (method == nullptr) {
			errorAtNode(*this, ErrorLevel::Error, node, "expected function or method name", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}

		auto* retValue = f.allocateTemporary();
		if (value->getType() == TemporaryObject::Type::FrameMethod) {
			retValue->setRvalue().allocate<FunctionObject>(*this, value->getPackage(), value->getFrameIndex(),
					value->getMethodTable(), method->name);
		}
		else {
			assert(value->getType() == TemporaryObject::Type::ObjectMethod);
			retValue->setRvalue().allocate<FunctionObject>(*this, method->cl->getPackage(), value->getSourceRef(),
					value->getMethodTable(), method->name);
		}
		f.pop();
		f.push(retValue);
		f.stack.pop_back();
		return true;
	}
	}
}

bool Thread::asyncOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0: {
		auto* callNode = node->subNodes[0].get();
		if (callNode->op != Operator::Subscript && callNode->op != Operator::Call) {
			errorAtNode(*this, ErrorLevel::Error, callNode, "expected subscripting or function call", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		nodePhase++;
		f.stack.emplace_back(callNode, 0);
		return true;
	}

	case 1: {
		auto* value = f.values.back();
		switch (evaluateTemporaryObject(value, false)) {
		case EvaluateValueResult::Suspend:  return false;
		case EvaluateValueResult::StackChanged:  return true;

		case EvaluateValueResult::FunctionObject:
			break;

		default: {
			auto valueType = value->getType();
			if (valueType != TemporaryObject::Type::FrameMethod && valueType != TemporaryObject::Type::ObjectMethod) {
				errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(), "expected subscripting or function call", {});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			break;
		}
		}
		nodePhase++;
		CHATRA_FALLTHROUGH;
	}

	case 2: {
		auto* value = f.values.back();
		if (value->hasRef() && !lockReference(value))
			return false;
		nodePhase++;
		CHATRA_FALLTHROUGH;
	}

	default: {
		auto* value = f.values.back();
		auto* argsValue = *(f.values.end() - 2);

		auto* method = value->getRefMethod();
		bool isFunctionObject = false;

		if (method == nullptr) {
			auto& object = value->getRef().deref<FunctionObject>();
			auto& tuple = argsValue->getRef().deref<Tuple>();
			method = object.methodTable->find(nullptr, object.name, StringId::Invalid, tuple.toArgs(), {});
			if (method == nullptr) {
				errorAtNode(*this, ErrorLevel::Error, value->getNode(), "arguments mismatch", {});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			isFunctionObject = true;
		}

		auto& targetThread = runtime.createThread(instance,
				*(method->cl == nullptr ? &f.package : method->cl->getPackage()), method->node);
		auto& f1 = targetThread.frames.back();
		f1.stack.emplace_back(&returnFromAsyncNode, 0);
		auto refAsync = f1.pushTemporary()->setRvalue();
		refAsync.allocate<Async>();
		f1.pushTemporary()->copyFrom(*argsValue);
		f1.pushTemporary()->copyFrom(*value);

		f.pop();
		f.pop();
		f.pushTemporary()->setRvalue().set(refAsync);
		f.stack.pop_back();

		if (isFunctionObject)
			targetThread.methodCallViaFunctionObject(false, method);
		else
			targetThread.methodCall(false);
		return true;
	}
	}
}

bool Thread::varArgOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	default:
		f.values.back()->addVarArg(node);
		f.stack.pop_back();
		return true;
	}
}

bool Thread::instanceOfOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	const Class* cl = nullptr;
	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	case 2:
		if (nullptr != (cl = isClassOrEvaluate(node, 1)))
			break;
		return true;
	case 3:  CHATRA_EVALUATE_VALUE_MODE(Class);
	default:
		cl = getClassOrThrow(node, 1);
		f.pop();
		break;
	}

	assert(cl != nullptr);
	auto ref = f.values.back()->getRef();
	bool result;
	switch (ref.valueType()) {
	case ReferenceValueType::Bool:  result = (cl == Bool::getClassStatic());  break;
	case ReferenceValueType::Int:  result = (cl == Int::getClassStatic());  break;
	case ReferenceValueType::Float:  result = (cl == Float::getClassStatic());  break;
	default:
		result = ref.isNull() ? false : cl->isAssignableFrom(ref.deref<ObjectBase>().getClass());
		break;
	}
	f.pop();
	f.pushTemporary()->setRvalue().setBool(result);
	f.stack.pop_back();
	return true;
}

bool Thread::conditionalOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  CHATRA_EVALUATE_VALUE;
	default: {
		auto ref = f.values.back()->getRef();
		if (ref.valueType() != ReferenceValueType::Bool) {
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(),
					"conditional operator requires Bool expression", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}
		size_t nextIndex = (ref.getBool() ? 1 : 2);
		f.pop();
		f.stack.pop_back();
		f.stack.emplace_back(node->subNodes[nextIndex].get(), 0);
		return true;
	}
	}
}

bool Thread::tupleOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	constexpr size_t prologuePhases = 1;
	constexpr size_t phasesPerElem = 1U << 3U;  // = 8

	switch (nodePhase) {
	case 0:
		nodePhase++;
		f.subStack.push_back(f.values.size());
		f.pushTemporary()->setRvalue().set(f.pushTemporaryTuple()).initialize(frames.size() - 1, node);
		CHATRA_FALLTHROUGH;
	default:
		break;
	}

	auto& tmp = f.values[f.subStack.back()]->getRef().deref<TemporaryTuple>();

	while (nodePhase < prologuePhases + node->subNodes.size() * phasesPerElem) {
		size_t phase = nodePhase - prologuePhases;
		size_t position = phase / phasesPerElem;
		size_t subPhase = phase % phasesPerElem;

		auto* subNode = node->subNodes[position].get();
		if (subNode->op == Operator::Pair) {
			auto& leftNode = subNode->subNodes[0];
			if (leftNode->type == NodeType::Name) {
				switch (subPhase) {
				case 0:  nodePhase++;  f.stack.emplace_back(subNode->subNodes[1].get(), 0);  return true;
				default:
					nodePhase += phasesPerElem - subPhase;
					tmp.capture(leftNode->sid);
				}
			}
			else if (leftNode->type == NodeType::Literal  && leftNode->literalValue->type == LiteralType::String) {
				// TODO MLString
				switch (subPhase) {
				case 0:  nodePhase++;  f.stack.emplace_back(subNode->subNodes[1].get(), 0);  return true;
				default:
					if (leftNode->literalValue->vString.empty()) {
						errorAtNode(*this, ErrorLevel::Error, leftNode.get(), "expected non-empty String literal", {});
						throw RuntimeException(StringId::IllegalArgumentException);
					}
					nodePhase += phasesPerElem - subPhase;
					tmp.capture(leftNode->literalValue->vString);
				}
			}
			else if (leftNode->op == Operator::TupleExtraction) {
				switch (subPhase) {
				case 0:  nodePhase++;  f.stack.emplace_back(subNode->subNodes[1].get(), 0);  return true;
				case 1:  nodePhase++;  f.stack.emplace_back(leftNode->subNodes[0].get(), 0);  return true;
				case 2:  CHATRA_EVALUATE_VALUE;

				default: {
					auto ref = f.values.back()->getRef();
					if (getReferClass(ref) != String::getClassStatic()) {
						errorAtNode(*this, ErrorLevel::Error, leftNode->subNodes[0].get(), "expected String expression", {});
						throw RuntimeException(StringId::IllegalArgumentException);
					}
					auto str = ref.deref<String>().getValue();
					if (str.empty()) {
						errorAtNode(*this, ErrorLevel::Error, leftNode->subNodes[0].get(), "expected non-empty String expression", {});
						throw RuntimeException(StringId::IllegalArgumentException);
					}
					StringId sid = sTable->find(str);

					f.pop();
					if (sid == StringId::Invalid)
						tmp.capture(str);
					else
						tmp.capture(sid);

					nodePhase = prologuePhases + (position + 1) * phasesPerElem;
				}
				}
			}
			else {
				errorAtNode(*this, ErrorLevel::Error, leftNode.get(), "expected expression or pair operator(:)", {});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
		}
		else if (subNode->op == Operator::TupleExtraction) {
			switch (subPhase) {
			case 0:
				f.pushEmptyTuple();  // arguments for toArray/toDict
				nodePhase++;
				f.stack.emplace_back(subNode->subNodes[0].get(), 0);
				return true;

			case 1:  CHATRA_EVALUATE_VALUE;

			case 2: {
				auto ref = f.values.back()->getRef();
				if (ref.valueType() != ReferenceValueType::Object || ref.isNull()) {
					errorAtNode(*this, ErrorLevel::Error, subNode->subNodes[0].get(), "expected object expression", {});
					throw RuntimeException(StringId::IllegalArgumentException);
				}

				auto* cl = ref.deref<ObjectBase>().getClass();
				if (cl == Tuple::getClassStatic()) {
					tmp.copyFrom(ref.deref<Tuple>());
					f.pop();  // +1 pop for (empty) arguments
					break;
				}

				nodePhase++;
				if (cl->refMethods().find(nullptr, StringId::toDict, StringId::Invalid, {}, {}) != nullptr)
					f.values.back()->selectElement(subNode, StringId::toDict);
				else
					f.values.back()->selectElement(subNode, StringId::toArray);

				f.values.back()->addArgument(subNode, {});
				CHATRA_FALLTHROUGH;
			}

			case 3:  CHATRA_EVALUATE_VALUE;

			default: {
				auto ref = f.values.back()->getRef();
				if (ref.valueType() != ReferenceValueType::Object || ref.isNull()) {
					errorAtNode(*this, ErrorLevel::Error, subNode, "toArray() or toDict() should return an object", {});
					throw RuntimeException(StringId::IllegalArgumentException);
				}

				auto* cl = ref.deref<ObjectBase>().getClass();
				if (Array::getClassStatic()->isAssignableFrom(cl))
					tmp.copyFrom(ref.deref<Array>());
				else if (Dict::getClassStatic()->isAssignableFrom(cl))
					tmp.copyFrom(ref.deref<Dict>(), subNode);
				else {
					errorAtNode(*this, ErrorLevel::Error, subNode, "returned value from toArray() or toDict() is not instance of Array nor Dict", {});
					throw RuntimeException(StringId::IllegalArgumentException);
				}
			}
			}

			f.pop();
			nodePhase = prologuePhases + (position + 1) * phasesPerElem;
		}
		else {
			switch (subPhase) {
			case 0:  nodePhase++;  f.stack.emplace_back(subNode, 0);  return true;
			default:
				nodePhase += phasesPerElem - subPhase;
				tmp.capture(StringId::Invalid);
			}
		}
	}

	f.subStack.pop_back();
	f.stack.pop_back();
	return true;
}

bool Thread::assignmentOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	constexpr size_t phaseReference = 10;
	constexpr size_t phaseMethod = 20;
	constexpr size_t phaseFunctionObject = 30;
	constexpr size_t phaseTuple = 40;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  nodePhase++;  f.stack.emplace_back(node->subNodes[1].get(), 0);  return true;
	case assignmentOperatorEvaluationPhase:  CHATRA_EVALUATE_VALUE;
	case assignmentOperatorEvaluationPhase + 1:  nodePhase = assignmentOperatorProcessPhase;  CHATRA_FALLTHROUGH;
	case assignmentOperatorProcessPhase: {
		// Note that node->subNode[1] cannot be accessed under specific situation
		// because following logic is also used by unaryAssignmentOperator().
		auto* v0 = *(f.values.end() - 2);
		auto* v1 = *(f.values.end() - 1);

		// Tuple <- Tuple: accept
		// Scalar <- Tuple: accept only if Tuple contains a single value
		// Tuple <- Scalar: reject
		// Scaler <- Scalar: accept

		bool v0IsTuple = (v0->getType() == TemporaryObject::Type::Rvalue &&
				getReferClass(v0->getRef()) == TemporaryTuple::getClassStatic());
		bool v1IsTuple = (getReferClass(v1->getRef()) == Tuple::getClassStatic());

		if (!v0IsTuple && v1IsTuple) {
			auto& tuple = v1->getRef().deref<Tuple>();
			if (tuple.tupleSize() != 1) {
				errorAtNode(*this, ErrorLevel::Error, node, "left side is not tuple even through value of right side is tuple", {});
				throw RuntimeException(StringId::UnsupportedOperationException);
			}
			v1->getRef().set(tuple.ref(0));
			v1IsTuple = false;
		}

		if (v0IsTuple) {
			if (!v1IsTuple) {
				errorAtNode(*this, ErrorLevel::Error, node, "right side is not tuple", {});
				throw RuntimeException(StringId::UnsupportedOperationException);
			}

			std::vector<TupleAssignmentMap::Element> map;
			v0->getRef().deref<TemporaryTuple>().toArgumentMatcher().mapReturns(v1->getRef().deref<Tuple>(),
					[&](size_t destIndex, const ArgumentDef& arg, size_t srcIndex) {
						map.emplace_back(destIndex, srcIndex, arg.type);

					}, [&](size_t destIndex, const ArgumentDef& arg, std::vector<size_t> srcIndexes) {
						map.emplace_back(destIndex, std::move(srcIndexes), arg.type);

					}, [&](const std::vector<size_t>& destIndexes) {
						std::vector<size_t> empty;
						for (size_t destIndex : destIndexes)
							map.emplace_back(destIndex, empty, ArgumentDef::Type::List);
					});

			f.subStack.push_back(f.values.size());
			f.pushTemporary()->setRvalue().allocate<TupleAssignmentMap>(std::move(map));
			nodePhase = phaseTuple;
			return true;
		}

		v0->addAssignment(node, {StringId::Invalid, getReferClass(v1->getRef())});
		nodePhase++;
		CHATRA_FALLTHROUGH;
	}

	case assignmentOperatorProcessPhase + 1:
		if (evaluateAndAllocateForAssignment(node->subNodes[0].get(), *(f.values.end() - 2)) != EvaluateValueResult::Next)
			return false;
		CHATRA_FALLTHROUGH;

	case assignmentOperatorProcessPhase + 2: {
		auto* v0 = *(f.values.end() - 2);
		switch (v0->getType()) {
		case TemporaryObject::Type::ScopeRef:
		case TemporaryObject::Type::ObjectRef:
			nodePhase = phaseReference;
			return true;

		case TemporaryObject::Type::FrameMethod:
		case TemporaryObject::Type::ObjectMethod:
			if (v0->getSetMethod() != nullptr) {
				nodePhase = phaseMethod;
				return true;
			}
			CHATRA_FALLTHROUGH;

		default:
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(), "expression is not assignable", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
	}

	case phaseReference: { // Reference; e.g. "a = 1", "b.c = 2"
		auto* v0 = *(f.values.end() - 2);
		auto* v1 = *(f.values.end() - 1);
		if (!lockReference(v0))
			return false;
		auto ref = v0->getRef();
		if (ref.isConst()) {
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(), "cannot modify constant reference", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		if (v0->hasArguments() && getReferClass(ref) == FunctionObject::getClassStatic()) {
			nodePhase = phaseFunctionObject;
			return true;
		}

		ref.set(v1->getRef());
		f.pop();
		f.stack.pop_back();
		return true;
	}

	case phaseMethod: {  // Method; e.g. "a.b(...) = 1" where a is an instance of some class and its class has a method "def b(...).set(r)"
		f.stack.pop_back();
		methodCall(true);
		return false;
	}

	case phaseFunctionObject: {
		f.stack.pop_back();
		methodCallViaFunctionObject(true);
		return false;
	}

	default: {
		if (nodePhase >= phaseTuple) {  // Tuple; e.g. "a, b = foo()"
			auto& tmp = f.values[f.subStack.back() - 2]->getRef().deref<TemporaryTuple>();
			auto& tuple = f.values[f.subStack.back() - 1]->getRef().deref<Tuple>();
			auto& map = f.values[f.subStack.back()]->getRef().deref<TupleAssignmentMap>();

			constexpr size_t phasesPerElem = 1U << 2U;  // = 4
			for (;;) {
				size_t phase = nodePhase - phaseTuple;
				size_t position = phase / phasesPerElem;
				size_t subPhase = phase % phasesPerElem;

				switch (subPhase) {
				case 0: {
					if (map.map.size() <= position) {
						f.popTemporaryTuple();
						f.pop();
						f.pop();
						f.pop();
						f.pushTemporary()->setRvalue().setNull();
						f.stack.pop_back();
						f.subStack.pop_back();
						return true;
					}
					auto& e = map.map[position];
					tmp.extract(e.destIndex);

					const Class* cl;
					switch (e.type) {
					case ArgumentDef::Type::ListVarArg:  cl = Array::getClassStatic();  break;
					case ArgumentDef::Type::DictVarArg:  cl = Dict::getClassStatic();  break;
					default:  cl = (e.sourceIndexes.empty() ? nullptr : getReferClass(tuple.ref(e.sourceIndexes[0])));  break;
					}
					if (cl != nullptr)
						f.values.back()->addAssignment(node, {StringId::Invalid, cl});
					nodePhase++;
					CHATRA_FALLTHROUGH;
				}

				case 1:
					if (evaluateAndAllocateForAssignment(node->subNodes[0].get(), f.values.back()) != EvaluateValueResult::Next)
						return false;
					CHATRA_FALLTHROUGH;

				case 2: {
					auto* v0 = f.values.back();
					switch (v0->getType()) {
					case TemporaryObject::Type::ScopeRef:
					case TemporaryObject::Type::ObjectRef:
						break;

					case TemporaryObject::Type::FrameMethod:
					case TemporaryObject::Type::ObjectMethod:
						if (v0->getSetMethod() != nullptr)
							break;
						CHATRA_FALLTHROUGH;

					default:
						errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(), "expression is not assignable", {});
						throw RuntimeException(StringId::UnsupportedOperationException);
					}
					nodePhase++;
					CHATRA_FALLTHROUGH;
				}

				default: {
					auto& e = map.map[position];
					auto srcRef = f.pushTemporary()->setRvalue();
					switch (e.type) {
					case ArgumentDef::Type::ListVarArg: {
						auto& array = srcRef.allocate<Array>();
						for (size_t sourceIndex : e.sourceIndexes)
							array.add(tuple.ref(sourceIndex));
						break;
					}

					case ArgumentDef::Type::DictVarArg: {
						auto& dict = srcRef.allocate<Dict>();
						for (size_t sourceIndex : e.sourceIndexes) {
							auto& key = tuple.key(sourceIndex);
							dict.add(key.sid == StringId::Invalid ? key.str : sTable->ref(key.sid),
									tuple.ref(sourceIndex));
						}
						break;
					}

					default:
						if (e.sourceIndexes.empty())
							srcRef.setNull();
						else
							srcRef.set(tuple.ref(e.sourceIndexes[0]));
						break;
					}

					auto* v0 = *(f.values.end() - 2);
					if (v0->hasRef()) {
						if (!lockReference(v0))
							return false;
						auto ref = v0->getRef();
						if (ref.isConst()) {
							errorAtNode(*this, ErrorLevel::Error, node->subNodes[0].get(), "cannot modify constant reference", {});
							throw RuntimeException(StringId::UnsupportedOperationException);
						}
						if (v0->hasArguments() && getReferClass(ref) == FunctionObject::getClassStatic()) {
							nodePhase = phaseTuple + (position + 1) * phasesPerElem;
							methodCallViaFunctionObject(true);
							return false;
						}

						ref.set(srcRef);
						f.pop();
						f.pop();
						nodePhase = phaseTuple + (position + 1) * phasesPerElem;
					}
					else {
						nodePhase = phaseTuple + (position + 1) * phasesPerElem;
						methodCall(true);
						return false;
					}
				}
				}
			}
		}
		throw InternalError();
	}
	}
}

bool Thread::defineAndAssignmentOperator() {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	if (nodePhase == 0) {
		// exclude "var foo: bar" case
		if (f.stack.size() == 1 || (f.stack.cend() - 2)->first != &varStatementMarkerNode) {
			StringId sid = node->subNodes[0]->sid;
			if (!f.scope->has(sid)) {
				allocateReference(sid);
				// printf("allocated \"%s\"(%u)\n", sTable->ref(sid).c_str(), static_cast<unsigned>(sid));
			}
		}
	}

	return assignmentOperator();
}

bool Thread::assignmentOperator(Operator op) {
	auto& f = frames.back();
	auto* node = f.stack.back().first;
	auto& nodePhase = f.stack.back().second;

	switch (nodePhase) {
	case 0:  nodePhase++;  f.stack.emplace_back(node->subNodes[0].get(), 0);  return true;
	case 1:  nodePhase++;  f.duplicateTop();  CHATRA_FALLTHROUGH;
	case 2:  CHATRA_EVALUATE_VALUE;
	case 3:  nodePhase++;  f.stack.emplace_back(node->subNodes[1].get(), 0);  return true;
	case 4:  CHATRA_EVALUATE_VALUE;
	case 5: {
		nodePhase = assignmentOperatorProcessPhase;
		f.stack.emplace_back(&operatorNode[static_cast<size_t>(op)], binaryOperatorProcessPhase);
		return true;
	}
	default:
		return assignmentOperator();
	}
}

bool Thread::initializePackage() {
	auto& f = frames.back();
	auto& nodePhase = f.stack.back().second;

	auto refInitializer = f.package.scope->ref(StringId::PackageInitializer);

	if (f.package.initialized) {
		f.stack.pop_back();
		f.lock->releaseIfExists(refInitializer, LockType::Hard);
		checkTransferReq();
		return true;
	}

	if (!lockReference(refInitializer, LockType::Hard))
		return false;

	if (f.package.initialized) {
		f.stack.pop_back();
		f.lock->release(refInitializer, LockType::Hard);
		checkTransferReq();
		return true;
	}

	for (; nodePhase < f.node->blockNodes.size(); nodePhase++) {
		auto* node = f.node->blockNodes[nodePhase].get();
		if (node->type != NodeType::Import)
			continue;
		if (!f.package.requiresProcessImport(*this, sTable, node))
			continue;

		auto sid = node->subNodes[SubNode::Import_Package]->sid;
		PackageId packageId;
		try {
			packageId = runtime.loadPackage(sTable->ref(sid));
		}
		catch(const PackageNotFoundException&) {
			errorAtNode(*this, ErrorLevel::Error, node->subNodes[SubNode::Import_Package].get(), "package not found", {});
			throw RuntimeException(StringId::PackageNotFoundException);
		}

		auto& targetPackage = f.package.import(node, packageId);

		nodePhase++;

		frames.emplace_back(*this, targetPackage, residentialFrameCount - 1, targetPackage.scope.get());
		targetPackage.pushNodeFrame(*this, targetPackage, frames.size() - 1, ScopeType::ScriptRoot, 2);
		frames.back().phase = Phase::ScriptRoot_Initial;

		checkTransferReq();
		runtime.enqueue(this);
		return false;
	}

	IErrorReceiverBridge errorReceiverBridge(*this);
	f.package.build(errorReceiverBridge, sTable);
	f.package.allocatePackageObject();
	if (errorReceiverBridge.hasError())
		throw RuntimeException(StringId::IllegalArgumentException);

	f.package.initialized = true;

	f.stack.pop_back();
	f.lock->release(refInitializer, LockType::Hard);
	checkTransferReq();
	return true;
}

bool Thread::initializeVars() {
	constexpr size_t callFrameCount = 3;
	auto& nodePhase = frames.back().stack.back().second;

	switch (nodePhase) {
	case 0: {
		nodePhase++;
		frames.back().lock->prepareToMethodCall();

		auto& initializationOrder = (frames.end() - callFrameCount - 1)->values.back()->getRef().deref<ObjectBase>().getClass()->refInitializationOrder();
		auto ref = (frames.end() - 2)->getSelf();
		for (auto it = initializationOrder.crbegin(); it != initializationOrder.crend(); it++) {
			auto* cl = *it;
			frames.back().stack.emplace_back(&popValueNode, 0);
			constructFrames(cl->getPackage(), cl->getNode(), cl, ref);
		}

		checkTransferReq();
		runtime.enqueue(this);
		return false;
	}

	default: {
		frames.back().stack.pop_back();
		auto& f0 = *(frames.end() - callFrameCount - 1);
		auto* methodValue = *(f0.values.end() - 2);
		auto* argsValue = *(f0.values.end() - 3);

		auto isNative = invokeNativeMethod(frames.size() - callFrameCount - 1, &(frames.end() - 2)->getSelf().deref<ObjectBase>(),
				methodValue, argsValue, [&]() { pop(); });
		if (isNative)
			return false;

		importMethodArguments(methodValue->getRefMethod(), argsValue);
		return true;
	}
	}
}

bool Thread::popValue() {
	auto& f = frames.back();
	f.stack.pop_back();
	f.pop();
	return true;
}

bool Thread::postConstructor() {
	auto& f = frames.back();
	f.stack.pop_back();
	f.pop();  // Remove a value returned from constructor

	auto& object = f.values.back()->getRef().derefWithoutLock<ObjectBase>();
	if (object.getTypeId() == typeId_UserObjectBase &&
			!static_cast<UserObjectBase*>(&object)->refUninitializedParents().empty()) {
		errorAtNode(*this, ErrorLevel::Error, (*(f.values.end() - 2))->getNode(), "one or more superclasses has not been initialized", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}

	f.pop(f.values.size() - 3, f.values.size() - 1);
	return true;
}

bool Thread::restoreTemporaryLock() {
	auto& f = frames.back();
	auto temp = f.lock->getTemporary();
	for (auto ref : temp) {
		if (!lockReference(ref, LockType::Temporary))
			return false;
	}
	f.lock->temporaryLockRestored();
	f.stack.pop_back();
	return true;
}

bool Thread::returnFromAsync() {
	auto& f = frames.back();
	auto& async = (*(f.values.end() - 2))->getRef().deref<Async>();
	async.updateResult(f.values.back()->getRef());
	finish();
	return false;
}

void Thread::prepareExpression(Node* node) {
	auto& f = frames.back();
	f.phase++;
	f.popAll();
	f.stack.emplace_back(&expressionMarkerNode, 0);
	f.stack.emplace_back(node, 0);
	// printf("prepareExpression: line %u\n", node->tokens[0]->line.lock()->lineNo);
}

static inline int64_t divisionFloor(int64_t v0, int64_t v1) {
	if (v1 < 0) {
		v0 = -v0;
		v1 = -v1;
	}
	return v0 >= 0 ? v0 / v1 : (v0 - (v1 - 1)) / v1;
}

static inline int64_t divisionCeiling(int64_t v0, int64_t v1) {
	if (v1 < 0) {
		v0 = -v0;
		v1 = -v1;
	}
	return v0 >= 0 ? (v0 + (v1 - 1)) / v1 : v0 / v1;
}

bool Thread::resumeExpression() {
	auto& f = frames.back();
	while (!f.stack.empty()) {
		auto* node = f.stack.back().first;

		switch (node->type) {
		case NodeType::Expression:
			if (expressionOperator())
				continue;
			return false;

		case NodeType::For:
			if (assignmentOperator())
				continue;
			return false;

		case NodeType::Return: {
			returnStatement(f.values.back(), true);
			return false;
		}

		case NodeType::Throw: {
			convertTopToScalar(node, 0);
			raiseException(f.values.back());
			return true;
		}

		case NodeType::Literal: {
			auto ref = f.pushTemporary()->setRvalue();
			switch (node->literalValue->type) {
			case LiteralType::Null:  ref.setNull();  break;
			case LiteralType::Bool:  ref.setBool(node->literalValue->vBool);  break;
			case LiteralType::Int:
				if (node->literalValue->vInt > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
					throw RuntimeException(StringId::OverflowException);
				ref.setInt(static_cast<int64_t>(node->literalValue->vInt));
				break;
			case LiteralType::Float:  ref.setFloat(node->literalValue->vFloat);  break;
			case LiteralType::String:  ref.allocate<String>().setValue(node->literalValue->vString);  break;
			default:  // TODO MLString
				throw NotImplementedException();
			}
			f.stack.pop_back();
			continue;
		}

		case NodeType::Name: {
			f.pushTemporary()->setName(node, node->sid);
			f.stack.pop_back();
			continue;
		}

		default:
			break;
		}

		assert(node->type == NodeType::Operator);
		bool shouldContinue = true;
		switch (node->op) {

		case Operator::Container:
			shouldContinue = containerOperator();
			break;

		case Operator::Subscript:
		case Operator::Call:
			shouldContinue = callOperator();
			break;

		case Operator::Parenthesis: {
			Node* nextNode = node->subNodes[0]->subNodes[0].get();
			f.stack.pop_back();
			f.stack.emplace_back(nextNode, 0);
			break;
		}

		case Operator::ElementSelection:
			shouldContinue = elementSelectionOperator();
			break;

		case Operator::PostfixIncrement:
			shouldContinue = unaryAssignmentOperator(false,
					[](bool v0) { (void)v0; return true; },
					[](int64_t v0) {
						if (v0 == std::numeric_limits<int64_t>::max())
							throw RuntimeException(StringId::OverflowException);
						return v0 + 1;
					},
					[](double v0) { return v0 + 1.0; });
			break;

		case Operator::PostfixDecrement:
			shouldContinue = unaryAssignmentOperator(false,
					[](bool v0) { (void)v0; return false; },
					[](int64_t v0) {
						if (v0 == std::numeric_limits<int64_t>::min())
							throw RuntimeException(StringId::OverflowException);
						return v0 - 1;
					},
					[](double v0) { return v0 - 1.0; });
			break;

		case Operator::VarArg:
			shouldContinue = varArgOperator();
			break;

		case Operator::PrefixIncrement:
			shouldContinue = unaryAssignmentOperator(true,
					[](bool v0) { (void)v0; return true; },
					[](int64_t v0) {
						if (v0 == std::numeric_limits<int64_t>::max())
							throw RuntimeException(StringId::OverflowException);
						return v0 + 1;
					},
					[](double v0) { return v0 + 1.0; });
			break;

		case Operator::PrefixDecrement:
			shouldContinue = unaryAssignmentOperator(true,
					[](bool v0) { (void)v0; return false; },
					[](int64_t v0) {
						if (v0 == std::numeric_limits<int64_t>::min())
							throw RuntimeException(StringId::OverflowException);
						return v0 - 1;
					},
					[](double v0) { return v0 - 1.0; });
			break;

		case Operator::BitwiseComplement:
			shouldContinue = standardUnaryOperator(
					[](bool v0) { return !v0; },
					[](int64_t v0) { return static_cast<int64_t>(~static_cast<uint64_t>(v0)); },
					[](double v0) -> double { (void)v0; throw OperatorNotSupportedException(); });
			break;

		case Operator::LogicalNot:
			shouldContinue = standardUnaryOperator(
					[](bool v0) { return !v0; },
					[](int64_t v0) -> int64_t { (void)v0; throw OperatorNotSupportedException(); },
					[](double v0) -> double { (void)v0; throw OperatorNotSupportedException(); });
			break;

		case Operator::UnaryPlus: {
			Node* nextNode = node->subNodes[0].get();
			f.stack.pop_back();
			f.stack.emplace_back(nextNode, 0);
			break;
		}

		case Operator::UnaryMinus: {
			// special case for "- <integer>"
			auto& subNode = node->subNodes[0];
			if (subNode->type == NodeType::Literal && subNode->literalValue->type == LiteralType::Int) {
				auto value = subNode->literalValue->vInt;
				if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1)
					throw RuntimeException(StringId::OverflowException);
				f.pushTemporary()->setRvalue().setInt(-static_cast<int64_t>(value));
				f.stack.pop_back();
				break;
			}

			shouldContinue = standardUnaryOperator(
					[](bool v0) { return !v0; },
					[](int64_t v0) {
						if (v0 == std::numeric_limits<int64_t>::min())
							throw RuntimeException(StringId::OverflowException);
						return -v0;
					},
					[](double v0) { return -v0; });
			break;
		}

		case Operator::TupleExtraction:
			errorAtNode(*this, ErrorLevel::Error, node, "tuple extraction operator is not allowed here", {});
			throw RuntimeException(StringId::UnsupportedOperationException);

		case Operator::FunctionObject:
			shouldContinue = functionObjectOperator();
			break;

		case Operator::Async:
			shouldContinue = asyncOperator();
			break;

		case Operator::Multiplication:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 && v1; },
					[](int64_t v0, int64_t v1) {
						int64_t t = v0 * v1;
						if (v1 != 0 && t / v1 != v0)
							throw RuntimeException(StringId::OverflowException);
						return t;
					},
					[](double v0, double v1) { return v0 * v1; });
			break;

		case Operator::MultiplicationOv:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 && v1; },
					[](int64_t v0, int64_t v1) { return v0 * v1; },
					[](double v0, double v1) { return v0 * v1; });
			break;

		case Operator::Division:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return v0 / v1;
					},
					[](double v0, double v1) { return v0 / v1; });
			break;

		case Operator::DivisionFloor:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return divisionFloor(v0, v1);
					},
					[](double v0, double v1) -> double { (void)v0; (void)v1; throw OperatorNotSupportedException(); });
			break;

		case Operator::DivisionCeiling:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return divisionCeiling(v0, v1);
					},
					[](double v0, double v1) -> double { (void)v0; (void)v1; throw OperatorNotSupportedException(); });
			break;

		case Operator::Modulus:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return v0 % v1;
					},
					[](double v0, double v1) { return std::fmod(v0, v1); });
			break;

		case Operator::ModulusFloor:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return v0 - divisionFloor(v0, v1) * v1;
					},
					[](double v0, double v1) -> double { (void)v0; (void)v1; throw OperatorNotSupportedException(); });
			break;

		case Operator::ModulusCeiling:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 == 0)
							throw RuntimeException(StringId::DivideByZeroException);
						return v0 - divisionCeiling(v0, v1) * v1;
					},
					[](double v0, double v1) -> double { (void)v0; (void)v1; throw OperatorNotSupportedException(); });
			break;

		case Operator::Exponent:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) -> bool { (void)v0; (void)v1; throw OperatorNotSupportedException(); },
					[](int64_t v0, int64_t v1) {
						if (v1 < 0)
							throw OperatorNotSupportedException();
						if (v1 == 0)
							return static_cast<int64_t>(1);
						if (v1 == 1)
							return v0;
						int64_t t = 1;
						for (int64_t i = 0; i < v1; i++)
							t *= v0;
						return t;
					},
					[](double v0, double v1) { return std::pow(v0, v1); });
			break;

		case Operator::Addition:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 | v1; },
					[](int64_t v0, int64_t v1) {
						int64_t t = v0 + v1;
						if ((~(v0 ^ v1) & (t ^ v0)) >> 63U)
							throw RuntimeException(StringId::OverflowException);
						return t;
					},
					[](double v0, double v1) { return v0 + v1; });
			break;

		case Operator::AdditionOv:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 | v1; },
					[](int64_t v0, int64_t v1) { return v0 + v1; },
					[](double v0, double v1) { return v0 + v1; });
			break;

		case Operator::Subtraction:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 ^ v1; },
					[](int64_t v0, int64_t v1) {
						int64_t t = v0 - v1;
						if (((v0 ^ v1) & (t ^ v0)) >> 63U)
							throw RuntimeException(StringId::OverflowException);
						return t;
					},
					[](double v0, double v1) { return v0 - v1; });
			break;

		case Operator::SubtractionOv:
			shouldContinue = standardBinaryOperator(
					[](bool v0, bool v1) { return v0 ^ v1; },
					[](int64_t v0, int64_t v1) { return v0 - v1; },
					[](double v0, double v1) { return v0 - v1; });
			break;

		case Operator::LeftShift:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) {
				if (v1 < 0)
					throw OperatorNotSupportedException();
				return v0 << v1;
			});
			break;

		case Operator::RightShift:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) {
				if (v1 < 0)
					throw OperatorNotSupportedException();
				return v0 >> v1;
			});
			break;

		case Operator::UnsignedRightShift:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) {
				if (v1 < 0)
					throw OperatorNotSupportedException();
				return static_cast<int64_t>(static_cast<uint64_t>(v0) >> static_cast<uint64_t>(v1));
			});
			break;

		case Operator::BitwiseAnd:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) { return v0 & v1; });
			break;

		case Operator::BitwiseOr:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) { return v0 | v1; });
			break;

		case Operator::BitwiseXor:
			shouldContinue = shiftOrBitwiseOperator([](int64_t v0, int64_t v1) { return v0 ^ v1; });
			break;

		case Operator::LessThan:
			shouldContinue = comparisonOperator(
					[](bool v0, bool v1) { return !v0 && v1; },
					[](int64_t v0, int64_t v1) { return v0 < v1; },
					[](double v0, double v1) { return v0 < v1; });
			break;

		case Operator::GreaterThan:
			shouldContinue = comparisonOperator(
					[](bool v0, bool v1) { return v0 && !v1; },
					[](int64_t v0, int64_t v1) { return v0 > v1; },
					[](double v0, double v1) { return v0 > v1; });
			break;

		case Operator::LessThanOrEqualTo:
			shouldContinue = comparisonOperator(
					[](bool v0, bool v1) { return !v0 || v1; },
					[](int64_t v0, int64_t v1) { return v0 <= v1; },
					[](double v0, double v1) { return v0 <= v1; });
			break;

		case Operator::GreaterThanOrEqualTo:
			shouldContinue = comparisonOperator(
					[](bool v0, bool v1) { return v0 || !v1; },
					[](int64_t v0, int64_t v1) { return v0 >= v1; },
					[](double v0, double v1) { return v0 >= v1; });
			break;

		case Operator::InstanceOf:
			shouldContinue = instanceOfOperator();
			break;

		case Operator::EqualTo:
			shouldContinue = equalityOperator<true>();
			break;

		case Operator::NotEqualTo:
			shouldContinue = equalityOperator<false>();
			break;

		case Operator::LogicalAnd:
			shouldContinue = logicalOperator([](bool v0) { return v0; }, [](bool v0) { (void)v0; return false; },
					[](bool v0, bool v1) { return v0 && v1; });
			break;

		case Operator::LogicalOr:
			shouldContinue = logicalOperator([](bool v0) { return !v0; }, [](bool v0) { (void)v0; return true; },
					[](bool v0, bool v1) { return v0 || v1; });
			break;

		case Operator::LogicalXor:
			shouldContinue = logicalOperator([](bool v0, bool v1) { return v0 ^ v1; });
			break;

		case Operator::Conditional:
			shouldContinue = conditionalOperator();
			break;

		case Operator::Tuple:
			shouldContinue = tupleOperator();
			break;

		case Operator::TupleDelimiter:
			f.pushTemporary()->setName(node, StringId::OpTupleDelimiter);
			f.stack.pop_back();
			break;

		case Operator::Pair:  // This is also used for evaluating variables or default arguments
			shouldContinue = defineAndAssignmentOperator();
			break;

		case Operator::Assignment:
			shouldContinue = assignmentOperator();
			break;

		case Operator::MultiplicationAssignment:  shouldContinue = assignmentOperator(Operator::Multiplication);  break;
		case Operator::MultiplicationOvAssignment:  shouldContinue = assignmentOperator(Operator::MultiplicationOv);  break;
		case Operator::DivisionAssignment:  shouldContinue = assignmentOperator(Operator::Division);  break;
		case Operator::DivisionFloorAssignment:  shouldContinue = assignmentOperator(Operator::DivisionFloor);  break;
		case Operator::DivisionCeilingAssignment:  shouldContinue = assignmentOperator(Operator::DivisionCeiling);  break;
		case Operator::ModulusAssignment:  shouldContinue = assignmentOperator(Operator::Modulus);  break;
		case Operator::ModulusFloorAssignment:  shouldContinue = assignmentOperator(Operator::ModulusFloor);  break;
		case Operator::ModulusCeilingAssignment:  shouldContinue = assignmentOperator(Operator::ModulusCeiling);  break;
		case Operator::ExponentAssignment:  shouldContinue = assignmentOperator(Operator::Exponent);  break;
		case Operator::AdditionAssignment:  shouldContinue = assignmentOperator(Operator::Addition);  break;
		case Operator::AdditionOvAssignment:  shouldContinue = assignmentOperator(Operator::AdditionOv);  break;
		case Operator::SubtractionAssignment:  shouldContinue = assignmentOperator(Operator::Subtraction);  break;
		case Operator::SubtractionOvAssignment:  shouldContinue = assignmentOperator(Operator::SubtractionOv);  break;
		case Operator::LeftShiftAssignment:  shouldContinue = assignmentOperator(Operator::LeftShift);  break;
		case Operator::RightShiftAssignment:  shouldContinue = assignmentOperator(Operator::RightShift);  break;
		case Operator::UnsignedRightShiftAssignment:  shouldContinue = assignmentOperator(Operator::UnsignedRightShift);  break;
		case Operator::BitwiseAndAssignment:  shouldContinue = assignmentOperator(Operator::BitwiseAnd);  break;
		case Operator::BitwiseOrAssignment:  shouldContinue = assignmentOperator(Operator::BitwiseOr);  break;
		case Operator::BitwiseXorAssignment:  shouldContinue = assignmentOperator(Operator::BitwiseXor);  break;

		case opInitializePackage:
			shouldContinue = initializePackage();
			break;

		case opEvaluateTuple: {
			if (evaluateTuple() == EvaluateValueResult::Suspend)
				return false;
			break;
		}

		case opInitializeVars:
			shouldContinue = initializeVars();
			break;

		case opPopValue:
			shouldContinue = popValue();
			break;

		case opPostConstructor:
			shouldContinue = postConstructor();
			break;

		case opPostPostfixUnaryOperator: {
			f.stack.pop_back();
			f.pop();
			break;
		}

		case opRestoreTemporaryLock:
			shouldContinue = restoreTemporaryLock();
			break;

		case opReturnFromAsync:
			shouldContinue = returnFromAsync();
			break;

		case opProcessFor_CallNext:
			shouldContinue = processFor_CallNext();
			break;

		case opVarStatementMarker:
			f.stack.pop_back();
			break;
		}
		if (!shouldContinue)
			return false;
	}
	return true;
}

bool Thread::processFor_CallNext() {
	auto& f = frames.back();
	f.pushEmptyTuple();
	f.pushTemporary()->invokeMethod(f.node->subNodes[SubNode::For_Iterable].get(),
			f.scope->ref(StringId::ForIterator), StringId::next, {});
	f.stack.pop_back();
	f.stack.emplace_back(f.node, assignmentOperatorEvaluationPhase);
	return true;
}

void Thread::processFor() {
	auto& f = frames.back();

	if (f.phase < Phase::For_Begin)
		f.phase = Phase::For_BeginLoop;

	switch (f.phase) {
	case Phase::For_Begin: {
		auto& nIterator = f.node->subNodes[SubNode::For_Iterator];
		if (nIterator)
			f.scope->addConst(nIterator->sid);
		prepareExpression(f.node->subNodes[SubNode::For_Iterable].get());
		return;
	}

	case Phase::For_CallIterator: {
		convertTopToScalar(f.node, SubNode::For_Iterable);
		auto* v0 = f.values.back();
		if (!v0->hasRef() || v0->getRef().isNull() || v0->getRef().valueType() != ReferenceValueType::Object) {
			errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[SubNode::For_Iterable].get(), "expected iterable expression", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}

		auto& nLoopVariable = f.node->subNodes[SubNode::For_LoopVariable];
		f.pushEmptyTuple();
		f.pushTemporary()->invokeMethod(f.node->subNodes[SubNode::For_Iterable].get(), v0->getRef(),
				nLoopVariable && nLoopVariable->subNodes.size() == 2 ? StringId::keyedIterator : StringId::iterator, {});
		f.phase++;
		f.stack.emplace_back(&expressionMarkerNode, 0);
		return;
	}

	case Phase::For_PrepareLoop: {
		convertTopToScalar(f.node, SubNode::For_Iterable);
		auto* v0 = f.values.back();
		if (!v0->hasRef() || v0->getRef().isNull() || v0->getRef().valueType() != ReferenceValueType::Object) {
			errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[SubNode::For_Iterable].get(),
					R"*(returned value from "iterator()" or "keyedIterator()" is not iterator object)*", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}

		f.scope->addConst(StringId::ForIterator).set(v0->getRef());
		auto& nIterator = f.node->subNodes[SubNode::For_Iterator];
		if (nIterator)
			f.scope->ref(nIterator->sid).set(v0->getRef());
		f.phase++;
		CHATRA_FALLTHROUGH;
	}

	case Phase::For_BeginLoop:
		f.pushEmptyTuple();
		f.pushTemporary()->invokeMethod(f.node->subNodes[SubNode::For_Iterable].get(), f.scope->ref(StringId::ForIterator),
				StringId::hasNext, {});
		f.phase++;
		f.stack.emplace_back(&expressionMarkerNode, 0);
		return;

	case Phase::For_AssignLoopVariables: {
		if (!getBoolOrThrow(f.node, SubNode::For_Iterable)) {
			pop();
			return;
		}

		auto& nLoopVariable = f.node->subNodes[SubNode::For_LoopVariable];
		if (nLoopVariable) {
			// extract nLoopVariable -> call next() -> assignmentOperatorEvaluation
			f.stack.emplace_back(&processFor_CallNextNode, 0);
			f.stack.emplace_back(nLoopVariable->subNodes.size() == 2 ? nLoopVariable.get() : nLoopVariable->subNodes[0].get(), 0);
		}
		else {
			f.pushEmptyTuple();
			f.pushTemporary()->invokeMethod(f.node->subNodes[SubNode::For_Iterable].get(),
					f.scope->ref(StringId::ForIterator), StringId::next, {});
			f.stack.emplace_back(&expressionMarkerNode, 0);
		}
		// Calling next() can throw an exception; To process such exception properly,
		// f.phase should keep "outside loop" semantics during process next().
		f.phase++;
		return;
	}

	case Phase::For_EnterLoop:
		f.phase = 0;
		return;

	default:
		throw InternalError();
	}
}

void Thread::processSwitchCase(bool enter) {
	if (!enter) {
		pop();
		return;
	}

	auto& f0 = *(frames.end() - 1);
	auto& f1 = *(frames.end() - 2);

	while (f0.node->blockNodes.empty() && f1.phase < f1.node->blockNodes.size()) {
		auto& nextNode = f1.node->blockNodes[f1.phase];
		checkIsValidNode(nextNode.get());
		if (nextNode->type != NodeType::Case && nextNode->type != NodeType::Default)
			break;
		f0.node = nextNode.get();
		f1.phase++;
	}

	f0.phase = 0;
	f1.phase = static_cast<size_t>(std::distance(f1.node->blockNodes.cbegin(),
			std::find_if(f1.node->blockNodes.cbegin(), f1.node->blockNodes.cend(), [](const std::shared_ptr<Node>& n) {
				return n->type == NodeType::Catch || n->type == NodeType::Finally;
			})));
}

bool Thread::processSwitchCase() {
	auto& f = frames.back();

	if (f.phase < Phase::Case_Base) {
		pop();
		return true;
	}

	switch (f.phase) {
	case Phase::Case_Check1:
		convertTopToScalar(f.node, 0);
		CHATRA_FALLTHROUGH;

	case Phase::Case_Check0: {
		auto* v0 = f.values.back();
		if (!v0->hasRef()) {
			errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[0].get(), "expected Bool or object expression", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}
		auto ref0 = v0->getRef();
		if (ref0.valueType() == ReferenceValueType::Bool) {
			if (f.node->subNodes[0]->type == NodeType::Literal && f.phase == Phase::Case_Check0) {
				errorAtNode(*this, ErrorLevel::Warning, f.node->subNodes[0].get(),
						"case statement with Bool constant always produces same result", {});
			}
			processSwitchCase(ref0.getBool());
			return true;
		}

		if (f.phase == Phase::Case_Check1) {
			errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[0].get(), R"*("has()" or "equals()" should return Bool)*", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}

		// v1 can be on LockType::Soft because it belong to previous frame (pop() moves it from Temporary to Soft).
		// Therefore, sometimes v1 might lose the lock by some operation prior to the current 'case' statement.
		auto* v1 = (frames.end() - 2)->values.back();
		if (v1->getType() != TemporaryObject::Type::Rvalue && !lockReference(v1))
			return false;

		auto ref1 = v1->getRef();
		auto t0 = ref0.valueType();
		auto t1 = ref1.valueType();
		if (t0 != ReferenceValueType::Object && t0 == t1) {
			switch (t0) {
			case ReferenceValueType::Int:  processSwitchCase(ref0.getInt() == ref1.getInt());  break;
			case ReferenceValueType::Float:  processSwitchCase(ref0.getFloat() == ref1.getFloat());  break;
			default: throw InternalError();
			}
			return true;
		}

		if (ref0.isNull() || ref1.isNull()) {
			processSwitchCase(ref0.isNull() == ref1.isNull());
			return true;
		}

		if (t0 != ReferenceValueType::Object) {
			if (t1 != ReferenceValueType::Object) {
				errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[0].get(), "expected ${0} or object expression", {
					t1 == ReferenceValueType::Int ? "Int" : "Float"});
			}
			else
				errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[0].get(), "expected object expression", {});
			throw RuntimeException(StringId::TypeMismatchException);
		}

		std::vector<std::pair<Tuple::Key, Reference>> tupleArgs = {{StringId::Invalid, ref1}};
		f.pushTemporary()->setRvalue().allocate<Tuple>(sTable, std::move(tupleArgs), getId());

		std::vector<ArgumentSpec> args = {{StringId::Invalid, getReferClass(ref1)}};
		if (ref0.deref<ObjectBase>().getClass()->refMethods().find(nullptr, StringId::has, StringId::Invalid, args, {}) != nullptr)
			f.pushTemporary()->invokeMethod(f.node->subNodes[0].get(), ref0, StringId::has, std::move(args));
		else
			f.pushTemporary()->invokeMethod(f.node->subNodes[0].get(), ref0, StringId::equals, std::move(args));

		f.phase++;
		f.stack.emplace_back(&expressionMarkerNode, 0);
		return true;
	}

	default:
		throw InternalError();
	}
}

void Thread::processFinalizer() {
	auto& f = frames.back();
	f.popAll();

	auto& objects = runtime.scope->ref(StringId::FinalizerObjects).deref<Array>();
	if (!objects.popIfExists([&](Reference ref) { f.pushTemporary()->setRvalue().set(ref); }))
		return;

	auto* v0 = f.values.back();
	f.pushEmptyTuple();
	f.pushTemporary()->invokeMethod(nullptr, v0->getRef(), StringId::Deinit, {});
	f.stack.emplace_back(&expressionMarkerNode, 0);
	runtime.enqueue(this);
}

Frame* Thread::findClassFrame() {
	for (size_t frameIndex = frames.size() - 1; frameIndex != SIZE_MAX; ) {
		auto& f = frames[frameIndex];
		if (f.scope->getScopeType() == ScopeType::Class)
			return &f;
		frameIndex = f.parentIndex;
	}
	return nullptr;
}

void Thread::error(ErrorLevel level,
		const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
		const std::string& message, const std::vector<std::string>& args) {

	if (level == ErrorLevel::Warning) {
		runtime.outputError(RuntimeImp::formatError(level, fileName, lineNo, line, first, last, message, args));
		return;
	}

	auto it = std::find_if(errors.begin(), errors.end(), [&](const std::tuple<std::string, unsigned, std::string, size_t>& e) {
		return std::get<0>(e) == fileName && std::get<1>(e) == lineNo;
	});
	if (it != errors.end()) {
		std::get<3>(*it)++;
		return;
	}

	auto formatted = RuntimeImp::formatError(level, fileName, lineNo, line, first, last, message, args, true);
	errors.emplace_back(fileName, lineNo, std::move(formatted), 1);
}

void Thread::emitError(StringId exceptionName, Node* exceptionNode) {
	if (exceptionName != StringId::Invalid) {
		std::string message = sTable->ref(exceptionName) + " raised";
		if (exceptionNode != nullptr && !exceptionNode->tokens.empty()) {
			auto line = exceptionNode->tokens[0]->line.lock();
			if (line)
				message.append(" from ").append(RuntimeImp::formatOrigin(line->fileName, line->lineNo));
		}
		message.append("\n");
		runtime.outputError(message);
	}
	for (auto& e : errors) {
		auto& message = std::get<2>(e);
		std::string target = "${0}";
		auto position = message.find(target);
		if (position != std::string::npos) {
			size_t count = std::get<3>(e);
			message.replace(position, target.size(), count <= 1 ? "" : std::string(" (+extra ") + std::to_string(count - 1) + " error(s))");
		}
		runtime.outputError(message);
	}
	errors.clear();
}

Node* Thread::refSpecialNode(NodeType type, Operator op) {
	return type == NodeType::Operator ? specialNodes1.at(op) : specialNodes0.at(type);
}

Thread::Thread(RuntimeImp& runtime, Instance& instance) noexcept
		: runtime(runtime), instance(instance), scope(runtime.storage->add(ScopeType::Thread)),
		temporaryObjectPool(*this), temporaryTuplePool(*this) {

	if (!initialized) {
		set(expressionMarkerNode, NodeType::Expression);
		set(initializePackageNode, opInitializePackage);
		set(evaluateTupleNode, opEvaluateTuple);

		set(defaultConstructorNode, NodeType::Def);
		defaultConstructorNode.subNodes.resize(SubNode::Def_SubNodes);

		set(initializeVarsNode, opInitializeVars);
		set(popValueNode, opPopValue);
		set(postConstructorNode, opPostConstructor);
		set(postPrefixUnaryOperatorNode, opPostPostfixUnaryOperator);
		set(restoreTemporaryLockNode, opRestoreTemporaryLock);
		set(returnFromAsyncNode, opReturnFromAsync);
		set(processFor_CallNextNode, opProcessFor_CallNext);
		set(varStatementMarkerNode, opVarStatementMarker);

		for (size_t i = 0; i < NumberOfOperators; i++)
			set(operatorNode[i], static_cast<Operator>(i));

		initialized = true;
	}
}

void Thread::postInitialize() {
	emptyTuple = &scope->add(StringId::EmptyTuple, getId()).allocate<Tuple>(getId());
}

unsigned Thread::requestToPause() {
	pauseRequested = true;
	return runtime.pause(this);
}

void Thread::finish() {
	for (;;) {
		auto& f = frames.back();
		f.popAll();

		if (f.exception != nullptr) {
			errorAtNode(*this, ErrorLevel::Error, frames.back().exceptionNode, "unhandled exception", {});
			emitError(getReferClass(frames.back().exception->getRef())->getName(), frames.back().exceptionNode);

			f.recycle(f.exception);
			f.exception = nullptr;
		}

		if (f.caughtException != nullptr) {
			f.recycle(f.caughtException);
			f.caughtException = nullptr;
		}

		if (frames.size() <= residentialFrameCount)
			break;
		pop();
	}

	// Remove returned value and exceptions
	frames.back().popAll();

	auto parser = runtime.scope->ref(StringId::Parser);
	if (parser.lockedBy() == getId())
		parser.unlock();

	std::unique_ptr<Thread> self;
	{
		std::lock_guard<SpinLock> lock(instance.lockThreads);
		self = std::move(instance.threads[getId()]);
		instance.threads.erase(getId());
	}
	remove();
	checkTransferReq();
	assert(transferReq.empty());
}

void Thread::run() {
	/*{
		std::lock_guard<SpinLock> lock(lockTransferReq);
		std::printf("run: instanceId %u, threadId = %u; packageId = %u, frames.size = %u, transferReq = %u\n",
				static_cast<unsigned>(instance.getId()),
				static_cast<unsigned>(getId()),
				static_cast<unsigned>(frames.back().package.getId()),
				static_cast<unsigned>(frames.size()),
				static_cast<unsigned>(transferReq.size()));
		std::fflush(stdout);
	}*/

	runtime.checkGc();
	captureStringTable();

	while (frames.size() > residentialFrameCount) {
		try {
			if (!resumeExpression())
				return;

			if (!parse())
				return;

			auto& f = frames.back();
			if (f.exception != nullptr) {
				consumeException();
				continue;
			}

			if (f.phase < f.node->blockNodes.size()) {
				f.lock->moveTemporaryToSoftLock();

				auto& n = f.node->blockNodes[f.phase];
				switch (n->type) {
				case NodeType::Import:
				case NodeType::Def:
				case NodeType::DefOperator:
				case NodeType::Class:
					f.phase++;
					continue;

				case NodeType::Expression:
					prepareExpression(n->subNodes[0].get());
					continue;

				case NodeType::Sync:
					f.phase++;
					pushBlock(n.get(), 0);
					continue;

				case NodeType::Touch:
					prepareExpression(n->subNodes[0].get());
					continue;

				case NodeType::Var:
					f.phase++;
					for (auto& nDef : n->subNodes[SubNode::Var_Definitions]->subNodes) {
						if (nDef->op != Operator::Pair)
							continue;
						f.stack.emplace_back(&popValueNode, 0);
						f.stack.emplace_back(&varStatementMarkerNode, 0);
						f.stack.emplace_back(nDef.get(), 0);
					}
					continue;

				case NodeType::IfGroup:
					f.phase++;
					pushBlock(n.get(), Phase::IfGroup_Base0);
					checkIsValidNode(n->blockNodes[0].get());
					prepareExpression(n->blockNodes[0]->subNodes[0].get());
					continue;

				case NodeType::For:
					f.phase++;
					pushBlock(n.get(), Phase::For_Begin, n->sid, Phase::For_Continue);
					processFor();
					continue;

				case NodeType::While:
					f.phase++;
					pushBlock(n.get(), Phase::While_Condition0, n->sid, Phase::While_Continue);
					prepareExpression(n->subNodes[SubNode::While_Condition].get());
					continue;

				case NodeType::Switch:
					f.phase++;
					pushBlock(n.get(), Phase::Switch_Base0);
					prepareExpression(n->subNodes[0].get());
					continue;

				case NodeType::Case:
					f.phase++;
					pushBlock(n.get(), Phase::Case_Base);
					prepareExpression(n->subNodes[0].get());
					continue;

				case NodeType::Default:
					f.phase++;
					pushBlock(n.get(), 0);
					continue;

				case NodeType::Do:
					f.phase++;
					pushBlock(n.get(), 0);
					continue;

				case NodeType::Catch:
					f.phase++;
					continue;

				case NodeType::Finally:
					f.phase++;
					pushBlock(n.get(), 0);
					continue;

				case NodeType::Break:
					breakStatement(n.get());
					continue;

				case NodeType::Continue:
					continueStatement(n.get());
					continue;

				case NodeType::Return:
					if (n->subNodes[0]) {
						f.stack.emplace_back(n.get(), 0);
						prepareExpression(n->subNodes[0].get());
					}
					else {
						returnStatement(nullptr, true);
						return;
					}
					continue;

				case NodeType::Throw:
					if (n->subNodes[0]) {
						f.stack.emplace_back(n.get(), 0);
						prepareExpression(n->subNodes[0].get());
					}
					else {
						if (f.caughtException == nullptr) {
							errorAtNode(*this, ErrorLevel::Error, n.get(), "no exception raised to re-throw", {});
							emitError(StringId::UnsupportedOperationException, n.get());
							throw AbortThreadException();
						}

						f.phase++;
						raiseException(f.caughtException);
						f.recycle(f.caughtException);
						f.caughtException = nullptr;
					}
					continue;

				case ntFinalizer:
					processFinalizer();
					return;

				case ntParserError:
					checkIsValidNode(n.get());
					return;

				default:
					throw InternalError();
				}
			}

			switch (f.node->type) {
			case NodeType::ScriptRoot:
				if (f.phase == Phase::ScriptRoot_Initial) {
					f.phase = 0;
					f.stack.emplace_back(&initializePackageNode, 0);
					continue;
				}

				if (f.phase != Phase::ScriptRoot_InitCall1) {
					f.phase = Phase::ScriptRoot_InitCall0;  // To avoid emitting meaningless exception
					auto* value = f.pushTemporary();
					value->setName(nullptr, StringId::Init);
					if (value->evaluateAsPackageInitCall() == TemporaryObject::EvaluationResult::Succeeded) {
						auto* method = value->getRefMethod();
						if (method != nullptr) {
							f.phase = Phase::ScriptRoot_InitCall1;
							methodCall(false);
							return;
						}
					}
				}

				returnStatement(nullptr, false);
				return;

			case NodeType::Def:
			case NodeType::DefOperator:
			case NodeType::Class:
				returnStatement(nullptr, false);
				return;

			case NodeType::Sync:
				pop();
				continue;

			case NodeType::IfGroup: {
				if (f.phase < Phase::IfGroup_Base0) {
					pop();
					continue;
				}

				assert(f.values.size() == 1);
				size_t index = f.phase - Phase::IfGroup_Base1;
				Node* blockNode = f.node->blockNodes[index].get();
				if (getBoolOrThrow(blockNode, 0)) {
					f.phase = f.node->blockNodes.size();
					pushBlock(blockNode, 0);
				}
				else {
					index++;
					if (index == f.node->blockNodes.size())
						pop();
					else {
						blockNode = f.node->blockNodes[index].get();
						checkIsValidNode(blockNode);
						if (blockNode->type == NodeType::Else) {
							f.phase = f.node->blockNodes.size();
							pushBlock(blockNode, 0);
						}
						else
							prepareExpression(blockNode->subNodes[0].get());
					}
				}
				continue;
			}

			case NodeType::If:
			case NodeType::ElseIf:
			case NodeType::Else:
				pop();
				continue;

			case NodeType::For:
				processFor();
				continue;

			case NodeType::While:
				if (f.phase <= Phase::While_Continue) {
					f.phase = Phase::While_Condition0;
					prepareExpression(f.node->subNodes[SubNode::While_Condition].get());
					continue;
				}
				assert(f.phase == Phase::While_Condition1 && f.values.size() == 1);
				if (getBoolOrThrow(f.node, SubNode::While_Condition))
					f.phase = 0;
				else
					pop();
				continue;

			case NodeType::Switch: {
				if (f.phase < Phase::Switch_Base0) {
					pop();
					continue;
				}

				assert(f.phase == Phase::Switch_Base1 && f.values.size() == 1);
				convertTopToScalar(f.node, 0);
				auto* value = f.values.back();
				if (value->hasRef() && value->getRef().valueType() == ReferenceValueType::Bool) {
					errorAtNode(*this, ErrorLevel::Error, f.node->subNodes[0].get(),
							"switch statement cannot handle Bool value", {});
					throw RuntimeException(StringId::UnsupportedOperationException);
				}

				f.phase = 0;
				continue;
			}

			case NodeType::Case:
				if (!processSwitchCase())
					return;
				continue;

			case NodeType::Default:
				pop();
				continue;

			case NodeType::Do:
				pop();
				continue;

			case NodeType::Catch:
				f.recycle(f.caughtException);
				f.caughtException = nullptr;
				pop();
				continue;

			case NodeType::Finally:
				pop();
				continue;

			default:
				throw InternalError();
			}
		}
		catch (const RuntimeException& ex) {
			raiseException(ex);
		}
		catch (const AbortThreadException&) {
			break;
		}
		catch (const std::exception&) {
			runtime.outputError("fatal: internal error\n");
			break;
		}
	}

	finish();
}

Thread::Thread(RuntimeImp& runtime, Instance& instance, Reader& r) noexcept
		: runtime(runtime), instance(instance),
		temporaryObjectPool(*this), temporaryTuplePool(*this) {
	(void)r;
}

TemporaryObject* Thread::restoreTemporary(Reader& r) {
	return &scope->ref(r.read<StringId>()).derefWithoutLock<TemporaryObject>();
}

TemporaryTuple* Thread::restoreTemporaryTuple(Reader& r) {
	return &scope->ref(r.read<StringId>()).derefWithoutLock<TemporaryTuple>();
}

void Thread::save(Writer& w) const {
	w.CHATRA_OUT_POINTER(scope.get(), Scope);
	w.out(frames, [&](const Frame& frame) {
		w.out(&frame.package);
		w.out(frame);
	});
	w.out(transferReq);

	temporaryObjectPool.save(w);
	temporaryTuplePool.save(w);

	w.out(errors, [&](const std::tuple<std::string, unsigned, std::string, size_t>& e) {
		w.out(std::get<0>(e));
		w.out(std::get<1>(e));
		w.out(std::get<2>(e));
		w.out(std::get<3>(e));
	});
}

void Thread::restore(Reader& r) {
	scope = r.CHATRA_READ_UNIQUE(Scope);
	r.inList([&]() { r.emplaceBack(frames, *this, *r.readValidPackage()); });
	r.inList([&]() { r.emplaceBack(transferReq); });
	hasTransferReq = !transferReq.empty();

	temporaryObjectPool.restore(r, [&]() { return restoreTemporary(r); });
	temporaryTuplePool.restore(r, [&]() { return restoreTemporaryTuple(r); });

	r.inList([&]() {
		auto fileName = r.read<std::string>();
		auto lineNo = r.read<unsigned>();
		auto message = r.read<std::string>();
		auto count = r.read<size_t>();
		errors.emplace_back(std::make_tuple(std::move(fileName), lineNo, std::move(message), count));
	});
}

void Thread::postInitialize(Reader& r) {
	(void)r;
	emptyTuple = &scope->ref(StringId::EmptyTuple).deref<Tuple>();
}


}  // namespace chatra
