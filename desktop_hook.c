/*
Copyright (C) 2011 Jay Satiro <raysatiro@yahoo.com>
All rights reserved.

This file is part of GetHooks.

GetHooks is free software: you can redistribute it and/or modify 
it under the terms of the GNU General Public License as published by 
the Free Software Foundation, either version 3 of the License, or 
(at your option) any later version.

GetHooks is distributed in the hope that it will be useful, 
but WITHOUT ANY WARRANTY; without even the implied warranty of 
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
GNU General Public License for more details.

You should have received a copy of the GNU General Public License 
along with GetHooks.  If not, see <http://www.gnu.org/licenses/>.
*/

/** 
This file contains functions for a desktop hook store (linked list of desktop and hook information).
Each function is documented in the comment block above its definition.

There are multiple desktop hook stores: Each snapshot has its own desktop hook store.
Each desktop hook store depends on all the other information in the snapshot (GUI, SPI, etc.)

-
create_desktop_hook_store()

Create a desktop hook store and its descendants or die.
-

-
add_desktop_hook_item()

Create a desktop hook item and append it to the desktop hook store's linked list.
-

-
compare_hook()

Compare two hook structs according to the kernel address of the associated HOOK struct.
-

-
init_desktop_hook_store()

Initialize the desktop hook store by recording the hooks for each desktop.
-

-
print_HANDLEENTRY()

Print a HANDLEENTRY struct.
-

-
print_HOOK()

Print a HOOK struct.
-

-
print_hook()

Print a hook struct.
-

-
print_desktop_hook_item()

Print an item from a desktop hook store's linked list.
-

-
print_desktop_hook_store()

Print a desktop hook store and all its descendants.
-

-
free_desktop_hook_item()

Free a desktop hook item (dirty -- linked list isn't updated).
-

-
free_desktop_hook_store()

Free a desktop hook store and all its descendants.
-

*/

#include <stdio.h>

#include "util.h"

#include "desktop_hook.h"

/* the global stores */
#include "global.h"



/* create_desktop_hook_store()
Create a desktop hook store and its descendants or die.
*/
void create_desktop_hook_store( 
	struct desktop_hook_list **const out   // out deref
)
{
	struct desktop_hook_list *desktop_hooks = NULL;
	
	FAIL_IF( !out );
	FAIL_IF( *out );
	
	
	/* allocate a desktop hook store */
	desktop_hooks = must_calloc( 1, sizeof( *desktop_hooks ) );
	
	
	*out = desktop_hooks;
	return;
}



/* add_desktop_hook_item()
Create a desktop hook item and append it to the desktop hook store's linked list.

returns on success a pointer to the desktop hook item that was added to the list.
if there is already an existing item with the same desktop a pointer to it is returned.
returns NULL on fail
*/
static struct desktop_hook_item *add_desktop_hook_item(
	struct desktop_hook_list *const store,   // in
	struct desktop_item *const desktop   // in
)
{
	struct desktop_hook_item *item = NULL;
	
	FAIL_IF( !store );
	FAIL_IF( !desktop );
	
	
	/* check if there is already an entry for this desktop */
	for( item = store->head; item; item = item->next )
	{
		if( item->desktop == desktop )
			return item;
	}
	
	
	/* create a new item and add it to the list */
	
	item = must_calloc( 1, sizeof( *item ) );
	
	item->desktop = desktop;
	
	/* the allocated/maximum number of elements in the array pointed to by hook.
	
	65535 is the maximum number of user objects
	http://blogs.technet.com/b/markrussinovich/archive/2010/02/24/3315174.aspx
	*/
	item->hook_max = 65535;
	
	/* allocate an array of hook structs */
	item->hook = must_calloc( item->hook_max, sizeof( *item->hook ) );
	
	
	return item;
}



/* compare_hook()
Compare two hook structs according to the kernel address of the associated HOOK struct.

Compare the pHead in two hook structs.

qsort() callback: this function is called to sort the hook array according to pHead

returns -1 if 'p1' pHead < 'p2' pHead
returns 1 if 'p1' pHead > 'p2' pHead
returns 0 if 'p1' pHead == 'p2' pHead
*/
static int compare_hook( 
	const void *const p1,   // in
	const void *const p2   // in
)
{
	const struct hook *const a = p1;
	const struct hook *const b = p2;
	
	
	if( a->entry.pHead < b->entry.pHead )
		return -1;
	else if( a->entry.pHead > b->entry.pHead )
		return 1;
	else
		return 0;
}



/* init_desktop_hook_store()
Initialize the desktop hook store by recording the hooks for each desktop.

The desktop hook store depends on the spi and gui info in its parent snapshot store and all global 
stores except other snapshot stores.

returns nonzero on success
*/
int init_desktop_hook_store( 
	struct snapshot *const parent   // in
)
{
	struct desktop_hook_list *store = NULL;
	struct desktop_hook_item *item = NULL;
	
	FAIL_IF( !G->prog->init_time );   // The program store must already be initialized.
	FAIL_IF( !G->config->init_time );   // The configuration store must already be initialized.
	FAIL_IF( !G->desktops->init_time );   // The desktop store must already be initialized.
	
	FAIL_IF( !parent );   // The snapshot parent of the desktop_hook store must always be passed in.
	FAIL_IF( !parent->init_time_spi );   // The snapshot's spi array must already be initialized.
	FAIL_IF( !parent->init_time_gui );   // The snapshot's gui array must already be initialized.
	
	FAIL_IF( GetCurrentThreadId() != G->prog->dwMainThreadId );   // main thread only
	
	
	store = parent->desktop_hooks;
	
	/* this store is reused. do a soft reset */
	store->init_time = 0;
	
	/* if the desktop hook store does not have a list of desktops yet create it */
	if( !store->head )
	{
		struct desktop_item *current = NULL;
		
		/* add the desktops from the global desktop store */
		for( current = G->desktops->head; current; current = current->next )
			add_desktop_hook_item( store->desktop_hooks, current );
	}
	else // the desktop hook list already exists. reuse it.
	{
		struct desktop_hook_item *current = NULL;
		
		/* soft reset on each desktop hook item's array of hooks */
		for( current = store->head; current; current = current->next )
			current->hook_count = 0; // soft reset of hook array
	}
	
	
	SwitchToThread();
	/* for every handle if it is a HOOK then add it to the desktop's hook array */
	for( i = 0; i < *G->prog->pcHandleEntries; ++i )
	{
		/* copy the HANDLEENTRY struct from the list of entries in the shared info section.
		the info may change so it can't just be pointed to.
		*/
		HANDLEENTRY entry = G->prog->aheList[ i ];
		
		
		if( entry.bType != TYPE_HOOK ) /* not for a HOOK object */
			continue;
		
		/* Check to see if the HOOK is located on a desktop we're attached to */
		for( item = store->head; item; item->next )
		{
			if( ( (void *)entry.pHead >= item->desktop->pvDesktopBase )
				&& ( (void *)entry.pHead < item->desktop->pvDesktopLimit )
			) /* The HOOK is on an accessible desktop */
				break;
		}
		
		if( !item ) /* The HOOK is on an inaccessible desktop */
			continue;
		
		hook = &item->hook[ item->hook_count ];
		
		hook->entry = entry;
		
		/* copy the HOOK struct from the desktop heap.
		the info may change so it can't just be pointed to.
		*/
		hook->object = 
			*(HOOK *)( (size_t)hook->entry.pHead - (size_t)item->desktop->pvClientDelta );
		
		/* search the gui threads to find the owner origin and target of the HOOK */
		hook->owner = find_Win32ThreadInfo( store, hook->entry.pOwner );
		hook->origin = find_Win32ThreadInfo( store, hook->object.pti );
		hook->target = find_Win32ThreadInfo( store, hook->object.ptiHooked );
		
		item->hook_count++;
		if( item->hook_count >= item->hook_max )
		{
			MSG_FATAL( "Too many HOOK objects!" );
			printf( "item->hook_count: %u\n", item->hook_count );
			printf( "item->hook_max: %u\n", item->hook_max );
			exit( 1 );
		}
	}
	
	
	/* sort the hook array for each desktop according to its position in the heap */
	for( item = store->head; item; item->next )
	{
		/* sort according to HANDLEENTRY's entry.pHead */
		qsort( 
			item->hook, 
			item->hook_count, 
			sizeof( *item->hook ), 
			compare_hook
		);
		
		/* search for invalid or duplicate entry.pHead */
		for( i = 1; i < item->hook_count; ++i )
		{
			struct hook *a = &item->hook[ i  - 1 ];
			struct hook *b = &item->hook[ i ];
			
			
			if( a->entry.pHead == b->entry.pHead )
			{
				MSG_FATAL( "Duplicate pHead." );
				print_hook( a );
				print_hook( b );
				exit( 1 );
			}
			
			if( !a->entry.pHead )
			{
				MSG_FATAL( "Invalid pHead." );
				print_hook( a );
				exit( 1 );
			}
			
			if( !b->entry.pHead )
			{
				MSG_FATAL( "Invalid pHead." );
				print_hook( b );
				exit( 1 );
			}
		}
	}
	
	
	/* the desktop hook store has been initialized */
	GetSystemTimeAsFileTime( (FILETIME *)&store->init_time );
	return TRUE;
}



/* print_HANDLEENTRY()
Print a HANDLEENTRY struct.

if the HANDLEENTRY pointer is != NULL then print the HANDLEENTRY
*/
void print_HANDLEENTRY(
	const HANDLEENTRY *const entry   // in
)
{
	const char *const objname = "HANDLEENTRY struct";
	
	
	if( !entry )
		return;
	
	PRINT_SEP_BEGIN( objname );
	
	if( entry->pHead )
	{
		PRINT_PTR( entry->pHead->h );
		printf( "entry->pHead->cLockObj: %lu\n", entry->pHead->cLockObj );
	}
	
	PRINT_PTR( entry->pOwner );
	
	if( entry->bType == TYPE_HOOK )
		printf( "entry->bType: TYPE_HOOK\n" );
	else
		printf( "entry->bType: %u\n", (unsigned)entry->bType );
	
	printf( "entry->bFlags: 0x%02X\n", (unsigned)entry->bFlags );
	printf( "entry->wUniq: %u\n", (unsigned)entry->wUniq );
	
	PRINT_SEP_END( objname );
	
	return;
}



/* print_HOOK()
Print a HOOK struct.

if the HOOK pointer is != NULL then print the HOOK
*/
void print_HOOK(
	const HOOK *const object   // in
)
{
	const char *const objname = "HOOK struct";
	
	
	if( !object )
		return;
	
	PRINT_SEP_BEGIN( objname );
	
	PRINT_PTR( object->Head.h );
	printf( "object->Head.cLockObj: %lu\n", object->Head.cLockObj );
	
	PRINT_PTR( object->pti );
	PRINT_PTR( object->rpdesk1 );
	PRINT_PTR( object->pSelf );
	PRINT_PTR( object->phkNext );
	printf( "object->iHook: %d\n", object->iHook );
	printf( "object->offPfn: 0x%08lX\n", object->offPfn );
	printf( "object->flags: 0x%08lX\n", object->flags );
	printf( "object->ihmod: %d\n", object->ihmod );
	PRINT_PTR( object->ptiHooked );
	PRINT_PTR( object->rpdesk2 );
	
	PRINT_SEP_END( objname );
	
	return;
}



/* print_hook()
Print a hook struct.

if the hook struct pointer is != NULL then print the hook struct
*/
void print_hook(
	const struct hook *const hook   // in
)
{
	const char *const objname = "hook struct";
	
	
	if( !hook )
		return;
	
	PRINT_SEP_BEGIN( objname );
	
	print_HANDLEENTRY( &hook->entry );
	print_HOOK( &hook->object );
	print_gui( hook->owner );
	print_gui( hook->origin );
	print_gui( hook->target );
	
	PRINT_SEP_END( objname );
	
	return;
}



/* print_desktop_hook_item()
Print an item from a desktop hook store's linked list.

if the desktop hook item pointer is != NULL print the item
*/
void print_desktop_hook_item( 
	const struct desktop_hook_item *const item   // in
)
{
	const char *const objname = "Desktop Hook Item";
	int i = 0;
	
	if( !item )
		return;
	
	PRINT_SEP_BEGIN( objname );
	
	if( item->desktop )
		printf( "item->desktop->pwszDesktopName: %ls\n", item->desktop->pwszDesktopName );
	else
		MSG_ERROR( "item->desktop == NULL" );
	
	printf( "item->hook_max: %u\n", item->hook_max );
	printf( "item->hook_count: %u\n", item->hook_count );
	for( i = 0; i < item->hook_count; ++i )
		print_hook( item->hook );
	
	PRINT_SEP_END( objname );
	
	return;
}



/* print_desktop_hook_store()
Print a desktop hook store and all its descendants.

if the desktop hook store pointer != NULL print the store
*/
void print_desktop_hook_store( 
	const struct desktop_hook_list *const store   // in
)
{
	struct desktop_hook_item *item = NULL;
	const char *const objname = "Desktop Hook List Store";
	
	
	if( !store )
		return;
	
	PRINT_DBLSEP_BEGIN( objname );
	print_init_time( "store->init_time", store->init_time );
	
	PRINT_PTR( store->head );
	
	for( item = store->head; item; item = item->next )
	{
		PRINT_PTR( item );
		print_desktop_hook_item( item );
	}
	
	PRINT_PTR( store->tail );
	
	PRINT_DBLSEP_END( objname );
	
	return;
}



/* free_desktop_hook_item()
Free a desktop hook item (dirty -- linked list isn't updated).

this function then sets the desktop hook item pointer to NULL and returns.

this function has no regard for the other items in the list and should only be called by 
free_desktop_hook_store().

'in' is a pointer to a pointer to the desktop hook item, which contains a desktop and its hooks.
if( !in || !*in ) then this function returns.
*/
static void free_desktop_hook_item( 
	struct desktop_hook_item **const in   // in deref
)
{
	if( !in || !*in )
		return;
	
	free( (*in)->hook );
	
	free( (*in) );
	*in = NULL;
	
	return;
}



/* free_desktop_hook_store()
Free a desktop hook store and all its descendants.

this function then sets the desktop hook store pointer to NULL and returns

'in' is a pointer to a pointer to the desktop hook store, which contains a linked list of desktops 
and their hooks.
if( !in || !*in ) then this function returns.
*/
void free_desktop_hook_store( 
	struct desktop_hook_list **const in   // in deref
)
{
	if( !in || !*in )
		return;
	
	if( (*in)->head )
	{
		struct desktop_hook_item *current = NULL, *p = NULL;
		
		for( current = (*in)->head; current; current = p )
		{
			p = current->next;
			
			free_desktop_hook_item( &current );
		}
	}
	
	free( (*in) );
	*in = NULL;
	
	return;
}