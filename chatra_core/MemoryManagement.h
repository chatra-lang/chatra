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

#ifndef CHATRA_MEMORY_MANAGEMENT_H
#define CHATRA_MEMORY_MANAGEMENT_H

#include "Internal.h"
#include "StringTable.h"
#include "Serialize.h"

namespace chatra {

enum class Requester : size_t {};
constexpr Requester InvalidRequester = enum_max<Requester>::value;

enum class TypeId : int {
	CapturedScope = -1,
	CapturedScopeObject = -2,
	Dummy = 0
};

constexpr TypeId toTypeId(int id) noexcept {
	return static_cast<TypeId>(id);
}


class Reference;
class Object;
class Scope;
class Storage;
class TemporaryObject;  // Runtime.h


class Lockable {
private:
	std::atomic<Requester> lockRequester;
	int lockCount;

protected:
	Lockable(Requester requester, int lockCount) noexcept
			: lockRequester(requester), lockCount(lockCount) {}

public:
	explicit Lockable(Requester requester = InvalidRequester) noexcept
			: lockRequester(requester), lockCount(requester == InvalidRequester ? 0 : 1) {}

	Requester lockedBy() const {
		return lockRequester;
	}

	int getLockCount() const {
		return lockCount;
	}

	bool lock(Requester requester) {
		Requester expected = InvalidRequester;
		if (lockRequester.compare_exchange_weak(expected, requester)) {
			lockCount = 1;
			return true;
		}
		else {
			if (expected == requester)
				lockCount++;
			return false;
		}
	}

	bool lockOnce(Requester requester) {
		Requester expected = InvalidRequester;
		if (lockRequester.compare_exchange_weak(expected, requester)) {
			lockCount = 1;
			return true;
		}
		else {
			chatra_assert(expected != requester || lockCount == 1);
			return expected == requester;
		}
	}

	/// [Requires lock]
	void unlock() {
		chatra_assert(lockedBy() != InvalidRequester);
		if (--lockCount > 0)
			return;
		chatra_assert(lockCount == 0);
		lockRequester = InvalidRequester;
	}

#ifndef CHATRA_NDEBUG
	void dump() const;
#endif // !CHATRA_NDEBUG
};


class ReferenceGroup final : public Lockable {
	friend class Object;
	friend class Reference;
	friend class Scope;
	friend class Storage;
	friend class ReferenceNode;

private:
	Storage& storage;
	bool isConst;
#ifndef CHATRA_NDEBUG
	bool isExclusive = false;
#endif

private:
	ReferenceGroup(Storage& storage, Requester requester) noexcept : Lockable(requester), storage(storage), isConst(false) {}

	struct IsConst {};
	ReferenceGroup(Storage& storage, IsConst isConst) noexcept : storage(storage), isConst(true) {
		(void)isConst;
	}

	struct IsExclusive {};
	ReferenceGroup(Storage& storage, IsExclusive isExclusive) noexcept : storage(storage), isConst(false) {
		(void)isExclusive;
#ifndef CHATRA_NDEBUG
		this->isExclusive = true;
#endif
	}

	// Special constructor for restore
	ReferenceGroup(Requester requester, int lockCount, Storage& storage, bool isConst, bool isExclusive) noexcept
			: Lockable(requester, lockCount), storage(storage), isConst(isConst) {
		(void)isExclusive;
#ifndef CHATRA_NDEBUG
		this->isExclusive = isExclusive;
#endif
	}
};


enum class ReferenceValueType {
	Bool = 0, Int = 1, Float = 2, Object = 3
};

constexpr ReferenceValueType referenceValueType_ObjectIndex = static_cast<ReferenceValueType>(4);


class ReferenceNode final {
	friend class Object;
	friend class Reference;
	friend class Scope;
	friend class Storage;

private:
	ReferenceGroup& group;
	ReferenceValueType type = ReferenceValueType::Object;

	// This should not be part of union because the garbage collector expects this can be read as atomic.
	Object* object = nullptr;

	union {
		bool vBool;
		int64_t vInt;
		double vFloat;
		size_t vObjectIndex;  // This is only used while restoring Storage
	};

private:
	explicit ReferenceNode(ReferenceGroup& group) noexcept : group(group), vBool(false) {}

	void setBool(bool newValue) {
		type = ReferenceValueType::Bool;
		object = nullptr;
		vBool = newValue;
	}

	void setInt(int64_t newValue) {
		type = ReferenceValueType::Int;
		object = nullptr;
		vInt = newValue;
	}

	void setFloat(double newValue) {
		type = ReferenceValueType::Float;
		object = nullptr;
		vFloat = newValue;
	}

	void setObject(Object* newObject);  // Can be nullptr

	void setObjectIndex(size_t newValue) {
		type = referenceValueType_ObjectIndex;
		vObjectIndex = newValue;
	}

#ifndef CHATRA_NDEBUG
	void dump() const;
#endif // !CHATRA_NDEBUG
};


/// This class is named "Reference", but actually this represents "reference to reference".
/// example:
///   class A / var b -> Reference object points to "b" itself, not the value of b
class Reference final {
	friend class Object;
	friend class Scope;
	friend class CapturedScope;
	friend class Storage;
	friend class TemporaryObject;
	friend class Reader;

private:
	ReferenceNode* node;

public:
	// This strange method is used for implementing serializing, std::hash() and std::equal_to().
	// Also used for some debugging features.
	ReferenceNode* internal_nodePtr() const {
		return node;
	}

private:
	explicit Reference(ReferenceNode& node) noexcept : node(&node) {}

public:
	struct SetNull {};
	explicit Reference(SetNull setNull) noexcept : node(nullptr) { (void)setNull; }

	Reference(const Reference&) noexcept = default;
	Reference(Reference&&) noexcept = default;
	~Reference() = default;

	Reference& operator=(const Reference& r) noexcept {
		this->~Reference();
		new(this) Reference(r);
		return *this;
	}

	Reference& operator=(Reference&& r) noexcept {
		this->~Reference();
		new(this) Reference(r);
		return *this;
	}

	bool isConst() const {
		chatra_assert(node != nullptr);
		return node->group.isConst;
	}

	bool requiresLock() const {
		chatra_assert(node != nullptr);
#ifdef CHATRA_NDEBUG
		return !node->group.isConst;
#else
		return !node->group.isConst && !node->group.isExclusive;
#endif
	}

	Requester lockedBy() const {
		chatra_assert(node != nullptr);
		return node->group.lockedBy();
	}

	bool lock(Requester requester) const {
		chatra_assert(node != nullptr);
		return node->group.lock(requester);
	}

	bool lockOnce(Requester requester) const {
		chatra_assert(node != nullptr);
		return node->group.lockOnce(requester);
	}

	void unlock() const {
		chatra_assert(node != nullptr);
		node->group.unlock();
	}

	bool isNullWithoutLock() const {
		chatra_assert(node != nullptr);
		return node->type == ReferenceValueType::Object && node->object == nullptr;
	}

	/// [Requires lock]
	bool isNull() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return isNullWithoutLock();
	}

	ReferenceValueType valueTypeWithoutLock() const {
		chatra_assert(node != nullptr);
		return node->type;
	}

	/// [Requires lock]
	ReferenceValueType valueType() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return valueTypeWithoutLock();
	}

	template <class Type = Object>
	Type& derefWithoutLock() const {
		chatra_assert(node != nullptr && node->object != nullptr);
		return *static_cast<Type *>(node->object);
	}

	/// [Requires lock]
	template <class Type = Object>
	Type& deref() const {
		static_assert(std::is_base_of<Object, Type>::value, "Type should be derived class of Object");
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return derefWithoutLock<Type>();
	}

	bool getBoolWithoutLock() const {
		chatra_assert(node->type == ReferenceValueType::Bool);
		return node->vBool;
	}

	int64_t getIntWithoutLock() const {
		chatra_assert(node->type == ReferenceValueType::Int);
		return node->vInt;
	}

	double getFloatWithoutLock() const {
		chatra_assert(node->type == ReferenceValueType::Float);
		return node->vFloat;
	}

	/// [Requires lock]
	bool getBool() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return getBoolWithoutLock();
	}

	/// [Requires lock]
	int64_t getInt() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return getIntWithoutLock();
	}

	/// [Requires lock]
	double getFloat() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return getFloatWithoutLock();
	}

	void setWithoutBothLock(const Reference& ref) const {
		chatra_assert(node != nullptr && ref.node != nullptr);
		switch (ref.node->type) {
		case ReferenceValueType::Bool:  node->setBool(ref.node->vBool); break;
		case ReferenceValueType::Int:  node->setInt(ref.node->vInt); break;
		case ReferenceValueType::Float:  node->setFloat(ref.node->vFloat); break;
		case ReferenceValueType::Object:  node->setObject(ref.node->object); break;
		}
	}

	/// [Requires lock to ref]
	void setWithoutLock(const Reference& ref) const {
		chatra_assert(!ref.requiresLock() || ref.node->group.lockedBy() != InvalidRequester);
		setWithoutBothLock(ref);
	}

	/// [Requires lock (both *this and ref)]
	void set(const Reference& ref) const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		setWithoutLock(ref);
	}

	/// [Requires lock]
	void setNull() const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		node->setObject(nullptr);
	}

	/// [Requires lock]
	void setBool(bool newValue) const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		node->setBool(newValue);
	}

	/// [Requires lock]
	void setInt(int64_t newValue) const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		node->setInt(newValue);
	}

	/// [Requires lock]
	void setFloat(double newValue) const {
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		node->setFloat(newValue);
	}

	template <class Type, class... Args>
	Type& allocateWithoutLock(Args&&... args) const;

	/// [Requires lock]
	template <class Type, class... Args>
	Type& allocate(Args&&... args) const {
		static_assert(std::is_base_of<Object, Type>::value, "Type should be derived class of Object");
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return allocateWithoutLock<Type>(std::forward<Args>(args)...);
	}

	template <class Type>
	Type& setWithoutLock(Type* value) const;

	/// [Requires lock]
	template <class Type>
	Type& set(Type* value) const {
		static_assert(std::is_base_of<Object, Type>::value, "Type should be derived class of Object");
		chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);
		return setWithoutLock<Type>(value);
	}

	/// [Requires lock]
	void capture(Scope& scope) const;
};


class Object : public Lockable {
	friend class ReferenceNode;
	friend class Reference;
	friend class Scope;
	friend class Storage;

public:
	virtual ~Object() = default;

private:
	TypeId typeId;
	size_t objectIndex = SIZE_MAX;
	unsigned gcGeneration = 0;

	std::vector<std::unique_ptr<ReferenceGroup>> groups;
	std::vector<std::unique_ptr<ReferenceNode>> nodes;
	std::unordered_map<StringId, Reference> refsMap;
	std::vector<Reference> refsArray;

private:
	explicit Object(Storage& storage, TypeId typeId,
			std::vector<std::unique_ptr<ReferenceGroup>> groups,
			std::vector<std::unique_ptr<ReferenceNode>> nodes,
			std::unordered_map<StringId, Reference> refs) noexcept
			: typeId(typeId), groups(std::move(groups)), nodes(std::move(nodes)), refsMap(std::move(refs)) { (void)storage; }

protected:
	// Constructor for array-like objects
	Object(Storage& storage, TypeId typeId, size_t size, Requester requester = InvalidRequester) noexcept;

	// Constructor for container's body
	struct ElementsAreExclusive {};
	Object(Storage& storage, TypeId typeId, size_t size, ElementsAreExclusive elementsAreExclusive) noexcept;

	// Constructor for compound types
	Object(Storage& storage, TypeId typeId, const std::vector<std::vector<StringId>>& references,
			Requester requester = InvalidRequester) noexcept;

	// Constructor for classes
	Object(Storage& storage, TypeId typeId, const std::vector<size_t>& primaryIndexes,
			Requester requester = InvalidRequester) noexcept;

	// Constructor used for restoring
	explicit Object(TypeId typeId) : typeId(typeId) {}

	void restoreReferencesForClass();

	// Ready to call deinit()
	virtual void onDestroy(void* tag, Requester requester, Reference& ref) { (void)tag; (void)requester; (void)ref; }
	virtual bool hasOnDestroy() const { return false; }

public:
	TypeId getTypeId() const {
		return typeId;
	}

	size_t getObjectIndex() const {
		return objectIndex;
	}

	std::vector<StringId> keys() const;

	bool has(StringId key) const {
		return refsMap.count(key) != 0;
	}

	Reference ref(StringId key) const;

	size_t size() const {
		return refsArray.size();
	}

	Reference ref(size_t position) const;

#ifndef CHATRA_NDEBUG
	virtual void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !CHATRA_NDEBUG
};


enum class ScopeType {
	Thread,
	Global,
	Package,
	ScriptRoot,
	Class,
	Method,
	InnerMethod,
	Block,
};

using RefsContainer = std::unordered_map<StringId, Reference>;

class Scope final : private AsyncRead<RefsContainer> {
	friend class Reference;
	friend class Storage;

private:
	Storage& storage;
	ScopeType type;

	std::vector<std::unique_ptr<ReferenceGroup>> groups;
	std::vector<std::unique_ptr<ReferenceNode>> nodes;
	std::underlying_type<StringId>::type nextId = static_cast<std::underlying_type<StringId>::type>(StringId::Invalid) - 1;

	SpinLock lockAllocateCapture;
	std::atomic<bool> captured = {false};

private:
	Scope(Storage& storage, ScopeType type) noexcept;
	Scope(Storage& storage, ScopeType type, Reader& r) noexcept;

public:
	~Scope();

	ScopeType getScopeType() const {
		return type;
	}

	/// Allocate reference; Note that only one context can call this method at the same time.
	Reference add(StringId key, Requester requester = InvalidRequester);

	/// Allocate const reference
	Reference addConst(StringId key);

	/// Allocate exclusive reference
	Reference addExclusive(StringId key);

	/// Allocate temporary reference on this Scope
	Reference add(Requester requester = InvalidRequester);

	/// Allocate exclusive reference
	Reference addExclusive();

	template <class Type, class... Args>
	Type& add(Requester requester, Args&&... args) {
		static_assert(std::is_base_of<Object, Type>::value, "Type should be derived class of Object");
		auto& ret = add(requester).allocate<Type>(std::forward<Args>(args)...);
		return ret;
	}

	bool has(StringId key) const {
		return read<bool>([&](const RefsContainer& refs) { return refs.count(key) != 0; });
	}

	Reference ref(StringId key) const {
		return read<Reference>([&](const RefsContainer& refs) { return refs.at(key); });
	}

	std::vector<Reference> getAllRefs() const;
	std::unordered_map<StringId, Reference> getAllRefsWithKey() const;
	std::unordered_map<const Object*, StringId> getObjectMap() const;

#ifndef CHATRA_NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !CHATRA_NDEBUG
};


class CapturedScope final : private Object {
	friend class Reference;
	friend class Scope;
	friend class Storage;

private:
	AsyncRead<Scope*> scope;

private:
	CapturedScope(Storage& storage, Scope* scopePtr) noexcept
			: Object(storage, TypeId::CapturedScope, {{StringId::CapturedScope}}) {
		scope.write([&](Scope*& scope) { scope = scopePtr; });
	}

	explicit CapturedScope(Scope* scopePtr) noexcept : Object(TypeId::CapturedScope) {
		scope.write([&](Scope*& scope) { scope = scopePtr; });
	}

public:
	bool hasCaptured(StringId key) const;

	Reference refCaptured(StringId key) const;

#ifndef CHATRA_NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !CHATRA_NDEBUG
};


struct IConcurrentGcConfiguration {
	virtual ~IConcurrentGcConfiguration() = default;
	virtual size_t stepCountForScope(size_t totalScopeCount) = 0;
	virtual size_t stepCountForMarking(size_t totalObjectCount) = 0;
	virtual size_t stepCountForSweeping(size_t totalObjectCount) = 0;
};


enum class GcState {
	Idle, MarkingScope, MarkingObject, Sweeping
};


class Storage {
	friend class ReferenceNode;
	friend class Reference;
	friend class Scope;

private:
	void* tag;

	bool armed = false;
	std::vector<Scope*> systemScopes;
	size_t systemObjectIndex = 0;

	mutable SpinLock lockScopes;
	std::unordered_set<Scope*> scopes;

	mutable SpinLock lockObjects;
	std::vector<Object*> objects;
	std::deque<size_t> recycledObjectIndexes;

	mutable SpinLock lockTrash;
	std::vector<Object*> trash;

	std::atomic<unsigned> gcGeneration = {0};

	GcState gcState = GcState::Idle;
	std::atomic<bool> marking = {false};

	std::vector<Scope*> gcScopes;

	SpinLock lockGcTargets;
	std::deque<Object*> gcTargets;

	size_t totalScopeCount = 0;
	size_t totalObjectCount = 0;
	size_t position = 0;

protected:
	explicit Storage(void* tag) noexcept : tag(tag) {}

private:
	void registerObject(Object* object);

	void beginMarkingScope();
	bool stepMarkingScope(IConcurrentGcConfiguration& conf);

	void beginMarkingObject();
	bool stepMarkingObject(IConcurrentGcConfiguration& conf);

	void beginSweeping();
	bool stepSweeping(IConcurrentGcConfiguration& conf);

	static void saveReferences(Writer& w,
			const std::vector<std::unique_ptr<ReferenceGroup>>& groups,
			const std::vector<std::unique_ptr<ReferenceNode>>& nodes);
	static std::unordered_map<ReferenceNode*, size_t> createRefIndexes(
			const std::vector<std::unique_ptr<ReferenceNode>>& nodes);
	static void saveRefsMap(Writer& w, const std::unordered_map<ReferenceNode*, size_t>& refIndexes,
			const std::unordered_map<StringId, Reference>& refsMap);
	static void saveRefsArray(Writer& w, const std::unordered_map<ReferenceNode*, size_t>& refIndexes,
			const std::vector<Reference>& refsArray);

	static void restoreReferences(Storage& storage, Reader& r,
			std::vector<std::unique_ptr<ReferenceGroup>>& groups,
			std::vector<std::unique_ptr<ReferenceNode>>& nodes);
	static void restoreRefsMap(Reader& r,
			const std::vector<std::unique_ptr<ReferenceNode>>& nodes,
			std::unordered_map<StringId, Reference>& refsMap);
	static void restoreRefsArray(Reader& r,
			const std::vector<std::unique_ptr<ReferenceNode>>& nodes,
			std::vector<Reference>& refsArray);
	void restoreObjectRefs(std::vector<std::unique_ptr<ReferenceNode>>& nodes);

public:
	void deploy();

	std::unique_ptr<Scope> add(ScopeType type);

	/// Step concurrent GC; returns whether GC was completed.
	/// Once this was called and until this returns true, collect() cannot be called.
	bool tidy(IConcurrentGcConfiguration& conf);

	void collect(Requester requester);

	template<typename WriteObject, typename WriteObjectReferences>
	void save(Writer& w, WriteObject writeObject, WriteObjectReferences writeObjectReferences) const;

	template <typename ReadObject, typename ReadObjectReferences>
	void restore(Reader& r, ReadObject readObject, ReadObjectReferences readObjectReferences);

	static std::shared_ptr<Storage> newInstance(void* tag = nullptr);

	/// for debug
	bool audit() const;

#ifndef CHATRA_NDEBUG
	size_t dumpReferencesSub(const std::shared_ptr<StringTable>& sTable, const Object& object,
			const std::string& suffix, size_t count) const;
	void dumpReferences(const std::shared_ptr<StringTable>& sTable, const Object& object) const;
	size_t objectCount() const;
	Object* refDirect(size_t objectIndex) const;
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !CHATRA_NDEBUG
};


inline void ReferenceNode::setObject(Object* newObject) {
	type = ReferenceValueType::Object;
	object = newObject;
	if (newObject != nullptr && group.storage.marking && newObject->gcGeneration != group.storage.gcGeneration) {
		std::lock_guard<SpinLock> lock(group.storage.lockGcTargets);
		group.storage.gcTargets.push_back(newObject);
	}
}

template <class Type, class... Args>
Type& Reference::allocateWithoutLock(Args&&... args) const {
	chatra_assert(node != nullptr);
	Type* object = new Type(node->group.storage, std::forward<Args>(args)...);
	node->setObject(object);
	node->group.storage.registerObject(node->object);
	return *object;
}

template <class Type>
Type& Reference::setWithoutLock(Type* value) const {
	chatra_assert(value->objectIndex != SIZE_MAX);
	node->setObject(value);
	return *value;
}

inline void Reference::capture(Scope& scope) const {
	chatra_assert(!requiresLock() || node->group.lockedBy() != InvalidRequester);

	auto ref = scope.ref(StringId::CapturedScope);
	CapturedScope* captured;
	{
		std::lock_guard<SpinLock> lock(scope.lockAllocateCapture);
		captured = static_cast<CapturedScope*>(ref.node->object);
		if (captured == nullptr)
			captured = &ref.allocateWithoutLock<CapturedScope>(&scope);
	}

	node->setObject(captured);
	scope.captured = true;
}

inline std::vector<StringId> Object::keys() const {
	std::vector<StringId> ret;
	ret.reserve(refsMap.size());
	for (auto& e : refsMap)
		ret.emplace_back(e.first);
	return ret;
}

inline Reference Object::ref(StringId key) const {
	return refsMap.at(key);
}

inline Reference Object::ref(size_t position) const {
	return refsArray[position];
}

inline Reference Scope::add(chatra::StringId key, Requester requester) {
#ifndef CHATRA_NDEBUG
	bool alreadyAdded = false;
	read([&](const RefsContainer& refs) {
		alreadyAdded = (refs.count(key) != 0);
	});
	chatra_assert(!alreadyAdded);
#endif // !CHATRA_NDEBUG

	groups.emplace_back(new ReferenceGroup(storage, requester));
	nodes.emplace_back(new ReferenceNode(*groups.back()));
	auto ref = Reference(*nodes.back());
	write([&](RefsContainer& refs) { refs.emplace(key, ref); });
	return ref;
}

inline Reference Scope::addConst(chatra::StringId key) {
	groups.emplace_back(new ReferenceGroup(storage, ReferenceGroup::IsConst()));
	nodes.emplace_back(new ReferenceNode(*groups.back()));
	auto ref = Reference(*nodes.back());
	write([&](RefsContainer& refs) { refs.emplace(key, ref); });
	return ref;
}

inline Reference Scope::addExclusive(chatra::StringId key) {
	groups.emplace_back(new ReferenceGroup(storage, ReferenceGroup::IsExclusive()));
	nodes.emplace_back(new ReferenceNode(*groups.back()));
	auto ref = Reference(*nodes.back());
	write([&](RefsContainer& refs) { refs.emplace(key, ref); });
	return ref;
}

inline Reference Scope::add(Requester requester) {
	return add(static_cast<StringId>(nextId--), requester);
}

inline Reference Scope::addExclusive() {
	return addExclusive(static_cast<StringId>(nextId--));
}

inline std::vector<Reference> Scope::getAllRefs() const {
	return read<std::vector<Reference>>([&](const RefsContainer& refs) {
		std::vector<Reference> ret;
		ret.reserve(refs.size());
		for (auto& e : refs)
			ret.emplace_back(e.second);
		return ret;
	});
}

inline std::unordered_map<StringId, Reference> Scope::getAllRefsWithKey() const {
	return read<std::unordered_map<StringId, Reference>>([&](const RefsContainer& refs) {
		return refs;
	});
}

inline std::unordered_map<const Object*, StringId> Scope::getObjectMap() const {
	std::unordered_map<const Object*, StringId> objectMap;
	for (auto& e : getAllRefsWithKey()) {
		if (e.second.valueTypeWithoutLock() == ReferenceValueType::Object && !e.second.isNullWithoutLock())
			objectMap.emplace(&e.second.derefWithoutLock<Object>(), e.first);
	}
	return objectMap;
}

inline bool CapturedScope::hasCaptured(StringId key) const {
	return scope.read<bool>([&](const Scope* scope) {
		return scope == nullptr ? this->ref(StringId::CapturedScope).derefWithoutLock().has(key)
				: scope->has(key);
	});
}

inline Reference CapturedScope::refCaptured(StringId key) const {
	return scope.read<Reference>([&](const Scope* scope) {
		return scope == nullptr ? this->ref(StringId::CapturedScope).derefWithoutLock().ref(key)
				: scope->ref(key);
	});
}

inline void Storage::registerObject(Object* object) {
	{
		std::lock_guard<SpinLock> lock(lockObjects);
		if (recycledObjectIndexes.empty()) {
			object->objectIndex = objects.size();
			objects.push_back(object);
		}
		else {
			object->objectIndex = recycledObjectIndexes.back();
			recycledObjectIndexes.pop_back();
			objects[object->objectIndex] = object;
		}
	}
	object->gcGeneration = gcGeneration;
}

template<typename WriteObject, typename WriteObjectReferences>
inline void Storage::save(Writer& w, WriteObject writeObject, WriteObjectReferences writeObjectReferences) const {

	chatra_assert(audit());

#ifndef CHATRA_NDEBUG
	w.out(systemScopes.size());
	w.out(systemObjectIndex);
#endif

	// scopes (1st)
	w.saveResync(1000);

	std::unordered_set<Scope*> systemScopeSet(systemScopes.cbegin(), systemScopes.cend());
	for (auto* scope : systemScopes)
		w.CHATRA_REGISTER_POINTER(scope, Scope);

	std::vector<Scope*> scopeList;
	scopeList.reserve(scopes.size());
	for (auto* scope : scopes) {
		if (systemScopeSet.count(scope) == 0)
			scopeList.emplace_back(scope);
	}

	w.out(scopeList, [&](Scope* scope) {
		w.out(scope->type);
		w.out(scope->nextId);
		w.out(scope->captured.load());
		w.CHATRA_REGISTER_POINTER(scope, Scope);
	});

	// objects (1st)
	w.saveResync(1001);
	w.out(objects.cbegin() + systemObjectIndex, objects.cend(), [&](Object* object) {
		w.out(object != nullptr);
		if (object == nullptr)
			return;

		bool saveRefs = true;

		w.out(object->typeId);
		switch (object->typeId) {
		case TypeId::CapturedScope:
			static_cast<CapturedScope*>(object)->scope.read([&](const Scope* scope) {
				w.CHATRA_OUT_POINTER(scope, Scope);
			});
			break;

		case TypeId::CapturedScopeObject:
			break;

		default:
			(void)TypeId::Dummy;
			saveRefs = !writeObject(object->typeId, object);
			break;
		}

		saveReferences(w, object->groups, object->nodes);

		if (saveRefs) {
			auto refIndexes = createRefIndexes(object->nodes);
			saveRefsMap(w, refIndexes, object->refsMap);
			saveRefsArray(w, refIndexes, object->refsArray);
		}
	});

	w.out(recycledObjectIndexes);

	// scopes (2nd)
	w.saveResync(1002);
	w.out(scopeList, [&](Scope* scope) {
		saveReferences(w, scope->groups, scope->nodes);

		auto refIndexes = createRefIndexes(scope->nodes);
		scope->read([&](const RefsContainer& refs) {
			saveRefsMap(w, refIndexes, refs);
		});
	});

	// objects (2nd)
	w.saveResync(1003);
	w.out(objects.cbegin() + systemObjectIndex, objects.cend(), [&](Object* object) {
		if (object == nullptr)
			return;
		switch (object->typeId) {
		case TypeId::CapturedScope:
		case TypeId::CapturedScopeObject:
			break;
		default:
			writeObjectReferences(object->typeId, object);
			break;
		}
	});

	w.saveResync(1004);
}

template <typename ReadObject, typename ReadObjectReferences>
inline void Storage::restore(Reader& r, ReadObject readObject, ReadObjectReferences readObjectReferences) {

	chatra_assert(systemScopes.size() == r.read<size_t>());
	chatra_assert(systemScopes.size() == scopes.size());
	chatra_assert(systemObjectIndex == r.read<size_t>());
	chatra_assert(systemObjectIndex == objects.size());

	// scopes (1st)
	r.restoreResync(1000);

	for (auto* scope : systemScopes)
		r.CHATRA_REGISTER_POINTER(scope, Scope);

	std::vector<Scope*> scopeList;
	r.inList([&]() {
		auto type = r.read<ScopeType>();
		auto scope = std::unique_ptr<Scope>(new Scope(*this, type, r));
		r.in(scope->nextId);
		scope->captured = r.read<bool>();

		scopeList.emplace_back(scope.get());
		r.CHATRA_REGISTER_POINTER(std::move(scope), Scope);
	});

	// objects (1st)
	r.restoreResync(1001);
	r.inList([&]() {
		if (!r.read<bool>()) {
			objects.emplace_back(nullptr);
			return;
		}

		Object* object = nullptr;
		bool restoreRefs = true;

		auto typeId = r.read<TypeId>();
		switch (typeId) {
		case TypeId::CapturedScope:
			object = new CapturedScope(r.CHATRA_READ_RAW(Scope));
			break;

		case TypeId::CapturedScopeObject:
			object = new Object(TypeId::CapturedScopeObject);
			break;

		default:
			try {
				restoreRefs = !readObject(typeId, &object);
			}
			catch (...) {
				delete object;
				throw;
			}
			break;
		}

		if (object == nullptr || object->typeId != typeId)
			throw IllegalArgumentException();

		restoreReferences(*this, r, object->groups, object->nodes);

		if (restoreRefs) {
			restoreRefsMap(r, object->nodes, object->refsMap);
			restoreRefsArray(r, object->nodes, object->refsArray);
		}

		object->objectIndex = objects.size();
		object->gcGeneration = gcGeneration;
		objects.emplace_back(object);
	});

	r.inArray(recycledObjectIndexes);

	// scopes (2nd)
	r.restoreResync(1002);
	size_t scopeListIndex = 0;
	r.inList([&]() {
		auto* scope = scopeList[scopeListIndex++];
		restoreReferences(*this, r, scope->groups, scope->nodes);

		RefsContainer refsContainer;
		restoreRefsMap(r, scope->nodes, refsContainer);
		scope->write([&](RefsContainer& refs) {
			refs.insert(refsContainer.cbegin(), refsContainer.cend());
		});

		restoreObjectRefs(scope->nodes);
	});

	// objects (2nd): Restore object's cross-references
	r.restoreResync(1003);
	auto it = objects.cbegin() + systemObjectIndex;
	r.inList([&]() {
		auto* object = *it++;
		if (object == nullptr)
			return;

		restoreObjectRefs(object->nodes);

		switch (object->typeId) {
		case TypeId::CapturedScope:
		case TypeId::CapturedScopeObject:
			break;

		default:
			readObjectReferences(object->typeId, object);
			break;
		}
	});

	r.restoreResync(1004);
	chatra_assert(audit());
}


}  // namespace chatra


namespace std {

template<> struct hash<chatra::Reference> {
	size_t operator()(const chatra::Reference& x) const noexcept {
		return std::hash<void*>()(x.internal_nodePtr());
	}
};

template<> struct equal_to<chatra::Reference> {
	bool operator()(const chatra::Reference& a, const chatra::Reference& b) const noexcept {
		return a.internal_nodePtr() == b.internal_nodePtr();
	}
};

}  // namespace std

CHATRA_ENUM_HASH(chatra::Requester)

#endif //CHATRA_MEMORY_MANAGEMENT_H
