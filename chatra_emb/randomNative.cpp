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

#include "chatra.h"
#include <random>

namespace chatra {
namespace emb {
namespace random {

static const char* script =
#include "random.cha"
;

static void comply(Ct& ct) {
	auto first = ct.at(0).get<int64_t>();
	auto last = ct.at(1).get<int64_t>();
	auto value = ct.at(2).get<uint64_t>();

	auto period = static_cast<uint64_t>(last - first);
	if (period == 1) {
		ct.setBool(true);
		return;
	}

	constexpr auto maxValue = std::numeric_limits<uint64_t>::max();
	ct.setBool(value < (maxValue / period + (maxValue % period + 1 == period ? 1 : 0)) * period);
}

static void convert(Ct& ct) {
	auto first = ct.at(0).get<int64_t>();
	auto last = ct.at(1).get<int64_t>();
	auto value = ct.at(2).get<uint64_t>();
	ct.setInt(first + value % static_cast<uint64_t>(last - first));
}

static void systemRandom(Ct& ct) {
	ct.setInt(static_cast<int64_t>(std::random_device()()));
}

PackageInfo packageInfo() {
	std::vector<Script> scripts = {{"random", script}};
	std::vector<HandlerInfo> handlers = {
			{comply, "_native_comply"},
			{convert, "_native_convert"},
			{systemRandom, "_native_systemRandom"},
	};
	return {scripts, handlers, nullptr};
}

}  // namespace random
}  // namespace emb
}  // namespace chatra
