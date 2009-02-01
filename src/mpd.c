/**
 * mpd.c: MPD backend.
 *
 * ==================================================================
 * Copyright (c) 2009 Christoph Mende <angelos@unkreativ.org>
 * Based on Jonathan Coome's work on scmpc
 *
 * This file is part of scmpc.
 *
 * scmpc is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * scmpc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with scmpc; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 * ==================================================================
 */


#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <netinet/in.h>

#include "misc.h"
#include "audioscrobbler.h"
#include "scmpc.h"
#include "mpd.h"
#include "preferences.h"

static int server_connect_unix(const char *path)
{
	int sockfd;
	struct sockaddr_un addr;
	unsigned int len;

	if((sockfd = socket(AF_UNIX,SOCK_STREAM,0))  < 0)
		return -1;

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path,path,strlen(path) + 1);
	len = strlen(addr.sun_path) + sizeof(addr.sun_family);

	if(fcntl(sockfd,F_SETFL,fcntl(sockfd,F_GETFL,0) | O_NONBLOCK) < 0)
		return -1;

	if(connect(sockfd,(struct sockaddr *)&addr,len) < 0)
		return -1;

	return sockfd;
}

static int server_connect_tcp(const char *host, int port)
{
	fd_set write_flags;
	int sockfd, valopt, ret;
	char service[5];
	socklen_t len;
	struct addrinfo hints, *result, *rp;
	struct timeval timeout;

	memset(&hints,0,sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	sprintf(service,"%d",port);

	if((ret = getaddrinfo(host,service,&hints,&result)) != 0) {
		scmpc_log(ERROR,"getaddrinfo: %s",gai_strerror(ret));
		freeaddrinfo(result);
		return -1;
	}

	for(rp = result;rp != NULL;rp = rp->ai_next) {
		if((sockfd = socket(rp->ai_family,rp->ai_socktype,
			rp->ai_protocol)) >= 0) break;

		close(sockfd);
	}

	if(fcntl(sockfd,F_SETFL,fcntl(sockfd,F_GETFL,0) | O_NONBLOCK) < 0) {
		freeaddrinfo(result);
		return -1;
	}

	if(connect(sockfd,rp->ai_addr,rp->ai_addrlen) < 0) {
		if(errno == EINPROGRESS) {
			timeout.tv_sec = prefs.mpd_timeout;
			timeout.tv_usec = 0;

			FD_ZERO(&write_flags);
			FD_SET(sockfd,&write_flags);
			if(select(sockfd+1,NULL,&write_flags,NULL,&timeout) > 0) {
				len = sizeof(int);
				getsockopt(sockfd,SOL_SOCKET,SO_ERROR,
					(void*)(&valopt),&len);
				if(valopt) {
					errno = valopt;
					freeaddrinfo(result);
					return -1;
				}
			}
			else {
				errno = ETIMEDOUT;
				freeaddrinfo(result);
				return -1;
			}
		}
		else {
			freeaddrinfo(result);
			return -1;
		}
	}

	freeaddrinfo(result);
	errno = 0;
	return sockfd;
}

void mpd_write(const char *string)
{
	char *tmp;

	/* exit idle mode before sending commands */
	if(mpd_info->version[0] > 0 || mpd_info->version[1] >= 14)
		write(mpd_info->sockfd,"noidle\n",7);

	asprintf(&tmp,"%s\n",string);
	write(mpd_info->sockfd,string,strlen(tmp));
	free(tmp);

	/* re-enter idle mode */
	if(mpd_info->version[0] > 0 || mpd_info->version[1] >= 14)
		write(mpd_info->sockfd,"idle\n",7);
}

int mpd_connect(void)
{
	char *tmp;

	mpd_info = calloc(sizeof(struct mpd_info),1);
	current_song.filename = strdup("");
	mpd_info->status = DISCONNECTED;
	if(strncmp(prefs.mpd_hostname,"/",1) == 0)
		mpd_info->sockfd = server_connect_unix(prefs.mpd_hostname);
	else
		mpd_info->sockfd = server_connect_tcp(prefs.mpd_hostname,
			prefs.mpd_port);

	if(mpd_info->sockfd < 0) {
		scmpc_log(ERROR,"Failed to connect to MPD: %s",strerror(errno));
		return -1;
	}

	if(strlen(prefs.mpd_password) > 0) {
		asprintf(&tmp,"password %s\n",prefs.mpd_password);
		if(write(mpd_info->sockfd,tmp,strlen(tmp)) < 0) {
			free(tmp);
			scmpc_log(ERROR,"Failed to write to MPD: %s",
				strerror(errno));
			return -1;
		}
		free(tmp);
	}
	mpd_info->status = CONNECTED;
	return 0;
}

void mpd_parse(char *buf)
{
	char *saveptr, *line;

	line = strtok_r(buf,"\n",&saveptr);
	do {
		scmpc_log(DEBUG,"mpd said: %s",line);
		if(strncmp(line,"ACK",3) == 0) {
			if(strstr(line,"incorrect password")) {
				scmpc_log(ERROR,"[MPD] Incorrect password");
				mpd_info->status = BADAUTH;
			} else {
				/* Unknown error */
				scmpc_log(ERROR,"Received ACK error from MPD: "
					"%s",&line[13]);
			}
		}
		else if(strncmp(line,"changed: ",8) == 0) {
			if(strncmp(line,"changed: player",14) == 0) {
				write(mpd_info->sockfd,"currentsong\n",12);
			}
			write(mpd_info->sockfd,"idle\n",5);
		}
		else if(strncmp(line,"file: ",6) == 0) {
			if(strcmp(current_song.filename,&line[6])) {
				free(current_song.filename);
				free(current_song.artist);
				current_song.artist = NULL;
				free(current_song.title);
				current_song.artist = NULL;
				free(current_song.album);
				current_song.artist = NULL;
				current_song.track = 0;
				current_song.filename = strdup(&line[6]);
				while((line = strtok_r(NULL,"\n",&saveptr)) != NULL) {
					if(strncmp(line,"Artist: ",8) == 0)
						current_song.artist = strdup(&line[8]);
					if(strncmp(line,"Album: ",7) == 0)
						current_song.album = strdup(&line[7]);
					if(strncmp(line,"Title: ",7) == 0)
						current_song.title = strdup(&line[7]);
					if(strncmp(line,"Time: ",6) == 0)
						current_song.length = atoi(&line[6]);
					if(strncmp(line,"Track: ",7) == 0)
						current_song.track = atoi(strtok(&line[7],"/"));
				}
				current_song.date = time(NULL);
				as_now_playing();
			}
			continue;
		}
		else if(strncmp(line,"volume: ",8) == 0) {
			
		}
		else if(strncmp(line,"OK MPD",6) == 0) {
			sscanf(line,"%*s %*s %d.%d.%d",&mpd_info->version[0],
				&mpd_info->version[1],&mpd_info->version[2]);
			scmpc_log(INFO,"Connected to MPD.");
			//write(mpd_info->sockfd,"currentsong\n",12);
			write(mpd_info->sockfd,"status\n",7);
			if(mpd_info->version[0] > 0 || mpd_info->version[1] >= 14) {
				write(mpd_info->sockfd,"idle\n",5);
				scmpc_log(INFO,"MPD >= 0.14, using idle");
			}
		}
	} while((line = strtok_r(NULL,"\n",&saveptr)) != NULL);
}

void mpd_cleanup(void)
{
	close(mpd_info->sockfd);
	free(mpd_info);
	free(current_song.filename);
	free(current_song.artist);
	free(current_song.title);
	free(current_song.album);
}
