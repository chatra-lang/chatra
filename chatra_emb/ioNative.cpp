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

#include "containersNative.h"
#include <cstdio>
#include <algorithm>

// note: This is not part of C++11 standard
// This is because there is no canonical way in C++11 standard to get length of binary files.
#include <sys/stat.h>

namespace chatra {
namespace emb {
namespace io {

static const char* script =
#include "io.cha"
;

enum class Type {
	FileInputStream, FileOutputStream
};

struct NativeData : public cha::INativePtr {
	Type type;
	explicit NativeData(Type type) : type(type) {}
};

struct FileInputStreamData : public NativeData {
	// TODO Exclusive access(mutex?)


	FileInputStreamData() : NativeData(Type::FileInputStream) {}
};

struct FileOutputStreamData : public NativeData {


	FileOutputStreamData() : NativeData(Type::FileOutputStream) {}
};


struct IoPackageInterface : public cha::IPackage {
	std::vector<uint8_t> saveNativePtr(cha::PackageContext& pct, cha::INativePtr* ptr) override {
		(void)pct;
		std::vector<uint8_t> buffer;

		// TODO
		(void)ptr;

		return buffer;
	}

	cha::INativePtr* restoreNativePtr(cha::PackageContext& pct, const std::vector<uint8_t>& stream) override {
		(void)pct;
		// size_t offset = 0;

		// TODO
		(void)stream;

		return nullptr;
	}
};

static void fileInputStream_initInstance(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileInputStream_close(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileInputStream_available(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileInputStream_read(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileInputStream_seek(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileInputStream_position(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_initInstance(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_close(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_flush(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_write(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_seek(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void fileOutputStream_position(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void reader_convertToString(cha::Ct& ct) {
	// TODO
	(void)ct;
}

static void writer_convertToUtf8(cha::Ct& ct) {
	// TODO
	(void)ct;
}

PackageInfo packageInfo() {
	std::vector<Script> scripts = {{"io", script}};
	std::vector<HandlerInfo> handlers = {
			{fileInputStream_initInstance, "FileInputStream", "_init_instance"},
			{fileInputStream_close, "FileInputStream", "close"},
			{fileInputStream_available, "FileInputStream", "available"},
			{fileInputStream_read, "FileInputStream", "_native_read"},
			{fileInputStream_seek, "FileInputStream", "_native_seek"},
			{fileInputStream_position, "FileInputStream", "position"},
			{fileOutputStream_initInstance, "FileOutputStream", "_init_instance"},
			{fileOutputStream_close, "FileOutputStream", "close"},
			{fileOutputStream_flush, "FileOutputStream", "flush"},
			{fileOutputStream_write, "FileOutputStream", "_native_write"},
			{fileOutputStream_seek, "FileOutputStream", "_native_seek"},
			{fileOutputStream_position, "FileOutputStream", "position"},
			{reader_convertToString, "Reader", "_convertToString"},
			{writer_convertToUtf8, "Writer", "_convertToUtf8"},
	};
	return {scripts, handlers, std::make_shared<IoPackageInterface>()};
}

}  // namespace io
}  // namespace emb
}  // namespace chatra

// TODO split file

namespace chatra {

struct StandardFileCommon : public IFile {
	std::FILE* fp;
	size_t length;
	size_t current = 0;

	explicit StandardFileCommon(std::FILE* fp, size_t length) : fp(fp), length(length) {}

	~StandardFileCommon() override {
		if (fp != nullptr)
			std::fclose(fp);
	}

	void close() override {
		if (fp != nullptr)
			std::fclose(fp);
		fp = nullptr;
	}

	void flush() override {
		if (fp != nullptr)
			std::fflush(fp);
	}

	size_t available() override {
		return length - current;
	}

	size_t position() override {
		return current;
	}

	void seek(ptrdiff_t offset, SeekOrigin origin) override {
		if (fp == nullptr)
			throw cha::UnsupportedOperationException();

		size_t newPosition = 0;
		switch (origin) {
		case SeekOrigin::Begin:
			newPosition = std::max(static_cast<ptrdiff_t>(0), offset);  break;
		case SeekOrigin::End:
			newPosition = length + std::min(static_cast<ptrdiff_t>(0), offset);  break;
		case SeekOrigin::Current:
			newPosition = std::max(static_cast<ptrdiff_t>(0), std::min(static_cast<ptrdiff_t>(length), offset));  break;
		}

		if (std::fseek(fp, static_cast<long>(newPosition), SEEK_SET)) {
			close();
			throw cha::NativeException();
		}

		current = newPosition;
	}

};

struct ReadStandardFile : public StandardFileCommon {
	using StandardFileCommon::StandardFileCommon;

	size_t read(uint8_t* dest, size_t length) override {
		if (fp == nullptr)
			throw cha::UnsupportedOperationException();

		length = std::min(length, available());
		auto readBytes = std::fread(dest, 1, length, fp);
		if (std::ferror(fp)) {
			close();
			throw cha::NativeException();
		}
		current += readBytes;
		return readBytes;
	}

	size_t write(const uint8_t* src, size_t length) override {
		(void)src;
		(void)length;
		throw cha::UnsupportedOperationException();
	}

};

struct WriteStandardFile : public StandardFileCommon {
	WriteStandardFile(std::FILE* fp, size_t length, bool append) : StandardFileCommon(fp, length) {
		if (append)
			current = length;
	}

	size_t available() override {
		return 0;
	}

	size_t read(uint8_t* dest, size_t length) override {
		(void)dest;
		(void)length;
		throw cha::UnsupportedOperationException();
	}

	size_t write(const uint8_t* src, size_t length) override {
		if (fp == nullptr)
			throw cha::UnsupportedOperationException();

		auto wroteBytes = std::fwrite(src, 1, length, fp);
		if (wroteBytes != length) {
			close();
			throw cha::NativeException();
		}
		current += wroteBytes;
		this->length = std::max(this->length, current);

		return wroteBytes;
	}

};

std::unique_ptr<IFile> openStandardFile(const std::string& fileName, FileOpenFlags::Type flags,
		const NativeReference& kwargs) {

	(void)kwargs;

	// TODO Do not use 'a' mode
	(void)fileName;
	(void)flags;

	return nullptr;
}

}  // namespace chatra
