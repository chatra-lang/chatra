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

#ifndef CHATRA_INTERNAL_H
#define CHATRA_INTERNAL_H

#include <memory>
#include <algorithm>
#include <numeric>
#include <iterator>
#include <utility>
#include <limits>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <cassert>
#include <type_traits>
#include <functional>

#include <vector>
#include <array>
#include <deque>
#include <map>
#include <forward_list>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <string>

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

#include <cmath>

#ifndef NDEBUG
#include <cstdio>
#endif // !NDEBUG

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
	#define CHATRA_MAYBE_GCC   __GNUC__
#endif

#ifndef CHATRA_WHEN
	#define CHATRA_WHEN(...)  typename std::enable_if<__VA_ARGS__, std::nullptr_t>::type = nullptr
#endif
#define CHATRA_HAS_TYPE(...)  std::is_same<__VA_ARGS__, __VA_ARGS__>::value
#define CHATRA_TYPE_EXISTS(...)  CHATRA_WHEN(std::is_same<__VA_ARGS__, __VA_ARGS__>::value)

#if defined(__clang__)
	#define CHATRA_FALLTHROUGH  [[clang::fallthrough]]
	#define CHATRA_FALLTHROUGH_DEFINED
#endif

#if defined(CHATRA_MAYBE_GCC)
	#if __GNUC__ >= 7
		#define CHATRA_FALLTHROUGH  [[gnu::fallthrough]]
		#define CHATRA_FALLTHROUGH_DEFINED
	#endif
#endif

#ifndef CHATRA_FALLTHROUGH_DEFINED
	#define CHATRA_FALLTHROUGH
#endif

namespace chatra {

template <class Type>
struct remove_pointer_cv {
	using type = typename std::remove_cv<typename std::remove_pointer<Type>::type>::type*;
};

enum class ErrorLevel {
	Warning,
	Error,
};

struct AbortCompilingException : public std::exception {};
struct NotImplementedException : public std::exception {};
struct InternalError : public std::exception {};

struct IErrorReceiver {
	virtual ~IErrorReceiver() = default;
	virtual void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) = 0;
};

class IErrorReceiverBridge : public IErrorReceiver {
private:
	IErrorReceiver& target;
	unsigned errorCount = 0;

public:
	explicit IErrorReceiverBridge(IErrorReceiver& target) noexcept : target(target) {}

	void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override {
		if (level != ErrorLevel::Warning)
			errorCount++;
		target.error(level, fileName, lineNo, line, first, last, message, args);
	}

	bool hasError() const {
		return errorCount != 0;
	}
};

struct INullErrorReceiver final : public IErrorReceiver {
	void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override {
		(void)level;  (void)fileName;  (void)lineNo;  (void)line;  (void)first;  (void)last;
		(void)message;  (void)args;
	}
};

struct IAssertionNullErrorReceiver final : public IErrorReceiver {
	void error(ErrorLevel level,
			const std::string& fileName, unsigned lineNo, const std::string& line, size_t first, size_t last,
			const std::string& message, const std::vector<std::string>& args) override {
		(void)level;  (void)fileName;  (void)lineNo;  (void)line;  (void)first;  (void)last;
		(void)message;  (void)args;
		assert(false);
	}
};


class SpinLock {
private:
	std::atomic_flag flag = ATOMIC_FLAG_INIT;

public:
	void lock() {
		while (flag.test_and_set(std::memory_order_acq_rel))
			;
	}

	void unlock() {
		flag.clear(std::memory_order_release);
	}
};


template <class ValueType>
class AsyncRead {
private:
	ValueType values[2];
	size_t index = 0;
	std::atomic<ValueType*> valueForRead = {&values[0]};
	mutable std::atomic<int> readCount = {0};

public:
	template <class RetType, typename Process>
	RetType read(Process process) const {
		readCount++;
		RetType ret = process(*valueForRead.load());
		readCount--;
		return ret;
	}

	template <typename Process>
	void read(Process process) const {
		readCount++;
		process(*valueForRead.load());
		readCount--;
	}

	template <typename Process>
	void write(Process process) {
		process(values[index]);
		valueForRead = &values[index];
		while (readCount > 0)
			;
		index ^= 1U;
		process(values[index]);
	}

	// for debug
	int getReadCount() const {
		return readCount;
	}

	AsyncRead& operator=(const AsyncRead<ValueType>& r) {
		r.read([&](const ValueType& rValue) {
			write([&](ValueType& lValue) {
				lValue = rValue;
			});
		});
		return *this;
	}
};


template <class ValueType>
class AsyncReadWrite : public AsyncRead<ValueType> {
private:
	SpinLock lock;

public:
	template <typename Process>
	void writeAsync(Process process) {
		std::lock_guard<SpinLock> lock(this->lock);
		AsyncRead<ValueType>::write(std::move(process));
	}

	AsyncReadWrite& operator=(const AsyncReadWrite<ValueType>& r) {
		r.read([&](const ValueType& rValue) {
			writeAsync([&](ValueType& lValue) {
				lValue = rValue;
			});
		});
		return *this;
	}
};


template <class Type, class Enable = void>
struct key_base_type {
	using type = Type;
};

template <class Type>
struct key_base_type<Type, typename std::enable_if<std::is_enum<Type>::value>::type> {
	using type = typename std::underlying_type<Type>::type;
};

template <class Type>
struct enum_max {
	static constexpr Type value = static_cast<Type>(
			std::numeric_limits<typename std::underlying_type<Type>::type>::max());
};


template <class KeyType, class ValueType> class IdPool;

class IdTypeBase {};

template <class KeyType, class ValueType>
class IdType : public IdTypeBase {
	friend class IdPool<KeyType, ValueType>;

private:
	IdPool<KeyType, ValueType>* pool = nullptr;
	KeyType id = enum_max<KeyType>::value;

public:
	~IdType() {
		if (pool != nullptr)
			pool->remove(id);
	}

	void remove() {
		if (pool != nullptr) {
			pool->remove(id);
			pool = nullptr;
		}
	}

	KeyType getId() const {
		return id;
	}
};


template <class KeyType, class ValueType>
class IdPool {
	static_assert(std::is_base_of<IdType<KeyType, ValueType>, ValueType>::value, "ValueType should be derived from IdType");
	friend class IdType<KeyType, ValueType>;
	using KeyBaseType = typename key_base_type<KeyType>::type;

public:
	using _KeyType = KeyType;
	using _ValueType = ValueType;

private:
	mutable SpinLock lockValues;
	std::unordered_map<KeyBaseType, ValueType*> values;
	std::vector<KeyBaseType> recycledIds;

private:
	std::unique_ptr<ValueType> allocateId(ValueType* value) {
		if (recycledIds.empty())
			value->id = static_cast<KeyType>(values.size());
		else {
			value->id = static_cast<KeyType>(recycledIds.back());
			recycledIds.pop_back();
		}
		values.emplace(static_cast<KeyBaseType>(value->id), value);
		return std::unique_ptr<ValueType>(value);
	}

	std::unique_ptr<ValueType> setId(KeyType id, ValueType* value) {
		auto idBaseType = static_cast<KeyBaseType>(id);
		if (values.count(idBaseType) != 0)
			throw InternalError();

		auto it = std::find(recycledIds.cbegin(), recycledIds.cend(), idBaseType);
		if (it != recycledIds.cend())
			recycledIds.erase(it);
		else {
			while (values.size() + recycledIds.size() < idBaseType)
				recycledIds.emplace_back(values.size() + recycledIds.size());
		}

		value->id = id;
		values.emplace(idBaseType, value);
		return std::unique_ptr<ValueType>(value);
	}

	void remove(KeyType id) {
		auto idBaseType = static_cast<KeyBaseType>(id);
		std::lock_guard<SpinLock> lock0(lockValues);
		values.erase(idBaseType);
		recycledIds.push_back(idBaseType);
	}

public:
	void lock() {
		lockValues.lock();
	}

	void unlock() {
		lockValues.unlock();
	}

	template <class... Args>
	std::unique_ptr<ValueType> allocate(Args&&... args) {
		auto* value = new ValueType(std::forward<Args>(args)...);
		value->pool = this;
		return allocateId(value);
	}

	template <class... Args>
	std::unique_ptr<ValueType> lockAndAllocate(Args&&... args) {
		auto* value = new ValueType(std::forward<Args>(args)...);
		value->pool = this;
		std::lock_guard<SpinLock> lock(lockValues);
		return allocateId(value);
	}

	template <class... Args>
	std::unique_ptr<ValueType> allocateWithId(KeyType id, Args&&... args) {
		auto* value = new ValueType(std::forward<Args>(args)...);
		value->pool = this;
		return setId(id, value);
	}

	bool has(KeyType id) const {
		return values.count(static_cast<KeyBaseType>(id)) != 0;
	}

	ValueType* ref(KeyType id) const {
		return values.at(static_cast<KeyBaseType>(id));
	}

	ValueType* lockAndRef(KeyType id) const {
		std::lock_guard<SpinLock> lock(lockValues);
		auto it = values.find(static_cast<KeyBaseType>(id));
		return it == values.end() ? nullptr : it->second;
	}

	template <typename Process>
	void forEach(Process process) const {
		std::lock_guard<SpinLock> lock(lockValues);
		for (auto& e : values)
			process(*e.second);
	}
};


inline bool startsWith(const std::string& str, size_t index, const char * prefix) {
	size_t prefixLength = std::char_traits<char>::length(prefix);
	return index + prefixLength <= str.size() &&
			std::equal(prefix, prefix + prefixLength, str.cbegin() + static_cast<ptrdiff_t>(index));
}

inline bool startsWith(std::string::const_iterator first, std::string::const_iterator last, const char * prefix,
		size_t prefixLength) {
	return static_cast<size_t>(std::distance(first, last)) >= prefixLength &&
			std::equal(prefix, prefix + prefixLength, first);
}

inline bool startsWith(std::string::const_iterator first, std::string::const_iterator last, const char * prefix) {
	return startsWith(first, last, prefix, std::char_traits<char>::length(prefix));
}

inline bool endsWith(std::string::const_iterator first, std::string::const_iterator last, const char * suffix) {
	size_t suffixLength = std::char_traits<char>::length(suffix);
	return static_cast<size_t>(std::distance(first, last)) >= suffixLength &&
			std::equal(suffix, suffix + suffixLength, last - static_cast<ptrdiff_t>(suffixLength));
}

inline size_t byteCount(const std::string& str, size_t index) {
	auto c = static_cast<unsigned>(str[index]);
	size_t length = ((c & 0x80U) == 0U ? 1 : (c & 0xE0U) == 0xC0U ? 2 : (c & 0xF0U) == 0xE0U ? 3 :
			(c & 0xF8U) == 0xF0U ? 4 : 0);
	if (index + length > str.size())
		return 0;
	for (size_t i = 1; i < length; i++)  if ((static_cast<unsigned>(str[index + i]) & 0xC0U) != 0x80U)
			return 0;
	return length;
}

inline size_t extractChar(char32_t c, char* dest) {
	if (c < 0x80) {
		dest[0] = static_cast<char>(c);
		return 1;
	}
	if (c < 0x800) {
		dest[0] = static_cast<char>(c >> 6U | 0xC0U);
		dest[1] = static_cast<char>((c & 0x3FU) | 0x80U);
		return 2;
	}
	if (c < 0x10000) {
		dest[0] = static_cast<char>(c >> 12U | 0xE0U);
		dest[1] = static_cast<char>((c >> 6U & 0x3FU) | 0x80U);
		dest[2] = static_cast<char>((c & 0x3FU) | 0x80U);
		return 3;
	}
	if (c < 0x110000) {
		dest[0] = static_cast<char>(c >> 18U | 0xE0U);
		dest[1] = static_cast<char>((c >> 12U & 0x3FU) | 0x80U);
		dest[2] = static_cast<char>((c >> 6U & 0x3FU) | 0x80U);
		dest[3] = static_cast<char>((c & 0x3FU) | 0x80U);
		return 4;
	}
	return 0;
}


}  // namespace chatra


#endif //CHATRA_INTERNAL_H
