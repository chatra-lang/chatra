/*
 * Programming language 'Chatra' reference implementation
 *
 * Copyright(C) 2020 Chatra Project Team
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

#include "containersNative.h"

namespace chatra {
namespace emb {
namespace containers {

static const char *script =
#include "containers.cha"
;

enum class Type {
	ByteArray
};

struct NativeData : public INativePtr {
	Type type;
	explicit NativeData(Type type) : type(type) {}
};

struct ByteArrayData : public NativeData, public ByteArray {
	ByteArrayData() : NativeData(Type::ByteArray) {}

	static uint8_t fetchValue(Ct& ct, size_t position);
	size_t fetchIndex(Ct& ct, size_t position, bool allowBoundary);
};

struct ContainersPackageInterface : public IPackage {
	std::vector<uint8_t> saveNativePtr(PackageContext& pct, INativePtr* ptr) override {
		(void)pct;
		std::vector<uint8_t> buffer;

		auto* data = static_cast<NativeData*>(ptr);
		writeInt(buffer, static_cast<uint64_t>(data->type));

		switch (data->type) {
		case Type::ByteArray: {
			auto* self = static_cast<ByteArrayData*>(ptr);
			writeInt(buffer, self->data.size());
			buffer.reserve(buffer.size() + self->data.size());
			buffer.insert(buffer.cend(), self->data.cbegin(), self->data.cend());
			break;
		}
		}
		return buffer;
	}

	INativePtr* restoreNativePtr(PackageContext& pct, const std::vector<uint8_t>& stream) override {
		(void)pct;
		size_t offset = 0;

		auto type = readInt<Type>(stream, offset);
		switch (type) {
		case Type::ByteArray: {
			auto* self = new ByteArrayData();
			auto size = readInt<size_t>(stream, offset);
			self->data.reserve(size);
			self->data.insert(self->data.cend(), stream.cbegin() + offset, stream.cbegin() + offset + size);
			return self;
		}

		default:
			throw NativeException();
		}
	}
};

uint8_t ByteArrayData::fetchValue(Ct& ct, size_t position) {
	if (!ct.at(position).isInt())
		throw IllegalArgumentException("specified value is not integer");
	auto value = ct.at(position).getInt();
	if (value < 0 || value > 255)
		throw IllegalArgumentException("specified value is out of range");
	return static_cast<uint8_t>(value);
}

size_t ByteArrayData::fetchIndex(Ct& ct, size_t position, bool allowBoundary) {
	auto rawIndex = ct.at(position).get<ptrdiff_t>();
	auto index = static_cast<size_t>(rawIndex >= 0 ? rawIndex : data.size() + rawIndex);
	if (index >= data.size()) {
		if (index == data.size() && allowBoundary)
			return index;
		throw IllegalArgumentException(
				"specified position is out of range; size=%zu, specified = %lld",
				data.size(), static_cast<long long>(position));
	}
	return index;
}

ByteArray& refByteArray(Ref& ref) {
	return *ref.native<ByteArrayData>();
}

static void byteArray_initInstance(Ct& ct) {
	auto* self = new ByteArrayData();
	ct.setSelf(self);

	if (ct.size() == 0)
		return;

	if (ct.at(0).isString()) {
		auto value = ct.at(0).getString();
		auto* ptr = reinterpret_cast<const uint8_t*>(value.data());
		self->data.insert(self->data.cbegin(), ptr, ptr + value.length());
	}
	else if (ct.at(0).isArray()) {
		auto& value = ct.at(0);
		auto size = value.size();
		for (size_t i = 0; i < size; i++) {
			auto& ref = value.at(i);
			if (!ref.isInt())
				throw IllegalArgumentException("specified Array contains a non-Int value");
			auto t = ref.getInt();
			if (t < 0 || t > 255)
				throw IllegalArgumentException("specified value is out of range");
			self->data.emplace_back(static_cast<uint8_t>(t));
		}
	}
}

static void byteArray_size(Ct& ct) {
	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	ct.set(self->data.size());
}

static void byteArray_resize(Ct& ct) {
	auto newSize = ct.at(0).getInt();
	if (newSize < 0)
		throw IllegalArgumentException("invalid size");

	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	self->data.resize(static_cast<size_t>(newSize), static_cast<uint8_t>(0));
}

static void byteArray_add(Ct& ct) {
	auto value = ByteArrayData::fetchValue(ct, 0);

	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	self->data.emplace_back(value);
}

static void byteArray_insert(Ct& ct) {
	auto value = ByteArrayData::fetchValue(ct, 1);

	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	auto position = self->fetchIndex(ct, 0, true);
	self->data.insert(self->data.cbegin() + position, value);
}

static void byteArray_at(Ct& ct) {
	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	auto position = self->fetchIndex(ct, 0, false);

	if (ct.size() == 1) {
		ct.set(self->data[position]);
		return;
	}

	self->data[position] = ByteArrayData::fetchValue(ct, 1);
}

static void byteArray_remove(Ct& ct) {
	auto* self = ct.self<ByteArrayData>();
	std::lock_guard<SpinLock> lock(self->lock);
	auto position = self->fetchIndex(ct, 0, false);
	ct.set(self->data[position]);
	self->data.erase(self->data.cbegin() + position);
}

PackageInfo packageInfo() {
	std::vector<Script> scripts = {{"containers", script}};
	std::vector<HandlerInfo> handlers = {
			{byteArray_initInstance, "ByteArray", "_init_instance"},
			{byteArray_size, "ByteArray", "size"},
			{byteArray_resize, "ByteArray", "_native_resize"},
			{byteArray_add, "ByteArray", "_native_add"},
			{byteArray_insert, "ByteArray", "_native_insert"},
			{byteArray_at, "ByteArray", "_native_at"},
			{byteArray_remove, "ByteArray", "remove"},
	};
	return {scripts, handlers, std::make_shared<ContainersPackageInterface>()};
}


}  // namespace containers
}  // namespace emb
}  // namespace chatra
