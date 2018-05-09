# SmartPool

SmartPool is an object pool single-header library for fast object allocation. All objects inside of a pool placed in a contiguous memory block to be cache-friendly.

## Overview

Pool serves for a few goals: eliminates unnecessary memory allocations, preserves data locality, gives mechanism of control for object indices.

This pool provides user the easy way of control for spawned objects through special handles. Why we can't use ordinary pointers? Answer is simple: when pool grows, its memory block may change its position in memory and all pointers will become invalid. To avoid this problem, we use handles. 

What is "handle"? Handle is something like index, but with additional information, that allows us to ensure that handle "points" to same object as before. This additional info called "stamp". When you asks pool for a new object, pool marks it with unique "stamp" and gives you handle with index of a new object and "stamp" of an object. Now if you want to ensure, that handle "points" to same object as before, you just compare "stamps" - if they are same, then handle is correct (you can check handle validity using IsValid method).

Pool implementation is object-agnostic, so you can store any suitable object in it. It also may contain non-POD objects.

## Installation

SmartPool requires C++11-compliant compiler. To use pool, just copy Pool.h to your source directory.

## Notes
Your object should have constructor without arguments. 

Create pool with large enough capacity, because any Spawn method called on full pool will result in memory reallocation and memory movement, which is quite expensive operations.

You can pass your own memory allocation/deallocation functions as 2nd and 3rd parameters in Pool constructor.

If you need to use pool from any class that is stored in the pool, inherit your class from Poolable<T> like this:
```c++
class Foo : public Poolable<Foo> {
...
};
```
Now when you need to access owning pool, call ParentPool()
```c++
class Foo : public Poolable<Foo> {
	PoolHandle<Foo> mSomeHandle;
	void DoSomething() {
		Pool<Foo>* pool = ParentPool();
		
		if(pool->IsValid(mSomeHandle)) {
			auto & object = pool->At(mSomeHandle);
		}
	}
};
```
Why this works? Inside of Spawn method, pool checks if stored class have base of Poolable<T>, and if it does, just sets some internal variable of Poolable<T>. 

## Examples

```c++
class Foo {
private:
  int fooBar;
public:
  Foo() {}
  Foo(int foobar) : fooBar(foobar) {}
};

...

Pool<Foo> pool(1024); // make pool with default capacity of 1024 objects
PoolHandle<Foo> bar = pool.Spawn(); // spawn object with default constructor
PoolHandle<Foo> baz = pool.Spawn(42); // spawn object with any args

// do something

// return object to pool
pool.Return(bar);
pool.Return(baz);

...

// somewhere in the code

// check handle for validity
if(pool.IsValid(baz)) {
  cout << "Baz is valid" << endl;
} else {
  cout << "Baz is invalid" << endl;
}

```

## How it works
Firstly, pool allocates memory block with initial size = initialCapacity * recordSize, fills it with special marks, which indicates that memory piece is unused. Also pool creates list of indices of free memory blocks.

When user calls Spawn method, pool pops index of free memory block, constructs object in it using placement new, makes new stamp and returns handle to the user.

When user calls Return method, pool returns index of the object to "free list", marks object as free and calls destructor of the object.

When pool is destroyed, it calls destructors of all "busy" objects inside of it. So there is no memory leaks in the end.

## Q&A

**Q**: How fast is Spawn method?
**A**: Spawn method has amortized complexity of O(1).

**Q**: How fast is Return method?
**A**: Return method has amortized complexity of O(1).

## Tests

Tests (sanity and performance) are all in Tests.cpp.

## License

The MIT License

Copyright (c) 2017-2018 Stepanov Dmitriy a.k.a mr.DIMAS a.k.a v1al

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
