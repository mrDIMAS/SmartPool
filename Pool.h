/*
The MIT License

Copyright(c) 2017 Stepanov Dmitriy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.


=============
Overview:
Pool serves for the few goals: eliminates unnecessary memory allocations,
preserves data locality, gives mechanism of control for object indices.

This pool provides user the easy way of control for spawned objects through
special handles. Why we can't use ordinary pointers? Answer is simple: when pool
grows, it's memory block may change it's position in memory and all pointers
will become invalid. To avoid this problem, we use handles.

What is "handle"? Handle is something like index, but with additional
information, that allows us to ensure that handle "points" to same object as
before. This additional info called "stamp". When you asks pool for a new
object, pool marks it with unique "stamp" and gives you handle with index of a
new object and "stamp" of an object. Now if you want to ensure, that handle
"points" to same object as before, you just compare "stamps" - if they are same,
then handle is correct.

Pool implementation is object-agnostic, so you can store any suitable object in
it. It also may contain non-POD objects.

=============
How it works:
Firstly, pool allocates memory block with initial size = initialCapacity *
recordSize, fills it with special marks, which indicates that memory piece is
unused. Also pool creates list of indices of free memory blocks.

When user calls Spawn method, pool pops index of free memory block, constructs
object in it using placement new, makes new stamp and returns handle to the
user.

When user calls Return methos, pool returns index of the object to "free list",
marks object as free and calls destructor of the object.

=============
Notes:
Your object should have constructor without arguments.
Create pool with large enough capacity, because any Spawn method called on full
pool will result in memory reallocation and memory movement, which is quite
expensive operation

=============
Q&A:

Q: How fast is Spawn method?
A: Spawn method has amortized complexity of O(1).

Q: How fast is Return method?
A: Return method has amortized complexity of O(1).

=============
Example:

class Foo {
private:
  int fooBar;
public:
  Foo() {}
  Foo(int foobar) : fooBar(foobar) {}
};

Pool<Foo> pool(1024); // make pool with default capacity of 1024 objects
Handle<Foo> bar = pool.Spawn(); // spawn object with default constructor
Handle<Foo> baz = pool.Spawn(42); // spawn object with any args

// do something

pool.Return(bar);
pool.Return(baz);
*/

#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string.h>
#include <utility>
#include <queue>

using Index = uint32_t;
using Stamp = uint32_t;

enum { NotConstructedStamp, FreeStamp, StampOrigin };

template <typename T> class Pool;

template <typename T> class Handle {
private:
  friend class Pool<T>;
  Index mIndex{0};
  Stamp mStamp{FreeStamp};
  Handle(Index index, Stamp stamp) : mIndex(index), mStamp(stamp) {
  }

public:
  Handle() {
  }
  ~Handle() {
  }
};

template <typename T> class PoolRecord final {
private:
  friend class Pool<T>;
  Stamp mStamp{FreeStamp};

public:
  T mObject;
  template <typename... Args>
  PoolRecord(int stamp, Args &&... args) : mStamp(stamp), mObject(args...) {
  }
  PoolRecord() {
  }
  ~PoolRecord() {
  }
  bool IsValid() const noexcept {
    return mStamp != FreeStamp;
  }
  const T *operator->() const {
    return &mObject;
  }
  T *operator->() {
    return &mObject;
  }
};

template <typename T> class Pool final {
private:
  static constexpr float GrowRate = 1.5f;
  static_assert(GrowRate > 1.0f, "Grow rate must be greater than 1");

  size_t mSpawnedCount{0};
  Stamp mGlobalStamp{StampOrigin};
  PoolRecord<T> *mRecords{nullptr};
  size_t mCapacity{0};
  std::queue<Index> mFreeQueue;

  Stamp MakeStamp() noexcept {
    return mGlobalStamp++;
  }
  void DestroyObjects(PoolRecord<T> *ptr, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      // Destruct only busy objects, not the free ones (they are already
      // destructed)
      if (ptr[i].mStamp != FreeStamp && ptr[i].mStamp != NotConstructedStamp) {
        ptr[i].mObject.~T();
      }
    }
    free(ptr);
  }

  // Allocates new memory block
  void AllocMemory() {
    const size_t sizeBytes = sizeof(PoolRecord<T>) * mCapacity;
    mRecords = reinterpret_cast<PoolRecord<T> *>(malloc(sizeBytes));
    memset(mRecords, NotConstructedStamp, sizeBytes);
  }

  Index GetFreeIndex() {
    const auto index = mFreeQueue.front();
    mFreeQueue.pop();
    return index;
  }

public:
  // baseSize - base count of preallocated objects in the pool
  Pool(size_t baseSize) : mCapacity(baseSize) {
    AllocMemory();
    for (Index i = 0; i < baseSize; ++i) {
      mFreeQueue.push(i);
    }
  }
  ~Pool() {
    Clear();
  }
  // Returns handle to free object, or if pool is full
  // allocates new object and returns handle to it
  //
  // Throws std::bad_alloc when unable to allocate memory
  template <typename... Args> Handle<T> Spawn(Args &&... args) {
    if (mFreeQueue.empty()) {
      // No free objects, grow pool
      const auto oldCapacity = mCapacity;
      if (mCapacity == 0) {
        mCapacity = 1;
      } else {
        mCapacity = static_cast<size_t>(ceil(mCapacity * GrowRate));
      }
      // Remember old records
      const auto old = mRecords;
      // Create new array with larger size
      AllocMemory();
      if (!mRecords) {
        throw std::bad_alloc();
      }
      // Register new free indices
      for (Index i = oldCapacity; i < mCapacity; ++i) {
        mFreeQueue.push(i);
      }
      for (size_t i = 0; i < oldCapacity; ++i) {
        if (old[i].mStamp != NotConstructedStamp) {
          // Constructed objects will be copied (moved in ideal case) from the
          // old array
          new (&mRecords[i]) PoolRecord<T>(std::move(old[i]));
        }
      }
      // Remove old array of records
      DestroyObjects(old, oldCapacity);
    }

    const auto index = GetFreeIndex();
    auto &rec = mRecords[index];
    // Reconstruct record via placement new
    new (&rec) PoolRecord<T>(MakeStamp(), args...);
    ++mSpawnedCount;
    // Return handle to existing object
    return Handle<T>{index, rec.mStamp};
  }

  // Will return object with 'handle' to the pool
  // Calls destructor of returnable object
  void Return(const Handle<T> &handle) {
    assert(handle.mIndex < mCapacity);
    auto &rec = mRecords[handle.mIndex];
    if (rec.mStamp != FreeStamp) {
      rec.mStamp = FreeStamp;
      // Destruct
      rec.mObject.~T();
      --mSpawnedCount;
      // Register handle's index as free
      mFreeQueue.push(handle.mIndex);
    }
  }
  void Clear() {
    DestroyObjects(mRecords, mCapacity);
    // Clear free indices queue
    mFreeQueue = std::queue<Index>();
    mRecords = nullptr;
    mCapacity = 0;
    mGlobalStamp = StampOrigin;
    mSpawnedCount = 0;
  }
  // Returns true if 'handle' corresponds to object, that handle indexes
  bool IsValid(const Handle<T> &handle) noexcept {
    assert(handle.mIndex < mCapacity);
    return handle.mStamp == mRecords[handle.mIndex].mStamp;
  }
  // Returns reference to object by its handle
  // You should check handle to validity thru IsValid before pass it to method
  T &At(const Handle<T> &handle) const {
    assert(handle.mIndex < mCapacity);
    return mRecords[handle.mIndex].mObject;
  }
  // Same as At
  T &operator[](const Handle<T> &handle) const {
    return At(handle);
  }
  size_t GetSpawnedCount() const noexcept {
    return mSpawnedCount;
  }
  size_t GetCapacity() const noexcept {
    return mCapacity;
  }
  PoolRecord<T> *GetRecords() noexcept {
    return mRecords;
  }
  // range-based for
  PoolRecord<T> *begin() {
    return mRecords;
  }
  PoolRecord<T> *end() {
    return mRecords + mCapacity - 1;
  }
  // Use this only to obtain handle by 'this' pointer inside class method
  // Note: Do not rely on pointers to objects in pool, they can suddenly become
  // invalid. 'this' pointer can be used, because it is always valid
  Handle<T> HandleByPointer(const T *ptr) const {
    // Pointer must be in bounds
    if (ptr < &mRecords[0].mObject || ptr >= &mRecords[mCapacity - 1].mObject) {
      return Handle<T>();
    }

    // Offset ptr to get record
    const auto record = reinterpret_cast<const PoolRecord<T> *>(
        reinterpret_cast<const char *>(ptr) - sizeof(Stamp));

    const auto p0 = reinterpret_cast<const intptr_t>(&mRecords[0]);
    const auto pn = reinterpret_cast<const intptr_t>(record);

    // Calculate index
    const intptr_t distance = pn - p0;
    const intptr_t index = distance / sizeof(PoolRecord<T>);

    return Handle<T>(index, record->mStamp);
  }
};
