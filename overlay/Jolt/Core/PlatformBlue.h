// impulse: PlatformBlue.h — freestanding single-threaded platform for Jolt
// Physics on OpenVM (RV32IM + zfinx).
//
// Jolt's JPH_PLATFORM_BLUE hook is the documented way to target a custom
// platform (see Jolt/Core/Core.h). This file is supplied out-of-tree via the
// overlay/ include directory so that the JoltPhysics submodule stays
// pristine; it is included by Core.h when JPH_PLATFORM_BLUE is defined.
//
// The VM executes a single thread with no OS, so all synchronization
// primitives are no-ops and the tick counter is constant. JPH_BREAKPOINT
// traps into the VM's panic path.

#pragma once

#include <stdint.h>
#include <mutex>

// ---------------------------------------------------------------- STL threads
// The impulse libc++ is built with LIBCXX_ENABLE_THREADS=OFF (OpenVM guests
// are single-threaded and freestanding), so std::mutex / std::shared_mutex /
// std::thread / std::shared_lock do not exist. Jolt's Core/Mutex.h
// references all of them through unconditional using-declarations — even
// under JPH_PLATFORM_BLUE, where MutexBase/SharedMutexBase above replace
// std::mutex/std::shared_mutex — and Jolt code instantiates
// shared_lock<SharedMutex> over them, calls thread::hardware_concurrency()
// (BodyManager) and this_thread::yield() (PhysicsSystem job polling).
//
// So the platform layer supplies single-threaded replacements: a thread
// class that reports one core and a trivial id, and a shared_lock that
// forwards to lock_shared/unlock_shared of the wrapped (BLUE) mutex type.
// PlatformBlue.h is included from Core.h before any Jolt header uses
// these names; the real libc++ headers never declare them with threads
// off, so there are no collisions. On threaded libc++ / libstdc++ builds
// the guard skips this block entirely.
// (_LIBCPP_HAS_NO_THREADS is the pre-LLVM-20 spelling of _LIBCPP_HAS_THREADS 0.)
#if (defined(_LIBCPP_HAS_THREADS) && _LIBCPP_HAS_THREADS == 0) || defined(_LIBCPP_HAS_NO_THREADS)
namespace std
{
	class mutex;					// Referenced by Mutex.h using-declarations only
	class shared_mutex;				// (MutexBase/SharedMutexBase replace them here)

	template <class MutexT>
	class shared_lock
	{
	public:
		explicit					shared_lock(MutexT &inMutex)
			: mMutex(inMutex)
		{
			mMutex.lock_shared();
		}

									~shared_lock()
		{
			mMutex.unlock_shared();
		}

									shared_lock(const shared_lock<MutexT> &) = delete;
		shared_lock<MutexT> &		operator = (const shared_lock<MutexT> &) = delete;

	private:
		MutexT &					mMutex;
	};

	class thread
	{
	public:
		class id
		{
		public:
			friend bool		operator == (id, id)			{ return true; } // single thread: all ids equal
			friend bool		operator != (id, id)			{ return false; }
		};

								id		get_id() const noexcept		{ return id(); }

		static unsigned			hardware_concurrency() noexcept	{ return 1; }
	};

	namespace this_thread
	{
		inline thread::id		get_id() noexcept					{ return thread::id(); }
		inline void				yield() { }
	}
}
#endif

// ---------------------------------------------------------------- breakpoints

#define JPH_BREAKPOINT __builtin_trap()

// ------------------------------------------------------------------- ticks

// No cycle CSR is available to the guest; the value is only used by the
// (disabled) profiler, so a constant is fine.
#define JPH_PLATFORM_BLUE_GET_TICKS() (uint64(0))

// ------------------------------------------------------------------ mutexes
// Single-threaded: lock ownership is trivially exclusive, try_lock always
// succeeds.

typedef int JPH_PLATFORM_BLUE_MUTEX;

#define JPH_PLATFORM_BLUE_MUTEX_INIT(m) ((void)(m))
#define JPH_PLATFORM_BLUE_MUTEX_DESTROY(m) ((void)(m))
#define JPH_PLATFORM_BLUE_MUTEX_TRYLOCK(m) (true)
#define JPH_PLATFORM_BLUE_MUTEX_LOCK(m) ((void)(m))
#define JPH_PLATFORM_BLUE_MUTEX_UNLOCK(m) ((void)(m))

// ------------------------------------------------------------- shared mutex

typedef int JPH_PLATFORM_BLUE_RWLOCK;

#define JPH_PLATFORM_BLUE_RWLOCK_INIT(l) ((void)(l))
#define JPH_PLATFORM_BLUE_RWLOCK_DESTROY(l) ((void)(l))
#define JPH_PLATFORM_BLUE_RWLOCK_TRYWLOCK(l) (true)
#define JPH_PLATFORM_BLUE_RWLOCK_TRYRLOCK(l) (true)
#define JPH_PLATFORM_BLUE_RWLOCK_WLOCK(l) ((void)(l))
#define JPH_PLATFORM_BLUE_RWLOCK_WUNLOCK(l) ((void)(l))
#define JPH_PLATFORM_BLUE_RWLOCK_RLOCK(l) ((void)(l))
#define JPH_PLATFORM_BLUE_RWLOCK_RUNLOCK(l) ((void)(l))

// ---------------------------------------------------------------- semaphores
// Only used by JobSystemThreadPool / JobSystemWithBarrier, which impulse does
// not use (it runs JobSystemSingleThreaded). Keep them linkable no-ops.

typedef int JPH_PLATFORM_BLUE_SEMAPHORE;

#define JPH_PLATFORM_BLUE_SEMAPHORE_INIT(s) (true)
#define JPH_PLATFORM_BLUE_SEMAPHORE_DESTROY(s) ((void)(s))
#define JPH_PLATFORM_BLUE_SEMAPHORE_SIGNAL(s, n) ((void)(s), (void)(n))
#define JPH_PLATFORM_BLUE_SEMAPHORE_WAIT(s, n) ((void)(s), (void)(n))
