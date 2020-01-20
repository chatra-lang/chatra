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

struct NativeData : public cha::INativePtr {
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

