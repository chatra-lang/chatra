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

#ifndef CHATRA_CLASSES_H
#define CHATRA_CLASSES_H

#include "chatra.h"
#include "Internal.h"
#include "Parser.h"
#include "StringTable.h"
#include "MemoryManagement.h"
#include "Serialize.h"

namespace chatra {

constexpr TypeId typeId_UserObjectBase = toTypeId(0);
constexpr TypeId typeId_Exception = toTypeId(1);
constexpr TypeId typeId_Tuple = toTypeId(2);
constexpr TypeId typeId_Async = toTypeId(3);
constexpr TypeId typeId_String = toTypeId(4);
constexpr TypeId typeId_ContainerBody = toTypeId(5);
constexpr TypeId typeId_Array = toTypeId(6);
constexpr TypeId typeId_Dict = toTypeId(7);

class Class;
class ClassTable;
struct IClassFinder;
class ObjectBase;
class Tuple;
class Frame;
class Thread;
class Package;
class NativeReferenceImp;

struct HashPairStringId {
	size_t operator()(const std::pair<StringId, StringId>& x) const noexcept {
		return std::hash<std::underlying_type<StringId>::type>()(static_cast<std::underlying_type<StringId>::type>(x.first))
		        ^ std::hash<std::underlying_type<StringId>::type>()(static_cast<std::underlying_type<StringId>::type>(x.second) + 1);
	}
};

struct HashPairClassPtr {
	size_t operator()(const std::pair<const Class*, const Class*>& x) const noexcept;
};


struct ArgumentDef {
	enum class Type {
		// Before delimiter (contains the case of no delimiter)
		List,
		ListVarArg,
		// After delimiter
		Dict,
		DictVarArg,
	};

	Type type = Type::List;
	StringId name = StringId::Invalid;
	const Class* cl = nullptr;
	Node* defaultOp = nullptr;  // Pair operator
};


struct ArgumentSpec {
	StringId key = StringId::Invalid;
	const Class* cl = nullptr;
public:
	ArgumentSpec(StringId key = StringId::Invalid, const Class* cl = nullptr) noexcept : key(key), cl(cl) {}

	CHATRA_DECLARE_SERIALIZE_METHODS(ArgumentSpec);
};

std::string toString(const StringTable* sTable, const ArgumentSpec& arg);
std::string toString(const StringTable* sTable, const std::vector<ArgumentSpec>& args);


struct ArgumentMatcher {
	std::vector<ArgumentDef> args;
	std::vector<ArgumentDef> subArgs;

private:
	size_t listCount = 0;
	size_t listCountWithoutDefaults = 0;
	size_t dictCountWithoutDefaults = 0;
	bool hasListVarArg = false;
	bool hasDictVarArg = false;
	std::unordered_map<StringId, size_t> byName;

public:
	ArgumentMatcher() noexcept;
	ArgumentMatcher(std::vector<ArgumentDef> args, std::vector<ArgumentDef> subArgs) noexcept;

	const std::vector<ArgumentDef>& refSubArgs() const {
		return subArgs;
	}

	bool matches(const std::vector<ArgumentSpec>& args, const std::vector<ArgumentSpec>& subArgs) const;

	template <class TupleType, typename Mapper, typename MapperForContainer, typename MapperForNull>
	void mapArgs(const TupleType& tuple, Mapper mapper, MapperForContainer mapperForContainer,
			MapperForNull mapperForNull) const;

	template <class TupleType, typename Mapper, typename MapperForContainer, typename MapperForNull>
	void mapReturns(const TupleType& tuple, Mapper mapper, MapperForContainer mapperForContainer,
			MapperForNull mapperForNull) const;

#ifndef NDEBUG
	void dumpArgumentMatcher(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


struct MethodBase {
	Node* node;  // Can be nullptr for default constructor

public:
	explicit MethodBase(Node* node) noexcept : node(node) {}
};


struct Method : public MethodBase, public ArgumentMatcher {
	const Class* cl;
	StringId name;
	StringId subName;  // init.*() or foo()."set"(); default = StringId::Invalid
	size_t position;  // for variables; SIZE_MAX for methods
	size_t primaryPosition;  // for variables

public:
	// Constructor for variables
	Method(Node* node, const Class* cl, StringId name, size_t position, size_t primaryPosition) noexcept;

	// Constructor for methods
	Method(Node* node, const Class* cl, StringId name, StringId subName,
			std::vector<ArgumentDef> args, std::vector<ArgumentDef> subArgs) noexcept;

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


struct OperatorMethod : public MethodBase {
	friend class OperatorTable;

	Package* package;  // Can be nullptr for embedded methods
	Operator op;  // [name = "operator"]
	std::vector<ArgumentDef> args;

public:
	OperatorMethod(Package* package, Node* node, Operator op, std::vector<ArgumentDef> args) noexcept
			: MethodBase(node), package(package), op(op), args(std::move(args)) {}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


struct NativeMethod {
	using Signature = void (*)(NativeCallHandler handler, Thread& thread, ObjectBase* object,
			StringId name, StringId subName, Tuple& args, Reference ret);
	Signature method;
	NativeCallHandler handler;

public:
	explicit NativeMethod(Signature method, NativeCallHandler handler = nullptr) noexcept
			: method(method), handler(handler) {}

	bool isNull() const { return method == nullptr; }
};


class MethodTable {
public:
	enum class Source {
		EmbeddedMethods, ClassMethods, ClassSuperMethods, ClassConstructors, InnerFunctions
	};

private:
	Source source;
	const Class* sourceClassPtr = nullptr;
	Package* sourcePackagePtr = nullptr;
	Node* sourceNodePtr = nullptr;

	std::forward_list<Method> methods;
	std::unordered_map<StringId, std::vector<const Method*>> byName;  // lower priority first
	size_t size = 0;
	size_t baseSize = 0;
	std::unordered_map<Node*, size_t> syncMap;
	std::unordered_map<std::pair<StringId, StringId>, NativeMethod, HashPairStringId> nativeMethods;

public:
	struct ForEmbeddedMethods {};
	explicit MethodTable(ForEmbeddedMethods forEmbeddedMethods) noexcept;

	MethodTable(const Class* cl, Source source) noexcept;
	MethodTable(Package& package, Node* node) noexcept;

	Source getSource() const {
		return source;
	}

	const Class* sourceClass() const {
		assert(source == Source::ClassMethods || source == Source::ClassSuperMethods || source == Source::ClassConstructors);
		return sourceClassPtr;
	}

	Package* sourcePackage() const {
		assert(source == Source::InnerFunctions);
		return sourcePackagePtr;
	}

	Node* sourceNode() const {
		assert(source == Source::InnerFunctions);
		return sourceNodePtr;
	}

	size_t add(Node* node, const Class* cl, StringId name, Node* syncNode) noexcept;

	void add(Node* node, const Class* cl, StringId name, StringId subName,
			std::vector<ArgumentDef> args, std::vector<ArgumentDef> subArgs) noexcept;

	void add(StringId name, StringId subName, NativeMethod method) noexcept {
		nativeMethods.emplace(std::make_pair(name, subName), method);
	}

	// Add all methods in r except native methods; Some Method instances are copied.
	void inherit(const MethodTable& r, const Class* cl);

	const Method* find(const Class* cl, StringId name, StringId subName,
			const std::vector<ArgumentSpec>& args, const std::vector<ArgumentSpec>& subArgs) const;

	std::vector<const Method*> findByName(StringId name, StringId subName) const;

	NativeMethod findNativeMethod(StringId name, StringId subName) const {
		auto it = nativeMethods.find(std::make_pair(name, subName));
		return it == nativeMethods.cend() ? NativeMethod(nullptr) : it->second;
	}

	std::vector<size_t> getPrimaryIndexes() const;

	template <typename Predicate>
	void forEach(Predicate pred) const {
		for (auto& method : methods)
			pred(method);
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


class OperatorTable {
	friend class AsyncOperatorTable;

private:
	std::forward_list<OperatorMethod> methods;
	std::unordered_map<const Class*, const OperatorMethod*> byOp1[NumberOfOperators];
	std::unordered_map<std::pair<const Class*, const Class*>, const OperatorMethod*, HashPairClassPtr>
	        byOp2[NumberOfOperators];

private:
	void addOp1(const OperatorMethod* method) {
		byOp1[static_cast<size_t>(method->op)].emplace(method->args[0].cl, method);
	}

	void addOp2(const OperatorMethod* method) {
		byOp2[static_cast<size_t>(method->op)].emplace(std::make_pair(method->args[0].cl, method->args[1].cl), method);
	}

public:
	void add(Package* package, Node* node, Operator op, std::vector<ArgumentDef> args) {
		methods.emplace_front(package, node, op, std::move(args));
		if (methods.front().args.size() == 1)
			addOp1(&methods.front());
		else
			addOp1(&methods.front());
	}

	const OperatorMethod* find(Operator op, const Class* argCl) const;
	const OperatorMethod* find(Operator op, const Class* argCl0, const Class* argCl1) const;

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


class AsyncOperatorTable : private AsyncReadWrite<OperatorTable> {
private:
	std::forward_list<OperatorMethod> methods;

public:
	void add(Package* package, Node* node, Operator op, std::vector<ArgumentDef> args) {
		methods.emplace_front(package, node, op, std::move(args));
		auto method = &methods.front();
		writeAsync([&](OperatorTable& table) {
			if (method->args.size() == 1)
				table.addOp1(method);
			else
				table.addOp2(method);
		});
	}

	const OperatorMethod* find(Operator op, const Class* argCl) {
		return read<const OperatorMethod*>([&](const OperatorTable& table) { return table.find(op, argCl); });
	}

	const OperatorMethod* find(Operator op, const Class* argCl0, const Class* argCl1) {
		return read<const OperatorMethod*>([&](const OperatorTable& table) { return table.find(op, argCl0, argCl1); });
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


class MethodTableCache {
private:
	std::unordered_map<Node*, std::unique_ptr<MethodTable>> cache;

public:
	const MethodTable* scanInnerFunctions(IErrorReceiver* errorReceiver, const StringTable* sTable,
			Package& package, Node* node);
};


using ObjectBuilder = void (*)(const Class* cl, Reference ref);


class Class {
	friend class ClassTable;

private:
	Package* package;  // Can be nullptr for embedded classes
	Node* node;
	StringId name;
	ObjectBuilder objectBuilder;

	std::vector<const Class*> directParents;
	std::vector<const Class*> initializationOrder;
	std::unordered_set<const Class*> parentsSet;

	MethodTable methods;
	MethodTable superMethods;
	MethodTable constructors;

private:
	Class(Package* package, Node* node, StringId name, ObjectBuilder objectBuilder) noexcept
			: package(package), node(node), name(name), objectBuilder(objectBuilder),
			methods(this, MethodTable::Source::ClassMethods),
			superMethods(this, MethodTable::Source::ClassSuperMethods),
			constructors(this, MethodTable::Source::ClassConstructors) {}

	void inheritParents(std::vector<const Class*> directParents);
	static std::unordered_map<Node*, Node*> getSyncMap(Node* node);

public:
	explicit Class(StringId name) noexcept : Class(nullptr, nullptr, name, nullptr) {}

	// Adding parent class; this is used for declaring exceptions
	void addParentClass(const Class* clParent);

	// Constructor for primitives
	struct AddCopyConstructor {};
	Class(StringId name, AddCopyConstructor addCopyConstructor) noexcept;

	// Adding special constructor for some embedded types (e.g. exceptions)
	void addDefaultConstructor(ObjectBuilder objectBuilder);

	// Adding special constructor for primitives
	void addConvertingConstructor(const Class* sourceCl);

	// Constructor for packages and compound types
	// Constructor itself does only allocating memory and
	Class(IErrorReceiver& errorReceiver, const StringTable* sTable, IClassFinder& classFinder,
			Package* package, Node* node,
			ObjectBuilder objectBuilder, std::vector<std::tuple<StringId, StringId, NativeMethod>> nativeMethods) noexcept;

	// Constructor for compound types (two-step initialization)
	Class(Package* package, Node* node) noexcept;

	void initialize(IErrorReceiver& errorReceiver, const StringTable* sTable, IClassFinder& classFinder,
			ObjectBuilder objectBuilder, std::vector<std::tuple<StringId, StringId, NativeMethod>> nativeMethods);

	Package* getPackage() const {
		return package;
	}

	Node* getNode() const {
		return node;
	}

	StringId getName() const {
		return name;
	}

	ObjectBuilder getObjectBuilder() const {
		return objectBuilder;
	}

	bool isAssignableFrom(const Class* cl) const {
		assert(cl != nullptr);
		return cl == this || cl->parentsSet.count(this) != 0;
	}

	const std::vector<const Class*>& refDirectParents() const {
		return directParents;
	}

	const std::vector<const Class*>& refInitializationOrder() const {
		return initializationOrder;
	}

	const std::unordered_set<const Class*>& refParents() const {
		return parentsSet;
	}

	const MethodTable& refMethods() const {
		return methods;
	}

	const MethodTable& refSuperMethods() const {
		return superMethods;
	}

	const MethodTable& refConstructors() const {
		return constructors;
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


class ClassTable {
private:
	std::forward_list<Class> classes;
	std::unordered_map<StringId, const Class*> byName;

public:
	void add(const Class* cl) {
		byName.emplace(cl->name, cl);
	}

	// Return value is not with const qualifier because this method will be followed by Class::initialize() call
	Class* emplace(Package* package, Node* node) {
		classes.emplace_front(package, node);
		auto* cl = &classes.front();
		add(cl);
		return cl;
	}

	void add(const ClassTable& r);

	const Class* find(StringId name) const {
		auto it = byName.find(name);
		return it == byName.end() ? nullptr : it->second;
	}

	const std::unordered_map<StringId, const Class*>& refClassMap() const {
		return byName;
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const;
#endif // !NDEBUG
};


struct IClassFinder {
	virtual ~IClassFinder() = default;
	virtual const Class* findClass(StringId name) = 0;
	virtual const Class* findPackageClass(StringId packageName, StringId name) = 0;
};

class WrappedClassFinder : public IClassFinder {
protected:
	IClassFinder& classFinder;
public:
	explicit WrappedClassFinder(IClassFinder& classFinder) noexcept : classFinder(classFinder) {}

	const Class* findClass(StringId name) override {
		return classFinder.findClass(name);
	}

	const Class* findPackageClass(StringId packageName, StringId name) override {
		return classFinder.findPackageClass(packageName, name);
	}
};

ArgumentDef nodeToArgument(IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node);

std::vector<ArgumentDef> tupleToArguments(IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node);

void addMethod(MethodTable& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node, const Class* cl = nullptr);

template <class OperatorMethodTableType>
void addOperatorMethod(OperatorMethodTableType& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Package* package, Node* node);

void addInnerMethods(MethodTable& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Node* node);


class ObjectBase : public Object {
private:
	const Class* cl;

public:
	// Constructor for primitives and compound types (= classes)
	ObjectBase(Storage& storage, TypeId typeId, const Class* cl) noexcept
			: Object(storage, typeId, cl->refMethods().getPrimaryIndexes()), cl(cl) {}

	// Constructor for array-like objects
	ObjectBase(Storage& storage, TypeId typeId, const Class* cl, size_t size, Requester requester = InvalidRequester) noexcept
			: Object(storage, typeId, size, requester), cl(cl) {}

	// Constructor for containers
	struct ElementsAreExclusive {};
	ObjectBase(Storage& storage, TypeId typeId, const Class* cl, size_t size, ElementsAreExclusive elementsAreExclusive) noexcept
			: Object(storage, typeId, size, Object::ElementsAreExclusive()), cl(cl) {
		(void)elementsAreExclusive;
	}

	// Constructor used for only restoring class's body
	ObjectBase(TypeId typeId, const Class* cl) noexcept : Object(typeId), cl(cl) {}

	const Class* getClass() const {
		return cl;
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};

class UserObjectBase : public ObjectBase {
private:
	bool deinitCalled = false;
	std::unique_ptr<INativePtr> nativePtr;
	std::unordered_set<const Class*> uninitializedParents;

public:
	UserObjectBase(Storage& storage, const Class* cl) noexcept : ObjectBase(storage, typeId_UserObjectBase, cl) {
		uninitializedParents.insert(cl->refParents().cbegin(), cl->refParents().cend());
	}

	INativePtr* refNativePtr() const {
		return nativePtr.get();
	}

	void setNativePtr(INativePtr * ptr) {
		nativePtr.reset(ptr);
	}

	std::unordered_set<const Class*>& refUninitializedParents() {
		return uninitializedParents;
	}

	// Implemented in Runtime.cpp
	void onDestroy(void* tag, Requester requester, Reference& ref) override;
	bool hasOnDestroy() const override { return true; }

	// CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(UserObjectBase);
	bool save(Writer& w) const;
	explicit UserObjectBase(const Class* cl) noexcept : ObjectBase(typeId_UserObjectBase, cl) {}
	bool restore(Reader& r);
	void restoreReferences(Reader& r);
};

const Class* getReferClass(Reference ref);


template <class Type, StringId name>
class PreDefined {
private:
	static Class pdCl;

public:
	static const Class* getClassStatic() {
		return &pdCl;
	}

	static Class* getWritableClassStatic() {
		return &pdCl;
	}
};

template <class Type, StringId name>
Class PreDefined<Type, name>::pdCl(name);


template <class Type, StringId name>
class Primitive : public ObjectBase {
private:
	static Class cl;

public:
	using ObjectBase::ObjectBase;

	static const Class* getClassStatic() {
		return &cl;
	}

	static Class* getWritableClassStatic() {
		return &cl;
	}
};

template <class Type, StringId name>
Class Primitive<Type, name>::cl(name, Class::AddCopyConstructor());

using Bool = Primitive<bool, StringId::Bool>;
using Int = Primitive<int64_t, StringId::Int>;
using Float = Primitive<double, StringId::Float>;


struct ExceptionBase : public ObjectBase {
protected:
	ExceptionBase(Storage& storage, const Class* cl) noexcept : ObjectBase(storage, typeId_Exception, cl) {}

public:
	virtual bool save(Writer& w) const = 0;
};

template <StringId name>
struct Exception : public ExceptionBase, public PreDefined<Exception<name>, name> {
public:
	explicit Exception(Storage& storage) noexcept
			: ExceptionBase(storage, PreDefined<Exception<name>, name>::getClassStatic()) {}

	bool save(Writer& w) const override {
		w.out(name);
		return true;
	}
};

#define CHATRA_SWITCH_EXCEPTION(stringId)  case StringId::stringId:  \
		Predicate<Exception<StringId::stringId>>()(std::forward<Args>(args)...);  break

template <template <class Type> class Predicate, class... Args>
void switchException(StringId name, Args&&... args) {
	switch (name) {
	CHATRA_SWITCH_EXCEPTION(ParserErrorException);
	CHATRA_SWITCH_EXCEPTION(UnsupportedOperationException);
	CHATRA_SWITCH_EXCEPTION(PackageNotFoundException);
	CHATRA_SWITCH_EXCEPTION(ClassNotFoundException);
	CHATRA_SWITCH_EXCEPTION(MemberNotFoundException);
	CHATRA_SWITCH_EXCEPTION(IncompleteExpressionException);
	CHATRA_SWITCH_EXCEPTION(TypeMismatchException);
	CHATRA_SWITCH_EXCEPTION(NullReferenceException);
	CHATRA_SWITCH_EXCEPTION(OverflowException);
	CHATRA_SWITCH_EXCEPTION(DivideByZeroException);
	CHATRA_SWITCH_EXCEPTION(IllegalArgumentException);
	CHATRA_SWITCH_EXCEPTION(NativeException);
	default:
		throw InternalError();
	}
}

struct RuntimeException : public std::exception {
	StringId name;
public:
	explicit RuntimeException(StringId name) noexcept : name(name) {}
};


class Tuple : public ObjectBase, public PreDefined<Tuple, StringId::Tuple> {
public:
	struct Key {
		StringId sid = StringId::Invalid;
		std::string str;
	public:
		Key() noexcept : sid(StringId::Invalid) {}
		Key(StringId sid) noexcept : sid(sid) {}
		Key(std::string str) noexcept : sid(StringId::Invalid), str(std::move(str)) {}

		bool isNull() const {
			return sid == StringId::Invalid && str.empty();
		}

		CHATRA_DECLARE_SERIALIZE_METHODS(Key);
	};

private:
	std::vector<Key> indexToKey;
	std::unordered_map<StringId, size_t> sidToIndex;
	std::unordered_map<std::string, size_t> strToIndex;

public:
	Tuple(Storage& storage, Requester requester) noexcept;
	Tuple(Storage& storage, const StringTable* sTable, const std::vector<std::pair<Key, Reference>>& args,
			Requester requester) noexcept;

	size_t tupleSize() const {
		return size();
	}

	const Key& key(size_t position) const;
	size_t find(const StringTable* sTable, StringId key) const;
	size_t find(const StringTable* sTable, const std::string& key) const;

	std::vector<ArgumentSpec> toArgs() const;

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(Tuple);

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};


#define CHATRA_NATIVE_ARGS  NativeCallHandler handler, Thread& thread, ObjectBase* object,   \
	StringId name, StringId subName, Tuple& args, Reference ret
#define CHATRA_NATIVE_ARGS_FORWARD  handler, thread, object, name, subName, args, ret
#define CHATRA_NATIVE_ARGS_CAPTURE  (void)handler; (void)thread; (void)object;   \
	(void)name; (void)subName; (void)args; (void)ret

void native_wait(CHATRA_NATIVE_ARGS);


template <class Type>
class NativeSelf {
public:
	static Type& getSelf(ObjectBase* object) {
		static_assert(std::is_base_of<ObjectBase, Type>::value, "Type should be derived class of ObjectBase");
		return *static_cast<Type*>(object);
	}
};

#define CHATRA_NATIVE_SELF  CHATRA_NATIVE_ARGS_CAPTURE; auto& self = getSelf(object); (void)self


using EventWatcher = bool (*)(void* tag);

class EventObject {
private:
	SpinLock lockWatchers;
	std::unordered_map<void*, EventWatcher> registered;
	std::unordered_map<void*, EventWatcher> activated;
	unsigned count = 0;

public:
	void registerWatcher(void* tag, EventWatcher watcher);
	void activateWatcher(void* tag);
	bool unregisterWatcher(void* tag);

protected:
	void notifyOne();
	void notifyAll();

	void saveEventObject(Writer& w) const;
	void restoreEventObject(Reader& r);
};


class Async : public ObjectBase, private NativeSelf<Async>, public EventObject {
private:
	std::atomic<bool> updated = {false};
public:
	static const Class* getClassStatic();

	explicit Async(Storage& storage) noexcept : ObjectBase(storage, typeId_Async, getClassStatic()) {}

	void updateResult(Reference ref);

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(Async);

	static void native_updated(CHATRA_NATIVE_ARGS);
};


class String : public ObjectBase, private NativeSelf<String> {
private:
	static constexpr size_t positionHashPitch = 1U << 4U;
	mutable SpinLock lockValue;
	std::string value;
	size_t length = 0;
	std::vector<size_t> positionHash;

private:
	static char32_t getCharOrThrow(const std::string& str, size_t index);
	static size_t extractCharOrThrow(char32_t c, char* dest);

	size_t fetchIndex(const Reference& ref, bool allowBoundary = false) const;
	size_t getPosition(size_t index) const;
	void rehash();

public:
	static const Class* getClassStatic();

	explicit String(Storage& storage) noexcept : ObjectBase(storage, typeId_String, getClassStatic()) {}

	void setValue(std::string value) {
		std::lock_guard<SpinLock> lock(lockValue);
		this->value = std::move(value);
		rehash();
	}

	std::string getValue() const {
		std::lock_guard<SpinLock> lock(lockValue);
		return value;
	}

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(String);

	static void native_initFromString(CHATRA_NATIVE_ARGS);
	static void native_initFromChar(CHATRA_NATIVE_ARGS);
	static void native_size(CHATRA_NATIVE_ARGS);
	static void native_set(CHATRA_NATIVE_ARGS);
	static void native_add(CHATRA_NATIVE_ARGS);
	static void native_insert(CHATRA_NATIVE_ARGS);
	static void native_append(CHATRA_NATIVE_ARGS);
	static void native_at(CHATRA_NATIVE_ARGS);
	static void native_remove(CHATRA_NATIVE_ARGS);
	static void native_clone(CHATRA_NATIVE_ARGS);
	static void native_equals(CHATRA_NATIVE_ARGS);
	static void native_sub(CHATRA_NATIVE_ARGS);

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};


class ContainerBody : public Object {
public:
	ContainerBody(Storage& storage, size_t capacity) noexcept;
	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(ContainerBody);
};

class ContainerBase : public ObjectBase {
	friend class TemporaryTuple;
	friend class NativeReferenceImp;
	friend void native_wait(CHATRA_NATIVE_ARGS);

protected:
	mutable SpinLock lockValue;
	size_t length = 0;

private:
	size_t capacity = 8;

protected:
	size_t getCapacity() const {
		return capacity;
	}

	void extend(size_t required);
	Object& container() const;

	ContainerBase(Storage& storage, TypeId typeId, const Class* cl) noexcept;
	ContainerBase(TypeId typeId, const Class* cl) noexcept;
};


class Array : public ContainerBase, private NativeSelf<Array> {
	friend class Dict;
	friend class TemporaryTuple;
	friend class NativeReferenceImp;
	friend void native_wait(CHATRA_NATIVE_ARGS);

private:
	size_t fetchIndex(const Reference& ref, bool allowBoundary = false) const;

public:
	static const Class* getClassStatic();

	explicit Array(Storage& storage, size_t initialLength = 0) noexcept
			: ContainerBase(storage, typeId_Array, getClassStatic()) {
		extend(initialLength);
		length = initialLength;
	}

	void add(Reference ref);

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(Array);

	static void native_size(CHATRA_NATIVE_ARGS);
	static void native_add(CHATRA_NATIVE_ARGS);
	static void native_insert(CHATRA_NATIVE_ARGS);
	static void native_remove(CHATRA_NATIVE_ARGS);
	static void native_at(CHATRA_NATIVE_ARGS);

	// Special member access for finalizer
	template <typename Process>
	bool popIfExists(Process process) {
		std::lock_guard<SpinLock> lock(lockValue);
		if (length == 0)
			return false;
		auto ref = container().ref(--length);
		process(ref);
		ref.setNull();
		return true;
	}

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};


class Dict : public ContainerBase, private NativeSelf<Dict> {
	friend class TemporaryTuple;
	friend class NativeReferenceImp;
	friend void native_wait(CHATRA_NATIVE_ARGS);

private:
	std::unordered_map<std::string, size_t> keyToIndex;
	std::vector<size_t> freeIndexes;

public:
	static const Class* getClassStatic();

	explicit Dict(Storage& storage) noexcept : ContainerBase(storage, typeId_Dict, getClassStatic()) {}

	void add(std::string key, Reference ref);
	bool remove(const std::string& key, Reference& ret);

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(Dict);

	static void native_size(CHATRA_NATIVE_ARGS);
	static void native_has(CHATRA_NATIVE_ARGS);
	static void native_add(CHATRA_NATIVE_ARGS);
	static void native_at(CHATRA_NATIVE_ARGS);
	static void native_remove(CHATRA_NATIVE_ARGS);
	static void native_keys(CHATRA_NATIVE_ARGS);
	static void native_values(CHATRA_NATIVE_ARGS);

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};


void initializeEmbeddedClasses();
ClassTable& refEmbeddedClassTable();

void registerEmbeddedClasses(ClassTable& classes);
const std::unordered_map<StringId, Node*>& refNodeMapForEmbeddedClasses();


inline size_t HashPairClassPtr::operator()(const std::pair<const Class*, const Class*>& x) const noexcept {
	return std::hash<const void*>()(x.first) ^ std::hash<const void*>()(x.second + 1);
}

template <class TupleType, typename Mapper, typename MapperForContainer, typename MapperForNull>
inline void ArgumentMatcher::mapArgs(const TupleType& tuple, Mapper mapper, MapperForContainer mapperForContainer,
		MapperForNull mapperForNull) const {

	std::vector<size_t> indexesForList;
	std::vector<size_t> indexesForDict;
	std::vector<bool> destMapped(args.size(), false);

	for (size_t i = 0; i < tuple.tupleSize(); i++) {
		auto& arg = tuple.key(i);
		if (arg.isNull()) {
			if (i < listCount) {
				mapper(i, args[i], i);
				destMapped[i] = true;
				continue;
			}

			if (hasListVarArg)
				indexesForList.emplace_back(i);
			continue;
		}

		if (arg.sid != StringId::Invalid) {
			auto it = byName.find(arg.sid);
			if (it != byName.cend()) {
				mapper(it->second, args[it->second], i);
				destMapped[it->second] = true;
				continue;
			}
		}

		if (hasDictVarArg)
			indexesForDict.emplace_back(i);
		else if (hasListVarArg)
			indexesForList.emplace_back(i);
	}

	if (hasListVarArg) {
		mapperForContainer(listCount, args[listCount], std::move(indexesForList));
		destMapped[listCount] = true;
	}
	if (hasDictVarArg) {
		mapperForContainer(args.size() - 1, args.back(), std::move(indexesForDict));
		destMapped.back() = true;
	}

	std::vector<size_t> destIndexes;
	for (size_t j = 0; j < args.size(); j++) {
		if (!destMapped[j])
			destIndexes.emplace_back(j);
	}
	if (!destIndexes.empty())
		mapperForNull(std::move(destIndexes));
}

template <class TupleType, typename Mapper, typename MapperForContainer, typename MapperForNull>
inline void ArgumentMatcher::mapReturns(const TupleType& tuple, Mapper mapper, MapperForContainer mapperForContainer,
		MapperForNull mapperForNull) const {

	std::vector<size_t> indexesForList;
	std::vector<size_t> indexesForDict;

	std::vector<size_t> srcIndexes(tuple.tupleSize());
	for (size_t i = 0; i < tuple.tupleSize(); i++)
		srcIndexes[i] = i;

	std::vector<size_t> destIndexes(args.size());
	for (size_t i = 0; i < args.size(); i++)
		destIndexes[i] = i;
	if (hasListVarArg)
		destIndexes[listCount] = SIZE_MAX;
	if (hasDictVarArg)
		destIndexes.back() = SIZE_MAX;

	for (size_t i = 0; i < tuple.tupleSize(); i++) {
		auto& arg = tuple.key(i);
		if (arg.sid == StringId::Invalid)
			continue;
		auto it = byName.find(arg.sid);
		if (it == byName.cend())
			continue;
		mapper(it->second, args[it->second], i);
		srcIndexes[i] = SIZE_MAX;
		destIndexes[it->second] = SIZE_MAX;
	}

	size_t srcIndex = 0;
	for (size_t j = 0; j < listCount; j++) {
		if (destIndexes[j] == SIZE_MAX)
			continue;
		auto it = std::find_if(srcIndexes.cbegin() + srcIndex, srcIndexes.cend(), [&](size_t srcIndex) { return srcIndex != SIZE_MAX; });
		if (it == srcIndexes.cend())
			break;
		srcIndex = static_cast<size_t>(std::distance(srcIndexes.cbegin(), it));
		mapper(j, args[j], srcIndex);
		srcIndexes[srcIndex] = SIZE_MAX;
		destIndexes[j] = SIZE_MAX;
		srcIndex++;
	}

	for (; srcIndex < tuple.tupleSize(); srcIndex++) {
		if (srcIndexes[srcIndex] == SIZE_MAX)
			continue;
		if (tuple.key(srcIndex).sid != StringId::Invalid && hasDictVarArg)
			indexesForDict.emplace_back(srcIndex);
		else if (hasListVarArg)
			indexesForList.emplace_back(srcIndex);
	}

	if (hasListVarArg)
		mapperForContainer(listCount, args[listCount], std::move(indexesForList));
	if (hasDictVarArg)
		mapperForContainer(args.size() - 1, args.back(), std::move(indexesForDict));

	destIndexes.erase(std::remove_if(destIndexes.begin(), destIndexes.end(), [](size_t destIndex) {
		return destIndex == SIZE_MAX; }), destIndexes.end());
	if (!destIndexes.empty())
		mapperForNull(std::move(destIndexes));
}

template <class OperatorMethodTableType>
inline void addOperatorMethod(OperatorMethodTableType& table, IErrorReceiver& errorReceiver,
		const StringTable* sTable, IClassFinder& classFinder, Package* package, Node* node) {

	std::vector<ArgumentDef> args;
	for (auto& n : node->subNodes[0]->subNodes)
		args.emplace_back(nodeToArgument(errorReceiver, sTable, classFinder, n.get()));

	if (std::find_if(args.cbegin(), args.cend(), [](const ArgumentDef& arg) { return arg.cl != nullptr; }) == args.cend()) {
		errorAtNode(errorReceiver, ErrorLevel::Warning, node,
				"strongly recommended adding at least one class restriction to avoid breaking core functionality", {});
	}

	table.add(package, node, node->subNodes[0]->op, std::move(args));
}

}  // namespace chatra

#endif //CHATRA_VALUES_H
