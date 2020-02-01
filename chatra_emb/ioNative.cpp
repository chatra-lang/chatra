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
#include <algorithm>
#include <mutex>

namespace chatra {
namespace emb {
namespace io {

static const char* script =
#include "io.cha"
;

enum class Type {
	FileInputStream, FileOutputStream
};

struct NativeData : public INativePtr {
	Type type;
	explicit NativeData(Type type) : type(type) {}
};

struct FileInputStreamData : public NativeData {
	std::mutex mt;
	std::unique_ptr<IFile> file;
	FileInputStreamData() : NativeData(Type::FileInputStream) {}
};

struct FileOutputStreamData : public NativeData {
	std::mutex mt;
	std::unique_ptr<IFile> file;
	FileOutputStreamData() : NativeData(Type::FileOutputStream) {}
};


struct IoPackageInterface : public IPackage {
	std::vector<uint8_t> saveNativePtr(PackageContext& pct, INativePtr* ptr) override {
		(void)pct;
		std::vector<uint8_t> buffer;

		auto* data = static_cast<NativeData*>(ptr);
		writeInt(buffer, static_cast<uint64_t>(data->type));

		auto* fs = static_cast<IFileSystem*>(pct.getDriver(DriverType::FileSystem));
		switch (data->type) {
		case Type::FileInputStream: {
			auto* self = static_cast<FileInputStreamData*>(ptr);
			auto fileData = fs->saveFile(self->file.get());
			buffer.insert(buffer.cend(), fileData.cbegin(), fileData.cend());
			break;
		}
		case Type::FileOutputStream: {
			auto* self = static_cast<FileOutputStreamData*>(ptr);
			auto fileData = fs->saveFile(self->file.get());
			buffer.insert(buffer.cend(), fileData.cbegin(), fileData.cend());
			break;
		}
		}
		return buffer;
	}

	INativePtr* restoreNativePtr(PackageContext& pct, const std::vector<uint8_t>& stream) override {
		(void)pct;
		size_t offset = 0;

		auto type = readInt<Type>(stream, offset);

		auto* fs = static_cast<IFileSystem*>(pct.getDriver(DriverType::FileSystem));
		switch (type) {
		case Type::FileInputStream: {
			auto* self = new FileInputStreamData();
			std::vector<uint8_t> fileData(stream.cbegin() + offset, stream.cend());
			self->file.reset(fs->restoreFile(fileData));
			return self;
		}
		case Type::FileOutputStream: {
			auto* self = new FileOutputStreamData();
			std::vector<uint8_t> fileData(stream.cbegin() + offset, stream.cend());
			self->file.reset(fs->restoreFile(fileData));
			return self;
		}
		default:
			throw NativeException();
		}
	}
};

static void parsePositionArgs(Ct& ct, size_t offsetIndex, size_t lengthIndex, ptrdiff_t bufferSize,
		ptrdiff_t& offset, ptrdiff_t& length) {

	offset = ct.at(offsetIndex).get<ptrdiff_t>();
	length = ct.at(lengthIndex).get<ptrdiff_t>();

	if (offset < 0)
		offset = bufferSize + offset;
	if (offset < 0)
		throw IllegalArgumentException();
	if (length < 0)
		length = bufferSize - offset;
	if (length < 0 || offset + length > bufferSize)
		throw IllegalArgumentException();
}

static IFile::SeekOrigin parseOrigin(Ct& ct, size_t originIndex) {
	auto origin = ct.at(originIndex).get<int>();
	return origin == 0 ? IFile::SeekOrigin::Begin : origin == 1 ? IFile::SeekOrigin::End : IFile::SeekOrigin::Current;
}

static FileInputStreamData* derefSelfAsInput(Ct& ct) {
	auto* m = ct.self<FileInputStreamData>();
	if (m == nullptr)
		throw IllegalArgumentException("invalid FileInputStream object");
	return m;
}

static void fileInputStream_initInstance(Ct& ct) {
	auto* fs = static_cast<IFileSystem*>(ct.getDriver(DriverType::FileSystem));

	auto* file = fs->openFile(ct.at(0).getString(), FileOpenFlags::Read, ct.at(1));
	if (file == nullptr)
		throw NativeException();

	auto* self = new FileInputStreamData();
	ct.setSelf(self);
	self->file.reset(file);
}

static void fileInputStream_close(Ct& ct) {
	auto* self = derefSelfAsInput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	self->file->close();
}

static void fileInputStream_available(Ct& ct) {
	auto* self = derefSelfAsInput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	ct.set(self->file->available());
}

static void fileInputStream_read(Ct& ct) {
	auto* self = derefSelfAsInput(ct);
	auto& buffer = containers::refByteArray(ct.at(0));
	ptrdiff_t bufferSize;
	{
		std::lock_guard<SpinLock> lock(buffer.lock);
		bufferSize = static_cast<ptrdiff_t>(buffer.data.size());
	}

	ptrdiff_t offset, length;
	parsePositionArgs(ct, 1, 2, bufferSize, offset, length);

	std::vector<uint8_t> tmp(length);
	size_t read;
	{
		std::lock_guard<std::mutex> lock(self->mt);
		read = self->file->read(tmp.data(), length);
	}
	ct.set(read);

	{
		std::lock_guard<SpinLock> lock(buffer.lock);
		if (bufferSize != static_cast<ptrdiff_t>(buffer.data.size()))
			throw NativeException();
		std::copy(tmp.cbegin(), tmp.cend(), buffer.data.begin() + offset);
	}
}

static void fileInputStream_seek(Ct& ct) {
	auto* self = derefSelfAsInput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	self->file->seek(ct.at(0).get<ptrdiff_t>(), parseOrigin(ct, 1));
}

static void fileInputStream_position(Ct& ct) {
	auto* self = derefSelfAsInput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	ct.set(self->file->position());
}

static FileOutputStreamData* derefSelfAsOutput(Ct& ct) {
	auto* m = ct.self<FileOutputStreamData>();
	if (m == nullptr)
		throw IllegalArgumentException("invalid FileOutputStream object");
	return m;
}

static void fileOutputStream_initInstance(Ct& ct) {
	auto* fs = static_cast<IFileSystem*>(ct.getDriver(DriverType::FileSystem));

	auto* file = fs->openFile(ct.at(0).getString(),
			ct.at(1).getBool() ? FileOpenFlags::Append : FileOpenFlags::Write, ct.at(2));
	if (file == nullptr)
		throw NativeException();

	auto* self = new FileOutputStreamData();
	ct.setSelf(self);
	self->file.reset(file);
}

static void fileOutputStream_close(Ct& ct) {
	auto* self = derefSelfAsOutput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	self->file->close();
}

static void fileOutputStream_flush(Ct& ct) {
	auto* self = derefSelfAsOutput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	self->file->flush();
}

static void fileOutputStream_write(Ct& ct) {
	auto* self = derefSelfAsOutput(ct);
	auto& buffer = containers::refByteArray(ct.at(0));
	std::vector<uint8_t> tmp;
	{
		std::lock_guard<SpinLock> lock(buffer.lock);
		ptrdiff_t offset, length;
		parsePositionArgs(ct, 1, 2, static_cast<ptrdiff_t>(buffer.data.size()),
				offset, length);
		tmp.resize(length);
		std::copy(buffer.data.cbegin() + offset, buffer.data.cbegin() + offset + length, tmp.begin());
	}

	size_t wrote;
	{
		std::lock_guard<std::mutex> lock(self->mt);
		wrote = self->file->write(tmp.data(), tmp.size());
	}
	ct.set(wrote);
}

static void fileOutputStream_seek(Ct& ct) {
	auto* self = derefSelfAsOutput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	self->file->seek(ct.at(0).get<ptrdiff_t>(), parseOrigin(ct, 1));
}

static void fileOutputStream_position(Ct& ct) {
	auto* self = derefSelfAsOutput(ct);
	std::lock_guard<std::mutex> lock(self->mt);
	ct.set(self->file->position());
}

static void reader_convertToString(Ct& ct) {
	auto& buffer = containers::refByteArray(ct.at(0));
	std::lock_guard<SpinLock> lock(buffer.lock);
	ct.set(reinterpret_cast<char*>(buffer.data.data()));
}

static void writer_convertToUtf8(Ct& ct) {
	auto& buffer = containers::refByteArray(ct.at(0));
	auto line = ct.at(1).getString();
	std::lock_guard<SpinLock> lock(buffer.lock);
	if (buffer.data.size() <= line.length())
		buffer.data.resize(line.length() * 4 + 64);
	std::copy(line.cbegin(), line.cend(), reinterpret_cast<char*>(buffer.data.data()));
	ct.set(line.length());
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

