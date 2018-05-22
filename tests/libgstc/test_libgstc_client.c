/*
 * GStreamer Daemon - Gst Launch under steroids
 * Copyright (c) 2015-2018 Ridgerun, LLC (http://www.ridgerun.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <gst/check/gstcheck.h>

#include "libgstc.h"
#include "libgstc_socket.h"

/* Mock implementation of a socket */
typedef struct _GstcSocket
{
} GstcSocket;

static GstcSocket _socket;
static gboolean _fail_socket = FALSE;

GstcSocket *
gstc_socket_new (const char *address, const unsigned int port,
    const unsigned long wait_time, const int keep_connection_open)
{
  if (_fail_socket) {
    return NULL;
  } else {
    return &_socket;
  }
}

void
gstc_socket_free (GstcSocket * socket)
{
}

GstcStatus
gstc_socket_send (GstcSocket *socket, const gchar *request)
{
  return GSTC_OK;
}

/* Mock implementation of malloc */
gboolean _use_mock_malloc = FALSE;
void *
malloc (gsize size) {
  if (_use_mock_malloc) {
    /* Simulate out of memory */
    return NULL;
  } else {
    return calloc (1, size);
  }
}

void setup ()
{
  _use_mock_malloc = FALSE;
  _fail_socket = FALSE;
}

void teardown ()
{
}

GST_START_TEST (test_client_success)
{
  GstClient * client;
  GstcStatus ret;

  const gchar * address = "127.0.0.1";
  guint port = 12345;
  guint64 wait_time = 0;
  gint keep_connection_open = 1;

  ret = gstc_client_new (address, port, wait_time, keep_connection_open, &client);
  assert_equals_int(GSTC_OK, ret);
  fail_if (NULL == client);

  gstc_client_free (client);
}
GST_END_TEST;

GST_START_TEST (test_client_out_of_memory)
{
  GstClient * client;
  GstcStatus ret;

  const gchar * address = "127.0.0.1";
  guint port = 12345;
  guint64 wait_time = 0;
  gint keep_connection_open = 1;

  _use_mock_malloc = TRUE;

  ret = gstc_client_new (address, port, wait_time, keep_connection_open, &client);
  assert_equals_int (GSTC_OOM, ret);
  assert_equals_pointer(NULL, client)
}
GST_END_TEST;

GST_START_TEST (test_client_null_address)
{
  GstClient * client;
  GstcStatus ret;

  const gchar * address = NULL;
  guint port = 12345;
  guint64 wait_time = 0;
  gint keep_connection_open = 1;

  ret = gstc_client_new (address, port, wait_time, keep_connection_open, &client);
  assert_equals_int (GSTC_NULL_ARGUMENT, ret);
}
GST_END_TEST;

GST_START_TEST (test_client_null_placeholder)
{
  GstcStatus ret;

  const gchar * address = "127.0.0.1";
  guint port = 12345;
  guint64 wait_time = 0;
  gint keep_connection_open = 1;

  ret = gstc_client_new (address, port, wait_time, keep_connection_open, NULL);
  assert_equals_int (GSTC_NULL_ARGUMENT, ret);
}
GST_END_TEST;

GST_START_TEST (test_client_null_in_free)
{
  gstc_client_free (NULL);
}
GST_END_TEST;

GST_START_TEST (test_client_no_socket)
{
  GstClient * client;
  GstcStatus ret;

  const gchar * address = "127.0.0.1";
  guint port = 12345;
  guint64 wait_time = 0;
  gint keep_connection_open = 1;

  _fail_socket = TRUE;

  ret = gstc_client_new (address, port, wait_time, keep_connection_open, &client);
  fail_if (GSTC_OK, ret);
  assert_equals_pointer(NULL, client);
}
GST_END_TEST;

static Suite *
libgstc_client_suite (void)
{
  Suite *suite = suite_create ("libgstc_client");
  TCase *tc = tcase_create ("general");

  suite_add_tcase (suite, tc);

  tcase_add_checked_fixture (tc, setup, teardown);
  tcase_add_test (tc, test_client_success);
  tcase_add_test (tc, test_client_out_of_memory);
  tcase_add_test (tc, test_client_null_address);
  tcase_add_test (tc, test_client_null_placeholder);
  tcase_add_test (tc, test_client_null_in_free);
  tcase_add_test (tc, test_client_no_socket);

  return suite;
}

GST_CHECK_MAIN (libgstc_client);