#include <stdio.h>
#include <gio/gio.h>
#include <gio/gsocketclient.h>
#include <glib.h>
#include <locale.h>
#include <glib/gprintf.h>

#define DEFAULT_PORT 7777

#define UNKNOWN -1

#define RESPONSE_ERROR 0
#define RESPONSE_CONNECT_SUCCESSFULLY 1
#define RESPONSE_USER_CONNECTED 2
#define RESPONSE_USER_DISCONNECTED 3
#define RESPONSE_MESSAGE 4

#define ERROR_USER_ALREADY_EXISTS 1
#define ERROR_SERVER_UNAVAILABLE 0

static gboolean verbose = FALSE;

static GOptionEntry cmd_entries[] = {
    {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Be verbose", NULL},
    {NULL}
};

typedef struct _SimpCtx SimpCtx;

struct _SimpCtx {
  GSocketConnection *connection;
  GCancellable *cancellable;
  gchar *username;
};

static gpointer
output_handler(gpointer context) {
  g_debug("Start output handler thread");
  SimpCtx *ctx = context;
  GOutputStream *ostream;
  GCancellable *cancellable;
  GError *error = NULL;
  gchar *username;

  username = ctx->username;
  cancellable = ctx->cancellable;
  ostream = g_io_stream_get_output_stream(G_IO_STREAM(ctx->connection));

  guint8 connect_header[3];
  connect_header[0] = 1;  // version
  connect_header[1] = 0;  // connect
  connect_header[2] = strlen(username);

  GBytes *user = g_bytes_new(username, strlen(username));
  gsize user_size = g_bytes_get_size(user);
  GByteArray *connect = g_bytes_unref_to_array(user);
  connect = g_byte_array_prepend(connect, connect_header, 3);

  GBytes *bytes = g_byte_array_free_to_bytes(connect);
  gsize to_send = g_bytes_get_size(bytes);
  g_debug("To send %"
              G_GSIZE_FORMAT
              " bytes", to_send);

  while (TRUE) {
    gssize size;
    gchar buffer[4096];

    if (!to_send) {
      if (fgets(buffer, sizeof buffer, stdin) == NULL) {
        break;
      }
      if (!g_strcmp0(buffer, "\n")) {
        g_debug("Exit...");
        break;
      }

      guint8 message_header[4];
      gulong n = strlen(buffer);
      message_header[0] = n >> 24 & 0xFF;
      message_header[1] = n >> 16 & 0xFF;
      message_header[2] = n >> 8 & 0xFF;
      message_header[3] = n & 0xFF;

      GByteArray *message = g_bytes_unref_to_array(g_bytes_new(buffer, strlen(buffer)));
      message = g_byte_array_prepend(message, message_header, 4);
      user = g_bytes_new(username, strlen(username));
      message = g_byte_array_prepend(message, g_bytes_unref_to_data(user, &user_size), strlen(username));
      connect_header[1] = 2; // message
      message = g_byte_array_prepend(message, connect_header, 3);

      bytes = g_byte_array_free_to_bytes(message);
      to_send = g_bytes_get_size(bytes);
    }

    while (to_send > 0) {
      size = g_output_stream_write_bytes(ostream, bytes, cancellable, &error);
      if (size < 0) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
          g_debug("Socket send would block, handling");
          g_error_free(error);
          error = NULL;
          continue;
        } else {
          g_fprintf(stderr, "Error sending to socket: %s\n", error->message);
          g_cancellable_cancel(cancellable);
          return NULL;
        }
      }

      g_debug("Sent %"
                  G_GSSIZE_FORMAT
                  " bytes of data", size);
      if (size == 0) {
        g_fprintf(stderr, "Unexpected short write\n");
        g_cancellable_cancel(cancellable);
        return NULL;
      }

      to_send -= size;
    }
    to_send = 0;
  }
  g_debug("Stop output handler thread");
  g_cancellable_cancel(cancellable);
  return NULL;
}

static gssize
read_buffer(GInputStream *istream, GCancellable *cancellable, void *buff, gsize count) {
  gssize size = 0;
  GError *error = NULL;

  while (size != count) {
    size = g_input_stream_read(istream, buff, count - size, cancellable, &error);
    if (size < 0) {
      g_fprintf(stderr, "Error receiving from socket: %s\n", error->message);
      break;
    } else if (size == 0) {
      g_fprintf(stderr, "Socket closed\n");
      break;
    } else {
      g_debug("Received %"
                  G_GSSIZE_FORMAT
                  " bytes of data", size);
    }
  }
  return size;
}

static gint8
read_response_type(GInputStream *istream, GCancellable *cancellable) {
  guint8 header[2];
  gssize size = read_buffer(istream, cancellable, header, 2);
  if (size <= 0) {
    return UNKNOWN;
  }
  if (header[0] != 1) {
    g_fprintf(stderr, "Unsupported protocol version: %d\n", header[0]);
    return UNKNOWN;
  }
  g_debug("Received version: %d", header[0]);
  if (header[1] > 4 || header[1] < 0) {
    g_fprintf(stderr, "Unsupported response type: %d\n", header[1]);
    return UNKNOWN;
  }
  return header[1];
}

static gchar *
read_connected_successfully(GInputStream *istream, GCancellable *cancellable) {
  guint8 ul_size_buff[2];

  gssize size = read_buffer(istream, cancellable, ul_size_buff, 2);
  if (size <= 0)
    return NULL;

  guint16 ul_size = (ul_size_buff[0] & 0xFF) << 8 | (ul_size_buff[1] & 0xFF);

  gchar *ul_buff = g_new0(gchar, ul_size);

  size = read_buffer(istream, cancellable, ul_buff, ul_size);
  if (size <= 0)
    return NULL;

  return ul_buff;
}

static gchar *
read_user(GInputStream *istream, GCancellable *cancellable) {
  guint8 u_size_buff[1];

  gssize size = read_buffer(istream, cancellable, u_size_buff, 1);
  if (size <= 0)
    return NULL;

  gchar *u_buff = g_new0(gchar, u_size_buff[0]);

  size = read_buffer(istream, cancellable, u_buff, u_size_buff[0]);
  if (size <= 0)
    return NULL;

  return u_buff;
}

static gchar *
read_message(GInputStream *istream, GCancellable *cancellable) {
  guint8 m_size_buf[4];

  gssize size = read_buffer(istream, cancellable, m_size_buf, 4);
  if (size <= 0)
    return NULL;

  guint m_size = (m_size_buf[0] & 0xFF) << 24 |
      (m_size_buf[1] & 0xFF) << 16 |
      (m_size_buf[2] & 0xFF) << 8 |
      (m_size_buf[3] & 0xFF);

  gchar *m_buff = g_new0(gchar, m_size);

  size = read_buffer(istream, cancellable, m_buff, m_size);
  if (size <= 0)
    return NULL;

  return m_buff;
}

static gint8
read_error(GInputStream *istream, GCancellable *cancellable) {
  guint8 e_buff[1];

  gssize size = read_buffer(istream, cancellable, e_buff, 1);
  if (size <= 0)
    return UNKNOWN;

  return e_buff[0];
}

static gpointer
input_handler(gpointer context) {
  g_debug("Start input handler thread");
  SimpCtx *ctx = context;
  GCancellable *cancellable = ctx->cancellable;
  GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(ctx->connection));

  while (TRUE) {
    gint8 response_type = read_response_type(istream, cancellable);
    if (response_type == UNKNOWN) {
      break;
    }
    g_debug("Received type: %d", response_type);
    if (response_type == RESPONSE_CONNECT_SUCCESSFULLY) {
      gchar *users_list = read_connected_successfully(istream, cancellable);
      if (users_list == NULL)
        break;
      g_printf("SERVER: Online users: %s\n", users_list);
      g_free(users_list);
    } else if (response_type == RESPONSE_USER_CONNECTED || response_type == RESPONSE_USER_DISCONNECTED) {
      gchar *user = read_user(istream, cancellable);
      if (user == NULL)
        break;
      if (response_type == RESPONSE_USER_CONNECTED)
        g_printf("SERVER: Connected user: %s\n", user);
      else
        g_printf("SERVER: Disconnected user: %s\n", user);
      g_free(user);
    } else if (response_type == RESPONSE_MESSAGE) {
      gchar *user = read_user(istream, cancellable);
      if (user == NULL)
        break;
      gchar *message = read_message(istream, cancellable);
      if (message == NULL)
        break;
      g_printf("[%s] %s\n", user, message);
      g_free(user);
      g_free(message);
    } else if (response_type == RESPONSE_ERROR) {
      gint8 error_code = read_error(istream, cancellable);
      if (error_code == ERROR_USER_ALREADY_EXISTS) {
        g_fprintf(stderr, "SERVER: User %s is already connected\n", ctx->username);
      } else if (error_code == ERROR_SERVER_UNAVAILABLE) {
        g_fprintf(stderr, "SERVER: Server unavailable\n");
      } else {
        g_fprintf(stderr, "Unsupported error code: %d\n", error_code);
      }
      break;
    }
  }
  g_debug("Stop input handler thread");
  g_fprintf(stderr, "Print any key to exit...");
  g_cancellable_cancel(cancellable);
  return NULL;
}

int
main(int argc, char *argv[]) {

  GSocketClient *client;
  GSocketConnectable *connectable;
  GCancellable *cancellable;
  GError *error = NULL;
  GOptionContext *context;
  GSocketConnection *connection;

  context = g_option_context_new(" <hostname>[:port] <username> - Simp console client");
  g_option_context_add_main_entries(context, cmd_entries, NULL);
  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    g_fprintf(stderr, "%s: %s\n", argv[0], error->message);
    return 1;
  }

  if (argc != 3) {
    g_fprintf(stderr, "%s: %s\n", argv[0], "Need to specify hostname[:port] and username");
    return 1;
  }

  connectable = g_network_address_parse(argv[1], DEFAULT_PORT, &error);
  if (connectable == NULL) {
    g_fprintf(stderr, "%s: %s\n", argv[0], error->message);
    return 1;
  }

  client = g_socket_client_new();
  cancellable = g_cancellable_new();
  connection = g_socket_client_connect(client, connectable, cancellable, &error);
  if (connection == NULL) {
    g_fprintf(stderr, "%s: %s\n", argv[0], error->message);
    return 1;
  }

  if (verbose) {
    g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
  }

  SimpCtx *ctx = g_new0(SimpCtx, 1);
  ctx->connection = connection;
  ctx->cancellable = cancellable;
  ctx->username = argv[2];

  GThread *output_thread;
  GThread *input_thread;

  output_thread = g_thread_new("simp_output", output_handler, ctx);
  input_thread = g_thread_new("simp_input", input_handler, ctx);

  g_thread_join(input_thread);
  g_thread_join(output_thread);

  g_thread_unref(input_thread);
  g_thread_unref(output_thread);

  g_debug("Closing socket");

  if (!g_io_stream_close(G_IO_STREAM(connection), cancellable, &error)) {
    g_fprintf(stderr, "Error closing connection: %s\n", error->message);
    return 1;
  }
  g_object_unref(connection);
  g_free(ctx);

}

