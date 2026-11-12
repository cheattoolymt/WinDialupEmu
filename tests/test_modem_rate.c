/* SPDX-License-Identifier: BSD-2-Clause
 * Exercise real modem callbacks -> paced NAT -> paced PPP -> serial output.
 * Only the clock is replaced. No wall-clock sleeps or Internet dependency. */
#define vm_nat_now_ns modem_test_now_ns
#include "../src/modem/vm_modem.c"
#undef vm_nat_now_ns
#include <assert.h>
#include "vmodem/vm_eth.h"
static uint64_t test_time;
int64_t modem_test_now_ns(void) { return (int64_t)test_time; }
int main(void)
{
    vm_modem_t m;
    vm_nat_cfg_t nc;
    vm_ppp_cfg_t pc;
    vm_serial_params_t sp;
    vm_nat_stats_t stats;
    uint8_t ip[1500], received[4096];
    char err[256];
    memset(&m,0,sizeof(m)); memset(ip,0,sizeof(ip));
    vm_log_init(VM_LOG_ERROR,NULL);
    vm_nat_cfg_defaults(&nc);nc.backend=VM_NAT_LOOPBACK;
    assert(vm_nat_create(&m.nat,&nc,cb_nat_ip,&m)==VM_OK);
    assert(vm_nat_link_up(m.nat)==VM_OK);
    vm_serial_params_defaults(&sp);sp.backend=VM_SERIAL_BACKEND_LOOPBACK;
    assert(vm_serial_open(&m.ser,&sp,err,sizeof(err))==VM_OK);
    vm_ppp_cfg_defaults(&pc);
    assert(vm_ppp_init(&m.ppp,&pc,cb_ppp_write,&m,cb_ppp_ip,&m,0)==VM_OK);
    m.ppp.state=VM_PPP_RUNNING;m.ppp.ipcp_up=true;
    m.state=VM_MODEM_DATA;
    vm_rate_reset(&m.uplink,33600);vm_rate_reset(&m.downlink,33600);
    ip[0]=0x45;ip[2]=1500>>8;ip[3]=1500&255;ip[8]=64;ip[9]=1;
    ip[12]=192;ip[13]=168;ip[14]=99;ip[15]=2;
    ip[16]=1;ip[17]=1;ip[18]=1;ip[19]=1;ip[20]=8;
    { uint16_t c=vm_ip_checksum(ip+20,sizeof(ip)-20);ip[22]=c>>8;ip[23]=c; }
    { uint16_t c=vm_ip_checksum(ip,20);ip[10]=c>>8;ip[11]=c; }
    cb_ppp_ip(&m,ip,sizeof(ip));
    assert(m.uplink.count==1);
    test_time=357142857; pump_link(&m);
    vm_nat_get_stats(m.nat,&stats);assert(stats.tx_pkts==0);
    test_time=357142858; pump_link(&m);
    vm_nat_get_stats(m.nat,&stats);assert(stats.tx_pkts==1 && m.downlink.count==1);
    assert(vm_serial_peer_read(m.ser,received,sizeof(received))==0);
    test_time=714285715; pump_link(&m);
    assert(vm_serial_peer_read(m.ser,received,sizeof(received))==0);
    test_time=714285716; pump_link(&m);
    assert(!m.stop && vm_serial_peer_read(m.ser,received,sizeof(received))>1500);
    { uint8_t dns[60]={0}; unsigned j;
      dns[0]=0x45;dns[3]=60;dns[8]=64;dns[9]=17;
      memcpy(dns+12,ip+12,8);dns[20]=0x90;dns[21]=1;dns[23]=53;dns[25]=40;
      {uint16_t c=vm_ip_checksum(dns,20);dns[10]=c>>8;dns[11]=c;}
      for(j=0;j<VM_RATE_BULK_DEPTH;++j)cb_ppp_ip(&m,ip,sizeof(ip));
      cb_ppp_ip(&m,dns,sizeof(dns));
      assert(m.uplink.count==VM_RATE_BULK_DEPTH+1);
      test_time=m.uplink.due_ns;pump_link(&m);
      test_time=m.uplink.due_ns;pump_link(&m);
      vm_nat_get_stats(m.nat,&stats);assert(stats.tx_pkts==3);
      assert(m.uplink.count==VM_RATE_BULK_DEPTH-1 && test_time<1200000000ULL);
    }
    do_hangup(&m,false);assert(!m.uplink.count && !m.downlink.count);
    vm_nat_destroy(m.nat);vm_serial_close(m.ser);
    puts("PASS: production modem upload -> NAT echo -> download -> PPP serial obeys both directional delays; DNS bypasses bulk backlog; hangup clears queued data");
}
