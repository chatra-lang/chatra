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

#ifndef CHATRA_SERIALIZE_H
#define CHATRA_SERIALIZE_H

#include "chatra.h"
#include "Internal.h"

namespace chatra {

struct Node;
class Reference;
class Class;
struct Method;
struct OperatorMethod;
class MethodTable;
class AsyncOperatorTable;
class Thread;
class Instance;
class Package;
class RuntimeImp;

#ifdef NDEBUG

#define CHATRA_SAVE_TYPE_TAG(tag)  (void)0
#define CHATRA_RESTORE_TYPE_TAG(tag)  (void)0
#define CHATRA_REGISTER_POINTER(pointer, type)  registerPointer(pointer)
#define CHATRA_OUT_POINTER(pointer, type)  outPointer(pointer)
#define CHATRA_READ_UNIQUE(className)  readUnique<className>()
#define CHATRA_READ_RAW(className)  readRaw<className>()

#else  // NDEBUG

enum class TypeTag : unsigned {
	Node = 0, Class = 1, Method = 2, MethodTable = 3, Reference = 4,
	Bool = 5, Integer = 6, Float = 7, Enum = 8, String = 9,
	Pointer = 10,
	Serializable = 11,
	List = 12,
	ByteStream = 13,
	IdType = 14,
};

#define CHATRA_SAVE_TYPE_TAG(tag)  saveTypeTag(TypeTag::tag)
#define CHATRA_RESTORE_TYPE_TAG(tag)  restoreTypeTag(TypeTag::tag)

enum class PointerType : unsigned {
	ReferenceGroup = 0, ReferenceNode = 1, Scope = 2, Lock = 3,
};

#define CHATRA_REGISTER_POINTER(pointer, type)  registerPointer(pointer, PointerType::type)
#define CHATRA_OUT_POINTER(pointer, type)  outPointer(pointer, PointerType::type)
#define CHATRA_READ_UNIQUE(className)  readUnique<className>(PointerType::className)
#define CHATRA_READ_RAW(className)  readRaw<className>(PointerType::className)

#endif  // NDEBUG


class Writer final {
private:
#ifndef NDEBUG
	RuntimeImp& runtime;
#endif  // !NDEBUG

	std::vector<uint8_t>* buffer;
	std::unordered_map<std::string, std::vector<uint8_t>> chunks;

#ifdef NDEBUG
	std::unordered_map<const void*, size_t> pointerMap;
#else
	std::unordered_map<const void*, std::tuple<size_t, PointerType>> pointerMap;
#endif

	bool entityMapEnabled = false;
	std::unordered_map<const Class*, size_t> classMap;
	std::unordered_map<const Node*, ptrdiff_t> rootMap;
	std::unordered_map<const Node*, const Node*> parentMap;

	template <class Type>
	Writer& outRawInteger(Type value) {
		using SignedType = typename std::make_signed<Type>::type;
		auto valueSigned = static_cast<SignedType>(value);
		int sign = (valueSigned >= 0 ? 1 : -1);
		auto t = (sign > 0 ? static_cast<uint64_t>(value) : static_cast<uint64_t>(~static_cast<int64_t>(value)));
		if (sign > 0 && t < 128) {
			buffer->emplace_back(static_cast<uint8_t>(t));
			return *this;
		}
		for (;;) {
			buffer->emplace_back(static_cast<uint8_t>((t & 0x7FU) | 0x80U));
			t >>= 7U;
			if (t < 64) {
				buffer->emplace_back(static_cast<uint8_t>(t | (sign > 0 ? 0U : 0x40U)));
				break;
			}
		}
		return *this;
	}

#ifndef NDEBUG
	void saveTypeTag(TypeTag tag);
#endif  // !NDEBUG

public:
	explicit Writer(RuntimeImp& runtime) noexcept
#ifndef NDEBUG
			: runtime(runtime)
#endif  // !NDEBUG
	{
		(void)runtime;
		buffer = &chunks[""];
#ifdef NDEBUG
		pointerMap.emplace(nullptr, 0);
#else
		pointerMap.emplace(nullptr, std::make_tuple(0, static_cast<PointerType>(0)));
#endif
	}

	std::vector<uint8_t> getBytes(unsigned version);

	Writer& select(const std::string& chunk);

	void setEntityMap(
			std::unordered_map<const Class*, size_t> classMap,
			std::unordered_map<const Node*, ptrdiff_t> rootMap,
			std::unordered_map<const Node*, const Node*> parentMap);

	Writer& saveResync(int tag);

	Writer& outNode(const Node* node);
	Writer& outClass(const Class* cl);
	Writer& outMethod(const Method* method);
	Writer& outMethodTable(const MethodTable* methodTable);
	Writer& outReference(const Reference& ref);

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Node*>::value)>
	Writer& out(Type node) { return outNode(node); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Class*>::value)>
	Writer& out(Type cl) { return outClass(cl); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Method*>::value)>
	Writer& out(Type method) { return outMethod(method); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, MethodTable*>::value)>
	Writer& out(Type methodTable) { return outMethodTable(methodTable); }

	Writer& out(Reference& ref) { return outReference(ref); }
	Writer& out(const Reference& ref) { return outReference(ref); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	Writer& out(Type value) {
		CHATRA_SAVE_TYPE_TAG(Bool);
		return out(value ? 1 : 0);
	}

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	Writer& out(Type value) {
		CHATRA_SAVE_TYPE_TAG(Integer);
		return outRawInteger<Type>(value);
	}

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	Writer& out(Type value) {
		CHATRA_SAVE_TYPE_TAG(Float);
		auto* ptr = reinterpret_cast<const uint8_t*>(&value);
		buffer->insert(buffer->end(), ptr, ptr + sizeof(value));
		return *this;
	}

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	Writer& out(Type value) {
		CHATRA_SAVE_TYPE_TAG(Enum);
		return out(static_cast<typename std::underlying_type<Type>::type>(value));
	}

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	Writer& out(const Type& value) {
		CHATRA_SAVE_TYPE_TAG(String);
		out(value.size());
		for (char c : value)
			buffer->emplace_back(static_cast<uint8_t>(c));
		return *this;
	}

#ifdef NDEBUG
	template <class Type>
	Writer& outPointer(const Type* value) {
		CHATRA_SAVE_TYPE_TAG(Pointer);
		assert(pointerMap.count(value) != 0);
		return out(pointerMap.at(value));
	}
#else  // NDEBUG
	template <class Type>
	Writer& outPointer(const Type* value, PointerType type) {
		CHATRA_SAVE_TYPE_TAG(Pointer);
		assert(pointerMap.count(value) != 0);
		if (value == nullptr) {
			out(static_cast<size_t>(0));
			return *this;
		}
		auto& e = pointerMap.at(value);
		assert(std::get<1>(e) == type);
		out(std::get<0>(e));
		out(std::get<1>(e));
		return *this;
	}
#endif  // NDEBUG

	template <class Type, CHATRA_TYPE_EXISTS(typename Type::hasSerializeMethods)>
	Writer& out(const Type& value) {
		CHATRA_SAVE_TYPE_TAG(Serializable);
		value.save(*this);
		return *this;
	}

	template <class Iterator, typename ElementWriter>
	Writer& out(Iterator first, Iterator last, ElementWriter elementWriter) {
		CHATRA_SAVE_TYPE_TAG(List);
		out(std::distance(first, last));
		while (first != last)
			elementWriter(*first++);
		return *this;
	}

	template <class Iterator, typename Predicate, typename ElementWriter>
	Writer& out(Iterator first, Iterator last, Predicate predicate, ElementWriter elementWriter) {
		CHATRA_SAVE_TYPE_TAG(List);
		size_t length = 0;
		for (auto it = first; it != last; it++) {
			if (predicate(*it))
				length++;
		}
		out(length);
		for (auto it = first; it != last; it++) {
			if (predicate(*it))
				elementWriter(*it);
		}
		return *this;
	}

	template <class Container, typename ElementWriter, CHATRA_TYPE_EXISTS(typename Container::iterator)>
	Writer& out(const Container& container, ElementWriter elementWriter) {
		return out(container.cbegin(), container.cend(), elementWriter);
	}

	template <class Container, typename Predicate, typename ElementWriter,
			CHATRA_TYPE_EXISTS(typename Container::iterator)>
	Writer& out(const Container& container, Predicate predicate, ElementWriter elementWriter) {
		return out(container.cbegin(), container.cend(), predicate, elementWriter);
	}

	template <class Container, CHATRA_WHEN(
			!std::is_same<Container, std::string>::value &&
			!std::is_same<Container, std::vector<uint8_t>>::value &&
			CHATRA_HAS_TYPE(typename Container::iterator) && (
			std::is_integral<typename Container::value_type>::value ||
			std::is_enum<typename Container::value_type>::value ||
			std::is_same<typename Container::value_type, std::string>::value))>
	Writer& out(const Container& container) {
		return out(container.cbegin(), container.cend(),
				[&](const typename Container::value_type& value) { out(value); });
	}

	template <class Container, CHATRA_WHEN(std::is_same<Container, std::vector<uint8_t>>::value)>
	Writer& out(const Container& container) {
		CHATRA_SAVE_TYPE_TAG(ByteStream);
		out(container.size());
		buffer->insert(buffer->cend(), container.cbegin(), container.cend());
		return *this;
	}

	template <class Container, CHATRA_WHEN(
			CHATRA_HAS_TYPE(typename Container::iterator) &&
			CHATRA_HAS_TYPE(typename Container::value_type::hasSerializeMethods))>
	Writer& out(const Container& container) {
		return out(container.cbegin(), container.cend(),
				[&](const typename Container::value_type& value) {
			CHATRA_SAVE_TYPE_TAG(Serializable);
			value.save(*this);
		});
	}

	template <class KeyType, class ValueType>
	void out(const IdType<KeyType, ValueType>* entity) {
		CHATRA_SAVE_TYPE_TAG(IdType);
		if (entity == nullptr)
			out(enum_max<KeyType>::value);
		else
			out(entity->getId());
	}

	template <class Container, typename Predicate, typename ElementWriter,
			CHATRA_WHEN(std::is_base_of<IdPool<typename Container::KeyType_s, typename Container::ValueType_s>, Container>::value)>
	void out(const Container& pool, Predicate predicate, ElementWriter elementWriter) {
		CHATRA_SAVE_TYPE_TAG(List);
		size_t length = 0;
		pool.forEach([&](typename Container::ValueType_s& value) {
			if (predicate(value))
				length++;
		});
		out(length);
		pool.forEach([&](typename Container::ValueType_s& value) {
			if (predicate(value))
				elementWriter(value);
		});
	}

#ifdef NDEBUG
	template <class Type>
	Writer& registerPointer(const Type* value) {
		pointerMap.emplace(value, pointerMap.size());
		return *this;
	}
#else  // NDEBUG
	template <class Type>
	Writer& registerPointer(const Type* value, PointerType type) {
		pointerMap.emplace(value, std::make_tuple(pointerMap.size(), type));
		return *this;
	}
#endif  // NDEBUG

#ifndef NDEBUG
	RuntimeImp& getRuntime() const {
		return runtime;
	}
#endif  // !NDEBUG
};


class Reader final {
private:
	enum PointerMode {
		Raw, Unique, UniqueExported
	};

	struct PointerInfo {
		PointerMode mode;
		void* raw;
#ifndef NDEBUG
		PointerType type;
#endif  // !NDEBUG
	};

	RuntimeImp& runtime;
	const std::vector<uint8_t>* buffer = nullptr;
	size_t offset = 0;
	std::unordered_map<std::string, std::vector<uint8_t>> chunks;
	std::vector<PointerInfo> pointerList;

	bool entityMapEnabled = false;
	std::vector<const Class*> classMap;
	std::unordered_map<ptrdiff_t, const Node*> rootMap;

	std::unordered_map<Node*, const Class*> nodeClassMap;
	std::unordered_map<Node*, const Method*> nodeMethodMap;
	std::unordered_map<Node*, const MethodTable*> nodeMethodTableMap;

private:
	void checkSpace(size_t size = 1);

#ifdef NDEBUG
	PointerInfo& readPointerInfo();
#else
	PointerInfo& readPointerInfo(PointerType type);
#endif

	template <class Type>
	Type readRawInteger() {
		checkSpace();
		auto t = static_cast<uint64_t>(buffer->at(offset++));
		if (t < 128)
			return static_cast<Type>(t);
		t &= 0x7FU;
		for (unsigned position = 7; position < 64; position += 7) {
			checkSpace();
			auto c = static_cast<uint64_t>(buffer->at(offset++));
			if (c & 0x80U)
				t |= (c & 0x7FU) << position;
			else {
				t |= (c & 0x3FU) << position;
				return static_cast<Type>(c & 0x40U ? ~t : t);
			}
		}
		throw IllegalArgumentException();
	}

#ifndef NDEBUG
	void restoreTypeTag(TypeTag tag);
#endif  // !NDEBUG

	template <class KeyType, class ValueType>
	ValueType* readIdType(Reader& r, IdPool<KeyType, ValueType>& pool) {
		CHATRA_RESTORE_TYPE_TAG(IdType);
		auto id = r.read<KeyType>();
		return id == enum_max<KeyType>::value ? nullptr : pool.ref(id);
	}

	template <class KeyType, class ValueType>
	ValueType* readValidIdType(Reader& r, IdPool<KeyType, ValueType>& pool) {
		auto* value = readIdType(r, pool);
		if (value == nullptr)
			throw IllegalArgumentException();
		return value;
	}

public:
	explicit Reader(RuntimeImp& runtime) noexcept;

#ifndef NDEBUG
	~Reader();
#endif

	void parse(unsigned expectedVersion, const std::vector<uint8_t>& bytes);

	Reader& select(const std::string& chunk);

	void setEntityMap(
			const std::unordered_map<const Class*, size_t>& classMap,
			const std::unordered_map<const Node*, ptrdiff_t>& rootMap,
			const std::vector<const Class*>& systemClasses,
			const std::vector<const Method*>& systemMethods);
	void add(const Class* cl);
	void add(const Method* method);
	void add(const MethodTable* methodTable);

	Reader& restoreResync(int tag);

	Node* readNode();
	const Class* readClass();
	const Method* readMethod();
	const MethodTable* readMethodTable();

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Node*>::value)>
	Node* read() { return readNode(); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Class*>::value)>
	const Class* read() { return readClass(); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, Method*>::value)>
	const Method* read() { return readMethod(); }

	template <class Type, CHATRA_WHEN(std::is_same<typename remove_pointer_cv<Type>::type, MethodTable*>::value)>
	const MethodTable* read() { return readMethodTable(); }

	Reference readReference();
	Thread* readThread();
	Thread* readValidThread();
	Package* readPackage();
	Package* readValidPackage();

	template <class Type, CHATRA_WHEN(std::is_same<Type, Thread*>::value)>
	Thread* read() { return readThread(); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, Package*>::value)>
	Package* read() { return readPackage(); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(Bool);
		auto t = read<int>();
		if (t != 0 && t != 1)
			throw IllegalArgumentException();
		return t != 0;
	}

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(Integer);
		return readRawInteger<Type>();
	}

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(Float);
		checkSpace(sizeof(Type));
		auto ret = *reinterpret_cast<const Type*>(buffer->data() + offset);
		offset += sizeof(Type);
		return ret;
	}

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(Enum);
		return static_cast<Type>(read<typename std::underlying_type<Type>::type>());
	}

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(String);
		auto length = read<size_t>();
		checkSpace(length);
		std::vector<char> buffer(length);
		for (size_t i = 0; i < length; i++)
			buffer[i] = static_cast<char>(this->buffer->at(offset++));
		return std::string(buffer.cbegin(), buffer.cend());
	}

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::vector<uint8_t>>::value)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(ByteStream);
		auto length = read<size_t>();
		checkSpace(length);
		auto itBegin = buffer->cbegin() + offset;
		offset += length;
		return std::vector<uint8_t>(itBegin, buffer->cbegin() + offset);
	}

	template <class Type, CHATRA_TYPE_EXISTS(typename Type::hasSerializeMethods)>
	Type read() {
		CHATRA_RESTORE_TYPE_TAG(Serializable);
		Type value(*this);
		value.restore(*this);
		return value;
	}

	template <class Type, CHATRA_TYPE_EXISTS(typename Type::hasSerializeMethods)>
	Type* allocate() {
		CHATRA_RESTORE_TYPE_TAG(Serializable);
		auto* ptr = new Type(*this);
		ptr->restore(*this);
		return ptr;
	}

	template <class Container, CHATRA_WHEN(
			CHATRA_HAS_TYPE(typename Container::iterator) &&
			CHATRA_HAS_TYPE(typename Container::value_type::hasSerializeMethods)), class... Args>
	Reader& emplaceBack(Container& container, Args&&... args) {
		CHATRA_RESTORE_TYPE_TAG(Serializable);
		container.emplace_back(std::forward<Args>(args)..., *this);
		container.back().restore(*this);
		return *this;
	}

	template <class Type>
	Reader& in(Type& value) {
		value = read<Type>();
		return *this;
	}

	template <typename ElementReader>
	Reader& inList(ElementReader elementReader) {
		CHATRA_RESTORE_TYPE_TAG(List);
		auto length = read<size_t>();
		for (size_t i = 0; i < length; i++)
			elementReader();
		return *this;
	}

	template <class Container, CHATRA_WHEN(
			(std::is_same<Container, std::vector<typename Container::value_type>>::value ||
			std::is_same<Container, std::deque<typename Container::value_type>>::value) &&
			!std::is_same<Container, std::vector<uint8_t>>::value && (
			std::is_integral<typename Container::value_type>::value ||
			std::is_enum<typename Container::value_type>::value ||
			std::is_same<typename Container::value_type, std::string>::value))>
	Reader& inArray(Container& container) {
		return inList([&]() {
			container.emplace_back(read<typename Container::value_type>());
		});
	}

	template <class Container, CHATRA_WHEN(
			(std::is_same<Container, std::vector<typename Container::value_type>>::value ||
			std::is_same<Container, std::deque<typename Container::value_type>>::value) &&
			CHATRA_HAS_TYPE(typename Container::value_type::hasSerializeMethods))>
	Reader& inArray(Container& container) {
		return inList([&]() {
			CHATRA_RESTORE_TYPE_TAG(Serializable);
			container.emplace_back(*this);
			container.back().restore(*this);
		});
	}

#ifdef NDEBUG
	template <class Type>
	void registerPointer(Type* ptr) {
		assert(ptr != nullptr);
		pointerList.emplace_back();
		auto& p = pointerList.back();
		p.mode = PointerMode::Raw;
		p.raw = ptr;
	}
#else  // NDEBUG
	template <class Type>
	void registerPointer(Type* ptr, PointerType type) {
		assert(ptr != nullptr);
		pointerList.emplace_back();
		auto& p = pointerList.back();
		p.mode = PointerMode::Raw;
		p.raw = ptr;
		p.type = type;
	}
#endif // NDEBUG

#ifdef NDEBUG
	template <class Type>
	void registerPointer(std::unique_ptr<Type>&& ptr) {
		pointerList.emplace_back();
		auto& p = pointerList.back();
		p.mode = PointerMode::Unique;
		p.raw = ptr.release();
	}
#else // NDEBUG
	template <class Type>
	void registerPointer(std::unique_ptr<Type>&& ptr, PointerType type) {
		pointerList.emplace_back();
		auto& p = pointerList.back();
		p.mode = PointerMode::Unique;
		p.raw = ptr.release();
		p.type = type;
	}
#endif // NDEBUG

#ifdef NDEBUG
	template <class Type>
	std::unique_ptr<Type> readUnique() {
		auto& p = readPointerInfo();
		assert(p.raw == nullptr || p.mode == PointerMode::Unique);
		if (p.raw == nullptr)
			return nullptr;
		p.mode = PointerMode::UniqueExported;
		return std::unique_ptr<Type>(static_cast<Type*>(p.raw));
	}
#else  // NDEBUG
	template <class Type>
	std::unique_ptr<Type> readUnique(PointerType type) {
		auto& p = readPointerInfo(type);
		assert(p.raw == nullptr || p.mode == PointerMode::Unique);
		if (p.raw == nullptr)
			return nullptr;
		p.mode = PointerMode::UniqueExported;
		return std::unique_ptr<Type>(static_cast<Type*>(p.raw));
	}
#endif // NDEBUG

#ifdef NDEBUG
	template <class Type>
	Type* readRaw() {
		return static_cast<Type*>(readPointerInfo().raw);
	}
#else  // NDEBUG
	template <class Type>
	Type* readRaw(PointerType type) {
		return static_cast<Type*>(readPointerInfo(type).raw);
	}
#endif // NDEBUG
};


#define CHATRA_DECLARE_SERIALIZE_METHODS(className)  \
		using hasSerializeMethods = std::true_type;  \
		void save(Writer& w) const;  \
		explicit className(Reader& r) noexcept { (void)r; }  \
		void restore(Reader& r)

#define CHATRA_DECLARE_SERIALIZE_METHODS_WITHOUT_CONSTRUCTOR(className)  \
		using hasSerializeMethods = std::true_type;  \
		void save(Writer& w) const;  \
		void restore(Reader& r)

#define CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS(className)  \
		bool save(Writer& w) const;  \
		explicit className(Reader& r) noexcept;  \
		bool restore(Reader& r)

#define CHATRA_DECLARE_SERIALIZE_OBJECT_METHODS_WITH_THREAD(className)  \
		void saveThread(Writer& w) const;  \
		bool save(Writer& w) const;  \
		className(Thread& thread, Reader& r) noexcept;  \
		bool restore(Reader& r)

#define CHATRA_DECLARE_SERIALIZE_OBJECT_REFS_METHODS  \
		void saveReferences(Writer& w) const;  \
		void restoreReferences(Reader& r)


}  // namespace chatra

#endif //CHATRA_SERIALIZE_H
