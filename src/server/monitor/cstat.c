#include "include.h"

#include <dirent.h>

static int set_cstat_from_conf(struct cstat *c, struct conf *cconf)
{
	sdirs_free_content((struct sdirs *)c->sdirs);
	if(sdirs_init((struct sdirs *)c->sdirs, cconf)) return -1;
	return 0;
}

static int get_client_names(struct cstat **clist, struct conf *conf)
{
	int m=0;
	int n=-1;
	int ret=-1;
	struct cstat *c;
	struct cstat *cnew;
	struct dirent **dir=NULL;

	if((n=scandir(conf->clientconfdir, &dir, 0, 0))<0)
	{
		logp("could not scandir clientconfdir: %s\n",
			conf->clientconfdir, strerror(errno));
		goto end;
	}
        for(m=0; m<n; m++)
	{
		if(dir[m]->d_ino==0
		// looks_like...() also avoids '.' and '..'.
		  || looks_like_tmp_or_hidden_file(dir[m]->d_name))
			continue;
		for(c=*clist; c; c=c->next)
		{
			if(!c->name) continue;
			if(!strcmp(dir[m]->d_name, c->name))
				break;
		}
		if(c) continue;

		// We do not have this client yet. Add it.
		if(!(cnew=cstat_alloc())
		  || cstat_init(cnew, dir[m]->d_name, conf->clientconfdir)
		  || cstat_add_to_list(clist, cnew))
			goto end;
	}

	ret=0;
end:
	for(m=0; m<n; m++) free_v((void **)&dir[m]);
	free_v((void **)&dir);
	return ret;
}

static void cstat_remove(struct cstat **clist, struct cstat **cstat)
{
	struct cstat *c;
	struct cstat *clast=NULL;
	if(!cstat || !*cstat) return;
	if(*clist==*cstat)
	{
		*clist=(*cstat)->next;
		cstat_free(cstat);
		*cstat=*clist;
		return;
	}
	for(c=*clist; c; c=c->next)
	{
		if(c->next!=*cstat)
		{
			clast=c;
			continue;
		}
		c->next=(*cstat)->next;
		c->prev=clast;
		cstat_free(cstat);
		*cstat=*clist;

		return;
	}
}

static int reload_from_client_confs(struct cstat **clist, struct conf *conf)
{
	struct cstat *c;
	static struct conf *cconf=NULL;

	if(!cconf && !(cconf=conf_alloc())) goto error;

	while(1)
	{
		for(c=*clist; c; c=c->next)
		{
			// Look at the client conf files to see if they have
			// changed, and reload bits and pieces if they have.
			struct stat statp;

			if(!c->conffile) continue;
			if(stat(c->conffile, &statp)
			  || !S_ISREG(statp.st_mode))
			{
				cstat_remove(clist, &c);
				break; // Go to the beginning of the list.
			}
			if(statp.st_mtime==c->conf_mtime)
			{
				// conf file has not changed - no need to do
				// anything.
				continue;
			}
			c->conf_mtime=statp.st_mtime;

			conf_free_content(cconf);
			if(!(cconf->cname=strdup_w(c->name, __func__)))
				goto error;
			if(conf_set_client_global(conf, cconf)
			  || conf_load(c->conffile, cconf, 0))
			{
				cstat_remove(clist, &c);
				break; // Go to the beginning of the list.
			}

			if(set_cstat_from_conf(c, cconf))
				goto error;
		}
		// Only stop if the end of the list was not reached.
		if(!c) break;
	}
	return 0;
error:
	conf_free(cconf);
	cconf=NULL;
	return -1;
}

int cstat_set_status(struct cstat *cstat)
{
	struct stat statp;
	struct sdirs *sdirs=(struct sdirs *)cstat->sdirs;

	if(lstat(sdirs->lock->path, &statp))
	{
		if(lstat(sdirs->working, &statp))
			cstat->status=STATUS_IDLE;
		else
			cstat->status=STATUS_CLIENT_CRASHED;
		// It is not running, so free the running_detail.
		free_w(&cstat->running_detail);
	}
	else
	{
		if(!lock_test(sdirs->lock->path)) // Could have got lock.
		{
			cstat->status=STATUS_SERVER_CRASHED;
			// It is not running, so free the running_detail.
			free_w(&cstat->running_detail);
		}
		else
			cstat->status=STATUS_RUNNING;
	}

	return 0;
}

static int reload_from_clientdir(struct cstat **clist, struct conf *conf)
{
	struct cstat *c;
	for(c=*clist; c; c=c->next)
	{
		time_t ltime=0;
		struct stat statp;
		struct stat lstatp;
		struct sdirs *sdirs=(struct sdirs *)c->sdirs;
		if(!sdirs->client) continue;
		if(stat(sdirs->client, &statp))
		{
			// No clientdir.
			if(!c->status
			  && cstat_set_status(c))
				goto error;
			continue;
		}
		if(!lstat(sdirs->lock->path, &lstatp))
			ltime=lstatp.st_mtime;
		if(statp.st_mtime==c->clientdir_mtime
		  && ltime==c->lockfile_mtime
		  && c->status!=STATUS_SERVER_CRASHED
		  && !c->running_detail)
		{
			// clientdir has not changed - no need to do anything.
			continue;
		}
		c->clientdir_mtime=statp.st_mtime;
		c->lockfile_mtime=ltime;
		if(cstat_set_status(c)) goto error;


printf("reload from clientdir\n");
		bu_list_free(&c->bu);
		if(bu_get_current(sdirs, &c->bu))
			goto error;
	}
	return 0;
error:
	return -1;
}

int cstat_load_data_from_disk(struct cstat **clist, struct conf *conf)
{
	return get_client_names(clist, conf)
	  || reload_from_client_confs(clist, conf)
	  || reload_from_clientdir(clist, conf);
}

int cstat_set_backup_list(struct cstat *cstat)
{
	struct bu *bu=NULL;

	// Free any previous list.
	bu_list_free(&cstat->bu);

	if(bu_get_list_with_working((struct sdirs *)cstat->sdirs, &bu))
	{
		//logp("error when looking up current backups\n");
		return 0;
	}

	// Find the end of the list just loaded, so we can traverse
	// it backwards later.
	while(bu && bu->next) bu=bu->next;

	cstat->bu=bu;
	return 0;
}
