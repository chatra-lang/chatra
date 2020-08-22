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

#include <cstdio>
#include <algorithm>
#include <cerrno>

// note: This is not part of C++11 standard
// This is because there is no canonical way on C++11 standard to get length of binary files from filesystem.
#include <sys/stat.h>

namespace chatra {
namespace emb {
namespace io {

struct StandardFileCommon : public IFile {
	std::string fileName;
	FileOpenFlags::Type flags;
	std::FILE* fp;
	size_t length;
	size_t current = 0;

	explicit StandardFileCommon(std::string fileName, FileOpenFlags::Type flags,
			std::FILE* fp, size_t length) : fileName(std::move(fileName)), flags(flags), fp(fp), length(length) {}

	~StandardFileCommon() override {
		if (fp != nullptr)
			std::fclose(fp);
	}

	void doClose() {
		if (fp != nullptr)
			std::fclose(fp);
		fp = nullptr;
	}

	void close() override {
		doClose();
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
			throw UnsupportedOperationException();

		ptrdiff_t newPosition = 0;
		switch (origin) {
		case SeekOrigin::Begin:
			newPosition = offset;  break;
		case SeekOrigin::End:
			newPosition = length + offset;  break;
		case SeekOrigin::Current:
			newPosition = current + offset;  break;
		}
		if (newPosition < 0 || length < static_cast<size_t>(newPosition))
			throw IllegalArgumentException("specified offset is out of range");

		if (std::fseek(fp, static_cast<long>(newPosition), SEEK_SET)) {
			close();
			throw NativeException();
		}

		current = newPosition;
	}
};

struct ReadStandardFile : public StandardFileCommon {
	using StandardFileCommon::StandardFileCommon;

	size_t read(uint8_t* dest, size_t length) override {
		if (fp == nullptr)
			throw UnsupportedOperationException();

		length = std::min(length, available());
		auto readBytes = std::fread(dest, 1, length, fp);
		if (std::ferror(fp)) {
			close();
			throw NativeException();
		}
		current += readBytes;
		return readBytes;
	}

	size_t write(const uint8_t* src, size_t length) override {
		(void)src;
		(void)length;
		throw UnsupportedOperationException();
	}
};

struct WriteStandardFile : public StandardFileCommon {
	WriteStandardFile(std::string fileName, FileOpenFlags::Type flags,
			std::FILE* fp, size_t length, bool append) : StandardFileCommon(std::move(fileName), flags, fp, length) {
		if (append) {
			current = length;
			if (std::fseek(fp, static_cast<long>(length), SEEK_SET))
				doClose();
		}
	}

	size_t available() override {
		return 0;
	}

	size_t read(uint8_t* dest, size_t length) override {
		(void)dest;
		(void)length;
		throw UnsupportedOperationException();
	}

	size_t write(const uint8_t* src, size_t length) override {
		if (fp == nullptr)
			throw UnsupportedOperationException();

		auto wroteBytes = std::fwrite(src, 1, length, fp);
		if (wroteBytes != length) {
			close();
			throw NativeException();
		}
		current += wroteBytes;
		this->length = std::max(this->length, current);
		return wroteBytes;
	}
};

static size_t getFileLengthOrClose(const std::string& fileName, FILE* fp) {
	struct stat st = {};
	if (0 != stat(fileName.data(), &st)) {
		std::fclose(fp);
		throw UnsupportedOperationException("failed to get length of specified file");
	}
	return static_cast<size_t>(st.st_size);
}

struct StandardFileSystem : public IFileSystem {
	FileNameFilter filter;
	explicit StandardFileSystem(FileNameFilter filter) : filter(filter) {}

	static StandardFileCommon *doOpenFile(std::string fileName, FileOpenFlags::Type flags) {
		if ((flags & FileOpenFlags::Write) && (flags & FileOpenFlags::Append)) {
			errno = 0;
			auto fp = std::fopen(fileName.data(), "r+b");
			if (fp == nullptr && errno == ENOENT)
				fp = std::fopen(fileName.data(), "wb");
			if (fp == nullptr)
				throw IllegalArgumentException("cannot open file: %s", fileName.data());

			size_t length = getFileLengthOrClose(fileName, fp);
			return new WriteStandardFile(std::move(fileName), flags, fp, length, true);
		}
		else if (flags & FileOpenFlags::Write) {
			auto fp = std::fopen(fileName.data(), "wb");
			return new WriteStandardFile(std::move(fileName), flags, fp, 0, false);
		}
		else if (flags & FileOpenFlags::Read) {
			auto fp = std::fopen(fileName.data(), "rb");
			size_t length = getFileLengthOrClose(fileName, fp);
			return new ReadStandardFile(std::move(fileName), flags, fp, length);
		}
		else
			throw IllegalArgumentException();
	}

	IFile *openFile(const std::string& fileName, FileOpenFlags::Type flags, const NativeReference& kwargs) override {
		(void)kwargs;
		return doOpenFile(filter == nullptr ? fileName : filter(fileName), flags);
	}

	std::vector<uint8_t> saveFile(IFile* file) override {
		auto* f = static_cast<StandardFileCommon*>(file);
		std::vector<uint8_t> buffer;
		writeString(buffer, f->fileName);
		writeInt(buffer, f->flags);
		writeInt(buffer, f->current);
		return buffer;
	}

	IFile *restoreFile(const std::vector<uint8_t>& stream) override {
		size_t offset = 0;
		auto fileName = readString(stream, offset);
		auto flags = readInt<FileOpenFlags::Type>(stream, offset);
		if (flags & FileOpenFlags::Write)
			flags |= FileOpenFlags::Append;  // avoiding re-creation

		auto* f = doOpenFile(fileName, flags);

		auto current = readInt<size_t>(stream, offset);
		try {
			if (current > f->length)
				throw IllegalArgumentException();
			f->seek(current, IFile::SeekOrigin::Begin);
		}
		catch (NativeException&) {
			delete f;
			throw;
		}

		return f;
	}
};

}  // namespace io
}  // namespace emb

IFileSystem* getStandardFileSystem(FileNameFilter filter) {
	return new emb::io::StandardFileSystem(filter);
}

}  // namespace chatra
