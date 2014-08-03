#include "include.h"

static int write_all(struct asfd *asfd)
{
	int ret=-1;
	size_t len;
	const unsigned char *buf;
	yajl_gen_get_buf(yajl, &buf, &len);
	ret=asfd->write_strn(asfd, CMD_GEN /* not used */,
		(const char *)buf, len);
	yajl_gen_clear(yajl);
	return ret;
}

int json_start(struct asfd *asfd)
{
	if(!yajl)
	{
		if(!(yajl=yajl_gen_alloc(NULL)))
			return -1;
		yajl_gen_config(yajl, yajl_gen_beautify, 1);
	}
	if(yajl_map_open_w()
	  || yajl_gen_str_w("clients")
	  || yajl_array_open_w())
		return -1;
	return 0;
}

int json_end(struct asfd *asfd)
{
	int ret=-1;
	if(yajl_array_close_w()
	  || yajl_map_close_w())
		goto end;
	ret=write_all(asfd);
end:
	yajl_gen_free(yajl);
	yajl=NULL;
	return ret;
}

static long timestamp_to_long(const char *buf)
{
	struct tm tm;
	const char *b=NULL;
	if(!(b=strchr(buf, ' '))) return 0;
	memset(&tm, 0, sizeof(struct tm));
	if(!strptime(b, " %Y-%m-%d %H:%M:%S", &tm)) return 0;
	// Tell mktime to use the daylight savings time setting
	// from the time zone of the system.
	tm.tm_isdst=-1;
	return (long)mktime(&tm);
}

static int json_send_backup(struct asfd *asfd, struct bu *bu)
{
	long long bno=0;
	long long deletable=0;
	long long timestamp=0;
	if(bu)
	{
		bno=(long long)bu->bno;
		deletable=(long long)bu->deletable;
		timestamp=(long long)timestamp_to_long(bu->timestamp);
	}

	if(yajl_map_open_w()
	  || yajl_gen_int_pair_w("number", bno)
	  || yajl_gen_int_pair_w("deletable", deletable)
	  || yajl_gen_int_pair_w("timestamp", timestamp)
	  || yajl_gen_map_close(yajl)!=yajl_gen_status_ok)
		return -1;

	return 0;
}

static int json_send_client_start(struct asfd *asfd,
	struct cstat *clist, struct cstat *cstat)
{
	const char *status=cstat_status_to_str(cstat);
	struct bu *bu_current=cstat->bu_current;
	long long bno=0;
	long long timestamp=0;
	if(bu_current)
	{
		bno=(long long)bu_current->bno;
		timestamp=(long long)timestamp_to_long(bu_current->timestamp);
	}

	if(yajl_map_open_w()
	  || yajl_gen_str_pair_w("name", cstat->name)
	  || yajl_gen_str_pair_w("status", status)
	  || yajl_gen_int_pair_w("number", bno)
	  || yajl_gen_int_pair_w("timestamp", timestamp)
	  || yajl_gen_str_w("backups")
	  || yajl_array_open_w())
			return -1;
	return 0;
}

static int json_send_client_end(struct asfd *asfd)
{
	if(yajl_array_close_w()
	  || yajl_map_close_w())
		return -1;
	return 0;
}

int json_send_backup_list(struct asfd *asfd,
	struct cstat *clist, struct cstat *cstat)
{
	int ret=-1;
	struct bu *bu;
	if(json_send_client_start(asfd, clist, cstat)) return -1;
	for(bu=cstat->bu; bu; bu=bu->prev)
	{
		if(json_send_backup(asfd, bu))
			goto end;
	}
	ret=0;
end:
	if(json_send_client_end(asfd)) ret=-1;
	return ret;
}
