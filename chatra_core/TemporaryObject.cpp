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

void TemporaryObject::clearTargetRef() {
	targetRef = Reference(Reference::SetNull());
}

void TemporaryObject::clearRestrictions() {
	hasName = hasArgs = hasSetArg = hasVarArgOp = false;
	args.clear();
}

const Method* TemporaryObject::findSetMethod(const MethodTable& table, const Class* cl, StringId name) {
	return hasSetArg ? table.find(cl, name, StringId::Set, args, {setArg}) : nullptr;
}

void TemporaryObject::resolveNativeMethod(StringId name, StringId subName) {
	chatra_assert(method0 != nullptr);
	chatra_assert(methodTable != nullptr);

	if (method1 != nullptr || method0->node == nullptr || !(method0->node->flags & NodeFlags::Native)) {
		nativeMethod = nullptr;
		return;
	}

	nativeMethod = methodTable->findNativeMethod(name, subName);
	if (nativeMethod == nullptr && method0->node->blockNodes.empty()) {
		errorAtNode(thread, ErrorLevel::Error, method0->node, "native method not found", {});
		throw RuntimeException(StringId::MemberNotFoundException);
	}
}

bool TemporaryObject::findOnObject(Reference sourceRef, const MethodTable& table, const Class* cl,
		StringId overwriteName) {

	StringId methodName = (overwriteName != StringId::Invalid ? overwriteName
			: hasName ? name : StringId::Empty);

	auto* refMethod = table.find(cl, methodName, StringId::Invalid, args, {});
	if (refMethod == nullptr)
		return false;

	if (refMethod->position != SIZE_MAX) {
		setObjectRef(sourceRef, sourceRef.deref<ObjectBase>().ref(refMethod->position), refMethod->primaryPosition);
		return true;
	}

	setObjectMethod(sourceRef, &table, refMethod, findSetMethod(table, cl, methodName), hasArgs);
	resolveNativeMethod(methodName, StringId::Invalid);
	return true;
}

bool TemporaryObject::findOnPackage(Package* package) {

	StringId methodName = (hasName ? name : StringId::Empty);
	auto& table = package->clPackage->refMethods();

	auto* refMethod = table.find(nullptr, methodName, StringId::Invalid, args, {});
	if (refMethod == nullptr)
		return false;

	if (refMethod->position != SIZE_MAX) {
		Reference sourceRef = package->scope->ref(StringId::PackageObject);
		setObjectRef(sourceRef, sourceRef.deref<ObjectBase>().ref(refMethod->position), refMethod->primaryPosition);
		return true;
	}

	setFrameMethod(SIZE_MAX, package, &table, refMethod, findSetMethod(table, nullptr, methodName), hasArgs);
	resolveNativeMethod(methodName, StringId::Invalid);
	return true;
}

bool TemporaryObject::findScopeRef(Scope* scope) {
	chatra_assert(hasName && !hasArgs);
	if (!scope->has(name))
		return false;
	setScopeRef(scope->ref(name));
	return true;
}

bool TemporaryObject::findFrameMethods(size_t frameIndex, Package* package, const MethodTable& table) {
	chatra_assert(hasName && hasArgs);

	auto* refMethod = table.find(nullptr, name, StringId::Invalid, args, {});
	if (refMethod == nullptr)
		return false;

	setFrameMethod(frameIndex, package, &table, refMethod, findSetMethod(table, nullptr, name), hasArgs);
	resolveNativeMethod(name, StringId::Invalid);
	return true;
}

bool TemporaryObject::findConstructor(const Class* cl, StringId subName) {
	chatra_assert(hasArgs);
	auto& table = cl->refConstructors();
	auto* constructor = table.find(nullptr, StringId::Init, subName, args, {});
	if (constructor == nullptr)
		return false;
	setConstructor(cl, &table, constructor);
	resolveNativeMethod(StringId::Init, constructor->subName);
	return true;
}

bool TemporaryObject::findConstructorCall(Reference sourceRef, const Class* cl, StringId subName) {
	chatra_assert(hasArgs);
	auto& table = cl->refConstructors();
	auto* constructor = table.find(nullptr, StringId::Init, subName, args, {});
	if (constructor == nullptr)
		return false;
	setConstructorCall(sourceRef, cl, &table, constructor);
	resolveNativeMethod(StringId::Init, constructor->subName);
	return true;
}

void TemporaryObject::checkBeforeEvaluation() {
	// To implement partial evaluation correctly, checking validity of argument/assignment should be at this time,
	// not at addArgument() or addAssignment().
	if (hasArgs) {
		if (type == Type::Rvalue && targetRef.valueType() != ReferenceValueType::Object) {
			errorAtNode(thread, ErrorLevel::Error, node, "invalid suffix or method call on primitive", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		if ((type == Type::ScopeRef || type == Type::ObjectRef) && targetRef.valueType() != ReferenceValueType::Object) {
			errorAtNode(thread, ErrorLevel::Error, node, "base type of method call is not object", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		if (hasRef() && targetRef.isNull()) {
			errorAtNode(thread, ErrorLevel::Error, node, "de-referencing null", {});
			throw RuntimeException(StringId::NullReferenceException);
		}
	}

	if (hasSetArg) {
		if ((type == Type::Self || type == Type::Super || type == Type::ExplicitSuper) && !hasName && !hasArgs) {
			errorAtNode(thread, ErrorLevel::Error, node, R"("self" or "super" is not assignable)", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		if ((name == StringId::Self || name == StringId::Super) && !hasArgs) {
			errorAtNode(thread, ErrorLevel::Error, node, R"("self" or "super" is not assignable)", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
	}
}

TemporaryObject::EvaluationResult TemporaryObject::evaluateOnFrame(size_t frameIndex) {
	auto& f = thread.frames[frameIndex];

	const Class* cl = nullptr;
	switch(f.scope->getScopeType()) {
	case ScopeType::Thread:
		break;

	case ScopeType::Global:
		if (hasArgs && findFrameMethods(SIZE_MAX, nullptr, thread.runtime.methods))
			return EvaluationResult::Succeeded;
		if (!hasArgs && findScopeRef(f.scope))
			return EvaluationResult::Succeeded;
		break;

	case ScopeType::Package: {
		if (findOnPackage(&f.package))
			return EvaluationResult::Succeeded;

		for (auto* importedPackage : f.package.refAnonymousImports()) {
			if (findOnPackage(importedPackage))
				return EvaluationResult::Succeeded;
		}
		break;
	}

	case ScopeType::Class: {
		auto instanceRef = f.getSelf();
		cl = instanceRef.deref<ObjectBase>().getClass();
		if (findOnObject(instanceRef, cl->refMethods(), nullptr))
			return EvaluationResult::Succeeded;
		break;
	}

	case ScopeType::ScriptRoot:
	case ScopeType::Method:
	case ScopeType::InnerMethod:
	case ScopeType::Block: {
		if (hasArgs && f.methods != nullptr && findFrameMethods(frameIndex, &f.package, *f.methods))
			return EvaluationResult::Succeeded;
		if (f.captured) {
			auto scopeRef = f.scope->ref(StringId::Captured);
			auto& scope = scopeRef.deref<CapturedScope>();
			if (!hasArgs && scope.hasCaptured(name)) {
				setObjectRef(scopeRef, scope.refCaptured(name), SIZE_MAX);
				return EvaluationResult::Succeeded;
			}
		}
		else if (!hasArgs && findScopeRef(f.scope))
			return EvaluationResult::Succeeded;
		break;
	}
	}

	if (partialEvaluation(frameIndex, cl))
		return EvaluationResult::Partial;

	return EvaluationResult::Failed;
}

TemporaryObject::EvaluationResult TemporaryObject::evaluateDirect(bool throwIfNotFound) {
	const Class* clTarget;
	switch (type) {
	case Type::Rvalue:
		clTarget = targetRef.deref<ObjectBase>().getClass();
		if (findOnObject(targetRef, clTarget->refMethods(), nullptr))
			return EvaluationResult::Succeeded;
		break;

	case Type::Package: {
		auto* cl = package->findClass(name);
		if (cl != nullptr) {
			if (!hasArgs) {
				setClass(cl);
				return EvaluationResult::Succeeded;
			}
			if (findConstructor(cl, StringId::Invalid))
				return EvaluationResult::Succeeded;
		}

		if (findOnPackage(package))
			return EvaluationResult::Succeeded;

		clTarget = nullptr;
		break;
	}

	case Type::Class: {
		if (!hasArgs) {
			if (node != nullptr)
				errorAtNode(thread, ErrorLevel::Error, node, "cannot use element selection operator on a class", {});
			throw RuntimeException(StringId::UnsupportedOperationException);
		}
		if (findConstructor(cl, hasName ? name : StringId::Invalid))
			return EvaluationResult::Succeeded;

		if (node != nullptr)
			errorAtNode(thread, ErrorLevel::Error, node, "constructor not found", {});
		throw RuntimeException(StringId::MemberNotFoundException);
	}

	case Type::Constructor:
	case Type::FrameMethod:
	case Type::ObjectMethod:
		throw InternalError();

	case Type::ScopeRef:
	case Type::ObjectRef:
	case Type::Self:
		clTarget = targetRef.deref<ObjectBase>().getClass();
		if (findOnObject(targetRef, clTarget->refMethods(), nullptr))
			return EvaluationResult::Succeeded;
		break;

	case Type::Super: {
		// check "super.Class" or "super.Class()" style
		if (hasName) {
			auto& clParents = cl->refDirectParents();
			auto it = std::find_if(clParents.cbegin(), clParents.cend(), [&](const Class* clParent) {
				return clParent->getName() == name; });
			if (it != clParents.cend()) {
				auto* clParent = *it;

				if (!hasArgs) {
					setExplicitSuper(targetRef, clParent);
					return EvaluationResult::Succeeded;
				}
				if (findConstructorCall(targetRef, clParent, StringId::Invalid))
					return EvaluationResult::Succeeded;
			}
		}

		// check "super.constructor()" style
		if (hasName && hasArgs) {
			for (auto* clParent : cl->refDirectParents()) {
				if (findConstructorCall(targetRef, clParent, name))
					return EvaluationResult::Succeeded;
			}
		}

		if (findOnObject(targetRef, cl->refSuperMethods(), nullptr))
			return EvaluationResult::Succeeded;

		clTarget = cl;
		break;
	}

	case Type::ExplicitSuper: {
		// check "super.Class.constructor()" style
		if (hasName && hasArgs && findConstructorCall(targetRef, cl, name))
			return EvaluationResult::Succeeded;

		if (findOnObject(targetRef, cl->refMethods(), cl))
			return EvaluationResult::Succeeded;
		clTarget = cl;
		break;
	}

	default:
		throw InternalError();
	}

	if (partialEvaluation(SIZE_MAX, clTarget))
		return EvaluationResult::Partial;

	if (throwIfNotFound) {
		if (clTarget == nullptr)
			errorAtNode(thread, ErrorLevel::Error, node, "expected expression", {});
		else {
			errorAtNode(thread, ErrorLevel::Error, node, R"*(no method or member named "${0}${1}" in class "${2}")*",
					{thread.sTable->ref(name), toString(thread.sTable, args), thread.sTable->ref(clTarget->getName())});
		}
		throw RuntimeException(StringId::MemberNotFoundException);
	}

	return EvaluationResult::Failed;
}

bool TemporaryObject::partialEvaluation(size_t frameIndex, const Class* cl) {
	// "foo(...)" can be matched to three cases:
	// #1. def foo(...)
	// #2. foo -> def (...)
	// #3. foo (= FunctionObject) -> captured def * (...)
	// evaluate() searches only #1 case; This method will be called after evaluate() fails and searches #2 or #3 case.

	if (!hasName || !hasArgs)
		return false;

	std::vector<ArgumentSpec> _args = args;
	bool _hasSetArg = hasSetArg;
	ArgumentSpec _setArg = setArg;

	hasArgs = false;
	hasSetArg = false;
	args.clear();

	checkBeforeEvaluation();

	EvaluationResult result;
	if (frameIndex != SIZE_MAX)
		result = evaluateOnFrame(frameIndex);
	else
		result = evaluateDirect(false);

	chatra_assert(result != EvaluationResult::Partial);
	if (result == EvaluationResult::Failed) {
		hasArgs = true;
		args = _args;
		hasSetArg = _hasSetArg;
		setArg = _setArg;
		return false;
	}

	// Ignore the case of "foo(...)" -> "foo()" + "(...)"
	if (hasMethods()) {
		if (cl == nullptr) {
			errorAtNode(thread, ErrorLevel::Error, node, R"*(no method or member named "${0}${1}"")*",
					{thread.sTable->ref(name), toString(thread.sTable, _args)});
		}
		else {
			errorAtNode(thread, ErrorLevel::Error, node, R"*(no method or member named "${0}${1}" in class "${2}")*",
					{thread.sTable->ref(name), toString(thread.sTable, _args), thread.sTable->ref(cl->getName())});
		}
		throw RuntimeException(StringId::MemberNotFoundException);
	}

	addArgument(node, _args);
	if (_hasSetArg)
		addAssignment(node, _setArg);

	return true;
}

TemporaryObject::TemporaryObject(Storage& storage, Thread& thread) noexcept
		: Object(storage, typeId_TemporaryObject, {{StringId::Rvalue}, {StringId::SourceObject}}, thread.getId()),
		thread(thread), type(Type::Rvalue), targetRef(ref(StringId::Rvalue)) {
	targetRef.setNull();
}

void TemporaryObject::copyFrom(TemporaryObject& r) {
	ref(StringId::Rvalue).set(r.ref(StringId::Rvalue));
	ref(StringId::SourceObject).set(r.ref(StringId::SourceObject));

	type = r.type;
	targetRef = (type == Type::Rvalue ? ref(StringId::Rvalue) : r.targetRef);
	primaryIndex = r.primaryIndex;
	frameIndex = r.frameIndex;
	cl = r.cl;
	methodTable = r.methodTable;
	method0 = r.method0;
	method1 = r.method1;
	methodHasArgs = r.methodHasArgs;
	hasName = r.hasName;
	hasArgs = r.hasArgs;
	hasSetArg = r.hasSetArg;
	node = r.node;
	name = r.name;
	args = r.args;
	setArg = r.setArg;
}

void TemporaryObject::clear() {
	ref(StringId::Rvalue).setNull();
	ref(StringId::SourceObject).setNull();
	clearTargetRef();
}

Reference TemporaryObject::setRvalue() {
	type = Type::Rvalue;
	targetRef = ref(StringId::Rvalue);
	clearRestrictions();

	node = nullptr;
	return targetRef;
}

void TemporaryObject::setName(Node* node, StringId name) {
	chatra_assert(name != StringId::Invalid);
	type = Type::Empty;
	clearTargetRef();
	clearRestrictions();

	this->node = node;
	this->name = name;
	hasName = true;
}

void TemporaryObject::setReference(Reference sourceRef) {
	type = Type::ScopeRef;
	this->targetRef = sourceRef;
	clearRestrictions();
}

void TemporaryObject::invokeMethod(Node* node, Reference sourceRef, StringId name, std::vector<ArgumentSpec> args) {
	setReference(sourceRef);
	selectElement(node, name);
	addArgument(nullptr, std::move(args));
}

void TemporaryObject::setPackage(Package* package) {
	type = Type::Package;
	clearTargetRef();
	this->package = package;
	clearRestrictions();
}

void TemporaryObject::setClass(const Class* cl) {
	type = Type::Class;
	clearTargetRef();
	this->cl = cl;
	clearRestrictions();
}

void TemporaryObject::setConstructor(const Class* cl, const MethodTable* methodTable, const Method* method) {
	type = Type::Constructor;
	clearTargetRef();
	this->cl = cl;
	this->methodTable = methodTable;
	method0 = method;
	method1 = nullptr;
	methodHasArgs = true;
	clearRestrictions();
}

void TemporaryObject::setConstructorCall(Reference sourceRef, const Class* cl, const MethodTable* methodTable,
		const Method* method) {
	type = Type::ConstructorCall;
	clearTargetRef();
	ref(StringId::SourceObject).set(sourceRef);
	this->cl = cl;
	this->methodTable = methodTable;
	method0 = method;
	method1 = nullptr;
	methodHasArgs = true;
	clearRestrictions();
}

void TemporaryObject::setFrameMethod(size_t frameIndex, Package* package, const MethodTable* methodTable,
		const Method* refMethod, const Method* setMethod, bool hasArgs) {

	chatra_assert(methodTable != nullptr);
	chatra_assert(frameIndex == SIZE_MAX || (thread.frames[frameIndex].scope->getScopeType() != ScopeType::Thread &&
			thread.frames[frameIndex].scope->getScopeType() != ScopeType::Global &&
			thread.frames[frameIndex].scope->getScopeType() != ScopeType::Package));

	type = Type::FrameMethod;
	clearTargetRef();
	this->frameIndex = frameIndex;
	this->package = package;
	this->methodTable = methodTable;
	method0 = refMethod;
	method1 = setMethod;
	methodHasArgs = hasArgs;
	clearRestrictions();
}

void TemporaryObject::setObjectMethod(Reference sourceRef, const MethodTable* methodTable,
		const Method* refMethod, const Method* setMethod, bool hasArgs) {

	chatra_assert(methodTable != nullptr);

	type = Type::ObjectMethod;
	clearTargetRef();
	ref(StringId::SourceObject).set(sourceRef);
	this->methodTable = methodTable;
	method0 = refMethod;
	method1 = setMethod;
	methodHasArgs = hasArgs;
	clearRestrictions();
}

void TemporaryObject::setScopeRef(Reference targetRef) {
	type = Type::ScopeRef;
	this->targetRef = targetRef;
	clearRestrictions();
}

void TemporaryObject::setObjectRef(Reference sourceRef, Reference targetRef, size_t primaryIndex) {
	type = Type::ObjectRef;
	this->targetRef = targetRef;
	this->primaryIndex = primaryIndex;
	ref(StringId::SourceObject).set(sourceRef);
	clearRestrictions();
}

void TemporaryObject::setSelf(Reference targetRef) {
	type = Type::Self;
	this->targetRef = targetRef;
	clearRestrictions();
}

void TemporaryObject::setSuper(Reference targetRef, const Class* cl) {
	type = Type::Super;
	this->targetRef = targetRef;
	this->cl = cl;
	clearRestrictions();
}

void TemporaryObject::setExplicitSuper(Reference targetRef, const Class* cl) {
	type = Type::ExplicitSuper;
	this->targetRef = targetRef;
	this->cl = cl;
	clearRestrictions();
}

void TemporaryObject::selectElement(Node* node, StringId name) {
	chatra_assert(!requiresEvaluate());
	chatra_assert(!hasArgs && !hasSetArg);
	chatra_assert(type != Type::Empty);
	chatra_assert(!hasMethods());

	if (type == Type::Rvalue && targetRef.valueType() != ReferenceValueType::Object) {
		errorAtNode(thread, ErrorLevel::Error, node, "invalid suffix or element selection on primitive", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	if (name == StringId::Self || name == StringId::Super) {
		errorAtNode(thread, ErrorLevel::Error, node, R"(cannot access "self" or "super" from outside class)", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	if ((type == Type::ScopeRef || type == Type::ObjectRef) && targetRef.valueType() != ReferenceValueType::Object) {
		errorAtNode(thread, ErrorLevel::Error, node, "base type of element selection is not object", {});
		throw RuntimeException(StringId::UnsupportedOperationException);
	}
	if (hasRef() && targetRef.isNull()) {
		errorAtNode(thread, ErrorLevel::Error, node, "de-referencing null", {});
		throw RuntimeException(StringId::NullReferenceException);
	}

	if (node != nullptr)
		this->node = node;
	this->name = name;
	hasName = true;
}

void TemporaryObject::addArgument(Node* node, std::vector<ArgumentSpec> args) {
	chatra_assert(!hasArgs);
	chatra_assert(type != Type::Empty || hasName);
	chatra_assert(!hasMethods());

	if (this->node == nullptr)
		this->node = node;
	this->args = std::move(args);
	hasArgs = true;
}

void TemporaryObject::addAssignment(Node* node, ArgumentSpec arg) {
	chatra_assert(!hasSetArg);
	chatra_assert(!hasMethods());

	if (this->node == nullptr)
		this->node = node;
	setArg = arg;
	hasSetArg = true;
}

void TemporaryObject::addVarArg(Node* node) {
	if (hasVarArgOp) {
		errorAtNode(thread, ErrorLevel::Error, node, "applied multiple variadic arguments", {});
		throw RuntimeException(StringId::NullReferenceException);
	}

	if (this->node == nullptr)
		this->node = node;
	hasVarArgOp = true;
}

bool TemporaryObject::requiresEvaluate() const {
	return hasName || hasArgs;
}

TemporaryObject::EvaluationResult TemporaryObject::evaluate(bool raiseExceptionIfVariableNotFound) {
	chatra_assert(requiresEvaluate());
	chatra_assert(!hasRef() || !targetRef.requiresLock() || targetRef.lockedBy() == thread.getId());

	checkBeforeEvaluation();

	if (type != Type::Empty)
		return evaluateDirect(true);

	if (!hasName) {
		if (node != nullptr)
			errorAtNode(thread, ErrorLevel::Error, node, "expected expression", {});
		throw RuntimeException(StringId::MemberNotFoundException);
	}

	// "self" and "super"
	if (name == StringId::Self) {
		auto* f = thread.findClassFrame();
		if (f == nullptr) {
			errorAtNode(thread, ErrorLevel::Error, node, "invalid use of \"self\" outside of a method", {});
			throw RuntimeException(StringId::MemberNotFoundException);
		}
		auto instanceRef = f->getSelf();
		if (hasArgs) {
			return findOnObject(instanceRef, instanceRef.deref<ObjectBase>().getClass()->refMethods(), nullptr, StringId::Empty) ?
					EvaluationResult::Succeeded : EvaluationResult::Failed;
		}
		setSelf(instanceRef);
		return EvaluationResult::Succeeded;
	}

	if (name == StringId::Super) {
		auto* f = thread.findClassFrame();
		if (f == nullptr) {
			errorAtNode(thread, ErrorLevel::Error, node, "invalid use of \"super\" outside of a method", {});
			throw RuntimeException(StringId::MemberNotFoundException);
		}
		auto instanceRef = f->getSelf();
		chatra_assert(f->cl != nullptr);
		if (hasArgs) {
			for (auto* cl : f->cl->refDirectParents()) {
				if (findConstructorCall(instanceRef, cl, StringId::Invalid))
					return EvaluationResult::Succeeded;
			}
		}
		setSuper(instanceRef, f->cl);
		return EvaluationResult::Succeeded;
	}

	// Packages
	if (!hasArgs) {
		auto* package = thread.frames.back().package.findPackage(name);
		if (package != nullptr) {
			setPackage(package);
			return EvaluationResult::Succeeded;
		}
	}

	// Classes (including class constructors)
	auto* cl = thread.frames.back().package.findClass(name);
	if (cl != nullptr) {
		if (!hasArgs) {
			setClass(cl);
			return EvaluationResult::Succeeded;
		}
		if (findConstructor(cl, StringId::Invalid))
			return EvaluationResult::Succeeded;
	}

	for (size_t frameIndex = thread.frames.size() - 1; frameIndex != SIZE_MAX;
			frameIndex = thread.frames[frameIndex].parentIndex) {
		auto result = evaluateOnFrame(frameIndex);
		if (result != EvaluationResult::Failed)
			return result;
	}

	if (!raiseExceptionIfVariableNotFound)
		return EvaluationResult::Failed;

	// dump(thread.runtime.distributedSTable);
	errorAtNode(thread, ErrorLevel::Error, node, "expected expression", {});
	throw RuntimeException(StringId::MemberNotFoundException);
}

TemporaryObject::EvaluationResult TemporaryObject::evaluateAsPackageInitCall() {
	chatra_assert(requiresEvaluate());
	chatra_assert(!hasRef());
	chatra_assert(type == Type::Empty && hasName && name == StringId::Init && !hasArgs);

	size_t frameIndex = thread.frames.size() - 1;
	while (frameIndex != SIZE_MAX) {
		auto& f = thread.frames[frameIndex];
		if (f.scope->getScopeType() != ScopeType::Package) {
			frameIndex = f.parentIndex;
			continue;
		}

		auto& table = f.package.clPackage->refConstructors();
		auto* refMethod = table.find(nullptr, name, StringId::Invalid, args, {});
		if (refMethod == nullptr || refMethod->position != SIZE_MAX || refMethod->node == nullptr)
			return EvaluationResult::Failed;

		setFrameMethod(SIZE_MAX, &f.package, &table, refMethod, nullptr, hasArgs);
		resolveNativeMethod(name, StringId::Invalid);
		return EvaluationResult::Succeeded;
	}

	return EvaluationResult::Failed;
}

bool TemporaryObject::isComplete() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Rvalue) |
			1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod) |
			1U << static_cast<unsigned>(Type::ScopeRef) |
			1U << static_cast<unsigned>(Type::ObjectRef) |
			1U << static_cast<unsigned>(Type::Self);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

bool TemporaryObject::hasSource() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::ObjectMethod) |
			1U << static_cast<unsigned>(Type::ObjectRef);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

bool TemporaryObject::hasRef() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Rvalue) |
			1U << static_cast<unsigned>(Type::ScopeRef) |
			1U << static_cast<unsigned>(Type::ObjectRef) |
			1U << static_cast<unsigned>(Type::Self) |
			1U << static_cast<unsigned>(Type::Super) |
			1U << static_cast<unsigned>(Type::ExplicitSuper);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

bool TemporaryObject::hasPackage() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Package) |
			1U << static_cast<unsigned>(Type::FrameMethod);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

bool TemporaryObject::hasClass() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Class) |
			1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::Super) |
			1U << static_cast<unsigned>(Type::ExplicitSuper);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

bool TemporaryObject::hasMethods() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return (1U << static_cast<unsigned>(type) & mask) != 0;
}

const MethodTable* TemporaryObject::getMethodTable() const {
	// "methodTable" holds valid value in the case of Constructor/ConstructorCall,
	// but it is only useful for resolveNativeMethod() and does not need to external user of TemporaryObject.
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return 1U << static_cast<unsigned>(type) & mask ? methodTable : nullptr;
}

const Method* TemporaryObject::getRefMethod() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return 1U << static_cast<unsigned>(type) & mask ? method0 : nullptr;
}

const Method* TemporaryObject::getSetMethod() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return 1U << static_cast<unsigned>(type) & mask ? method1 : nullptr;
}

bool TemporaryObject::methodHasArguments() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return (1U << static_cast<unsigned>(type) & mask) && methodHasArgs;
}

const NativeMethod* TemporaryObject::getNativeMethod() const {
	static constexpr unsigned mask = 1U << static_cast<unsigned>(Type::Constructor) |
			1U << static_cast<unsigned>(Type::ConstructorCall) |
			1U << static_cast<unsigned>(Type::FrameMethod) |
			1U << static_cast<unsigned>(Type::ObjectMethod);
	return 1U << static_cast<unsigned>(type) & mask ? nativeMethod : nullptr;
}

void TemporaryObject::saveThread(Writer& w) const {
	w.out(&thread);
}

bool TemporaryObject::save(Writer& w) const {
	w.out(type);

	if (type == Type::ObjectRef)
		w.out(primaryIndex);
	if (type == Type::FrameMethod)
		w.out(frameIndex);
	if (hasPackage())
		w.out(package);
	if (hasClass())
		w.out(cl);

	if (hasMethods()) {
		w.out(methodTable);
		w.out(method0);
		w.out(method1);
		w.out(methodHasArgs);

		w.out(nativeMethod != nullptr);
		if (nativeMethod != nullptr) {
			w.out(nativeMethod->name);
			w.out(nativeMethod->subName);
		}
	}

	bool temporary = hasName || hasArgs || hasSetArg || hasVarArgOp;
	w.out(temporary);
	if (temporary) {
		w.out(hasName);
		w.out(hasArgs);
		w.out(hasSetArg);
		w.out(hasVarArgOp);
		w.out(node);
		if (hasName)
			w.out(name);
		if (hasArgs)
			w.out(args);
		if (hasSetArg)
			setArg.save(w);
	}

	return false;
}

void TemporaryObject::saveReferences(Writer& w) const {
	w.out(targetRef);
}

TemporaryObject::TemporaryObject(Thread& thread, Reader& r) noexcept
		: Object(typeId_TemporaryObject), thread(thread), type(Type::Rvalue), targetRef(Reference::SetNull()) {
	(void)r;
}

bool TemporaryObject::restore(Reader& r) {
	r.in(type);

	if (type == Type::ObjectRef)
		r.in(primaryIndex);
	if (type == Type::FrameMethod)
		r.in(frameIndex);
	if (hasPackage())
		package = r.read<Package*>();
	if (hasClass())
		r.in(cl);

	if (hasMethods()) {
		r.in(methodTable);
		r.in(method0);
		r.in(method1);
		r.in(methodHasArgs);

		if (r.read<bool>()) {
			StringId nativeMethodName, nativeMethodSubName;
			r.in(nativeMethodName);
			r.in(nativeMethodSubName);

			if (methodTable == nullptr)
				throw IllegalArgumentException();
			nativeMethod = methodTable->findNativeMethod(nativeMethodName, nativeMethodSubName);
			if (nativeMethod == nullptr)
				throw IllegalArgumentException();
		}
	}

	if (r.read<bool>()) {
		r.in(hasName);
		r.in(hasArgs);
		r.in(hasSetArg);
		r.in(hasVarArgOp);
		r.in(node);
		if (hasName)
			r.in(name);
		if (hasArgs)
			r.inArray(args);
		if (hasSetArg)
			setArg.restore(r);
	}

	return false;
}

void TemporaryObject::restoreReferences(Reader& r) {
	targetRef = r.readReference();
}


TemporaryTuple::TemporaryTuple(Storage& storage, Thread& thread) noexcept
		: ObjectBase(storage, typeId_TemporaryTuple, getClassStatic(), 0, thread.getId()),
		thread(thread) {
}

void TemporaryTuple::initialize(size_t frameIndex, Node* node) {
	this->frameIndex = frameIndex;
	this->node = node;

	valuesTop = thread.frames.back().values.size();
	delimiterPosition = SIZE_MAX;
}

void TemporaryTuple::clear() {
	// Remove all captured TemporaryObject with using exportAll() prior to call this method.
	chatra_assert(values.empty());
	frameIndex = SIZE_MAX;
}

std::vector<TemporaryObject*> TemporaryTuple::exportAll() {
	std::vector<TemporaryObject*> ret;
	for (auto& e : values)
		ret.insert(ret.cend(), e.second.cbegin(), e.second.cend());
	values.clear();
	return ret;
}

Node* TemporaryTuple::getNode() const {
	return node;
}

size_t TemporaryTuple::tupleSize() const {
	return values.size();
}

const Tuple::Key& TemporaryTuple::key(size_t position) const {
	return values[position].first;
}

void TemporaryTuple::capture(Tuple::Key key) {
	chatra_assert(thread.frames.size() - 1 == frameIndex);
	auto& f = thread.frames.back();
	chatra_assert(valuesTop < f.values.size());
	if (f.values.back()->getName() == StringId::OpTupleDelimiter)
		delimiterPosition = values.size();
	else {
		std::vector<TemporaryObject*> collected(f.values.cbegin() + valuesTop, f.values.cend());
		values.emplace_back(std::move(key), std::move(collected));
	}
	f.values.erase(f.values.cbegin() + valuesTop, f.values.cend());
}

void TemporaryTuple::copyFrom(Tuple& source) {
	chatra_assert(thread.frames.size() - 1 == frameIndex);
	auto& f = thread.frames.back();
	for (size_t i = 0; i < source.tupleSize(); i++) {
		std::vector<TemporaryObject*> v = {f.allocateTemporary()};
		v[0]->setRvalue().set(source.ref(i));
		values.emplace_back(source.key(i), v);
	}
}

void TemporaryTuple::copyFrom(Array& source) {
	chatra_assert(thread.frames.size() - 1 == frameIndex);
	auto& f = thread.frames.back();
	std::lock_guard<SpinLock> lock(source.lockValue);
	for (size_t i = 0; i < source.length; i++) {
		std::vector<TemporaryObject*> v = {f.allocateTemporary()};
		v[0]->setRvalue().set(source.container().ref(i));
		values.emplace_back(StringId::Invalid, v);
	}
}

void TemporaryTuple::copyFrom(Dict& source, Node* node) {
	chatra_assert(thread.frames.size() - 1 == frameIndex);
	auto& f = thread.frames.back();
	std::lock_guard<SpinLock> lock(source.lockValue);
	for (auto& e : source.keyToIndex) {
		std::vector<TemporaryObject*> v = {f.allocateTemporary()};
		v[0]->setRvalue().set(source.container().ref(e.second));
		if (e.first.empty()) {
			errorAtNode(thread, ErrorLevel::Error, node, "empty String cannot be used as a key for Tuple", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}
		values.emplace_back(e.first, v);
	}
}

void TemporaryTuple::extract(size_t position) {
	chatra_assert(thread.frames.size() - 1 == frameIndex);
	auto& collected = values[position].second;
	auto& f = thread.frames.back();
	f.values.insert(f.values.cend(), collected.cbegin(), collected.cend());
	collected.clear();
}

ArgumentMatcher TemporaryTuple::toArgumentMatcher() const {
	std::vector<ArgumentDef> args;

	bool delimiterFound = false;
	bool varArgFound = false;
	std::unordered_set<StringId> knownNames;

	for (size_t i = 0; i < values.size(); i++) {
		auto& key = values[i].first;
		auto& value = values[i].second.back();

		if (i == delimiterPosition) {
			if (delimiterFound) {
				errorAtNode(thread, ErrorLevel::Error, value->getNode(), "duplicated \";\"", {});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			delimiterFound = true;
			varArgFound = false;
		}

		if (varArgFound) {
			errorAtNode(thread, ErrorLevel::Error, node, "variadic arguments should be last of parameters", {});
			throw RuntimeException(StringId::IllegalArgumentException);
		}

		args.emplace_back();
		auto& arg = args.back();
		arg.name = StringId::Invalid;
		arg.cl = nullptr;
		arg.defaultOp = nullptr;

		if (value->hasVarArg()) {
			varArgFound = true;
			arg.type = (delimiterFound ? ArgumentDef::Type::DictVarArg : ArgumentDef::Type::ListVarArg);
			continue;
		}

		if (key.sid != StringId::Invalid) {
			if (knownNames.count(key.sid) != 0) {
				errorAtNode(thread, ErrorLevel::Error, value->getNode(), "redefinition of key \"${0}\"", {thread.sTable->ref(key.sid)});
				throw RuntimeException(StringId::IllegalArgumentException);
			}
			arg.name = key.sid;
			knownNames.emplace(key.sid);
		}
		arg.type = (delimiterFound ? ArgumentDef::Type::Dict : ArgumentDef::Type::List);
	}

	return ArgumentMatcher(std::move(args), {});
}

void TemporaryTuple::saveThread(Writer& w) const {
	w.out(&thread);
}

bool TemporaryTuple::save(Writer& w) const {
	w.out(frameIndex);
	if (frameIndex == SIZE_MAX)
		return true;
	w.out(node);
	w.out(valuesTop);
	w.out(delimiterPosition);
	return true;
}

void TemporaryTuple::saveReferences(Writer& w) const {
	if (frameIndex == SIZE_MAX)
		return;

	chatra_assert(frameIndex < thread.frames.size());

	w.CHATRA_OUT_POINTER(thread.scope.get(), Scope);

	auto objectMap = thread.scope->getObjectMap();
	w.out(values, [&](const std::pair<Tuple::Key, std::vector<TemporaryObject*>>& e) {
		e.first.save(w);
		w.out(e.second, [&](const TemporaryObject* value) { w.out(objectMap.at(value)); });
	});
}

TemporaryTuple::TemporaryTuple(Thread& thread, Reader& r) noexcept
		: ObjectBase(typeId_TemporaryTuple, getClassStatic()), thread(thread) {
	(void)r;
}

bool TemporaryTuple::restore(Reader& r) {
	r.in(frameIndex);
	if (frameIndex == SIZE_MAX)
		return true;
	r.in(node);
	r.in(valuesTop);
	r.in(delimiterPosition);
	return true;
}

void TemporaryTuple::restoreReferences(Reader& r) {
	if (frameIndex == SIZE_MAX)
		return;

	auto* scope = r.CHATRA_READ_RAW(Scope);

	r.inList([&]() {
		Tuple::Key key(r);
		key.restore(r);

		std::vector<TemporaryObject*> valueList;
		r.inList([&]() {
			auto sid = r.read<StringId>();
			if (!scope->has(sid))
				throw IllegalArgumentException();
			auto refValue = scope->ref(sid);
			if (refValue.valueTypeWithoutLock() != ReferenceValueType::Object)
				throw IllegalArgumentException();
			valueList.emplace_back(&refValue.derefWithoutLock<TemporaryObject>());
		});

		values.emplace_back(std::make_pair(key, std::move(valueList)));
	});
}


#ifndef CHATRA_NDEBUG
void TemporaryObject::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("TemporaryObject: type=");
	switch (type) {
	case Type::Empty:  printf("Empty");  break;
	case Type::Rvalue:  printf("Rvalue");  break;
	case Type::Package:  printf("Package, package=%p", static_cast<void*>(package));  break;
	case Type::Class:  printf("Class, class=%p", static_cast<const void*>(cl));  break;
	case Type::Constructor:  printf("Constructor, class=%p, method=%p",
			static_cast<const void*>(cl), static_cast<const void*>(method0));  break;
	case Type::ConstructorCall:  printf("ConstructorCall, class=%p, method=%p",
			static_cast<const void*>(cl), static_cast<const void*>(method0));  break;
	case Type::FrameMethod:  printf("FrameMethod, frameIndex=%s, Package=%p, refMethod=%p, setMethod=%p",
			frameIndex == SIZE_MAX ? "invalid" : std::to_string(frameIndex).c_str(),
			static_cast<void*>(package), static_cast<const void*>(method0), static_cast<const void*>(method1));  break;
	case Type::ObjectMethod:  printf("ObjectMethod, refMethod=%p, setMethod=%p",
			static_cast<const void*>(method0), static_cast<const void*>(method1));  break;
	case Type::ScopeRef:  printf("ScopeRef");  break;
	case Type::ObjectRef:  printf("ObjectRef");  break;
	case Type::Self:  printf("Self");  break;
	case Type::Super:  printf("Super");  break;
	case Type::ExplicitSuper:  printf("ExplicitSuper, class=%p", static_cast<const void*>(cl));  break;
	}
	if (hasName)
		printf(", name=\"%s\"", sTable->ref(name).c_str());
	if (hasArgs)
		printf(", args=\"%s\"", toString(sTable.get(), args).c_str());
	if (hasSetArg)
		printf(", setArgs=\"%s\"", toString(sTable.get(), setArg).c_str());
	if (hasVarArgOp)
		printf(", hasVarArg");
	printf(" ");
	Object::dump(sTable);
}
#endif // !CHATRA_NDEBUG

}  // namespace chatra
