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

#include "Classes.h"

namespace chatra {

static ClassTable embeddedClasses;
static std::forward_list<std::shared_ptr<Node>> embeddedClassesNode;
static std::forward_list<std::unique_ptr<Class>> embeddedClassesPtr;
static std::unique_ptr<Class> clAsync;
static std::unique_ptr<Class> clString;
static std::unique_ptr<Class> clArray;
static std::unique_ptr<Class> clDict;
static std::unordered_map<StringId, Node*> nodeMap;

// note: Any tokens used in embedded scripts must be pre-defined in StringTable.
// note: Do not use inner functions because these are not scanned during restoring state.

// TODO Some method should be native method to gain more performance

static const char* initIterator = R"***(
class Iterator
)***";

static const char* initKeyedIterator = R"***(
class KeyedIterator extends Iterator
	def init()
		super()
)***";

static const char* initIndexSet = R"***(
class IndexSet
)***";

static const char* initRangeIterator = R"***(
class RangeIterator extends Iterator
	var m1, m2, m3

	def init(a0, a1, a2)
		super()
		m1 = a0
		m2 = a1
		m3 = a2

	def hasNext()
		return m1 != m2

	def next()
		t0 = m1
		m1 += m3
		return value: t0
)***";

static const char* initKeyedRangeIterator = R"***(
class KeyedRangeIterator extends KeyedIterator
	var m0: 0
	var m1, m2, m3

	def init(a0, a1, a2)
		super()
		m1 = a0
		m2 = a1
		m3 = a2

	def hasNext()
		return m1 != m2

	def next()
		t0 = m1
		m1 += m3
		return key: m0++, value: t0
)***";

static const char* initRange = R"***(
class Range extends IndexSet
	var m1, m2, m3

	def init(last: Int)
		super()
		if last < 0
			throw IllegalArgumentException()
		m1 = 0
		m2 = last
		m3 = 1

	def init(first: Int, last: Int; step: Int(1))
		super()
		if step == 0 or (step > 0 and first > last) or (step < 0 and first < last)
			throw IllegalArgumentException()
		m1 = first
		m2 = first + (last - first) /+ step * step
		m3 = step

	def (position: Int)
		return m1 + m3 * position

	def iterator()
		return RangeIterator(m1, m2, m3)

	def keyedIterator()
		return KeyedRangeIterator(m1, m2, m3)

	def size()
		return (m2 - m1) / m3

	def first()
		return m1

	def last()
		return m2

	def step()
		return m3

	def ascend()
		return m3 > 0

	def reverse()
		return Range(m2 - m3, m1 - m3, step: -m3)

	def toString()
		return 'Range('.append(m1).append(', ').append(m2).append(', step: ').append(m3).append(')')
)***";

static const char* initArrayIterator = R"***(
class ArrayIterator extends Iterator
	var m0
	var m1: 0

	def init(a0)
		super()
		m0 = a0

	def hasNext()
		return m1 < m0.size

	def next()
		return value: m0[m1++]

	def remove()
		return m0.remove(--m1)
)***";

static const char* initArrayKeyedIterator = R"***(
class ArrayKeyedIterator extends KeyedIterator
	var m0
	var m1: 0

	def init(a0)
		super()
		m0 = a0

	def hasNext()
		return m1 < m0.size()

	def next()
		t0: m1++
		return key: t0, value: m0[t0]

	def remove()
		return m0.remove(--m1)
)***";

static const char* initDictIterator = R"***(
class DictIterator extends Iterator
	var m0, m1
	var m2: 0

	def init(a0)
		super()
		m0 = a0
		m1 = a0.keys()

	def hasNext()
		return m2 < m1.size()

	def next()
		t0: m1[m2++]
		return value: m0[t0]

	def remove()
		return m0.remove(m1[m2 - 1])
)***";

static const char* initDictKeyedIterator = R"***(
class DictKeyedIterator extends KeyedIterator
	var m0, m1
	var m2: 0

	def init(a0)
		super()
		m0 = a0
		m1 = a0.keys()

	def hasNext()
		return m2 < m1.size()

	def next()
		t0: m1[m2++]
		return key: t0, value: m0[t0]

	def remove()
		return m0.remove(m1[m2 - 1])
)***";

static const char* initArrayView = R"***(
class ArrayView
	var m0, m1

	def init(a0, a1: IndexSet)
		m0 = a0
		m1 = a1

	def size()
		return m1.size()

	def add(a0...)
		throw UnsupportedOperationException()

	def insert(a0...)
		throw UnsupportedOperationException()

	def append(a0...)
		throw UnsupportedOperationException()

	def (a0: Int)
		return m0[m1[a0]]

	def (a0: Int).set(r)
		return m0[m1[a0]] = r

	def remove(a0...)
		throw UnsupportedOperationException()

	def clear(a0...)
		throw UnsupportedOperationException()

	def clone()
		return ArrayView(m0, m1)

	def equals(a0)
		if a0 == null or !(a0 is Array or a0 is ArrayView)
			return false
		if size != a0.size
			return false
		t0 = size
		t1 = 0
		while t1 < t0
			if self[t1] != a0[t1]
				return false
			t1++
		return true

	def sub(a0: IndexSet)
		return ArrayView(self, a0)

	def iterator()
		return ArrayIterator(self)

	def keyedIterator()
		return ArrayKeyedIterator(self)

	def toArray()
		t0 = Array()
		t1 = m1.size()
		t2 = 0
		while t2 < t1
			t0.add(m0[m1[t2++]])
		return t0

	def toString()
		t0 = m0.toString()
		t0.append('.sub(')
		t0.append(m1.toString())
		return t0.append(')')
)***";

void EventObject::registerWatcher(void* tag, EventWatcher watcher) {
	std::lock_guard<SpinLock> lock(lockWatchers);
	registered.emplace(tag, watcher);
}

void EventObject::activateWatcher(void* tag) {
	bool shouldRestore = false;
	EventWatcher watcher;
	{
		std::lock_guard<SpinLock> lock(lockWatchers);
		auto it = registered.find(tag);
		if (it == registered.cend())
			return;
		watcher = it->second;
		registered.erase(it);

		if (count == 0) {
			activated.emplace(tag, watcher);
			return;
		}
		if (count != UINT_MAX) {
			count--;
			shouldRestore = true;
		}
	}
	if (!watcher(tag) && shouldRestore)
		notifyOne();
}

bool EventObject::unregisterWatcher(void* tag) {
	std::lock_guard<SpinLock> lock(lockWatchers);
	return registered.erase(tag) + activated.erase(tag) != 0;
}

void EventObject::notifyOne() {
	for (;;) {
		void* tag;
		EventWatcher watcher;
		{
			std::lock_guard<SpinLock> lock(lockWatchers);
			assert(count != UINT_MAX);
			if (activated.empty()) {
				count++;
				return;
			}
			auto it = activated.cbegin();
			tag = it->first;
			watcher = it->second;
			activated.erase(it);
		}
		if (watcher(tag))
			return;
	}
}

void EventObject::notifyAll() {
	std::unordered_map<void*, EventWatcher>  watchersCopy;
	{
		std::lock_guard<SpinLock> lock(lockWatchers);
		count = UINT_MAX;
		std::swap(activated, watchersCopy);
	}
	for (auto& e : watchersCopy)
		e.second(e.first);
}

void EventObject::saveEventObject(Writer& w) const {
	w.out(count);
}

void EventObject::restoreEventObject(Reader& r) {
	r.in(count);
}

const Class* Async::getClassStatic() {
	return clAsync.get();
}

static const char* initAsync = R"***(
class Async
	var m0

	def init()

	def _native_updated() as native
	def finished()
		return _native_updated()

	def result()
		if !_native_updated()
			throw UnsupportedOperationException()
		return m0

	def result().set(r)
		throw UnsupportedOperationException()
)***";

[[noreturn]] static void createAsync(const Class* cl, Reference ref) {
	(void)cl;
	(void)ref;
	assert(cl == clAsync.get());
	throw RuntimeException(StringId::UnsupportedOperationException);
}

void Async::native_updated(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	ret.setBool(self.updated);
}

void Async::updateResult(Reference ref) {
	assert(Async::getClassStatic()->refMethods().find(nullptr, StringId::m0, StringId::Invalid, {}, {})->position == 0);
	this->ref(0).setWithoutLock(ref);
	updated = true;
	notifyAll();
}

bool Async::save(Writer& w) const {
	w.out(updated.load());
	saveEventObject(w);
	return false;
}

Async::Async(Reader& r) noexcept : ObjectBase(typeId_Async, getClassStatic()) {
	(void)r;
}

bool Async::restore(Reader& r) {
	updated.store(r.read<bool>());
	restoreEventObject(r);
	return false;
}

static void createString(const Class* cl, Reference ref) {
	(void)cl;
	assert(cl == clString.get());
	ref.allocate<String>();
}

static inline unsigned uc(char c) {
	return static_cast<unsigned>(c);
}

char32_t String::getCharOrThrow(const std::string& str, size_t index) {
	auto c = uc(str[index]);
	if ((c & 0x80U) == 0)
		return static_cast<char32_t>(c);
	if ((c & 0xE0U) == 0xC0U)
		return static_cast<char32_t>((c & 0x1FU) << 6U | (uc(str[index + 1]) & 0x3FU));
	if ((c & 0xF0U) == 0xE0U) {
		return static_cast<char32_t>((c & 0x0FU) << 12U | (uc(str[index + 1]) & 0x3FU) << 6U |
				(uc(str[index + 2]) & 0x3FU));
	}
	if ((c & 0xF8U) == 0xF0U) {
		return static_cast<char32_t>((c & 0x07U) << 18U | (uc(str[index + 1]) & 0x3FU) << 12U |
				(uc(str[index + 2]) & 0x3FU) << 6U | (uc(str[index + 3]) & 0x3FU));
	}
	throw RuntimeException(StringId::IllegalArgumentException);
}

size_t String::extractCharOrThrow(char32_t c, char* dest) {
	size_t count = extractChar(c, dest);
	if (count == 0)
		throw RuntimeException(StringId::IllegalArgumentException);
	return count;
}

size_t String::fetchIndex(const Reference& ref, bool allowBoundary) const {
	auto rawIndex = static_cast<ptrdiff_t>(ref.getInt());
	auto index = static_cast<size_t>(rawIndex >= 0 ? rawIndex : length + rawIndex);
	if (index >= length) {
		if (index == length && allowBoundary)
			return index;
		throw RuntimeException(StringId::IllegalArgumentException);
	}
	return index;
}

size_t String::getPosition(size_t index) const {
	if (index == 0)
		return 0;
	size_t position = positionHash[index / positionHashPitch];
	size_t left = index % positionHashPitch;
	for (size_t i = 0; i < left; i++)
		position += byteCount(value, position);
	return position;
}

void String::rehash() {
	length = 0;
	positionHash.clear();
	positionHash.reserve(value.size() / positionHashPitch);
	for (size_t i = 0; i < value.size(); length++) {
		size_t count = byteCount(value, i);
		if (count == 0)
			throw RuntimeException(StringId::IllegalArgumentException);
		if (length % positionHashPitch == 0)
			positionHash.emplace_back(i);
		i += count;
	}
}

const Class* String::getClassStatic() {
	return clString.get();
}

bool String::save(Writer& w) const {
	w.out(value);
	return true;
}

String::String(Reader& r) noexcept : ObjectBase(typeId_String, getClassStatic()) {
	(void)r;
}

bool String::restore(chatra::Reader& r) {
	r.in(value);
	rehash();
	return true;
}

static const char* initString = R"***(
class String
	def init()

	def init.fromString(a0: String) as native

	def init.fromChar(a0: Int) as native

	def size() as native

	def set(a0: String) as native

	def add(a0...;)
		t0 = a0.size()
		t1 = 0
		while t1 < t0
			t2 = a0[t1++]
			if !(t2 is Int)
				throw IllegalArgumentException()
			_native_add(t2)
		return self

	def _native_add(a0) as native
	def add(a0: Int)
		_native_add(a0)
		return self

	def add(a0: String)
		_native_insert(size, a0)
		return self

	def insert(position: Int, a0...;)
		t0 = a0.size()
		t1 = 0
		while t1 < t0
			t2 = a0[t1]
			if !(t2 is Int)
				throw IllegalArgumentException()
			_native_insert(position + t1, t2)
			t1++
		return self

	def _native_insert(a0, a1) as native
	def insert(position: Int, a0: Int)
		_native_insert(position, a0)
		return self

	def insert(position: Int, a0: String)
		_native_insert(position, a0)
		return self

	def _native_append(a0) as native
	def append(a0)
		if a0 == null
			_native_append("null")
		else if a0 is String or a0 is Bool or a0 is Int or a0 is Float
			_native_append(a0)
		else
			_native_append(a0.toString())
		return self

	def _native_at(a0) as native
	def (a0: Int)
		return _native_at(a0)

	def _native_at(a0, a1) as native
	def (a0: Int).set(r: Int)
		return _native_at(a0, r)

	def remove(position: Int) as native

	def remove(a0: IndexSet)
		t0 = ''
		if a0.first == a0.last
			return t0
		t2 = size
		if a0.ascend
			for t1 in a0.reverse
				t0.insert(0, remove(t1 >= 0 ? t1 : t2 + t1))
		else
			for t1 in a0
				t0.add(remove(t1 >= 0 ? t1 : t2 + t1))
		return t0

	def clear()
		set('')
		return self

	def clone() as native

	def equals(a0) as native

	def _native_sub(a0, a1) as native
	def sub(first: Int)
		return _native_sub(first, size)

	def sub(first: Int, last: Int)
		return _native_sub(first, last)

	def iterator()
		return ArrayIterator(self)

	def keyedIterator()
		return ArrayKeyedIterator(self)

	def toArray()
		return Array().append(self)

	def toString()
		return clone()
)***";

void String::native_initFromString(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	auto& r = args.ref(0).deref<String>();
	self.value = r.value;
	self.length = r.length;
	self.positionHash = r.positionHash;
}

void String::native_initFromChar(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto c = args.ref(0).getInt();
	char buffer[4];
	size_t count = extractCharOrThrow(static_cast<char32_t>(c), buffer);

	self.value.insert(self.value.cbegin(), buffer, buffer + count);
	self.length = 1;
	self.positionHash.emplace_back(0);
}

void String::native_size(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	ret.setInt(static_cast<int64_t>(self.length));
}

void String::native_set(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	native_initFromString(CHATRA_NATIVE_ARGS_FORWARD);
}

void String::native_add(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	char buffer[4];
	size_t count = extractCharOrThrow(static_cast<char32_t>(args.ref(0).getInt()), buffer);
	self.value.append(buffer, buffer + count);
	self.rehash();
}

void String::native_insert(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	size_t position = self.getPosition(self.fetchIndex(args.ref(0), true));

	auto ref = args.ref(1);
	switch (ref.valueType()) {
	case ReferenceValueType::Int: {
		char buffer[4];
		size_t count = extractCharOrThrow(static_cast<char32_t>(ref.getInt()), buffer);
		self.value.insert(self.value.cbegin() + position, buffer, buffer + count);
		break;
	}

	case ReferenceValueType::Object: {
		if (ref.deref<ObjectBase>().getClass() != String::getClassStatic())
			throw RuntimeException(StringId::IllegalArgumentException);
		self.value.insert(position, ref.deref<String>().value);
		break;
	}

	default:
		throw RuntimeException(StringId::IllegalArgumentException);
	}

	self.rehash();
}

void String::native_append(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto ref = args.ref(0);
	switch (ref.valueType()) {
	case ReferenceValueType::Bool:  self.value += (ref.getBool() ? "true" : "false");  break;
	case ReferenceValueType::Int:  self.value += std::to_string(ref.getInt());  break;
	case ReferenceValueType::Float:  self.value += std::to_string(ref.getFloat());  break;
	default:
		if (ref.deref<ObjectBase>().getClass() != String::getClassStatic())
			throw RuntimeException(StringId::IllegalArgumentException);
		self.value += ref.deref<String>().value;
		break;
	}
	self.rehash();
}

void String::native_at(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	size_t position = self.getPosition(self.fetchIndex(args.ref(0)));

	char32_t c0 = getCharOrThrow(self.value, position);
	if (args.size() == 1) {
		ret.setInt(static_cast<int64_t>(c0));
		return;
	}

	char32_t c1 = static_cast<char32_t>(args.ref(1).getInt());
	ret.setInt(static_cast<int64_t>(c1));

	size_t c0Count = byteCount(self.value, position);
	char buffer[4];
	size_t c1Count = extractCharOrThrow(c1, buffer);
	self.value.replace(self.value.cbegin() + position, self.value.cbegin() + position + c0Count, buffer, buffer + c1Count);
	self.rehash();
}

void String::native_remove(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	size_t position = self.getPosition(self.fetchIndex(args.ref(0)));
	ret.setInt(static_cast<int64_t>(getCharOrThrow(self.value, position)));

	size_t count = byteCount(self.value, position);
	self.value.erase(position, count);
	self.rehash();
}

void String::native_clone(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	ret.allocate<String>().setValue(self.value);
}

void String::native_equals(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;

	auto value = args.ref(0);
	if (value.isNull() || value.valueType() != ReferenceValueType::Object ||
			value.deref<ObjectBase>().getClass() != getClassStatic()) {
		ret.setBool(false);
		return;
	}

	std::string lValue;
	{
		std::lock_guard<SpinLock> lock(self.lockValue);
		lValue = self.value;
	}

	std::string rValue;
	{
		auto& target = value.deref<String>();
		std::lock_guard<SpinLock> lock(target.lockValue);
		rValue = target.value;
	}

	ret.setBool(lValue == rValue);
}

void String::native_sub(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	size_t position0 = self.getPosition(self.fetchIndex(args.ref(0)));
	size_t position1 = self.getPosition(self.fetchIndex(args.ref(1), true));
	if (position0 > position1)
		throw RuntimeException(StringId::IllegalArgumentException);

	ret.allocate<String>().setValue(self.value.substr(position0, position1 - position0));
}


ContainerBody::ContainerBody(Storage& storage, size_t capacity) noexcept
		: Object(storage, typeId_ContainerBody, capacity, Object::ElementsAreExclusive()) {}

bool ContainerBody::save(Writer& w) const {
	(void)w;
	return false;
}

ContainerBody::ContainerBody(Reader& r) noexcept : Object(typeId_ContainerBody) {
	(void)r;
}

bool ContainerBody::restore(Reader& r) {
	(void)r;
	return false;
}

void ContainerBase::extend(size_t required) {
	if (required <= capacity)
		return;

	if (capacity == 0) {
		capacity = ref(0).deref().size();
		if (required <= capacity)
			return;
	}

	capacity = std::max(required, capacity * 3 / 2);
	auto& o1 = ref(1).allocate<ContainerBody>(capacity);
	auto& o0 = ref(0).deref();
	for (size_t i = 0; i < length; i++)
		o1.ref(i).set(o0.ref(i));
	ref(0).set(ref(1));
}

Object& ContainerBase::container() const {
	return ref(0).deref();
}

ContainerBase::ContainerBase(Storage& storage, TypeId typeId, const Class* cl) noexcept
		: ObjectBase(storage, typeId, cl, 2, ObjectBase::ElementsAreExclusive()) {
	ref(0).allocate<ContainerBody>(capacity);
}

ContainerBase::ContainerBase(TypeId typeId, const Class* cl) noexcept : ObjectBase(typeId, cl), capacity(0) {
}

static void createArray(const Class* cl, Reference ref) {
	(void)cl;
	assert(cl == clArray.get());
	ref.allocate<Array>();
}

size_t Array::fetchIndex(const Reference& ref, bool allowBoundary) const {
	auto rawIndex = static_cast<ptrdiff_t>(ref.getInt());
	auto index = static_cast<size_t>(rawIndex >= 0 ? rawIndex : length + rawIndex);
	if (index >= length) {
		if (index == length && allowBoundary)
			return index;
		throw RuntimeException(StringId::IllegalArgumentException);
	}
	return index;
}

const Class* Array::getClassStatic() {
	return clArray.get();
}

void Array::add(Reference ref) {
	std::lock_guard<SpinLock> lock(lockValue);
	extend(length + 1);
	container().ref(length++).set(ref);
}

bool Array::save(Writer& w) const {
	w.out(length);
	return false;
}

Array::Array(Reader& r) noexcept : ContainerBase(typeId_Array, getClassStatic()) {
	(void)r;
}

bool Array::restore(Reader& r) {
	r.in(length);
	return false;
}

static const char* initArray = R"***(
class Array
	def init()

	def size() as native

	def has(a0)
		t0 = size()
		t1 = 0
		while t1 < t0
			if _native_at(t1++) == a0
				return true
		return false

	def _native_add(a0) as native
	def add(a0...;)
		t0 = a0.size()
		t1 = 0
		while t1 < t0
			_native_add(a0[t1++])
		return self

	def add(a0)
		_native_add(a0)
		return self

	def insert(position: Int, a0...;)
		t0 = a0.size()
		t1 = 0
		while t1 < t0
			_native_insert(position + t1, a0[t1])
			t1++
		return self

	def _native_insert(a0, a1) as native
	def insert(position: Int, a0)
		_native_insert(position, a0)
		return self

	def append(a0)
		if a0 == null
			return self
		for t0 in a0
			add(t0)
		return self

	def _native_at(a0) as native
	def (position: Int)
		return _native_at(position)

	def _native_at(a0, a1) as native
	def (position: Int).set(r)
		return _native_at(position, r)

	def remove(position: Int) as native

	def remove(a0: IndexSet)
		t0 = Array()
		if a0.first == a0.last
			return t0
		t2 = size
		if a0.ascend
			for t1 in a0.reverse
				t0.insert(0, remove(t1 >= 0 ? t1 : t2 + t1))
		else
			for t1 in a0
				t0.add(remove(t1 >= 0 ? t1 : t2 + t1))
		return t0

	def clear()
		while size > 0
			remove(size - 1)
		return self

	def clone()
		return Array().append(self)

	def equals(a0)
		if a0 == null or !(a0 is Array or a0 is ArrayView)
			return false
		if size != a0.size
			return false
		t0 = size
		t1 = 0
		while t1 < t0
			if self[t1] != a0[t1]
				return false
			t1++
		return true

	def sub(a0: IndexSet)
		return ArrayView(self, a0)

	def iterator()
		return ArrayIterator(self)

	def keyedIterator()
		return ArrayKeyedIterator(self)

	def toArray()
		return clone()

	def toString()
		t0 = '{'

		if size > 0
			t3 = self[0]
			if t3 is String
				t0.append('"').append(t3).append('"')
			else
				t0.append(t3)

			t1 = size()
			t2 = 1
			while t2 < t1
				t0.append(', ')
				t3 = self[t2++]
				if t3 is String
					t0.append('"').append(t3).append('"')
				else
					t0.append(t3)

		return t0.append('}')
)***";

void Array::native_size(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	ret.setInt(static_cast<int64_t>(self.length));
}

void Array::native_add(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	self.extend(self.length + 1);
	self.container().ref(self.length++).set(args.ref(0));
}

void Array::native_insert(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto index = self.fetchIndex(args.ref(0), true);

	self.extend(self.length + 1);
	auto& c = self.container();
	for (auto i = self.length; i-- > index; )
		c.ref(i + 1).set(c.ref(i));
	c.ref(index).set(args.ref(1));
	self.length++;
}

void Array::native_remove(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto index = self.fetchIndex(args.ref(0));

	auto& c = self.container();
	ret.set(c.ref(index));
	while (++index < self.length)
		c.ref(index - 1).set(c.ref(index));
	c.ref(--self.length).setNull();
}

void Array::native_at(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto index = self.fetchIndex(args.ref(0));

	auto ref0 = self.container().ref(index);
	if (args.size() == 1) {
		ret.set(ref0);
		return;
	}

	ref0.set(args.ref(1));
	ret.set(ref0);
}

static void createDict(const Class* cl, Reference ref) {
	(void)cl;
	assert(cl == clDict.get());
	ref.allocate<Dict>();
}

const Class* Dict::getClassStatic() {
	return clDict.get();
}

void Dict::add(std::string key, Reference ref) {
	std::lock_guard<SpinLock> lock(lockValue);
	auto it = keyToIndex.find(key);
	if (it != keyToIndex.cend()) {
		container().ref(it->second).set(ref);
		return;
	}

	size_t index;
	if (freeIndexes.empty()) {
		index = keyToIndex.size();
		extend(index + 1);
	}
	else {
		index = freeIndexes.back();
		freeIndexes.pop_back();
	}

	keyToIndex.emplace(key, index);
	length++;
	container().ref(index).set(ref);
}

bool Dict::remove(const std::string& key, Reference& ret) {
	std::lock_guard<SpinLock> lock(lockValue);
	auto it = keyToIndex.find(key);
	if (it == keyToIndex.cend())
		return false;

	size_t index = it->second;
	auto ref = container().ref(index);
	ret.set(ref);
	ref.setNull();
	keyToIndex.erase(it);
	freeIndexes.emplace_back(index);
	length--;
	return true;
}

bool Dict::save(Writer& w) const {
	w.out(length);
	w.out(keyToIndex, [&](const std::pair<std::string, size_t>& e) {
		w.out(e.first);
		w.out(e.second);
	});

	size_t maxIndex = 0;
	for (auto& e : keyToIndex)
		maxIndex = std::max(maxIndex, e.second);
	for (auto index : freeIndexes)
		maxIndex = std::max(maxIndex, index);
	w.out(maxIndex);
	return false;
}

Dict::Dict(Reader& r) noexcept : ContainerBase(typeId_Dict, getClassStatic()) {
	(void)r;
}

bool Dict::restore(Reader& r) {
	r.in(length);
	r.inList([&]() {
		auto key = r.read<std::string>();
		auto index = r.read<size_t>();
		keyToIndex.emplace(std::move(key), index);
	});

	auto maxIndex = r.read<size_t>();
	std::vector<bool> indexes(maxIndex + 1, false);
	for (auto& e : keyToIndex)
		indexes[e.second] = true;
	for (size_t i = 0; i <= maxIndex; i++) {
		if (!indexes[i])
			freeIndexes.emplace_back(i);
	}
	return false;
}

static const char* initDict = R"***(
class Dict
	def init()

	def size() as native

	def has(a0: String) as native

	def add(; a0...)
		t0 = a0.keys()
		for t1 in t0
			_native_add(t1, a0[t1])
		return self

	def _native_add(a0: String, a1) as native
	def add(key : String, value)
		_native_add(key, value)
		return self

	def append(a0)
		if a0 == null
			return self
		for key, value in a0
			if !(key is String)
				throw IllegalArgumentException()
			add(key, value)
		return self

	def _native_at(a0) as native
	def (key: String)
		if key == null
			throw IllegalArgumentException()
		return _native_at(key)

	def (key: String).set(r)
		_native_add(key, r)
		return r

	def remove(a0)
		t0 = Dict()
		for t1 in a0
			if !(t1 is String)
				throw IllegalArgumentException()
			t0.add(t1, remove(t1))
		return t0

	def remove(key: String) as native

	def clear()
		remove(keys())
		return self

	def clone()
		return Dict().append(self)

	def equals(a0)
		if a0 == null or !(a0 is Dict)
			return false
		if size != a0.size
			return false
		for t0 in keys
			if !a0.has(t0) or self[t0] != a0[t0]
				return false
		return true

	def iterator()
		return DictIterator(self)

	def keyedIterator()
		return DictKeyedIterator(self)

	def keys() as native

	def values() as native

	def toDict()
		return clone()

	def toString()
		t0 = '{'
		t1 = 0
		for key, value in self
			if t1++ != 0
				t0.append(', ')
			t0.append('"').append(key).append('": ')
			if value is String
				t0.append('"').append(value).append('"')
			else
				t0.append(value)
		return t0.append('}')
)***";

void Dict::native_size(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	ret.setInt(static_cast<int64_t>(self.length));
}

void Dict::native_has(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);
	auto it = self.keyToIndex.find(args.ref(0).deref<String>().getValue());
	ret.setBool(it != self.keyToIndex.cend());
}

void Dict::native_add(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	self.add(args.ref(0).deref<String>().getValue(), args.ref(1));
}

void Dict::native_at(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto key = args.ref(0).deref<String>().getValue();
	auto it = self.keyToIndex.find(key);
	if (it == self.keyToIndex.cend())
		throw RuntimeException(StringId::IllegalArgumentException);
	else
		ret.set(self.container().ref(it->second));
}

void Dict::native_remove(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	if (!self.remove(args.ref(0).deref<String>().getValue(), ret))
		throw RuntimeException(StringId::IllegalArgumentException);
}

void Dict::native_keys(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto& retValue = ret.allocate<Array>(self.keyToIndex.size());
	size_t index = 0;
	for (auto& e : self.keyToIndex)
		retValue.container().ref(index++).allocate<String>().setValue(e.first);
}

void Dict::native_values(CHATRA_NATIVE_ARGS) {
	CHATRA_NATIVE_SELF;
	std::lock_guard<SpinLock> lock(self.lockValue);

	auto& retValue = ret.allocate<Array>(self.keyToIndex.size());
	size_t index = 0;
	for (auto& e : self.keyToIndex)
		retValue.container().ref(index++).set(self.container().ref(e.second));
}

template <StringId name>
static void createException(const Class* cl, Reference ref) {
	(void)cl;
	ref.allocate<Exception<name>>();
}

template <StringId name>
static void registerException() {
	auto* cl = PreDefined<Exception<name>, name>::getWritableClassStatic();
	cl->addDefaultConstructor(createException<name>);
	embeddedClasses.add(cl);
}

template <StringId name, StringId baseName>
static void registerException_Derived() {
	auto* cl = PreDefined<Exception<name>, name>::getWritableClassStatic();
	cl->addDefaultConstructor(createException<name>);
	cl->addParentClass(PreDefined<Exception<baseName>, baseName>::getClassStatic());
	embeddedClasses.add(cl);
}

static std::unique_ptr<Class> createEmbeddedClass(ParserWorkingSet& ws, IErrorReceiver& errorReceiver,
		std::shared_ptr<StringTable>& sTable, IClassFinder& classFinder, std::string script,
		ObjectBuilder nativeConstructor, std::vector<std::tuple<StringId, StringId, NativeMethod>> nativeMethods) {

	auto lines = parseLines(errorReceiver, sTable, "(internal-classes)", 1, std::move(script));
	auto node = groupScript(errorReceiver, sTable, lines);
	structureInnerNode(errorReceiver, sTable, node.get(), true);
	parseInnerNode(ws, errorReceiver, sTable, node.get(), true);
	assert(node->blockNodes.size() == 1 && node->blockNodes[0]->type == NodeType::Class);

	nodeMap.emplace(node->blockNodes[0]->sid, node.get());

	std::unique_ptr<Class> cl(new Class(errorReceiver, sTable.get(), classFinder, nullptr, node->blockNodes[0].get(),
			nativeConstructor, std::move(nativeMethods)));
	embeddedClasses.add(cl.get());
	embeddedClassesNode.emplace_front(std::move(node));
	return cl;
}

void initializeEmbeddedClasses() {
	registerException<StringId::Exception>();
	registerException_Derived<StringId::RuntimeException, StringId::Exception>();
	registerException_Derived<StringId::ArithmeticException, StringId::RuntimeException>();

	registerException_Derived<StringId::ParserErrorException, StringId::Exception>();
	registerException_Derived<StringId::UnsupportedOperationException, StringId::Exception>();
	registerException_Derived<StringId::PackageNotFoundException, StringId::Exception>();
	registerException_Derived<StringId::ClassNotFoundException, StringId::Exception>();
	registerException_Derived<StringId::MemberNotFoundException, StringId::Exception>();
	registerException_Derived<StringId::IncompleteExpressionException, StringId::Exception>();
	registerException_Derived<StringId::TypeMismatchException, StringId::Exception>();
	registerException_Derived<StringId::NullReferenceException, StringId::RuntimeException>();
	registerException_Derived<StringId::OverflowException, StringId::ArithmeticException>();
	registerException_Derived<StringId::DivideByZeroException, StringId::ArithmeticException>();
	registerException_Derived<StringId::IllegalArgumentException, StringId::RuntimeException>();
	registerException_Derived<StringId::NativeException, StringId::Exception>();

	embeddedClasses.add(Bool::getClassStatic());
	embeddedClasses.add(Int::getClassStatic());
	embeddedClasses.add(Float::getClassStatic());

	ParserWorkingSet ws;

	IAssertionNullErrorReceiver nullErrorReceiver;
	IErrorReceiverBridge errorReceiver(nullErrorReceiver);

	std::shared_ptr<StringTable> sTable = StringTable::newInstance();

	struct ClassFinder : public IClassFinder {
		const Class* findClass(StringId name) override {
			return embeddedClasses.find(name);
		}
		const Class* findPackageClass(StringId packageName, StringId name) override {
			(void)packageName;
			(void)name;
			return nullptr;
		}
	} classFinder;

	auto addClass = [&](const char* init) {
		embeddedClassesPtr.emplace_front(
				createEmbeddedClass(ws, errorReceiver, sTable, classFinder, init, nullptr, {}));
	};

	addClass(initIterator);
	addClass(initKeyedIterator);
	addClass(initIndexSet);
	addClass(initRangeIterator);
	addClass(initKeyedRangeIterator);
	addClass(initRange);
	addClass(initArrayIterator);
	addClass(initArrayKeyedIterator);
	addClass(initDictIterator);
	addClass(initDictKeyedIterator);
	addClass(initArrayView);

	/*embeddedClassesPtr.emplace_front(
			createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initArrayIterator, nullptr, {}));
	embeddedClassesPtr.emplace_front(
			createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initArrayKeyedIterator, nullptr, {}));
			*/

	clAsync = createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initAsync, createAsync, {
			{StringId::_native_updated, StringId::Invalid, NativeMethod(&Async::native_updated)},
	});

	clString = createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initString, createString, {
			{StringId::Init, StringId::fromString, NativeMethod(&String::native_initFromString)},
			{StringId::Init, StringId::fromChar, NativeMethod(&String::native_initFromChar)},
			{StringId::size, StringId::Invalid, NativeMethod(&String::native_size)},
			{StringId::Set, StringId::Invalid, NativeMethod(&String::native_set)},
			{StringId::_native_add, StringId::Invalid, NativeMethod(&String::native_add)},
			{StringId::_native_insert, StringId::Invalid, NativeMethod(&String::native_insert)},
			{StringId::_native_append, StringId::Invalid, NativeMethod(&String::native_append)},
			{StringId::_native_at, StringId::Invalid, NativeMethod(&String::native_at)},
			{StringId::remove, StringId::Invalid, NativeMethod(&String::native_remove)},
			{StringId::clone, StringId::Invalid, NativeMethod(&String::native_clone)},
			{StringId::equals, StringId::Invalid, NativeMethod(&String::native_equals)},
			{StringId::_native_sub, StringId::Invalid, NativeMethod(&String::native_sub)}
	});

	clArray = createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initArray, createArray, {
			{StringId::size, StringId::Invalid, NativeMethod(&Array::native_size)},
			{StringId::_native_add, StringId::Invalid, NativeMethod(&Array::native_add)},
			{StringId::_native_insert, StringId::Invalid, NativeMethod(&Array::native_insert)},
			{StringId::remove, StringId::Invalid, NativeMethod(&Array::native_remove)},
			{StringId::_native_at, StringId::Invalid, NativeMethod(&Array::native_at)}
	});

	clDict = createEmbeddedClass(ws, errorReceiver, sTable, classFinder, initDict, createDict, {
			{StringId::size, StringId::Invalid, NativeMethod(&Dict::native_size)},
			{StringId::has, StringId::Invalid, NativeMethod(&Dict::native_has)},
			{StringId::_native_add, StringId::Invalid, NativeMethod(&Dict::native_add)},
			{StringId::_native_at, StringId::Invalid, NativeMethod(&Dict::native_at)},
			{StringId::remove, StringId::Invalid, NativeMethod(&Dict::native_remove)},
			{StringId::keys, StringId::Invalid, NativeMethod(&Dict::native_keys)},
			{StringId::values, StringId::Invalid, NativeMethod(&Dict::native_values)}
	});

	// Add embedded conversions
	Bool::getWritableClassStatic()->addConvertingConstructor(Int::getClassStatic());
	Bool::getWritableClassStatic()->addConvertingConstructor(clString.get());
	Int::getWritableClassStatic()->addConvertingConstructor(Bool::getClassStatic());
	Int::getWritableClassStatic()->addConvertingConstructor(Float::getClassStatic());
	Int::getWritableClassStatic()->addConvertingConstructor(clString.get());
	Float::getWritableClassStatic()->addConvertingConstructor(Bool::getClassStatic());
	Float::getWritableClassStatic()->addConvertingConstructor(Int::getClassStatic());
	Float::getWritableClassStatic()->addConvertingConstructor(clString.get());

	if (errorReceiver.hasError())
		throw InternalError();

	if (sTable->getVersion() != 0) {
#ifndef NDEBUG
		printf("Additional strings:");
		for (auto i = static_cast<size_t>(StringId::PredefinedStringIds); i < sTable->validIdCount(); i++)
			printf(" %s", sTable->ref(static_cast<StringId>(i)).c_str());
		printf("\n");
#endif
		throw InternalError();
	}
}

ClassTable& refEmbeddedClassTable() {
	return embeddedClasses;
}

void registerEmbeddedClasses(ClassTable& classes) {
	classes.add(embeddedClasses);
}

const std::unordered_map<StringId, Node*>& refNodeMapForEmbeddedClasses() {
	return nodeMap;
}


#ifndef NDEBUG
void String::dump(const std::shared_ptr<chatra::StringTable>& sTable) const {
	printf("String: value=\"%s\" ", value.c_str());
	ObjectBase::dump(sTable);
}

void Array::dump(const std::shared_ptr<chatra::StringTable>& sTable) const {
	printf("Array: capacity=%u, length=%u ", static_cast<unsigned>(getCapacity()), static_cast<unsigned>(length));
	container().dump(sTable);
}

void Dict::dump(const std::shared_ptr<chatra::StringTable>& sTable) const {
	printf("Dict: capacity=%u, length=%u, free=%u ", static_cast<unsigned>(getCapacity()), static_cast<unsigned>(length),
			static_cast<unsigned>(freeIndexes.size()));
	container().dump(sTable);
	std::vector<std::pair<size_t, std::string>> indexToKey;
	for (auto& e : keyToIndex)
		indexToKey.emplace_back(e.second, e.first);
	std::sort(indexToKey.begin(), indexToKey.end(), [](const std::pair<size_t, std::string>& a, const std::pair<size_t, std::string>& b) {
		return a.first < b.first;
	});
	for (auto& e : indexToKey)
		printf("  [%u] <- \"%s\"\n", static_cast<unsigned>(e.first), e.second.c_str());
}
#endif // !NDEBUG

}  // namespace chatra
