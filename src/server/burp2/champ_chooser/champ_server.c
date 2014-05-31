#include "include.h"

#include <sys/un.h>

// FIX THIS: test error conditions.
static int champ_chooser_new_client(struct async *as, struct conf *conf)
{
	socklen_t t;
	int fd=-1;
	struct asfd *newfd=NULL;
	struct sockaddr_un remote;

	t=sizeof(remote);
	if((fd=accept(as->asfd->fd, (struct sockaddr *)&remote, &t))<0)
	{
		logp("accept error in %s: %s\n", __func__, strerror(errno));
		goto error;
	}
	set_non_blocking(fd);

	if(!(newfd=asfd_alloc())
	  || newfd->init(newfd, "(unknown)", as, fd, NULL, conf)
	  || !(newfd->blist=blist_alloc()))
		goto error;
	as->asfd_add(as, newfd);

	logp("Connected to fd %d\n", newfd->fd);

	return 0;
error:
	asfd_free(&newfd);
	return -1;
}

static int deduplicate_maybe(struct asfd *asfd,
	struct blk *blk, struct conf *conf)
{
	if(!asfd->in && !(asfd->in=incoming_alloc())) return -1;

	blk->fingerprint=strtoull(blk->weak, 0, 16);
	if(is_hook(blk->weak))
	{
		if(incoming_grow_maybe(asfd->in)) return -1;
		asfd->in->weak[asfd->in->size-1]=blk->fingerprint;
	}
	if(++(asfd->blkcnt)<MANIFEST_SIG_MAX) return 0;
	asfd->blkcnt=0;

	if(deduplicate(asfd, conf)<0) return -1;

	return 0;
}

static int deal_with_rbuf_sig(struct asfd *asfd,
	struct conf *conf)
{
	struct blk *blk;
	if(!(blk=blk_alloc())) return -1;

	blist_add_blk(asfd->blist, blk);

	// FIX THIS: Should not just load into strings.
	if(split_sig(asfd->rbuf->buf,
		asfd->rbuf->len, blk->weak, blk->strong)) return -1;

	printf("Got weak/strong from %d: %lu - %s %s\n",
		asfd->fd, blk->index, blk->weak, blk->strong);
return 0;

	return deduplicate_maybe(asfd, blk, conf);
}

static int deal_with_client_rbuf(struct asfd *asfd, struct conf *conf)
{
	if(asfd->rbuf->cmd==CMD_GEN)
	{
		if(!strncmp_w(asfd->rbuf->buf, "cname:"))
		{
			struct iobuf wbuf;
			if(asfd->desc) free(asfd->desc);
			if(!(asfd->desc=strdup_w(asfd->rbuf->buf
				+strlen("cname:"), __func__)))
					goto error;
			logp("%d has name: %s\n", asfd->fd, asfd->desc);
			iobuf_set(&wbuf, CMD_GEN,
				(char *)"cname ok", strlen("cname ok"));

			if(asfd->write(asfd, &wbuf))
				goto error;
		}
		else
		{
			iobuf_log_unexpected(asfd->rbuf, __func__);
			goto error;
		}
	}
	else if(asfd->rbuf->cmd==CMD_SIG)
	{
		if(deal_with_rbuf_sig(asfd, conf)) goto error;
	}
	else
	{
		iobuf_log_unexpected(asfd->rbuf, __func__);
		goto error;
	}
	iobuf_free_content(asfd->rbuf);
	return 0;
error:
	iobuf_free_content(asfd->rbuf);
	return -1;
}

int champ_chooser_server(struct sdirs *sdirs, struct conf *conf)
{
	int s;
	int ret=-1;
	int len;
	struct asfd *asfd=NULL;
	struct sockaddr_un local;
	struct lock *lock=NULL;
	struct async *as=NULL;
	int started=0;

	if(!(lock=lock_alloc_and_init(sdirs->champlock)))
		goto end;
	lock_get(lock);
	switch(lock->status)
	{
		case GET_LOCK_GOT:
			set_logfp(sdirs->champlog, conf);
			logp("Got champ lock for dedup_group: %s\n",
				conf->dedup_group);
			break;
		case GET_LOCK_NOT_GOT:
		case GET_LOCK_ERROR:
		default:
			//logp("Did not get champ lock\n");
			goto end;
	}

	unlink(local.sun_path);
	if((s=socket(AF_UNIX, SOCK_STREAM, 0))<0)
	{
		logp("socket error in %s: %s\n", __func__, strerror(errno));
		goto end;
	}

	memset(&local, 0, sizeof(struct sockaddr_un));
	local.sun_family=AF_UNIX;
	strcpy(local.sun_path, sdirs->champsock);
	len=strlen(local.sun_path)+sizeof(local.sun_family);
	unlink(sdirs->champsock);
	if(bind(s, (struct sockaddr *)&local, len)<0)
	{
		logp("bind error in %s: %s\n", __func__, strerror(errno));
		goto end;
	}

	if(listen(s, 5)<0)
	{
		logp("listen error in %s: %s\n", __func__, strerror(errno));
		goto end;
	}
	set_non_blocking(s);

	if(!(as=async_alloc())
	  || !(asfd=asfd_alloc())
	  || as->init(as, 0)
	  || asfd->init(asfd, "champ chooser main socket", as, s, NULL, conf))
		goto end;
	as->asfd_add(as, asfd);
	asfd->listening_for_new_clients=1;

	// Load the sparse indexes for this dedup group.
	if(champ_chooser_init(sdirs->data, conf))
		goto end;

	while(1)
	{
		switch(as->read_write(as))
		{
			case 0:
				// Check the main socket last, as it might add
				// a new client to the list.
				for(asfd=as->asfd->next; asfd; asfd=asfd->next)
				{
					while(asfd->rbuf->buf)
					{
						if(deal_with_client_rbuf(asfd,
							conf)) goto end;
						// Get as much out of the
						// readbuf as possible.
						if(asfd->parse_readbuf(asfd))
							goto end;
					}
				}
				if(as->asfd->new_client)
				{
					// Incoming client.
					as->asfd->new_client=0;
					if(champ_chooser_new_client(as, conf))
						goto end;
					started=1;
				}
				break;
			default:
				// Maybe one of the fds had a problem.
				// Find and remove it and carry on if possible.
				for(asfd=as->asfd->next; asfd; asfd=asfd->next)
				{
					if(!asfd->want_to_remove) continue;
					as->asfd_remove(as, asfd);
					logp("%d disconnected: %s\n",
						asfd->fd, asfd->desc);
					asfd_free(&asfd);
				}
				// If we got here, there was no fd to remove.
				// It is a fatal error.
				goto end;
		}
				
		if(started && !as->asfd->next)
		{
			logp("All clients disconnected.\n");
			ret=0;
			break;
		}
	}

end:
	logp("champ chooser exiting: %d\n", ret);
	set_logfp(NULL, conf);
	async_free(&as);
	asfd_free(&asfd); // This closes s for us.
	close_fd(&s);
	unlink(sdirs->champsock);
// FIX THIS: free asfds.
	lock_release(lock);
	lock_free(&lock);
	return ret;
}

// The return code of this is the return code of the standalone process.
int champ_chooser_server_standalone(struct conf *conf, const char *sclient)
{
	int ret=1;
	struct sdirs *sdirs=NULL;
	struct conf *cconf=NULL;

	if(!(cconf=conf_alloc()))
		goto end;
	conf_init(cconf);
	// We need to be given a client name and load the relevant server side
	// clientconfdir file, because various settings may be overridden
	// there.
	if(!(cconf->cname=strdup(sclient))
	  || conf_load_client(conf, cconf)
	  || !(sdirs=sdirs_alloc())
	  || sdirs_init(sdirs, cconf)
	  || champ_chooser_server(sdirs, cconf))
		goto end;
	ret=0;
end:
	conf_free(cconf);
	sdirs_free(&sdirs);
	return ret;
}