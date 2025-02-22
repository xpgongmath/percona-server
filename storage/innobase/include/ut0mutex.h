/*****************************************************************************

Copyright (c) 2012, 2015, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/ut0mutex.h
Policy based mutexes.

Created 2012-03-24 Sunny Bains.
***********************************************************************/

#ifndef UNIV_INNOCHECKSUM

#ifndef ut0mutex_h
#define ut0mutex_h

extern ulong	srv_spin_wait_delay;
extern ulong	srv_n_spin_wait_rounds;
extern ulong	srv_force_recovery_crash;

#include "os0atomic.h"
#include "sync0policy.h"
#include "ib0mutex.h"
#include <set>

/** Create a typedef using the MutexType<PolicyType>
@param[in]	M		Mutex type
@param[in[	P		Policy type
@param[in]	T		The resulting typedef alias */
#define UT_MUTEX_TYPE(M, P, T) typedef PolicyMutex<M<P> > T;

#ifndef UNIV_DEBUG

# ifdef HAVE_IB_LINUX_FUTEX
UT_MUTEX_TYPE(TTASFutexMutex, GenericPolicy, FutexMutex);
UT_MUTEX_TYPE(TTASFutexMutex, AggregateMutexStatsPolicy, BlockFutexMutex);
# endif /* HAVE_IB_LINUX_FUTEX */

UT_MUTEX_TYPE(TTASMutex, GenericPolicy, SpinMutex);
UT_MUTEX_TYPE(TTASMutex, AggregateMutexStatsPolicy, BlockSpinMutex);


UT_MUTEX_TYPE(OSTrackMutex, GenericPolicy, SysMutex);
UT_MUTEX_TYPE(OSTrackMutex, AggregateMutexStatsPolicy, BlockSysMutex);

UT_MUTEX_TYPE(TTASEventMutex, GenericPolicy, SyncArrayMutex);
UT_MUTEX_TYPE(TTASEventMutex, AggregateMutexStatsPolicy, BlockSyncArrayMutex);

#else /* !UNIV_DEBUG */

# ifdef HAVE_IB_LINUX_FUTEX
UT_MUTEX_TYPE(TTASFutexMutex, GenericPolicy, FutexMutex);
UT_MUTEX_TYPE(TTASFutexMutex, AggregateMutexStatsPolicy, BlockFutexMutex);
# endif /* HAVE_IB_LINUX_FUTEX */

UT_MUTEX_TYPE(TTASMutex, GenericPolicy, SpinMutex);
UT_MUTEX_TYPE(TTASMutex, AggregateMutexStatsPolicy, BlockSpinMutex);

UT_MUTEX_TYPE(OSTrackMutex, GenericPolicy, SysMutex);
UT_MUTEX_TYPE(OSTrackMutex, AggregateMutexStatsPolicy, BlockSysMutex);

UT_MUTEX_TYPE(TTASEventMutex, GenericPolicy, SyncArrayMutex);
UT_MUTEX_TYPE(TTASEventMutex, AggregateMutexStatsPolicy, BlockSyncArrayMutex);

#endif /* !UNIV_DEBUG */

#ifdef MUTEX_FUTEX
/** The default mutex type. */
typedef FutexMutex ib_mutex_t;
typedef BlockFutexMutex ib_bpmutex_t;
#define MUTEX_TYPE	"Uses futexes"
#elif defined(MUTEX_SYS)
typedef SysMutex ib_mutex_t;
typedef BlockSysMutex ib_bpmutex_t;
#define MUTEX_TYPE	"Uses system mutexes"
#elif defined(MUTEX_EVENT)
typedef SyncArrayMutex ib_mutex_t;
typedef BlockSyncArrayMutex ib_bpmutex_t;
#define MUTEX_TYPE	"Uses event mutexes"
#else
#error "ib_mutex_t type is unknown"
#endif /* MUTEX_FUTEX */

#include "ut0mutex.ic"

extern ulong	srv_spin_wait_delay;
extern ulong	srv_n_spin_wait_rounds;

#define mutex_create(I, M)		mutex_init((M), (I), __FILE__, __LINE__)

#define mutex_enter(M)			(M)->enter(			\
					srv_n_spin_wait_rounds,		\
					srv_spin_wait_delay,		\
					__FILE__, __LINE__)

#define mutex_enter_nospin(M)		(M)->enter(			\
					0,				\
					0,				\
					__FILE__, __LINE__)

#define mutex_enter_nowait(M)		(M)->trylock(__FILE__, __LINE__)

#define mutex_exit(M)			(M)->exit()

#define mutex_free(M)			mutex_destroy(M)

#ifdef UNIV_DEBUG
/**
Checks that the mutex has been initialized. */
#define mutex_validate(M)		(M)->validate()

/**
Checks that the current thread owns the mutex. Works only
in the debug version. */
#define mutex_own(M)			(M)->is_owned()
#else
#define mutex_own(M)			/* No op */
#define mutex_validate(M)		/* No op */
#endif /* UNIV_DEBUG */

/** Iterate over the mutex meta data */
class MutexMonitor {
public:
	/** Constructor */
	MutexMonitor() { }

	/** Destructor */
	~MutexMonitor() { }

	/** Enable the mutex monitoring */
	void enable();

	/** Disable the mutex monitoring */
	void disable();

	/** Reset the mutex monitoring values */
	void reset();

	/** Invoke the callback for each active mutex collection
	@param[in,out]	callback	Functor to call
	@return false if callback returned false */
	template<typename Callback>
	bool iterate(Callback& callback) const
		UNIV_NOTHROW
	{
		LatchMetaData::iterator	end = latch_meta.end();

		for (LatchMetaData::iterator it = latch_meta.begin();
		     it != end;
		     ++it) {

			/* Some of the slots will be null in non-debug mode */

			if (*it == NULL) {
				continue;
			}

			latch_meta_t*	latch_meta = *it;

			bool	ret = callback(*latch_meta);

			if (!ret) {
				return(ret);
			}
		}

		return(true);
	}
};

/** Defined in sync0sync.cc */
extern MutexMonitor*	mutex_monitor;

/**
Creates, or rather, initializes a mutex object in a specified memory
location (which must be appropriately aligned). The mutex is initialized
in the reset state. Explicit freeing of the mutex with mutex_free is
necessary only if the memory block containing it is freed.
Add the mutex instance to the global mutex list.
@param[in,out]	mutex		mutex to initialise
@param[in]	id		The mutex ID (Latch ID)
@param[in]	filename	Filename from where it was called
@param[in]	line		Line number in filename from where called */
template <typename Mutex>
void mutex_init(
	Mutex*		mutex,
	latch_id_t	id,
	const char*	file_name,
	uint32_t	line)
{
	new(mutex) Mutex();

	mutex->init(id, file_name, line);
}

/**
Removes a mutex instance from the mutex list. The mutex is checked to
be in the reset state.
@param[in,out]	 mutex		mutex instance to destroy */
template <typename Mutex>
void mutex_destroy(
	Mutex*		mutex)
{
	mutex->destroy();
}

#endif /* ut0mutex_h */

#endif /* UNIV_INNOCHECKSUM */
