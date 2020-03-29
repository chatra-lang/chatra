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

#include "Classes.h"
#include "Runtime.h"

namespace chatra {

void ArgumentSpec::save(Writer& w) const {
	w.out(key);
	w.out(cl);
}

void ArgumentSpec::restore(Reader& r) {
	r.in(key);
	r.in(cl);
}

std::string toString(const StringTable* sTable, const ArgumentSpec& arg) {
	std::string ret;
	if (arg.key != StringId::Invalid)
		ret += sTable->ref(arg.key);
	if (arg.cl != nullptr)
		ret += ":" + sTable->ref(arg.cl->getName());
	return ret;
}

std::string toString(const StringTable* sTable, const std::vector<ArgumentSpec>& args) {
	std::string ret = "(";
	for (size_t i = 0; i < args.size(); i++) {
		if (i != 0)
			ret += ", ";
		ret += toString(sTable, args[i]);
	}
	return ret + ")";
}

ArgumentMatcher::ArgumentMatcher() noexcept {
	listCount = listCountWithoutDefaults = dictCountWithoutDefaults = 0;
	hasListVarArg = hasDictVarArg = false;
}

ArgumentMatcher::ArgumentMatcher(std::vector<ArgumentDef> _args, std::vector<ArgumentDef> _subArgs) noexcept
		: args(std::move(_args)), subArgs(std::move(_subArgs)){

	listCount = static_cast<size_t>(std::distance(args.cbegin(),
			std::find_if(args.cbegin(), args.cend(), [](const ArgumentDef& arg) {
				return arg.type != ArgumentDef::Type::List; })));
	listCountWithoutDefaults = static_cast<size_t>(std::distance(args.cbegin(),
			std::find_if(args.cbegin(), args.cbegin() + listCount, [](const ArgumentDef& arg) {
				return arg.defaultOp != nullptr; })));
	dictCountWithoutDefaults = static_cast<size_t>(std::count_if(args.cbegin() + listCount, args.cend(),
			[](const ArgumentDef& arg) {
				return arg.type == ArgumentDef::Type::Dict && arg.defaultOp == nullptr;
			}));

	hasListVarArg = (listCount < args.size() && args[listCount].type == ArgumentDef::Type::ListVarArg);
	hasDictVarArg = (!args.empty() && args.back().type == ArgumentDef::Type::DictVarArg);

	for (size_t i = 0; i < args.size(); i++) {
		if (args[i].name != StringId::Invalid &&
				(args[i].type == ArgumentDef::Type::List || args[i].type == ArgumentDef::Type::Dict))
			byName.emplace(args[i].name, i);
	}
}

bool ArgumentMatcher::matches(const std::vector<ArgumentSpec>& args, const std::vector<ArgumentSpec>& subArgs) const {

	if (args.size() == 1 && args[0].key == StringId::AnyArgs)
		return true;

	if (subArgs.size() != this->subArgs.size())
		return false;
	for (size_t i = 0; i < subArgs.size(); i++) {
		auto& arg0 = this->subArgs[i];
		auto& arg1 = subArgs[i];
		if (arg0.cl != nullptr && arg1.cl != nullptr && !arg0.cl->isAssignableFrom(arg1.cl))
			return false;
	}

	size_t index = 0;
	size_t matchedCountWithoutDefaults = 0;

	for (auto& arg : args) {
		if (index < listCount) {
			auto& target = this->args[index++];
			if ((arg.key == StringId::Invalid || arg.key == target.name) &&
					(target.cl == nullptr || arg.cl == nullptr || target.cl->isAssignableFrom(arg.cl))) {
				if (target.defaultOp == nullptr)
					matchedCountWithoutDefaults++;
				continue;
			}
			index = listCount;
		}
		if (arg.key != StringId::Invalid) {
			auto it = byName.find(arg.key);
			if (it != byName.end()) {
				auto& target = this->args[it->second];
				if (target.cl != nullptr && arg.cl != nullptr && !target.cl->isAssignableFrom(arg.cl))
					return false;
				if (target.defaultOp == nullptr)
					matchedCountWithoutDefaults++;
				continue;
			}
		}
		if (hasListVarArg || (arg.key != StringId::Invalid && hasDictVarArg))
			continue;
		return false;
	}

	return index >= listCountWithoutDefaults &&
			matchedCountWithoutDefaults == listCountWithoutDefaults + dictCountWithoutDefaults;
}

Method::Method(Node* node, const Class* cl, StringId name, size_t position, size_t primaryPosition) noexcept
		: MethodBase(node), cl(cl), name(name), subName(StringId::Invalid), position(position), primaryPosition(primaryPosition) {}

Method::Method(Node* node, const Class* cl, StringId name, StringId subName,
		std::vector<ArgumentDef> args, std::vector<ArgumentDef> subArgs) noexcept
		: MethodBase(node), ArgumentMatcher(std::move(args), std::move(subArgs)),
		cl(cl), name(name), subName(subName), position(SIZE_MAX), primaryPosition(SIZE_MAX) {}

MethodTable::MethodTable(ForEmbeddedMethods forEmbeddedMethods) noexcept
		: source(Source::EmbeddedMethods), sourceClassPtr(nullptr) {
	(void)forEmbeddedMethods;
}

MethodTable::MethodTable(const Class* cl, Source source) noexcept : source(source), sourceClassPtr(cl) {
	assert(source == Source::ClassMethods || source == Source::ClassSuperMethods || source == Source::ClassConstructors);
	assert(cl != nullptr);
}

MethodTable::MethodTable(Package& package, Node* node) noexcept
		: source(Source::InnerFunctions), sourcePackagePtr(&package), sourceNodePtr(node) {
	assert(node != nullptr);
}

size_t MethodTable::add(Node* node, const Class* cl, StringId name, Node* syncNode) noexcept {
	size_t position = size++;
	size_t primaryPosition = SIZE_MAX;
	if (syncNode != nullptr) {
		auto it = syncMap.find(syncNode);
		if (it == syncMap.end()) {
			syncMap.emplace(syncNode, position);
			primaryPosition = position;
		}
		else
			primaryPosition = it->second;
	}
	methods.emplace_front(node, cl, name, position, primaryPosition);
	byName[name].emplace_back(&methods.front());
	return position;
}

void MethodTable::add(Node* node, const Class* cl, StringId name, StringId subName,
		std::vector<ArgumentDef> args, std::vector<ArgumentDef> subArgs) noexcept {
	methods.emplace_front(node, cl, name, subName, std::move(args), std::move(subArgs));
	byName[name].emplace_back(&methods.front());
}

void MethodTable::add(const NativeMethod& method) noexcept {
	nativeMethods.emplace_front(method);
	nativeByName.emplace(std::make_pair(method.name, method.subName), &nativeMethods.front());
}

void MethodTable::inherit(const MethodTable& r, const Class* cl) {
	ptrdiff_t delta = size - r.baseSize;
	assert(delta >= 0);

	for (auto& e : r.byName) {
		auto& methodList = byName[e.first];
		for (auto* method : e.second) {
			if (method->cl != cl)
				continue;
			if (method->position == SIZE_MAX) {
				if ((method->node->flags & NodeFlags::Native) == 0)
					methodList.emplace_back(method);
			}
			else {
				methods.emplace_front(*method);
				auto& method1 = methods.front();
				method1.position += delta;
				if (method1.primaryPosition != SIZE_MAX)
					method1.primaryPosition += delta;
				methodList.emplace_back(&method1);
				size++;
			}
		}
	}

	baseSize = size;
}

const Method* MethodTable::find(const Class* cl, StringId name, StringId subName,
		const std::vector<ArgumentSpec>& args, const std::vector<ArgumentSpec>& subArgs) const {

	auto itCandidates = byName.find(name);
	if (itCandidates == byName.end())
		return nullptr;

	for (auto it = itCandidates->second.crbegin(); it != itCandidates->second.crend(); it++) {
		auto method = *it;
		if (cl != nullptr && !method->cl->isAssignableFrom(cl))
			continue;
		if (subName != StringId::Invalid && subName != method->subName)
			continue;
		if (method->matches(args, subArgs))
			return method;
	}

	return nullptr;
}

std::vector<const Method*> MethodTable::findByName(StringId name, StringId subName) const {
	auto itCandidates = byName.find(name);
	if (itCandidates == byName.end())
		return {};

	std::vector<const Method*> ret;
	for (auto it = itCandidates->second.crbegin(); it != itCandidates->second.crend(); it++) {
		auto method = *it;
		if (subName != StringId::Invalid && subName != method->subName)
			continue;
		ret.emplace_back(method);
	}
	return ret;
}

std::vector<size_t> MethodTable::getPrimaryIndexes() const {
	std::vector<size_t> ret(size);
	for (auto& e : byName) {
		for (auto* method : e.second) {
			if (method->position != SIZE_MAX)
				ret[method->position] = (method->primaryPosition == SIZE_MAX ? method->position : method->primaryPosition);
		}
	}
	return ret;
}

const OperatorMethod* OperatorTable::find(Operator op, const Class* argCl) const {
	auto& container = byOp1[static_cast<size_t>(op)];
	auto it = container.find(argCl);
	if (it != container.end())
		return it->second;
	if (argCl != nullptr) {
		it = container.find(nullptr);
		if (it != container.end())
			return it->second;
	}
	return nullptr;
}

const OperatorMethod* OperatorTable::find(Operator op, const Class* argCl0, const Class* argCl1) const {
	auto& container = byOp2[static_cast<size_t>(op)];
	auto it = container.find(std::make_pair(argCl0, argCl1));
	if (it != container.end())
		return it->second;
	if (argCl0 != nullptr && argCl1 != nullptr) {
		it = container.find(std::make_pair(argCl0, nullptr));
		if (it != container.end())
			return it->second;
		it = container.find(std::make_pair(nullptr, argCl1));
		if (it != container.end())
			return it->second;
	}
	it = container.find(std::make_pair(nullptr, nullptr));
	return it == container.end() ? nullptr : it->second;
}

const MethodTable* MethodTableCache::scanInnerFunctions(IErrorReceiver* errorReceiver, const StringTable* sTable,
		Package& package, Node* node) {

	auto it = cache.find(node);
	if (it != cache.end())
		return it->second.get();

	std::unique_ptr<MethodTable> table(new MethodTable(package, node));
	auto* ret = table.get();
	cache.emplace(node, std::move(table));

	INullErrorReceiver nullErrorReceiver;
	if (errorReceiver == nullptr)
		errorReceiver = &nullErrorReceiver;

	addInnerMethods(*ret, *errorReceiver, sTable, package, node);
	return ret;
}

void Class::inheritParents(std::vector<const Class*> directParents) {
	this->directParents = std::move(directParents);

	// A, B(B1, B2), C(C1, C2) -> {self, A, B, B1, B2, C, C1, C2}
	std::vector<const Class*> overridePrecedence;
	std::vector<const Class*> candidates(this->directParents.crbegin(), this->directParents.crend());
	while (!candidates.empty()) {
		auto cl = candidates.back();
		candidates.pop_back();
		overridePrecedence.emplace_back(cl);
		candidates.insert(candidates.end(), cl->directParents.crbegin(), cl->directParents.crend());
	}
	parentsSet.insert(overridePrecedence.cbegin(), overridePrecedence.cend());

	std::unordered_set<const Class*> inherited;
	for (auto it = overridePrecedence.crbegin(); it != overridePrecedence.crend(); it++) {
		auto* cl = *it;
		if (inherited.count(cl))
			continue;
		inherited.emplace(cl);
		initializationOrder.emplace_back(cl);
		methods.inherit(cl->methods, cl);
		superMethods.inherit(cl->methods, cl);
	}
	initializationOrder.emplace_back(this);
}

static std::vector<const Class*> getDirectParents(IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node) {

	std::vector<const Class*> ret;
	if (!node->subNodes[SubNode::Class_Extends])
		return ret;

	auto& listNode = node->subNodes[SubNode::Class_BaseClassList];
	for (auto& nClass : listNode->subNodes) {
		auto* cl = (nClass->type == NodeType::Name ? classFinder.findClass(nClass->sid) :
				classFinder.findPackageClass(nClass->subNodes[0]->sid, nClass->subNodes[1]->sid));
		if (cl == nullptr) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nClass.get(), "unknown type name \"${0}\"",
					{sTable->ref(nClass->sid)});
			throw RuntimeException(StringId::ClassNotFoundException);
		}
		ret.emplace_back(cl);
	}

	return ret;
}

std::unordered_map<Node*, Node*> Class::getSyncMap(Node* node) {
	// Find parent of "var" statement
	std::unordered_map<Node*, Node*> varSyncMap;
	for (auto& n : node->blockNodes) {
		if (n->type != NodeType::Sync)
			continue;
		for (auto& nSub : n->blockNodes)
			varSyncMap.emplace(nSub.get(), n.get());
	}
	return varSyncMap;
}

void Class::addParentClass(const Class* clParent) {
	directParents.emplace_back(clParent);
	parentsSet.insert(clParent->parentsSet.cbegin(), clParent->parentsSet.cend());
	parentsSet.emplace(clParent);
}

Class::Class(StringId name, AddCopyConstructor addCopyConstructor) noexcept
		: Class(nullptr, nullptr, name, nullptr) {

	(void)addCopyConstructor;

	ArgumentDef arg;
	arg.type = ArgumentDef::Type::List;
	arg.name = StringId::a0;
	arg.cl = this;
	constructors.add(nullptr, this, StringId::Init, StringId::Invalid, {arg}, {});
}

void Class::addDefaultConstructor(ObjectBuilder objectBuilder) {
	this->objectBuilder = objectBuilder;
	constructors.add(nullptr, this, StringId::Init, StringId::Invalid, {}, {});
}

void Class::addConvertingConstructor(const Class* sourceCl) {
	assert(sourceCl != this);
	ArgumentDef arg;
	arg.type = ArgumentDef::Type::List;
	arg.name = StringId::a0;
	arg.cl = sourceCl;
	constructors.add(nullptr, this, StringId::Init, StringId::Invalid, {arg}, {});
}

Class::Class(IErrorReceiver& errorReceiver, const StringTable* sTable, IClassFinder& classFinder,
		Package* package, Node* node,
		ObjectBuilder objectBuilder, const std::vector<NativeMethod>& nativeMethods) noexcept
		: Class(package, node, node->sid, nullptr) {

	try {
		initialize(errorReceiver, sTable, classFinder, objectBuilder, nativeMethods);
	}
	catch (...) {
		// do nothing
	}
}

Class::Class(Package* package, Node* node) noexcept : Class(package, node, node->sid, nullptr) {
}

void Class::initialize(IErrorReceiver& errorReceiver, const StringTable* sTable, IClassFinder& classFinder,
		ObjectBuilder objectBuilder, const std::vector<NativeMethod>& nativeMethods) {
	this->objectBuilder = objectBuilder;

	assert(node->blockNodesState == NodeState::Parsed);

	bool isClass = (node->type == NodeType::Class);
	if (isClass)
		inheritParents(getDirectParents(errorReceiver, sTable, classFinder, node));

	struct ClassFinder : public WrappedClassFinder {
		const Class* clSelf;
		StringId name;
		ClassFinder(const Class* clSelf, StringId name, IClassFinder& classFinder) : WrappedClassFinder(classFinder), clSelf(clSelf), name(name) {}

		const Class* findClass(StringId name) override {
			return clSelf != nullptr && this->name == name ? clSelf : classFinder.findClass(name);
		}
	} wrappedClassFinder(isClass ? this : nullptr, name, classFinder);

	std::unordered_map<Node*, Node*> varSyncMap = getSyncMap(node);

	bool hasConstructor = false;
	for (auto& n : node->symbols) {
		if (n->type == NodeType::Var) {
			for (auto& defNode : n->subNodes[SubNode::Var_Definitions]->subNodes) {
				auto name = (defNode->type == NodeType::Name ? defNode->sid : defNode->subNodes[0]->sid);

				auto duplicatedMethods = methods.findByName(name, StringId::Invalid);
				for (auto* m : duplicatedMethods) {
					if (m->position != SIZE_MAX) {
						errorAtNode(errorReceiver, ErrorLevel::Error, defNode.get(), "duplicated variable name", {});
						errorAtNode(errorReceiver, ErrorLevel::Error, m->node, "previous declaration is here", {});
						throw RuntimeException(StringId::ParserErrorException);
					}
				}

				Node* syncNode = nullptr;
				auto it = varSyncMap.find(n.get());
				if (it != varSyncMap.end())
					syncNode = it->second;

				methods.add(defNode.get(), this, name, syncNode);
			}
		}
		else if (n->type == NodeType::Def) {
			if (n->sid == StringId::Init) {
				hasConstructor = true;
				addMethod(constructors, errorReceiver, sTable, wrappedClassFinder, n.get(), isClass ? this : nullptr);
			}
			else
				addMethod(methods, errorReceiver, sTable, wrappedClassFinder, n.get(), isClass ? this : nullptr);
		}
	}

	if (!hasConstructor)
		constructors.add(nullptr, this, StringId::Init, StringId::Invalid, {}, {});

	for (auto& method : nativeMethods) {
		if (method.name == StringId::Init)
			constructors.add(method);
		else
			methods.add(method);
	}
}

void ClassTable::add(const ClassTable& r) {
	byName.insert(r.byName.cbegin(), r.byName.cend());
}

static std::pair<StringId, const Class*> findArgumentClass(IClassFinder& classFinder, Node* nValue) {
	if (nValue->type == NodeType::Name)
		return std::make_pair(nValue->sid, classFinder.findClass(nValue->sid));
	if (nValue->op == Operator::ElementSelection) {
		if (nValue->subNodes[0]->op == Operator::ElementSelection) {
			return std::make_pair(nValue->subNodes[0]->subNodes[1]->sid,
					classFinder.findPackageClass(nValue->subNodes[0]->subNodes[0]->sid, nValue->subNodes[0]->subNodes[1]->sid));
		}
		// class.constructorSpecifier or package.class
		auto* cl = classFinder.findClass(nValue->subNodes[0]->sid);
		if (cl != nullptr)
			return std::make_pair(nValue->subNodes[0]->sid, cl);

		return std::make_pair(nValue->subNodes[1]->sid,
				classFinder.findPackageClass(nValue->subNodes[0]->sid, nValue->subNodes[1]->sid));
	}
	return std::make_pair(StringId::Invalid, nullptr);
}

static std::pair<StringId, const Class*> findArgumentClassAsPrimitive(IClassFinder& classFinder, Node* nValue) {
    if (nValue->type != NodeType::Literal) {
        assert(nValue->op == Operator::UnaryMinus || nValue->op == Operator::UnaryPlus);
        nValue = nValue->subNodes[0].get();
        assert(nValue->type == NodeType::Literal);
    }

	switch (nValue->literalValue->type) {
	case LiteralType::Bool:  return std::make_pair(StringId::Bool, Bool::getClassStatic());
	case LiteralType::Int:  return std::make_pair(StringId::Int, Int::getClassStatic());
	case LiteralType::Float:  return std::make_pair(StringId::Float, Float::getClassStatic());
	case LiteralType::String:
	case LiteralType::MultilingualString:
		return std::make_pair(StringId::String, classFinder.findClass(StringId::String));  // Is it safe?
	default:
		assert(false);
		return std::make_pair(StringId::Invalid, nullptr);
	}
}

ArgumentDef nodeToArgument(IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node) {

	ArgumentDef arg;

	if (node->op == Operator::VarArg) {
		arg.name = node->subNodes[0]->sid;
	}
	else if (node->op == Operator::Pair) {
		arg.name = node->subNodes[0]->sid;
		arg.defaultOp = node;
	}
	else
		arg.name = node->sid;

	if (arg.defaultOp == nullptr) {
		arg.cl = nullptr;
		return arg;
	}

	auto* nValue = arg.defaultOp->subNodes[1].get();
	if (nValue->type == NodeType::Literal && nValue->literalValue->type == LiteralType::Null)
		arg.cl = nullptr;
	else {
		auto c = findArgumentClass(classFinder, nValue);
		if (c.first != StringId::Invalid)
			arg.defaultOp = nullptr;
		else if (nValue->type == NodeType::Literal
		        || nValue->op == Operator::UnaryMinus || nValue->op == Operator::UnaryPlus)
			c = findArgumentClassAsPrimitive(classFinder, nValue);
		else {
			assert(nValue->op == Operator::Call);
			c = findArgumentClass(classFinder, nValue->subNodes[0].get());
		}

		arg.cl = c.second;
		if (arg.cl == nullptr) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nValue, "unknown type name \"${0}\"", {sTable->ref(c.first)});
			throw RuntimeException(StringId::ClassNotFoundException);
		}
	}

	return arg;
}

std::vector<ArgumentDef> tupleToArguments(IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node) {

	std::vector<ArgumentDef> ret;
	ret.reserve(node->subNodes.size());

	bool delimiterFound = false;
	for (auto& n : node->subNodes) {
		if (n->op == Operator::TupleDelimiter) {
			delimiterFound = true;
			continue;
		}

		ret.emplace_back(nodeToArgument(errorReceiver, sTable, classFinder, n.get()));

		auto& arg = ret.back();
		if (n->op == Operator::VarArg)
			arg.type = (delimiterFound ? ArgumentDef::Type::DictVarArg : ArgumentDef::Type::ListVarArg);
		else
			arg.type = (delimiterFound ? ArgumentDef::Type::Dict : ArgumentDef::Type::List);
	}
	return ret;
}

void addMethod(MethodTable& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node, const Class* cl) {

	StringId subName = StringId::Invalid;
	auto args = tupleToArguments(errorReceiver, sTable, classFinder, node->subNodes[SubNode::Def_Parameter].get());
	std::vector<ArgumentDef> subArgs;

	auto& nVariation = node->subNodes[SubNode::Def_Variation];
	if (nVariation) {
		if (node->sid != StringId::Init) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nVariation.get(), "only \"init()\" can have variation suffix", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
		subName = nVariation->sid;
	}

	auto& nOperator = node->subNodes[SubNode::Def_Operator];
	if (nOperator) {
		if (node->sid == StringId::Init || node->sid == StringId::Deinit) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nOperator.get(), "\"init()\" and \"deinit()\" cannot have operator suffix", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
		if (nOperator->sid != StringId::Set) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nOperator.get(), "unknown operator suffix \"${0}\"",
					{sTable->ref(nOperator->sid)});
			throw RuntimeException(StringId::ParserErrorException);
		}

		subName = nOperator->sid;
		auto& nOperatorParameter = node->subNodes[SubNode::Def_OperatorParameter];
		subArgs = tupleToArguments(errorReceiver, sTable, classFinder, nOperatorParameter.get());
		if (subArgs.size() != 1) {
			errorAtNode(errorReceiver, ErrorLevel::Error, nOperatorParameter.get(), "expected single argument", {});
			throw RuntimeException(StringId::ParserErrorException);
		}

		if (node->flags & NodeFlags::Native) {
			errorAtNode(errorReceiver, ErrorLevel::Error, node, "function with operator suffix cannot be declared as native", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
	}

	if (node->sid == StringId::Init && cl == nullptr && (!args.empty() || subName != StringId::Invalid)) {
		errorAtNode(errorReceiver, ErrorLevel::Error, nVariation.get(), "\"init()\" for package cannot have arguments nor suffixes", {});
		throw RuntimeException(StringId::ParserErrorException);
	}
	if (node->sid == StringId::Deinit && !args.empty()) {
		errorAtNode(errorReceiver, ErrorLevel::Error, nVariation.get(), "\"deinit()\" for package cannot have arguments", {});
		throw RuntimeException(StringId::ParserErrorException);
	}

	auto exists = table.findByName(node->sid, StringId::Invalid);
	if (nOperator) {
		auto it = std::find_if(exists.cbegin(), exists.cend(), [](const Method* m) -> bool { return m->node->flags & NodeFlags::Native; });
		if (it != exists.cend()) {
			errorAtNode(errorReceiver, ErrorLevel::Error, node, "function with operator suffix cannot have the same name with native functions", {});
			if ((*it)->node != nullptr)
				errorAtNode(errorReceiver, ErrorLevel::Error, (*it)->node, "one of previous declaration is here", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
	}
	else if (node->sid != StringId::Init && node->flags & NodeFlags::Native) {
		auto it = std::find_if(exists.cbegin(), exists.cend(), [](const Method* m) { return m->subName != StringId::Invalid; });
		if (it != exists.cend()) {
			errorAtNode(errorReceiver, ErrorLevel::Error, node, "native function cannot have the same name with functions with operator suffix", {});
			if ((*it)->node != nullptr)
				errorAtNode(errorReceiver, ErrorLevel::Error, (*it)->node, "one of previous declaration is here", {});
			throw RuntimeException(StringId::ParserErrorException);
		}
	}

	table.add(node, cl, node->sid, subName, std::move(args), std::move(subArgs));
}

void addInnerMethods(MethodTable& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node) {
	assert(node->type != NodeType::ScriptRoot && node->type != NodeType::Class);
	for (auto& n : node->symbols) {
		if (n->type == NodeType::Def)
			addMethod(table, errorReceiver, sTable, classFinder, n.get());
	}
}

bool UserObjectBase::save(Writer& w) const {
	w.out(getClass());
	w.out(uninitializedParents, [&](const Class* cl) {
		w.out(cl);
	});
	w.out(deinitCalled);
	w.out(nativePtr != nullptr);
	if (nativePtr) {
		auto* package = getClass()->getPackage();
		w.out(package->interface->saveNativePtr(*package, nativePtr.get()));
	}
	return true;
}

bool UserObjectBase::restore(Reader& r) {
	r.inList([&]() {
		uninitializedParents.emplace(r.read<Class*>());
	});
	r.in(deinitCalled);
	if (r.read<bool>()) {
		auto* package = getClass()->getPackage();
		auto* ptr = package->interface->restoreNativePtr(*package, r.read<std::vector<uint8_t>>());
		if (ptr == nullptr)
			throw IllegalArgumentException();
		nativePtr.reset(ptr);
	}
	return true;
}

void UserObjectBase::restoreReferences(Reader& r) {
	(void)r;
	restoreReferencesForClass();
}

const Class* getReferClass(Reference ref) {
	switch (ref.valueType()) {
	case ReferenceValueType::Bool:  return Bool::getClassStatic();
	case ReferenceValueType::Int:  return Int::getClassStatic();
	case ReferenceValueType::Float:  return Float::getClassStatic();
	default:
		return ref.isNull() ? nullptr : ref.deref<ObjectBase>().getClass();
	}
}

void Tuple::Key::save(Writer& w) const {
	w.out(sid);
	w.out(str);
}

void Tuple::Key::restore(Reader& r) {
	r.in(sid);
	r.in(str);
}

Tuple::Tuple(Storage& storage, Requester requester) noexcept
		: ObjectBase(storage, typeId_Tuple, getClassStatic(), 0, requester) {
	assert(requester != InvalidRequester);
}

Tuple::Tuple(Storage& storage, const StringTable* sTable, const std::vector<std::pair<Key, Reference>>& args,
		Requester requester) noexcept
		: ObjectBase(storage, typeId_Tuple, getClassStatic(), args.size(), requester) {

	// Tuple is always temporary value, never at outside of scope
	assert(requester != InvalidRequester);

	indexToKey.reserve(args.size());

	for (size_t i = 0; i < args.size(); i++) {
		auto& e = args[i];

		indexToKey.emplace_back(e.first);
		if (e.first.sid != StringId::Invalid)
			sidToIndex.emplace(e.first.sid, i);
		else if (!e.first.str.empty()) {
			auto sid = sTable->find(e.first.str);
			if (sid == StringId::Invalid)
				strToIndex.emplace(e.first.str, i);
			else {
				sidToIndex.emplace(sid, i);
				indexToKey.back().sid = sid;
			}
		}

		ref(i).set(e.second);
	}
}

const Tuple::Key& Tuple::key(size_t position) const {
	return indexToKey[position];
}

size_t Tuple::find(const StringTable* sTable, StringId key) const {
	auto it0 = sidToIndex.find(key);
	if (it0 != sidToIndex.end())
		return it0->second;

	auto it1 = strToIndex.find(sTable->ref(key));
	if (it1 != strToIndex.end())
		return it1->second;

	return SIZE_MAX;
}

size_t Tuple::find(const StringTable* sTable, const std::string& key) const {
	auto it1 = strToIndex.find(key);
	if (it1 != strToIndex.end())
		return it1->second;

	auto sid = sTable->find(key);
	if (sid == StringId::Invalid)
		return SIZE_MAX;

	auto it0 = sidToIndex.find(sid);
	if (it0 != sidToIndex.end())
		return it0->second;

	return SIZE_MAX;
}

std::vector<ArgumentSpec> Tuple::toArgs() const {
	std::vector<ArgumentSpec> ret;
	ret.reserve(size());
	for (size_t i = 0; i < size(); i++) {
		auto& key = indexToKey[i];
		auto sid = (key.sid != StringId::Invalid ? key.sid
				: !key.str.empty() ? StringId::AnyString : StringId::Invalid);

		auto r0 = ref(i);
		assert(!r0.requiresLock() || r0.lockedBy() != InvalidRequester);
		ret.emplace_back(sid, getReferClass(r0));
	}
	return ret;
}

bool Tuple::save(Writer& w) const {
	w.out(indexToKey);
	return false;
}

Tuple::Tuple(Reader& r) noexcept : ObjectBase(typeId_Tuple, getClassStatic()) {
	(void)r;
}

bool Tuple::restore(Reader& r) {
	r.inArray(indexToKey);
	for (size_t i = 0; i < indexToKey.size(); i++) {
		auto& key = indexToKey[i];
		if (key.sid != StringId::Invalid)
			sidToIndex.emplace(key.sid, i);
		else
			strToIndex.emplace(key.str, i);
	}
	return false;
}


#ifndef NDEBUG
void ArgumentMatcher::dumpArgumentMatcher(const std::shared_ptr<StringTable>& sTable) const {
	printf("(");
	for (size_t i = 0; i < args.size(); i++) {
		if (i != 0)
			printf(", ");
		auto& arg = args[i];
		printf("\"%s\"", sTable->ref(arg.name).c_str());
		if (arg.cl != nullptr)
			printf(":\"%s\"", sTable->ref(arg.cl->getName()).c_str());
		if (arg.type == ArgumentDef::Type::ListVarArg || arg.type == ArgumentDef::Type::DictVarArg)
			printf("...");
		if (arg.defaultOp != nullptr)
			printf("=%p", static_cast<void*>(arg.defaultOp->subNodes[1].get()));
	}
	printf(")");
}

void Method::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("\"%s\"", sTable->ref(name).c_str());
	if (subName != StringId::Invalid)
		printf(".\"%s\"", sTable->ref(subName).c_str());
	if (position != SIZE_MAX) {
		printf(": cl=%p, variable[%u/%s]\n", static_cast<const void*>(cl), static_cast<unsigned>(position),
				primaryPosition == SIZE_MAX ? "-" : std::to_string(primaryPosition).c_str());
	}
	else {
		dumpArgumentMatcher(sTable);
		printf(": cl=%p, node=%p\n", static_cast<const void*>(cl), static_cast<void*>(node));
	}
}

void OperatorMethod::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("%s: ", getOpDescription(op).c_str());
	for (size_t i = 0; i < args.size(); i++) {
		if (i != 0)
			printf(", ");
		auto& arg = args[i];
		printf("\"%s\"", sTable->ref(arg.name).c_str());
		if (arg.cl != nullptr)
			printf(":\"%s\"", sTable->ref(arg.cl->getName()).c_str());
	}
	printf(": node=%p\n", static_cast<void*>(node));
}

void MethodTable::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("    %u method groups, %u refs\n", static_cast<unsigned>(byName.size()), static_cast<unsigned>(size));
	for (auto& e : byName) {
		for (auto* method : e.second) {
			printf("    ");
			method->dump(sTable);
		}
	}
}

void OperatorTable::dump(const std::shared_ptr<StringTable>& sTable) const {
	for (size_t i = 0; i < NumberOfOperators; i++) {
		for (auto& e : byOp1[i])
			e.second->dump(sTable);
		for (auto& e : byOp2[i])
			e.second->dump(sTable);
	}
}

void AsyncOperatorTable::dump(const std::shared_ptr<StringTable>& sTable) const {
	read([&](const OperatorTable& table) { table.dump(sTable); });
}

void Class::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("Class \"%s\" %p: directParents={", sTable->ref(name).c_str(), static_cast<const void*>(this));
	for (size_t i = 0; i < directParents.size(); i++)
		printf("%s%p", i == 0 ? "" : ", ", static_cast<const void*>(directParents[i]));
	printf("}\n");
	methods.dump(sTable);
	constructors.dump(sTable);
}

void ClassTable::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("ClassTable %u classes\n", static_cast<unsigned>(byName.size()));
	for (auto& e : byName) {
		printf("  ");
		e.second->dump(sTable);
	}
}

void ObjectBase::dump(const std::shared_ptr<chatra::StringTable>& sTable) const {
	printf("Object(class=%s): ", sTable->ref(getClass()->getName()).c_str());
	Object::dump(sTable);
}

void Tuple::dump(const std::shared_ptr<StringTable>& sTable) const {
	printf("Tuple: ");
	ObjectBase::dump(sTable);
	printf(" ");
	if (sidToIndex.empty() && strToIndex.empty())
		printf(" empty");
	for (auto&e : sidToIndex)
		printf(" \"%s\"[%u]", sTable->ref(e.first).c_str(), static_cast<unsigned>(e.second));
	for (auto&e : strToIndex)
		printf(" \"%s\"[%u]", e.first.c_str(), static_cast<unsigned>(e.second));
	printf("\n");
}
#endif // !NDEBUG

}  // namespace chatra
