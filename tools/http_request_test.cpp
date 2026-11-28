#include "../android/jni/include/service/http_request.h"
#include <cassert>
#include <thread>
#include <unistd.h>
static int parse(std::string wire, Http::Request &out) {
 int fds[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds)==0);
 std::thread sender([&]{ size_t off=0; while(off<wire.size()) { auto n=send(fds[1],wire.data()+off,wire.size()-off,MSG_NOSIGNAL); if(n<=0)break; off+=n; } shutdown(fds[1],SHUT_WR); });
 int status=Http::Receive(fds[0],out); close(fds[0]);sender.join();close(fds[1]);return status;
}
int main(){
 Http::Request r;
 assert(parse("POST /api/rpc HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}",r)==0&&r.body=="{}");
 for(auto wire : {"GET / HTTP/9.0\r\n\r\n", "GET / HTTP/1.1\r\nbadheader\r\n\r\n", "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nx"}) assert(parse(wire,r)==400);
 assert(parse("GET /mcp HTTP/1.1\r\nAccept: a\r\nAccept: b\r\n\r\n",r)==0&&r.headers["accept"]=="a,b");
 std::string body(70000,'x'); assert(parse("POST / HTTP/1.1\r\nContent-Length: 70000\r\n\r\n"+body,r)==0&&r.body==body);
 assert(parse("GET / HTTP/1.1\r\nLarge: "+body+"\r\n\r\n",r)==431);
 assert(parse("POST / HTTP/1.1\r\nContent-Length: 16777217\r\n\r\n",r)==413);
 int pair[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
 auto start=std::chrono::steady_clock::now();
 assert(Http::Receive(pair[0],r)==-ETIMEDOUT);
 assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(17));
 close(pair[0]);close(pair[1]);
 puts("HTTP parser and idle deadline contracts PASS");
}
