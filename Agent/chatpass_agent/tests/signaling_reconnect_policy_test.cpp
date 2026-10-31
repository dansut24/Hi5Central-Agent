#include "../src/signaling_reconnect_policy.h"
#include <cassert>
#include <iostream>
int main() {
 using namespace hi5::signaling;
 const int expected[]={1,2,4,8,16,30,30,30};
 for(int i=1;i<=8;++i) assert(reconnectDelaySeconds(i)==expected[i-1]);
 assert(!shouldReconnect(true,false,true,true));
 assert(!shouldReconnect(false,true,true,true));
 assert(!shouldReconnect(false,false,true,false));
 assert(!shouldReconnect(false,false,false,true));
 assert(shouldReconnect(false,false,true,true));
 std::cout<<"SIGNALING_RECONNECT_BOUNDED_RETRY_SERVICE_STAYS_RUNNING_PASSED\n";
}
