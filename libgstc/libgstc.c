/*
 * GStreamer Daemon - gst-launch on steroids
 * C client library abstracting gstd interprocess communication
 *
 * Copyright (c) 2015-2018 RidgeRun, LLC (http://www.ridgerun.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libgstc.h"
#include "libgstc_socket.h"
#include "libgstc_json.h"
#include "libgstc_assert.h"
#include "libgstc_thread.h"

#define PRINTF_ERROR -1

static GstcStatus gstc_cmd_send (GstClient * client, const char *request);
static GstcStatus gstc_cmd_send_get_response (GstClient * client,
    const char *request, char **reponse, const int timeout);
static GstcStatus gstc_cmd_create (GstClient * client, const char *where,
    const char *what);
static GstcStatus gstc_cmd_read (GstClient * client, const char *what,
    char **response, const int timeout);
static GstcStatus gstc_cmd_update (GstClient * client, const char *what,
    const char *how);
static GstcStatus gstc_cmd_delete (GstClient * client, const char *where,
    const char *what);
static GstcStatus gstc_cmd_change_state (GstClient * client, const char *pipe,
    const char *state);
static GstcStatus gstc_response_get_code (const char *response, int *code);
static void *gstc_bus_thread (void *user_data);
static GstcStatus
gstc_pipeline_bus_wait_callback (GstClient * _client, const char *pipeline_name,
    const char *message_name, const long long timeout, char *message,
    void *user_data);

struct _GstClient
{
  GstcSocket *socket;
  int timeout;
};

typedef struct _GstcThreadData GstcThreadData;
struct _GstcThreadData
{
  GstClient *client;
  const char *pipeline_name;
  const char *message;
  GstcPipelineBusWaitCallback func;
  void *user_data;
  long long timeout;
};

typedef struct _GstcSyncBusData GstcSyncBusData;
struct _GstcSyncBusData
{
  GstcCond cond;
  GstcMutex mutex;
  int waiting;
  char *message;
  GstcStatus ret;
};

static GstcStatus
gstc_response_get_code (const char *response, int *code)
{
  const char *code_field_name = "code";

  gstc_assert_and_ret_val (NULL != response, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != code, GSTC_NULL_ARGUMENT);

  return gstc_json_get_int (response, code_field_name, code);
}

static GstcStatus
gstc_cmd_send_get_response (GstClient * client, const char *request,
    char **response, const int timeout)
{
  GstcStatus ret;
  int code = GSTC_NOT_FOUND;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != request, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != response, GSTC_NULL_ARGUMENT);

  ret = gstc_socket_send (client->socket, request, response, timeout);
  if (GSTC_OK != ret) {
    goto out;
  }

  ret = gstc_response_get_code (*response, &code);
  if (GSTC_OK != ret) {
    goto out;
  }

  /* Everything went okay, forward the server's code to the user */
  ret = code;

out:
  return ret;
}

static GstcStatus
gstc_cmd_send (GstClient * client, const char *request)
{
  GstcStatus ret;
  char *response = NULL;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != request, GSTC_NULL_ARGUMENT);

  ret =
      gstc_cmd_send_get_response (client, request, &response, client->timeout);

  free (response);

  return ret;
}

static GstcStatus
gstc_cmd_create (GstClient * client, const char *where, const char *what)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *template = "create %s %s";
  char *request;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != where, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != what, GSTC_NULL_ARGUMENT);

  /* Concatenate pieces into request */
  asprintf_ret = asprintf (&request, template, where, what);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_send (client, request);

  free (request);

  return ret;
}

static GstcStatus
gstc_cmd_read (GstClient * client, const char *what, char **response,
    const int timeout)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *template = "read %s";
  char *request;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != what, GSTC_NULL_ARGUMENT);

  /* Concatenate pieces into request */
  asprintf_ret = asprintf (&request, template, what);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_send_get_response (client, request, response, timeout);

  free (request);

  return ret;
}

static GstcStatus
gstc_cmd_update (GstClient * client, const char *what, const char *how)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *template = "update %s %s";
  char *request;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != what, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != how, GSTC_NULL_ARGUMENT);

  /* Concatenate pieces into request */
  asprintf_ret = asprintf (&request, template, what, how);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_send (client, request);

  free (request);

  return ret;
}

static GstcStatus
gstc_cmd_delete (GstClient * client, const char *where, const char *what)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *template = "delete %s %s";
  char *request;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != where, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != what, GSTC_NULL_ARGUMENT);

  /* Concatenate pieces into request */
  asprintf_ret = asprintf (&request, template, where, what);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_send (client, request);

  free (request);

  return ret;
}

GstcStatus
gstc_client_new (const char *address, const unsigned int port,
    const int wait_time, const int keep_connection_open, GstClient ** out)
{
  GstClient *client;
  GstcStatus ret = GSTC_OK;

  gstc_assert_and_ret_val (NULL != address, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != out, GSTC_NULL_ARGUMENT);

  *out = NULL;

  client = (GstClient *) malloc (sizeof (GstClient));
  if (NULL == client) {
    return GSTC_OOM;
  }

  client->timeout = wait_time;

  ret =
      gstc_socket_new (address, port, keep_connection_open, &(client->socket));
  if (GSTC_OK != ret) {
    free (client);
    return ret;
  }

  *out = client;

  return ret;
}

GstcStatus
gstc_pipeline_create (GstClient * client, const char *pipeline_name,
    const char *pipeline_desc)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *resource = "/pipelines";
  const char *template = "%s %s";
  char *create_args;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_desc, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&create_args, template, pipeline_name, pipeline_desc);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_create (client, resource, create_args);

  free (create_args);

  return ret;
}

GstcStatus
gstc_pipeline_delete (GstClient * client, const char *pipeline_name)
{
  GstcStatus ret;
  const char *resource = "/pipelines";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  ret = gstc_cmd_delete (client, resource, pipeline_name);

  return ret;
}

static GstcStatus
gstc_cmd_change_state (GstClient * client, const char *pipe, const char *state)
{
  GstcStatus ret;
  int asprintf_ret;
  const char *template = "/pipelines/%s/state";
  char *resource;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipe, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != state, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&resource, template, pipe);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_update (client, resource, state);

  free (resource);

  return ret;
}

GstcStatus
gstc_pipeline_play (GstClient * client, const char *pipeline_name)
{
  const char *state = "playing";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  return gstc_cmd_change_state (client, pipeline_name, state);
}

GstcStatus
gstc_pipeline_pause (GstClient * client, const char *pipeline_name)
{
  const char *state = "paused";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  return gstc_cmd_change_state (client, pipeline_name, state);
}

GstcStatus
gstc_pipeline_stop (GstClient * client, const char *pipeline_name)
{
  const char *state = "null";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  return gstc_cmd_change_state (client, pipeline_name, state);
}

void
gstc_client_free (GstClient * client)
{
  gstc_assert_and_ret (NULL != client);

  gstc_socket_free (client->socket);
  free (client);
}

GstcStatus
gstc_client_ping (GstClient * client)
{
  const char *request = "read /";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);

  return gstc_cmd_send (client, request);
}

GstcStatus
gstc_client_debug (GstClient * client, const char *threshold, const int colors)
{
  GstcStatus ret;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != threshold, GSTC_NULL_ARGUMENT);

  /* Enable debug of the pipeline */
  ret = gstc_cmd_update (client, "/debug/enable", "true");
  if (ret != GSTC_OK) {
    return ret;
  }

  /* Set the level of debug */
  ret = gstc_cmd_update (client, "/debug/threshold", threshold);
  if (ret != GSTC_OK) {
    return ret;
  }

  /* Enable the color in debug */
  const char * colored = colors == 0 ? "false" : "true";
  ret = gstc_cmd_update (client, "/debug/color", colored);
  
  if (ret != GSTC_OK) {
    return ret;
  }

  return ret;
}

GstcStatus
gstc_element_get (GstClient * client, const char *pname,
    const char *element, const char *property, const char *format, ...)
{
  GstcStatus ret;
  int asprintf_ret;
  va_list ap;
  const char *what_fmt = "/pipelines/%s/elements/%s/properties/%s";
  char *what;
  char *response;
  char *out;

  gstc_assert_and_ret_val (client != NULL, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (pname != NULL, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (element != NULL, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (property != NULL, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (format != NULL, GSTC_NULL_ARGUMENT);

  va_start (ap, format);

  asprintf_ret = asprintf (&what, what_fmt, pname, element, property);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_read (client, what, &response, client->timeout);
  if (ret != GSTC_OK) {
    goto unref;
  }

  ret = gstc_json_child_string (response, "response", "value", &out);
  if (ret != GSTC_OK) {
    goto unref_response;
  }

  vsscanf (out, format, ap);

  free (out);

unref_response:
  free (response);

unref:
  free (what);
  va_end (ap);
  return GSTC_OK;
}

GstcStatus
gstc_element_set (GstClient * client, const char *pname,
    const char *element, const char *parameter, const char *format, ...)
{
  va_list ap;
  int asprintf_ret;
  const char *what_fmt = "/pipelines/%s/elements/%s/properties/%s";
  char *what;
  char *how;

  va_start (ap, format);

  asprintf_ret = asprintf (&what, what_fmt, pname, element, parameter);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }
  asprintf_ret = vasprintf (&how, format, ap);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  gstc_cmd_update (client, what, how);

  va_end (ap);

  free (what);
  free (how);

  return GSTC_OK;
}

GstcStatus
gstc_pipeline_flush_start (GstClient * client, const char *pipeline_name)
{
  GstcStatus ret;
  int asprintf_ret;
  char *where;
  const char *what = "flush_start";
  const char *where_fmt = "/pipelines/%s/event";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&where, where_fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_create (client, where, what);

  free (where);

  return ret;
}

GstcStatus
gstc_pipeline_flush_stop (GstClient * client, const char *pipeline_name,
    const int reset)
{
  GstcStatus ret;
  int asprintf_ret;
  char *where;
  char *what;
  const char *what_fmt = "flush_stop %s";
  const char *where_fmt = "/pipelines/%s/event";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&where, where_fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  if (reset != 0) {
    asprintf_ret = asprintf (&what, what_fmt, "true");
  } else {
    asprintf_ret = asprintf (&what, what_fmt, "false");
  }
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_create (client, where, what);

  free (where);
  free (what);

  return ret;
}

GstcStatus
gstc_element_properties_list (GstClient * client,
    const char *pipeline_name, char *element, char **properties[],
    int *list_lenght)
{
  GstcStatus ret;
  int asprintf_ret;
  char *response;
  char *what;
  const char *what_fmt = "/pipelines/%s/elements/%s/properties";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != element, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != properties, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != list_lenght, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&what, what_fmt, pipeline_name, element);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_read (client, what, &response, client->timeout);
  if (GSTC_OK != ret) {
    goto out;
  }

  ret = gstc_json_get_child_char_array (response, "response", "nodes",
      "name", properties, list_lenght);

  free (response);

out:
  free (what);
  return ret;
}

GstcStatus
gstc_pipeline_inject_eos (GstClient * client, const char *pipeline_name)
{
  GstcStatus ret;
  int asprintf_ret;
  char *where;
  const char *what = "eos";
  const char *where_fmt = "/pipelines/%s/event";

  asprintf_ret = asprintf (&where, where_fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_create (client, where, what);

  free (where);

  return ret;
}

GstcStatus
gstc_pipeline_seek(GstClient *client, const char *pipeline_name,
    double rate, int format, int flags, int start_type, long long start,
    int stop_type, long long stop)
{
  GstcStatus ret;
  int asprintf_ret;
  char *where;
  char *what;
  const char *where_fmt = "/pipelines/%s/event";
  const char *what_fmt = "seek %f %d %d %d %lld %d %lld";
  
  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&where, where_fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }
  asprintf_ret = asprintf (&what, what_fmt, rate, format, flags, start_type, start, stop_type, stop);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }
  
  ret = gstc_cmd_create (client, where, what);

  free (where);
  free (what);

  return ret;
}

GstcStatus
gstc_pipeline_list_elements (GstClient * client,
    const char *pipeline_name, char **elements[], int *list_lenght)
{
  GstcStatus ret;
  int asprintf_ret;
  char *response;
  char *what;
  const char *what_fmt = "/pipelines/%s/elements/";

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipeline_name, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != elements, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != list_lenght, GSTC_NULL_ARGUMENT);

  asprintf_ret = asprintf (&what, what_fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  ret = gstc_cmd_read (client, what, &response, client->timeout);
  if (GSTC_OK != ret) {
    goto out;
  }

  ret =
      gstc_json_get_child_char_array (response, "response", "nodes", "name",
      elements, list_lenght);

  free (response);

out:
  free (what);

  return ret;
}

static void *
gstc_bus_thread (void *user_data)
{
  GstcThreadData *data = (GstcThreadData *) user_data;
  int asprintf_ret;
  char *where;
  char *response;
  const char *fmt = "/pipelines/%s/bus/message";
  const char *pipeline_name = data->pipeline_name;
  const char *message_name = data->message;
  long long timeout = data->timeout;
  GstClient *client = data->client;

  asprintf_ret = asprintf (&where, fmt, pipeline_name);
  if(asprintf_ret == PRINTF_ERROR) {
    return NULL;
  }

  /* -1 is used in this function so that the socket has an unlimited timeout */ 
  gstc_cmd_read (client, where, &response, -1);
  data->func (client, pipeline_name, message_name, timeout, response,
      data->user_data);

  free (where);
  free (response);
  free (data);

  return NULL;
}

GstcStatus
gstc_pipeline_bus_wait_async (GstClient * client,
    const char *pipeline_name, const char *message_name,
    const long long timeout, GstcPipelineBusWaitCallback callback,
    void *user_data)
{
  int asprintf_ret;
  GstcThread thread;
  GstcThreadData *data;
  char *where_timeout;
  char *where_types;
  char *how_timeout;
  const char *what_timeout = "timeout";
  const char *what_types = "types";
  const char *where_fmt = "/pipelines/%s/bus/%s";
  const char *how_timeout_fmt = "%lli";

  asprintf_ret = asprintf (&where_timeout, where_fmt, pipeline_name, what_timeout);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  asprintf_ret = asprintf (&how_timeout, how_timeout_fmt, timeout);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  asprintf_ret = asprintf (&where_types, where_fmt, pipeline_name, what_types);
  if(asprintf_ret == PRINTF_ERROR) {
    return GSTC_OOM;
  }

  gstc_cmd_update (client, where_types, message_name);
  gstc_cmd_update (client, where_timeout, how_timeout);

  data = malloc (sizeof (GstcThreadData));
  data->client = client;
  data->pipeline_name = pipeline_name;
  data->message = message_name;
  data->func = callback;
  data->user_data = user_data;
  data->timeout = timeout;
  gstc_thread_new (&thread, gstc_bus_thread, data);

  free (where_timeout);
  free (where_types);
  free (how_timeout);

  return GSTC_OK;
}

/*
 * The Wunused-parameter warning is ignored for this function since it should
 * carry all of this information for the callback even when the parameters
 * aren't directly used
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
static GstcStatus
gstc_pipeline_bus_wait_callback (GstClient * _client, const char *pipeline_name,
    const char *message_name, const long long timeout, char *message,
    void *user_data)
{
  GstcSyncBusData *data = (GstcSyncBusData *) user_data;
  GstcStatus ret = GSTC_OK;
  const int msglen = strlen (message) + 1;
  const char *response_tag = "response";
  int is_null;

  gstc_mutex_lock (&(data->mutex));
  data->waiting = 0;
  data->message = (char *) malloc (msglen);
  memcpy (data->message, message, msglen);

  /* If a valid string was received, a valid bus message was received.
     Otherwise, a timeout occurred */
  ret = gstc_json_is_null (message, response_tag, &is_null);
  if (GSTC_OK == ret) {
    data->ret = is_null ? GSTC_BUS_TIMEOUT : ret;
  } else {
    data->ret = ret;
  }

  gstc_cond_signal (&(data->cond));
  gstc_mutex_unlock (&(data->mutex));

  return GSTC_OK;
}
#pragma GCC diagnostic pop

GstcStatus
gstc_pipeline_bus_wait (GstClient * client,
    const char *pipeline_name, const char *message_name,
    const long long timeout, char **message)
{
  GstcSyncBusData data;

  gstc_cond_init (&(data.cond));
  gstc_mutex_init (&(data.mutex));
  data.waiting = 1;

  gstc_pipeline_bus_wait_async (client, pipeline_name, message_name,
      timeout, gstc_pipeline_bus_wait_callback, &data);

  gstc_mutex_lock (&(data.mutex));
  while (1 == data.waiting) {
    gstc_cond_wait (&(data.cond), &(data.mutex));
  }
  gstc_mutex_unlock (&(data.mutex));

  /* Output the message back to the user */
  *message = data.message;

  return data.ret;
}

GstcStatus
gstc_pipeline_list (GstClient * client, char **pipelines[], int *list_lenght)
{
  GstcStatus ret;
  char *response;

  gstc_assert_and_ret_val (NULL != client, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != list_lenght, GSTC_NULL_ARGUMENT);
  gstc_assert_and_ret_val (NULL != pipelines, GSTC_NULL_ARGUMENT);

  ret = gstc_cmd_read (client, "/pipelines", &response, client->timeout);
  if (GSTC_OK != ret) {
    goto out;
  }

  ret = gstc_json_get_child_char_array (response, "response", "nodes",
      "name", pipelines, list_lenght);

out:
  return ret;
}
