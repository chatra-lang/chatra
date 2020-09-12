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
#include <cstdio>
#include <cstdarg>

namespace chatra {

class NativeReferenceImp;
class NativeCallContextImp;

NativeException::NativeException(const char* format, ...) {
	va_list args;
	va_start(args, format);
	setMessage(format, args);
	va_end(args);
}

#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(CHATRA_MAYBE_GCC)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

void NativeException::setMessage(const char *format, va_list args) {
	va_list args2;
	va_copy(args2, args);
	auto length = std::vsnprintf(nullptr, 0, format, args);
	if (length < 0)
		message = "(error)";
	else {
		std::vector<char> buffer(static_cast<size_t>(length) + 1);
		std::vsnprintf(buffer.data(), buffer.size(), format, args2);
		message = std::string(buffer.data());
	}
	va_end(args2);
}

#if defined(__clang__)
	#pragma clang diagnostic pop
#endif
#if defined(CHATRA_MAYBE_GCC)
	#pragma GCC diagnostic pop
#endif

NativeEventObjectImp::NativeEventObjectImp(RuntimeImp& runtime, unsigned waitingId) noexcept
		: runtime(runtime), waitingId(waitingId) {
}

void NativeEventObjectImp::unlock() {
	runtime.resume(waitingId);
}


class NativeCallContextImp final : public NativeCallContext {
	friend class NativeReferenceImp;

private:
	Thread& thread;
	UserObjectBase* object;
	std::string _name;
	std::string _subName;
	Tuple& args;
	Reference ret;
	mutable std::forward_list<std::unique_ptr<NativeReferenceImp>> refs;

public:
	NativeCallContextImp(Thread& thread, ObjectBase* object, std::string name, std::string subName, Tuple& args, Reference ret)
			: thread(thread), object(static_cast<UserObjectBase*>(object)),
			_name(std::move(name)), _subName(std::move(subName)), args(args), ret(std::move(ret)) {
		chatra_assert(object == nullptr || object->getTypeId() == typeId_UserObjectBase);
	}

	RuntimeId runtimeId() const override {
		return thread.runtime.runtimeId;
	}

	InstanceId instanceId() const override {
		return thread.instance.getId();
	}

	bool hasSelf() const override {
		return object != nullptr;
	}

	INativePtr* selfPtr() const override {
		return object == nullptr ? nullptr : object->refNativePtr();
	}

	bool isConstructor() const override {
		return _name == "init";
	}

	std::string name() const override {
		return _name;
	}

	std::string subName() const override {
		return _subName;
	}

	size_t size() const override {
		return args.tupleSize();
	}

	NativeReference& at(size_t position) const override;

	void setSelf(INativePtr* ptr) override {
		if (object == nullptr)
			throw UnsupportedOperationException();
		object->setNativePtr(ptr);
	}

	void setNull() override {
		ret.setNull();
	}

	void setBool(bool value) override {
		ret.setBool(value);
	}

	void setInt(int64_t value) override {
		ret.setInt(value);
	}

	void setFloat(double value) override {
		ret.setFloat(value);
	}

	void setString(const std::string& value) override {
		ret.allocate<String>().setValue(value);
	}

	NativeEventObject* pause() override {
		return new NativeEventObjectImp(thread.runtime, thread.requestToPause());
	}

	IDriver *getDriver(DriverType driverType) const override {
		return thread.runtime.getDriver(driverType);
	}

	void log(const std::string& message) override {
		outputLog(thread, message);
	}
};


class NativeReferenceImp final : public NativeReference {
private:
	const NativeCallContextImp& ct;
	Reference ref;
	bool topLevel;

public:
	NativeReferenceImp(const NativeCallContextImp& ct, Reference ref, bool topLevel)
			: ct(ct), ref(std::move(ref)), topLevel(topLevel) {
		chatra_assert(!ref.requiresLock() || ref.lockedBy() == ct.thread.getId());
	}

	bool isNull() const override {
		return ref.isNull();
	}

	bool isBool() const override {
		return ref.valueType() == ReferenceValueType::Bool;
	}

	bool isInt() const override {
		return ref.valueType() == ReferenceValueType::Int;
	}

	bool isFloat() const override {
		return ref.valueType() == ReferenceValueType::Float;
	}

	bool isString() const override {
		return getReferClass(ref) == String::getClassStatic();
	}

	bool isArray() const override {
		return getReferClass(ref) == Array::getClassStatic();
	}

	bool isDict() const override {
		return getReferClass(ref) == Dict::getClassStatic();
	}

	std::string className() const override {
		return isNull() ? "" : ct.thread.sTable->ref(getReferClass(ref)->getName());
	}

	bool getBool() const override {
		if (!isBool())
			throw UnsupportedOperationException();
		return ref.getBool();
	}

	int64_t getInt() const override {
		if (!isInt())
			throw UnsupportedOperationException();
		return ref.getInt();
	}

	double getFloat() const override {
		if (!isFloat())
			throw UnsupportedOperationException();
		return ref.getFloat();
	}

	std::string getString() const override {
		if (!isString())
			throw UnsupportedOperationException();
		return ref.deref<String>().getValue();
	}

	void setNull() override {
		if (topLevel)
			throw UnsupportedOperationException();
		ref.setNull();
	}

	void setBool(bool value) override {
		if (topLevel)
			throw UnsupportedOperationException();
		ref.setBool(value);
	}

	void setInt(int64_t value) override {
		if (topLevel)
			throw UnsupportedOperationException();
		ref.setInt(value);
	}

	void setFloat(double value) override {
		if (topLevel)
			throw UnsupportedOperationException();
		ref.setFloat(value);
	}

	void setString(const std::string& value) override {
		if (topLevel)
			throw UnsupportedOperationException();
		ref.allocate<String>().setValue(value);
	}

	size_t size() const override {
		if (!isArray() || !topLevel)
			throw UnsupportedOperationException();
		auto& value = ref.deref<Array>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		return value.length;
	}

	NativeReference& add() override {
		if (!isArray() || !topLevel)
			throw UnsupportedOperationException();

		auto& value = ref.deref<Array>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		value.extend(value.length + 1);
		value.container().ref(value.length++).setNull();

		ct.refs.emplace_front(new NativeReferenceImp(ct, value.container().ref(value.length - 1), false));
		return *ct.refs.front();
	}

	NativeReference& at(size_t position) const override {
		if (!isArray() || !topLevel)
			throw UnsupportedOperationException();

		auto& value = ref.deref<Array>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		if (position >= value.length)
			throw IllegalArgumentException();

		ct.refs.emplace_front(new NativeReferenceImp(ct, value.container().ref(position), false));
		return *ct.refs.front();
	}

	std::vector<std::string> keys() const override {
		if (!isDict() || !topLevel)
			throw UnsupportedOperationException();

		auto& value = ref.deref<Dict>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		std::vector<std::string> ret;
		for (auto& e : value.keyToIndex)
			ret.emplace_back(e.first);
		return ret;
	}

	bool has(const std::string& key) const override {
		if (!isDict() || !topLevel)
			throw UnsupportedOperationException();

		auto& value = ref.deref<Dict>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		return value.keyToIndex.count(key) != 0;
	}

	NativeReference& at(const std::string& key) const override {
		if (!isDict() || !topLevel)
			throw UnsupportedOperationException();

		auto& value = ref.deref<Dict>();
		std::lock_guard<SpinLock> lock(value.lockValue);
		auto it = value.keyToIndex.find(key);
		if (it == value.keyToIndex.cend())
			throw IllegalArgumentException();

		ct.refs.emplace_front(new NativeReferenceImp(ct, value.container().ref(it->second), false));
		return *ct.refs.front();
	}

	INativePtr* nativePtr() const override {
		if (ref.valueType() != ReferenceValueType::Object || ref.isNull())
			throw UnsupportedOperationException();
		auto* object = &ref.deref<ObjectBase>();
		if (object->getTypeId() != typeId_UserObjectBase)
			throw UnsupportedOperationException();
		return dynamic_cast<UserObjectBase*>(object)->refNativePtr();
	}

	void setNative(INativePtr* ptr) override {
		if (ref.valueType() != ReferenceValueType::Object || ref.isNull())
			throw UnsupportedOperationException();
		auto* object = &ref.deref<ObjectBase>();
		if (object->getTypeId() != typeId_UserObjectBase)
			throw UnsupportedOperationException();
		dynamic_cast<UserObjectBase*>(object)->setNativePtr(ptr);
	}
};


NativeReference& NativeCallContextImp::at(size_t position) const {
	if (position >= args.tupleSize())
		throw IllegalArgumentException();
	refs.emplace_front(new NativeReferenceImp(*this, args.ref(position), true));
	return *refs.front();
}

void nativeCall(CHATRA_NATIVE_ARGS) {
	chatra_assert(handler != nullptr);

	NativeCallContextImp ct(thread, object,
			name == StringId::Invalid ? "" : thread.sTable->ref(name),
			subName == StringId::Invalid ? "" : thread.sTable->ref(subName),
			args, ret);

	handler(ct);
}

}  // namespace chatra
