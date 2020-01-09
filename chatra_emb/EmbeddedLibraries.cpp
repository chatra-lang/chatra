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
#include <unordered_map>
#include <atomic>
#include <mutex>

cha::PackageInfo chatra_emb_sysPackageInfo();
cha::PackageInfo chatra_emb_formatPackageInfo();
cha::PackageInfo chatra_emb_regexPackageInfo();

static std::atomic<bool> initialized = {false};
static std::mutex mtInitialize;
static std::unordered_map<std::string, cha::PackageInfo> packages;

static void initialize() {
	std::lock_guard<std::mutex> lock(mtInitialize);
	if (initialized)
		return;

	std::vector<cha::PackageInfo> packageList = {
			chatra_emb_sysPackageInfo(),
			chatra_emb_formatPackageInfo(),
			chatra_emb_regexPackageInfo(),
	};

	for (auto& pi : packageList)
		packages.emplace(pi.scripts[0].name, pi);

	initialized = true;
}

namespace chatra {
PackageInfo queryEmbeddedPackage(const std::string& packageName) {
	if (!initialized)
		initialize();
	auto it = packages.find(packageName);
	return it != packages.cend() ? it->second : PackageInfo{{}, {}, nullptr};
}
}  // namespace chatra
