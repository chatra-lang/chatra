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

#ifndef CHATRA_RUNTIME_H
#define CHATRA_RUNTIME_H

#include "chatra.h"
#include "Internal.h"
#include "LexicalAnalyzer.h"
#include "Parser.h"
#include "StringTable.h"
#include "MemoryManagement.h"
#include "Classes.h"
#include "Timer.h"
#include "Serialize.h"

#ifndef NDEBUG
	// #define CHATRA_DEBUG_LOCK
	// #define CHATRA_TRACE_TEMPORARY_ALLOCATION
#endif

namespace chatra {

constexpr TypeId typeId_TemporaryObject = toTypeId(8);
constexpr TypeId typeId_TemporaryTuple = toTypeId(9);
constexpr TypeId typeId_TupleAssignmentMap = toTypeId(10);
constexpr TypeId typeId_FunctionObject = toTypeId(11);
constexpr TypeId typeId_WaitContext = toTypeId(12);
constexpr TypeId typeId_PackageObject = toTypeId(13);

class Frame;
class Thread;
class Instance;
class Package;
class RuntimeImp;

struct OperatorNotSupportedException : public std::exception {};
struct AbortThreadException : public std::exception {};

// Special node types
constexpr NodeType adhocNodeType(size_t delta) noexcept {
	return static_cast<NodeType>(NumberOfNodeTypes + delta);
}

constexpr NodeType ntFinalizer = adhocNodeType(0);
constexpr NodeType ntParserError = adhocNodeType(1);

// Special operator types
constexpr Operator adhocOperator(size_t delta) noexcept {
	return static_cast<Operator>(NumberOfOperators + delta);
}

constexpr Operator opInitializePackage = adhocOperator(0);
constexpr Operator opEvaluateTuple = adhocOperator(1);
constexpr Operator opInitializeVars = adhocOperator(2);
constexpr Operator opPopValue = adhocOperator(3);
constexpr Operator opPostConstructor = adhocOperator(4);
constexpr Operator opPostPostfixUnaryOperator = adhocOperator(5);
constexpr Operator opRestoreTemporaryLock = adhocOperator(6);
constexpr Operator opReturnFromAsync = adhocOperator(7);
constexpr Operator opProcessFor_CallNext = adhocOperator(8);
constexpr Operator opVarStatementMarker = adhocOperator(9);

constexpr PackageId finalizerPackageId = static_cast<PackageId>(0);
constexpr InstanceId finalizerInstanceId = static_cast<InstanceId>(1);
constexpr Requester finalizerThreadId = static_cast<Requester>(1);


class TemporaryObject : public Object {
public:
	enum class Type : unsigned {
		// Nothing is pointed (implicitly point to top level of frames).
		Empty,

		// No parent, temporary reference value
		Rvalue,

		// Specifies imported package
		Package,

		// Class name
		Class,
		// Calling constructor for building new object
		Constructor,
		// Calling constructor for initialization of some object
		ConstructorCall,

		// Methods
		FrameMethod,
		ObjectMethod,

		// Reference to global, package or local variable
		ScopeRef,
		// Reference to variable on object
		ObjectRef,

		// Reference to "self"
		Self,
		// Reference to "super"
		Super,
		// Reference to "super.SomeSuperClass"
		ExplicitSuper,
	};

	enum class EvaluationResult {
		Succeeded, Failed, Partial
	};

private:
	Thread& thread;

	Type type;
	Reference targetRef;
	size_t primaryIndex = SIZE_MAX;
	size_t frameIndex = SIZE_MAX;
	Package* package = nullptr;
	const Class* cl = nullptr;
	const MethodTable* methodTable = nullptr;
	const Method* method0 = nullptr;
	const Method* method1 = nullptr;
	bool methodHasArgs = false;
	const NativeMethod* nativeMethod = nullptr;

	// Restrictions
	bool hasName = false;
	bool hasArgs = false;
	bool hasSetArg = false;
	bool hasVarArgOp = false;

	Node* node = nullptr;
	StringId name = StringId::Invalid;
	std::vector<ArgumentSpec> args;
	ArgumentSpec setArg;

private:
	void clearTargetRef();
	void clearRestrictions();

	const Method* findSetMethod(const MethodTable& table, const Class* cl, StringId name);
	void resolveNativeMethod(StringId name, StringId subName);

	bool findOnObject(Reference sourceRef, const MethodTable& table, const Class* cl,
			StringId overwriteName = StringId::Invalid);
	bool findOnPackage(Package* package);
	bool findScopeRef(Scope* scope);
	bool findFrameMethods(size_t frameIndex, Package* package, const MethodTable& table);
	bool findConstructor(const Class* cl, StringId subName);
	bool findConstructorCall(Reference sourceRef, const Class* cl, StringId subName);

	void checkBeforeEvaluation();
	EvaluationResult evaluateOnFrame(size_t frameIndex);
	EvaluationResult evaluateDirect(bool throwIfNotFound);
	bool partialEvaluation(size_t frameIndex, const Class* cl);

public:
	TemporaryObject(Storage& storage, Thread& thread) noexcept;

	void copyFrom(TemporaryObject& r);

	void clear();

	// Clear current value and prepare empty reference to set rvalue.
	Reference setRvalue();

	// Starts from a name
	void setName(Node* node, StringId name);

	// Starts from a reference;
	// note that TemporaryObject initialized by this method will retain no reference against garbage collector,
	// so GC may collect the object pointed by sourceRef otherwise another reference exists.
	void setReference(Reference sourceRef);

	// Invoke a method; equivalent to setReference -> selectElement -> addArgument. Requires evaluation.
	void invokeMethod(Node* node, Reference sourceRef, StringId name, std::vector<ArgumentSpec> args);

private:
	void setPackage(Package* package);
	void setClass(const Class* cl);
	void setConstructor(const Class* cl, const MethodTable* methodTable, const Method* method);
	void setConstructorCall(Reference sourceRef, const Class* cl, const MethodTable* methodTable,
			const Method* method);
	void setFrameMethod(size_t frameIndex, Package* package, const MethodTable* methodTable,
			const Method* refMethod, const Method* setMethod, bool hasArgs);
	void setObjectMethod(Reference sourceRef, const MethodTable* methodTable,
			const Method* refMethod, const Method* setMethod, bool hasArgs);
	void setScopeRef(Reference targetRef);
	void setObjectRef(Reference sourceRef, Reference targetRef, size_t primaryIndex);
	void setSelf(Reference targetRef);
	void setSuper(Reference targetRef, const Class* cl);
	void setExplicitSuper(Reference targetRef, const Class* cl);

public:
	// Process element selection; note that this object should be !requiresEvaluate() prior to call this method.
	void selectElement(Node* node, StringId name);

	// Add other restrictions
	void addArgument(Node* node, std::vector<ArgumentSpec> args);
	void addAssignment(Node* node, ArgumentSpec arg);
	void addVarArg(Node* node);

	// Get pointer to node which likely represents current value.
	// Valid whether before or after evaluate() calls
	Node* getNode() const {
		return node;
	}

	StringId getName() const {
		return hasName ? name : StringId::Invalid;
	}

	bool hasArguments() const {
		return hasArgs;
	}

	bool hasVarArg() const {
		return hasVarArgOp;
	}

	// Check whether this object is reference and should be evaluated.
	bool requiresEvaluate() const;

	// Find reference to object or method
	EvaluationResult evaluate(bool raiseExceptionIfVariableNotFound = true);
	EvaluationResult evaluateAsPackageInitCall();

	Type getType() const {
		return type;
	}

	// Check whether this object contains full information to find referred object or methods.
	bool isComplete() const;

	bool hasSource() const;

	Reference getSourceRef() const {
		assert(hasSource());
		return ref(StringId::SourceObject);
	}

	bool hasRef() const;

	Reference getRef() const {
		assert(hasRef());
		return targetRef;
	}

	size_t getPrimaryIndex() const {
		assert(type == Type::ObjectRef);
		return primaryIndex;
	}

	// Get frameIndex. SIZE_MAX for methods, !SIZE_MAX for inner methods
	size_t getFrameIndex() const {
		assert(type == Type::FrameMethod);
		return frameIndex;
	}

	bool hasPackage() const;

	Package* getPackage() const {
		assert(hasPackage());
		return package;
	}

	bool hasClass() const;

	const Class* getClass() const {
		assert(hasClass());
		return cl;
	}

	bool hasMethods() const;
	const MethodTable* getMethodTable() const;
	const Method* getRefMethod() const;
	const Method* getSetMethod() const;
	bool methodHasArguments() const;
	const NativeMethod* getNativeMethod() const;

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS_WITH_THREAD(TemporaryObject);
	CHATRA_DECLARE_SERIALIZE_OBJECT_REFS_METHODS;

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable) const override;
#endif // !NDEBUG
};


class TemporaryTuple : public ObjectBase, public PreDefined<TemporaryTuple, StringId::TemporaryTuple> {
private:
	Thread& thread;
	size_t frameIndex = SIZE_MAX;
	Node* node = nullptr;
	size_t valuesTop = 0;
	std::vector<std::pair<Tuple::Key, std::vector<TemporaryObject*>>> values;
	size_t delimiterPosition = 0;

public:
	TemporaryTuple(Storage& storage, Thread& thread) noexcept;
	void initialize(size_t frameIndex, Node* node);
	void clear();
	std::vector<TemporaryObject*> exportAll();

	Node* getNode() const;
	size_t tupleSize() const;
	const Tuple::Key& key(size_t position) const;
	void capture(Tuple::Key key);
	void copyFrom(Tuple& source);
	void copyFrom(Array& source);
	void copyFrom(Dict& source, Node* node);
	void extract(size_t position);

	ArgumentMatcher toArgumentMatcher() const;

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS_WITH_THREAD(TemporaryTuple);
	CHATRA_DECLARE_SERIALIZE_OBJECT_REFS_METHODS;
};


class TupleAssignmentMap : public ObjectBase, public PreDefined<TupleAssignmentMap, StringId::TupleAssignmentMap> {
public:
	struct Element {
		size_t destIndex = 0;
		std::vector<size_t> sourceIndexes;
		ArgumentDef::Type type = ArgumentDef::Type::List;

	public:
		Element(size_t destIndex, size_t sourceIndex, ArgumentDef::Type type) noexcept
				: destIndex(destIndex), sourceIndexes({sourceIndex}), type(type) {}
		Element(size_t destIndex, std::vector<size_t> sourceIndexes, ArgumentDef::Type type) noexcept
				: destIndex(destIndex), sourceIndexes(std::move(sourceIndexes)), type(type) {}

		CHATRA_DECLARE_SERIALIZE_METHODS(Element);
	};

	std::vector<Element> map;

public:
	TupleAssignmentMap(Storage& storage, std::vector<Element> map) noexcept
			: ObjectBase(storage, typeId_TupleAssignmentMap, getClassStatic()), map(std::move(map)) {}

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(TupleAssignmentMap);
};


class FunctionObject : public ObjectBase, public PreDefined<FunctionObject, StringId::FunctionObject> {
public:
	Package* package;
	std::vector<std::tuple<ScopeType, Node*, const Class*>> scopes;  // Class, Method or Block; reverse order
	ScopeType scopeType;
	const MethodTable* methodTable;
	StringId name;

public:
	FunctionObject(Storage& storage, Thread& thread, Package* package, size_t parentIndex,
			const MethodTable* methodTable, StringId name) noexcept;
	FunctionObject(Storage& storage, Thread& thread, Package* package, Reference ref,
			const MethodTable* methodTable, StringId name) noexcept;

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(FunctionObject);
};


class WaitContext : public Object {
	friend bool WaitContextEventWatcher(void* tag);

private:
	struct Target {
		WaitContext* ct = nullptr;
		size_t index;
		std::string key;
		EventObject* eventObject;

	public:
		Target(size_t index, std::string key, EventObject* eventObject) noexcept
				: index(index), key(std::move(key)), eventObject(eventObject) {}
	};

	Thread& thread;
	Reference self;

	unsigned waitingId = 0;
	size_t callerFrame = 0;

	SpinLock lockTriggered;
	std::vector<std::unique_ptr<Target>> targets;
	size_t remainingCount = 0;

public:
	WaitContext(Storage& storage, Reference self, Thread& thread, size_t targetCount) noexcept;

	void deploy(unsigned waitingId, size_t callerFrame,
			std::vector<std::tuple<size_t, std::string, Reference>> targets);

	CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS_WITH_THREAD(WaitContext);
	CHATRA_DECLARE_SERIALIZE_OBJECT_REFS_METHODS;
};

bool WaitContextEventWatcher(void* tag);
EventObject* derefAsEventObject(Reference ref);


class NativeEventObjectImp : public NativeEventObject {
private:
	RuntimeImp& runtime;
	unsigned waitingId;

public:
	NativeEventObjectImp(RuntimeImp& runtime, unsigned waitingId) noexcept;
	unsigned getWaitingId() const { return waitingId; }

	void unlock() override;
};


enum class LockType {
	Hard,
	Soft,
	Temporary
};

constexpr size_t lockTypes = 3;

class Lock {
private:
	std::unordered_set<Reference> refs[lockTypes];
	bool methodCall = false;

public:
#ifdef CHATRA_DEBUG_LOCK
	Thread* thread = nullptr;
	size_t frameIndex = SIZE_MAX;
	unsigned id = std::numeric_limits<unsigned>::max();
	std::unordered_set<Reference> suspendedRefs;
	mutable bool saved = false;

	void setFrame(Frame* currentFramePtr);
	void outputHeader(std::FILE* fp);
	static void outputReference(std::FILE* fp, Reference ref, LockType lockType);
	void outputLog(const char* verb);
	void outputLog(const char* verb, Reference ref, LockType lockType);
	void audit();
#else
	void setFrame(Frame* currentFramePtr) { (void)currentFramePtr; }
#endif

public:
	Lock() noexcept {}
	~Lock();

	bool has(Reference ref, LockType lockType) const;
	void add(Reference ref, LockType lockType);
	void moveFromSoftToTemporary(Reference ref);
	void release(Reference ref, LockType lockType);
	void releaseIfExists(Reference ref, LockType lockType);
	void releaseAll();
	void releaseIfExists(Reference ref);
	void moveTemporaryToSoftLock();
	void prepareToMethodCall();
	bool isInMethodCallState() const;
	std::vector<Reference> getTemporary() const;
	void temporaryLockRestored();

	CHATRA_DECLARE_SERIALIZE_METHODS(Lock);
};


class Frame {
public:
	Thread& thread;
	Package& package;
	size_t parentIndex;
	Scope* scope;
	std::unique_ptr<Scope> frameScope;
	const Class* cl = nullptr;
	size_t popCount;
	bool captured = false;

	bool isLoop = false;
	StringId label = StringId::Invalid;
	size_t phaseOnContinue = 0;

	Lock* lock = nullptr;
	std::unique_ptr<Lock> frameLock;

	Node* node = nullptr;
	bool hasMethods = false;
	const MethodTable* methods = nullptr;  // inner functions only

	size_t phase = 0;
	std::vector<std::pair<Node*, size_t>> stack;  // second = Phase
	std::vector<size_t> subStack;
	std::vector<TemporaryObject*> values;
	TemporaryObject* exception = nullptr;
	Node* exceptionNode = nullptr;
	TemporaryObject* caughtException = nullptr;

	std::vector<TemporaryTuple*> temporaryTuples;

public:
	TemporaryObject* allocateTemporary();
	TemporaryObject* pushTemporary();
	void push(TemporaryObject* value);
	void push(size_t position, TemporaryObject* value);
	void pushEmptyTuple();
	void pop();
	void pop(size_t first, size_t last);
	void popAll();
	void recycle(TemporaryObject* value);
	void duplicateTop();

	TemporaryTuple* pushTemporaryTuple();
	void popTemporaryTuple();

	void clearAllTemporaries();

	Reference getSelf();

public:
	Frame(Thread& thread, Package& package, size_t parentIndex,
			Scope* scope, size_t popCount = 1) noexcept;
	Frame(Thread& thread, Package& package, size_t parentIndex,
			ScopeType type, Node* node, size_t popCount = 1) noexcept;

	// Constructor for ScopeType::Class
	Frame(Thread& thread, Package& package, size_t parentIndex,
			const Class* cl, Reference instanceRef, size_t popCount = 1) noexcept;

	Frame(Thread& thread, Package& package, Reader& r) noexcept;
	CHATRA_DECLARE_SERIALIZE_METHODS_WITHOUT_CONSTRUCTOR(Frame);
};

constexpr size_t residentialFrameCount = 2;

namespace Phase {
	constexpr size_t ScriptRoot_Initial = SIZE_MAX - 3;
	constexpr size_t ScriptRoot_InitCall0 = SIZE_MAX - 2;
	constexpr size_t ScriptRoot_InitCall1 = ScriptRoot_InitCall0 + 1;

	constexpr size_t IfGroup_Base0 = SIZE_MAX >> 1U;
	constexpr size_t IfGroup_Base1 = IfGroup_Base0 + 1;

	constexpr size_t Switch_Base0 = SIZE_MAX - 2;
	constexpr size_t Switch_Base1 = Switch_Base0 + 1;

	constexpr size_t Case_Base = SIZE_MAX - 3;
	constexpr size_t Case_Check0 = Case_Base + 1;
	constexpr size_t Case_Check1 = Case_Base + 2;

	constexpr size_t For_Begin = SIZE_MAX - 6;
	constexpr size_t For_CallIterator = For_Begin + 1;
	constexpr size_t For_PrepareLoop = For_Begin + 2;
	constexpr size_t For_BeginLoop = For_Begin + 3;
	constexpr size_t For_AssignLoopVariables = For_Begin + 4;
	constexpr size_t For_EnterLoop = For_Begin + 5;
	constexpr size_t For_Continue = For_BeginLoop;

	constexpr size_t While_Continue = SIZE_MAX - 3;
	constexpr size_t While_Condition0 = SIZE_MAX - 2;
	constexpr size_t While_Condition1 = While_Condition0 + 1;
}


class TransferRequest {
public:
	Requester requester;
	Reference ref;
	LockType lockType;

public:
	TransferRequest(Requester requester, Reference ref, LockType lockType) noexcept
			: requester(requester), ref(std::move(ref)), lockType(lockType) {}

	explicit TransferRequest(Reader& r) noexcept;
	CHATRA_DECLARE_SERIALIZE_METHODS_WITHOUT_CONSTRUCTOR(TransferRequest);
};


template <class Type>
class ObjectPool {
private:
	Thread& thread;
	std::vector<Type*> recycled;

#ifdef CHATRA_TRACE_TEMPORARY_ALLOCATION
	mutable bool saved = false;
	std::unordered_map<size_t, unsigned> history;
	std::vector<Type*> allocated;
	unsigned allocationCount = 0;
	size_t breakObjectIndex = SIZE_MAX;
	unsigned breakAllocationCount = 0;
#endif

public:
	explicit ObjectPool(Thread& thread) noexcept : thread(thread) {}

	template <typename Allocator>
	Type* allocate(Allocator allocator);

	void recycle(Type* value);

	template <class InputIterator>
	void recycle(InputIterator first, InputIterator last);

	void save(Writer& w) const;

	template <typename Restore>
	void restore(Reader& r, Restore restore);

#ifdef CHATRA_TRACE_TEMPORARY_ALLOCATION
	void setBreakPoint(size_t objectIndex, unsigned allocationCount);
	~ObjectPool();
#endif
};


class Thread : public IdType<Requester, Thread>, public IErrorReceiver {
	friend class Frame;

public:
	RuntimeImp& runtime;
	Instance& instance;

	static bool initialized;
	static Node expressionMarkerNode;
	static Node initializePackageNode;
	static Node evaluateTupleNode;
	static Node defaultConstructorNode;
	static Node initializeVarsNode;
	static Node popValueNode;
	static Node postConstructorNode;
	static Node postPrefixUnaryOperatorNode;
	static Node restoreTemporaryLockNode;
	static Node returnFromAsyncNode;
	static Node processFor_CallNextNode;
	static Node varStatementMarkerNode;
	static std::array<Node, NumberOfOperators> operatorNode;
	static std::unordered_map<NodeType, Node*> specialNodes0;
	static std::unordered_map<Operator, Node*> specialNodes1;

	std::atomic<bool> hasNewSTable = {true};
	std::shared_ptr<StringTable> sTableHolder;  // locked by RuntimeImp::lockSTable
	const StringTable* sTable = nullptr;

	std::unique_ptr<Scope> scope;
	Tuple* emptyTuple = nullptr;

	// Frame structure:
	// (r) = reference to object (has StringId::Self), (c) = captured scope (has StringId::Captured)
	//
	// ScriptRoot: Global, Thread, Package, ScriptRoot
	//
	// Global function: Global, Thread, Package, Method
	// Inner global function: Global, Thread, Package, Method, [Block, ...] InnerMethod
	// Captured inner global function: Global, Thread, Package, Method(c), [Block(c), ...] InnerMethod
	//
	// Class method: Global, Thread, Package, Class(r), Method
	// Inner class method: Global, Thread, Package, Class(r), Method, [Block, ...] InnerMethod
	// Captured inner class method: Global, Thread, Package, Class(r), Method(c), [Block(c), ...] InnerMethod
	std::vector<Frame> frames;

	std::atomic<bool> hasTransferReq = {false};
	SpinLock lockTransferReq;
	std::deque<TransferRequest> transferReq;
	std::deque<TransferRequest> transferReqCopy;

	ObjectPool<TemporaryObject> temporaryObjectPool;
	ObjectPool<TemporaryTuple> temporaryTuplePool;

	std::thread::id nativeCallThreadId;
	SpinLock lockNative;
	size_t callerFrame = 0;
	bool pauseRequested = false;

	std::vector<std::tuple<std::string, unsigned, std::string, size_t>> errors;

	MethodTableCache methodTableCache;

	bool stackOverflowWarned = false;

private:
	static void set(Node& node, NodeType type);
	static void set(Node& node, Operator op);

	void captureStringTable();
	void scanInnerFunctions(IErrorReceiver* errorReceiver, const StringTable* sTable);
	bool parse();

	Node* getExceptionNode();
	void raiseException(const RuntimeException& ex);
	void raiseException(TemporaryObject* exValue);
	void consumeException();
	void checkIsValidNode(Node* node);

	void sendTransferReq(Requester requester, Reference ref, LockType lockType, Requester holder);
	bool lockReference(Reference ref, LockType lockType);
	bool lockReference(TemporaryObject* value);
	void checkTransferReq();

	Reference allocateReference(StringId sid);
	TemporaryObject* allocateTemporary();
	TemporaryTuple* allocateTemporaryTuple();
	void recycleTemporary(TemporaryObject* value);
	void recycleTemporaryTuple(TemporaryTuple* value);

	template <class InputIterator>
	void recycleTemporary(InputIterator first, InputIterator last) {
		temporaryObjectPool.recycle(first, last);
	}

	template <class InputIterator>
	void recycleTemporaryTuple(InputIterator first, InputIterator last) {
		std::for_each(first, last, [&](TemporaryTuple* value) {
			auto objects = value->exportAll();
			recycleTemporary(objects.cbegin(), objects.cend());
		});
		temporaryTuplePool.recycle(first, last);
	}

	void checkStackOverflow(Node* node);
	Frame& pushBlock(Node* node, size_t phase);
	Frame& pushBlock(Node* node, size_t phase, StringId label, size_t phaseOnContinue);
	void pop();

	enum class FinallyBlockScheme {
		Through, Run, IncludesLast
	};
	void pop(size_t targetSize, FinallyBlockScheme scheme);

	void constructFrames(Package* package, Node* node, size_t parentIndex);
	void constructFrames(Package* package, Node* node, const Class* cl, Reference ref);
	void buildPrimitive(Node* node, const Class* cl, Reference rSource, Reference rTarget);
	void methodCall(bool hasSetArg);
	void operatorMethodCall(const OperatorMethod* method);
	void methodCallViaFunctionObject(bool hasSetArg, const Method* method = nullptr);
	void importMethodArguments(const Method* method, TemporaryObject* argsValue,
			TemporaryObject* setArgValue = nullptr);

	template <typename PostProcess>
	bool invokeNativeMethod(size_t callerFrame, ObjectBase* object,
			TemporaryObject* methodValue, TemporaryObject* argsValue, PostProcess postProcess);

	void returnStatement(TemporaryObject* returnValue, bool runFinallyBlock);
	void breakStatement(Node* node);
	void continueStatement(Node* node);

	void convertTopToScalar(Node* node);
	void convertTopToScalar(Node* node, size_t subNodeIndex);
	bool isNameOrEvaluate(Node* node, size_t subNodeIndex);
	bool getBoolOrThrow(Node* node, size_t subNodeIndex);  // this contains convertTopToScalar()
	std::string getStringOrThrow(Node* node, size_t subNodeIndex);  // this contains convertTopToScalar()
	StringId getStringIdOrThrow(Node* node, size_t subNodeIndex);
	const Class* isClassOrEvaluate(Node* node, size_t subNodeIndex);
	const Class* getClassOrThrow(Node* node, size_t subNodeIndex);

	void checkTupleAsArgOrThrow(Node* node, const Tuple& tuple);
	void checkTupleAsContainerValueOrThrow(Node* node, const Tuple& tuple);

	enum class EvaluateValueMode {
		Value, Class, ElementSelectionLeft
	};
	enum class EvaluateValueResult {
		Next, Suspend, StackChanged, FunctionObject
	};
	EvaluateValueResult evaluateTemporaryObject(TemporaryObject* value, bool allocateIfNotFound);
	EvaluateValueResult evaluateValue(EvaluateValueMode mode);
	EvaluateValueResult evaluateTuple();
	EvaluateValueResult evaluateAndAllocateForAssignment(Node* node, TemporaryObject* value);

	std::string getClassName(const Class* cl);

	template <typename PrBool, typename PrInt, typename PrFloat>
	bool standardUnaryOperator(PrBool prBool, PrInt prInt, PrFloat prFloat);

	template <typename PrBool, typename PrInt, typename PrFloat>
	bool unaryAssignmentOperator(bool prefix, PrBool prBool, PrInt prInt, PrFloat prFloat);

	template <typename Process>
	bool binaryOperator(Process process);

	template <typename PrBool, typename PrInt, typename PrFloat>
	bool standardBinaryOperator(PrBool prBool, PrInt prInt, PrFloat prFloat);

	template <typename PrInt>
	bool shiftOrBitwiseOperator(PrInt prInt);

	template <typename PrBool, typename PrInt, typename PrFloat>
	bool comparisonOperator(PrBool prBool, PrInt prInt, PrFloat prFloat);

	template <bool equal>
	bool equalityOperator();

	template <typename PrEvaluateRight, typename PrBoolLeft, typename PrBoolRight>
	bool logicalOperator(PrEvaluateRight prEvaluateRight, PrBoolLeft prBoolLeft, PrBoolRight prBoolRight);

	template <typename PrBool>
	bool logicalOperator(PrBool prBool);

	bool expressionOperator();
	bool containerOperator();
	bool callOperator();
	bool elementSelectionOperator();
	bool functionObjectOperator();
	bool asyncOperator();
	bool varArgOperator();
	bool instanceOfOperator();
	bool conditionalOperator();
	bool tupleOperator();
	bool assignmentOperator();
	bool defineAndAssignmentOperator();
	bool assignmentOperator(Operator op);

	bool initializePackage();
	bool initializeVars();
	bool popValue();
	bool postConstructor();
	bool restoreTemporaryLock();
	bool returnFromAsync();

	void prepareExpression(Node* node);
	bool resumeExpression();

	bool processFor_CallNext();
	void processFor();
	void processSwitchCase(bool enter);
	bool processSwitchCase();

	void processFinalizer();

public:
	Frame* findClassFrame();

	void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override;
	void emitError(StringId exceptionName, Node* exceptionNode);

public:
	static Node* refSpecialNode(NodeType type, Operator op = defaultOp);

	Thread(RuntimeImp& runtime, Instance& instance) noexcept;
	void postInitialize();

	unsigned requestToPause();

	void finish();

	void run();

	Thread(RuntimeImp& runtime, Instance& instance, Reader& r) noexcept;
	TemporaryObject* restoreTemporary(Reader& r);
	TemporaryTuple* restoreTemporaryTuple(Reader& r);
	CHATRA_DECLARE_SERIALIZE_METHODS_WITHOUT_CONSTRUCTOR(Thread);
	void postInitialize(Reader& r);
};


class Instance : public IdType<InstanceId, Instance> {
public:
	PackageId primaryPackageId;
	SpinLock lockThreads;
	std::unordered_map<Requester, std::unique_ptr<Thread>> threads;

public:
	Instance() noexcept : primaryPackageId(enum_max<PackageId>::value) {}
	explicit Instance(PackageId primaryPackageId) noexcept : primaryPackageId(primaryPackageId) {}
};


class PackageObject : public ObjectBase {
public:
	PackageObject(Storage& storage, const Class* cl) noexcept
			: ObjectBase(storage, typeId_PackageObject, cl) {}

	// CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(PackageObject);
	bool save(Writer& w) const { w.out(getClass()); return false; }
	explicit PackageObject(const Class* cl) noexcept : ObjectBase(typeId_PackageObject, cl) {}
};


class Package : public PackageInfo, public IdType<PackageId, Package>, public IClassFinder, public PackageContext {
public:
	RuntimeImp& runtime;
	std::atomic<bool> initialized = {false};
	bool temporaryInitialized = false;

	std::string name;
	std::unique_ptr<Scope> scope;  // PackageObject, PackageInitializer
	std::vector<std::shared_ptr<Line>> lines;
	bool fromHost = false;

	bool grouped = false;
	bool hasDefOperator = false;  // contains "def operator"

	SpinLock lockNode;
	std::shared_ptr<Node> node;
	std::vector<Thread*> threadsWaitingForNode;

	SpinLock lockInstances;
	std::unordered_map<InstanceId, std::unique_ptr<Instance>> instances;

	std::unique_ptr<Class> clPackage;
	ClassTable classes;

	std::unordered_set<StringId> imports;
	std::unordered_map<StringId, Package*> importsByName;
	std::vector<Package*> anonymousImports;

public:
	Package(RuntimeImp& runtime, std::string name, PackageInfo packageInfo, bool fromHost) noexcept;
	void postInitialize();

	std::shared_ptr<Node> parseNode(IErrorReceiver& errorReceiver, Node* node);

	bool requiresProcessImport(IErrorReceiver& errorReceiver, const StringTable* sTable, Node* node);
	Package& import(Node* node, PackageId targetPackageId);
	void build(IErrorReceiver& errorReceiver, const StringTable* sTable);
	void allocatePackageObject();

	const Class* findClass(StringId name) override;
	const Class* findPackageClass(StringId packageName, StringId name) override;

	Package* findPackage(StringId name);
	const std::vector<Package*>& refAnonymousImports();

	void pushNodeFrame(Thread& thread, Package& package, size_t parentIndex, ScopeType type, size_t popCount = 1);

	RuntimeId runtimeId() const override;
	std::vector<uint8_t> saveEvent(NativeEventObject* event) const override;
	NativeEventObject* restoreEvent(const std::vector<uint8_t>& stream) const override;

	void saveScripts(Writer& w) const;
	Package(RuntimeImp& runtime, Reader& r) noexcept;
	void restoreScripts(Reader& r);
	CHATRA_DECLARE_SERIALIZE_METHODS_WITHOUT_CONSTRUCTOR(Package);

#ifndef NDEBUG
	void dump(const std::shared_ptr<StringTable>& sTable);
#endif // !NDEBUG
};


class RuntimeImp : public Runtime, public IErrorReceiver, public IConcurrentGcConfiguration {
public:
	RuntimeId runtimeId;
	std::shared_ptr<IHost> host;
	bool multiThread = false;
	bool closed = false;
	bool parserErrorRaised = false;

	std::atomic<bool> attemptToShutdown = {false};

	std::shared_ptr<StringTable> primarySTable;  // locked by #parser
	SpinLock lockSTable;
	std::shared_ptr<StringTable> distributedSTable;

	ParserWorkingSet parserWs;  // locked by #parser

	std::shared_ptr<Storage> storage;
	std::atomic<int> gcWaitCount = {0};
	std::mutex mtGc;
	std::unique_ptr<Instance> gcInstance;
	std::unique_ptr<Thread> gcThread;

	IdPool<PackageId, Package> packageIds;
	IdPool<InstanceId, Instance> instanceIds;
	IdPool<Requester, Thread> threadIds;

	SpinLock lockScope;  // only used for adding WaitContexts
	std::unique_ptr<Scope> scope;  // Parser, FinalizerObjects, FinalizerTemporary, {WaitContext}
	std::vector<Reference> recycledRefs;

	std::mutex mtLoadPackage;
	SpinLock lockPackages;
	std::unordered_map<PackageId, std::unique_ptr<Package>> packages;
	std::unordered_map<std::string, PackageId> packageIdByName;

	ClassTable classes;  // Embedded classes
	MethodTable methods;  // System global functions only
	AsyncOperatorTable operators;

	std::deque<Thread*> queue;

	// [Single thread]
	SpinLock lockQueue;

	// [Multi-thread]
	std::mutex mtQueue;
	std::condition_variable cvQueue;
	std::condition_variable cvShutdown;
	unsigned targetWorkerThreads = 0;
	unsigned nextId = 0;
	std::unordered_map<unsigned, std::unique_ptr<std::thread>> workerThreads;

	std::atomic<bool> hasWaitingThreads = {false};
	SpinLock lockWaitingThreads;
	std::unordered_map<unsigned, Thread*> waitingThreads;
	std::vector<unsigned> recycledWaitingIds;

	SpinLock lockTimers;
	std::unordered_map<std::string, std::unique_ptr<Timer>> timers;
	std::vector<Timer*> idToTimer;
	std::unordered_map<unsigned, std::tuple<std::string, int64_t>> sleepRequests;  // [waitingId]

	// Finalizer
	Thread* finalizerThread = nullptr;
	SpinLock lockFinalizerTemporary;

	MethodTableCache methodTableCache;  // by restoreEntities() only

private:
	void launchStorage();
	void launchFinalizerThread();
	void launchSystem(unsigned initialThreadCount);
	void shutdownThreads();
	void shutdownTimers();

	std::unordered_map<const Class*, size_t> getClassMap() const;

	void saveEntityFrames(Writer& w);
	void saveEntityMap(Writer& w);
	void saveStorage(Writer& w);
	void saveState(Writer& w);

	void restoreEntityFrames(Reader& r);
	void restoreEntityMap(Reader& r);
	void restoreEntities(Reader& r, PackageId packageId, Node* node);
	void restoreStorage(Reader& r);
	void restoreState(Reader& r);

	void reactivateFinalizerThread();
	void reactivateThreads();
	void reactivateTimers();

public:
	void checkGc();
	void fullGc();
	size_t stepCountForScope(size_t totalScopeCount) override;
	size_t stepCountForMarking(size_t totalObjectCount) override;
	size_t stepCountForSweeping(size_t totalObjectCount) override;

	Thread& createThread(Instance& instance, Package& package, Node* node = nullptr);
	void enqueue(Thread* thread);

	unsigned pause(Thread* thread);
	void resume(unsigned waitingId);

	void issueTimer(unsigned waitingId, Timer& timer, Time time);

	static std::string formatOrigin(const std::string& fileName, unsigned lineNo);
	static std::string formatError(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args, bool outputExtraMessagePlaceholder = false);

	void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override;

	void outputError(const std::string& message);

public:
	explicit RuntimeImp(std::shared_ptr<IHost> host) noexcept;

	void initialize(unsigned initialThreadCount);
	void initialize(unsigned initialThreadCount, const std::vector<uint8_t>& savedState);

	// Interface to Reader
	Node* restorePackageNode(Reader& r, PackageId packageId, bool initialNode);
	void restoreNode(Reader& r, PackageId packageId, Node* node);
	const MethodTable* restoreMethodTable(PackageId packageId, Node* node);
	const MethodTable* refMethodTable() { return &methods; }

	~RuntimeImp() override;

	std::vector<uint8_t> shutdown(bool save) override;
	void setWorkers(unsigned threadCount) override;
	bool handleQueue() override;
	void loop() override;
	PackageId loadPackage(const Script& script) override;
	PackageId loadPackage(const std::vector<Script>& scripts) override;
	PackageId loadPackage(const std::string& packageName) override;
	InstanceId run(PackageId packageId) override;
	bool isRunning(InstanceId instanceId) override;
	void stop(InstanceId instanceId) override;
	TimerId addTimer(const std::string& name) override;
	void increment(TimerId timerId, int64_t step) override;

#ifndef NDEBUG
	size_t objectCount() { return storage->objectCount(); }
	void dump();
#endif // !NDEBUG
};

void initializeEmbeddedFunctions();  // should be called after initializeEmbeddedClasses()
void registerEmbeddedFunctions(MethodTable& methods, AsyncOperatorTable& operators);
const std::unordered_map<StringId, Node*>& refNodeMapForEmbeddedFunctions();

void outputLog(Thread& thread, const std::string& message);
void native_log(CHATRA_NATIVE_ARGS);
void native_dump(CHATRA_NATIVE_ARGS);
void native_gc(CHATRA_NATIVE_ARGS);
void native_time(CHATRA_NATIVE_ARGS);
void native_sleep(CHATRA_NATIVE_ARGS);
// void native_wait(CHATRA_NATIVE_ARGS);  // -> Classes.h
void native_type(CHATRA_NATIVE_ARGS);
void native_objectId(CHATRA_NATIVE_ARGS);

#ifndef NDEBUG
void enableStdout(bool enabled);
void setTestMode(const std::string& testMode);
void beginCheckScript();
void endCheckScript();
bool showResults();
void waitUntilFinished();
#endif // !NDEBUG

void native_check(CHATRA_NATIVE_ARGS);
void native_checkCmd(CHATRA_NATIVE_ARGS);
void native_incrementTestTimer(CHATRA_NATIVE_ARGS);

void nativeCall(CHATRA_NATIVE_ARGS);


#ifdef CHATRA_TRACE_TEMPORARY_ALLOCATION
template <class Type>
template <typename Allocator>
Type* ObjectPool<Type>::allocate(Allocator allocator) {
	if (recycled.empty()) {
		auto* ret = allocator();

		if (ret->getObjectIndex() == breakObjectIndex && allocationCount == breakAllocationCount)
			std::printf("Target TemporaryObject is allocated\n");

		history.emplace(ret->getObjectIndex(), allocationCount++);
		allocated.emplace_back(ret);
		return ret;
	}

	auto* ret = recycled.back();
	recycled.pop_back();

	if (ret->getObjectIndex() == breakObjectIndex && allocationCount == breakAllocationCount)
		std::printf("Target TemporaryObject is allocated\n");

	history.at(ret->getObjectIndex()) = allocationCount++;
	return ret;
}

template <class Type>
void ObjectPool<Type>::recycle(Type* value) {
	assert(std::find(allocated.cbegin(), allocated.cend(), value) != allocated.cend());
	assert(std::find(recycled.cbegin(), recycled.cend(), value) == recycled.cend());
	value->clear();
	recycled.emplace_back(value);
}

template <class Type>
template <class InputIterator>
void ObjectPool<Type>::recycle(InputIterator first, InputIterator last) {
	for (auto it = first; it != last; it++) {
		assert(std::find(allocated.cbegin(), allocated.cend(), *it) != allocated.cend());
		assert(std::find(recycled.cbegin(), recycled.cend(), *it) == recycled.cend());
	}
	std::for_each(first, last, [](Type* value) { value->clear(); });
	recycled.insert(recycled.end(), first, last);
}

template <class Type>
void ObjectPool<Type>::save(Writer& w) const {
	auto objectMap = thread.scope->getObjectMap();
	w.out(recycled, [&](const Type* value) { w.out(objectMap.at(value)); });
	w.out(history, [&](const typename decltype(history)::value_type& e) {
		w.out(e.first);
		w.out(e.second);
	});
	w.out(allocated, [&](const Type* value) { w.out(objectMap.at(value)); });
	w.out(allocationCount);
	saved = true;
}

template <class Type>
template <typename Restore>
void ObjectPool<Type>::restore(Reader& r, Restore restore) {
	r.inList([&]() { recycled.emplace_back(restore()); });
	r.inList([&]() {
		auto objectIndex = r.read<size_t>();
		auto count = r.read<unsigned>();
		history.emplace(objectIndex, count);
	});
	r.inList([&]() { allocated.emplace_back(restore()); });
	r.in(allocationCount);
}

template <class Type>
void ObjectPool<Type>::setBreakPoint(size_t objectIndex, unsigned allocationCount) {
	breakObjectIndex = objectIndex;
	breakAllocationCount = allocationCount;
}

template <class Type>
ObjectPool<Type>::~ObjectPool() {
	if (saved)
		return;

	int leaked = static_cast<int>(allocated.size()) - static_cast<int>(recycled.size());
	std::printf("Thread #%u: temporaryValueCount = %u (%d leaked)\n",
			static_cast<unsigned>(thread.getId()), static_cast<unsigned>(allocated.size()), leaked);

	if (leaked > 0) {
		for (auto* value : allocated) {
			if (std::find(recycled.cbegin(), recycled.cend(), value) != recycled.cend())
				continue;
			std::printf("leaked object: objectIndex = %u, allocationCount = %u\n",
					static_cast<unsigned>(value->getObjectIndex()), history.at(value->getObjectIndex()));
			value->dump(thread.runtime.primarySTable);
		}
	}
	assert(leaked == 0);
}

#else // CHATRA_TRACE_TEMPORARY_ALLOCATION

template <class Type>
template <typename Allocator>
Type* ObjectPool<Type>::allocate(Allocator allocator) {
	if (recycled.empty())
		return allocator();
	auto* ret = recycled.back();
	recycled.pop_back();
	return ret;
}

template <class Type>
void ObjectPool<Type>::recycle(Type* value) {
	value->clear();
	recycled.emplace_back(value);
}

template <class Type>
template <class InputIterator>
void ObjectPool<Type>::recycle(InputIterator first, InputIterator last) {
	std::for_each(first, last, [](Type* value) { value->clear(); });
	recycled.insert(recycled.end(), first, last);
}

template <class Type>
void ObjectPool<Type>::save(Writer& w) const {
	auto objectMap = thread.scope->getObjectMap();
	w.out(recycled, [&](const Type* value) { w.out(objectMap.at(value)); });
}

template <class Type>
template <typename Restore>
void ObjectPool<Type>::restore(Reader& r, Restore restore) {
	r.inList([&]() { recycled.emplace_back(restore()); });
}

#endif // CHATRA_TRACE_TEMPORARY_ALLOCATION


}  // namespace chatra

#endif //CHATRA_RUNTIME_H
