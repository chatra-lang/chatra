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

#ifndef CHATRA_H
#define CHATRA_H

#include <vector>
#include <string>
#include <memory>
#include <type_traits>
#include <limits>
#include <cstddef>

#define CHATRA_IGNORE_THIS_LINE

#define CHATRA_WHEN(...)  typename std::enable_if<__VA_ARGS__, std::nullptr_t>::type = nullptr

namespace chatra {

enum class RuntimeId : size_t {};
enum class PackageId : size_t {};
enum class InstanceId : size_t {};
enum class TimerId : size_t {};


struct NativeException : public std::exception {};
struct IllegalArgumentException : public NativeException {};
struct PackageNotFoundException : public IllegalArgumentException {};
struct UnsupportedOperationException : public NativeException {};


struct Script {
	/// Name for this script file. The value specified here is only used for error message.
	std::string name;
	/// Script text
	std::string script;

public:
	Script(std::string name, std::string script) noexcept : name(std::move(name)), script(std::move(script)) {}
};


struct INativePtr {
	virtual ~INativePtr() = default;
};


/// aka "Ref"
struct NativeReference {
	virtual ~NativeReference() = default;

	virtual bool isNull() const = 0;
	virtual bool isBool() const = 0;
	virtual bool isInt() const = 0;
	virtual bool isFloat() const = 0;
	virtual bool isString() const = 0;
	virtual bool isArray() const = 0;
	virtual bool isDict() const = 0;

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	bool is() const { return isBool(); }

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	bool is() const { return isInt(); }

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	bool is() const { return isFloat(); }

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	bool is() const { return isInt(); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	bool is() const { return isString(); }

	virtual std::string className() const = 0;

	virtual bool getBool() const = 0;
	virtual int64_t getInt() const = 0;
	virtual double getFloat() const = 0;
	virtual std::string getString() const = 0;

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	bool get() const { return getBool(); }

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	Type get() const { return static_cast<Type>(getInt()); }

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	Type get() const { return static_cast<Type>(getFloat()); }

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	Type get() const { return static_cast<Type>(static_cast<typename std::underlying_type<Type>::type>(getInt())); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	std::string get() const { return getString(); }

	// value write (only for element of Array/Dict)
	virtual void setNull() = 0;
	virtual void setBool(bool value) = 0;
	virtual void setInt(int64_t value) = 0;
	virtual void setFloat(double value) = 0;
	virtual void setString(const std::string& value) = 0;

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	void set(Type value) { setBool(value); }

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	void set(Type value) { setInt(static_cast<int64_t>(value)); }

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	void set(Type value) { setFloat(static_cast<double>(value)); }

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	void set(Type value) { setInt(static_cast<int64_t>(static_cast<typename std::underlying_type<Type>::type>(value))); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	void set(const Type& value) { setString(value); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, char>::value)>
	void set(const Type* value) { setString(value); }

	// for Array
	virtual size_t size() const = 0;
	virtual NativeReference& add() = 0;

	virtual NativeReference& at(size_t position) const = 0;
	NativeReference& operator[](size_t position) const { return at(position); }

	// for Dict
	virtual std::vector<std::string> keys() const = 0;
	virtual bool has(const std::string& key) const = 0;

	virtual NativeReference& at(const std::string& key) const = 0;
	NativeReference& operator[](const std::string& key) const { return at(key); }

	// for object
	virtual INativePtr* nativePtr() const = 0;

	template <class Type>
	Type* native() const {
		static_assert(std::is_base_of<INativePtr, Type>::value, "Type should be derived class of INativePtr");
		return static_cast<Type*>(nativePtr());
	}

	virtual void setNative(INativePtr* ptr) = 0;
};


/// aka "Event"
struct NativeEventObject {
	virtual ~NativeEventObject() = default;
	virtual void unlock() = 0;
};


/// aka "Ct"
struct NativeCallContext {
	virtual ~NativeCallContext() = default;

	virtual RuntimeId runtimeId() const = 0;

	virtual InstanceId intanceId() const = 0;

	virtual bool hasSelf() const = 0;
	virtual INativePtr* selfPtr() const = 0;

	template <class Type>
	Type* self() const {
		static_assert(std::is_base_of<INativePtr, Type>::value, "Type should be derived class of INativePtr");
		return static_cast<Type*>(selfPtr());
	}

	virtual bool isConstructor() const = 0;
	virtual std::string name() const = 0;
	virtual std::string subName() const = 0;

	virtual size_t size() const = 0;
	virtual NativeReference& at(size_t position) const = 0;
	NativeReference& operator[](size_t position) const { return at(position); }

	virtual void setSelf(INativePtr* ptr) = 0;

	virtual void setNull() = 0;
	virtual void setBool(bool value) = 0;
	virtual void setInt(int64_t value) = 0;
	virtual void setFloat(double value) = 0;
	virtual void setString(const std::string& value) = 0;

	template <class Type, CHATRA_WHEN(std::is_same<Type, bool>::value)>
	void set(Type value) { setBool(value); }

	template <class Type, CHATRA_WHEN(std::is_integral<Type>::value && !std::is_same<Type, bool>::value)>
	void set(Type value) { setInt(static_cast<int64_t>(value)); }

	template <class Type, CHATRA_WHEN(std::is_floating_point<Type>::value)>
	void set(Type value) { setFloat(static_cast<double>(value)); }

	template <class Type, CHATRA_WHEN(std::is_enum<Type>::value)>
	void set(Type value) { setInt(static_cast<int64_t>(static_cast<typename std::underlying_type<Type>::type>(value))); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, std::string>::value)>
	void set(const Type& value) { setString(value); }

	template <class Type, CHATRA_WHEN(std::is_same<Type, char>::value)>
	void set(const Type* value) { setString(value); }

	virtual NativeEventObject* pause() = 0;

	virtual void log(const std::string& message) = 0;
};


/// aka "Handler"
using NativeCallHandler = void (*)(NativeCallContext& ct);


/// aka "HandlerInfo"
struct NativeCallHandlerInfo {
	NativeCallHandler handler;
	std::string className;
	std::string name;
	std::string subName;

	NativeCallHandlerInfo(NativeCallHandler handler, std::string name) noexcept
			: handler(handler), name(std::move(name)) {}
	NativeCallHandlerInfo(NativeCallHandler handler, std::string className, std::string name) noexcept
			: handler(handler), className(std::move(className)), name(std::move(name)) {}
	NativeCallHandlerInfo(NativeCallHandler handler, std::string className, std::string name, std::string subName) noexcept
			: handler(handler), className(std::move(className)), name(std::move(name)), subName(std::move(subName)) {}
};


struct PackageContext {
	virtual ~PackageContext() = default;

	virtual RuntimeId runtimeId() const = 0;

	virtual std::vector<uint8_t> saveEvent(NativeEventObject* event) const = 0;
	virtual NativeEventObject* restoreEvent(const std::vector<uint8_t>& stream) const = 0;
};


struct IPackage {
	virtual ~IPackage() = default;

	virtual void attemptToShutdown(PackageContext& pct, bool save) {
		(void)pct; (void)save;
	}

	virtual std::vector<uint8_t> savePackage(PackageContext& pct) {
		(void)pct; return {};
	}

	virtual void restorePackage(PackageContext& pct, const std::vector<uint8_t>& stream) {
		(void)pct; (void)stream;
	}

	virtual std::vector<uint8_t> saveNativePtr(PackageContext& pct, INativePtr* ptr) {
		(void)pct; (void)ptr; return {};
	}

	virtual INativePtr* restoreNativePtr(PackageContext& pct, const std::vector<uint8_t>& stream) {
		(void)pct; (void)stream; return nullptr;
	}
};

struct PackageInfo {
	std::vector<Script> scripts;
	std::vector<NativeCallHandlerInfo> handlers;
	std::shared_ptr<IPackage> interface;
};

// This requires "chatra_emb" module.
PackageInfo queryEmbeddedPackage(const std::string& packageName);

struct IHost {
	virtual ~IHost() = default;

	virtual void console(const std::string& message) { (void)message; }

	virtual PackageInfo queryPackage(const std::string& packageName) {
		(void)packageName;
		return {{}, {}, nullptr};  // or return queryEmbeddedPackage(packageName);
	}
};


class Runtime {
public:
	virtual ~Runtime() = default;

	/// Create new instance of Runtime
	static std::shared_ptr<Runtime> newInstance(std::shared_ptr<IHost> host,
			const std::vector<uint8_t>& savedState = {},
			unsigned initialThreadCount = std::numeric_limits<unsigned>::max());

	/// Stop all running threads and close this Runtime instance
	virtual std::vector<uint8_t> shutdown(bool save) = 0;

	/// Stop all running threads and close this Runtime instance
	void shutdown() { (void)shutdown(false); }

	/// [Multi-thread] Change number of worker threads
	virtual void setWorkers(unsigned threadCount) = 0;

	/// [Single thread] Process message queue; returns false if message queue becomes empty
	virtual bool handleQueue() = 0;

	/// [Single thread] Process message until message queue becomes empty.
	/// Equivalent to "while(handleQueue()) ;"
	virtual void loop() = 0;

	/// Load anonymous temporary package (single script)
	virtual PackageId loadPackage(const Script& script) = 0;

	/// Load anonymous temporary package (multiple scripts)
	virtual PackageId loadPackage(const std::vector<Script>& scripts) = 0;

	/// Load package via IHost
	/// @throws PackageNotFoundException
	virtual PackageId loadPackage(const std::string& packageName) = 0;

	/// Start running package
	/// @throws PackageNotFoundException
	virtual InstanceId run(PackageId packageId) = 0;

	/// Check whether at least one thread remains or not
	/// @throws IllgalArgumentException  instance is not found
	virtual bool isRunning(InstanceId instanceId) = 0;

	/// Stop specified instance.
	/// @throws IllgalArgumentException  instance is not found
	/// @throws NotSupportedOperationException specified instance has active threads (= isRunning() returns true)
	virtual void stop(InstanceId instanceId) = 0;

	/// Add emulated timer
	/// @throws IllegalArgumentException
	virtual TimerId addTimer(const std::string& name) = 0;

	/// Increment emulated timer value
	/// @param step  must be &gt;=0
	/// @throws IllegalArgumentException
	virtual void increment(TimerId timerId, int64_t step) = 0;
};


// Shortcuts
using Ref = NativeReference;
using Event = NativeEventObject;
using Ct = NativeCallContext;
using Handler = NativeCallHandler;
using HandlerInfo = NativeCallHandlerInfo;


}  // namespace chatra

namespace cha = chatra;


#define CHATRA_ENUM_HASH(Type)  \
	namespace std { template<> struct hash<Type> { size_t operator()(Type x) const noexcept { \
		using BaseType = typename std::underlying_type<Type>::type;  \
		return std::hash<BaseType>()(static_cast<BaseType>(x));  \
	} }; }  // namespace std

CHATRA_ENUM_HASH(chatra::RuntimeId)
CHATRA_ENUM_HASH(chatra::PackageId)
CHATRA_ENUM_HASH(chatra::InstanceId)
CHATRA_ENUM_HASH(chatra::TimerId)


#endif // CHATRA_H
