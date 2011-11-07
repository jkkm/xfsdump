/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <xfs/xfs.h>
#include <xfs/jdm.h>

#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/prctl.h>
#include <errno.h>
#include <pthread.h>

#include "types.h"
#include "lock.h"
#include "qlock.h"
#include "stream.h"
#include "mlog.h"
#include "cldmgr.h"

extern size_t pgsz;

#define CLD_MAX	( STREAM_SIMMAX * 2 )
struct cld {
	bool_t c_busy;
	pthread_t c_tid;
	ix_t c_streamix;
	int ( * c_entry )( void *arg1 );
	void * c_arg1;
};

typedef struct cld cld_t;

static cld_t cld[ CLD_MAX ];
static bool_t cldmgr_stopflag;

static cld_t *cldmgr_getcld( void );
static cld_t * cldmgr_findbytid( pthread_t );
static void *cldmgr_entry( void * );
/* REFERENCED */
static pthread_t cldmgr_parenttid;

bool_t
cldmgr_init( void )
{
	( void )memset( ( void * )cld, 0, sizeof( cld ));
	cldmgr_stopflag = BOOL_FALSE;
	cldmgr_parenttid = pthread_self( );

	return BOOL_TRUE;
}

bool_t
cldmgr_create( int ( * entry )( void *arg1 ),
	       ix_t streamix,
	       char *descstr,
	       void *arg1 )
{
	cld_t *cldp;
	intgen_t rval;

	ASSERT( pthread_equal( pthread_self( ), cldmgr_parenttid ) );

	cldp = cldmgr_getcld( );
	if ( ! cldp ) {
		mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_PROC, _(
		      "cannot create %s thread for stream %u: "
		      "too many child threads (max allowed is %d)\n"),
		      descstr,
		      streamix,
		      CLD_MAX );
		return BOOL_FALSE;
	}

	cldp->c_streamix = streamix;
	cldp->c_entry = entry;
	cldp->c_arg1 = arg1;
	rval = pthread_create( &cldp->c_tid, NULL, cldmgr_entry, cldp );
	if ( rval ) {
		mlog( MLOG_NORMAL | MLOG_ERROR | MLOG_PROC, _(
		      "failed creating %s thread for stream %u: %s\n"),
		      descstr,
		      streamix,
		      strerror( rval ));
	} else {
		mlog( MLOG_NITTY | MLOG_PROC,
		      "%s thread created for stream %u: tid %lu\n",
		      descstr,
		      streamix,
		      cldp->c_tid );
	}

	return rval ? BOOL_FALSE : BOOL_TRUE;
}

void
cldmgr_stop( void )
{
	/* must NOT mlog here!
	 * locked up by main loop dialog
	 */
	cldmgr_stopflag = BOOL_TRUE;
}

void
cldmgr_died( pthread_t tid )
{
	cld_t *cldp = cldmgr_findbytid( tid );

	if ( ! cldp ) {
		return;
	}
	cldp->c_busy = BOOL_FALSE;
	if ( ( intgen_t )( cldp->c_streamix ) >= 0 ) {
		stream_dead( tid );
	}
}

bool_t
cldmgr_stop_requested( void )
{
	return cldmgr_stopflag;
}

size_t
cldmgr_remainingcnt( void )
{
	cld_t *p = cld;
	cld_t *ep = cld + sizeof( cld ) / sizeof( cld[ 0 ] );
	size_t cnt;

	cnt = 0;
	lock( );
	for ( ; p < ep ; p++ ) {
		if ( p->c_busy ) {
			cnt++;
		}
	}
	unlock( );

	return cnt;
}

bool_t
cldmgr_otherstreamsremain( ix_t streamix )
{
	cld_t *p = cld;
	cld_t *ep = cld + sizeof( cld ) / sizeof( cld[ 0 ] );

	lock( );
	for ( ; p < ep ; p++ ) {
		if ( p->c_busy && p->c_streamix != streamix ) {
			unlock( );
			return BOOL_TRUE;
		}
	}
	unlock( );

	return BOOL_FALSE;
}

static cld_t *
cldmgr_getcld( void )
{
	cld_t *p = cld;
	cld_t *ep = cld + sizeof( cld ) / sizeof( cld[ 0 ] );

	lock();
	for ( ; p < ep ; p++ ) {
		if ( ! p->c_busy ) {
			p->c_busy = BOOL_TRUE;
			break;
		}
	}
	unlock();

	return ( p < ep ) ? p : 0;
}

static cld_t *
cldmgr_findbytid( pthread_t tid )
{
	cld_t *p = cld;
	cld_t *ep = cld + sizeof( cld ) / sizeof( cld[ 0 ] );

	for ( ; p < ep ; p++ ) {
		if ( p->c_busy && pthread_equal( p->c_tid, tid )) {
			break;
		}
	}

	return ( p < ep ) ? p : 0;
}

static void *
cldmgr_entry( void *arg1 )
{
	cld_t *cldp = ( cld_t * )arg1;
	pthread_t tid = pthread_self( );

	if ( ( intgen_t )( cldp->c_streamix ) >= 0 ) {
		stream_register( tid, ( intgen_t )cldp->c_streamix );
	}
	mlog( MLOG_DEBUG | MLOG_PROC,
	      "thread %lu created for stream %d\n",
	      tid,
	      cldp->c_streamix );

	( * cldp->c_entry )( cldp->c_arg1 );
	return NULL;
}
