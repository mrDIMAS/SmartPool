/*
=============
Overview:
Pool serves for the few goals: eliminates unnecessary memory allocations,
preserves data locality, gives mechanism of control for object indices.

This pool provides user the easy way of control for spawned objects thru special
handles. Why we can't use ordinary pointers? Answer is simple: when pool grows,
it's memory block may change it's position in memory and all pointers will
become invalid. To avoid this problem, we use indices.

Objects inside of a pool stored in continuous array, which is good for cpu
cache.

Pool implementation is object-agnostic, so you can store any suitable object in
it. It also may contain non-POD objects.

=============
How it works:
Allocate memory block with initial size = capacity * recordSize, fill it with
zeros

The main idea of this pool is to provide user not object or pointers itselves,
but to provide special handles to objects inside of a pool. Such handles
contains actual object index in the pool's array and special stamp field. Stamp
field used to ensure, that the handle is a handle to the right object. In other
words, you spawn object A from a pool, store it index in few places, then you
return A to the pool. Then you spawn a new object, it overwrites our object A.
Now you have few invalid indices to unexisted object A in different parts of
your app. How can you ensure that some index is an index to object A? Correct
answer is: you can't. But if you add some additional "stamp" info to the index,
you can ensure that you have correct handle just by comparing "stamp" of the
index and 'stamp' of the object. Such "index + stamp" construct called handle.

=============
Notes:
Your object should have constructor without arguments.
Create pool with large enough capacity, because any Spawn method called on full
pool will result in memory reallocation and memory movement, which is quite
expensive operation

=============
Q&A:

Q: How fast this pool?
A: Well, performance relies on correct usage of this pool. First of all, you
should create this pool with large enough capacity to reduce reallocations of
memory. Secondly, performance of the Spawn method is O(n) (where n - count of
free objects) in worst case.


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
