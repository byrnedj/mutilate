Protocol.cc                                                                                         0000644 0116471 0021131 00000020561 12772015773 013533  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             #include <netinet/tcp.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

#include "config.h"

#include "Protocol.h"
#include "Connection.h"
#include "distributions.h"
#include "Generator.h"
#include "mutilate.h"
#include "binary_protocol.h"
#include "util.h"

#define unlikely(x) __builtin_expect((x),0)

/**
 *
 * First we build a RESP Array:
 *  1. * character as the first byte 
 *  2.  the number of elements in the array as a decimal number
 *  3.  CRLF
 *  4. The actual RESP element we are putting into the array
 *
 * All Redis commands are sent as arrays of bulk strings. 
 * For example, the command “SET mykey ‘my value’” would be written and sent as:
 * *3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$8\r\nmy value\r\n
 *
 * Then package command as a RESP Bulk String to the server
 *
 * Bulk String is the defined by the following:
 *     1."$" byte followed by the number of bytes composing the 
 *        string (a prefixed length), terminated by CRLF.
 *     2. The actual string data.
 *     3. A final CRLF.
 *
 */
int ProtocolRESP::set_request(const char* key, const char* value, int value_len) {
  
  int l;
  l = evbuffer_add_printf(bufferevent_get_output(bev),
                          "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n", 
                          strlen(key),key,strlen(value),value);
  //if (read_state == IDLE) read_state = WAITING_FOR_END;
  return l;

  //size_t req_size = strlen("*3$$$") + 7*strlen("\r\n") + strlen(key) + strlen(value);

  //char *req = (char*)malloc(req_size*(sizeof(char)));
  //snprintf(req,req_size,"*%d\r\n$%dSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
  //              3,3,strlen(key),key,strlen(value),value);
  //
  //bufferevent_write(bev, req, req_size);
  //if (read_state == IDLE) read_state = WAITING_FOR_END;
  //return req_size;
}

/**
 * Send a RESP get request.
 */
int ProtocolRESP::get_request(const char* key) {
  int l;
  l = evbuffer_add_printf(bufferevent_get_output(bev),
                          "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n",strlen(key),key);
  //if (read_state == IDLE) read_state = WAITING_FOR_END;
  return l;
}

/**
 * Handle a RESP response.
 *
 * In RESP, the type of data depends on the first byte:
 * 
 * Simple Strings the first byte of the reply is "+"
 * Errors the first byte of the reply is "-"
 * Integers the first byte of the reply is ":"
 * Bulk Strings the first byte of the reply is "$"
 * Arrays the first byte of the reply is "*"
 *
 * Right now we are only implementing GET response
 * so the RESP type will be bulk string.
 *
 *
 */
bool ProtocolRESP::handle_response(evbuffer *input, bool &done) {
  char *resp = NULL;
  char *resp_bytes = NULL;
  size_t n_read_out; //first read number of bytes
  size_t m_read_out; //second read number of bytes

  //A bulk string resp: "$6\r\nfoobar\r\n"
  //1. Consume the first "$##\r\n", evbuffer_readln returns
  //   "$##", where ## is the number of bytes in the bulk string
  resp_bytes = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF_STRICT);
  resp_bytes++; //consume the "$"

  //2. Consume the next "foobar"
  resp = evbuffer_readln(input,&m_read_out,EVBUFFER_EOL_CRLF_STRICT);
  
  //make sure we got a results
  if (resp == NULL || resp_bytes == NULL) return false;

  //keep track of recieved stats
  conn->stats.rx_bytes += n_read_out + m_read_out;

  //if nil we have a miss, keep waiting? (check if this is correct action)
  if (strncmp(resp, "nil", 3) {
    conn->stats.get_misses++;
    done = true;
  } else {
    //we have a proper respsone
    done = true;
  }
  evbuffer_drain(input,n_read_out + m_read_out + 4) //4 = 2*(CRLF bytes) 
  free(resp);
  free(resp_bytes);
  return true;
}

/**
 * Send an ascii get request.
 */
int ProtocolAscii::get_request(const char* key) {
  int l;
  l = evbuffer_add_printf(
    bufferevent_get_output(bev), "get %s\r\n", key);
  if (read_state == IDLE) read_state = WAITING_FOR_GET;
  return l;
}

/**
 * Send an ascii set request.
 */
int ProtocolAscii::set_request(const char* key, const char* value, int len) {
  int l;
  l = evbuffer_add_printf(bufferevent_get_output(bev),
                          "set %s 0 0 %d\r\n", key, len);
  bufferevent_write(bev, value, len);
  bufferevent_write(bev, "\r\n", 2);
  l += len + 2;
  if (read_state == IDLE) read_state = WAITING_FOR_END;
  return l;
}

/**
 * Handle an ascii response.
 */
bool ProtocolAscii::handle_response(evbuffer *input, bool &done) {
  char *buf = NULL;
  int len;
  size_t n_read_out;

  switch (read_state) {

  case WAITING_FOR_GET:
  case WAITING_FOR_END:
    buf = evbuffer_readln(input, &n_read_out, EVBUFFER_EOL_CRLF);
    if (buf == NULL) return false;

    conn->stats.rx_bytes += n_read_out;

    if (!strncmp(buf, "END", 3)) {
      if (read_state == WAITING_FOR_GET) conn->stats.get_misses++;
      read_state = WAITING_FOR_GET;
      done = true;
    } else if (!strncmp(buf, "VALUE", 5)) {
      sscanf(buf, "VALUE %*s %*d %d", &len);

      // FIXME: check key name to see if it corresponds to the op at
      // the head of the op queue?  This will be necessary to
      // support "gets" where there may be misses.

      data_length = len;
      read_state = WAITING_FOR_GET_DATA;
      done = false;
    } else {
      // must be a value line..
      done = false;
    }
    free(buf);
    return true;

  case WAITING_FOR_GET_DATA:
    len = evbuffer_get_length(input);
    if (len >= data_length + 2) {
      evbuffer_drain(input, data_length + 2);
      read_state = WAITING_FOR_END;
      conn->stats.rx_bytes += data_length + 2;
      done = false;
      return true;
    }
    return false;

  default: printf("state: %d\n", read_state); DIE("Unimplemented!");
  }

  DIE("Shouldn't ever reach here...");
}

/**
 * Perform SASL authentication if requested (write).
 */
bool ProtocolBinary::setup_connection_w() {
  if (!opts.sasl) return true;

  string user = string(opts.username);
  string pass = string(opts.password);

  binary_header_t header = {0x80, CMD_SASL, 0, 0, 0, {0}, 0, 0, 0};
  header.key_len = htons(5);
  header.body_len = htonl(6 + user.length() + 1 + pass.length());

  bufferevent_write(bev, &header, 24);
  bufferevent_write(bev, "PLAIN\0", 6);
  bufferevent_write(bev, user.c_str(), user.length() + 1);
  bufferevent_write(bev, pass.c_str(), pass.length());

  return false;
}

/**
 * Perform SASL authentication if requested (read).
 */
bool ProtocolBinary::setup_connection_r(evbuffer* input) {
  if (!opts.sasl) return true;

  bool b;
  return handle_response(input, b);
}

/**
 * Send a binary get request.
 */
int ProtocolBinary::get_request(const char* key) {
  uint16_t keylen = strlen(key);

  // each line is 4-bytes
  binary_header_t h = { 0x80, CMD_GET, htons(keylen),
                        0x00, 0x00, {htons(0)},
                        htonl(keylen) };

  bufferevent_write(bev, &h, 24); // size does not include extras
  bufferevent_write(bev, key, keylen);
  return 24 + keylen;
}



/**
 * Send a binary set request.
 */
int ProtocolBinary::set_request(const char* key, const char* value, int len) {
  uint16_t keylen = strlen(key);

  // each line is 4-bytes
  binary_header_t h = { 0x80, CMD_SET, htons(keylen),
                        0x08, 0x00, {htons(0)},
                        htonl(keylen + 8 + len) };

  bufferevent_write(bev, &h, 32); // With extras
  bufferevent_write(bev, key, keylen);
  bufferevent_write(bev, value, len);
  return 24 + ntohl(h.body_len);
}

/**
 * Tries to consume a binary response (in its entirety) from an evbuffer.
 *
 * @param input evBuffer to read response from
 * @return  true if consumed, false if not enough data in buffer.
 */
bool ProtocolBinary::handle_response(evbuffer *input, bool &done) {
  // Read the first 24 bytes as a header
  int length = evbuffer_get_length(input);
  if (length < 24) return false;
  binary_header_t* h =
          reinterpret_cast<binary_header_t*>(evbuffer_pullup(input, 24));
  assert(h);

  // Not whole response
  int targetLen = 24 + ntohl(h->body_len);
  if (length < targetLen) return false;

  // If something other than success, count it as a miss
  if (h->opcode == CMD_GET && h->status) {
      conn->stats.get_misses++;
  }

  if (unlikely(h->opcode == CMD_SASL)) {
    if (h->status == RESP_OK) {
      V("SASL authentication succeeded");
    } else {
      DIE("SASL authentication failed");
    }
  }

  evbuffer_drain(input, targetLen);
  conn->stats.rx_bytes += targetLen;
  done = true;
  return true;
}

                                                                                                                                               Connection.cc                                                                                       0000644 0116471 0021131 00000026772 12772016203 014030  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             #include <netinet/tcp.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

#include "config.h"

#include "Connection.h"
#include "distributions.h"
#include "Generator.h"
#include "mutilate.h"
#include "binary_protocol.h"
#include "util.h"

/**
 * Create a new connection to a server endpoint.
 */
Connection::Connection(struct event_base* _base, struct evdns_base* _evdns,
                       string _hostname, string _port, options_t _options,
                       bool sampling) :
  start_time(0), stats(sampling), options(_options),
  hostname(_hostname), port(_port), base(_base), evdns(_evdns)
{
  valuesize = createGenerator(options.valuesize);
  keysize = createGenerator(options.keysize);
  keygen = new KeyGenerator(keysize, options.records);

  if (options.lambda <= 0) {
    iagen = createGenerator("0");
  } else {
    D("iagen = createGenerator(%s)", options.ia);
    iagen = createGenerator(options.ia);
    iagen->set_lambda(options.lambda);
  }

  read_state  = INIT_READ;
  write_state = INIT_WRITE;

  last_tx = last_rx = 0.0;

  bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(bev, bev_read_cb, bev_write_cb, bev_event_cb, this);
  bufferevent_enable(bev, EV_READ | EV_WRITE);

  if (options.binary) {
    prot = new ProtocolBinary(options, this, bev);
  } else if (options.redis) {
    prot = new ProtocolRESP(options, this, bev);
  } else {
    prot = new ProtocolAscii(options, this, bev);
  }

  if (bufferevent_socket_connect_hostname(bev, evdns, AF_UNSPEC,
                                          hostname.c_str(),
                                          atoi(port.c_str()))) {
    DIE("bufferevent_socket_connect_hostname()");
  }

  timer = evtimer_new(base, timer_cb, this);
}

/**
 * Destroy a connection, performing cleanup.
 */
Connection::~Connection() {
  event_free(timer);
  timer = NULL;
  // FIXME:  W("Drain op_q?");
  bufferevent_free(bev);

  delete iagen;
  delete keygen;
  delete keysize;
  delete valuesize;
}

/**
 * Reset the connection back to an initial, fresh state.
 */
void Connection::reset() {
  // FIXME: Actually check the connection, drain all bufferevents, drain op_q.
  assert(op_queue.size() == 0);
  evtimer_del(timer);
  read_state = IDLE;
  write_state = INIT_WRITE;
  stats = ConnectionStats(stats.sampling);
}

/**
 * Set our event processing priority.
 */
void Connection::set_priority(int pri) {
  if (bufferevent_priority_set(bev, pri)) {
    DIE("bufferevent_set_priority(bev, %d) failed", pri);
  }
}

/**
 * Load any required test data onto the server.
 */
void Connection::start_loading() {
  read_state = LOADING;
  loader_issued = loader_completed = 0;

  for (int i = 0; i < LOADER_CHUNK; i++) {
    if (loader_issued >= options.records) break;
    char key[256];
    int index = lrand48() % (1024 * 1024);
    string keystr = keygen->generate(loader_issued);
    strcpy(key, keystr.c_str());
    issue_set(key, &random_char[index], valuesize->generate());
    loader_issued++;
  }
}

/**
 * Issue either a get or set request to the server according to our probability distribution.
 */
void Connection::issue_something(double now) {
  char key[256];
  // FIXME: generate key distribution here!
  string keystr = keygen->generate(lrand48() % options.records);
  strcpy(key, keystr.c_str());

  if (drand48() < options.update) {
    int index = lrand48() % (1024 * 1024);
    issue_set(key, &random_char[index], valuesize->generate(), now);
  } else {
    issue_get(key, now);
  }
}

/**
 * Issue a get request to the server.
 */
void Connection::issue_get(const char* key, double now) {
  Operation op;
  int l;

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) {
#if USE_CACHED_TIME
    struct timeval now_tv;
    event_base_gettimeofday_cached(base, &now_tv);
    op.start_time = tv_to_double(&now_tv);
#else
    op.start_time = get_time();
#endif
  } else {
    op.start_time = now;
  }
#endif

  op.key = string(key);
  op.type = Operation::GET;
  op_queue.push(op);

  if (read_state == IDLE) read_state = WAITING_FOR_GET;
  l = prot->get_request(key);
  if (read_state != LOADING) stats.tx_bytes += l;
}

/**
 * Issue a set request to the server.
 */
void Connection::issue_set(const char* key, const char* value, int length,
                           double now) {
  Operation op;
  int l;

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) op.start_time = get_time();
  else op.start_time = now;
#endif

  op.type = Operation::SET;
  op_queue.push(op);

  if (read_state == IDLE) read_state = WAITING_FOR_SET;
  l = prot->set_request(key, value, length);
  if (read_state != LOADING) stats.tx_bytes += l;
}

/**
 * Return the oldest live operation in progress.
 */
void Connection::pop_op() {
  assert(op_queue.size() > 0);

  op_queue.pop();

  if (read_state == LOADING) return;
  read_state = IDLE;

  // Advance the read state machine.
  if (op_queue.size() > 0) {
    Operation& op = op_queue.front();
    switch (op.type) {
    case Operation::GET: read_state = WAITING_FOR_GET; break;
    case Operation::SET: read_state = WAITING_FOR_SET; break;
    default: DIE("Not implemented.");
    }
  }
}

/**
 * Finish up (record stats) an operation that just returned from the
 * server.
 */
void Connection::finish_op(Operation *op) {
  double now;
#if USE_CACHED_TIME
  struct timeval now_tv;
  event_base_gettimeofday_cached(base, &now_tv);
  now = tv_to_double(&now_tv);
#else
  now = get_time();
#endif
#if HAVE_CLOCK_GETTIME
  op->end_time = get_time_accurate();
#else
  op->end_time = now;
#endif

  switch (op->type) {
  case Operation::GET: stats.log_get(*op); break;
  case Operation::SET: stats.log_set(*op); break;
  default: DIE("Not implemented.");
  }

  last_rx = now;
  pop_op();
  drive_write_machine();
}

/**
 * Check if our testing is done and we should exit.
 */
bool Connection::check_exit_condition(double now) {
  if (read_state == INIT_READ) return false;
  if (now == 0.0) now = get_time();
  if (now > start_time + options.time) return true;
  if (options.loadonly && read_state == IDLE) return true;
  return false;
}

/**
 * Handle new connection and error events.
 */
void Connection::event_callback(short events) {
  if (events & BEV_EVENT_CONNECTED) {
    D("Connected to %s:%s.", hostname.c_str(), port.c_str());
    int fd = bufferevent_getfd(bev);
    if (fd < 0) DIE("bufferevent_getfd");

    if (!options.no_nodelay) {
      int one = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     (void *) &one, sizeof(one)) < 0)
        DIE("setsockopt()");
    }

    read_state = CONN_SETUP;
    if (prot->setup_connection_w()) {
      read_state = IDLE;
    }

  } else if (events & BEV_EVENT_ERROR) {
    int err = bufferevent_socket_get_dns_error(bev);
    if (err) DIE("DNS error: %s", evutil_gai_strerror(err));
    DIE("BEV_EVENT_ERROR: %s", strerror(errno));

  } else if (events & BEV_EVENT_EOF) {
    DIE("Unexpected EOF from server.");
  }
}

/**
 * Request generation loop. Determines whether or not to issue a new command,
 * based on timer events.
 *
 * Note that this function loops. Be wary of break vs. return.
 */
void Connection::drive_write_machine(double now) {
  if (now == 0.0) now = get_time();

  double delay;
  struct timeval tv;

  if (check_exit_condition(now)) return;

  while (1) {
    switch (write_state) {
    case INIT_WRITE:
      delay = iagen->generate();
      next_time = now + delay;
      double_to_tv(delay, &tv);
      evtimer_add(timer, &tv);
      write_state = WAITING_FOR_TIME;
      break;

    case ISSUING:
      if (op_queue.size() >= (size_t) options.depth) {
        write_state = WAITING_FOR_OPQ;
        return;
      } else if (now < next_time) {
        write_state = WAITING_FOR_TIME;
        break; // We want to run through the state machine one more time
               // to make sure the timer is armed.
      } else if (options.moderate && now < last_rx + 0.00025) {
        write_state = WAITING_FOR_TIME;
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          delay = last_rx + 0.00025 - now;
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }

      issue_something(now);
      last_tx = now;
      stats.log_op(op_queue.size());
      next_time += iagen->generate();

      if (options.skip && options.lambda > 0.0 &&
          now - next_time > 0.005000 &&
          op_queue.size() >= (size_t) options.depth) {

        while (next_time < now - 0.004000) {
          stats.skips++;
          next_time += iagen->generate();
        }
      }
      break;

    case WAITING_FOR_TIME:
      if (now < next_time) {
        if (!event_pending(timer, EV_TIMEOUT, NULL)) {
          delay = next_time - now;
          double_to_tv(delay, &tv);
          evtimer_add(timer, &tv);
        }
        return;
      }
      write_state = ISSUING;
      break;

    case WAITING_FOR_OPQ:
      if (op_queue.size() >= (size_t) options.depth) return;
      write_state = ISSUING;
      break;

    default: DIE("Not implemented");
    }
  }
}

/**
 * Handle incoming data (responses).
 */
void Connection::read_callback() {
  struct evbuffer *input = bufferevent_get_input(bev);

  Operation *op = NULL;
  bool done, full_read;

  if (op_queue.size() == 0) V("Spurious read callback.");

  while (1) {
    if (op_queue.size() > 0) op = &op_queue.front();

    switch (read_state) {
    case INIT_READ: DIE("event from uninitialized connection");
    case IDLE: return;  // We munched all the data we expected?

    case WAITING_FOR_GET:
      assert(op_queue.size() > 0);
      full_read = prot->handle_response(input, done);
      if (!full_read) {
        return;
      } else if (done) {
        finish_op(op); // sets read_state = IDLE
      }
      break;

    case WAITING_FOR_SET:
      assert(op_queue.size() > 0);
      if (!prot->handle_response(input, done)) return;
      finish_op(op);
      break;

    case LOADING:
      assert(op_queue.size() > 0);
      if (!prot->handle_response(input, done)) return;
      loader_completed++;
      pop_op();

      if (loader_completed == options.records) {
        D("Finished loading.");
        read_state = IDLE;
      } else {
        while (loader_issued < loader_completed + LOADER_CHUNK) {
          if (loader_issued >= options.records) break;

          char key[256];
          string keystr = keygen->generate(loader_issued);
          strcpy(key, keystr.c_str());
          int index = lrand48() % (1024 * 1024);
          issue_set(key, &random_char[index], valuesize->generate());

          loader_issued++;
        }
      }

      break;

    case CONN_SETUP:
      assert(options.binary);
      if (!prot->setup_connection_r(input)) return;
      read_state = IDLE;
      break;

    default: DIE("not implemented");
    }
  }
}

/**
 * Callback called when write requests finish.
 */
void Connection::write_callback() {}

/**
 * Callback for timer timeouts.
 */
void Connection::timer_callback() { drive_write_machine(); }


/* The follow are C trampolines for libevent callbacks. */
void bev_event_cb(struct bufferevent *bev, short events, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->event_callback(events);
}

void bev_read_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->read_callback();
}

void bev_write_cb(struct bufferevent *bev, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->write_callback();
}

void timer_cb(evutil_socket_t fd, short what, void *ptr) {
  Connection* conn = (Connection*) ptr;
  conn->timer_callback();
}

      ConnectionOptions.h                                                                                 0000644 0116471 0021131 00000001304 12772016400 015225  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             #ifndef CONNECTIONOPTIONS_H
#define CONNECTIONOPTIONS_H

#include "distributions.h"

typedef struct {
  int connections;
  bool blocking;
  double lambda;
  int qps;
  int records;

  bool binary;
  bool redis;
  bool sasl;
  char username[32];
  char password[32];

  char keysize[32];
  char valuesize[32];
  // int keysize;
  //  int valuesize;
  char ia[32];

  // qps_per_connection
  // iadist

  double update;
  int time;
  bool loadonly;
  int depth;
  bool no_nodelay;
  bool noload;
  int threads;
  enum distribution_t iadist;
  int warmup;
  bool skip;

  bool roundrobin;
  int server_given;
  int lambda_denom;

  bool oob_thread;

  bool moderate;
} options_t;

#endif // CONNECTIONOPTIONS_H
                                                                                                                                                                                                                                                                                                                            mutilate.cc                                                                                         0000644 0116471 0021131 00000075653 12772016507 013566  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             #include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <queue>
#include <string>
#include <vector>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

#include "config.h"

#ifdef HAVE_LIBZMQ
#include <zmq.hpp>
#endif

#include "AdaptiveSampler.h"
#include "AgentStats.h"
#ifndef HAVE_PTHREAD_BARRIER_INIT
#include "barrier.h"
#endif
#include "cmdline.h"
#include "Connection.h"
#include "ConnectionOptions.h"
#include "log.h"
#include "mutilate.h"
#include "util.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

using namespace std;

gengetopt_args_info args;
char random_char[2 * 1024 * 1024];  // Buffer used to generate random values.

#ifdef HAVE_LIBZMQ
vector<zmq::socket_t*> agent_sockets;
zmq::context_t context(1);
#endif

struct thread_data {
  const vector<string> *servers;
  options_t *options;
  bool master;  // Thread #0, not to be confused with agent master.
#ifdef HAVE_LIBZMQ
  zmq::socket_t *socket;
#endif
};

// struct evdns_base *evdns;

pthread_barrier_t barrier;

double boot_time;

void init_random_stuff();

void go(const vector<string> &servers, options_t &options,
        ConnectionStats &stats
#ifdef HAVE_LIBZMQ
, zmq::socket_t* socket = NULL
#endif
);

void do_mutilate(const vector<string> &servers, options_t &options,
                 ConnectionStats &stats, bool master = true
#ifdef HAVE_LIBZMQ
, zmq::socket_t* socket = NULL
#endif
);
void args_to_options(options_t* options);
void* thread_main(void *arg);

#ifdef HAVE_LIBZMQ
static std::string s_recv (zmq::socket_t &socket) {
  zmq::message_t message;
  socket.recv(&message);

  return std::string(static_cast<char*>(message.data()), message.size());
}

//  Convert string to 0MQ string and send to socket
static bool s_send (zmq::socket_t &socket, const std::string &string) {
  zmq::message_t message(string.size());
  memcpy(message.data(), string.data(), string.size());

  return socket.send(message);
}

/*
 * Agent protocol
 *
 * PREPARATION PHASE
 *
 * 1. Master -> Agent: options_t
 *
 * options_t contains most of the information needed to drive the
 * client, including the aggregate QPS that has been requested.
 * However, neither the master nor the agent know at this point how
 * many total connections will be made to the memcached server.
 *
 * 2. Agent -> Master: int num = (--threads) * (--lambda_mul)
 *
 * The agent sends a number to the master indicating how many threads
 * this mutilate agent will spawn, and a mutiplier that weights how
 * many QPS this agent's connections will send relative to unweighted
 * connections (i.e. we can request that a purely load-generating
 * agent or an agent on a really fast network connection be more
 * aggressive than other agents or the master).
 *
 * 3. Master -> Agent: lambda_denom
 *
 * The master aggregates all of the numbers collected in (2) and
 * computes a global "lambda_denom".  Which is essentially a count of
 * the total number of Connections across all mutilate instances,
 * weighted by lambda_mul if necessary.  It broadcasts this number to
 * all agents.
 *
 * Each instance of mutilate at this point adjusts the lambda in
 * options_t sent in (1) to account for lambda_denom.  Note that
 * lambda_mul is specific to each instance of mutilate
 * (i.e. --lambda_mul X) and not sent as part of options_t.
 *
 *   lambda = qps / lambda_denom * args.lambda_mul;
 *
 * RUN PHASE
 *
 * After the PREP phase completes, everyone executes do_mutilate().
 * All clients spawn threads, open connections, load the DB, and wait
 * for all connections to become IDLE.  Following that, they
 * synchronize and finally do the heavy lifting.
 * 
 * [IF WARMUP] -1:  Master <-> Agent: Synchronize
 * [IF WARMUP]  0:  Everyone: RUN for options.warmup seconds.
 * 1. Master <-> Agent: Synchronize
 * 2. Everyone: RUN for options.time seconds.
 * 3. Master -> Agent: Dummy message
 * 4. Agent -> Master: Send AgentStats [w/ RX/TX bytes, # gets/sets]
 *
 * The master then aggregates AgentStats across all agents with its
 * own ConnectionStats to compute overall statistics.
 */

void agent() {
  zmq::context_t context(1);

  zmq::socket_t socket(context, ZMQ_REP);
  socket.bind((string("tcp://*:")+string(args.agent_port_arg)).c_str());

  while (true) {
    zmq::message_t request;

    socket.recv(&request);

    zmq::message_t num(sizeof(int));
    *((int *) num.data()) = args.threads_arg * args.lambda_mul_arg;
    socket.send(num);

    options_t options;
    memcpy(&options, request.data(), sizeof(options));

    vector<string> servers;

    for (int i = 0; i < options.server_given; i++) {
      servers.push_back(s_recv(socket));
      s_send(socket, "ACK");
    }

    for (auto i: servers) {
      V("Got server = %s", i.c_str());
    }

    options.threads = args.threads_arg;

    socket.recv(&request);
    options.lambda_denom = *((int *) request.data());
    s_send(socket, "THANKS");

    //    V("AGENT SLEEPS"); sleep(1);
    options.lambda = (double) options.qps / options.lambda_denom * args.lambda_mul_arg;

    V("lambda_denom = %d, lambda = %f, qps = %d",
      options.lambda_denom, options.lambda, options.qps);

    //    if (options.threads > 1)
      pthread_barrier_init(&barrier, NULL, options.threads);

    ConnectionStats stats;

    go(servers, options, stats, &socket);

    AgentStats as;

    as.rx_bytes = stats.rx_bytes;
    as.tx_bytes = stats.tx_bytes;
    as.gets = stats.gets;
    as.sets = stats.sets;
    as.get_misses = stats.get_misses;
    as.start = stats.start;
    as.stop = stats.stop;
    as.skips = stats.skips;

    string req = s_recv(socket);
    //    V("req = %s", req.c_str());
    request.rebuild(sizeof(as));
    memcpy(request.data(), &as, sizeof(as));
    socket.send(request);
  }
}

void prep_agent(const vector<string>& servers, options_t& options) {
  int sum = options.lambda_denom;
  if (args.measure_connections_given)
    sum = args.measure_connections_arg * options.server_given * options.threads;

  int master_sum = sum;
  if (args.measure_qps_given) {
    sum = 0;
    if (options.qps) options.qps -= args.measure_qps_arg;
  }

  for (auto s: agent_sockets) {
    zmq::message_t message(sizeof(options_t));

    memcpy((void *) message.data(), &options, sizeof(options_t));
    s->send(message);

    zmq::message_t rep;
    s->recv(&rep);
    unsigned int num = *((int *) rep.data());

    sum += options.connections * (options.roundrobin ?
            (servers.size() > num ? servers.size() : num) : 
            (servers.size() * num));

    for (auto i: servers) {
      s_send(*s, i);
      string rep = s_recv(*s);
    }
  }

  // Adjust options_t according to --measure_* arguments.
  options.lambda_denom = sum;
  options.lambda = (double) options.qps / options.lambda_denom *
    args.lambda_mul_arg;

  V("lambda_denom = %d", sum);

  if (args.measure_qps_given) {
    double master_lambda = (double) args.measure_qps_arg / master_sum;

    if (options.qps && master_lambda > options.lambda)
      V("warning: master_lambda (%f) > options.lambda (%f)",
        master_lambda, options.lambda);

    options.lambda = master_lambda;
  }

  if (args.measure_depth_given) options.depth = args.measure_depth_arg;

  for (auto s: agent_sockets) {
    zmq::message_t message(sizeof(sum));
    *((int *) message.data()) = sum;
    s->send(message);
    string rep = s_recv(*s);
  }

  // Master sleeps here to give agents a chance to connect to
  // memcached server before the master, so that the master is never
  // the very first set of connections.  Is this reasonable or
  // necessary?  Most probably not.
  V("MASTER SLEEPS"); sleep_time(1.5);
}

void finish_agent(ConnectionStats &stats) {
  for (auto s: agent_sockets) {
    s_send(*s, "stats");

    AgentStats as;
    zmq::message_t message;

    s->recv(&message);
    memcpy(&as, message.data(), sizeof(as));
    stats.accumulate(as);
  }
}

/*
 * This synchronization routine is ridiculous because the master only
 * has a ZMQ_REQ socket to the agents, but it needs to wait for a
 * message from each agent before it releases them.  In order to get
 * the ZMQ socket into a state where it'll allow the agent to send it
 * a message, it must first send a message ("sync_req").  In order to
 * not leave the socket dangling with an incomplete transaction, the
 * agent must send a reply ("ack").
 *
 * Without this stupid complication it would be:
 *
 * For each agent:
 *   Agent -> Master: sync
 * For each agent:
 *   Master -> Agent: proceed
 *
 * In this way, all agents must arrive at the barrier and the master
 * must receive a message from each of them before it continues.  It
 * then broadcasts the message to proceed, which reasonably limits
 * skew.
 */

void sync_agent(zmq::socket_t* socket) {
  //  V("agent: synchronizing");

  if (args.agent_given) {
    for (auto s: agent_sockets)
      s_send(*s, "sync_req");

    /* The real sync */
    for (auto s: agent_sockets)
      if (s_recv(*s).compare(string("sync")))
        DIE("sync_agent[M]: out of sync [1]");
    for (auto s: agent_sockets)
      s_send(*s, "proceed");
    /* End sync */

    for (auto s: agent_sockets)
      if (s_recv(*s).compare(string("ack")))
        DIE("sync_agent[M]: out of sync [2]");
  } else if (args.agentmode_given) {
    if (s_recv(*socket).compare(string("sync_req")))
      DIE("sync_agent[A]: out of sync [1]");

    /* The real sync */
    s_send(*socket, "sync");
    if (s_recv(*socket).compare(string("proceed")))
      DIE("sync_agent[A]: out of sync [2]");
    /* End sync */

    s_send(*socket, "ack");
  }

  //  V("agent: synchronized");
}
#endif

string name_to_ipaddr(string host) {
  char *s_copy = new char[host.length() + 1];
  strcpy(s_copy, host.c_str());

  char *saveptr = NULL;  // For reentrant strtok().

  char *h_ptr = strtok_r(s_copy, ":", &saveptr);
  char *p_ptr = strtok_r(NULL, ":", &saveptr);

  char ipaddr[16];

  if (h_ptr == NULL)
    DIE("strtok(.., \":\") failed to parse %s", host.c_str());

  string hostname = h_ptr;
  string port = "11211";
  if (p_ptr) port = p_ptr;

  struct evutil_addrinfo hints;
  struct evutil_addrinfo *answer = NULL;
  int err;

  /* Build the hints to tell getaddrinfo how to act. */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; /* v4 or v6 is fine. */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP; /* We want a TCP socket */
  /* Only return addresses we can use. */
  hints.ai_flags = EVUTIL_AI_ADDRCONFIG;

  /* Look up the hostname. */
  err = evutil_getaddrinfo(h_ptr, NULL, &hints, &answer);
  if (err < 0) {
    DIE("Error while resolving '%s': %s",
        host.c_str(), evutil_gai_strerror(err));
  }

  if (answer == NULL) DIE("No DNS answer.");

  void *ptr = NULL;
  switch (answer->ai_family) {
  case AF_INET:
    ptr = &((struct sockaddr_in *) answer->ai_addr)->sin_addr;
    break;
  case AF_INET6:
    ptr = &((struct sockaddr_in6 *) answer->ai_addr)->sin6_addr;
    break;
  }

  inet_ntop (answer->ai_family, ptr, ipaddr, 16);

  D("Resolved %s to %s", h_ptr, (string(ipaddr) + ":" + string(port)).c_str());

  delete[] s_copy;

  return string(ipaddr) + ":" + string(port);
}

int main(int argc, char **argv) {
  if (cmdline_parser(argc, argv, &args) != 0) exit(-1);

  for (unsigned int i = 0; i < args.verbose_given; i++)
    log_level = (log_level_t) ((int) log_level - 1);

  if (args.quiet_given) log_level = QUIET;

  if (args.depth_arg < 1) DIE("--depth must be >= 1");
  //  if (args.valuesize_arg < 1 || args.valuesize_arg > 1024*1024)
  //    DIE("--valuesize must be >= 1 and <= 1024*1024");
  if (args.qps_arg < 0) DIE("--qps must be >= 0");
  if (args.update_arg < 0.0 || args.update_arg > 1.0)
    DIE("--update must be >= 0.0 and <= 1.0");
  if (args.time_arg < 1) DIE("--time must be >= 1");
  //  if (args.keysize_arg < MINIMUM_KEY_LENGTH)
  //    DIE("--keysize must be >= %d", MINIMUM_KEY_LENGTH);
  if (args.connections_arg < 1 || args.connections_arg > MAXIMUM_CONNECTIONS)
    DIE("--connections must be between [1,%d]", MAXIMUM_CONNECTIONS);
  //  if (get_distribution(args.iadist_arg) == -1)
  //    DIE("--iadist invalid: %s", args.iadist_arg);
  if (!args.server_given && !args.agentmode_given)
    DIE("--server or --agentmode must be specified.");

  // TODO: Discover peers, share arguments.

  init_random_stuff();
  boot_time = get_time();
  setvbuf(stdout, NULL, _IONBF, 0);

  //  struct event_base *base;

  //  if ((base = event_base_new()) == NULL) DIE("event_base_new() fail");
  //  evthread_use_pthreads();

  //  if ((evdns = evdns_base_new(base, 1)) == 0) DIE("evdns");

#ifdef HAVE_LIBZMQ
  if (args.agentmode_given) {
    agent();
    return 0;
  } else if (args.agent_given) {
    for (unsigned int i = 0; i < args.agent_given; i++) {
      zmq::socket_t *s = new zmq::socket_t(context, ZMQ_REQ);
      string host = string("tcp://") + string(args.agent_arg[i]) +
        string(":") + string(args.agent_port_arg);
      s->connect(host.c_str());
      agent_sockets.push_back(s);
    }
  }
#endif

  options_t options;
  args_to_options(&options);

  pthread_barrier_init(&barrier, NULL, options.threads);

  vector<string> servers;
  for (unsigned int s = 0; s < args.server_given; s++)
    servers.push_back(name_to_ipaddr(string(args.server_arg[s])));

  ConnectionStats stats;

  double peak_qps = 0.0;

  if (args.search_given) {
    char *n_ptr = strtok(args.search_arg, ":");
    char *x_ptr = strtok(NULL, ":");

    if (n_ptr == NULL || x_ptr == NULL) DIE("Invalid --search argument");

    int n = atoi(n_ptr);
    int x = atoi(x_ptr);

    I("Search-mode.  Find QPS @ %dus %dth percentile.", x, n);

    int high_qps = 2000000;
    int low_qps = 1; // 5000;
    double nth;
    int cur_qps;

    go(servers, options, stats);

    nth = stats.get_nth(n);
    peak_qps = stats.get_qps();
    high_qps = stats.get_qps();
    cur_qps = stats.get_qps();

    I("peak qps = %d, nth = %.1f", high_qps, nth);

    if (nth > x) {
      //    while ((high_qps > low_qps * 1.02) && cur_qps > 10000) {
    while ((high_qps > low_qps * 1.02) && cur_qps > (peak_qps * .1)) {
      cur_qps = (high_qps + low_qps) / 2;

      args_to_options(&options);

      options.qps = cur_qps;
      options.lambda = (double) options.qps / (double) options.lambda_denom * args.lambda_mul_arg;

      stats = ConnectionStats();

      go(servers, options, stats);

      nth = stats.get_nth(n);

      I("cur_qps = %d, get_qps = %f, nth = %f", cur_qps, stats.get_qps(), nth);

      if (nth > x /*|| cur_qps > stats.get_qps() * 1.05*/) high_qps = cur_qps;
      else low_qps = cur_qps;
    }

    //    while (nth > x && cur_qps > 10000) { // > low_qps) { // 10000) {
      //    while (nth > x && cur_qps > 10000 && cur_qps > (low_qps * 0.90)) {
    while (nth > x && cur_qps > (peak_qps * .1) && cur_qps > (low_qps * 0.90)) {
      cur_qps = cur_qps * 98 / 100;

      args_to_options(&options);

      options.qps = cur_qps;
      options.lambda = (double) options.qps / (double) options.lambda_denom * args.lambda_mul_arg;

      stats = ConnectionStats();

      go(servers, options, stats);

      nth = stats.get_nth(n);

      I("cur_qps = %d, get_qps = %f, nth = %f", cur_qps, stats.get_qps(), nth);
    }

    }
  } else if (args.scan_given) {
    char *min_ptr = strtok(args.scan_arg, ":");
    char *max_ptr = strtok(NULL, ":");
    char *step_ptr = strtok(NULL, ":");

    if (min_ptr == NULL || min_ptr == NULL || step_ptr == NULL)
      DIE("Invalid --scan argument");

    int min = atoi(min_ptr);
    int max = atoi(max_ptr);
    int step = atoi(step_ptr);

    printf("%-7s %7s %7s %7s %7s %7s %7s %7s %7s %8s %8s\n",
           "#type", "avg", "min", "1st", "5th", "10th",
           "90th", "95th", "99th", "QPS", "target");

    for (int q = min; q <= max; q += step) {
      args_to_options(&options);

      options.qps = q;
      options.lambda = (double) options.qps / (double) options.lambda_denom * args.lambda_mul_arg;
      //      options.lambda = (double) options.qps / options.connections /
      //        args.server_given /
      //        (args.threads_arg < 1 ? 1 : args.threads_arg);

      stats = ConnectionStats();

      go(servers, options, stats);

      stats.print_stats("read", stats.get_sampler, false);
      printf(" %8.1f", stats.get_qps());
      printf(" %8d\n", q);
    }    
  } else {
    go(servers, options, stats);
  }

  if (!args.scan_given && !args.loadonly_given) {
    stats.print_header();
    stats.print_stats("read",   stats.get_sampler);
    stats.print_stats("update", stats.set_sampler);
    stats.print_stats("op_q",   stats.op_sampler);

    int total = stats.gets + stats.sets;

    printf("\nTotal QPS = %.1f (%d / %.1fs)\n",
           total / (stats.stop - stats.start),
           total, stats.stop - stats.start);

    if (args.search_given && peak_qps > 0.0)
      printf("Peak QPS  = %.1f\n", peak_qps);

    printf("\n");

    printf("Misses = %" PRIu64 " (%.1f%%)\n", stats.get_misses,
           (double) stats.get_misses/stats.gets*100);

    printf("Skipped TXs = %" PRIu64 " (%.1f%%)\n\n", stats.skips,
           (double) stats.skips / total * 100);

    printf("RX %10" PRIu64 " bytes : %6.1f MB/s\n",
           stats.rx_bytes,
           (double) stats.rx_bytes / 1024 / 1024 / (stats.stop - stats.start));
    printf("TX %10" PRIu64 " bytes : %6.1f MB/s\n",
           stats.tx_bytes,
           (double) stats.tx_bytes / 1024 / 1024 / (stats.stop - stats.start));

    if (args.save_given) {
      printf("Saving latency samples to %s.\n", args.save_arg);

      FILE *file;
      if ((file = fopen(args.save_arg, "w")) == NULL)
        DIE("--save: failed to open %s: %s", args.save_arg, strerror(errno));

      for (auto i: stats.get_sampler.samples) {
        fprintf(file, "%f %f\n", i.start_time - boot_time, i.time());
      }
    }
  }

  //  if (args.threads_arg > 1) 
    pthread_barrier_destroy(&barrier);

#ifdef HAVE_LIBZMQ
  if (args.agent_given) {
    for (auto i: agent_sockets) delete i;
  }
#endif

  // evdns_base_free(evdns, 0);
  // event_base_free(base);

  cmdline_parser_free(&args);
}

void go(const vector<string>& servers, options_t& options,
        ConnectionStats &stats
#ifdef HAVE_LIBZMQ
, zmq::socket_t* socket
#endif
) {
#ifdef HAVE_LIBZMQ
  if (args.agent_given > 0) {
    prep_agent(servers, options);
  }
#endif

  if (options.threads > 1) {
    pthread_t pt[options.threads];
    struct thread_data td[options.threads];
#ifdef __clang__
    vector<string>* ts = static_cast<vector<string>*>(alloca(sizeof(vector<string>) * options.threads));
#else
    vector<string> ts[options.threads];
#endif

#ifdef __linux__
    int current_cpu = -1;
#endif

    for (int t = 0; t < options.threads; t++) {
      td[t].options = &options;
#ifdef HAVE_LIBZMQ
      td[t].socket = socket;
#endif
      if (t == 0) td[t].master = true;
      else td[t].master = false;

      if (options.roundrobin) {
        for (unsigned int i = (t % servers.size());
             i < servers.size(); i += options.threads)
          ts[t].push_back(servers[i % servers.size()]);

        td[t].servers = &ts[t];
      } else {
        td[t].servers = &servers;
      }

      pthread_attr_t attr;
      pthread_attr_init(&attr);

#ifdef __linux__
      if (args.affinity_given) {
        int max_cpus = 8 * sizeof(cpu_set_t);
        cpu_set_t m;
        CPU_ZERO(&m);
        sched_getaffinity(0, sizeof(cpu_set_t), &m);

        for (int i = 0; i < max_cpus; i++) {
          int c = (current_cpu + i + 1) % max_cpus;
          if (CPU_ISSET(c, &m)) {
            CPU_ZERO(&m);
            CPU_SET(c, &m);
            int ret;
            if ((ret = pthread_attr_setaffinity_np(&attr,
                                                   sizeof(cpu_set_t), &m)))
              DIE("pthread_attr_setaffinity_np(%d) failed: %s",
                  c, strerror(ret));
            current_cpu = c;
            break;
          }
        }
      }
#endif

      if (pthread_create(&pt[t], &attr, thread_main, &td[t]))
        DIE("pthread_create() failed");
    }

    for (int t = 0; t < options.threads; t++) {
      ConnectionStats *cs;
      if (pthread_join(pt[t], (void**) &cs)) DIE("pthread_join() failed");
      stats.accumulate(*cs);
      delete cs;
    }
  } else if (options.threads == 1) {
    do_mutilate(servers, options, stats, true
#ifdef HAVE_LIBZMQ
, socket
#endif
);
  } else {
#ifdef HAVE_LIBZMQ
    if (args.agent_given) {
      sync_agent(socket);
    }
#endif
  }

#ifdef HAVE_LIBZMQ
  if (args.agent_given > 0) {
    int total = stats.gets + stats.sets;

    V("Local QPS = %.1f (%d / %.1fs)",
      total / (stats.stop - stats.start),
      total, stats.stop - stats.start);    

    finish_agent(stats);
  }
#endif
}

void* thread_main(void *arg) {
  struct thread_data *td = (struct thread_data *) arg;

  ConnectionStats *cs = new ConnectionStats();

  do_mutilate(*td->servers, *td->options, *cs, td->master
#ifdef HAVE_LIBZMQ
, td->socket
#endif
);

  return cs;
}

void do_mutilate(const vector<string>& servers, options_t& options,
                 ConnectionStats& stats, bool master
#ifdef HAVE_LIBZMQ
, zmq::socket_t* socket
#endif
) {
  int loop_flag =
    (options.blocking || args.blocking_given) ? EVLOOP_ONCE : EVLOOP_NONBLOCK;

  char *saveptr = NULL;  // For reentrant strtok().

  struct event_base *base;
  struct evdns_base *evdns;
  struct event_config *config;

  if ((config = event_config_new()) == NULL) DIE("event_config_new() fail");

#ifdef HAVE_DECL_EVENT_BASE_FLAG_PRECISE_TIMER
  if (event_config_set_flag(config, EVENT_BASE_FLAG_PRECISE_TIMER))
    DIE("event_config_set_flag(EVENT_BASE_FLAG_PRECISE_TIMER) fail");
#endif

  if ((base = event_base_new_with_config(config)) == NULL)
    DIE("event_base_new() fail");

  //  evthread_use_pthreads();

  if ((evdns = evdns_base_new(base, 1)) == 0) DIE("evdns");

  //  event_base_priority_init(base, 2);

  // FIXME: May want to move this to after all connections established.
  double start = get_time();
  double now = start;

  vector<Connection*> connections;
  vector<Connection*> server_lead;

  for (auto s: servers) {
    // Split args.server_arg[s] into host:port using strtok().
    char *s_copy = new char[s.length() + 1];
    strcpy(s_copy, s.c_str());

    char *h_ptr = strtok_r(s_copy, ":", &saveptr);
    char *p_ptr = strtok_r(NULL, ":", &saveptr);

    if (h_ptr == NULL) DIE("strtok(.., \":\") failed to parse %s", s.c_str());

    string hostname = h_ptr;
    string port = "11211";
    if (p_ptr) port = p_ptr;

    delete[] s_copy;

    int conns = args.measure_connections_given ? args.measure_connections_arg :
      options.connections;

    for (int c = 0; c < conns; c++) {
      Connection* conn = new Connection(base, evdns, hostname, port, options,
                                        args.agentmode_given ? false :
                                        true);
      connections.push_back(conn);
      if (c == 0) server_lead.push_back(conn);
    }
  }

  // Wait for all Connections to become IDLE.
  while (1) {
    // FIXME: If all connections become ready before event_base_loop
    // is called, this will deadlock.
    event_base_loop(base, EVLOOP_ONCE);

    bool restart = false;
    for (Connection *conn: connections)
      if (!conn->is_ready()) restart = true;

    if (restart) continue;
    else break;
  }

  // Load database on lead connection for each server.
  if (!options.noload) {
    V("Loading database.");

    for (auto c: server_lead) c->start_loading();

    // Wait for all Connections to become IDLE.
    while (1) {
      // FIXME: If all connections become ready before event_base_loop
      // is called, this will deadlock.
      event_base_loop(base, EVLOOP_ONCE);

      bool restart = false;
      for (Connection *conn: connections)
        if (!conn->is_ready()) restart = true;

      if (restart) continue;
      else break;
    }
  }

  if (options.loadonly) {
    evdns_base_free(evdns, 0);
    event_base_free(base);
    return;
  }

  // FIXME: Remove.  Not needed, testing only.
  //  // FIXME: Synchronize start_time here across threads/nodes.
  //  pthread_barrier_wait(&barrier);

  // Warmup connection.
  if (options.warmup > 0) {
    if (master) V("Warmup start.");

#ifdef HAVE_LIBZMQ
    if (args.agent_given || args.agentmode_given) {
      if (master) V("Synchronizing.");

      // 1. thread barrier: make sure our threads ready before syncing agents
      // 2. sync agents: all threads across all agents are now ready
      // 3. thread barrier: don't release our threads until all agents ready
      pthread_barrier_wait(&barrier);
      if (master) sync_agent(socket);
      pthread_barrier_wait(&barrier);

      if (master) V("Synchronized.");
    }
#endif

    int old_time = options.time;
    //    options.time = 1;

    start = get_time();
    for (Connection *conn: connections) {
      conn->start_time = start;
      conn->options.time = options.warmup;
      conn->start(); // Kick the Connection into motion.
    }

    while (1) {
      event_base_loop(base, loop_flag);

      //#ifdef USE_CLOCK_GETTIME
      //      now = get_time();
      //#else
      struct timeval now_tv;
      event_base_gettimeofday_cached(base, &now_tv);
      now = tv_to_double(&now_tv);
      //#endif

      bool restart = false;
      for (Connection *conn: connections)
        if (!conn->check_exit_condition(now))
          restart = true;

      if (restart) continue;
      else break;
    }

    bool restart = false;
    for (Connection *conn: connections)
      if (!conn->is_ready()) restart = true;

    if (restart) {

    // Wait for all Connections to become IDLE.
    while (1) {
      // FIXME: If there were to use EVLOOP_ONCE and all connections
      // become ready before event_base_loop is called, this will
      // deadlock.  We should check for IDLE before calling
      // event_base_loop.
      event_base_loop(base, EVLOOP_ONCE); // EVLOOP_NONBLOCK);

      bool restart = false;
      for (Connection *conn: connections)
        if (!conn->is_ready()) restart = true;

      if (restart) continue;
      else break;
    }
    }

    for (Connection *conn: connections) {
      conn->reset();
      conn->options.time = old_time;
    }

    if (master) V("Warmup stop.");
  }


  // FIXME: Synchronize start_time here across threads/nodes.
  pthread_barrier_wait(&barrier);

  if (master && args.wait_given) {
    if (get_time() < boot_time + args.wait_arg) {
      double t = (boot_time + args.wait_arg)-get_time();
      V("Sleeping %.1fs for -W.", t);
      sleep_time(t);
    }
  }

#ifdef HAVE_LIBZMQ
  if (args.agent_given || args.agentmode_given) {
    if (master) V("Synchronizing.");

    pthread_barrier_wait(&barrier);
    if (master) sync_agent(socket);
    pthread_barrier_wait(&barrier);

    if (master) V("Synchronized.");
  }
#endif

  if (master && !args.scan_given && !args.search_given)
    V("started at %f", get_time());

  start = get_time();
  for (Connection *conn: connections) {
    conn->start_time = start;
    conn->start(); // Kick the Connection into motion.
  }

  //  V("Start = %f", start);

  // Main event loop.
  while (1) {
    event_base_loop(base, loop_flag);

    //#if USE_CLOCK_GETTIME
    //    now = get_time();
    //#else
    struct timeval now_tv;
    event_base_gettimeofday_cached(base, &now_tv);
    now = tv_to_double(&now_tv);
    //#endif

    bool restart = false;
    for (Connection *conn: connections)
      if (!conn->check_exit_condition(now))
        restart = true;

    if (restart) continue;
    else break;
  }

  if (master && !args.scan_given && !args.search_given)
    V("stopped at %f  options.time = %d", get_time(), options.time);

  // Tear-down and accumulate stats.
  for (Connection *conn: connections) {
    stats.accumulate(conn->stats);
    delete conn;
  }

  stats.start = start;
  stats.stop = now;

  event_config_free(config);
  evdns_base_free(evdns, 0);
  event_base_free(base);
}

void args_to_options(options_t* options) {
  //  bzero(options, sizeof(options_t));
  options->connections = args.connections_arg;
  options->blocking = args.blocking_given;
  options->qps = args.qps_arg;
  options->threads = args.threads_arg;
  options->server_given = args.server_given;
  options->roundrobin = args.roundrobin_given;

  int connections = options->connections;
  if (options->roundrobin) {
    connections *= (options->server_given > options->threads ?
                    options->server_given : options->threads);
  } else {
    connections *= options->server_given * options->threads;
  }

  //  if (args.agent_given) connections *= (1 + args.agent_given);

  options->lambda_denom = connections > 1 ? connections : 1;
  if (args.lambda_mul_arg > 1) options->lambda_denom *= args.lambda_mul_arg;

  if (options->threads < 1) options->lambda_denom = 0;

  options->lambda = (double) options->qps / (double) options->lambda_denom * args.lambda_mul_arg;

  //  V("%d %d %d %f", options->qps, options->connections,
  //  connections, options->lambda);

  //  if (args.no_record_scale_given)
  //    options->records = args.records_arg;
  //  else
  options->records = args.records_arg / options->server_given;

  options->binary = args.binary_given;
  options->redis = args.redis_given;
  options->sasl = args.username_given;
  
  if (args.password_given)
    strcpy(options->password, args.password_arg);
  else
    strcpy(options->password, "");

  if (args.username_given)
    strcpy(options->username, args.username_arg);
  else
    strcpy(options->username, "");

  D("options->records = %d", options->records);

  if (!options->records) options->records = 1;
  strcpy(options->keysize, args.keysize_arg);
  //  options->keysize = args.keysize_arg;
  strcpy(options->valuesize, args.valuesize_arg);
  //  options->valuesize = args.valuesize_arg;
  options->update = args.update_arg;
  options->time = args.time_arg;
  options->loadonly = args.loadonly_given;
  options->depth = args.depth_arg;
  options->no_nodelay = args.no_nodelay_given;
  options->noload = args.noload_given;
  options->iadist = get_distribution(args.iadist_arg);
  strcpy(options->ia, args.iadist_arg);
  options->warmup = args.warmup_given ? args.warmup_arg : 0;
  options->oob_thread = false;
  options->skip = args.skip_given;
  options->moderate = args.moderate_given;
}

void init_random_stuff() {
  static char lorem[] =
    R"(Lorem ipsum dolor sit amet, consectetur adipiscing elit. Maecenas turpis dui, suscipit non vehicula non, malesuada id sem. Phasellus suscipit nisl ut dui consectetur ultrices tincidunt eros aliquet. Donec feugiat lectus sed nibh ultrices ultrices. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Mauris suscipit eros sed justo lobortis at ultrices lacus molestie. Duis in diam mi. Cum sociis natoque penatibus et magnis dis parturient montes, nascetur ridiculus mus. Ut cursus viverra sagittis. Vivamus non facilisis tortor. Integer lectus arcu, sagittis et eleifend rutrum, condimentum eget sem. Vestibulum tempus tellus non risus semper semper. Morbi molestie rhoncus mi, in egestas dui facilisis et.)";

  size_t cursor = 0;

  while (cursor < sizeof(random_char)) {
    size_t max = sizeof(lorem);
    if (sizeof(random_char) - cursor < max)
      max = sizeof(random_char) - cursor;

    memcpy(&random_char[cursor], lorem, max);
    cursor += max;
  }
}

                                                                                     Connection.h                                                                                        0000644 0116471 0021131 00000006211 12772016616 013664  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             // -*- c++-mode -*-
#ifndef CONNECTION_H
#define CONNECTION_H

#include <queue>
#include <string>

#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/util.h>

#include "AdaptiveSampler.h"
#include "cmdline.h"
#include "ConnectionOptions.h"
#include "ConnectionStats.h"
#include "Generator.h"
#include "Operation.h"
#include "util.h"

#include "Protocol.h"

using namespace std;

void bev_event_cb(struct bufferevent *bev, short events, void *ptr);
void bev_read_cb(struct bufferevent *bev, void *ptr);
void bev_write_cb(struct bufferevent *bev, void *ptr);
void timer_cb(evutil_socket_t fd, short what, void *ptr);

class Protocol;

class Connection {
public:
  Connection(struct event_base* _base, struct evdns_base* _evdns,
             string _hostname, string _port, options_t options,
             bool sampling = true);
  ~Connection();

  double start_time; // Time when this connection began operations.
  ConnectionStats stats;
  options_t options;

  bool is_ready() { return read_state == IDLE; }
  void set_priority(int pri);

  // state commands
  void start() { drive_write_machine(); }
  void start_loading();
  void reset();
  bool check_exit_condition(double now = 0.0);

  // event callbacks
  void event_callback(short events);
  void read_callback();
  void write_callback();
  void timer_callback();

private:
  string hostname;
  string port;

  struct event_base *base;
  struct evdns_base *evdns;
  struct bufferevent *bev;

  struct event *timer; // Used to control inter-transmission time.
  double next_time;    // Inter-transmission time parameters.
  double last_rx;      // Used to moderate transmission rate.
  double last_tx;

  enum read_state_enum {
    INIT_READ,
    CONN_SETUP,
    LOADING,
    IDLE,
    WAITING_FOR_GET,
    WAITING_FOR_SET,
    MAX_READ_STATE,
  };

  enum write_state_enum {
    INIT_WRITE,
    ISSUING,
    WAITING_FOR_TIME,
    WAITING_FOR_OPQ,
    MAX_WRITE_STATE,
  };

  read_state_enum read_state;
  write_state_enum write_state;

  // Parameters to track progress of the data loader.
  int loader_issued, loader_completed;

  Protocol *prot;
  Generator *valuesize;
  Generator *keysize;
  KeyGenerator *keygen;
  Generator *iagen;
  std::queue<Operation> op_queue;

  // state machine functions / event processing
  void pop_op();
  void finish_op(Operation *op);
  void issue_something(double now = 0.0);
  void drive_write_machine(double now = 0.0);

  // request functions
  void issue_sasl();
  void issue_get(const char* key, double now = 0.0);
  void issue_set(const char* key, const char* value, int length,
                 double now = 0.0);

  // protocol fucntions
  int set_request_ascii(const char* key, const char* value, int length);
  int set_request_binary(const char* key, const char* value, int length);
  int set_request_resp(const char* key, const char* value, int length);
  
  int get_request_ascii(const char* key);
  int get_request_binary(const char* key);
  int get_request_resp(const char* key);

  bool consume_binary_response(evbuffer *input);
  bool consume_ascii_line(evbuffer *input, bool &done);
  bool consume_resp_line(evbuffer *input, bool &done);
};

#endif
                                                                                                                                                                                                                                                                                                                                                                                       cmdline.ggo                                                                                         0000644 0116471 0021131 00000011613 12772017021 013516  0                                                                                                    ustar   djbyrne                         celerra_du                                                                                                                                                                                                             package "mutilate"
version "0.1"
usage "mutilate -s server[:port] [options]"
description "\"High-performance\" memcached benchmarking tool"

args "-c cc --show-required -C --default-optional -l"

option "verbose" v "Verbosity. Repeat for more verbose." multiple
option "quiet" - "Disable log messages."

text "\nBasic options:"

option "server" s "Memcached server hostname[:port].  \
Repeat to specify multiple servers." string multiple
option "binary" - "Use binary memcached protocol instead of ASCII."
option "redis"  - "Use Redis RESP protocol instead of memchached."
option "qps" q "Target aggregate QPS. 0 = peak QPS." int default="0"
option "time" t "Maximum time to run (seconds)." int default="5"

option "keysize" K "Length of memcached keys (distribution)."
       string default="30"
option "valuesize" V "Length of memcached values (distribution)."
       string default="200"

option "records" r "Number of memcached records to use.  \
If multiple memcached servers are given, this number is divided \
by the number of servers." int default="10000"

option "update" u "Ratio of set:get commands." float default="0.0"

text "\nAdvanced options:"

option "username" U "Username to use for SASL authentication." string
option "password" P "Password to use for SASL authentication." string
option "threads" T "Number of threads to spawn." int default="1"
option "affinity" - "Set CPU affinity for threads, round-robin"
option "connections" c "Connections to establish per server." int default="1"
option "depth" d "Maximum depth to pipeline requests." int default="1"
option "roundrobin" R "Assign threads to servers in round-robin fashion.  \
By default, each thread connects to every server."

option "iadist" i "Inter-arrival distribution (distribution).  Note: \
The distribution will automatically be adjusted to match the QPS given \
by --qps." string default="exponential"
option "skip" S "Skip transmissions if previous requests are late.  This \
harms the long-term QPS average, but reduces spikes in QPS after \
long latency requests."
option "moderate" - "Enforce a minimum delay of ~1/lambda between requests."

option "noload" - "Skip database loading."
option "loadonly" - "Load database and then exit."

option "blocking" B "Use blocking epoll().  May increase latency."
option "no_nodelay" - "Don't use TCP_NODELAY."

option "warmup" w "Warmup time before starting measurement." int
option "wait" W "Time to wait after startup to start measurement." int
option "save" - "Record latency samples to given file." string

option "search" - "Search for the QPS where N-order statistic < Xus.  \
(i.e. --search 95:1000 means find the QPS where 95% of requests are \
faster than 1000us)." string typestr="N:X"
option "scan" - "Scan latency across QPS rates from min to max."
       string typestr="min:max:step"

text "\nAgent-mode options:"
option "agentmode" A "Run client in agent mode."
option "agent" a "Enlist remote agent." string typestr="host" multiple
option "agent_port" p "Agent port." string default="5556"
option "lambda_mul" l "Lambda multiplier.  Increases share of \
QPS for this client." int default="1"
option "measure_connections" C "Master client connections per server, \
overrides --connections." int
option "measure_qps" Q "Explicitly set master client QPS, \
spread across threads and connections." int
option "measure_depth" D "Set master client connection depth." int

text "
The --measure_* options aid in taking latency measurements of the
memcached server without incurring significant client-side queuing
delay.  --measure_connections allows the master to override the
--connections option.  --measure_depth allows the master to operate as
an \"open-loop\" client while other agents continue as a regular
closed-loop clients.  --measure_qps lets you modulate the QPS the
master queries at independent of other clients.  This theoretically
normalizes the baseline queuing delay you expect to see across a wide
range of --qps values.
"

text "
Some options take a 'distribution' as an argument.
Distributions are specified by <distribution>[:<param1>[,...]].
Parameters are not required.  The following distributions are supported:

   [fixed:]<value>              Always generates <value>.
   uniform:<max>                Uniform distribution between 0 and <max>.
   normal:<mean>,<sd>           Normal distribution.
   exponential:<lambda>         Exponential distribution.
   pareto:<loc>,<scale>,<shape> Generalized Pareto distribution.
   gev:<loc>,<scale>,<shape>    Generalized Extreme Value distribution.

   To recreate the Facebook \"ETC\" request stream from [1], the
   following hard-coded distributions are also provided:

   fb_value   = a hard-coded discrete and GPareto PDF of value sizes
   fb_key     = \"gev:30.7984,8.20449,0.078688\", key-size distribution
   fb_ia      = \"pareto:0.0,16.0292,0.154971\", inter-arrival time dist.

[1] Berk Atikoglu et al., Workload Analysis of a Large-Scale Key-Value Store,
    SIGMETRICS 2012
"

                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     