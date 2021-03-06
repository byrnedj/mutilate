#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>

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
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <string.h>
#include "blockingconcurrentqueue.h"


using namespace moodycamel;
std::hash<string> hashstr;

extern ifstream kvfile;
extern pthread_mutex_t flock;
extern pthread_mutex_t *item_locks;
extern int item_lock_hashpower;

#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)

pthread_mutex_t cid_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t connids = 0;

pthread_mutex_t opaque_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_opaque = 0;

void item_lock(size_t hv, uint32_t cid) {
    //char out[128];
    //sprintf(out,"conn: %u, locking %lu\n",cid,hv);
    //write(2,out,strlen(out));
    pthread_mutex_lock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

void item_unlock(size_t hv, uint32_t cid) {
    //char out[128];
    //sprintf(out,"conn: %u, unlocking %lu\n",cid,hv);
    //write(2,out,strlen(out));
    pthread_mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

void *item_trylock(uint32_t hv, uint32_t cid) {
    pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
    if (pthread_mutex_trylock(lock) == 0) {
        //char out[128];
        //sprintf(out,"conn: %u, locking %u\n",cid,hv);
        //write(2,out,strlen(out));
        return lock;
    }
    return NULL;
}

void item_trylock_unlock(void *lock, uint32_t cid) {
    //char out[128];
    //sprintf(out,"conn: %u, unlocking\n",cid);
    //write(2,out,strlen(out));
    pthread_mutex_unlock((pthread_mutex_t *) lock);
}

void Connection::output_op(Operation *op, int type, bool found) {
    char output[1024];
    char k[256];
    char a[256];
    char s[256];
    memset(k,0,256);
    memset(a,0,256);
    memset(s,0,256);
    strcpy(k,op->key.c_str());
    switch (type) {
        case 0: //get
            sprintf(a,"issue_get");
            break;
        case 1: //set
            sprintf(a,"issue_set");
            break;
        case 2: //resp
            sprintf(a,"resp");
            break;
    }
    switch(read_state) {
        case INIT_READ:
            sprintf(s,"init");
            break;
        case CONN_SETUP:
            sprintf(s,"setup");
            break;
        case LOADING:
            sprintf(s,"load");
            break;
        case IDLE:
            sprintf(s,"idle");
            break;
        case WAITING_FOR_GET:
            sprintf(s,"waiting for get");
            break;
        case WAITING_FOR_SET:
            sprintf(s,"waiting for set");
            break;
        case WAITING_FOR_DELETE:
            sprintf(s,"waiting for del");
            break;
        case MAX_READ_STATE:
            sprintf(s,"max");
            break;
    }
    if (type == 2) {
        sprintf(output,"conn: %u, action: %s op: %s, opaque: %u, found: %d, type: %d\n",cid,a,k,op->opaque,found,op->type);
    } else {
        sprintf(output,"conn: %u, action: %s op: %s, opaque: %u, type: %d\n",cid,a,k,op->opaque,op->type);
    }
    write(2,output,strlen(output));
}

/**
 * Create a new connection to a server endpoint.
 */
Connection::Connection(struct event_base* _base, struct evdns_base* _evdns,
                       string _hostname, string _port, options_t _options,
                       ConcurrentQueue<string>* a_trace_queue,
                       bool sampling ) :
  start_time(0), stats(sampling), options(_options),
  hostname(_hostname), port(_port), base(_base), evdns(_evdns)
{
  valuesize = createGenerator(options.valuesize);
  keysize = createGenerator(options.keysize);

  trace_queue = a_trace_queue;
  eof = 0;

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

  last_miss = 0;
  pthread_mutex_lock(&cid_lock);
  cid = connids++;
  pthread_mutex_unlock(&cid_lock);
  timer = evtimer_new(base, timer_cb, this);
}

int Connection::do_connect() {

  int connected = 0;
  if (options.unix_socket) {
  
    bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, bev_read_cb, bev_write_cb, bev_event_cb, this);
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    struct sockaddr_un sin;
    memset(&sin, 0, sizeof(sin));
    sin.sun_family = AF_LOCAL;
    strcpy(sin.sun_path, hostname.c_str());

    int addrlen;
    addrlen = sizeof(sin);
    int err = bufferevent_socket_connect(bev,  (struct sockaddr*)&sin, addrlen);
    if (err == 0) {
        connected = 1;
        if (options.binary) {
          prot = new ProtocolBinary(options, this, bev);
        } else if (options.redis) {
          prot = new ProtocolRESP(options, this, bev);
        } else {
          prot = new ProtocolAscii(options, this, bev);
        }
    } else {
	connected = 0;
        err = errno;
	fprintf(stderr,"error %s\n",strerror(err));
        bufferevent_free(bev);
        //event_base_free(_evbase_ptr);
    }
  } else {
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
                                          atoi(port.c_str())) == 0) {
        connected = 1;
    } else {
        bufferevent_free(bev);
        connected = 0;
    }
  }
  return connected;
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
  memset(key,0,256);
  // FIXME: generate key distribution here!
  string keystr = keygen->generate(lrand48() % options.records);
  strncpy(key, keystr.c_str(),255);

  if (drand48() < options.update) {
    int index = lrand48() % (1024 * 1024);
    issue_set(key, &random_char[index], valuesize->generate(), now);
  } else {
    issue_get(key, now);
  }
}


/**
 * Get/Set Style
 * Issue a get first, if not found then set
 */
void Connection::issue_getset(double now) {
  
    if (!options.read_file && !kvfile.is_open())
    {
        string keystr;
        char key[256];
        memset(key,0,256);
        keystr = keygen->generate(lrand48() % options.records);
        strncpy(key, keystr.c_str(),255);
        
        char log[1024];
        int length = valuesize->generate();
        sprintf(log,"%s,%d\n",key,length);
        write(2,log,strlen(log));
        
        issue_get_with_len(key, length, now);
    }
    else
    {
        string line;
        string rT;
        string rApp;
        string rReq;
        string rKey;
        string rvaluelen;
        
        pthread_mutex_lock(&flock);
        getline(kvfile,line);
        pthread_mutex_unlock(&flock);
        stringstream ss(line);
        getline( ss, rT, ',');
        getline( ss, rApp, ',');
        getline( ss, rReq, ',');
        getline( ss, rKey, ',' );
        getline( ss, rvaluelen, ',' );
        
        int vl = atoi(rvaluelen.c_str());
        
        char key[256];
        memset(key,0,256);
        strncpy(key, rKey.c_str(),255);
        issue_get_with_len(key, vl, now);
    }

}

int Connection::issue_something_trace(double now) {
    int ret = 0;

    string line;
    string rT;
    string rApp;
    string rOp;
    string rKey;
    string rKeySize;
    string rvaluelen;

    pthread_mutex_lock(&flock);
    if (kvfile.good()) {
        getline(kvfile,line);
        pthread_mutex_unlock(&flock);
    }
    else {
        pthread_mutex_unlock(&flock);
        return 1;
    }
    stringstream ss(line);
    int Op = 0;
    int vl = 0;

    if (options.twitter_trace == 1) {
        getline( ss, rT, ',' );
        getline( ss, rKey, ',' );
        getline( ss, rKeySize, ',' );
        getline( ss, rvaluelen, ',' );
        getline( ss, rApp, ',' );
        getline( ss, rOp, ',' );
        vl = atoi(rvaluelen.c_str());
        if (vl < 1) vl = 1;
        if (rOp.compare("get") == 0) {
            Op = 1;
        } else if (rOp.compare("set") == 0) {
            Op = 2;
        } else {
            Op = 0;
        }

        while (Op == 0) {
            string line1;
            pthread_mutex_lock(&flock);
            if (kvfile.good()) {
                getline(kvfile,line1);
                pthread_mutex_unlock(&flock);
            }
            stringstream ss1(line1);
            getline( ss1, rT, ',' );
            getline( ss1, rKey, ',' );
            getline( ss1, rKeySize, ',' );
            getline( ss1, rvaluelen, ',' );
            getline( ss1, rApp, ',' );
            getline( ss1, rOp, ',' );
            vl = atoi(rvaluelen.c_str());
            if (vl < 1) vl = 1;

            if (rOp.compare("get") == 0) {
                Op = 1;
            } else if (rOp.compare("set") == 0) {
                Op = 2;
            } else {
                Op = 0;
            }
        }
        
    } else {
        getline( ss, rT, ',' );
        getline( ss, rApp, ',' );
        getline( ss, rOp, ',' );
        getline( ss, rKey, ',' );
        getline( ss, rvaluelen, ',' );
        if (rOp.compare("read") == 0) 
            Op = 1;
        if (rOp.compare("write") == 0) 
            Op = 2;
        vl = atoi(rvaluelen.c_str());
    }


    if (vl > 524000) vl = 524000;
    //if (strcmp(key,"100004781") == 0) {
    //   fprintf(stderr,"ready!\n");
    //}
    switch(Op)
    {
      case 1:
          issue_get_with_len(rKey.c_str(), vl, now);
          break;
      case 2:
          int index = lrand48() % (1024 * 1024);
          issue_set(rKey.c_str(), &random_char[index], vl, now,true);
          break;
    }
    return ret;
}


/**
 * Get/Set or Set Style
 * If a GET command: Issue a get first, if not found then set
 * If trace file (or prob. write) says to set, then set it
 */
int Connection::issue_getsetorset(double now) {
 
  int ret = 0;

  if (!options.read_file)
  {
        string keystr;
        char key[256];
        memset(key,0,256);
        keystr = keygen->generate(lrand48() % options.records);
        strncpy(key, keystr.c_str(),255);
        
        char log[1024];
        int length = valuesize->generate();
        sprintf(log,"%s,%d\n",key,length);
        write(2,log,strlen(log));
        
        issue_get_with_len(key, length, now);
  }
  else
  {
        string line;
        string rT;
        string rApp;
        string rOp;
        string rKey;
        string rKeySize;
        string rvaluelen;

        int nissued = 0;
        while (nissued < options.depth) {
            bool res = trace_queue->try_dequeue(line);
            if (res) {
                if (line.compare("EOF") == 0) {
                    eof = 1;
                    return 1;
                }
                /*
                pthread_mutex_lock(&flock);
                if (kvfile.good()) {
                    getline(kvfile,line);
                    pthread_mutex_unlock(&flock);
                }
                else {
                    pthread_mutex_unlock(&flock);
                    return 1;
                }
                */
                stringstream ss(line);
                int Op = 0;
                int vl = 0; 

                if (options.twitter_trace == 1) {
                    getline( ss, rT, ',' );
                    getline( ss, rKey, ',' );
                    getline( ss, rKeySize, ',' );
                    getline( ss, rvaluelen, ',' );
                    getline( ss, rApp, ',' );
                    getline( ss, rOp, ',' );
                    vl = atoi(rvaluelen.c_str());
                    if (vl < 1) vl = 1;
                    if (vl > 524000) vl = 524000;
                    if (rOp.compare("get") == 0) {
                        Op = 1;
                    } else if (rOp.compare("set") == 0) {
                        Op = 2;
                    } else {
                        Op = 0;
                    }
                    
                    //char buf[1024];
                    //sprintf(buf,"%s,%d\n",rKey.c_str(),vl);
                    //write(1,buf,strlen(buf));
                    
                } else if (options.twitter_trace == 2) {
                    getline( ss, rT, ',' );
                    getline( ss, rApp, ',' );
                    getline( ss, rOp, ',' );
                    getline( ss, rKey, ',' );
                    getline( ss, rvaluelen, ',' );
                    Op = atoi(rOp.c_str());
                    vl = atoi(rvaluelen.c_str());
                }
                else {
                    getline( ss, rT, ',' );
                    getline( ss, rApp, ',' );
                    getline( ss, rOp, ',' );
                    getline( ss, rKey, ',' );
                    getline( ss, rvaluelen, ',' );
                    vl = atoi(rvaluelen.c_str());
                    if (rOp.compare("read") == 0) 
                        Op = 1;
                    if (rOp.compare("write") == 0) 
                        Op = 2;
                }


                char key[256];
                memset(key,0,256);
                strncpy(key, rKey.c_str(),255);
                int issued = 0;
                switch(Op)
                {
                  case 0:
                      fprintf(stderr,"invalid line: %s, vl: %d @T: %d\n",
                              key,vl,atoi(rT.c_str()));
                      break;
                  case 1:
                      issued = issue_get_with_len(key, vl, now);
                      break;
                  case 2:
                      int index = lrand48() % (1024 * 1024);
                      issued = issue_set(key, &random_char[index], vl, now,true);
                      break;
                
                }
                if (issued) {
                    nissued++;
                } else {
                      fprintf(stderr,"failed to issue line: %s, vl: %d @T: %d\n",
                              key,vl,atoi(rT.c_str()));
                      break;
                }
            }
        }
   }
  
  return ret;
}

/**
 * Issue a get request to the server.
 */
int Connection::issue_get_with_len(const char* key, int valuelen, double now) {
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

  //record before rx 
  //r_vsize = stats.rx_bytes % 100000;
  pthread_mutex_lock(&opaque_lock);
  op.opaque = g_opaque++;
  pthread_mutex_unlock(&opaque_lock);

  op.key = string(key);
  op.valuelen = valuelen;
  op.type = Operation::GET;
  op.hv = hashstr(op.key);
  //item_lock(op.hv,cid);
  //pthread_mutex_t *lock = (pthread_mutex_t*)item_trylock(op.hv,cid);
  //if (lock != NULL) {
    op_queue[op.opaque] = op;
    //output_op(&op,0,0);

    //if (read_state == IDLE) read_state = WAITING_FOR_GET;
    l = prot->get_request(key,op.opaque);
    if (read_state != LOADING) stats.tx_bytes += l;
    
    stats.log_access(op);
    return 1;
  //} else {
  // return 0;
  //}
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

  //record before rx 
  //r_vsize = stats.rx_bytes % 100000;
  
  pthread_mutex_lock(&opaque_lock);
  op.opaque = g_opaque++;
  pthread_mutex_unlock(&opaque_lock);
  
  op.key = string(key);
  op.type = Operation::GET;
  op.hv = hashstr(op.key);
  //item_lock(op.hv,cid);
  op_queue[op.opaque] = op;

  if (read_state == IDLE) read_state = WAITING_FOR_GET;
  l = prot->get_request(key,op.opaque);
  if (read_state != LOADING) stats.tx_bytes += l;
  
  stats.log_access(op);
}

/**
 * Issue a delete90 request to the server.
 */
void Connection::issue_delete90(double now) {
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

  op.type = Operation::DELETE;
  op.opaque = 0;
  op_queue[op.opaque] = op;

  if (read_state == IDLE) read_state = WAITING_FOR_DELETE;
  l = prot->delete90_request();
  if (read_state != LOADING) stats.tx_bytes += l;
}

/**
 * Issue a set request as a result of a miss to the server.
 * The difference here is that we will yield to any outstanding SETs to this
 * key, i.e. while waiting for GET response a SET to the key was issued.
 *
 *
 * or v2?
 *  - works with the lock held, since we want to beat any incoming writes
 *  - maintains program order, total set ordering
 *  - currenlty using this design
 */
void Connection::issue_set_miss(const char* key, const char* value, int length,
                           double now, bool is_access) {
  Operation op;
  int l;

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) op.start_time = get_time();
  else op.start_time = now;
#endif

  //record value size
  //r_vsize = length;
  //r_appid = key[0] - '0';
  //const char* kptr = key;
  //kptr += 2;
  //r_key = atoi(kptr);
  //r_ksize = strlen(kptr);
  
  pthread_mutex_lock(&opaque_lock);
  op.opaque = g_opaque++;
  pthread_mutex_unlock(&opaque_lock);
  op.key = string(key);
  op.valuelen = length;
  op.type = Operation::SET;
  op.hv = hashstr(op.key);
  op_queue[op.opaque] = op;

  //output_op(&op,1,0);

  //if (read_state == IDLE) read_state = WAITING_FOR_SET;
  l = prot->set_request(key, value, length, op.opaque);
  if (read_state != LOADING) stats.tx_bytes += l;

  if (is_access)
      stats.log_access(op);
}

/**
 * Issue a set request to the server.
 */
int Connection::issue_set(const char* key, const char* value, int length,
                           double now, bool is_access) {
  Operation op;
  int l;

#if HAVE_CLOCK_GETTIME
  op.start_time = get_time_accurate();
#else
  if (now == 0.0) op.start_time = get_time();
  else op.start_time = now;
#endif

  //record value size
  //r_vsize = length;
  //r_appid = key[0] - '0';
  //const char* kptr = key;
  //kptr += 2;
  //r_key = atoi(kptr);
  //r_ksize = strlen(kptr);
  
  pthread_mutex_lock(&opaque_lock);
  op.opaque = g_opaque++;
  pthread_mutex_unlock(&opaque_lock);
  op.key = string(key);
  op.valuelen = length;
  op.type = Operation::SET;
  op.hv = hashstr(op.key);
  //pthread_mutex_t *lock = (pthread_mutex_t*)item_trylock(op.hv,cid);
  //if (lock != NULL) {
  //item_lock(op.hv,cid);
    op_queue[op.opaque] = op;

    //output_op(&op,1,0);

    //if (read_state == IDLE) read_state = WAITING_FOR_SET;
    l = prot->set_request(key, value, length, op.opaque);
    if (read_state != LOADING) stats.tx_bytes += l;

    if (is_access)
        stats.log_access(op);
    return 1;
  //} else {
  //  return 0;
  //}
}

/**
 * Return the oldest live operation in progress.
 */
void Connection::pop_op(Operation *op) {

  assert(op_queue.size() > 0);

  //op_queue.pop();
  size_t hv = op->hv;
  //pthread_mutex_t *l = op->lock;
  op_queue.erase(op->opaque);
  
  //item_trylock_unlock(l,cid);
  //item_unlock(hv,cid);

  if (read_state == LOADING) return;
  read_state = IDLE;

  // Advance the read state machine.
  //if (op_queue.size() > 0) {
  //  Operation& op = op_queue.front();
  //  switch (op.type) {
  //  case Operation::GET: read_state = WAITING_FOR_GET; break;
  //  case Operation::SET: read_state = WAITING_FOR_SET; break;
  //  case Operation::DELETE: read_state = WAITING_FOR_DELETE; break;
  //  default: DIE("Not implemented.");
  //  }
  //}
}

/**
 * Finish up (record stats) an operation that just returned from the
 * server.
 */
void Connection::finish_op(Operation *op, int was_hit) {
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

  if (options.successful_queries && was_hit) { 
    switch (op->type) {
    case Operation::GET: stats.log_get(*op); break;
    case Operation::SET: stats.log_set(*op); break;
    case Operation::DELETE: break;
    default: DIE("Not implemented.");
    }
  } else {
    switch (op->type) {
    case Operation::GET: stats.log_get(*op); break;
    case Operation::SET: stats.log_set(*op); break;
    case Operation::DELETE: break;
    default: DIE("Not implemented.");
    }
  }

  last_rx = now;
  op_queue.erase(op->opaque);
  read_state = IDLE;

  //lets check if we should output stats for the window
  //Do the binning for percentile outputs
  //crude at start
  if ((options.misswindow != 0) && ( ((stats.window_accesses) % options.misswindow) == 0))
  {
      if (stats.window_gets != 0)
      {
        //printf("%lu,%.4f\n",(stats.accesses),
        //        ((double)stats.window_get_misses/(double)stats.window_accesses));
        stats.window_gets = 0;
        stats.window_get_misses = 0;
        stats.window_sets = 0;
        stats.window_accesses = 0;
      }
  }

  drive_write_machine();
}

void Connection::finish_op_miss(Operation *op, int was_hit) {
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

  if (options.successful_queries && was_hit) { 
    switch (op->type) {
    case Operation::GET: stats.log_get(*op); break;
    case Operation::SET: stats.log_set(*op); break;
    case Operation::DELETE: break;
    default: DIE("Not implemented.");
    }
  } else {
    switch (op->type) {
    case Operation::GET: stats.log_get(*op); break;
    case Operation::SET: stats.log_set(*op); break;
    case Operation::DELETE: break;
    default: DIE("Not implemented.");
    }
  }

  last_rx = now;
  //we are atomically issuing a set 
  op_queue.erase(op->opaque);
  read_state = IDLE;
  

  //lets check if we should output stats for the window
  //Do the binning for percentile outputs
  //crude at start
  if ((options.misswindow != 0) && ( ((stats.window_accesses) % options.misswindow) == 0))
  {
      if (stats.window_gets != 0)
      {
        //printf("%lu,%.4f\n",(stats.accesses),
        //        ((double)stats.window_get_misses/(double)stats.window_accesses));
        stats.window_gets = 0;
        stats.window_get_misses = 0;
        stats.window_sets = 0;
        stats.window_accesses = 0;
      }
  }
  
  drive_write_machine();

}


/**
 * Check if our testing is done and we should exit.
 */
bool Connection::check_exit_condition(double now) {
  if (read_state == INIT_READ) return false;
  if (now == 0.0) now = get_time();

  if (options.read_file) {
    if (eof) {
        return true;
    }
    else if ((options.queries == 1) && 
        (now > start_time + options.time))
    {
        return true;
    }
    else {
        return false;
    }

  } else {
    if (options.queries != 0 && 
       (((long unsigned)options.queries) == (stats.accesses))) 
    {
        return true;
    }
    if ((options.queries == 0) && 
        (now > start_time + options.time))
    {
        return true;
    }
    if (options.loadonly && read_state == IDLE) return true;
  }

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

    if (!options.no_nodelay && !options.unix_socket) {
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
    //if (err) DIE("DNS error: %s", evutil_gai_strerror(err));
    if (err) fprintf(stderr,"DNS error: %s", evutil_gai_strerror(err));
    fprintf(stderr,"Got an error: %s\n",
        evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

    //stats.print_header();
    //stats.print_stats("read",   stats.get_sampler);
    //stats.print_stats("update", stats.set_sampler);
    //stats.print_stats("op_q",   stats.op_sampler);

    //int total = stats.gets + stats.sets;

    //printf("\nTotal QPS = %.1f (%d / %.1fs)\n",
    //       total / (stats.stop - stats.start),
    //       total, stats.stop - stats.start);


    //printf("\n");

    //printf("Misses = %" PRIu64 " (%.1f%%)\n", stats.get_misses,
    //       (double) stats.get_misses/stats.gets*100);

    //printf("Skipped TXs = %" PRIu64 " (%.1f%%)\n\n", stats.skips,
    //       (double) stats.skips / total * 100);

    //printf("RX %10" PRIu64 " bytes : %6.1f MB/s\n",
    //       stats.rx_bytes,
    //       (double) stats.rx_bytes / 1024 / 1024 / (stats.stop - stats.start));
    //printf("TX %10" PRIu64 " bytes : %6.1f MB/s\n",
    //       stats.tx_bytes,
    //       (double) stats.tx_bytes / 1024 / 1024 / (stats.stop - stats.start));
    
    //if (
    //DIE("BEV_EVENT_ERROR: %s", strerror(errno));

  } else if (events & BEV_EVENT_EOF) {
    //stats.print_header();
    //stats.print_stats("read",   stats.get_sampler);
    //stats.print_stats("update", stats.set_sampler);
    //stats.print_stats("op_q",   stats.op_sampler);

    //int total = stats.gets + stats.sets;

    //printf("\nTotal QPS = %.1f (%d / %.1fs)\n",
    //       total / (stats.stop - stats.start),
    //       total, stats.stop - stats.start);


    //printf("\n");

    //printf("Misses = %" PRIu64 " (%.1f%%)\n", stats.get_misses,
    //       (double) stats.get_misses/stats.gets*100);

    //printf("Skipped TXs = %" PRIu64 " (%.1f%%)\n\n", stats.skips,
    //       (double) stats.skips / total * 100);

    //printf("RX %10" PRIu64 " bytes : %6.1f MB/s\n",
    //       stats.rx_bytes,
    //       (double) stats.rx_bytes / 1024 / 1024 / (stats.stop - stats.start));
    //printf("TX %10" PRIu64 " bytes : %6.1f MB/s\n",
    //       stats.tx_bytes,
    //       (double) stats.tx_bytes / 1024 / 1024 / (stats.stop - stats.start));

    //DIE("Unexpected EOF from server.");
    fprintf(stderr,"Unexpected EOF from server.");
    return;
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

  if (check_exit_condition(now)) {
      return;
  }

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
      }
      //uncommenting for lowest delay possible
      //if (op_queue.size() >= (size_t) options.depth) {
      //  write_state = WAITING_FOR_OPQ;
      //  return;
      //} else if (now < next_time) {
      //  write_state = WAITING_FOR_TIME;
      //  break; // We want to run through the state machine one more time
      //         // to make sure the timer is armed.
      //} else if (options.moderate && now < last_rx + 0.00025) {
      //  write_state = WAITING_FOR_TIME;
      //  if (!event_pending(timer, EV_TIMEOUT, NULL)) {
      //    delay = last_rx + 0.00025 - now;
      //    double_to_tv(delay, &tv);
      //    evtimer_add(timer, &tv);
      //  }
      //  return;
      //}

      if (options.getsetorset) {
        int ret = issue_getsetorset(now);
        if (ret) return; //if at EOF
      } else {
        issue_something(now);
      }
      
      last_tx = now;
      stats.log_op(op_queue.size());
      //next_time += iagen->generate();

      //if (options.skip && options.lambda > 0.0 &&
      //    now - next_time > 0.005000 &&
      //    op_queue.size() >= (size_t) options.depth) {

      //  while (next_time < now - 0.004000) {
      //    stats.skips++;
      //    next_time += iagen->generate();
      //  }
      //}
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
  bool done, found;

  //initially assume found (for sets that may come through here)
  //is this correct? do we want to assume true in case that 
  //GET was found, but wrong value size (i.e. update value)
  //
  found = true;

  if (op_queue.size() == 0) V("Spurious read callback.");

  while (1) {
    
    if (read_state == CONN_SETUP) {
      assert(options.binary);
      if (!prot->setup_connection_r(input)) return;
      read_state = IDLE;
      break;
    }
      
    int obj_size;
    uint32_t opaque;
    bool full_read = prot->handle_response(input, done, found, obj_size, opaque);
    if (full_read) {
        op = &op_queue[opaque];
        //char out[128];
        //sprintf(out,"conn: %u, reading opaque: %u\n",cid,opaque);
        //write(2,out,strlen(out));
        //output_op(op,2,found);
    } else {
        return;
    }
    

    switch (op->type) {
        case Operation::GET:
            if (done) {
                if ((!found && options.getset) || 
                    (!found && options.getsetorset)) {
                    char key[256];
                    string keystr = op->key;
                    strcpy(key, keystr.c_str());
                    int valuelen = op->valuelen;
                
                    //if not found and in getset mode, issue set
                    if (options.read_file) {
                        int index = lrand48() % (1024 * 1024);
                        issue_set_miss(key, &random_char[index], valuelen);
                    }
                    else {
                        int index = lrand48() % (1024 * 1024);
                        issue_set_miss(key, &random_char[index], valuelen);
                    }
                    finish_op(op,0); // sets read_state = IDLE
                    
                } else {
                    if (found) {
                        finish_op(op,1);
                    } else {
                        finish_op(op,0);
                    }
                }
            } else {
                char out[128];
                sprintf(out,"conn: %u, not done reading, should do something",cid);
                write(2,out,strlen(out));
            }
            break;
        case Operation::SET:
            finish_op(op,1);
            break;
        default: DIE("not implemented");
    }


    //switch (read_state) {
    //case INIT_READ: DIE("event from uninitialized connection");
    //case IDLE: return;  // We munched all the data we expected?

    //case WAITING_FOR_GET:
    //  assert(op_queue.size() > 0);
    //  if (!full_read) {
    //    return;
    //  } else if (done) {

    //    
    //    if ((!found && options.getset) || 
    //        (!found && options.getsetorset)) {
    //        char key[256];
    //        string keystr = op->key;
    //        strcpy(key, keystr.c_str());
    //        int valuelen = op->valuelen;
    //    
    //        finish_op(op,0); // sets read_state = IDLE
    //        //if not found and in getset mode, issue set
    //        if (options.read_file) {
    //            int index = lrand48() % (1024 * 1024);
    //            issue_set(key, &random_char[index], valuelen);
    //        }
    //        else {
    //            int index = lrand48() % (1024 * 1024);
    //            issue_set(key, &random_char[index], valuelen);
    //        }
    //    } else {
    //        if (!found) {
    //            finish_op(op,1);
    //        } else {
    //            finish_op(op,0);
    //        }
    //    }


    //    //char log[256];
    //    //sprintf(log,"%f,%d,%d,%d,%d,%d,%d\n",
    //    //        r_time,r_appid,r_type,r_ksize,r_vsize,r_key,r_hit);
    //    //write(2,log,strlen(log));
    //    

    //  }
    //  break;

    //case WAITING_FOR_SET:
    //  
    //  assert(op_queue.size() > 0);
    //  //full_read = prot->handle_response(input, done, found, obj_size, opaque);
    //  if (!full_read) {

    //    //char key[256];
    //    //char log[1024];
    //    //string keystr = op->key;
    //    //strcpy(key, keystr.c_str());
    //    //int valuelen = op->valuelen;
    //    //sprintf(log,"ERROR SETTING: %s,%d\n",key,valuelen);
    //    //write(2,log,strlen(log));
    //    return; 
    //  }
    //  
    //  finish_op(op,1);
    //  break;
    //
    //case WAITING_FOR_DELETE:
    //  if (!full_read) return;
    //  finish_op(op,1);
    //  break;

    //case LOADING:
    //  assert(op_queue.size() > 0);
    //  if (!full_read) return;
    //  loader_completed++;
    //  pop_op(op);

    //  if (loader_completed == options.records) {
    //    D("Finished loading.");
    //    read_state = IDLE;
    //  } else {
    //    while (loader_issued < loader_completed + LOADER_CHUNK) {
    //      if (loader_issued >= options.records) break;

    //      char key[256];
    //      string keystr = keygen->generate(loader_issued);
    //      strcpy(key, keystr.c_str());
    //      int index = lrand48() % (1024 * 1024);
    //      issue_set(key, &random_char[index], valuesize->generate());

    //      loader_issued++;
    //    }
    //  }

    //  break;

    //case CONN_SETUP:
    //  assert(options.binary);
    //  if (!prot->setup_connection_r(input)) return;
    //  read_state = IDLE;
    //  break;

    //default: DIE("not implemented");
    //}
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

