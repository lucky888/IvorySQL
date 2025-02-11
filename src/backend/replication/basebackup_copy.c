/*-------------------------------------------------------------------------
 *
 * basebackup_copy.c
 *	  send basebackup archives using COPY OUT
 *
 * We have two different ways of doing this.
 *
 * 'copytblspc' is an older method still supported for compatibility
 * with releases prior to v15. In this method, a separate COPY OUT
 * operation is used for each tablespace. The manifest, if it is sent,
 * uses an additional COPY OUT operation.
 *
 * 'copystream' sends a starts a single COPY OUT operation and transmits
 * all the archives and the manifest if present during the course of that
 * single COPY OUT. Each CopyData message begins with a type byte,
 * allowing us to signal the start of a new archive, or the manifest,
 * by some means other than ending the COPY stream. This also allows
 * this protocol to be extended more easily, since we can include
 * arbitrary information in the message stream as long as we're certain
 * that the client will know what to do with it.
 *
 * Regardless of which method is used, we sent a result set with
 * information about the tabelspaces to be included in the backup before
 * starting COPY OUT. This result has the same format in every method.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/basebackup_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "replication/basebackup.h"
#include "replication/basebackup_sink.h"
#include "utils/timestamp.h"

typedef struct bbsink_copystream
{
	/* Common information for all types of sink. */
	bbsink		base;

	/*
	 * Protocol message buffer. We assemble CopyData protocol messages by
	 * setting the first character of this buffer to 'd' (archive or manifest
	 * data) and then making base.bbs_buffer point to the second character so
	 * that the rest of the data gets copied into the message just where we
	 * want it.
	 */
	char	   *msgbuffer;

	/*
	 * When did we last report progress to the client, and how much progress
	 * did we report?
	 */
	TimestampTz last_progress_report_time;
	uint64		bytes_done_at_last_time_check;
} bbsink_copystream;

/*
 * We don't want to send progress messages to the client excessively
 * frequently. Ideally, we'd like to send a message when the time since the
 * last message reaches PROGRESS_REPORT_MILLISECOND_THRESHOLD, but checking
 * the system time every time we send a tiny bit of data seems too expensive.
 * So we only check it after the number of bytes sine the last check reaches
 * PROGRESS_REPORT_BYTE_INTERVAL.
 */
#define	PROGRESS_REPORT_BYTE_INTERVAL				65536
#define PROGRESS_REPORT_MILLISECOND_THRESHOLD		1000

static void bbsink_copystream_begin_backup(bbsink *sink);
static void bbsink_copystream_begin_archive(bbsink *sink,
											const char *archive_name);
static void bbsink_copystream_archive_contents(bbsink *sink, size_t len);
static void bbsink_copystream_end_archive(bbsink *sink);
static void bbsink_copystream_begin_manifest(bbsink *sink);
static void bbsink_copystream_manifest_contents(bbsink *sink, size_t len);
static void bbsink_copystream_end_manifest(bbsink *sink);
static void bbsink_copystream_end_backup(bbsink *sink, XLogRecPtr endptr,
										 TimeLineID endtli);
static void bbsink_copystream_cleanup(bbsink *sink);

static void bbsink_copytblspc_begin_backup(bbsink *sink);
static void bbsink_copytblspc_begin_archive(bbsink *sink,
											const char *archive_name);
static void bbsink_copytblspc_archive_contents(bbsink *sink, size_t len);
static void bbsink_copytblspc_end_archive(bbsink *sink);
static void bbsink_copytblspc_begin_manifest(bbsink *sink);
static void bbsink_copytblspc_manifest_contents(bbsink *sink, size_t len);
static void bbsink_copytblspc_end_manifest(bbsink *sink);
static void bbsink_copytblspc_end_backup(bbsink *sink, XLogRecPtr endptr,
										 TimeLineID endtli);
static void bbsink_copytblspc_cleanup(bbsink *sink);

static void SendCopyOutResponse(void);
static void SendCopyData(const char *data, size_t len);
static void SendCopyDone(void);
static void SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli);
static void SendTablespaceList(List *tablespaces);
static void send_int8_string(StringInfoData *buf, int64 intval);

const bbsink_ops bbsink_copystream_ops = {
	.begin_backup = bbsink_copystream_begin_backup,
	.begin_archive = bbsink_copystream_begin_archive,
	.archive_contents = bbsink_copystream_archive_contents,
	.end_archive = bbsink_copystream_end_archive,
	.begin_manifest = bbsink_copystream_begin_manifest,
	.manifest_contents = bbsink_copystream_manifest_contents,
	.end_manifest = bbsink_copystream_end_manifest,
	.end_backup = bbsink_copystream_end_backup,
	.cleanup = bbsink_copystream_cleanup
};

const bbsink_ops bbsink_copytblspc_ops = {
	.begin_backup = bbsink_copytblspc_begin_backup,
	.begin_archive = bbsink_copytblspc_begin_archive,
	.archive_contents = bbsink_copytblspc_archive_contents,
	.end_archive = bbsink_copytblspc_end_archive,
	.begin_manifest = bbsink_copytblspc_begin_manifest,
	.manifest_contents = bbsink_copytblspc_manifest_contents,
	.end_manifest = bbsink_copytblspc_end_manifest,
	.end_backup = bbsink_copytblspc_end_backup,
	.cleanup = bbsink_copytblspc_cleanup
};

/*
 * Create a new 'copystream' bbsink.
 */
bbsink *
bbsink_copystream_new(void)
{
	bbsink_copystream *sink = palloc0(sizeof(bbsink_copystream));

	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_copystream_ops;

	/* Set up for periodic progress reporting. */
	sink->last_progress_report_time = GetCurrentTimestamp();
	sink->bytes_done_at_last_time_check = UINT64CONST(0);

	return &sink->base;
}

/*
 * Send start-of-backup wire protocol messages.
 */
static void
bbsink_copystream_begin_backup(bbsink *sink)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = sink->bbs_state;
	char *buf;

	/*
	 * Initialize buffer. We ultimately want to send the archive and manifest
	 * data by means of CopyData messages where the payload portion of each
	 * message begins with a type byte. However, basebackup.c expects the
	 * buffer to be aligned, so we can't just allocate one extra byte for the
	 * type byte. Instead, allocate enough extra bytes that the portion of
	 * the buffer we reveal to our callers can be aligned, while leaving room
	 * to slip the type byte in just beforehand.  That will allow us to ship
	 * the data with a single call to pq_putmessage and without needing any
	 * extra copying.
	 */
	buf = palloc(mysink->base.bbs_buffer_length + MAXIMUM_ALIGNOF);
	mysink->msgbuffer = buf + (MAXIMUM_ALIGNOF - 1);
	mysink->base.bbs_buffer = buf + MAXIMUM_ALIGNOF;
	mysink->msgbuffer[0] = 'd'; /* archive or manifest data */

	/* Tell client the backup start location. */
	SendXlogRecPtrResult(state->startptr, state->starttli);

	/* Send client a list of tablespaces. */
	SendTablespaceList(state->tablespaces);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");

	/* Begin COPY stream. This will be used for all archives + manifest. */
	SendCopyOutResponse();
}

/*
 * Send a CopyData message announcing the beginning of a new archive.
 */
static void
bbsink_copystream_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_state *state = sink->bbs_state;
	tablespaceinfo *ti;
	StringInfoData buf;

	ti = list_nth(state->tablespaces, state->tablespace_num);
	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'n');		/* New archive */
	pq_sendstring(&buf, archive_name);
	pq_sendstring(&buf, ti->path == NULL ? "" : ti->path);
	pq_endmessage(&buf);
}

/*
 * Send a CopyData message containing a chunk of archive content.
 */
static void
bbsink_copystream_archive_contents(bbsink *sink, size_t len)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = mysink->base.bbs_state;
	StringInfoData buf;
	uint64		targetbytes;

	/* Send the archive content to the client (with leading type byte). */
	pq_putmessage('d', mysink->msgbuffer, len + 1);

	/* Consider whether to send a progress report to the client. */
	targetbytes = mysink->bytes_done_at_last_time_check
		+ PROGRESS_REPORT_BYTE_INTERVAL;
	if (targetbytes <= state->bytes_done)
	{
		TimestampTz now = GetCurrentTimestamp();
		long		ms;

		/*
		 * OK, we've sent a decent number of bytes, so check the system time
		 * to see whether we're due to send a progress report.
		 */
		mysink->bytes_done_at_last_time_check = state->bytes_done;
		ms = TimestampDifferenceMilliseconds(mysink->last_progress_report_time,
											 now);

		/*
		 * Send a progress report if enough time has passed. Also send one if
		 * the system clock was set backward, so that such occurrences don't
		 * have the effect of suppressing further progress messages.
		 */
		if (ms < 0 || ms >= PROGRESS_REPORT_MILLISECOND_THRESHOLD)
		{
			mysink->last_progress_report_time = now;

			pq_beginmessage(&buf, 'd'); /* CopyData */
			pq_sendbyte(&buf, 'p'); /* Progress report */
			pq_sendint64(&buf, state->bytes_done);
			pq_endmessage(&buf);
			pq_flush_if_writable();
		}
	}
}

/*
 * We don't need to explicitly signal the end of the archive; the client
 * will figure out that we've reached the end when we begin the next one,
 * or begin the manifest, or end the COPY stream. However, this seems like
 * a good time to force out a progress report. One reason for that is that
 * if this is the last archive, and we don't force a progress report now,
 * the client will never be told that we sent all the bytes.
 */
static void
bbsink_copystream_end_archive(bbsink *sink)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = mysink->base.bbs_state;
	StringInfoData buf;

	mysink->bytes_done_at_last_time_check = state->bytes_done;
	mysink->last_progress_report_time = GetCurrentTimestamp();
	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'p');		/* Progress report */
	pq_sendint64(&buf, state->bytes_done);
	pq_endmessage(&buf);
	pq_flush_if_writable();
}

/*
 * Send a CopyData message announcing the beginning of the backup manifest.
 */
static void
bbsink_copystream_begin_manifest(bbsink *sink)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'm');		/* Manifest */
	pq_endmessage(&buf);
}

/*
 * Each chunk of manifest data is sent using a CopyData message.
 */
static void
bbsink_copystream_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;

	/* Send the manifest content to the client (with leading type byte). */
	pq_putmessage('d', mysink->msgbuffer, len + 1);
}

/*
 * We don't need an explicit terminator for the backup manifest.
 */
static void
bbsink_copystream_end_manifest(bbsink *sink)
{
	/* Do nothing. */
}

/*
 * Send end-of-backup wire protocol messages.
 */
static void
bbsink_copystream_end_backup(bbsink *sink, XLogRecPtr endptr,
							 TimeLineID endtli)
{
	SendCopyDone();
	SendXlogRecPtrResult(endptr, endtli);
}

/*
 * Cleanup.
 */
static void
bbsink_copystream_cleanup(bbsink *sink)
{
	/* Nothing to do. */
}

/*
 * Create a new 'copytblspc' bbsink.
 */
bbsink *
bbsink_copytblspc_new(void)
{
	bbsink	   *sink = palloc0(sizeof(bbsink));

	*((const bbsink_ops **) &sink->bbs_ops) = &bbsink_copytblspc_ops;

	return sink;
}

/*
 * Begin backup.
 */
static void
bbsink_copytblspc_begin_backup(bbsink *sink)
{
	bbsink_state *state = sink->bbs_state;

	/* Create a suitable buffer. */
	sink->bbs_buffer = palloc(sink->bbs_buffer_length);

	/* Tell client the backup start location. */
	SendXlogRecPtrResult(state->startptr, state->starttli);

	/* Send client a list of tablespaces. */
	SendTablespaceList(state->tablespaces);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Each archive is set as a separate stream of COPY data, and thus begins
 * with a CopyOutResponse message.
 */
static void
bbsink_copytblspc_begin_archive(bbsink *sink, const char *archive_name)
{
	SendCopyOutResponse();
}

/*
 * Each chunk of data within the archive is sent as a CopyData message.
 */
static void
bbsink_copytblspc_archive_contents(bbsink *sink, size_t len)
{
	SendCopyData(sink->bbs_buffer, len);
}

/*
 * The archive is terminated by a CopyDone message.
 */
static void
bbsink_copytblspc_end_archive(bbsink *sink)
{
	SendCopyDone();
}

/*
 * The backup manifest is sent as a separate stream of COPY data, and thus
 * begins with a CopyOutResponse message.
 */
static void
bbsink_copytblspc_begin_manifest(bbsink *sink)
{
	SendCopyOutResponse();
}

/*
 * Each chunk of manifest data is sent using a CopyData message.
 */
static void
bbsink_copytblspc_manifest_contents(bbsink *sink, size_t len)
{
	SendCopyData(sink->bbs_buffer, len);
}

/*
 * When we've finished sending the manifest, send a CopyDone message.
 */
static void
bbsink_copytblspc_end_manifest(bbsink *sink)
{
	SendCopyDone();
}

/*
 * Send end-of-backup wire protocol messages.
 */
static void
bbsink_copytblspc_end_backup(bbsink *sink, XLogRecPtr endptr,
							 TimeLineID endtli)
{
	SendXlogRecPtrResult(endptr, endtli);
}

/*
 * Cleanup.
 */
static void
bbsink_copytblspc_cleanup(bbsink *sink)
{
	/* Nothing to do. */
}

/*
 * Send a CopyOutResponse message.
 */
static void
SendCopyOutResponse(void)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* overall format */
	pq_sendint16(&buf, 0);		/* natts */
	pq_endmessage(&buf);
}

/*
 * Send a CopyData message.
 */
static void
SendCopyData(const char *data, size_t len)
{
	pq_putmessage('d', data, len);
}

/*
 * Send a CopyDone message.
 */
static void
SendCopyDone(void)
{
	pq_putemptymessage('c');
}

/*
 * Send a single resultset containing just a single
 * XLogRecPtr record (in text format)
 */
static void
SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli)
{
	StringInfoData buf;
	char		str[MAXFNAMELEN];
	Size		len;

	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 2);		/* 2 fields */

	/* Field headers */
	pq_sendstring(&buf, "recptr");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, TEXTOID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	pq_sendstring(&buf, "tli");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */

	/*
	 * int8 may seem like a surprising data type for this, but in theory int4
	 * would not be wide enough for this, as TimeLineID is unsigned.
	 */
	pq_sendint32(&buf, INT8OID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	/* Data row */
	pq_beginmessage(&buf, 'D');
	pq_sendint16(&buf, 2);		/* number of columns */

	len = snprintf(str, sizeof(str),
				   "%X/%X", LSN_FORMAT_ARGS(ptr));
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	len = snprintf(str, sizeof(str), "%u", tli);
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	pq_endmessage(&buf);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Send a result set via libpq describing the tablespace list.
 */
static void
SendTablespaceList(List *tablespaces)
{
	StringInfoData buf;
	ListCell   *lc;

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 3);		/* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, OIDOID); /* type oid */
	pq_sendint16(&buf, 4);		/* typlen */
	pq_sendint32(&buf, 0);		/* typmod */
	pq_sendint16(&buf, 0);		/* format code */

	/* Second field - spclocation */
	pq_sendstring(&buf, "spclocation");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, TEXTOID);
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, INT8OID);
	pq_sendint16(&buf, 8);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);

		/* Send one datarow message */
		pq_beginmessage(&buf, 'D');
		pq_sendint16(&buf, 3);	/* number of columns */
		if (ti->path == NULL)
		{
			pq_sendint32(&buf, -1); /* Length = -1 ==> NULL */
			pq_sendint32(&buf, -1);
		}
		else
		{
			Size		len;

			len = strlen(ti->oid);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->oid, len);

			len = strlen(ti->path);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->path, len);
		}
		if (ti->size >= 0)
			send_int8_string(&buf, ti->size / 1024);
		else
			pq_sendint32(&buf, -1); /* NULL */

		pq_endmessage(&buf);
	}
}

/*
 * Send a 64-bit integer as a string via the wire protocol.
 */
static void
send_int8_string(StringInfoData *buf, int64 intval)
{
	char		is[32];

	sprintf(is, INT64_FORMAT, intval);
	pq_sendint32(buf, strlen(is));
	pq_sendbytes(buf, is, strlen(is));
}
