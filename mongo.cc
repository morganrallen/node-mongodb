#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <v8.h>
#include <node.h>
#include <node_events.h>
#include <fcntl.h>

extern "C" {
    #define MONGO_HAVE_STDINT
    #include <bson.h>
    #include <mongo.h>
    #include <platform_hacks.h>
}

#define NS "test.widgets"

const int chunk_size = 4094;
using namespace v8;
extern Local<Value> decodeObjectStr(const char *);
enum ReadState {
    STATE_READ_HEAD,
    STATE_READ_FIELDS,
    STATE_READ_MESSAGE,
    STATE_PARSE_MESSAGE,
};

void node_mongo_find(mongo_connection* conn, const char* ns, bson* query, bson* fields, int nToReturn, int nToSkip, int options) {
    int sl;
    char * data;
    mongo_message * mm = mongo_message_create( 16 + /* header */
                                               4 + /*  options */
                                               strlen( ns ) + 1 + /* ns */
                                               4 + 4 + /* skip,return */
                                               bson_size( query ) +
                                               bson_size( fields ) ,
                                               0 , 0 , mongo_op_query );

    data = &mm->data;
    data = mongo_data_append32( data , &options );
    data = mongo_data_append( data , ns , strlen( ns ) + 1 );
    data = mongo_data_append32( data , &nToSkip );
    data = mongo_data_append32( data , &nToReturn );
    data = mongo_data_append( data , query->data , bson_size( query ) );
    if ( fields )
        data = mongo_data_append( data , fields->data , bson_size( fields ) );

    bson_fatal_msg( (data == ((char*)mm) + mm->head.len), "query building fail!" );

    mongo_message_send( conn , mm );
}

class Connection : public node::EventEmitter {
    public:

    static void
    Initialize (Handle<Object> target) {
        HandleScope scope;

        Local<FunctionTemplate> t = FunctionTemplate::New(New);

        t->Inherit(EventEmitter::constructor_template);
        t->InstanceTemplate()->SetInternalFieldCount(1);

        NODE_SET_PROTOTYPE_METHOD(t, "connect", Connect);
        NODE_SET_PROTOTYPE_METHOD(t, "find", Find);

        target->Set(String::NewSymbol("Connection"), t->GetFunction());
    }

    bool
    Connect(const char *host, const int32_t port) {
        mongo_connection_options opts;
        memcpy(opts.host, host, strlen(host)+1);
        opts.host[254] = '\0';
        opts.port = port;

        printf("connecting! %s %d\n", host, port);

        if (mongo_connect(conn, &opts)) {
            return false;
        }

        // enable non-blocking mode
        int sockflags = fcntl(conn->sock, F_GETFL, 0);
        fcntl(conn->sock, F_SETFL, sockflags | O_NONBLOCK);

        ev_io_set(&read_watcher,  conn->sock, EV_READ);
        ev_io_set(&write_watcher, conn->sock, EV_WRITE);

        ev_io_start(EV_DEFAULT_ &write_watcher);

        Attach();

        return true;
    }

    void
    CheckBufferContents(void) {
        if (state == STATE_READ_HEAD) {
            if (buflen > sizeof(mongo_header)) {
                printf("got enough for the head\n");
                memcpy(&head, bufptr, sizeof(mongo_header));
                printf("memcpy'd\n");
                bufptr += sizeof(mongo_header);
                state = STATE_READ_FIELDS;
            }
        }
        if (state == STATE_READ_FIELDS) {
            if (buflen > sizeof(mongo_header) + sizeof(mongo_reply_fields)) {
                printf("got enough for the fields\n");
                memcpy(&fields, bufptr, sizeof(mongo_reply_fields));
                bufptr += sizeof(mongo_reply_fields);
                state = STATE_READ_MESSAGE;
            }
        }
        if (state == STATE_READ_MESSAGE) {
            printf("in read message\n");
            int len;
            bson_little_endian32(&len, &head.len);

            if (len-buflen == 0) {
                printf("its at zero!\n");
                state = STATE_PARSE_MESSAGE;
            }
        }
        if (state == STATE_PARSE_MESSAGE) {
            ParseMessage();
            delete [] buf;
            buf = bufptr = NULL;
            buflen = 0;

            state = STATE_READ_HEAD;
            ev_io_stop(&read_watcher);
            ev_io_start(&write_watcher);
            printf("listening for input again\n");
        }
    }

    bool
    SendGetMore(void) {
        HandleScope scope;
        if (cursor->mm && cursor->mm->fields.cursorID){
            char* data;
            int sl = strlen(cursor->ns)+1;
            mongo_message * mm = mongo_message_create(16 /*header*/
                                                     +4 /*ZERO*/
                                                     +sl
                                                     +4 /*numToReturn*/
                                                     +8 /*cursorID*/
                                                     , 0, 0, mongo_op_get_more);
            data = &mm->data;
            int zero = 0;
            data = mongo_data_append32(data, &zero);
            data = mongo_data_append(data, cursor->ns, sl);
            data = mongo_data_append32(data, &zero);
            data = mongo_data_append64(data, &cursor->mm->fields.cursorID);
            mongo_message_send(conn, mm);
            state = STATE_READ_HEAD;

            ev_io_start(&read_watcher);
            ev_io_stop(&write_watcher);
            return true;
        } else {
            printf("getting here\n");
            delete [] cursor->ns;
            free(cursor);
            printf("cursor was %p\n", cursor);
            Emit("result", 1, reinterpret_cast<Handle<Value> *>(&results));
            get_more = false;
            printf("didn't have to sendmore!!!\n");
            return false;
        }
    }

    bool
    GetMore(void) {
        free(cursor->mm);

        MONGO_TRY{
            cursor->mm = mongo_read_response(cursor->conn);
        }MONGO_CATCH{
            cursor->mm = NULL;
            mongo_cursor_destroy(cursor);
            MONGO_RETHROW();
        }

        return cursor->mm && cursor->mm->fields.num;
    }

    void
    ParseMessage(void) {
        HandleScope scope;
        printf("in parse message\n");

        int len;
        bson_little_endian32(&len, &head.len);

        mongo_reply *out = reinterpret_cast<mongo_reply*>(new char[len]);

        out->head.len = len;
        bson_little_endian32(&out->head.id, &head.id);
        bson_little_endian32(&out->head.responseTo, &head.responseTo);
        bson_little_endian32(&out->head.op, &head.op);

        bson_little_endian32(&out->fields.flag, &fields.flag);
        bson_little_endian64(&out->fields.cursorID, &fields.cursorID);
        bson_little_endian32(&out->fields.start, &fields.start);
        bson_little_endian32(&out->fields.num, &fields.num);

        printf("num = %d start = %d\n", fields.num, fields.start);
        printf("num = %d start = %d\n", fields.num, fields.start);

        memcpy(&out->objs, bufptr, len-sizeof(head)-sizeof(fields));

        cursor = static_cast<mongo_cursor*>(bson_malloc(sizeof(mongo_cursor)));

        ParseReply(out);
        delete out;
    }

    void
    ParseReply(mongo_reply *out) {
        HandleScope scope;
        printf("parsing reply\n");

        cursor->mm = out;

        int sl = strlen(NS)+1;
        cursor->ns = static_cast<char *>(new char[sl]);

        memcpy(static_cast<void*>(const_cast<char*>(cursor->ns)), NS, sl);
        cursor->conn = conn;
        cursor->current.data = NULL;

        for (int i = results->Length(); AdvanceCursor(); i++){
            Local<Value> val = decodeObjectStr(cursor->current.data);
            results->Set(Integer::New(i), val);
        }

        ev_io_stop(&read_watcher);
        ev_io_start(&write_watcher);
        printf("end of readresponse\n");

        return;
    }

    bool AdvanceCursor(void) {
        char* bson_addr;

        /* no data */
        if (!cursor->mm || cursor->mm->fields.num == 0)
            return false;

        /* first */
        if (cursor->current.data == NULL){
            bson_init(&cursor->current, &cursor->mm->objs, 0);
            return true;
        }

        bson_addr = cursor->current.data + bson_size(&cursor->current);
        if (bson_addr >= ((char*)cursor->mm + cursor->mm->head.len)){
            printf("i should be getting more here\n");
            get_more = true;

            // indicate that this is the last result
            return false;
        } else {
            printf("advancing cursor by one object\n");
            bson_init(&cursor->current, bson_addr, 0);
        }
    }

    bool ConsumeInput(void) {
        char *tmp;
        char readbuf[chunk_size];
        int32_t readbuflen;

        while (true) {
            readbuflen = read(conn->sock, readbuf, chunk_size);

            // no more input to consume
            if (readbuflen == -1 && errno == EAGAIN) {
                printf("len == -1 && errno == EAGAIN\n");
            }
            else if (readbuflen <= 0) {
                printf("length error on read %d errno = %d\n", readbuflen, errno);
            }
            else {
                printf("buf is %d bytes\n", buflen);
                printf("read %d bytes\n", readbuflen);
                tmp = static_cast<char *>(new char[buflen+readbuflen]);
                memset(tmp, 0, buflen+readbuflen);

                if (buf) {
                    memcpy(tmp, buf, buflen);
                }
                memcpy(tmp+buflen, readbuf, readbuflen);
                if (buf) {
                    printf("deleting old buf\n");
                    delete [] buf;
                }
                buflen = buflen + readbuflen;
                bufptr = tmp + (bufptr - buf);
                buf = tmp;
                printf("buf is %d bytes\n\n", buflen);
                break;
            }
        }
    }

    bool Find(void) {
        bson query;
        bson_empty(&query);

        node_mongo_find(conn, "test.widgets", &query, 0, 0, 0, 0);
        ev_io_start(&read_watcher);
    }

    protected:

    static Handle<Value>
    New (const Arguments& args) {
        HandleScope scope;

        Connection *connection = new Connection();
        connection->Wrap(args.This());
        return args.This();
    }

    Connection () : EventEmitter () {
        HandleScope scope;
        Handle<Array> r = Array::New();
        results = Persistent<Array>::New(r);

        get_more = false;
        buflen = 0;
        buf = bufptr = NULL;
        state = STATE_READ_HEAD;

        ev_init(&read_watcher, io_event);
        read_watcher.data = this;
        ev_init(&write_watcher, io_event);
        write_watcher.data = this;
    }

    static Handle<Value>
    Connect (const Arguments &args) {
        Connection *connection = ObjectWrap::Unwrap<Connection>(args.This());
        HandleScope scope;
        String::Utf8Value host(args[0]->ToString());
        bool r = connection->Connect(*host, args[1]->Int32Value());

        return Undefined();
    }

    static Handle<Value>
    Find (const Arguments &args) {
        Connection *connection = ObjectWrap::Unwrap<Connection>(args.This());
        HandleScope scope;

        connection->Find();
    }

    void Event(ev_io *w, int revents) {
        if (revents & EV_WRITE) {
            printf("-- got a write event\n");
            ev_io_stop(&write_watcher);
            if (get_more) {
                SendGetMore();
            }
        }
        if (revents & EV_READ) {
            printf("-- got a read event\n");
            ConsumeInput();
            CheckBufferContents();
        }
        if (revents & EV_ERROR) {
            printf("--got an error event\n");
        }
    }

    private:

    static void
    io_event (EV_P_ ev_io *w, int revents) {
        Connection *connection = static_cast<Connection *>(w->data);
        connection->Event(w, revents);
    }

    mongo_connection conn[1];

    // states
    bool get_more;
    ReadState state;

    mongo_header head;
    mongo_reply_fields fields;
    mongo_cursor *cursor;

    Persistent<Array> results;

    char *buf;
    char *bufptr;
    int32_t buflen;

    ev_io read_watcher;
    ev_io write_watcher;
};

extern "C" void
init (Handle<Object> target) {
    HandleScope scope;
    Connection::Initialize(target);
}