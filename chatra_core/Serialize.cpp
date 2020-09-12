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

#include "Serialize.h"
#include "Runtime.h"

namespace chatra {

#ifndef NDEBUG

constexpr uint16_t tagMarker = 0xB5A6;
constexpr uint32_t resyncMarker = 0xF1E2D3C4;
static int lastValidTag = 0;

void Writer::saveTypeTag(TypeTag tag) {
	outRawInteger(tagMarker);
	outRawInteger(tag);
}

void Reader::restoreTypeTag(TypeTag tag) {
	auto marker = readRawInteger<uint16_t>();
	if (marker != tagMarker) {
		std::fprintf(stderr, "TypeTag marker not found\n");
		throw IllegalArgumentException();
	}
	auto foundTag = readRawInteger<TypeTag>();
	if (foundTag != tag) {
		std::fprintf(stderr, "TypeTag marker found, but value of TypeTag doesn't match: expected = %u, found = %u\n",
				static_cast<unsigned>(tag), static_cast<unsigned>(foundTag));
		throw IllegalArgumentException();
	}
}

#endif // !NDEBUG

Writer& Writer::saveResync(int tag) {
#ifdef NDEBUG
	(void)tag;
#else
	outRawInteger(resyncMarker);
	outRawInteger(tag);
#endif
	return *this;
}

Reader& Reader::restoreResync(int tag) {
#ifdef NDEBUG
	(void)tag;
#else
	try {
		auto marker = readRawInteger<uint32_t>();
		if (marker != resyncMarker) {
			std::fprintf(stderr, "Resync maker not found: tag in previous marker = %d\n", lastValidTag);
			throw IllegalArgumentException();
		}
	}
	catch (...) {
		std::fprintf(stderr, "Cannot read resync maker: TypeId in previous marker = %d\n", lastValidTag);
		throw IllegalArgumentException();
	}

	lastValidTag = readRawInteger<int>();
	if (tag != lastValidTag) {
		std::fprintf(stderr, "Resync marker found, but TypeId doesn't match: expected = %d, found = %d\n", tag, lastValidTag);
		throw IllegalArgumentException();
	}
#endif
	return *this;
}


std::vector<uint8_t> Writer::getBytes(unsigned version) {
	std::vector<uint8_t> ret;
	std::vector<uint8_t> tmp;
	auto* oldBuffer = buffer;
	buffer = &tmp;

	out(version);
	ret.reserve(ret.size() + tmp.size());
	ret.insert(ret.end(), tmp.cbegin(), tmp.cend());

	for (auto& e : chunks) {
		tmp.clear();
		out(e.first);
		out(e.second.size());
		ret.reserve(ret.size() + tmp.size() + e.second.size());
		ret.insert(ret.end(), tmp.cbegin(), tmp.cend());
		ret.insert(ret.end(), e.second.cbegin(), e.second.cend());
	}

	buffer = oldBuffer;
	return ret;
}

Writer& Writer::select(const std::string& chunk) {
	buffer = &chunks[chunk];
	return *this;
}

void Writer::setEntityMap(
		std::unordered_map<const Class*, size_t> classMap,
		std::unordered_map<const Node*, ptrdiff_t> rootMap,
		std::unordered_map<const Node*, const Node*> parentMap) {

	this->classMap = std::move(classMap);
	this->rootMap = std::move(rootMap);
	this->parentMap = std::move(parentMap);
	entityMapEnabled = true;
}

static size_t find(const std::vector<std::shared_ptr<Node>>& childNodes, const Node* targetNode) {
	auto it = std::find_if(childNodes.cbegin(), childNodes.cend(),
			[&](const std::shared_ptr<Node>& n) { return n.get() == targetNode; });
	return it == childNodes.cend() ? SIZE_MAX : static_cast<size_t>(std::distance(childNodes.cbegin(), it));
}

Writer& Writer::outNode(const Node* node) {
	assert(entityMapEnabled);
	CHATRA_SAVE_TYPE_TAG(Node);

	out(node == nullptr ? 0 :
			node->flags & NodeFlags::ThreadSpecial ? (node->type == NodeType::Operator ? 2 : 3) :
			node->flags & NodeFlags::InitialNode ? 4 :
			1);

	if (node == nullptr)
		return *this;

	// Special nodes
	if (node->flags & NodeFlags::ThreadSpecial) {
		if (node->type == NodeType::Operator)
			out(node->op);
		else
			out(node->type);
		return *this;
	}

	if (node->flags & NodeFlags::InitialNode) {
		out(rootMap.at(node));
		return *this;
	}

	std::deque<const Node*> nodes;
	nodes.emplace_front(node);
	for (auto* n = node; n->type != NodeType::ScriptRoot; ) {
		n = parentMap.at(n);
		nodes.emplace_front(n);
	}

	out(rootMap.at(nodes.front()));
	out(nodes.size() - 1);
	while (nodes.size() > 1) {
		auto* n0 = *(nodes.begin() + 0);
		auto* n1 = *(nodes.begin() + 1);
		nodes.pop_front();

		// It is possible to save bytes when n0 has only one child Node, but this type of optimization may prevent hot-fix.
		// if (n0->blockNodes.size() + n1->subNodes.size() == 1) ...

		switch (n1->type) {
		case NodeType::Class:
			out(0);
			out(n1->sid);
			continue;

		case NodeType::Def: {
			size_t nameCount = 0;
			size_t nameIndex = SIZE_MAX;
			for (auto& n : n0->blockNodes) {
				if (n->type != NodeType::Def || n->sid != n1->sid)
					continue;
				if (n.get() == n1)
					nameIndex = nameCount;
				nameCount++;
			}
			out(nameCount == 1 ? 1 : 2);
			out(n1->sid);
			if (nameCount != 1)
				out(nameIndex);
			continue;
		}

		default:
			break;
		}

		size_t index0 = find(n0->blockNodes, n1);
		if (index0 != SIZE_MAX) {
			out(3);
			out(index0);
			continue;
		}
		size_t index1 = find(n0->subNodes, n1);
		if (index1 != SIZE_MAX) {
			out(4);
			out(index1);
			continue;
		}

		assert(false);
	}
	return *this;
}

Writer& Writer::outClass(const Class* cl) {
	assert(entityMapEnabled);
	CHATRA_SAVE_TYPE_TAG(Class);

	if (cl == nullptr)
		out(0);
	else if (cl->getNode() == nullptr) {
		out(2);
		out(classMap.at(cl));
	}
	else {
		out(1);
		out(cl->getNode());
	}
	return *this;
}

Writer& Writer::outMethod(const Method* method) {
	assert(entityMapEnabled);
	CHATRA_SAVE_TYPE_TAG(Method);

	if (method == nullptr)
		out(0);
	else if (method->node == nullptr) {
		assert(method->cl != nullptr);

		// default constructor or copy/converting constructor
		if (method->args.empty()) {
			out(2);
			out(method->cl);
		}
		else {
			assert(method->args.size() == 1);
			out(3);
			out(method->cl);
			out(method->args[0].cl);
		}
	}
	else {
		out(1);
		out(method->node);
	}
	return *this;
}

Writer& Writer::outMethodTable(const MethodTable* methodTable) {
	assert(entityMapEnabled);
	CHATRA_SAVE_TYPE_TAG(MethodTable);

	if (methodTable == nullptr) {
		out(0);
		return *this;
	}

	switch (methodTable->getSource()) {
	case MethodTable::Source::EmbeddedMethods:  out(1); return *this;
	case MethodTable::Source::ClassMethods:  out(2); out(methodTable->sourceClass()); return *this;
	case MethodTable::Source::ClassSuperMethods:  out(3); out(methodTable->sourceClass()); return *this;
	case MethodTable::Source::ClassConstructors:  out(4); out(methodTable->sourceClass()); return *this;
	case MethodTable::Source::InnerFunctions:  
		out(5);
		out(methodTable->sourcePackage()->getId());
		out(methodTable->sourceNode());
		return *this;
	default:
		throw InternalError();
	}
}

Writer& Writer::outReference(const Reference& ref) {
	CHATRA_SAVE_TYPE_TAG(Reference);
	CHATRA_OUT_POINTER(ref.internal_nodePtr(), ReferenceNode);
	return *this;
}

void Reader::checkSpace(size_t size) {
	if (offset + size > buffer->size())
		throw IllegalArgumentException();
}

#ifdef NDEBUG
Reader::PointerInfo& Reader::readPointerInfo() {
	CHATRA_RESTORE_TYPE_TAG(Pointer);
	auto index = read<size_t>();
	if (index >= pointerList.size())
		throw IllegalArgumentException();
	return pointerList.at(index);
}
#else // NDEBUG
Reader::PointerInfo& Reader::readPointerInfo(PointerType type) {
	CHATRA_RESTORE_TYPE_TAG(Pointer);
	auto index = read<size_t>();
	if (index == 0)
		return pointerList.at(0);
	if (index >= pointerList.size())
		throw IllegalArgumentException();

	auto savedType = read<PointerType>();
	assert(savedType == type);
	assert(savedType == pointerList.at(index).type);

	return pointerList.at(index);
}
#endif  // NDEBUG

Reader::Reader(RuntimeImp& runtime) noexcept : runtime(runtime) {
#ifdef NDEBUG
	pointerList.emplace_back(PointerInfo{PointerMode::Raw, nullptr});
#else
	pointerList.emplace_back(PointerInfo{PointerMode::Raw, nullptr, static_cast<PointerType>(0)});
#endif
}

#ifndef NDEBUG
Reader::~Reader() {
	for (auto& p : pointerList)
		assert(p.mode != PointerMode::Unique);
}
#endif

void Reader::parse(unsigned expectedVersion, const std::vector<uint8_t>& bytes) {
	buffer = &bytes;
	offset = 0;

	if (read<unsigned>() != expectedVersion)
		throw IllegalArgumentException();

	while (offset < buffer->size()) {
		auto chunk = read<std::string>();
		auto chunkSize = read<size_t>();
		std::vector<uint8_t> data(buffer->cbegin() + offset, buffer->cbegin() + offset + chunkSize);
		offset += chunkSize;
		chunks.emplace(std::move(chunk), std::move(data));
	}

	select("");
}

Reader& Reader::select(const std::string& chunk) {
	if (chunks.count(chunk) == 0)
		throw IllegalArgumentException();
	buffer = &chunks.at(chunk);
	offset = 0;
	return *this;
}

void Reader::setEntityMap(
		const std::unordered_map<const Class*, size_t>& classMap,
		const std::unordered_map<const Node*, ptrdiff_t>& rootMap,
		const std::vector<const Class*>& systemClasses,
		const std::vector<const Method*>& systemMethods) {

	using ClassMapValueType = std::unordered_map<const Class*, size_t>::value_type;
	this->classMap.resize(std::max_element(classMap.cbegin(), classMap.cend(),
			[](const ClassMapValueType& a, const ClassMapValueType& b) {
		return a.second < b.second;
	})->second + 1, nullptr);
	for (auto& e : classMap)
		this->classMap[e.second] = e.first;

	for (auto& e : rootMap)
		this->rootMap.emplace(e.second, e.first);

	for (auto* cl : systemClasses)
		add(cl);
	for (auto* method : systemMethods)
		add(method);

	entityMapEnabled = true;
}

void Reader::add(const Class* cl) {
	assert(cl->getNode() != nullptr);
	assert(nodeClassMap.count(cl->getNode()) == 0);
	nodeClassMap.emplace(cl->getNode(), cl);
	add(&cl->refMethods());
	add(&cl->refSuperMethods());
	add(&cl->refConstructors());
}

void Reader::add(const Method* method) {
	if (method->node != nullptr) {
		assert(nodeMethodMap.count(method->node) == 0);
		nodeMethodMap.emplace(method->node, method);
	}
}

void Reader::add(const MethodTable* methodTable) {
	if (methodTable->getSource() == MethodTable::Source::InnerFunctions)
		nodeMethodTableMap.emplace(methodTable->sourceNode(), methodTable);
	methodTable->forEach([&](const Method& method) {
		add(&method);
	});
}

Node* Reader::readNode() {
	assert(entityMapEnabled);
	CHATRA_RESTORE_TYPE_TAG(Node);

	switch (read<int>()) {
	case 0:
		return nullptr;
	case 1:
		break;
	case 2:
		return Thread::refSpecialNode(NodeType::Operator, read<Operator>());
	case 3:
		return Thread::refSpecialNode(read<NodeType>());
	case 4:
		return runtime.restorePackageNode(*this, static_cast<PackageId>(read<ptrdiff_t>()), true);
	default:
		throw IllegalArgumentException();
	}

	auto packageIndex = read<ptrdiff_t>();
	auto descentCount = read<size_t>();

	Node* node = nullptr;
	if (packageIndex < 0)
		node = const_cast<Node*>(rootMap.at(packageIndex));
	else
		node = runtime.restorePackageNode(*this, static_cast<PackageId>(packageIndex), false);

	for (size_t i = 0; i < descentCount; i++) {
		if (packageIndex >= 0)
			runtime.restoreNode(*this, static_cast<PackageId>(packageIndex), node);

		auto indexMode = read<int>();
		switch (indexMode) {
		case 0: {  // Class
			auto sid = read<StringId>();
			auto it = std::find_if(node->symbols.cbegin(), node->symbols.cend(), [&](const std::shared_ptr<Node>& n) {
				return n->sid == sid; });
			if (it == node->symbols.cend()) {
				errorAtNode(runtime, ErrorLevel::Error, node, "restore: class \"${0}\" is not found",
						{runtime.primarySTable->ref(sid)});
				throw IllegalArgumentException();
			}
			node = it->get();
			break;
		}

		case 1:
		case 2: {  // Def
			auto sid = read<StringId>();
			auto nameIndex = (indexMode == 2 ? read<size_t>() : 0U);
			size_t nameCount = 0;
			for (auto& n : node->blockNodes) {
				if (n->type != NodeType::Def || n->sid != sid)
					continue;
				if (++nameCount <= nameIndex)
					continue;
				node = n.get();
				break;
			}
			if (nameCount <= nameIndex) {
				errorAtNode(runtime, ErrorLevel::Error, node, "restore: function \"${0}\" is not found",
						{runtime.primarySTable->ref(sid)});
				throw IllegalArgumentException();
			}
			break;
		}

		case 3:  // blockNodes
		case 4: {  // subNodes
			auto& nodes = (indexMode == 3 ? node->blockNodes : node->subNodes);
			auto index = read<size_t>();
			if (index >= nodes.size() || !nodes.at(index)) {
				errorAtNode(runtime, ErrorLevel::Error, node, "restore: found unrecoverable changes in script structure", {});
				throw IllegalArgumentException();
			}
			node = nodes.at(index).get();
			break;
		}

		default:
			throw IllegalArgumentException();
		}
	}

	if (packageIndex >= 0)
		runtime.restoreNode(*this, static_cast<PackageId>(packageIndex), node);

	return node;
}

const Class* Reader::readClass() {
	assert(entityMapEnabled);
	CHATRA_RESTORE_TYPE_TAG(Class);

	switch (read<int>()) {
	case 0:  return nullptr;
	case 1:  return nodeClassMap.at(read<Node*>());
	case 2:  return classMap.at(read<size_t>());
	default:
		throw IllegalArgumentException();
	}
}

const Method* Reader::readMethod() {
	assert(entityMapEnabled);
	CHATRA_RESTORE_TYPE_TAG(Method);

	switch (read<int>()) {
	case 0:  return nullptr;
	case 1:  return nodeMethodMap.at(read<Node*>());

	case 2: {
		auto* method = readClass()->refConstructors().find(nullptr, StringId::Init, StringId::Invalid, {}, {});
		if (method == nullptr || method->node != nullptr)
			throw IllegalArgumentException();
		return method;
	}

	case 3: {
		auto* cl = readClass();
		auto* clArg = readClass();
		auto* method = cl->refConstructors().find(nullptr, StringId::Init, StringId::Invalid,
				{{StringId::Invalid, clArg}}, {});
		if (method == nullptr || method->node != nullptr)
			throw IllegalArgumentException();
		return method;
	}

	default:
		throw IllegalArgumentException();
	}
}

const MethodTable* Reader::readMethodTable() {
	assert(entityMapEnabled);
	CHATRA_RESTORE_TYPE_TAG(MethodTable);

	switch (read<int>()) {
	case 0:  return nullptr;
	case 1:  return runtime.refMethodTable();
	case 2:  return &readClass()->refMethods();
	case 3:  return &readClass()->refSuperMethods();
	case 4:  return &readClass()->refConstructors();
	case 5: {
		auto packageId = read<PackageId>();
		auto* node = read<Node*>();
		auto it = nodeMethodTableMap.find(node);
		if (it != nodeMethodTableMap.cend())
			return it->second;
		auto* methodTable = runtime.restoreMethodTable(packageId, node);
		add(methodTable);
		return methodTable;
	}
	default:
		throw IllegalArgumentException();
	}
}

Reference Reader::readReference() {
	CHATRA_RESTORE_TYPE_TAG(Reference);
	return Reference(*CHATRA_READ_RAW(ReferenceNode));
}

Thread* Reader::readThread() {
	return readIdType(*this, runtime.threadIds);
}

Thread* Reader::readValidThread() {
	return readValidIdType(*this, runtime.threadIds);
}

Package* Reader::readPackage() {
	return readIdType(*this, runtime.packageIds);
}

Package* Reader::readValidPackage() {
	return readValidIdType(*this, runtime.packageIds);
}

}  // namespace chatra
