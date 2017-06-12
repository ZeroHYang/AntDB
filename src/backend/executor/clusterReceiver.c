
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "executor/clusterReceiver.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "storage/ipc.h"
#include "tcop/dest.h"

#include <time.h>

typedef struct ClusterPlanReceiver
{
	DestReceiver pub;
	StringInfoData buf;
	time_t lastCheckTime;	/* last check client message time */
}ClusterPlanReceiver;

static bool cluster_receive_slot(TupleTableSlot *slot, DestReceiver *self);
static void cluster_receive_startup(DestReceiver *self,int operation,TupleDesc typeinfo);
static void cluster_receive_shutdown(DestReceiver *self);
static void cluster_receive_destroy(DestReceiver *self);

/*
 * return ture if got data
 * false is other data
 */
bool clusterRecvTuple(TupleTableSlot *slot, const char *msg, int len)
{
	if(*msg == 'D')
	{
		uint32 t_len = offsetof(MinimalTupleData, t_infomask2) - 1 + len;
		MinimalTuple tup = palloc(t_len);
		MemSet(tup, 0, offsetof(MinimalTupleData, t_infomask2) - offsetof(MinimalTupleData, t_len));
		tup->t_len = t_len;
		memcpy(&tup->t_infomask2, msg + 1, len-1);
		ExecStoreMinimalTuple(tup, slot, true);
		return true;
	}else if(*msg == 'T')
	{
		TupleDesc desc = slot->tts_tupleDescriptor;
		StringInfoData buf;
		int i;
		Oid oid;
		if(desc->tdhasoid != (bool)msg[1])
		{
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("diffent TupleDesc of has Oid"),
				errdetail("local is %d, remote is %d", desc->tdhasoid, msg[1])));
		}
		i = *(int*)(&msg[2]);
		if(i != desc->natts)
		{
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("diffent TupleDesc of number attribute"),
				errdetail("local is %d, remote is %d", desc->natts, i)));
		}
		buf.data = (char*)msg;
		buf.maxlen = buf.len = len;
		buf.cursor = 6;
		for(i=0;i<desc->natts;++i)
		{
			oid = load_oid_type(&buf);
			if(oid != desc->attrs[i]->atttypid)
			{
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
					errmsg("diffent TupleDesc of attribute[%d]", i),
					errdetail("local is %u, remote is %u", desc->attrs[i]->atttypid, oid)));
			}
		}
		return false;
	}else
	{
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
			errmsg("unknown cluster message type %d", msg[0])));
	}
	return false; /* keep compiler quiet */
}

static bool cluster_receive_slot(TupleTableSlot *slot, DestReceiver *self)
{
	ClusterPlanReceiver * r = (ClusterPlanReceiver*)self;
	MinimalTuple tup = ExecFetchSlotMinimalTuple(slot);
	time_t time_now;
	bool need_more_slot;

	resetStringInfo(&r->buf);
	appendStringInfoChar(&r->buf, 'D');
	appendBinaryStringInfo(&r->buf, (char*)&(tup->t_infomask2)
		, tup->t_len - offsetof(MinimalTupleData, t_infomask2));
	pq_putmessage('d', r->buf.data, r->buf.len);
	pq_flush();

	/* check client message */
	time_now = time(NULL);
	need_more_slot = true;
	if(time_now != r->lastCheckTime)
	{
		int n;
		unsigned char first_char;
		r->lastCheckTime = time_now;
		pq_startmsgread();
		n = pq_getbyte_if_available(&first_char);
		if(n == 0)
		{
			/* no message from client */
			pq_endmsgread();
			need_more_slot = true;
		}else if(n < 0)
		{
			/* eof, we don't need more slot */
			pq_endmsgread();
			need_more_slot = false;
		}else
		{
			resetStringInfo(&r->buf);
			if(pq_getmessage(&r->buf, 0))
			{
				ereport(COMMERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("unexpected EOF on coordinator connection")));
				proc_exit(0);
			}

			if(first_char == 'c'
				|| first_char == 'X')
			{
				/* copy end */
				need_more_slot = false;
			}else if(first_char == 'd')
			{
				/* message */
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("not support copy data in yet")));
			}else
			{
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid coordinator message type \"%c\"",
								first_char)));
			}
		}
	}
	return need_more_slot;
}

static void cluster_receive_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	ClusterPlanReceiver * r = (ClusterPlanReceiver*)self;
	int i;

	initStringInfo(&r->buf);

	/* send tuple desc */
	appendStringInfoChar(&r->buf, 'T');
	appendStringInfoChar(&r->buf, typeinfo->tdhasoid);
	appendBinaryStringInfo(&r->buf, (char*)&typeinfo->natts, sizeof(typeinfo->natts));
	for(i=0;i<typeinfo->natts;++i)
		save_oid_type(&r->buf, typeinfo->attrs[i]->atttypid);
	pq_putmessage('d', r->buf.data, r->buf.len);
	pq_flush();
}

static void cluster_receive_shutdown(DestReceiver *self)
{
}

static void cluster_receive_destroy(DestReceiver *self)
{
	ClusterPlanReceiver * r = (ClusterPlanReceiver*)self;
	if(r->buf.data)
		pfree(r->buf.data);
	pfree(r);
}

DestReceiver *createClusterReceiver(void)
{
	ClusterPlanReceiver *self;

	self = palloc0(sizeof(*self));
	self->pub.mydest = DestClusterOut;
	self->pub.receiveSlot = cluster_receive_slot;
	self->pub.rStartup = cluster_receive_startup;
	self->pub.rShutdown = cluster_receive_shutdown;
	self->pub.rDestroy = cluster_receive_destroy;

	return &self->pub;
}