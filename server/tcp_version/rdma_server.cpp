#include <stdio.h> 
#include <netdb.h> 
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <getopt.h>
#include <vector>
#include <ctime>

#include <numa.h>
#include <netdb.h> 
#include <netinet/in.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <fcntl.h> 
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <iostream>
#include <thread>
#include <deque>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <atomic>

#include "NuMA_KV_PM.h"
#include "variables.h"
#include "CCEH_PM_hybrid.h"
#include "circular_queue.h"

#include "rdma.h"
#include "server.h"
#include "tcp_internal.h"
#include "log.h"


extern int ib_port;
extern int tcp_port;
#undef DEBUG
#ifdef DEBUG
extern int errno;
#endif


/* option values */
int tcp_port = -1;
int ib_port = 1;
static int rdma_flag= 0;
char *path;
char *data_path;
char *pm_path;
size_t initialTableSize = 32*1024;
size_t numData = 0;
size_t numKVThreads = 0;
size_t numNetworkThreads = 0;
size_t numPollThreads = 0;
bool numa_on = false;
bool verbose_flag = false;
bool human = false;
struct bitmask *netcpubuf;
struct bitmask *kvcpubuf;
struct bitmask *pollcpubuf;

/* Global variables */
struct rdma_server_context* rctx = NULL;
queue_t **lfqs;
unsigned int nr_cpus;
std::atomic<bool> done(false);

/*
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cv = PTHREAD_COND_INITIALIZER;
*/

static int running;


/* counting valuse */
int putcnt = 0;
int getcnt = 0;
int sample_succ_get_cnt = 0;
int sample_fail_get_cnt = 0;
int found_cnt = 0;
int notfound_cnt = 0;
std::atomic<int> process_cnt[40];

/* performance timer */
uint64_t network_elapsed=0, pmput_elapsed=0, pmget_elapsed=0, pmlog_alloc_elapsed = 0;
uint64_t pmnet_rx_elapsed=0; 
uint64_t pmget_notexist_elapsed=0, pmget_exist_elapsed=0;
uint64_t pmput_queue_elapsed=0, pmget_queue_elapsed=0;
uint64_t rdpma_handle_write_req_elapsed=0 , rdpma_handle_write_elapsed=0;
uint64_t rdpma_handle_read_req_elapsed=0 , rdpma_handle_read_elapsed=0;

static void dprintf( const char* format, ... ) {
	if (verbose_flag) {
		va_list args;
		va_start( args, format );
		vprintf( format, args );
		va_end( args );
	}
}

static void usage(){
	printf("Usage\n");
	printf("\nOptions:\n");
	printf("\t-t --tcp_port=<port> (required) use <port> to listen tcp connection\n");
	printf("\t-i --ib_port=<port> (required) use <port> of infiniband device (default=1)\n");
	printf("\t-p --path=<port> (required) use <port> of infiniband device (default=1)\n");
}

static void rdpma_print_stats() {
	printf("\n--------------------REPORT---------------------\n");
//	printf("SAMPLE RATE [1/%d]\n", SAMPLE_RATE);
	printf("# of puts : %d , # of gets : %d ( %d / %d )\n",
			putcnt, getcnt, found_cnt, notfound_cnt);

	if (putcnt == 0)
		putcnt++;
	if (getcnt == 0)
		getcnt++;

	printf("\n--------------------SUMMARY--------------------\n");
	printf("Average (divided by number of ops)\n");
	printf("Write_req: %lu (us), Write: %lu (us)\n",
			rdpma_handle_write_req_elapsed/putcnt/1000,
			rdpma_handle_write_elapsed/putcnt/1000);

	printf("Read_req: %lu (us), Read: %lu (us)\n",
			rdpma_handle_read_req_elapsed/getcnt/1000,
			rdpma_handle_read_elapsed/getcnt/1000);

	rctx->kv->PrintStats();

	printf("--------------------FIN------------------------\n");

}

static void printCpuBuf(size_t nr_cpus, struct bitmask *bm, const char *str) {
	/* Print User specified cpus */
	dprintf("[ INFO ] %s\t threads: \t", str);
	for ( int i = 0; i< nr_cpus; i++) {
		if (i % 4 == 0)
			dprintf(" ");
		if (numa_bitmask_isbitset(bm, i))
			dprintf("1");
		else
			dprintf("0");
	}
	dprintf("\n");
}



void sigint_callback_handler_rdma(int signal){
	rdpma_print_stats();
	printf("SIGNAL occur\n");
	exit(signal);
}

enum ibv_mtu server_mtu_to_enum(int max_transfer_unit){
	switch(max_transfer_unit){
		case 256:	return IBV_MTU_256;
		case 512:	return IBV_MTU_512;
		case 1024:	return IBV_MTU_1024;
		case 2048:	return IBV_MTU_2048;
		case 4096:	return IBV_MTU_4096;
		default:	return (enum ibv_mtu)0;
	}
}

int poll_cq(struct ibv_cq* cq){
	struct ibv_wc wc;
	int ne, i;

	do{
		ne = ibv_poll_cq(cq, 1, &wc);
		if(ne < 0){
			die("poll_cq failed\n");
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "Failed status %s[%d] for wr_id %d\n", ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
	}
	return 0;
}

struct ibv_mr* ibv_register_mr(void* addr, int size, int flags){
	struct ibv_mr* ret;
	ret = ibv_reg_mr(rctx->pd, addr, size, flags);
	if(!ret)
		die("ibv_reg_mr failed\n");
	return ret;
}

int post_recv(int node_id){
	struct ibv_recv_wr wr;
	struct ibv_recv_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_recv_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = 0;
	sge.length = 0;
	sge.lkey = rctx->mr->lkey;

	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.next = NULL;

	if(ibv_post_recv(rctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_recv to node %d failed\n", __func__, node_id);
		return 1;
	}
	return 0;
}

/**
 * post_meta_request - post metadata request to target
 * @nid: Client node identifier.
 * @msg_num: Msg identifier.
 * @type: Message type (i.e. MSG_READ_REQUEST, MSG_WRITE_REQUEST, ...)
 * @num: IDK
 * @tx_state: Transaction state (i.e. TX_READ_BEGIN, TX_READ_COMMITTED, ...) 
 * @len: Size of content of addr
 * @dma_addr: DMA-able address.
 * @offset: offset from base to metadata region for msg_num
 *
 * This function post send in batch manner.
 * Note that only last work request to be signaled.
 *
 * If generate_single_write_request succeeds, then return 0
 * if not return negative value.
 */
int post_meta_request(int nid, int msg_num, int type, uint32_t num, 
		int tx_state, int len, uint64_t* dma_addr, uint64_t offset){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)dma_addr;
	sge.length = len;
	sge.lkey = rctx->mr->lkey;

	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(bit_mask(nid, msg_num, type, tx_state, num));
	wr.wr.rdma.remote_addr = (uintptr_t)(rctx->remote_mm[nid] + offset);
	wr.wr.rdma.rkey = rctx->rkey[nid];

//	dprintf("[%s]: nid(%d), msg_num(%d), type(%d), tx_state(%d), num(%d)\n", __func__, nid, msg_num, type, tx_state, num);
	if(ibv_post_send(rctx->qp[nid], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, nid);
		return 1;
	}

	struct ibv_wc wc;
	int ne;
	do{
		ne = ibv_poll_cq(rctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}



int rdma_write_imm(int node_id, int type, int imm_data, uint64_t offset){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;
	int ne;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uintptr_t)NULL;
	sge.length = 0;
	sge.lkey = rctx->mr->lkey;

	//    wr.wr_id = bit_mask(node_id, type, 0);
	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.imm_data = htonl(imm_data);
	wr.wr.rdma.remote_addr = (uintptr_t)(rctx->remote_mm[node_id] + offset);
	wr.wr.rdma.rkey = rctx->rkey[node_id];

	if(ibv_post_send(rctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, node_id);
		return 1;
	}
	struct ibv_wc wc;
	do{
		ne = ibv_poll_cq(rctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}


int rdma_write(int node_id, int type, uint64_t addr, int len){
	struct ibv_send_wr wr;
	struct ibv_send_wr* bad_wr;
	struct ibv_sge sge;
	int ne;

	memset(&wr, 0, sizeof(struct ibv_send_wr));
	memset(&sge, 0, sizeof(struct ibv_sge));

	sge.addr = (uint64_t)addr;
	sge.length = len;
	sge.lkey = rctx->mr->lkey;

	//    wr.wr_id = bit_mask(node_id, type, 0);
	wr.opcode = IBV_WR_RDMA_WRITE;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = rctx->remote_mm[node_id] + METADATA_SIZE; /*writing to reply region*/
	wr.wr.rdma.rkey = rctx->rkey[node_id];

	if(ibv_post_send(rctx->qp[node_id], &wr, &bad_wr)){
		fprintf(stderr, "[%s] ibv_post_send to node %d failed\n", __func__, node_id);
		return 1;
	}

	struct ibv_wc wc;
	do{
		ne = ibv_poll_cq(rctx->send_cq, 1, &wc);
		if(ne < 0){
			fprintf(stderr, "[%s] ibv_poll_cq failed\n", __func__);
			return 1;
		}
	}while(ne < 1);

	if(wc.status != IBV_WC_SUCCESS){
		fprintf(stderr, "[%s] sending rdma_write failed status %s (%d)\n", __func__, ibv_wc_status_str(wc.status), wc.status);
		return 1;
	}

	return 0;
}

uint32_t bit_mask(int node_id, int msg_num, int type, int state, uint32_t num){
	uint32_t target = (((uint32_t)node_id << 28) | ((uint32_t)msg_num << 16) | ((uint32_t)type << 12) | ((uint32_t)state << 8) | ((uint32_t)num & 0x000000ff));
	return target;
}

void bit_unmask(uint32_t target, int* node_id, int* msg_num, int* type, int* state, uint32_t* num){
	*num = (uint32_t)(target & 0x000000ff);
	*state = (int)((target >> 8) & 0x0000000f);
	*type = (int)((target >> 12) & 0x0000000f);
	*msg_num = (int)((target >> 16) & 0x00000fff);
	*node_id = (int)((target >> 28) & 0x0000000f);
}

/**
 * indicator - Show stats periodically
 */
void rdpma_indicator() {
	while (!done) {
		sleep(10);
		rdpma_print_stats();
	}
}

void server_recv_poll_cq(struct ibv_cq *cq){
	struct ibv_wc wc;
	int ne;
	static int num = 1;
	int targetQ;
	int targetNode = 0;
	int targetOp = 0;
	int targetQueue = 0;

	while(1){
		ne = 0;
		do{
			ne += ibv_poll_cq(cq, 1, &wc);
			if(ne < 0){
				fprintf(stderr, "ibv_poll_cq failed %d\n", ne);
				die("ibv_poll_cq failed");
			}
		}while(ne < 1);

		if(wc.status != IBV_WC_SUCCESS){
			fprintf(stderr, "Failed status %s (%d)\n", ibv_wc_status_str(wc.status), wc.status);
			die("Failed status");
		}

		int ret;
		if((int)wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
			int node_id, msg_num, type, tx_state;
			uint32_t num;
			bit_unmask(ntohl(wc.imm_data), &node_id, &msg_num, &type, &tx_state, &num);
			uint64_t* key = (uint64_t*)GET_CLIENT_META_REGION(rctx->local_mm, node_id, msg_num);
			targetNode = rctx->kv->GetNodeID(*key);
			targetOp = type == MSG_WRITE_REQUEST || MSG_WRITE ? 0 : 1;
			targetQueue = targetNode * 2 + targetOp;

			targetQ = *key % nr_cpus;

			//			dprintf("[%s]: node_id(%d), msg_num(%d), type(%d), tx_state(%d), num(%d)\n", __func__, node_id, msg_num, type, tx_state, num);
			post_recv(node_id);
			if(type == MSG_WRITE_REQUEST){
//				dprintf("[%s]: received MSG_WRITE_REQUEST\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->msg_num = msg_num;
				new_request->num = num;
				enqueue(lfqs[targetQueue], (void*)new_request);
//				dprintf("[%s]: MSG_WRITE_REQUEST(%lx) enqueued\n", __func__, *key);
				putcnt++;
			}
			else if(type == MSG_WRITE){
//				dprintf("[%s]: received MSG_WRITE\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->msg_num = msg_num;
				new_request->num = num;
				enqueue(lfqs[targetQueue], (void*)new_request);
			}
			else if(type == MSG_READ_REQUEST){
//				dprintf("[%s]: received MSG_READ_REQUEST\n", __func__);
				struct request_struct* new_request = (struct request_struct*)malloc(sizeof(struct request_struct));
				new_request->type = type;
				new_request->node_id = node_id;
				new_request->msg_num = msg_num;
				new_request->num = num;
				enqueue(lfqs[targetQueue], (void*)new_request);
				getcnt++;
			}
			else if(type == MSG_READ_REPLY){
//				dprintf("[%s]: received MSG_READ_REPLY\n", __func__);
				free((void*)rctx->temp_log[node_id][msg_num]);
				//munmap((void*)rctx->temp_log[node_id][msg_num], num*PAGE_SIZE);
			}
		}
		else if((int)wc.opcode == IBV_WC_RDMA_READ){
			dprintf("[%s]: received WC_RDMA_READ\n", __func__);
			/* the client is reading data from read region*/
		}
		else{
			fprintf(stderr, "Received a weired opcode (%d)\n", (int)wc.opcode);
		}
	}
}


void event_handler(int cpu){
	struct request_struct* new_request;
	//TOID(CCEH) hashtable = rctx->hashtable;
	int insert_cnt = 0;
	int search_cnt = 0;
#if defined(TIME_CHECK)
	struct timespec start,end;
	bool checkit = false;
#endif
	
//	dprintf("[  OK  ] event_handler is running on CPU %d \n", sched_getcpu());

	while(!done){
		new_request = (struct request_struct*)dequeue(lfqs[cpu]);

		if(new_request->type == MSG_WRITE_REQUEST){
#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif
			uint64_t* key = (uint64_t*)GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num);
//			dprintf("Processing [MSG_WRITE_REQUEST] %d num pages (node=%x, msg_num=%x, key=%lx)\n",  \
					new_request->num, new_request->node_id, new_request->msg_num, *key);
			uint64_t page = (uint64_t)malloc(new_request->num * PAGE_SIZE);
			rctx->temp_log[new_request->node_id][new_request->msg_num] = page;
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->msg_num + sizeof(uint64_t);
			uint64_t* addr = (uint64_t*)(GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num) + sizeof(uint64_t));
			*addr = page;
//			dprintf("[%s]: send page address: %lx\n", __func__, (uint64_t)page);
			post_meta_request(new_request->node_id, new_request->msg_num, MSG_WRITE_REQUEST_REPLY, new_request->num, TX_WRITE_READY, sizeof(uint64_t), addr, offset);

#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &end);
			rdpma_handle_write_req_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
		}
		else if(new_request->type == MSG_WRITE){
#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif
			uint64_t ptr = rctx->temp_log[new_request->node_id][new_request->msg_num];
			uint64_t* key = (uint64_t*)GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num);
//			dprintf("Processing [MSG_WRITE] %d num pages (node=%x, msg_num=%x, key=%lx)\n", \
					new_request->num, new_request->node_id, new_request->msg_num, *key);
			int targetNode = rctx->kv->GetNodeID(*key);
			for(int i = 0; i < new_request->num; i++){
				TOID(char) temp;
				POBJ_ALLOC(rctx->log_pop[targetNode], &temp, char, sizeof(char)*PAGE_SIZE, NULL, NULL);
				uint64_t temp_addr = (uint64_t)rctx->log_pop + temp.oid.off;
				memcpy((void*)temp_addr, (void *)(ptr + i * PAGE_SIZE), PAGE_SIZE);
				pmemobj_persist(rctx->log_pop[targetNode], (char*)temp_addr, sizeof(char)*PAGE_SIZE);

				rctx->kv->Insert(*key, (Value_t)temp_addr, 0, 0); /* XXX */
				/*
				void* check = (void*)D_RW(rctx->hashtable)->Get(*key);
				dprintf("[%s]: Insert value to page: %lx\n", __func__, (uint64_t)ptr);
				fprintf(stderr, "Inserted value for key %lu (%lx)\n", *key, *key);
				dprintf("[%s]: msg double check: %s\n", __func__, (char*)ptr);
				*/

				key += METADATA_SIZE;
			}
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->msg_num + sizeof(uint64_t);
			/* if successfully inserted */
			post_meta_request(new_request->node_id, new_request->msg_num, MSG_WRITE_REPLY, new_request->num, TX_WRITE_COMMITTED, 0, NULL, offset);
			free((void *)ptr);
#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &end);
			rdpma_handle_write_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
		}
		else if(new_request->type == MSG_READ_REQUEST){
#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif
			uint64_t* key = (uint64_t*)(GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num)); 
//			dprintf("Processing [MSG_READ_REQUEST] %d num pages (node=%x, msg_num=%x, key=%lx)\n",  \
					new_request->num, new_request->node_id, new_request->msg_num, *key);
			void* page = (void*)malloc(new_request->num * PAGE_SIZE);
			uint64_t offset = NUM_ENTRY * METADATA_SIZE * new_request->msg_num + sizeof(uint64_t);
			void* values[new_request->num];
			bool abort = false;
			for(int i=0; i<new_request->num; i++){
				key = (uint64_t*)(GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num) + i*METADATA_SIZE); 
				values[i] = (void*)rctx->kv->Get(*key, 0); /* XXX */
				if(!values[i]){
					dprintf("Value for key[%lx] not found\n", *key);
					abort = true;
					notfound_cnt++;
				} else
					found_cnt++;
			}

			if(!abort){
				memcpy(page, values[0], PAGE_SIZE * new_request->num);
				rctx->temp_log[new_request->node_id][new_request->msg_num] = (uint64_t)page;
				uint64_t* addr = (uint64_t*)(GET_CLIENT_META_REGION(rctx->local_mm, new_request->node_id, new_request->msg_num) + sizeof(uint64_t));
				*addr = (uint64_t)page;
				dprintf("return page (addr=%lx) : %s\n", *addr, page);
//				dprintf("[%s]: addr: %lx, page: %p\n", __func__, *addr, page);
//				dprintf("[%s]: msg double check: %s\n", __func__, (char*)page);
				post_meta_request(new_request->node_id, new_request->msg_num, MSG_READ_REQUEST_REPLY, new_request->num, TX_READ_READY, sizeof(uint64_t), addr, offset);
			}
			else{
				post_meta_request(new_request->node_id, new_request->msg_num, MSG_READ_REQUEST_REPLY, new_request->num, TX_READ_ABORTED, 0, NULL, offset);
				dprintf("Aborted [MSG_READ_REQUEST] %d num pages from node %d with %d msg_num\n", new_request->num, new_request->node_id, new_request->msg_num);
			}
#if defined(TIME_CHECK)
			clock_gettime(CLOCK_MONOTONIC, &end);
			rdpma_handle_read_req_elapsed += end.tv_nsec - start.tv_nsec + 1000000000 * (end.tv_sec - start.tv_sec);
#endif
		}
		else{
			fprintf(stderr, "Received weired request type %d from node %d\n", new_request->type, new_request->node_id);
		}

		free(new_request);
	}
	return;
}


static int modify_qp(struct ibv_qp* qp, int my_psn, int sl, struct node_info* dest){
	struct ibv_qp_attr attr;
	int flags;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = ib_port;
	attr.qp_access_flags =
		IBV_ACCESS_LOCAL_WRITE |
		IBV_ACCESS_REMOTE_READ |
		IBV_ACCESS_REMOTE_WRITE|
		IBV_ACCESS_REMOTE_ATOMIC;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	if(ibv_modify_qp(qp, &attr, flags)){
		die("ibv_modify_qp to INIT failed\n");
		return 1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_4096;
	attr.dest_qp_num = dest->qpn;
	attr.rq_psn = dest->psn;
	attr.max_dest_rd_atomic = 16;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dest->lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 1;
//	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = ib_port;

	/* TODO: IDK */
	if(dest->gid.global.interface_id){
//		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = -1;
	}

	if(ibv_modify_qp(qp, &attr, 
				IBV_QP_STATE |
				IBV_QP_PATH_MTU |
				IBV_QP_DEST_QPN |
				IBV_QP_RQ_PSN |
				IBV_QP_MAX_DEST_RD_ATOMIC |
				IBV_QP_MIN_RNR_TIMER |
				IBV_QP_AV )){
		die("ibv_modify_qp to RTR failed\n");
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	//    attr.sq_psn = 0;
	attr.sq_psn = my_psn;
	attr.max_rd_atomic = 16;
	attr.max_dest_rd_atomic = 16;
	if(ibv_modify_qp(qp, &attr,
				IBV_QP_STATE |
				IBV_QP_TIMEOUT |
				IBV_QP_RETRY_CNT |
				IBV_QP_RNR_RETRY |
				IBV_QP_SQ_PSN |
				IBV_QP_MAX_QP_RD_ATOMIC)){
		die("ibv_modify_qp to RTS failed\n");
	}

	dprintf("[  OK  ] modify_qp to RTS succeeded\n");
	return 0;
}

/* make PM file and global context */
static struct rdma_server_context* server_init_ctx(struct ibv_device* dev, int size, int rx_depth, char *ipath){
	int flags;
	void* ptr;
	char base_path[32] = "/mnt/pmem0";
	char log_path[32] = "/jy/log";
	const size_t hashtable_initialSize = 1024*16*4; 

	rctx = (struct rdma_server_context*)malloc(sizeof(struct rdma_server_context));
	rctx->node_id = SERVER_NODE_ID;
	rctx->size = size;
	rctx->send_flags = IBV_SEND_SIGNALED;
	rctx->rx_depth = rx_depth;
	rctx->local_mm = (uint64_t)malloc(LOCAL_META_REGION_SIZE);

	dprintf("create request queue...\n");
	rctx->request_queue = create_queue("lfqs");
	rctx->temp_log = (uint64_t**)malloc(sizeof(uint64_t*)*MAX_NODE);
	for(int i=0; i<MAX_NODE; i++){
		rctx->temp_log[i] = (uint64_t*)malloc(sizeof(uint64_t)*MAX_PROCESS);
	}

	for( int i = 0; i < NUM_NUMA; i++ ) {
		snprintf(&base_path[9], sizeof(int), "%d", i);
		strncpy(&base_path[10], log_path, strlen(log_path));
		if(access(base_path, 0) != 0){
			rctx->log_pop[i] = pmemobj_create(base_path, POBJ_LAYOUT_NAME(LOG), LOG_SIZE, 0666);
			if(!rctx->log_pop[i]){
				perror("pmemobj_create");
				exit(0);
			}
		}
		else{
			rctx->log_pop[i] = pmemobj_open(base_path, POBJ_LAYOUT_NAME(LOG));
			if(!rctx->log_pop){
				perror("pmemobj_open");
				exit(0);
			}
		}
	}
	dprintf("[  OK  ] log initialized\n");

	bool exists = false;
	char path[32] = "/mnt/pmem0";

	for( int i = 0 ; i < NUM_NUMA; i++ ){
		snprintf(&path[9], sizeof(int), "%d", i);
		strncpy(&path[10], pm_path, strlen(pm_path));
		dprintf("[ INFO ] File used for index: %s\n", path);
		if(access(path, 0) != 0){
			rctx->pop[i] = pmemobj_create(path, POBJ_LAYOUT_NAME(HashTable), INDEX_SIZE, 0666);
			if(!rctx->pop[i]){
				perror("pmemobj_create");
				exit(1);
			}
		}
		else{
			rctx->pop[i] = pmemobj_open(path, POBJ_LAYOUT_NAME(HashTable));
			if(!rctx->pop[i]){
				perror("pmemobj_open");
				exit(1);
			}
			exists = true;
		}
	}

	if(!exists) {
		rctx->kv = new NUMA_KV(rctx->pop, initialTableSize/Segment::kNumSlot, numKVThreads, numPollThreads);
		dprintf("[  OK  ] KVStore Initialized\n");
	} else {
		rctx->kv = new NUMA_KV(rctx->pop, true, numKVThreads, numPollThreads);
		if(!rctx->kv->Recovery()) {
			dprintf("[ FAIL ] KVStore Recovered \n");
			exit(1);
		}
		dprintf("[  OK  ] KVStore Recovered \n");
	}
	dprintf("[  OK  ] hashtable initialized\n");


	rctx->context = ibv_open_device(dev);
	if(!rctx->context){
		fprintf(stderr, "ibv_open_device failed for %s\n", ibv_get_device_name(dev));
		return NULL;
	}

	rctx->channel = NULL;
	rctx->pd = ibv_alloc_pd(rctx->context);
	if(!rctx->pd)
		die("ibv_alloc_pd failed\n");

	/* 
	 * To create an implicit ODP MR, IBV_ACCESS_ON_DEMAND should be set, 
	 * addr should be 0 and length should be SIZE_MAX.
	 */
	flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_ON_DEMAND;
	rctx->mr = ibv_reg_mr(rctx->pd, NULL, (uint64_t)-1, flags);
	if(!rctx->mr){
		fprintf(stderr, "ibv_reg_mr failed\n");
		goto dealloc_pd;
	}

	memset((void*)rctx->local_mm, 0, LOCAL_META_REGION_SIZE);

	QP_DEPTH = rx_depth+1;
	rctx->recv_cq = ibv_create_cq(rctx->context, QP_DEPTH, NULL, NULL, 0);
	if(!rctx->recv_cq){
		fprintf(stderr, "ibv_create_cq for recv_cq failed\n");
		goto dereg_mr;
	}

	rctx->send_cq = ibv_create_cq(rctx->context, QP_DEPTH, NULL, NULL, 0);
	if(!rctx->send_cq){
		fprintf(stderr, "ibv_create_cq for send_cq failed\n");
		goto destroy_qp;
	}

//	printf("[%s] Allocate queue pair region\n", __func__);
	rctx->qp = (struct ibv_qp**)malloc(MAX_NODE * sizeof(struct ibv_qp*));
	for(int i=0; i<MAX_NODE; i++){
		struct ibv_qp_init_attr init_attr;
		memset(&init_attr, 0, sizeof(struct ibv_qp_init_attr));

		init_attr.send_cq = rctx->send_cq;
		init_attr.recv_cq = rctx->recv_cq;
		init_attr.cap.max_send_wr = 64;
		init_attr.cap.max_recv_wr = 64;
		init_attr.cap.max_send_sge = 1;
		init_attr.cap.max_recv_sge = 1;

		init_attr.sq_sig_all = 0;
		init_attr.cap.max_inline_data = 0;

		init_attr.qp_type = IBV_QPT_RC;

		rctx->qp[i] = ibv_create_qp(rctx->pd, &init_attr);
		if(!rctx->qp[i]){
			fprintf(stderr, "ibv_create_qp[%d] failed\n", i);
			goto destroy_qp;
		}
	}

	return rctx;

destroy_qp:
	if(rctx->send_cq)
		ibv_destroy_cq(rctx->send_cq);
	if(rctx->recv_cq) 
		ibv_destroy_cq(rctx->recv_cq);
	for(int i=0; i<MAX_NODE; i++)
		if(rctx->qp[i]) 
			ibv_destroy_qp(rctx->qp[i]);
dereg_mr:
	if(rctx->mr) 
		ibv_dereg_mr(rctx->mr);
dealloc_pd:
	if(rctx->channel) 
		ibv_destroy_comp_channel(rctx->channel);
	if(rctx->pd) 
		ibv_dealloc_pd(rctx->pd);
	return NULL;
}

/**
 * init_rdma_network - Accept client until done
 */
void init_rdma_network(){
	int cur_node = 0;
	int sock, fd, ret;
	struct sockaddr_in local_sock;
	int gid_idx = 0; /* XXX 0 -> 2 */
	int on = 1;
	union ibv_gid gid;
	running = 1;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0){
		die("Socket creation failed\n");
	}

	memset(&local_sock, 0, sizeof(struct sockaddr_in));
	local_sock.sin_family = AF_INET;
	local_sock.sin_addr.s_addr = htonl(INADDR_ANY);
	local_sock.sin_port = htons(tcp_port);

	if((bind(sock, (struct sockaddr*)&local_sock, sizeof(local_sock))) < 0)
		die("Socket bind failed\n");

	if((listen(sock, 10)) < 0)
		die("Socket listen failed\n");

	ret = ibv_query_gid(rctx->context, ib_port, gid_idx, &gid);
	if(ret){
		fprintf(stderr, "Error, failed to query GID index %d of port %d in device '%s'\n",
						gid_idx, ib_port, ibv_get_device_name(rctx->context->device));
		close(fd);
		close(sock);
		exit(1);
	}

	while(!done){
		socklen_t sin_size = sizeof(struct sockaddr);
		struct sockaddr_in remote_sock;
		char remote_ip[INET_ADDRSTRLEN];
		struct node_info local_node, remote_node;
//		union ibv_gid gid;
		memset(&local_node, 0, sizeof(struct node_info));
		memset(&remote_node, 0, sizeof(struct node_info));
		memset(&remote_sock, 0, sizeof(struct sockaddr_in));

		fd = accept(sock, (struct sockaddr*)&remote_sock, (socklen_t*)&sin_size);
		if(fd < 0){
			fprintf(stderr, "Server accept failed\n");
			close(fd);
			close(sock);
			exit(1);
		}
		inet_ntop(AF_INET, &remote_sock.sin_addr, remote_ip, INET_ADDRSTRLEN);
		dprintf("TCP Socket accepted a connection %d from %s\n", cur_node, remote_ip);

		//	ret = ibv_query_gid(rctx->context, ib_port, 2, &gid);
		ret = ibv_query_gid(rctx->context, ib_port, gid_idx, &gid);
		if(ret){
			fprintf(stderr, "Error, failed to query GID index %d of port %d in device '%s'\n",
							gid_idx, ib_port, ibv_get_device_name(rctx->context->device));
			close(fd);
			close(sock);
			exit(1);
		}

		local_node.node_id = cur_node;
		local_node.lid = rctx->port_attr.lid;
		local_node.qpn = rctx->qp[cur_node]->qp_num;
		local_node.psn = lrand48() & 0xffffff;
		local_node.mm = rctx->local_mm + (cur_node * PER_NODE_META_REGION_SIZE);
		local_node.rkey = rctx->mr->rkey;
		local_node.gid = gid;
		dprintf("[ INFO ] LOCAL 	node_id(%d) lid(%d) qpn(%d) psn(%d) mm(%12lx) rkey(%x)\n", 
				local_node.node_id, local_node.lid, local_node.qpn, local_node.psn, local_node.mm, local_node.rkey);
		ret = write(fd, (char*)&local_node, sizeof(struct node_info));
		if(ret != sizeof(struct node_info)){
			fprintf(stderr, "[ FAIL ] TCP write failed\n");
			close(fd);
			close(sock);
			exit(1);
		}

		//	ret = tcp_recv(fd, &remote_node, sizeof(struct node_info));
		ret = read(fd, (char*)&remote_node, sizeof(struct node_info));
		if(ret != sizeof(struct node_info)){
			fprintf(stderr, "[ FAIL ] TCP read failed\n");
			close(fd);
			close(sock);
			exit(1);
		}
		dprintf("[ INFO ] REMOTE	node_id(%d) lid(%d) qpn(%d) psn(%d) mm(%12lx) rkey(%x)\n", remote_node.node_id, remote_node.lid, remote_node.qpn, remote_node.psn, remote_node.mm, remote_node.rkey);

		rctx->remote_mm[remote_node.node_id] = remote_node.mm;
		rctx->rkey[remote_node.node_id] = remote_node.rkey;

		ret = modify_qp(rctx->qp[remote_node.node_id], local_node.psn, 0, &remote_node);
		if(ret){
			fprintf(stderr, "ib_modify_qp failed for %d client\n", remote_node.node_id);
			close(fd);
			close(sock);
			exit(1);
		}
		post_recv(cur_node);

		cur_node++;

		printf("[  OK  ] RDMA connection with %s established.\n", remote_ip);

		std::thread p = std::thread( server_recv_poll_cq, rctx->recv_cq );
		std::thread i = std::thread( rdpma_indicator );

		std::mutex iomutex;
		std::vector<std::thread> threads(nr_cpus);
		for (unsigned i = 0; i < nr_cpus; ++i) {
			threads[i] = std::thread(event_handler, i);

			// Create a cpu_set_t object representing a set of CPUs. Clear it and mark
			// only CPU i as set.
			// threads[i] would be assigned to CPU i
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(i, &cpuset);
			int rc = pthread_setaffinity_np(threads[i].native_handle(),
					sizeof(cpu_set_t), &cpuset);
			if (rc != 0) {
				std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
			}
		}

		for (auto& t : threads) {
			t.join();
		}

		p.join();
		i.join();
	}

	if(fd)
		close(fd);
	if(sock)
		close(sock);
	exit(0);
}

int query_qp(struct ibv_qp* qp){
	int ret;
	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;

	ret = ibv_query_qp(qp, &attr, IBV_QP_STATE, &init_attr);
	switch(attr.qp_state){
		case IBV_QPS_INIT:
			printf("[%s] current qp state is ib_qps_init\n", __func__);
			break;
		case IBV_QPS_RTR:
			printf("[%s] current qp state is ib_qps_rts\n", __func__);
			break;
		case IBV_QPS_RTS:
			printf("[%s] current qp state is ib_qps_rts\n", __func__);
			break;
		case IBV_QPS_RESET:
			printf("[%s] current qp state is ib_qps_reset\n", __func__);
			break;
		case IBV_QPS_SQD:
			printf("[%s] current qp state is ib_qps_sqd\n", __func__);
			break;
		case IBV_QPS_SQE:
			printf("[%s] current qp state is ib_qps_sqe\n", __func__);
			break;
		case IBV_QPS_ERR:
			printf("[%s] current qp state is ib_qps_err\n", __func__);
			break;
		default:
			printf("[%s] current qp state is ib_qps_unknown\n", __func__);
	}
	return 1;
}

int server_init_interface(char *path){
	struct ibv_device** dev_list = NULL;
	struct ibv_device* dev = NULL;
	char* dev_name = NULL;
	struct ibv_context* context;
	struct ibv_device_attr dev_attr;
	int dev_num = 0;
	int size = 4096;
	int rx_depth = 256;
	int ret;

	dev_list = ibv_get_device_list(&dev_num);
	if(!dev_list)
		die("ibv_get_device_list failed\n");

	for(int i=0; i<dev_num; i++){
		dev_name = strdup(ibv_get_device_name(dev_list[i]));
		if (!strcmp(dev_name, "mlx5_0")){
			dev = dev_list[i];
			break;
		}
		/*
		if(!dev_name) dev_name = strdup(ibv_get_device_name(dev_list[i]));
		if(!strcmp(ibv_get_device_name(dev_list[i]), dev_name)){
			dev = dev_list[i];
		}
		*/
	}

	if(!dev)
		die("ib_device is not found\n");

/*
	context = ibv_open_device(dev);
	if(!context)
		die("ibv_open_device failed\n");

	ret = ibv_query_device(context, &dev_attr);
	if(ret)
		die("ibv_query_device failed\n");
	dattr.comp_mask = IBV_EXP_DEVICE_ATTR_ODP | IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS;
	ret = ibv_exp_query_device(context, &dattr);
	if (dattr.exp_device_cap_flags & IBV_EXP_DEVICE_ODP)
		printf("[  OK  ] ODP supported\n");
*/

	rctx = server_init_ctx(dev, size, rx_depth, path);
	if(!rctx)
		die("server_init_rctx failed\n");

	ret = ibv_query_port(rctx->context, ib_port, &rctx->port_attr);
	if(ret) {
		fprintf(stderr, "Error, failed to query port %d attributes in device '%s'\n",
						ib_port, ibv_get_device_name(rctx->context->device));
		die("terminated\n");
	}

	ibv_free_device_list(dev_list);

	printf("[  OK  ] Server ready to accept connection\n");

	return 0;
}

void init_rdma_server(char *path){
	int status;
	int ret;

	ret = server_init_interface(path);
	if (ret)
		die("[ FAIL ] server_init_interface failed\n");

	/* accept client and loop */
	init_rdma_network();

	printf("[ PASS ] Server successfully shutdown.\n");

}

int main(int argc, char* argv[]){
	char hostname[64];

	const char *short_options = "vs:t:i:n:d:z:hK:P:W:";
	static struct option long_options[] =
	{
		{"verbose", 0, NULL, 'v'},
		{"tcp_port", 1, NULL, 't'},
		{"ib_port", 1, NULL, 'i'},
		{"tablesize", 1, NULL, 's'},
		{"dataset", 1, NULL, 'd'},
		{"pm_path", 1, NULL, 'z'},
		{"nr_data", 1, NULL, 'n'},
		{"netcpubind", 1, NULL, 'W'},
		{"kvcpubind", 1, NULL, 'K'},
		{"pollcpubind", 1, NULL, 'P'},
		{0, 0, 0, 0} 
	};


	while(1){
		int c = getopt_long(argc, argv, short_options, long_options, NULL);
		if(c == -1) break;
		switch(c){
			case 'i':
				ib_port = strtol(optarg, NULL, 0);
				if(ib_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 't':
				tcp_port = strtol(optarg, NULL, 0);
				if(tcp_port <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 'n':
				numData = strtol(optarg, NULL, 0);
				if(numData <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 's':
				initialTableSize = strtol(optarg, NULL, 0);
				if(initialTableSize <= 0){
					printf ("<%s> is invalid\n", optarg);
					usage();
					return 0;
				}
				break;
			case 'd':
				data_path = strdup(optarg);
				break;
			case 'z':
				pm_path= strdup(optarg);
				break;
			case 'W':
				netcpubuf = numa_parse_cpustring(optarg);
				if (!netcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'K':
				kvcpubuf = numa_parse_cpustring(optarg);
				if (!kvcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'P':
				pollcpubuf = numa_parse_cpustring(optarg);
				if (!pollcpubuf) {
					printf ("<%s> is invalid\n", optarg);
					usage();
				}
				break;
			case 'h':
				human = true;
				break;
			case 'v':
				verbose_flag = 1;
				break;
			default:
				printf ("%c, <%s> is invalid\n", (char)c,optarg);
				usage();
				return 0;
		}
	}

	gethostname(hostname, 64);
	dprintf("[ INFO ] Hostname:\t %s IB port:\t %d TCP port:\t %d\n", hostname, ib_port, tcp_port);

	struct timespec i_start, i_end, g_start, g_end;
	uint64_t i_elapsed, g_elapsed;


	/* GET NUMA and CPU information */
	nr_cpus = std::thread::hardware_concurrency();
	dprintf("[ INFO ] NR_CPUS= %d\n", nr_cpus);

	/* count number of threads */
	for (int i = 0; i < nr_cpus ; i++) {
		if (numa_bitmask_isbitset(netcpubuf, i))
			numNetworkThreads++;	

		if (numa_bitmask_isbitset(kvcpubuf, i))
			numKVThreads++;	

		if (numa_bitmask_isbitset(pollcpubuf, i))
			numPollThreads++;	
	}

	/* Print User specified cpu binding */
	printCpuBuf(nr_cpus, netcpubuf, "net");
	printCpuBuf(nr_cpus, kvcpubuf, "kv");
	printCpuBuf(nr_cpus, pollcpubuf, "cqpoll");

//	lfqs = (queue_t**)malloc(nr_cpus * sizeof(queue_t*));
	lfqs = (queue_t**)malloc(NUM_NUMA * sizeof(queue_t*) * 2); /* GET, PUT QUEUE seperation */
//	for (int i = 0; i < nr_cpus; i++) {
	for (int i = 0; i < NUM_NUMA * 2; i++) {
		lfqs[i] = create_queue("lfqs");
	}

	for (int i = 0; i < nr_cpus; i++) {
		process_cnt[i] = 0;
	}

	signal(SIGINT, sigint_callback_handler_rdma);
	init_rdma_server(path);

	return 0;
}