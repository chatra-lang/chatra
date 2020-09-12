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
#ifndef CHATRA_EMB_INTERNAL_H
#define CHATRA_EMB_INTERNAL_H

#include "chatra.h"
#include <atomic>
#include <mutex>
#include <cassert>

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
	#define CHATRA_MAYBE_GCC   __GNUC__
#endif

namespace chatra {
namespace emb {

class SpinLock final {
private:
	std::atomic_flag flag = ATOMIC_FLAG_INIT;

public:
	void lock() {
		while (flag.test_and_set(std::memory_order_acq_rel));
	}

	void unlock() {
		flag.clear(std::memory_order_release);
	}
};

void writeRawInt(std::vector<uint8_t> &buffer, int64_t value);

template<class Type>
inline void writeInt(std::vector<uint8_t> &buffer, Type value) {
	writeRawInt(buffer, static_cast<int64_t>(value));
}

void writeString(std::vector<uint8_t> &buffer, const std::string &str);

int64_t readRawInt(const std::vector<uint8_t> &buffer, size_t &offset);

template<class Type>
inline Type readInt(const std::vector<uint8_t> &buffer, size_t &offset) {
	return static_cast<Type>(readRawInt(buffer, offset));
}

std::string readString(const std::vector<uint8_t> &buffer, size_t &offset);

}  // namespace emb
}  // namespace chatra

#endif //CHATRA_EMB_INTERNAL_H
