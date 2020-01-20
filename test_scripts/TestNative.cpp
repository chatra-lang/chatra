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

#include "chatra.h"
#include <unordered_map>

constexpr const char* packageName = "test_native";

static const char* script =
#include "test_native.cha"
;

struct TestNativeData : public cha::INativePtr {
	int value;
	explicit TestNativeData(int value = 0) : value(value) {}
};

struct PackageData {
	std::unique_ptr<cha::Event> event;
};

static std::unordered_map<cha::RuntimeId, std::unique_ptr<PackageData>> packageDataMap;

struct TestPackageInterface : public cha::IPackage {
	std::vector<uint8_t> savePackage(cha::PackageContext& pct) override {
		auto& packageData = packageDataMap[pct.runtimeId()];
		if (!packageData || !packageData->event)
			return {};
		return pct.saveEvent(packageData->event.get());
	}

	void restorePackage(cha::PackageContext& pct, const std::vector<uint8_t>& stream) override {
		auto& packageData = packageDataMap[pct.runtimeId()];
		if (!packageData)
			packageData.reset(new PackageData());
		packageData->event.reset();
		if (!stream.empty())
			packageData->event.reset(pct.restoreEvent(stream));
	}

	std::vector<uint8_t> saveNativePtr(cha::PackageContext& pct, cha::INativePtr* ptr) override {
		(void)pct;
		auto* p = static_cast<TestNativeData*>(ptr);
		std::vector<uint8_t> data(8);
		*reinterpret_cast<int32_t*>(data.data()) = static_cast<int32_t>(p->value);
		return data;
	}

	cha::INativePtr* restoreNativePtr(cha::PackageContext& pct, const std::vector<uint8_t>& stream) override {
		(void)pct;
		if (stream.size() != 8)
			throw cha::NativeException();
		return new TestNativeData(static_cast<int>(*reinterpret_cast<const int32_t*>(stream.data())));
	}
};


static void testCommand(cha::Ct& ct) {
	auto& a0 = ct.at(0);
	if (!a0.isString())
		throw cha::NativeException();
	auto verb = a0.get<std::string>();

	// NativeCallContext

	if (verb == "runtimeId") {
		ct.set(static_cast<std::underlying_type<cha::RuntimeId>::type>(ct.runtimeId()));
		return;
	}

	if (verb == "instanceId") {
		ct.set(static_cast<std::underlying_type<cha::InstanceId>::type>(ct.intanceId()));
		return;
	}

	if (verb == "hasSelf") {
		ct.set(ct.hasSelf());
		return;
	}

	if (verb == "self") {
		auto* ptr = ct.self<TestNativeData>();
		ct.set(ptr == nullptr ? 0 : ptr->value);
		return;
	}

	if (verb == "isConstructor") {
		ct.set(ct.isConstructor());
		return;
	}

	if (verb == "name") {
		ct.set(ct.name());
		return;
	}

	if (verb == "subName") {
		ct.set(ct.subName());
		return;
	}

	if (verb == "size") {
		ct.set(ct.size());
		return;
	}

	if (verb == "setSelf") {
		ct.setSelf(new TestNativeData(ct.at(1).get<int>()));
		return;
	}

	if (verb == "setNull") { ct.setNull(); return; }
	if (verb == "setBool") { ct.set(!ct.at(1).get<bool>()); return; }
	if (verb == "setInt") { ct.set(ct.at(1).get<int>() + 100); return; }
	if (verb == "setFloat") { ct.set(ct.at(1).get<double>() + 1000.0); return; }
	if (verb == "setString") { ct.set(ct.at(1).get<std::string>() + "hoge"); return; }

	if (verb == "setCString") {
		ct.set("hogehoge");
		return;
	}

	if (verb == "pause") {
		auto& packageData = packageDataMap[ct.runtimeId()];
		if (!packageData)
			packageData.reset(new PackageData());
		if (packageData->event)
			throw cha::NativeException();

		packageData->event.reset(ct.pause());
		return;
	}

	if (verb == "resume") {
		auto& packageData = packageDataMap[ct.runtimeId()];
		if (!packageData || !packageData->event)
			throw cha::NativeException();

		auto event = std::move(packageData->event);
		event->unlock();
		event.reset();
		return;
	}

	if (verb == "log") {
		ct.log(ct[1].get<std::string>());
		return;
	}

	// NativeReference

	auto index = (ct.size() >= 2 && ct.at(1).isInt() ? ct.at(1).get<size_t>() : SIZE_MAX);

	if (verb == "arg_isNull") { ct.set(ct[index].isNull()); return; }
	if (verb == "arg_isBool") { ct.set(ct[index].is<bool>()); return; }
	if (verb == "arg_isInt") { ct.set(ct[index].is<int>()); return; }
	if (verb == "arg_isFloat") { ct.set(ct[index].is<double>()); return; }
	if (verb == "arg_isString") { ct.set(ct[index].is<std::string>()); return; }
	if (verb == "arg_isArray") { ct.set(ct[index].isArray()); return; }
	if (verb == "arg_isDict") { ct.set(ct[index].isDict()); return; }

	if (verb == "arg_className") {
		ct.set(ct[index].className());
		return;
	}

	if (verb == "arg_getBool") { ct.set(!ct[index].get<bool>()); return; }
	if (verb == "arg_getInt") { ct.set(ct[index].get<int>() + 200); return; }
	if (verb == "arg_getFloat") { ct.set(ct[index].get<double>() + 2000.0); return; }
	if (verb == "arg_getString") { ct.set(ct[index].get<std::string>() + "uhe"); return; }

	if (verb == "arg_size") {
		ct.set(ct[index].size());
		return;
	}

	if (verb == "arg_keys") {
		auto& dest = ct.at(2);
		for (auto& key : ct[index].keys())
			dest.add().set<std::string>(key);
		return;
	}

	if (verb == "arg_setPtr") {
		ct[index].setNative(new TestNativeData(ct.at(2).get<int>()));
		return;
	}

	if (verb == "arg_getPtr") {
		auto* ptr = ct[index].native<TestNativeData>();
		ct.set(ptr == nullptr ? 0 : ptr->value + 500);
		return;
	}

	// Array
	if (ct.size() >= 3 && ct.at(2).isInt()) {
		auto arrayIndex = ct.at(2).get<size_t>();

		if (verb == "arg_array_at_isNull") { ct.set(ct[index][arrayIndex].isNull()); return; }
		if (verb == "arg_array_at_isInt") { ct.set(ct[index][arrayIndex].is<int>()); return; }
		if (verb == "arg_array_at_isString") { ct.set(ct[index][arrayIndex].is<std::string>()); return; }

		if (verb == "arg_array_at_setNull") { ct[index][arrayIndex].setNull(); return; }
		if (verb == "arg_array_at_setBool") { ct[index][arrayIndex].set(!ct.at(3).get<bool>()); return; }
		if (verb == "arg_array_at_setInt") { ct[index][arrayIndex].set(ct.at(3).get<int>() + 300); return; }
		if (verb == "arg_array_at_setFloat") { ct[index][arrayIndex].set(ct.at(3).get<double>() + 3000.0); return; }
		if (verb == "arg_array_at_setString") { ct[index][arrayIndex].set(ct.at(3).get<std::string>() + "array"); return; }
	}

	// Dict
	if (ct.size() >= 3 && ct.at(2).isString()) {
		auto key = ct.at(2).get<std::string>();

		if (verb == "arg_has") {
			ct.set(ct[index].has(key));
			return;
		}

		if (verb == "arg_dict_at_isNull") { ct.set(ct[index][key].isNull()); return; }
		if (verb == "arg_dict_at_isInt") { ct.set(ct[index][key].is<int>()); return; }
		if (verb == "arg_dict_at_isString") { ct.set(ct[index][key].is<std::string>()); return; }

		if (verb == "arg_dict_at_setNull") { ct[index][key].setNull(); return; }
		if (verb == "arg_dict_at_setBool") { ct[index][key].set(!ct.at(3).get<bool>()); return; }
		if (verb == "arg_dict_at_setInt") { ct[index][key].set(ct.at(3).get<int>() + 400); return; }
		if (verb == "arg_dict_at_setFloat") { ct[index][key].set(ct.at(3).get<double>() + 4000.0); return; }
		if (verb == "arg_dict_at_setString") { ct[index][key].set(ct.at(3).get<std::string>() + "dict"); return; }
	}

	throw cha::NativeException();
}

cha::PackageInfo getTestNativePackage() {
	std::vector<cha::Script> scripts = {{packageName, script}};
	std::vector<cha::HandlerInfo> handlers = {
			{testCommand, "testCommand"},
			{testCommand, "TestClass", "init"},
			{testCommand, "TestClass", "init", "native"},
			{testCommand, "TestClass", "method"}
	};
	return {scripts, handlers, std::make_shared<TestPackageInterface>()};
}
