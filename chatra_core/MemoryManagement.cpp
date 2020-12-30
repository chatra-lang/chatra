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

#include "MemoryManagement.h"

namespace chatra {

void Object::appendElementsAsArray(Storage& storage, const std::vector<size_t>& primaryIndexes,
		Requester requester) {
	chatra_assert(refsArray.size() <= primaryIndexes.size());

	groups.reserve(primaryIndexes.size());
	nodes.reserve(primaryIndexes.size());
	refsArray.reserve(primaryIndexes.size());

	std::unordered_map<size_t, size_t> indexMap;  // primaryIndex -> index of groups
	for (size_t i = refsArray.size(); i < primaryIndexes.size(); i++) {
		auto primaryIndex = primaryIndexes[i];
		chatra_assert(primaryIndex < primaryIndexes.size());

		ReferenceGroup* group;
		auto it = indexMap.find(primaryIndex);
		if (it == indexMap.end()) {
			indexMap.emplace(primaryIndex, groups.size());
			groups.emplace_back(new ReferenceGroup(storage, requester));
			group = groups.back().get();
		}
		else
			group = groups[it->second].get();

		nodes.emplace_back(new ReferenceNode(*group));
		refsArray.push_back(Reference(*nodes.back()));
	}
}

Object::Object(Storage& storage, TypeId typeId, size_t size, Requester requester) noexcept : typeId(typeId) {
	groups.reserve(size);
	nodes.reserve(size);
	refsArray.reserve(size);
	for (size_t i = 0; i < size; i++) {
		groups.emplace_back(new ReferenceGroup(storage, requester));
		nodes.emplace_back(new ReferenceNode(*groups.back()));
		refsArray.push_back(Reference(*nodes.back()));
	}
}

Object::Object(Storage& storage, TypeId typeId, size_t size, ElementsAreExclusive) noexcept : typeId(typeId) {
	groups.reserve(size);
	nodes.reserve(size);
	refsArray.reserve(size);
	for (size_t i = 0; i < size; i++) {
		groups.emplace_back(new ReferenceGroup(storage, ReferenceGroup::IsExclusive()));
		nodes.emplace_back(new ReferenceNode(*groups.back()));
		refsArray.push_back(Reference(*nodes.back()));
	}
}

Object::Object(Storage& storage, TypeId typeId, const std::vector<std::vector<StringId>>& references,
		Requester requester) noexcept : typeId(typeId) {
	groups.reserve(references.size());
	nodes.reserve(std::accumulate(references.cbegin(), references.cend(), static_cast<size_t>(0),
			[](size_t count, const std::vector<StringId>& keys) { return count + keys.size(); }));
	for (auto& keys : references) {
		groups.emplace_back(new ReferenceGroup(storage, requester));
		auto& group = *groups.back();
		for (auto key : keys) {
			chatra_assert(refsMap.count(key) == 0);
			nodes.emplace_back(new ReferenceNode(group));
			refsMap.emplace(key, Reference(*nodes.back()));
		}
	}
}

Object::Object(Storage& storage, TypeId typeId, const std::vector<size_t>& primaryIndexes,
		Requester requester) noexcept : typeId(typeId) {
	appendElementsAsArray(storage, primaryIndexes, requester);
}

void Object::restoreReferencesForClass() {
	chatra_assert(refsArray.empty());
	refsArray.reserve(nodes.size());
	for (auto& n : nodes)
		refsArray.push_back(Reference(*n));
}

void Object::appendElements(Storage& storage, const std::vector<size_t>& primaryIndexes,
		Requester requester) noexcept {
	appendElementsAsArray(storage, primaryIndexes, requester);
}

Scope::Scope(Storage& storage, ScopeType type) noexcept : storage(storage), type(type) {
	std::lock_guard<SpinLock> lock(storage.lockScopes);
	storage.scopes.emplace(this);
	add(StringId::CapturedScope, InvalidRequester);
}

Scope::Scope(Storage& storage, ScopeType type, Reader& r) noexcept : storage(storage), type(type) {
	(void)r;
	storage.scopes.emplace(this);
}

Scope::~Scope() {
	if (captured) {
		auto& capturedScope = this->ref(StringId::CapturedScope).derefWithoutLock<CapturedScope>();
		auto ref = capturedScope.ref(StringId::CapturedScope);
		read([&](const RefsContainer& refs) {
			ref.allocateWithoutLock<Object>(TypeId::CapturedScopeObject, std::move(groups), std::move(nodes), refs);
		});
		capturedScope.scope.write([](Scope*& scope) { scope = nullptr; });
	}

	{
		std::lock_guard<SpinLock> lock(storage.lockScopes);

		// Checking readCount outside lockScopes would be unsafe
		// because garbage collector may increment readCount asynchronously inside lockScopes
		chatra_assert(getReadCount() == 0);

		storage.scopes.erase(this);
	}
}

void Storage::beginMarkingScope() {
	gcGeneration++;
	marking = true;

	std::lock_guard<SpinLock> lock(lockScopes);
	totalScopeCount = scopes.size();
	gcScopes.reserve(scopes.size());
	gcScopes.insert(gcScopes.end(), scopes.cbegin(), scopes.cend());
}

bool Storage::stepMarkingScope(IConcurrentGcConfiguration& conf) {
	size_t stepCount = conf.stepCountForScope(totalScopeCount);

	std::lock_guard<SpinLock> lock0(lockScopes);
	std::lock_guard<SpinLock> lock1(lockGcTargets);

	for (size_t count = stepCount; count-- > 0 && !gcScopes.empty(); ) {
		Scope* scope = gcScopes.back();
		gcScopes.pop_back();
		if (scopes.count(scope) == 0)
			continue;

		scope->read([&](const RefsContainer& refs) {
			for (auto& e : refs) {
				auto object = e.second.node->object;
				if (object != nullptr && object->gcGeneration != gcGeneration)
					gcTargets.push_back(object);
			}
		});
	}

	return gcScopes.empty();
}

void Storage::beginMarkingObject() {
	std::lock_guard<SpinLock> lock(lockObjects);
	totalObjectCount = objects.size();
}

bool Storage::stepMarkingObject(IConcurrentGcConfiguration& conf) {
	size_t stepCount = conf.stepCountForMarking(totalObjectCount);

	std::lock_guard<SpinLock> lock(lockGcTargets);

	for (size_t count = stepCount; count-- > 0 && !gcTargets.empty(); ) {
		Object* object = gcTargets.front();
		gcTargets.pop_front();

		if (object->gcGeneration == gcGeneration)
			continue;
		object->gcGeneration = gcGeneration;
		for (auto& n : object->nodes) {
			Object* refObject = n->object;
			if (refObject != nullptr)
				gcTargets.push_back(refObject);
		}
	}
	return gcTargets.empty();
}

void Storage::beginSweeping() {
	marking = false;
	position = 0;
}

bool Storage::stepSweeping(IConcurrentGcConfiguration& conf) {
	size_t stepCount = conf.stepCountForSweeping(totalObjectCount);

	std::lock_guard<SpinLock> lock0(lockObjects);
	std::lock_guard<SpinLock> lock1(lockTrash);

	size_t endPosition = std::min(objects.size(), position + stepCount);
	for (; position < endPosition; position++) {
		Object* object = objects[position];
		if (object == nullptr || object->gcGeneration == gcGeneration)
			continue;
		chatra_assert(object->gcGeneration + 1 == gcGeneration);

		// Remove references which refer to unmarked object.
		for (auto& n : object->nodes) {
			Object*& refObject = n->object;
			if (refObject != nullptr && refObject->gcGeneration != gcGeneration)
				refObject = nullptr;
		}

		objects[object->objectIndex] = nullptr;
		recycledObjectIndexes.push_back(object->objectIndex);
		trash.push_back(object);
	}
	return position >= objects.size();
}

void Storage::saveReferences(Writer& w,
		const std::vector<std::unique_ptr<ReferenceGroup>>& groups,
		const std::vector<std::unique_ptr<ReferenceNode>>& nodes) {

	w.out(groups, [&](const std::unique_ptr<ReferenceGroup>& group) {
		w.out(group->lockedBy());
		if (group->lockedBy() != InvalidRequester)
			w.out(group->getLockCount());

		unsigned flags = (group->isConst ? 1U : 0U);
#ifndef CHATRA_NDEBUG
		flags |= (group->isExclusive ? 2U : 0U);
#endif
		w.out(flags);

		w.CHATRA_REGISTER_POINTER(group.get(), ReferenceGroup);
	});

	w.out(nodes, [&](const std::unique_ptr<ReferenceNode>& node) {
		w.CHATRA_OUT_POINTER(&node->group, ReferenceGroup);
		w.out(node->type);
		switch (node->type) {
		case ReferenceValueType::Bool:  w.out(node->vBool);  break;
		case ReferenceValueType::Int:  w.out(node->vInt);  break;
		case ReferenceValueType::Float:  w.out(node->vFloat);  break;

		case ReferenceValueType::Object:
			if (node->object == nullptr)
				w.out(SIZE_MAX);
			else
				w.out(node->object->objectIndex);
			break;
		}

		w.CHATRA_REGISTER_POINTER(node.get(), ReferenceNode);
	});
}

std::unordered_map<ReferenceNode*, size_t> Storage::createRefIndexes(
		const std::vector<std::unique_ptr<ReferenceNode>>& nodes) {

	std::unordered_map<ReferenceNode*, size_t> refIndexes;
	for (size_t i = 0; i < nodes.size(); i++)
		refIndexes.emplace(nodes[i].get(), i);
	return refIndexes;
}

void Storage::saveRefsMap(Writer& w, const std::unordered_map<ReferenceNode*, size_t>& refIndexes,
		const std::unordered_map<StringId, Reference>& refsMap) {

	w.out(refsMap, [&](const std::unordered_map<StringId, Reference>::value_type& e) {
		w.out(e.first);
		w.out(refIndexes.at(e.second.node));
	});
}

void Storage::saveRefsArray(Writer& w, const std::unordered_map<ReferenceNode*, size_t>& refIndexes,
		const std::vector<Reference>& refsArray) {

	w.out(refsArray, [&](const Reference& ref) {
		w.out(refIndexes.at(ref.node));
	});
}

void Storage::restoreReferences(Storage& storage, Reader& r,
		std::vector<std::unique_ptr<ReferenceGroup>>& groups,
		std::vector<std::unique_ptr<ReferenceNode>>& nodes) {

	r.inList([&]() {
		auto lockedBy = r.read<Requester>();
		auto lockedCount = (lockedBy != InvalidRequester ? r.read<int>() : 0);
		auto flags = r.read<unsigned>();
		groups.emplace_back(new ReferenceGroup(lockedBy, lockedCount, storage,
				(flags & 1U) != 0, (flags & 2U) != 0));
		r.CHATRA_REGISTER_POINTER(groups.back().get(), ReferenceGroup);
	});

	r.inList([&]() {
		auto* group = r.CHATRA_READ_RAW(ReferenceGroup);
		nodes.emplace_back(new ReferenceNode(*group));
		auto* node = nodes.back().get();

		auto type = r.read<ReferenceValueType>();
		switch (type) {
		case ReferenceValueType::Bool:
			node->setBool(r.read<bool>());
			break;
		case ReferenceValueType::Int:
			node->setInt(r.read<int64_t>());
			break;
		case ReferenceValueType::Float:
			node->setFloat(r.read<double>());
			break;
		case ReferenceValueType::Object:
			node->setObjectIndex(r.read<size_t>());
			break;
		}

		r.CHATRA_REGISTER_POINTER(node, ReferenceNode);
	});
}

void Storage::restoreRefsMap(Reader& r,
		const std::vector<std::unique_ptr<ReferenceNode>>& nodes,
		std::unordered_map<StringId, Reference>& refsMap) {

	r.inList([&]() {
		auto key = r.read<StringId>();
		auto refIndex = r.read<size_t>();
		refsMap.emplace(key, Reference(*nodes.at(refIndex).get()));
	});
}

void Storage::restoreRefsArray(Reader& r,
		const std::vector<std::unique_ptr<ReferenceNode>>& nodes,
		std::vector<Reference>& refsArray) {

	r.inList([&]() {
		refsArray.emplace_back(Reference(*nodes.at(r.read<size_t>()).get()));
	});
}

void Storage::restoreObjectRefs(std::vector<std::unique_ptr<ReferenceNode>>& nodes) {
	for (auto& n : nodes) {
		if (n->type == referenceValueType_ObjectIndex) {
			n->type = ReferenceValueType::Object;
			n->object = (n->vObjectIndex == SIZE_MAX ? nullptr : objects.at(n->vObjectIndex));
		}
	}
}

void Storage::deploy() {
	chatra_assert(recycledObjectIndexes.empty());
	armed = true;
	systemObjectIndex = objects.size();
}

std::unique_ptr<Scope> Storage::add(ScopeType type) {
	auto ret = std::unique_ptr<Scope>(new Scope(*this, type));
	if (!armed)
		systemScopes.emplace_back(ret.get());
	return ret;
}

bool Storage::tidy(IConcurrentGcConfiguration& conf) {
	switch (gcState) {
	case GcState::Idle:
		gcState = GcState::MarkingScope;
		beginMarkingScope();
		CHATRA_FALLTHROUGH;

	case GcState::MarkingScope:
		if (!stepMarkingScope(conf))
			return false;
		gcState = GcState::MarkingObject;
		beginMarkingObject();
		CHATRA_FALLTHROUGH;

	case GcState::MarkingObject:
		if (!stepMarkingObject(conf))
			return false;
		gcState = GcState::Sweeping;
		beginSweeping();
		CHATRA_FALLTHROUGH;

	case GcState::Sweeping:
		if (!stepSweeping(conf))
			return false;
		gcState = GcState::Idle;
		return true;

	default:
		throw InternalError();
	}
}

void Storage::collect(Requester requester) {
	chatra_assert(gcState == GcState::Idle);
	std::vector<Object*> trashCopy;
	{
		std::lock_guard<SpinLock> lock(lockTrash);
		std::swap(trash, trashCopy);
	}

	for (auto* object : trashCopy) {
		if (!object->hasOnDestroy()) {
			delete object;
			continue;
		}

		ReferenceGroup group(*this, InvalidRequester);
		group.lock(requester);

		ReferenceNode node(group);
		node.type = ReferenceValueType::Object;
		node.object = object;

		Reference ref(node);
		object->onDestroy(tag, requester, ref);
		chatra_assert(object->lockedBy() == InvalidRequester);

		delete object;
	}
}

std::shared_ptr<Storage> Storage::newInstance(void* tag) {
	struct StorageBridge final : Storage {
		explicit StorageBridge(void* tag) noexcept : Storage(tag) {}
	};
	return std::make_shared<StorageBridge>(tag);
}

bool Storage::audit() const {
#ifndef CHATRA_NDEBUG
	std::lock_guard<SpinLock> lock0(lockScopes);
	std::lock_guard<SpinLock> lock1(lockObjects);

	std::printf("audit: %u scopes, %u objects (%u valid, %u recycled)\n",
			static_cast<unsigned>(scopes.size()),
			static_cast<unsigned>(objects.size()),
			static_cast<unsigned>(objects.size() - recycledObjectIndexes.size()),
			static_cast<unsigned>(recycledObjectIndexes.size()));
	for (auto* scope : scopes) {
		size_t refCount = 0;
		scope->read([&](const RefsContainer& refs) {
			refCount = refs.size();
		});
		chatra_assert(refCount == scope->nodes.size());
		for (auto& n : scope->nodes) {
			if (n->type == ReferenceValueType::Object)
				chatra_assert(n->object == nullptr || n->object->objectIndex < 10000000);
			if (n->type == referenceValueType_ObjectIndex)
				chatra_assert(n->vObjectIndex == SIZE_MAX || n->vObjectIndex < 10000000);
		}
	}

	size_t releasedIndexes = 0;
	for (size_t i = 0; i < objects.size(); i++) {
		auto* object = objects[i];
		if (object == nullptr) {
			releasedIndexes++;
			continue;
		}
		chatra_assert(object->objectIndex == i);
		for (auto& n : object->nodes) {
			if (n->type == ReferenceValueType::Object)
				chatra_assert(n->object == nullptr || n->object->objectIndex < 10000000);
			if (n->type == referenceValueType_ObjectIndex)
				chatra_assert(n->vObjectIndex == SIZE_MAX || n->vObjectIndex < 10000000);
		}
	}
	chatra_assert(recycledObjectIndexes.size() == releasedIndexes);
#endif
	return true;
}


#ifndef CHATRA_NDEBUG
static const char* toString(ScopeType v) {
	switch (v) {
	case ScopeType::Thread:  return "Thread";
	case ScopeType::Global:  return "Global";
	case ScopeType::Package:  return "Package";
	case ScopeType::ScriptRoot:  return "ScriptRoot";
	case ScopeType::Class:  return "Class";
	case ScopeType::Method:  return "Method";
	case ScopeType::InnerMethod:  return "InnerMethod";
	case ScopeType::Block:  return "Block";
	default:  return "(invalid)";
	}
}

void Lockable::dump() const {
	auto requester = lockRequester.load();
	std::printf("lock=%s, lockCount=%d",
			requester == InvalidRequester ? "none" : std::to_string(static_cast<unsigned>(requester)).c_str(),
			lockCount);
}

void ReferenceNode::dump() const {
	switch (type) {
	case ReferenceValueType::Bool:
		std::printf(" = %s (Bool)", vBool ? "true" : "false");
		break;
	case ReferenceValueType::Int:
		std::printf(" = %lld (Int)", static_cast<long long int>(vInt));
		break;
	case ReferenceValueType::Float:
		std::printf(" = %g (Float)", vFloat);
		break;
	case ReferenceValueType::Object:
		if (object != nullptr)
			std::printf(" -> #%u@%p", static_cast<unsigned>(object->objectIndex), static_cast<void*>(object));
		break;
	}
}

void Object::dump(const std::shared_ptr<StringTable>& sTable) const {
	std::printf("#%u@%p: ", static_cast<unsigned>(objectIndex), static_cast<const void*>(this));
	Lockable::dump();
	std::printf(", gen=%u, %u refsMap, %u refsArray\n", gcGeneration,
			static_cast<unsigned>(refsMap.size()), static_cast<unsigned>(refsArray.size()));

	if (!refsMap.empty()) {
		std::unordered_map<ReferenceGroup*, size_t> groupToIndex;
		size_t groupCount = 0;
		for (auto& group : groups)
			groupToIndex.emplace(group.get(), groupCount++);
		for (auto& e : refsMap) {
			std::printf("  \"%s\" ", sTable->ref(e.first).c_str());
			auto& node = e.second.node;
			auto& group = node->group;
			std::printf("group %u, ", static_cast<unsigned>(groupToIndex[&group]));
			group.Lockable::dump();
			node->dump();
			std::printf("\n");
		}
	}

	for (size_t i = 0; i < refsArray.size(); i++) {
		std::printf("  [%u] ", static_cast<unsigned>(i));
		auto& node = refsArray[i].node;
		node->group.Lockable::dump();
		node->dump();
		std::printf("\n");
	}
}

void Scope::dump(const std::shared_ptr<StringTable>& sTable) const {
	read([&](const RefsContainer& refs) {
		std::printf("%s, %u refs\n", toString(type), static_cast<unsigned>(refs.size()));
		for (auto& e : refs) {
			std::printf("  \"%s\" ", (sTable->has(e.first) ? sTable->ref(e.first)
					: std::string("#tmp") + std::to_string(static_cast<int>(StringId::Invalid) - static_cast<int>(e.first))).c_str());
			auto& node = e.second.node;
			node->group.Lockable::dump();
			node->dump();
			std::printf("\n");
		}
	});
}

void CapturedScope::dump(const std::shared_ptr<StringTable>& sTable) const {
	std::printf("CapturedScope: %s ",
			scope.read<bool>([&](const Scope* scope) { return scope == nullptr; }) ? "captured" : "through");
	Object::dump(sTable);
}

size_t Storage::dumpReferencesSub(const std::shared_ptr<StringTable>& sTable, const Object& object,
		const std::string& suffix, size_t count) const {

	for (auto* scope : scopes) {
		scope->read([&](const RefsContainer& refs) {
			for (auto& e : refs) {
				auto& ref = e.second;
				if (ref.valueTypeWithoutLock() != ReferenceValueType::Object || ref.isNullWithoutLock())
					continue;
				if (&ref.derefWithoutLock() != &object)
					continue;
				std::printf("scope{%p: %s}.%s -> %s\n", static_cast<void*>(scope), toString(scope->type),
						(sTable->has(e.first) ? sTable->ref(e.first)
								: std::string("#tmp") + std::to_string(static_cast<int>(StringId::Invalid) - static_cast<int>(e.first))).c_str(),
						suffix.data());
				count++;
			}
		});
	}

	for (auto* o : objects) {
		if (o == nullptr)
			continue;
		for (auto& e : o->refsMap) {
			auto& ref = e.second;
			if (ref.valueTypeWithoutLock() != ReferenceValueType::Object || ref.isNullWithoutLock())
				continue;
			if (&ref.derefWithoutLock() != &object)
				continue;
			count = dumpReferencesSub(sTable, *o,
					"object{" + std::to_string(o->objectIndex) + "}." + sTable->ref(e.first) + " -> " + suffix, count);
		}

		for (size_t i = 0; i < o->refsArray.size(); i++) {
			auto& ref = o->refsArray[i];
			if (ref.valueTypeWithoutLock() != ReferenceValueType::Object || ref.isNullWithoutLock())
				continue;
			if (&ref.derefWithoutLock() != &object)
				continue;
			count = dumpReferencesSub(sTable, *o,
					"object{" + std::to_string(o->objectIndex) + "}[" + std::to_string(i) + "] -> " + suffix, count);
		}
	}

	if (count > 30) {
		std::printf("Storage::dumpReferences(): too many references\n");
		throw IllegalArgumentException();
	}
	return count;
}

void Storage::dumpReferences(const std::shared_ptr<StringTable>& sTable, const Object& object) const {
	std::lock_guard<SpinLock> lock0(lockScopes);
	std::lock_guard<SpinLock> lock1(lockObjects);

	std::printf("Storage::dumpReferences(): object{%u}\n", static_cast<unsigned>(object.objectIndex));
	if (object.objectIndex >= objects.size()) {
		std::fflush(stdout);
		std::fprintf(stderr, "broken object");
		std::fflush(stderr);
		return;
	}
	try {
		dumpReferencesSub(sTable, object, "target", 0);
	}
	catch (const IllegalArgumentException&) {
		// do nothing
	}
	std::fflush(stdout);
}

size_t Storage::objectCount() const {
	std::lock_guard<SpinLock> lock(lockObjects);
	return objects.size() - recycledObjectIndexes.size();
}

Object* Storage::refDirect(size_t objectIndex) const {
	return objects[objectIndex];
}

void Storage::dump(const std::shared_ptr<StringTable>& sTable) const {
	std::lock_guard<SpinLock> lock0(lockScopes);
	std::lock_guard<SpinLock> lock1(lockObjects);
	std::lock_guard<SpinLock> lock2(lockTrash);

	std::printf("%u scopes, %u objects, %u allocated, %u free, %u trash, gen=%u\n",
			static_cast<unsigned>(scopes.size()),
			static_cast<unsigned>(objects.size() - recycledObjectIndexes.size()),
			static_cast<unsigned>(objects.size()), static_cast<unsigned>(recycledObjectIndexes.size()),
			static_cast<unsigned>(trash.size()),
			gcGeneration.load());
	for (auto scope : scopes) {
		std::printf("S ");
		scope->dump(sTable);
	}
	for (auto object : objects) {
		if (object != nullptr) {
			std::printf("A ");
			object->dump(sTable);
		}
	}
	for (auto object : trash) {
		std::printf("T ");
		object->dump(sTable);
	}
}
#endif // !CHATRA_NDEBUG

}  // namespace chatra
