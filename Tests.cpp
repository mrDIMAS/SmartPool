#include "Pool.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define ONLY_POOL_TESTS 0

using namespace std;

// Performance test
constexpr int ObjectCountPerTest = 2000000;

class Vec3
{
public:
	float x, y, z;
	Vec3()
	{
	}
	Vec3(float x, float y, float z) : x(x), y(y), z(z)
	{
	}
	float Length() const
	{
		return sqrt(x * x + y * y + z * z);
	}
};

class Quat
{
public:
	float x, y, z, w;

	Quat()
	{
	}
	Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w)
	{
	}
	Quat(const Vec3 &axis, float angle)
	{
		float halfAngle = angle * 0.5f;
		float d = axis.Length();
		float s = sin(halfAngle) / d;
		x = axis.x * s;
		y = axis.y * s;
		z = axis.z * s;
		w = cos(halfAngle);
	}
	float SqrLength() const
	{
		return x * x + y * y + z * z + w * w;
	}
};

class Matrix
{
public:
	float mElements[16];
	Matrix()
	{
	}
	Matrix(const Quat &rotation, const Vec3 &scale, const Vec3 &translation)
	{
		FromRotationScaleTranslation(rotation, scale, translation);
	}
	void FromRotationScaleTranslation(
		const Quat &rotation, const Vec3 &scale, const Vec3 &translation)
	{
		float s = 2.0f / rotation.SqrLength();
		float xs = rotation.x * s, ys = rotation.y * s, zs = rotation.z * s;
		float wx = rotation.w * xs, wy = rotation.w * ys, wz = rotation.w * zs;
		float xx = rotation.x * xs, xy = rotation.x * ys, xz = rotation.x * zs;
		float yy = rotation.y * ys, yz = rotation.y * zs, zz = rotation.z * zs;
		mElements[0] = 1.0f - (yy + zz);
		mElements[1] = xy + wz;
		mElements[2] = xz - wy;
		mElements[3] = 0.0f;
		mElements[4] = xy - wz;
		mElements[5] = 1.0f - (xx + zz);
		mElements[6] = yz + wx;
		mElements[7] = 0.0f;
		mElements[8] = xz + wy;
		mElements[9] = yz - wx;
		mElements[10] = 1.0f - (xx + yy);
		mElements[11] = 0.0f;
		mElements[12] = translation.x;
		mElements[13] = translation.y;
		mElements[14] = translation.z;
		mElements[15] = 1.0f;
	}
	Matrix operator*(const Matrix &other) const
	{
		Matrix out;
		float *temp = out.mElements;
		const float *a = mElements;
		const float *b = other.mElements;
		temp[0] = a[0] * b[0] + a[1] * b[4] + a[2] * b[8] + a[3] * b[12];
		temp[1] = a[0] * b[1] + a[1] * b[5] + a[2] * b[9] + a[3] * b[13];
		temp[2] = a[0] * b[2] + a[1] * b[6] + a[2] * b[10] + a[3] * b[14];
		temp[3] = a[0] * b[3] + a[1] * b[7] + a[2] * b[11] + a[3] * b[15];
		temp[4] = a[4] * b[0] + a[5] * b[4] + a[6] * b[8] + a[7] * b[12];
		temp[5] = a[4] * b[1] + a[5] * b[5] + a[6] * b[9] + a[7] * b[13];
		temp[6] = a[4] * b[2] + a[5] * b[6] + a[6] * b[10] + a[7] * b[14];
		temp[7] = a[4] * b[3] + a[5] * b[7] + a[6] * b[11] + a[7] * b[15];
		temp[8] = a[8] * b[0] + a[9] * b[4] + a[10] * b[8] + a[11] * b[12];
		temp[9] = a[8] * b[1] + a[9] * b[5] + a[10] * b[9] + a[11] * b[13];
		temp[10] = a[8] * b[2] + a[9] * b[6] + a[10] * b[10] + a[11] * b[14];
		temp[11] = a[8] * b[3] + a[9] * b[7] + a[10] * b[11] + a[11] * b[15];
		temp[12] = a[12] * b[0] + a[13] * b[4] + a[14] * b[8] + a[15] * b[12];
		temp[13] = a[12] * b[1] + a[13] * b[5] + a[14] * b[9] + a[15] * b[13];
		temp[14] = a[12] * b[2] + a[13] * b[6] + a[14] * b[10] + a[15] * b[14];
		temp[15] = a[12] * b[3] + a[13] * b[7] + a[14] * b[11] + a[15] * b[15];
		return out;
	}
	Matrix &operator*=(const Matrix &other)
	{
		*this = *this * other;
		return *this;
	}
};

class PoolableNode : public Poolable<PoolableNode>
{
public:
	string mName { "Unnamed" };
	PoolHandle<PoolableNode> mParent;
	vector<PoolHandle<PoolableNode>> mChildren;
	Matrix mLocalTransform;
	Matrix mGlobalTransform;
	Vec3 mPosition { 0, 0, 0 };
	Vec3 mScale { 1, 1, 1 };
	Quat mRotation { 0, 0, 0, 1 };
	bool mNeedUpdate { true };
public:
	PoolableNode()
	{

	}
	virtual ~PoolableNode()
	{
		for (auto &child : mChildren)
		{
			ParentPool()->Return(child);
		}
	}
	void SetName(const string &name)
	{
		mName = name;
	}
	void AttachTo(const PoolHandle<PoolableNode> &parentHandle)
	{		
		mParent = parentHandle;
		auto self = ParentPool()->HandleByPointer(this);
		ParentPool()->At(parentHandle).mChildren.push_back(self);		
	}
	void Update()
	{
		if (mNeedUpdate)
		{
			mLocalTransform.FromRotationScaleTranslation(mRotation, mScale, mPosition);

			if (ParentPool()->IsValid(mParent))
			{				
				mGlobalTransform = ParentPool()->At(mParent).GetGlobalTransform() * mLocalTransform;
			}
			else
			{
				mGlobalTransform = mLocalTransform;
			}

			mNeedUpdate = false;

			for (const auto &child : mChildren)
			{
				ParentPool()->At(child).Update();
			}
		}
	}
	void SetPosition(const Vec3 &p)
	{
		mPosition = p;
		mNeedUpdate = true;
	}
	const Vec3 &GetPosition() const
	{
		return mPosition;
	}
	void SetRotation(const Quat &q)
	{
		mRotation = q;
		mNeedUpdate = true;
	}
	const Quat &GetRotation() const
	{
		return mRotation;
	}
	void SetScale(const Vec3 &scale)
	{
		mScale = scale;
		mNeedUpdate = true;
	}
	const Vec3 &GetScale() const
	{
		return mScale;
	}
	const Matrix &GetLocalTransform()
	{
		Update();
		return mLocalTransform;
	}
	const Matrix &GetGlobalTransform()
	{
		Update();
		return mGlobalTransform;
	}
};

class OrdinaryNode : public enable_shared_from_this<OrdinaryNode>
{
private:
	string mName { "Unnamed" };
	weak_ptr<OrdinaryNode> mParent;
	vector<shared_ptr<OrdinaryNode>> mChildren;
	Matrix mLocalTransform;
	Matrix mGlobalTransform;
	Vec3 mPosition { 0, 0, 0 };
	Vec3 mScale { 1, 1, 1 };
	Quat mRotation { 0, 0, 0, 1 };
	bool mNeedUpdate { true };

public:
	OrdinaryNode()
	{

	}
	virtual ~OrdinaryNode()
	{
	}
	void SetName(const string &name)
	{
		mName = name;
	}
	void AttachTo(const shared_ptr<OrdinaryNode> &parent)
	{
		parent->mChildren.push_back(shared_from_this());
		mParent = parent;
	}
};

void RunSanityTests()
{
	cout << endl << endl;
	cout << "Running sanity tests" << endl;
	{
		Pool<PoolableNode> pool(1);
		auto a = pool.Spawn();
		pool[a].SetName("A");
		auto b = pool.Spawn();
		pool[b].SetName("B");
		auto c = pool.Spawn();
		pool[c].SetName("C");
		pool.Return(a);
		pool.Return(b);
		pool.Return(c);
		a = pool.Spawn();
		pool[a].SetName("A");
	}

	{
		Pool<PoolableNode> pool(1024);

		auto handle = pool.Spawn();
		assert(pool.IsValid(handle));
		pool.Return(handle);

		assert(!pool.IsValid(handle));
		assert(pool.GetSpawnedCount() == 0);

		auto newHandle = pool.Spawn();
		assert(pool.IsValid(newHandle));
		assert(pool.GetSpawnedCount() == 1);
		pool.Return(newHandle);
	}

	cout << "Passed" << endl;
}

void RunRandomObjectPerformanceTest()
{
	cout << endl << endl;
	cout << "Running random object performance test" << endl;
	cout << "Object count: " << ObjectCountPerTest << endl;

	// PoolableNode
	{
		Pool<PoolableNode> pool(1024);

		auto lastTime = chrono::high_resolution_clock::now();
		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			auto parent = pool.Spawn();
			pool[parent].SetName("Parent");
			auto child = pool.Spawn();
			pool[child].SetName("Child");
			pool[child].AttachTo(parent);
			pool.Return(parent);
		}

		cout << "Pool<Node>: "
			<< chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
			.count()
			<< " microseconds" << endl;
		// cout << "Pool size: " << pool.GetSize() << endl;
	}

#if !ONLY_POOL_TESTS
	// OrdinaryNode
	{
		auto lastTime = chrono::high_resolution_clock::now();

		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			auto parent = make_shared<OrdinaryNode>();
			parent->SetName("Parent");
			auto child = make_shared<OrdinaryNode>();
			child->SetName("Child");
			child->AttachTo(parent);
			parent.reset();
		}

		cout << "shared_ptr<Node>: "
			<< chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
			.count()
			<< " microseconds" << endl;
	}
#endif
	cout << "Passed" << endl;
}

void RunHugeAmountOfObjectsPerformanceTest()
{
	class Foo
	{
	private:
		int mBar;
		string mBaz;

	public:
		Foo()
		{
		}
		Foo(int bar) : mBar(bar)
		{
		}
	};



	cout << endl << endl;
	cout << "Running huge amount of objects performance test" << endl;
	cout << "Object count: " << ObjectCountPerTest << endl;

	{
		auto lastTime = chrono::high_resolution_clock::now();

		vector<PoolHandle<Foo>> handles;
		handles.reserve(ObjectCountPerTest);
		Pool<Foo> pool(ObjectCountPerTest);

		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			handles.push_back(pool.Spawn(1234));
		}

		for (const auto &handle : handles)
		{
			pool.Return(handle);
		}

		cout << "Pool<Foo>: "
			<< chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
			.count()
			<< " microseconds" << endl;
	}

#if !ONLY_POOL_TESTS
	{
		auto lastTime = chrono::high_resolution_clock::now();

		vector<unique_ptr<Foo>> pointers;
		pointers.reserve(ObjectCountPerTest);
		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			pointers.push_back(make_unique<Foo>(1234));
		}

		for (auto &ptr : pointers)
		{
			ptr.reset();
		}

		cout << "unique_ptr<Foo>: "
			<< chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
			.count()
			<< " microseconds" << endl;
	}
#endif
	cout << "Passed" << endl;
}

void RunDataLocalityPerformanceTest()
{
	class Foo
	{
	private:
		Matrix mA;
		Matrix mB;
		Matrix mResult;
	public:
		Foo()
		{
			mA.FromRotationScaleTranslation({ 0, 0, 0, 1 }, { 1, 1, 1 }, { 1, 0, 0 });
			mB.FromRotationScaleTranslation({ 1, 0, 0, 1 }, { 1, 1, 1 }, { 1, 1, 0 });
		}

		void Calculate()
		{
			mResult = mA * mB;
		}
	};

	// Performance test
	constexpr int iterCount = 20;

	cout << endl << endl;
	cout << "Running data locality performance test" << endl;
	cout << "Object count: " << ObjectCountPerTest << endl;

	long long totalTime = 0;

#if !ONLY_POOL_TESTS
	{
		vector<unique_ptr<Foo>> pointers;
		pointers.reserve(ObjectCountPerTest);
		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			pointers.push_back(make_unique<Foo>());
		}

		for (int k = 0; k < iterCount; ++k)
		{
			auto lastTime = chrono::high_resolution_clock::now();
			for (const auto &ptr : pointers)
			{
				ptr->Calculate();
			}

			totalTime += chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
				.count();
		}

		for (auto &ptr : pointers)
		{
			ptr.reset();
		}
	}

	cout << "unique_ptr<Foo>: " << totalTime / iterCount << " microseconds" << endl;
#endif



	{
		vector<PoolHandle<Foo>> handles;
		handles.reserve(ObjectCountPerTest);
		Pool<Foo> pool(ObjectCountPerTest);

		for (int i = 0; i < ObjectCountPerTest; ++i)
		{
			handles.push_back(pool.Spawn());
		}

		totalTime = 0;
		for (int k = 0; k < iterCount; ++k)
		{
			auto lastTime = chrono::high_resolution_clock::now();
			for (const auto &handle : handles)
			{
				pool.At(handle).Calculate();
			}

			totalTime += chrono::duration_cast<chrono::microseconds>(
				chrono::high_resolution_clock::now() - lastTime)
				.count();
		}

		for (const auto &handle : handles)
		{
			pool.Return(handle);
		}
	}

	cout << "Pool<Foo>: " << totalTime / iterCount << " microseconds" << endl;

	cout << "Passed" << endl;
}

int main(int argc, char **argv)
{
	RunDataLocalityPerformanceTest();
	RunSanityTests();
	RunRandomObjectPerformanceTest();
	RunHugeAmountOfObjectsPerformanceTest();
	
	system("pause");
	return 0;
}
