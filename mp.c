#include <stdio.h>
#include <math.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <arpa/inet.h>


#include "lua.h"
#include "lauxlib.h"

#define ntohll(x) ( ( (uint64_t)(ntohl( (uint32_t)((x << 32) >> 32) )) << 32) | ntohl( ((uint32_t)(x >> 32)) ) )

#define htonll(x) ntohll(x)
    

typedef struct {
    unsigned char data[1024*1024];
    size_t used; // start from zero
    size_t capacity;
    int err;
} mpwbuf_t;

typedef struct {
    unsigned char *data; // read buffer dont have to allocate buffer.
    size_t ofs;
    size_t len;
    int err;
} mprbuf_t;

mpwbuf_t g_mpwbuf; // single threaded! 

void mpwbuf_init(mpwbuf_t *b){
    b->used=0;
    b->capacity = sizeof(b->data);
    b->err=0;
}
void mprbuf_init(mprbuf_t *b, const unsigned char *p, size_t len ){
    b->data = (unsigned char*)p;
    b->len = len;
    b->ofs=0;
    b->err=0;
}

size_t mpwbuf_left(mpwbuf_t *b){
    return b->capacity - b->used;
}
size_t mprbuf_left(mprbuf_t *b){
    return b->len - b->ofs;
}

void mp_rcopy( unsigned char *dest, unsigned char*from, size_t l ){
    size_t i;
    for(i=0;i<l;i++){
        dest[l-i-1]=from[i];
    }
}

size_t mpwbuf_append(mpwbuf_t *b, const unsigned char *toadd, size_t l){
    if(b->err){return 0;}
    if( mpwbuf_left(b)<l){
        b->err |= 1;
        return 0;
    }
    memcpy( b->data + b->used, toadd,l);
    b->used += l;
    return l;
}
// return packed size. 0 when error.
static size_t mpwbuf_pack_nil( mpwbuf_t *b ){
    unsigned char append[1]={ 0xc0 };    
    return mpwbuf_append(b,append,1);
}
static size_t mpwbuf_pack_boolean( mpwbuf_t *b, int i) {
    unsigned char append[1];
    if(i){
        append[0] = 0xc3;
    } else {
        append[0] = 0xc2;
    }
    return mpwbuf_append(b,append,1);
}
static size_t mpwbuf_pack_number( mpwbuf_t *b, lua_Number n ) {
    unsigned char buf[1+8];
    size_t len=0;
    if(floor(n)==n){
        long long lv = (long long)n;
        if(lv>=0){
            if(lv<128){
                buf[0]=(char)lv;
                len=1;
            } else if(lv<256){
                buf[0] = 0xcc;
                buf[1] = (char)lv;
                len=2;
            } else if(lv<65536){
                buf[0] = 0xcd;
                short v = htons((short)lv);
                memcpy(buf+1,&v,2);
                len=1+2;
            } else if(lv<4294967296LL){
                buf[0] = 0xce;
                long v = htonl((long)lv);
                memcpy(buf+1,&v,4);
                len=1+4;
            } else {
                buf[0] = 0xcf;
                long long v = htonll((long long)lv);
                memcpy(buf+1,&v,8);
                len=1+8;
            }
        } else {
            if(lv >= -32){
                buf[0] = 0xe0 | (char)lv;
                len=1;
            } else if( lv >= -128 ){
                buf[0] = 0xd0;
                buf[1] = lv;
                len=2;
            } else if( lv >= -32768 ){
                short v = htons(lv&0xffff);
                buf[0] = 0xd1;
                memcpy(buf+1, &v,2);
                len=1+2;
            } else if( lv >= -2147483648LL ){
                long v = htonl(lv&0xffffffff);
                buf[0] = 0xd2;
                memcpy(buf+1,&v,4);
                len=1+4;
            } else{
                long long v = htonll(lv);
                buf[0] = 0xd3;
                memcpy(buf+1,&v,8);
                len=1+8;
            }
        }
    } else { // floating point!
        assert(sizeof(double)==sizeof(n));
        buf[0] = 0xcb;
        mp_rcopy(buf+1,(unsigned char*)&n,sizeof(double)); // endianness
        len=1+8;
    }
    return mpwbuf_append(b,buf,len);
}
static size_t mpwbuf_pack_string( mpwbuf_t *b, const unsigned char *sval, size_t slen ) {
    unsigned char topbyte=0;
    size_t wl=0;
    if(slen<32){
        topbyte = 0xa0 | (char)slen;
        wl = mpwbuf_append(b, &topbyte, 1 );
        wl += mpwbuf_append(b, sval, slen);
    } else if(slen<65536){
        topbyte = 0xda;
        wl = mpwbuf_append(b,&topbyte,1);
        unsigned short l = htons(slen);
        wl += mpwbuf_append(b,(unsigned char*)(&l),2);
        wl += mpwbuf_append(b,sval,slen);
    } else if(slen<4294967296LL-1){ // TODO: -1 for avoiding (condition is always true warning)
        topbyte = 0xdb;
        wl = mpwbuf_append(b,&topbyte,1);
        unsigned int l = htonl(slen);
        wl += mpwbuf_append(b,(unsigned char*)(&l),4);
        wl += mpwbuf_append(b,sval,slen);
    } else {
        b->err |= 1;
    }
    return wl;    
}

static size_t mpwbuf_pack_anytype( mpwbuf_t *b, lua_State *L, int index ) ;


// from mplua:  void packTable(Packer& pk, int index) const {
// check if this is an array
// NOTE: This code strongly depends on the internal implementation
// of Lua5.1. The table in Lua5.1 consists of two parts: the array part
// and the hash part. The array part is placed before the hash part.
// Therefore, it is possible to obtain the first key of the hash part
// by using the return value of lua_objlen as the argument of lua_next.
// If lua_next return 0, it means the table does not have the hash part,
// that is, the table is an array.
//
// Due to the specification of Lua, the table with non-continous integral
// keys is detected as a table, not an array.

// index: index in stack
static size_t mpwbuf_pack_table( mpwbuf_t *b, lua_State *L, int index ) {
    size_t nstack = lua_gettop(L);
    size_t l = lua_objlen(L,index);
    
    size_t wl=0;
    //    fprintf(stderr, "mpwbuf_pack_table: lua_objlen: array part len: %d index:%d\n", (int)l,index);
    
    // try array first, and then map.
    if(l>0){
        unsigned char topbyte;
        // array!(ignore map part.) 0x90|n , 0xdc+2byte, 0xdd+4byte
        if(l<16){
            topbyte = 0x90 | (unsigned char)l;
            wl = mpwbuf_append(b,&topbyte,1);
        } else if( l<65536){
            topbyte = 0xdc;
            wl = mpwbuf_append(b,&topbyte,1);
            unsigned short elemnum = htons(l);
            wl += mpwbuf_append(b,(unsigned char*)&elemnum,2);
        } else if( l<4294967296LL-1){ // TODO: avoid C warn
            topbyte = 0xdd;
            wl = mpwbuf_append(b,&topbyte,1);
            unsigned int elemnum = htonl(l);
            wl += mpwbuf_append(b,(unsigned char*)&elemnum,4);
        }
        
        int i;
        for(i=1;i<=(int)l;i++){
            lua_rawgeti(L,index,i); // push table value to stack
            wl += mpwbuf_pack_anytype(b,L,nstack+1);
            lua_pop(L,1); // repair stack
        }
    } else {
        // map!
        l=0;
        lua_pushnil(L);
        while(lua_next(L,index)){
            l++;
            lua_pop(L,1);
        }
        // map fixmap, 16,32 : 0x80|num, 0xde+2byte, 0xdf+4byte
        unsigned char topbyte=0;
        if(l<16){
            topbyte = 0x80 | (char)l;
            wl = mpwbuf_append(b,&topbyte,1);
        }else if(l<65536){
            topbyte = 0xde;
            wl = mpwbuf_append(b,&topbyte,1);
            unsigned short elemnum = htons(l);
            wl += mpwbuf_append(b,(unsigned char*)&elemnum,2);
        }else if(l<4294967296LL-1){
            topbyte = 0xdf;
            wl = mpwbuf_append(b,&topbyte,1);
            unsigned int elemnum = htonl(l);
            wl += mpwbuf_append(b,(unsigned char*)&elemnum,4);
        }
        lua_pushnil(L); // nil for first iteration on lua_next
        while( lua_next(L,index)){
            wl += mpwbuf_pack_anytype(b,L,nstack+1); // -2:key
            wl += mpwbuf_pack_anytype(b,L,nstack+2); // -1:value
            lua_pop(L,1); // remove value and keep key for next iteration
        }
    }
    return wl;
}

static size_t mpwbuf_pack_anytype( mpwbuf_t *b, lua_State *L, int index ) {
    //    int top = lua_gettop(L); 
    //    fprintf(stderr, "mpwbuf_pack_anytype: top:%d index:%d\n", top, index);
    int t = lua_type(L,index);
    switch(t){
    case LUA_TNIL:
        return mpwbuf_pack_nil(&g_mpwbuf);
    case LUA_TBOOLEAN:
        {
            int iv = lua_toboolean(L,index);
            return mpwbuf_pack_boolean(&g_mpwbuf,iv);
        }
    case LUA_TNUMBER:
        {
            lua_Number nv = lua_tonumber(L,index);
            return mpwbuf_pack_number(&g_mpwbuf,nv);
        }
        break;
    case LUA_TSTRING:
        {
            size_t slen;
            const char *sval = luaL_checklstring(L,index,&slen);
            return mpwbuf_pack_string(&g_mpwbuf,(const unsigned char*)sval,slen);
        }
        break;
    case LUA_TTABLE:
        return mpwbuf_pack_table(&g_mpwbuf, L, index );
    case LUA_TLIGHTUSERDATA:
    case LUA_TFUNCTION:
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
        break;
    }
    b->err |= 1;
    return 0;    
}

// return num of return value
static int msgpack_pack_api( lua_State *L ) {
    mpwbuf_init( &g_mpwbuf);

    size_t wlen = mpwbuf_pack_anytype(&g_mpwbuf,L,1);
    if(wlen>0 && g_mpwbuf.err == 0 ){
        lua_pushlstring(L,(const char*)g_mpwbuf.data,g_mpwbuf.used);
        return 1;
    } else {
        lua_pushfstring( L, "msgpack_pack failed. error code:%d", g_mpwbuf.err );
        lua_error(L);
        return 2;
    }
}

// push a table
static void mprbuf_unpack_anytype( mprbuf_t *b, lua_State *L );
static void mprbuf_unpack_array( mprbuf_t *b, lua_State *L, int arylen ) {
    //    lua_newtable(L);
    lua_createtable(L,arylen,0);
    int i;
    for(i=0;i<arylen;i++){
        mprbuf_unpack_anytype(b,L); // array element
        if(b->err)break;
        lua_rawseti(L, -2, i+1);
    }
}
static void mprbuf_unpack_map( mprbuf_t *b, lua_State *L, int maplen ) {
    // return a table
    //    lua_newtable(L); // push {}
    lua_createtable(L,0,maplen);
    int i;
    for(i=0;i<maplen;i++){
        mprbuf_unpack_anytype(b,L); // key
        mprbuf_unpack_anytype(b,L); // value
        lua_rawset(L,-3);
    }
}

static void mprbuf_unpack_anytype( mprbuf_t *b, lua_State *L ) {
    if( mprbuf_left(b) < 1){
        b->err |= 1;
        return;
    }
    unsigned char t = b->data[ b->ofs ];
    //        fprintf( stderr, "mprbuf_unpack_anytype: topbyte:%x ofs:%d len:%d\n",(int)t, (int)b->ofs, (int)b->len );

    b->ofs += 1; // for toptypebyte
    
    if(t<0x80){ // fixed num
        lua_pushnumber(L,(lua_Number)t);
        return;
    }

    unsigned char *s = b->data + b->ofs;
    
    if(t>=0x80 && t <=0x8f){ // fixed map
        size_t maplen = t & 0xf;
        mprbuf_unpack_map(b,L,maplen);
        return;
    }
    if(t>=0x90 && t <=0x9f){ // fixed array
        size_t arylen = t & 0xf;
        mprbuf_unpack_array(b,L,arylen);
        return;
    }

    if(t>=0xa0 && t<=0xbf){ // fixed string
        size_t slen = t & 0x1f;
        if( mprbuf_left(b) < slen ){
            b->err |= 1;
            return;
        }
        lua_pushlstring(L,(const char*)s,slen);
        b->ofs += slen;
        return;
    }
    if(t>0xdf){ // fixnum_neg (-32 ~ -1)
        unsigned char ut = t;
        lua_Number n = ( 256 - ut ) * -1;
        lua_pushnumber(L,n);
        return;
    }
    
    switch(t){
    case 0xc0: // nil
        lua_pushnil(L);
        return;
    case 0xc2: // false
        lua_pushboolean(L,0);
        return;
    case 0xc3: // true
        lua_pushboolean(L,1);
        return;

    case 0xca: // float
        if(mprbuf_left(b)>=4){
            float f;
            mp_rcopy( (unsigned char*)(&f), s,4); // endianness
            lua_pushnumber(L,f);
            b->ofs += 4;
            return;
        }
        break;
    case 0xcb: // double
        if(mprbuf_left(b)>=8){
            double v;
            mp_rcopy( (unsigned char*)(&v), s,8); // endianness
            lua_pushnumber(L,v);
            b->ofs += 8;
            return;
        }
        break;
        
    case 0xcc: // 8bit large posi int
        if(mprbuf_left(b)>=1){
            lua_pushnumber(L,(unsigned char) s[0] );
            b->ofs += 1;
            return;
        }
        break;
    case 0xcd: // 16bit posi int
        if(mprbuf_left(b)>=2){
            unsigned short v = ntohs( *(short*)(s) );
            lua_pushnumber(L,v);
            b->ofs += 2;
            return;
        }
        break;
    case 0xce: // 32bit posi int
        if(mprbuf_left(b)>=4){
            unsigned long v = ntohl( *(long*)(s) );
            lua_pushnumber(L,v);
            b->ofs += 4;
            return;
        }
        break;
    case 0xcf: // 64bit posi int
        if(mprbuf_left(b)>=8){
            unsigned long long v = ntohll( *(long long*)(s));
            lua_pushnumber(L,v);
            b->ofs += 8;
            return;
        }
        break;
    case 0xd0: // 8bit neg int
        if(mprbuf_left(b)>=1){
            lua_pushnumber(L, (char) s[0] );
            b->ofs += 1;
            return;
        }
        break;
    case 0xd1: // 16bit neg int
        if(mprbuf_left(b)>=2){
            short v = *(short*)(s);
            v = ntohs(v);
            lua_pushnumber(L,v);
            b->ofs += 2;
            return;
        }
        break;
    case 0xd2: // 32bit neg int
        if(mprbuf_left(b)>=4){
            long v = *(long*)(s);
            v = ntohl(v);
            lua_pushnumber(L,v);
            b->ofs += 4;
            return;
        }
        break;
    case 0xd3: // 64bit neg int
        if(mprbuf_left(b)>=8){
            long long v = *(long long*)(s);
            v = ntohll(v);
            lua_pushnumber(L,v);
            b->ofs += 8;
            return;
        }
        break;
    case 0xda: // long string len<65536
        if(mprbuf_left(b)>=2){
            size_t slen = ntohs(*((unsigned short*)(s)));
            b->ofs += 2;
            if(mprbuf_left(b)>=slen){
                lua_pushlstring(L,(const char*)b->data+b->ofs,slen);
                b->ofs += slen;
                return;
            }
        }
        break;
    case 0xdb: // longer string
        if(mprbuf_left(b)>=4){
            size_t slen = ntohl(*((unsigned int*)(s)));
            b->ofs += 4;
            if(mprbuf_left(b)>=slen){
                lua_pushlstring(L,(const char*)b->data+b->ofs,slen);
                b->ofs += slen;
                return;
            }
        }

        break;

    case 0xdc: // ary16
        if(mprbuf_left(b)>=2){
            unsigned short elemnum = ntohs( *((unsigned short*)(b->data+b->ofs) ) );
            b->ofs += 2;
            mprbuf_unpack_array(b,L,elemnum);
            return;
        }
        break;
    case 0xdd: // ary32
        if(mprbuf_left(b)>=4){
            unsigned int elemnum = ntohl( *((unsigned int*)(b->data+b->ofs)));
            b->ofs += 4;
            mprbuf_unpack_array(b,L,elemnum);
            return;
        }
        break;
    case 0xde: // map16
        if(mprbuf_left(b)>=2){
            unsigned short elemnum = ntohs( *((unsigned short*)(b->data+b->ofs)));
            b->ofs += 2;
            mprbuf_unpack_map(b,L,elemnum);
            return;
        }
        break;
    case 0xdf: // map32
        if(mprbuf_left(b)>=4){
            unsigned int elemnum = ntohl( *((unsigned int*)(b->data+b->ofs)));
            b->ofs += 4;
            mprbuf_unpack_map(b,L,elemnum);
            return;
        }
        break;
    default:
        break;
    }
    b->err |= 1;
}


static int msgpack_unpack_api( lua_State *L ) {
    size_t len;
    const char * s = luaL_checklstring(L,1,&len);
    if(!s){
        lua_pushstring(L,"arg must be a string");
        lua_error(L);
        return 2;
    }
    if(len==0){
        lua_pushnil(L);
        return 1;
    }

    mprbuf_t rb;
    mprbuf_init(&rb,(const unsigned char*)s,len);

    lua_pushnumber(L,-123456); // push readlen and replace it later
    mprbuf_unpack_anytype(&rb,L);
    //    fprintf(stderr, "mprbuf_unpack_anytype: ofs:%d len:%d err:%d\n", (int)rb.ofs, (int)rb.len, rb.err );
    
    if( rb.ofs >0 && rb.err==0){
        lua_pushnumber(L,rb.ofs);
        lua_replace(L,-3); // replace dummy len
        //        fprintf(stderr, "msgpack_unpack_api: unpacked len: %d\n", (int)rb.ofs );
        return 2;
    } else{
        //        lua_pushfstring(L,"msgpack_unpack_api: unsupported type or buffer short. error code: %d\n", rb.err );
        //        lua_error(L);
        lua_pushnil(L);
        lua_replace(L,-3);
        lua_pushnil(L);
        lua_replace(L,-2);        
        return 2;        
    }
}        


static int msgpack_largetbl( lua_State *L ) {
    int n = luaL_checkint(L,1);
    lua_createtable(L,n,0);
    int i;
    for(i=0;i<n;i++){
        lua_pushnumber(L,i);
        lua_rawseti(L,-2,i+1);
    }
    return 1;
}


static const luaL_reg msgpack_f[] = {
    {"pack", msgpack_pack_api },
    {"unpack", msgpack_unpack_api },
    {"largetbl", msgpack_largetbl },
    {NULL,NULL}
};

LUALIB_API int luaopen_msgpack ( lua_State *L ) {    
    lua_newtable(L);
    luaL_register(L,NULL, msgpack_f );
    return 1;
}
