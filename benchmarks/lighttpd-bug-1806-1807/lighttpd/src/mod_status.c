/*
 * make sure _GNU_SOURCE is defined.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

#include "server.h"
#include "connections.h"
#include "response.h"
#include "connections.h"
#include "log.h"
#include "status_counter.h"

#include "plugin.h"

#include "inet_ntop_cache.h"

typedef struct {
	buffer *config_url;
	buffer *status_url;
	buffer *statistics_url;

	int     sort;
} plugin_config;

typedef struct {
	PLUGIN_DATA;

	double traffic_out;
	double requests;

	double mod_5s_traffic_out[5];
	double mod_5s_requests[5];
	size_t mod_5s_ndx;

	double rel_traffic_out;
	double rel_requests;

	double abs_traffic_out;
	double abs_requests;

	double bytes_written;

	buffer *module_list;

	plugin_config **config_storage;

	plugin_config conf;
} plugin_data;

INIT_FUNC(mod_status_init) {
	plugin_data *p;
	size_t i;

	UNUSED(srv);

	p = calloc(1, sizeof(*p));

	p->traffic_out = p->requests = 0;
	p->rel_traffic_out = p->rel_requests = 0;
	p->abs_traffic_out = p->abs_requests = 0;
	p->bytes_written = 0;
	p->module_list = buffer_init();

	for (i = 0; i < 5; i++) {
		p->mod_5s_traffic_out[i] = p->mod_5s_requests[i] = 0;
	}

	return p;
}

FREE_FUNC(mod_status_free) {
	plugin_data *p = p_d;

	UNUSED(srv);

	if (!p) return HANDLER_GO_ON;

	buffer_free(p->module_list);

	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];

			buffer_free(s->status_url);
			buffer_free(s->statistics_url);
			buffer_free(s->config_url);

			free(s);
		}
		free(p->config_storage);
	}


	free(p);

	return HANDLER_GO_ON;
}

SETDEFAULTS_FUNC(mod_status_set_defaults) {
	plugin_data *p = p_d;
	size_t i;

	config_values_t cv[] = {
		{ "status.status-url",           NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
		{ "status.config-url",           NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
		{ "status.enable-sort",          NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION },
		{ "status.statistics-url",       NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
		{ NULL,                          NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};

	if (!p) return HANDLER_ERROR;

	p->config_storage = calloc(1, srv->config_context->used * sizeof(specific_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;

		s = calloc(1, sizeof(plugin_config));
		s->config_url    = buffer_init();
		s->status_url    = buffer_init();
		s->sort          = 1;
		s->statistics_url    = buffer_init();

		cv[0].destination = s->status_url;
		cv[1].destination = s->config_url;
		cv[2].destination = &(s->sort);
		cv[3].destination = s->statistics_url;

		p->config_storage[i] = s;

		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}
	}

	return HANDLER_GO_ON;
}



static int mod_status_row_append(buffer *b, const char *key, const char *value) {
	BUFFER_APPEND_STRING_CONST(b, "   <tr>\n");
	BUFFER_APPEND_STRING_CONST(b, "    <td><b>");
	buffer_append_string(b, key);
	BUFFER_APPEND_STRING_CONST(b, "</b></td>\n");
	BUFFER_APPEND_STRING_CONST(b, "    <td>");
	buffer_append_string(b, value);
	BUFFER_APPEND_STRING_CONST(b, "</td>\n");
	BUFFER_APPEND_STRING_CONST(b, "   </tr>\n");

	return 0;
}

static int mod_status_header_append(buffer *b, const char *key) {
	BUFFER_APPEND_STRING_CONST(b, "   <tr>\n");
	BUFFER_APPEND_STRING_CONST(b, "    <th colspan=\"2\">");
	buffer_append_string(b, key);
	BUFFER_APPEND_STRING_CONST(b, "</th>\n");
	BUFFER_APPEND_STRING_CONST(b, "   </tr>\n");

	return 0;
}

static int mod_status_header_append_sort(buffer *b, void *p_d, const char* key) {
	plugin_data *p = p_d;

	if (p->conf.sort) {
		BUFFER_APPEND_STRING_CONST(b, "<th class=\"status\"><a href=\"#\" class=\"sortheader\" onclick=\"resort(this);return false;\">");
		buffer_append_string(b, key);
		BUFFER_APPEND_STRING_CONST(b, "<span class=\"sortarrow\">:</span></a></th>\n");
	} else {
		BUFFER_APPEND_STRING_CONST(b, "<th class=\"status\">");
		buffer_append_string(b, key);
		BUFFER_APPEND_STRING_CONST(b, "</th>\n");
	}

	return 0;
}

static int mod_status_get_multiplier(double *avg, char *multiplier, int size) {
	*multiplier = ' ';

	if (*avg > size) { *avg /= size; *multiplier = 'k'; }
	if (*avg > size) { *avg /= size; *multiplier = 'M'; }
	if (*avg > size) { *avg /= size; *multiplier = 'G'; }
	if (*avg > size) { *avg /= size; *multiplier = 'T'; }
	if (*avg > size) { *avg /= size; *multiplier = 'P'; }
	if (*avg > size) { *avg /= size; *multiplier = 'E'; }
	if (*avg > size) { *avg /= size; *multiplier = 'Z'; }
	if (*avg > size) { *avg /= size; *multiplier = 'Y'; }

	return 0;
}

static handler_t mod_status_handle_server_status_html(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	buffer *b;
	size_t j;
	double avg;
	char multiplier = '\0';
	char buf[128];
	time_t ts;

	int days, hours, mins, seconds;

	b = chunkqueue_get_append_buffer(con->send);

	BUFFER_COPY_STRING_CONST(b,
				 "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
				 "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
				 "         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
				 "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
				 " <head>\n"
				 "  <title>Status</title>\n");

	BUFFER_APPEND_STRING_CONST(b,
				   "  <style type=\"text/css\">\n"
				   "    table.status { border: black solid thin; }\n"
				   "    td.int { background-color: #f0f0f0; text-align: right }\n"
				   "    td.string { background-color: #f0f0f0; text-align: left }\n"
				   "    th.status { background-color: black; color: white; font-weight: bold; }\n"
				   "    a.sortheader { background-color: black; color: white; font-weight: bold; text-decoration: none; display: block; }\n"
				   "    span.sortarrow { color: white; text-decoration: none; }\n"
				   "  </style>\n");

	if (p->conf.sort) {
		BUFFER_APPEND_STRING_CONST(b,
					   "<script type=\"text/javascript\">\n"
					   "// <!--\n"
					   "var sort_column;\n"
					   "var prev_span = null;\n");

		BUFFER_APPEND_STRING_CONST(b,
					   "function get_inner_text(el) {\n"
					   " if((typeof el == 'string')||(typeof el == 'undefined'))\n"
					   "  return el;\n"
					   " if(el.innerText)\n"
					   "  return el.innerText;\n"
					   " else {\n"
					   "  var str = \"\";\n"
					   "  var cs = el.childNodes;\n"
					   "  var l = cs.length;\n"
					   "  for (i=0;i<l;i++) {\n"
					   "   if (cs[i].nodeType==1) str += get_inner_text(cs[i]);\n"
					   "   else if (cs[i].nodeType==3) str += cs[i].nodeValue;\n"
					   "  }\n"
					   " }\n"
					   " return str;\n"
					   "}\n");

		BUFFER_APPEND_STRING_CONST(b,
					   "function sortfn(a,b) {\n"
					   " var at = get_inner_text(a.cells[sort_column]);\n"
					   " var bt = get_inner_text(b.cells[sort_column]);\n"
					   " if (a.cells[sort_column].className == 'int') {\n"
					   "  return parseInt(at)-parseInt(bt);\n"
					   " } else {\n"
					   "  aa = at.toLowerCase();\n"
					   "  bb = bt.toLowerCase();\n"
					   "  if (aa==bb) return 0;\n"
					   "  else if (aa<bb) return -1;\n"
					   "  else return 1;\n"
					   " }\n"
					   "}\n");

		BUFFER_APPEND_STRING_CONST(b,
					   "function resort(lnk) {\n"
					   " var span = lnk.childNodes[1];\n"
					   " var table = lnk.parentNode.parentNode.parentNode.parentNode;\n"
					   " var rows = new Array();\n"
					   " for (j=1;j<table.rows.length;j++)\n"
					   "  rows[j-1] = table.rows[j];\n"
					   " sort_column = lnk.parentNode.cellIndex;\n"
					   " rows.sort(sortfn);\n");

		BUFFER_APPEND_STRING_CONST(b,
					   " if (prev_span != null) prev_span.innerHTML = '';\n"
					   " if (span.getAttribute('sortdir')=='down') {\n"
					   "  span.innerHTML = '&uarr;';\n"
					   "  span.setAttribute('sortdir','up');\n"
					   "  rows.reverse();\n"
					   " } else {\n"
					   "  span.innerHTML = '&darr;';\n"
					   "  span.setAttribute('sortdir','down');\n"
					   " }\n"
					   " for (i=0;i<rows.length;i++)\n"
					   "  table.tBodies[0].appendChild(rows[i]);\n"
					   " prev_span = span;\n"
					   "}\n"
					   "// -->\n"
					   "</script>\n");
	}

	BUFFER_APPEND_STRING_CONST(b,
				 " </head>\n"
				 " <body>\n");



	/* connection listing */
	BUFFER_APPEND_STRING_CONST(b, "<h1>Server-Status</h1>");

	BUFFER_APPEND_STRING_CONST(b, "<table class=\"status\" id=\"status\" summary=\"Server Status\">");
	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Hostname</td><td class=\"string\"><span id=\"host_addr\">");
	buffer_append_string_buffer(b, con->uri.authority);
	BUFFER_APPEND_STRING_CONST(b, "</span> (<span id=\"host_name\">");
	buffer_append_string_buffer(b, con->server_name);
	BUFFER_APPEND_STRING_CONST(b, "</span>)</td></tr>\n");
	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Uptime</td><td class=\"string\" id=\"uptime\">");

	ts = srv->cur_ts - srv->startup_ts;

	days = ts / (60 * 60 * 24);
	ts %= (60 * 60 * 24);

	hours = ts / (60 * 60);
	ts %= (60 * 60);

	mins = ts / (60);
	ts %= (60);

	seconds = ts;

	if (days) {
		buffer_append_long(b, days);
		BUFFER_APPEND_STRING_CONST(b, " days ");
	}

	if (hours) {
		buffer_append_long(b, hours);
		BUFFER_APPEND_STRING_CONST(b, " hours ");
	}

	if (mins) {
		buffer_append_long(b, mins);
		BUFFER_APPEND_STRING_CONST(b, " min ");
	}

	buffer_append_long(b, seconds);
	BUFFER_APPEND_STRING_CONST(b, " s");

	BUFFER_APPEND_STRING_CONST(b, "</td></tr>\n");
	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Started at</td><td class=\"string\">");

	ts = srv->startup_ts;

	strftime(buf, sizeof(buf) - 1, "<span id=\"start_date\">%Y-%m-%d</span> <span id=\"start_time\">%H:%M:%S</span>", localtime(&ts));
	buffer_append_string(b, buf);
	BUFFER_APPEND_STRING_CONST(b, "</td></tr>\n");


	BUFFER_APPEND_STRING_CONST(b, "<tr><th colspan=\"2\">absolute (since start)</th></tr>\n");

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Requests</td><td class=\"string\" ><span id=\"requests\">");
	avg = p->abs_requests;

	mod_status_get_multiplier(&avg, &multiplier, 1000);

	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "</span> <span id=\"requests_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);
	BUFFER_APPEND_STRING_CONST(b, "</span>req</td></tr>\n");

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Traffic</td><td class=\"string\"><span id=\"traffic\">");
	avg = p->abs_traffic_out;

	mod_status_get_multiplier(&avg, &multiplier, 1024);

	sprintf(buf, "%.2f", avg);
	buffer_append_string(b, buf);
	BUFFER_APPEND_STRING_CONST(b, "</span>  <span id=\"traffic_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);
	BUFFER_APPEND_STRING_CONST(b, "</span>byte</td></tr>\n");



	BUFFER_APPEND_STRING_CONST(b, "<tr><th colspan=\"2\">average (since start)</th></tr>\n");

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Requests</td><td class=\"string\"><span id=\"requests_avg\">");
	avg = p->abs_requests / (srv->cur_ts - srv->startup_ts);

	mod_status_get_multiplier(&avg, &multiplier, 1000);

	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "</span> <span id=\"requests_avg_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);
	BUFFER_APPEND_STRING_CONST(b, "</span>req/s</td></tr>\n");

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Traffic</td><td class=\"string\"><span id=\"traffic_avg\">");
	avg = p->abs_traffic_out / (srv->cur_ts - srv->startup_ts);

	mod_status_get_multiplier(&avg, &multiplier, 1024);

	sprintf(buf, "%.2f", avg);
	buffer_append_string(b, buf);
	BUFFER_APPEND_STRING_CONST(b, "</span> <span id=\"traffic_avg_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);
	BUFFER_APPEND_STRING_CONST(b, "</span>byte/s</td></tr>\n");



	BUFFER_APPEND_STRING_CONST(b, "<tr><th colspan=\"2\">average (5s sliding average)</th></tr>\n");
	for (j = 0, avg = 0; j < 5; j++) {
		avg += p->mod_5s_requests[j];
	}

	avg /= 5;

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Requests</td><td class=\"string\"><span id=\"requests_sliding_avg\">");

	mod_status_get_multiplier(&avg, &multiplier, 1000);

	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "</span> <span id=\"requests_sliding_avg_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);

	BUFFER_APPEND_STRING_CONST(b, "</span>req/s</td></tr>\n");

	for (j = 0, avg = 0; j < 5; j++) {
		avg += p->mod_5s_traffic_out[j];
	}

	avg /= 5;

	BUFFER_APPEND_STRING_CONST(b, "<tr><td>Traffic</td><td class=\"string\"><span id=\"requests_sliding_traffic\">");

	mod_status_get_multiplier(&avg, &multiplier, 1024);

	sprintf(buf, "%.2f", avg);
	buffer_append_string(b, buf);
	BUFFER_APPEND_STRING_CONST(b, "</span> <span id=\"requests_sliding_traffic_mult\">");
	if (multiplier)	buffer_append_string_len(b, &multiplier, 1);
	BUFFER_APPEND_STRING_CONST(b, "</span>byte/s</td></tr>\n");

	BUFFER_APPEND_STRING_CONST(b, "</table>\n");


	BUFFER_APPEND_STRING_CONST(b, "<hr />\n<pre><b>legend</b>\n");
	BUFFER_APPEND_STRING_CONST(b, ". = connect, C = close, E = hard error\n");
	BUFFER_APPEND_STRING_CONST(b, "r = read, R = read-POST, W = write, h = handle-request\n");
	BUFFER_APPEND_STRING_CONST(b, "q = request-start,  Q = request-end\n");
	BUFFER_APPEND_STRING_CONST(b, "s = response-start, S = response-end\n");

	BUFFER_APPEND_STRING_CONST(b, "<strong><span id=\"connections\">");
	buffer_append_long(b, srv->conns->used);
	BUFFER_APPEND_STRING_CONST(b, "</span> connections</strong>\n");

	for (j = 0; j < srv->conns->used; j++) {
		connection *c = srv->conns->ptr[j];
		const char *state = connection_get_short_state(c->state);

		buffer_append_string_len(b, state, 1);

		if (((j + 1) % 50) == 0) {
			BUFFER_APPEND_STRING_CONST(b, "\n");
		}
	}

	BUFFER_APPEND_STRING_CONST(b, "\n</pre><hr />\n<h2>Connections</h2>\n");

	BUFFER_APPEND_STRING_CONST(b, "<table class=\"status\" summary=\"Current connections\" id=\"clients\">\n");
	BUFFER_APPEND_STRING_CONST(b, "<tr>");
	mod_status_header_append_sort(b, p_d, "Client IP");
	mod_status_header_append_sort(b, p_d, "Read");
	mod_status_header_append_sort(b, p_d, "Written");
	mod_status_header_append_sort(b, p_d, "State");
	mod_status_header_append_sort(b, p_d, "Time");
	mod_status_header_append_sort(b, p_d, "Host");
	mod_status_header_append_sort(b, p_d, "URI");
	mod_status_header_append_sort(b, p_d, "File");
	BUFFER_APPEND_STRING_CONST(b, "</tr>\n");

	for (j = 0; j < srv->conns->used; j++) {
		connection *c = srv->conns->ptr[j];

		BUFFER_APPEND_STRING_CONST(b, "<tr><td class=\"string ip\">");

		buffer_append_string(b, inet_ntop_cache_get_ip(srv, &(c->dst_addr)));

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"int bytes_read\">");

		if (c->request.content_length != -1) {
			buffer_append_long(b, c->recv->bytes_in);
			BUFFER_APPEND_STRING_CONST(b, "/");
			buffer_append_long(b, c->request.content_length);
		} else {
			BUFFER_APPEND_STRING_CONST(b, "0/0");
		}

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"int bytes_written\">");

		buffer_append_off_t(b, chunkqueue_written(c->send_raw));
		BUFFER_APPEND_STRING_CONST(b, "/");
		buffer_append_off_t(b, chunkqueue_length(c->send_raw));

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"string state\">");

		buffer_append_string(b, connection_get_state(c->state));

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"int time\">");

		buffer_append_long(b, srv->cur_ts - c->request_start);

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"string host\">");

		if (buffer_is_empty(c->server_name)) {
			buffer_append_string_buffer(b, c->uri.authority);
		}
		else {
			buffer_append_string_buffer(b, c->server_name);
		}

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"string uri\">");

		if (!buffer_is_empty(c->uri.path)) {
			buffer_append_string_encoded(b, CONST_BUF_LEN(c->uri.path), ENCODING_HTML);
		}

		BUFFER_APPEND_STRING_CONST(b, "</td><td class=\"string file\">");

		buffer_append_string_buffer(b, c->physical.path);

		BUFFER_APPEND_STRING_CONST(b, "</td></tr>\n");
	}


	BUFFER_APPEND_STRING_CONST(b,
		      "</table>\n");


	BUFFER_APPEND_STRING_CONST(b,
		      " </body>\n"
		      "</html>\n"
		      );

	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));
	con->send->bytes_in += b->used-1;

	return 0;
}


static handler_t mod_status_handle_server_status_text(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	buffer *b;
	double avg;
	time_t ts;
	size_t j;

	b = chunkqueue_get_append_buffer(con->send);

	/* output total number of requests */
	BUFFER_APPEND_STRING_CONST(b, "Total Accesses: ");
	avg = p->abs_requests;
	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* output total traffic out in kbytes */
	BUFFER_APPEND_STRING_CONST(b, "Total kBytes: ");
	avg = p->abs_traffic_out / 1024;
	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* output uptime */
	BUFFER_APPEND_STRING_CONST(b, "Uptime: ");
	ts = srv->cur_ts - srv->startup_ts;
	buffer_append_long(b, ts);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* output busy servers */
	BUFFER_APPEND_STRING_CONST(b, "BusyServers: ");
	buffer_append_long(b, srv->conns->used);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* output traffic */
	BUFFER_APPEND_STRING_CONST(b, "Traffic: ");
	avg = p->abs_traffic_out / (srv->cur_ts - srv->startup_ts);
	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* output traffic 5s */
	BUFFER_APPEND_STRING_CONST(b, "Traffic5s: ");
	for (j = 0, avg = 0; j < 5; j++) {
		avg += p->mod_5s_traffic_out[j];
	}
	avg /= 5;
	buffer_append_long(b, avg);
	BUFFER_APPEND_STRING_CONST(b, "\n");

	/* set text/plain output */

	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/plain"));
	con->send->bytes_in += b->used-1;

	return 0;
}

static handler_t mod_status_handle_server_statistics(server *srv, connection *con, void *p_d) {
	buffer *b;
	size_t i;
	array *st = status_counter_get_array();

	UNUSED(p_d);

	if (0 == st->used) {
		/* we have nothing to send */
		con->http_status = 204;
		con->send->is_closed = 1;

		return HANDLER_FINISHED;
	}

	b = chunkqueue_get_append_buffer(con->send);

	for (i = 0; i < st->used; i++) {
		size_t ndx = st->sorted[i];

		buffer_append_string_buffer(b, st->data[ndx]->key);
		buffer_append_string(b, ": ");
		buffer_append_long(b, ((data_integer *)(st->data[ndx]))->value);
		buffer_append_string(b, "\n");
	}

	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/plain"));

	con->http_status = 200;
	con->send->bytes_in += b->used-1;
	con->send->is_closed = 1;

	return HANDLER_FINISHED;
}


static handler_t mod_status_handle_server_status(server *srv, connection *con, void *p_d) {

	if (buffer_is_equal_string(con->uri.query, CONST_STR_LEN("auto"))) {
		mod_status_handle_server_status_text(srv, con, p_d);
	} else {
		mod_status_handle_server_status_html(srv, con, p_d);
	}

	con->http_status = 200;
	con->send->is_closed = 1;

	return HANDLER_FINISHED;
}


static handler_t mod_status_handle_server_config(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	buffer *b, *m = p->module_list;
	size_t i;

	struct ev_map { fdevent_handler_t et; const char *name; } event_handlers[] =
	{
		/* - poll is most reliable
		 * - select works everywhere
		 * - linux-* are experimental
		 */
#ifdef USE_POLL
		{ FDEVENT_HANDLER_POLL,           "poll" },
#endif
#ifdef USE_SELECT
		{ FDEVENT_HANDLER_SELECT,         "select" },
#endif
#ifdef USE_LINUX_EPOLL
		{ FDEVENT_HANDLER_LINUX_SYSEPOLL, "linux-sysepoll" },
#endif
#ifdef USE_LINUX_SIGIO
		{ FDEVENT_HANDLER_LINUX_RTSIG,    "linux-rtsig" },
#endif
#ifdef USE_SOLARIS_DEVPOLL
		{ FDEVENT_HANDLER_SOLARIS_DEVPOLL,"solaris-devpoll" },
#endif
#ifdef USE_FREEBSD_KQUEUE
		{ FDEVENT_HANDLER_FREEBSD_KQUEUE, "freebsd-kqueue" },
#endif
		{ FDEVENT_HANDLER_UNSET,          NULL }
	};

	b = chunkqueue_get_append_buffer(con->send);

	BUFFER_COPY_STRING_CONST(b,
			   "<?xml version=\"1.0\" encoding=\"iso-8859-1\"?>\n"
			   "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
			   "         \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
			   "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en\" lang=\"en\">\n"
			   " <head>\n"
			   "  <title>Status</title>\n"
			   " </head>\n"
			   " <body>\n"
			   "  <h1>" PACKAGE_NAME " " PACKAGE_VERSION "</h1>\n"
			   "  <table summary=\"status\" border=\"1\">\n");

	mod_status_header_append(b, "Server-Features");
#ifdef HAVE_PCRE_H
	mod_status_row_append(b, "RegEx Conditionals", "enabled");
#else
	mod_status_row_append(b, "RegEx Conditionals", "disabled - pcre missing");
#endif
	mod_status_header_append(b, "Network Engine");

	for (i = 0; event_handlers[i].name; i++) {
		if (event_handlers[i].et == srv->event_handler) {
			mod_status_row_append(b, "fd-Event-Handler", event_handlers[i].name);
			break;
		}
	}

	mod_status_header_append(b, "Config-File-Settings");

	for (i = 0; i < srv->plugins.used; i++) {
		plugin **ps = srv->plugins.ptr;

		plugin *pl = ps[i];

		if (i == 0) {
			buffer_copy_string_buffer(m, pl->name);
		} else {
			BUFFER_APPEND_STRING_CONST(m, "<br />");
			buffer_append_string_buffer(m, pl->name);
		}
	}

	mod_status_row_append(b, "Loaded Modules", m->ptr);

	BUFFER_APPEND_STRING_CONST(b, "  </table>\n");

	BUFFER_APPEND_STRING_CONST(b,
		      " </body>\n"
		      "</html>\n"
		      );

	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_STR_LEN("text/html"));

	con->http_status = 200;
	con->send->bytes_in += b->used-1;
	con->send->is_closed = 1;

	return HANDLER_FINISHED;
}

static int mod_status_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH_OPTION(status_url);
	PATCH_OPTION(config_url);
	PATCH_OPTION(sort);
	PATCH_OPTION(statistics_url);

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("status.status-url"))) {
				PATCH_OPTION(status_url);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("status.config-url"))) {
				PATCH_OPTION(config_url);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("status.enable-sort"))) {
				PATCH_OPTION(sort);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("status.statistics-url"))) {
				PATCH_OPTION(statistics_url);
			}
		}
	}

	return 0;
}

static handler_t mod_status_handler(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;

	mod_status_patch_connection(srv, con, p);

	if (!buffer_is_empty(p->conf.status_url) &&
	    buffer_is_equal(p->conf.status_url, con->uri.path)) {
		return mod_status_handle_server_status(srv, con, p_d);
	} else if (!buffer_is_empty(p->conf.config_url) &&
	    buffer_is_equal(p->conf.config_url, con->uri.path)) {
		return mod_status_handle_server_config(srv, con, p_d);
	} else if (!buffer_is_empty(p->conf.statistics_url) &&
	    buffer_is_equal(p->conf.statistics_url, con->uri.path)) {
		return mod_status_handle_server_statistics(srv, con, p_d);
	}

	return HANDLER_GO_ON;
}

TRIGGER_FUNC(mod_status_trigger) {
	plugin_data *p = p_d;
	size_t i;

	/* check all connections */
	for (i = 0; i < srv->conns->used; i++) {
		connection *c = srv->conns->ptr[i];

		p->bytes_written += c->bytes_written_cur_second;
	}

	/* a sliding average */
	p->mod_5s_traffic_out[p->mod_5s_ndx] = p->bytes_written;
	p->mod_5s_requests   [p->mod_5s_ndx] = p->requests;

	p->mod_5s_ndx = (p->mod_5s_ndx+1) % 5;

	p->abs_traffic_out += p->bytes_written;
	p->rel_traffic_out += p->bytes_written;

	p->bytes_written = 0;

	/* reset storage - second */
	p->traffic_out = 0;
	p->requests    = 0;

	return HANDLER_GO_ON;
}

REQUESTDONE_FUNC(mod_status_account) {
	plugin_data *p = p_d;

	UNUSED(srv);

	p->requests++;
	p->rel_requests++;
	p->abs_requests++;

	p->bytes_written += con->bytes_written_cur_second;

	return HANDLER_GO_ON;
}

LI_EXPORT int mod_status_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("status");

	p->init        = mod_status_init;
	p->cleanup     = mod_status_free;
	p->set_defaults= mod_status_set_defaults;

	p->handle_uri_clean    = mod_status_handler;
	p->handle_trigger      = mod_status_trigger;
	p->handle_response_done = mod_status_account;

	p->data        = NULL;

	return 0;
}