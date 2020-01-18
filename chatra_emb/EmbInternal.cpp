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

#include "EmbInternal.h"

namespace chatra {
namespace emb {

void writeRawInt(std::vector<uint8_t> &buffer, int64_t value) {
	int sign = (value >= 0 ? 1 : -1);
	auto t = (sign > 0 ? static_cast<uint64_t>(value) : static_cast<uint64_t>(~value));
	if (sign > 0 && t < 128) {
		buffer.emplace_back(static_cast<uint8_t>(t));
		return;
	}
	for (;;) {
		buffer.emplace_back(static_cast<uint8_t>((t & 0x7FU) | 0x80U));
		t >>= 7U;
		if (t < 64) {
			buffer.emplace_back(static_cast<uint8_t>(t | (sign > 0 ? 0U : 0x40U)));
			break;
		}
	}
}

void writeString(std::vector<uint8_t> &buffer, const std::string &str) {
	writeInt(buffer, static_cast<uint64_t>(str.length()));
	auto *ptr = reinterpret_cast<const uint8_t *>(str.data());
	buffer.insert(buffer.cend(), ptr, ptr + str.length());
}

static void checkSpace(const std::vector<uint8_t> &buffer, size_t offset, size_t size = 1) {
	if (offset + size > buffer.size())
		throw cha::IllegalArgumentException("broken stream");
}

int64_t readRawInt(const std::vector<uint8_t> &buffer, size_t &offset) {
	checkSpace(buffer, offset);
	auto t = static_cast<uint64_t>(buffer[offset++]);
	if (t < 128)
		return t;
	t &= 0x7FU;
	for (unsigned position = 7; position < 64; position += 7) {
		checkSpace(buffer, offset);
		auto c = static_cast<uint64_t>(buffer[offset++]);
		if (c & 0x80U)
			t |= (c & 0x7FU) << position;
		else {
			t |= (c & 0x3FU) << position;
			return c & 0x40U ? ~t : t;
		}
	}
	throw cha::IllegalArgumentException("broken stream");
}

std::string readString(const std::vector<uint8_t> &buffer, size_t &offset) {
	auto length = readInt<size_t>(buffer, offset);
	if (offset + length > buffer.size())
		throw cha::NativeException();
	auto *ptr = reinterpret_cast<const char *>(buffer.data() + offset);
	offset += length;
	return std::string(ptr, ptr + length);
}

}  // namespace emb
}  // namespace chatra
