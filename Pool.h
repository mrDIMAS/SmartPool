/*
The MIT License

Copyright(C) 2017-2018 Stepanov Dmitriy a.k.a mr.DIMAS a.k.a v1al

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

#ifndef SMART_POOL_H
#define SMART_POOL_H

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string.h>
#include <utility>
#include <queue>

// Use large enough in types to hold stamps and indices
// uint64_t will overflows in about 370 years if you will increase it by 3 400 000 000 (3.4 GHz)
// every second. In normal cases there is eternity needed to overflow uint64_t
using PoolIndex = uint64_t;
using PoolStamp = uint64_t;

enum EPoolStamp
{
	PoolStamp_NotConstructed,
	PoolStamp_Free,
	PoolStamp_Origin
};

template<typename T>
class Pool;

///////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////
template<typename T>
class PoolHandle final
{
public:
	///////////////////////////////////////////////////////////////////////////////////////
	/// Default contructor. Create "invalid" pool handle
	///////////////////////////////////////////////////////////////////////////////////////
	PoolHandle() : mIndex(0), mStamp(PoolStamp_Free) { }
private:
	friend class Pool<T>;
	PoolIndex mIndex;
	PoolStamp mStamp;

	///////////////////////////////////////////////////////////////////////////////////////
	/// Private constructor for pool needs
	///////////////////////////////////////////////////////////////////////////////////////
	PoolHandle(PoolIndex index, PoolStamp stamp) : mIndex(index), mStamp(stamp) { }
};

///////////////////////////////////////////////////////////////////////////////////////
/// Internal class for holding user objects. Stores additional information along with 
/// user's object
///////////////////////////////////////////////////////////////////////////////////////
template <typename T>
class PoolRecord final
{
public:
	///////////////////////////////////////////////////////////////////////////////////////
	/// Constructor with arguments delegation
	///////////////////////////////////////////////////////////////////////////////////////
	template <typename... Args>
	PoolRecord(PoolStamp stamp, Args &&... args) : mStamp(stamp), mObject(args...) { }

	///////////////////////////////////////////////////////////////////////////////////////
	///
	///////////////////////////////////////////////////////////////////////////////////////
	PoolRecord() : mStamp(PoolStamp_Free) { }
private:
	friend class Pool<T>;
	PoolStamp mStamp;
	T mObject;
};

///////////////////////////////////////////////////////////////////////////////////////
/// Use this class as base to be able to obtain pointer to parent pool in derived class
///////////////////////////////////////////////////////////////////////////////////////
template<class T>
class Poolable
{
public:
	Pool<T>* ParentPool()
	{
		return mOwner;
	}
private:
	friend class Pool<T>;
	Pool<T>* mOwner { nullptr };
};

///////////////////////////////////////////////////////////////////////////////////////
/// See description in the beginning of this file
///////////////////////////////////////////////////////////////////////////////////////
template<typename T>
class Pool final
{
public:
	typedef void* (*MemoryAllocFunc)(size_t size);
	typedef void (*MemoryFreeFunc)(void* ptr);

	///////////////////////////////////////////////////////////////////////////////////////
	/// @param baseSize - base count of preallocated objects in the pool
	/// @param memoryAlloc Memory allocation function. malloc by default
	/// @param memoryFree Memory deallocation function. free by default
	///////////////////////////////////////////////////////////////////////////////////////
	Pool(size_t baseSize, MemoryAllocFunc memoryAlloc = malloc, MemoryFreeFunc memoryFree = free)
		: mCapacity(baseSize)
		, MemoryAlloc(memoryAlloc)
		, MemoryFree(memoryFree)
	{
		AllocMemory();
		for (PoolIndex i = 0; i < baseSize; ++i)
		{
			mFreeQueue.push(i);
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Destructor
	///////////////////////////////////////////////////////////////////////////////////////
	~Pool()
	{
		Clear();
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns handle to free object, or if pool is full allocates new object and returns
	/// handle to it.
	///
	/// Throws std::bad_alloc when unable to allocate memory.
	///////////////////////////////////////////////////////////////////////////////////////
	template <typename... Args>
	PoolHandle<T> Spawn(Args &&... args)
	{
		if (mFreeQueue.empty())
		{
			// No free objects, grow pool
			const auto oldCapacity = mCapacity;
			if (mCapacity == 0)
			{
				mCapacity = 1;
			}
			else
			{
				mCapacity = static_cast<size_t>(ceil(mCapacity * GrowRate));
			}
			// Remember old records
			const auto old = mRecords;
			// Create new array with larger size
			AllocMemory();
			// Register new free indices
			for (PoolIndex i = oldCapacity; i < mCapacity; ++i)
			{
				mFreeQueue.push(i);
			}
			for (size_t i = 0; i < oldCapacity; ++i)
			{
				if (old[i].mStamp != PoolStamp_NotConstructed)
				{
					// Try to invoke move contructor and fallback to copy contructor if no move 
					// constructor is presented.		
					new (&mRecords[i]) PoolRecord<T>(std::move(old[i]));
				}
			}
			// Remove old array of records
			DestroyObjects(old, oldCapacity);
		}
		const auto index = GetFreeIndex();
		auto &rec = mRecords[index];
		// Reconstruct record via placement new.
		auto element = new (&rec) PoolRecord<T>(MakeStamp(), args...);
		// For newly constructed object we have to check if it is derived from Poolable<T>
		// and set pointer to this pool as owner. 
		if (std::is_base_of<Poolable<T>, T>::value)
		{
			((Poolable<T>*)&element->mObject)->mOwner = this;
		}
		++mSpawnedCount;
		// Return handle to existing object.
		return PoolHandle<T>{index, rec.mStamp};
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Will return object with 'handle' to the pool
	/// Calls destructor of returnable object
	///////////////////////////////////////////////////////////////////////////////////////
	void Return(const PoolHandle<T> &handle)
	{
		assert(handle.mIndex < mCapacity);
		auto &rec = mRecords[handle.mIndex];
		if (rec.mStamp != PoolStamp_Free)
		{
			rec.mStamp = PoolStamp_Free;
			// Destruct
			rec.mObject.~T();
			--mSpawnedCount;
			// Register handle's index as free
			mFreeQueue.push(handle.mIndex);
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Destructs every object in pool, frees memory block that holds records. All refs 
	/// will become invalid!
	///////////////////////////////////////////////////////////////////////////////////////
	void Clear()
	{
		DestroyObjects(mRecords, mCapacity);
		// Clear free indices queue
		mFreeQueue = std::queue<PoolIndex>();
		mRecords = nullptr;
		mCapacity = 0;
		mGlobalStamp = PoolStamp_Origin;
		mSpawnedCount = 0;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns true if 'handle' corresponds to object, that handle indexes.
	///////////////////////////////////////////////////////////////////////////////////////	
	bool IsValid(const PoolHandle<T> &handle) noexcept
	{
		assert(handle.mIndex < mCapacity);
		return handle.mStamp == mRecords[handle.mIndex].mStamp;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns reference to object by its handle
	/// WARNING: You should check handle to validity thru IsValid before pass it to method, 
	/// otherwise you might get wrong object (destructed, or object that already in use by
	/// someone other), or even segfault if you pass handle with index out of bounds.
	/// Checking handle is similar as checking pointer for "non-nullptr" before use it.
	///////////////////////////////////////////////////////////////////////////////////////
	T &At(const PoolHandle<T> &handle) const
	{
		assert(handle.mIndex < mCapacity);
		return mRecords[handle.mIndex].mObject;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Same as At.
	///////////////////////////////////////////////////////////////////////////////////////
	T &operator[](const PoolHandle<T> &handle) const
	{
		return At(handle);
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns count of objects that are already spawned.
	///////////////////////////////////////////////////////////////////////////////////////
	size_t GetSpawnedCount() const noexcept
	{
		return mSpawnedCount;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns total capacity of this pool. 
	///////////////////////////////////////////////////////////////////////////////////////
	size_t GetCapacity() const noexcept
	{
		return mCapacity;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns pointer to memory block that holds every record of this pool.
	/// You should NEVER store returned pointer: its address may change by calling Spawn.
	///////////////////////////////////////////////////////////////////////////////////////
	PoolRecord<T> *GetRecords() noexcept
	{
		return mRecords;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// begin method for "range-based for".
	///////////////////////////////////////////////////////////////////////////////////////
	PoolRecord<T> *begin()
	{
		return mRecords;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// end method for "range-based for".
	///////////////////////////////////////////////////////////////////////////////////////
	PoolRecord<T> *end()
	{
		return mRecords + mCapacity - 1;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Use this only to obtain handle by 'this' pointer inside class method.
	/// Note: Do not rely on pointers to objects in pool, they can suddenly become
	/// invalid. 'this' pointer can be used, because it is always valid.
	///////////////////////////////////////////////////////////////////////////////////////
	PoolHandle<T> HandleByPointer(T * const ptr)
	{
		// Pointer must be in bounds
		if (ptr < &mRecords[0].mObject || ptr >= &mRecords[mCapacity - 1].mObject)
		{
			return PoolHandle<T>();
		}

		// Offset ptr to get record
		const auto record = reinterpret_cast<const PoolRecord<T> *>(
			reinterpret_cast<const char *>(ptr) - sizeof(PoolStamp));

		const auto p0 = reinterpret_cast<const intptr_t>(&mRecords[0]);
		const auto pn = reinterpret_cast<const intptr_t>(record);

		// Calculate index
		const intptr_t distance = pn - p0;
		const intptr_t index = distance / sizeof(PoolRecord<T>);

		return PoolHandle<T>(index, record->mStamp);
	}
private:
	friend class PoolHandle<T>;

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns unique global stamp
	///////////////////////////////////////////////////////////////////////////////////////
	PoolStamp MakeStamp() noexcept
	{
		return mGlobalStamp++;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Calls destructors for every spawned object and then frees memory block
	///////////////////////////////////////////////////////////////////////////////////////
	void DestroyObjects(PoolRecord<T> *ptr, size_t count)
	{
		for (size_t i = 0; i < count; ++i)
		{
			// Destruct only busy objects, not the free ones (they are already destructed)
			if (ptr[i].mStamp != PoolStamp_Free && ptr[i].mStamp != PoolStamp_NotConstructed)
			{
				ptr[i].mObject.~T();
			}
		}
		MemoryFree(ptr);
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Allocates new memory block
	///////////////////////////////////////////////////////////////////////////////////////
	void AllocMemory()
	{
		const size_t sizeBytes = sizeof(PoolRecord<T>) * mCapacity;
		mRecords = reinterpret_cast<PoolRecord<T> *>(MemoryAlloc(sizeBytes));
		if (!mRecords)
		{
			throw std::bad_alloc();
		}
		memset(mRecords, PoolStamp_NotConstructed, sizeBytes);
	}

	///////////////////////////////////////////////////////////////////////////////////////
	/// Returns index of next free record
	///////////////////////////////////////////////////////////////////////////////////////
	PoolIndex GetFreeIndex()
	{
		const auto index = mFreeQueue.front();
		mFreeQueue.pop();
		return index;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	// Internals
	///////////////////////////////////////////////////////////////////////////////////////

	/// By default, grow rate is golden ratio
	static constexpr float GrowRate = 1.618f;
	static_assert(GrowRate > 1.0f, "Grow rate must be greater than 1");

	size_t mSpawnedCount { 0 };
	PoolStamp mGlobalStamp { PoolStamp_Origin };
	PoolRecord<T> *mRecords { nullptr };
	size_t mCapacity { 0 };
	std::queue<PoolIndex> mFreeQueue;
	MemoryAllocFunc MemoryAlloc;
	MemoryFreeFunc MemoryFree;
};

#endif