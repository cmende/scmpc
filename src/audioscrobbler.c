/**
 * audioscrobbler.c: Audioscrobbler backend.
 *
 * ==================================================================
 * Copyright (c) 2008 Christoph Mende <angelos@unkreativ.org>
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "misc.h"
#include "preferences.h"
#include "audioscrobbler.h"

static char curl_error_buffer[CURL_ERROR_SIZE];

#define HANDSHAKE_URL \
	"http://post.audioscrobbler.com/?hs=true&p=1.2.1&c=spc&v=%s&u=%s&t=%u&a=%s"

void as_connection_init(void)
{
	as_conn = calloc(sizeof(struct as_connection),1);
	if(as_conn == NULL) return;

	as_conn->last_handshake = 0;
	as_conn->status = DISCONNECTED;
	as_conn->handle = curl_easy_init();
	if(as_conn->handle == NULL) return;
	as_conn->headers = curl_slist_append(as_conn->headers,
			"User-Agent: scmpc/" PACKAGE_VERSION);

	curl_easy_setopt(as_conn->handle,CURLOPT_HTTPHEADER,as_conn->headers);
	curl_easy_setopt(as_conn->handle,CURLOPT_WRITEFUNCTION,&buffer_write);
	curl_easy_setopt(as_conn->handle,CURLOPT_ERRORBUFFER,curl_error_buffer);
	curl_easy_setopt(as_conn->handle,CURLOPT_NOSIGNAL,1L);
	curl_easy_setopt(as_conn->handle,CURLOPT_CONNECTTIMEOUT,5L);
}

void as_cleanup(void)
{
	curl_slist_free_all(as_conn->headers);
	curl_easy_cleanup(as_conn->handle);
	free(as_conn);
}

void as_handshake(void)
{
	char *auth_token, *handshake_url, *line, *saveptr, *tmp;
	time_t timestamp;
	int ret;

	if(strlen(prefs.as_username) == 0 || (strlen(prefs.as_password) == 0 &&
		strlen(prefs.as_password_hash) == 0)) {
		scmpc_log(INFO,"No username or password specified. "
				"Not connecting to Audioscrobbler.");
		as_conn->status = BADAUTH;
		return;
	}

	timestamp = time(NULL);

	if(difftime(timestamp,as_conn->last_handshake) < 1800) {
		scmpc_log(DEBUG,"Requested handshake, but last handshake was less than 30 "
				"minutes ago.");
		return;
	}

	if(strlen(prefs.as_password_hash) > 0) {
		printf("wwa\n");
		if(asprintf(&tmp,"%s%u",prefs.as_password_hash,timestamp) < 0) return;
	} else {
		if(asprintf(&tmp,"%s%u",md5_hash(prefs.as_password),timestamp) < 0) return;
	}
	auth_token = md5_hash(tmp);
	free(tmp);
	if(asprintf(&handshake_url,HANDSHAKE_URL,PACKAGE_VERSION,prefs.as_username,
		timestamp,auth_token) < 0) return;

	scmpc_log(DEBUG,"handshake_url = %s",handshake_url);

	buffer = malloc(1024);
	if(buffer == NULL) return;

	curl_easy_setopt(as_conn->handle,CURLOPT_WRITEDATA,(void *)buffer);
	curl_easy_setopt(as_conn->handle,CURLOPT_HTTPGET,1);
	curl_easy_setopt(as_conn->handle,CURLOPT_URL,handshake_url);

	ret = curl_easy_perform(as_conn->handle);
	free(handshake_url);

	if(ret != 0) {
		scmpc_log(ERROR,"Could not connect to the Audioscrobbler"
			" server: %s",curl_easy_strerror(ret));
		free(buffer);
		return;
	}

	line = strtok_r(buffer,"\n",&saveptr);
	if(line == NULL) {
		scmpc_log(DEBUG,"Could not parse Audioscrobbler handshake response.");
		free(buffer);
	}

	if(strncmp(line,"OK",2) == 0) {
		unsigned short int line_no = 1;

		while((line = strtok_r(NULL,"\n",&saveptr)) != NULL) {
			line_no++;
			if(line_no == 2) {
				as_conn->session_id = line;
			} else if(line_no == 3) {
				as_conn->np_url = line;
			} else if(line_no == 4) {
				as_conn->submit_url = line;
				break;
			}
		}
		if(line_no < 4) {
			scmpc_log(DEBUG,"Truncated data from server returned. Found %d"
				" lines, expected 4 lines or more.",line_no);
		} else {
			scmpc_log(INFO,"Connected to Audioscrobbler.");
			as_conn->status = CONNECTED;
			as_conn->last_handshake = time(NULL);
			free(buffer);
		}
	} else if(strncmp(line,"FAILED",6) == 0) {
		scmpc_log(ERROR,"The Audioscrobbler handshake could not be "
			"completed: %s",&line[7]);
	} else if(strncmp(line,"BADAUTH",7) == 0) {
		scmpc_log(ERROR,"The user details you specified were not "
				"accepted by Audioscrobbler. Please correct "
				"them and restart this program.");
		as_conn->status = BADAUTH;
	} else if(strncmp(line,"BADTIME",7) == 0) {
		scmpc_log(ERROR,"Handshake failed because your system time is "
				"too far off. Please correct your clock.");
	} else {
		scmpc_log(DEBUG,"Could not parse Audioscrobbler handshake response.");
	}
}

int as_now_playing(void)
{
	return 0;
}

int as_submit(void)
{
	return 0;
}

void queue_save(void)
{
}
