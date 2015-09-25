/*********************************************************************
 * Copyright (C) 2015, International Business Machines Corporation
 * All Rights Reserved
 *********************************************************************/

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_random.h>
#include <rte_ring.h>
#include <rte_string_fns.h>

#include "config.h"
#include "init.h"

struct rte_mempool *socket_mempool_[MAX_SOCKETS];
struct lcore_conf lcore_conf_[RTE_MAX_LCORE];

static uint16_t num_rxd_ = STREAMS_SOURCE_RX_DESC_DEFAULT;
static uint16_t num_txd_ = STREAMS_SOURCE_TX_DESC_DEFAULT;

static const struct rte_eth_conf port_conf_ = {
    .rxmode = {
	.max_rx_pkt_len = ETHER_MAX_LEN,
	.split_hdr_size = 0,
	.header_split   = 0, /**< Header Split disabled */
	.hw_ip_checksum = 1, /**< IP checksum offload enabled */
	.hw_vlan_filter = 1, /**< VLAN filtering disabled */
	.hw_vlan_extend = 0, /**< Extended VLAN disabled. */
	.jumbo_frame    = 0, /**< Jumbo frame support disabled */
	.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	.mq_mode        = ETH_MQ_RX_RSS,
    },
    .rx_adv_conf = {
	.rss_conf = {
	    .rss_key = NULL,
	    .rss_hf =  ETH_RSS_IPV4 | ETH_RSS_IPV6,
	},
    },
    .txmode = {
	.mq_mode = ETH_DCB_NONE,
    },
};

static const struct rte_eth_rxconf rx_conf_ = {
    .rx_thresh = {
	.pthresh = RX_PTHRESH,
	.hthresh = RX_HTHRESH,
	.wthresh = RX_WTHRESH,
    },
    .rx_free_thresh = 32,
};

static struct rte_eth_txconf tx_conf_ = {
    .tx_thresh = {
	.pthresh = TX_PTHRESH,
	.hthresh = TX_HTHRESH,
	.wthresh = TX_WTHRESH,
    },
    .tx_free_thresh = 0, /* Use PMD default values */
    .tx_rs_thresh = 0, /* Use PMD detauls values */
    .txq_flags = 0x0,
};

/*
 * Create a memory pool for every enabled socket.
 */
static void init_pools(void) {
    unsigned i;
    unsigned socket_id;
    struct rte_mempool *mp;
    char s[64];

    for (i = 0; i < RTE_MAX_LCORE; ++i) {
	if (rte_lcore_is_enabled(i) == 0)
	    continue;

	socket_id = rte_lcore_to_socket_id(i);
	if (socket_id > MAX_SOCKETS)
	    rte_exit(EXIT_FAILURE, "socket_id %d > MAX_SOCKETS\n",
		    socket_id);

	if (socket_mempool_[socket_id] != NULL)
	    continue;
	mp = rte_mempool_create(s, NB_MBUF, MBUF_SIZE, MEMPOOL_CACHE_SIZE,
		sizeof(struct rte_pktmbuf_pool_private),
		rte_pktmbuf_pool_init, NULL,
		rte_pktmbuf_init, NULL, socket_id, 0);
	if (mp == NULL)
	    rte_exit(EXIT_FAILURE, "Error on rte_mempool_create");

	socket_mempool_[socket_id] = mp;
    }
}

static void init_ports(int promiscuous) {
    int ret;
    uint8_t num_rx_queues, num_tx_queues, socket_id, queue_id;
    unsigned port_id, lcore_id, queue;
    struct lcore_conf *conf;
    struct rte_eth_link link;
    struct ether_addr eth_addr;
    uint32_t num_ports = rte_eth_dev_count();

    lcore_id = rte_get_master_lcore();
    socket_id = rte_lcore_to_socket_id(lcore_id);
    conf = &lcore_conf_[lcore_id];

    for (port_id = 0; port_id < num_ports; port_id++) {

	RTE_LOG(INFO, STREAMS_SOURCE, "Init port %u\n", port_id);
	rte_eth_macaddr_get(port_id, &eth_addr);
	RTE_LOG(INFO, STREAMS_SOURCE, 
		"  Addr: %02x:%02x:%02x:%02x:%02x:%02x\n", 
		eth_addr.addr_bytes[0],
		eth_addr.addr_bytes[1],
		eth_addr.addr_bytes[2],
		eth_addr.addr_bytes[3],
		eth_addr.addr_bytes[4],
		eth_addr.addr_bytes[5]);
	RTE_LOG(INFO, STREAMS_SOURCE, "  Socket: %d\n", 
		rte_eth_dev_socket_id(port_id));

	num_rx_queues = 1; // TODO make this a param on the operator
	num_tx_queues = 1;
	RTE_LOG(INFO, STREAMS_SOURCE, "  num_rxq: %d\n", num_rx_queues);
	RTE_LOG(INFO, STREAMS_SOURCE, "  num_txq: %d\n", num_tx_queues);
	ret = rte_eth_dev_configure(port_id, num_rx_queues, num_tx_queues,
		&port_conf_);
	if (ret < 0) {
	    rte_exit(EXIT_FAILURE, "cannot configure device %u: err=%d\n",
		    port_id, ret);
	}

	/* Initialize tx queues for the port */
	queue_id = 0;
	ret = rte_eth_tx_queue_setup(port_id, queue_id, num_txd_,
		socket_id, &tx_conf_);
	if (ret < 0) {
	    rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d"
		    " port=%u\n", ret, port_id);
	}

	conf->tx_queue_id[port_id] = queue_id;
    }

    /* Initialize rx queues */
    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
	if (rte_lcore_is_enabled(lcore_id) == 0)
	    continue;

	conf = &lcore_conf_[lcore_id];
	RTE_LOG(INFO, STREAMS_SOURCE, "Init rx queues on lcore %u num_rx_queue: %d\n", lcore_id, conf->num_rx_queue);
	for (queue = 0; queue < conf->num_rx_queue; queue++) {
	    port_id = conf->rx_queue_list[queue].port_id;
	    queue_id = conf->rx_queue_list[queue].queue_id;
	    socket_id = rte_lcore_to_socket_id(lcore_id);

	    RTE_LOG(INFO, STREAMS_SOURCE, "  port: %d, queue: %d, socket: %d\n", 
		    port_id, queue_id, socket_id);
	    RTE_LOG(INFO, STREAMS_SOURCE, "  num_rxd_: %d, mempool: 0x%lx\n", 
		    num_rxd_, socket_mempool_[socket_id]) ;
	    ret = rte_eth_rx_queue_setup(port_id, queue_id, num_rxd_,
		    socket_id, &rx_conf_, socket_mempool_[socket_id]);
	    if (ret < 0) {
		rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: err=%d"
			" port=%u\n", ret, port_id);
	    }
	}
    }

    /* Start ports */
    for (port_id = 0; port_id < num_ports; port_id++) {

	RTE_LOG(INFO, STREAMS_SOURCE, 
		"Start port %d\n", port_id);
	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
	    rte_exit(EXIT_FAILURE, 
		    "rte_eth_dev_start: err=%d port=%u\n",
		    ret, port_id);
	}

	rte_eth_link_get(port_id, &link);
	if (link.link_status) {
	    RTE_LOG(INFO, STREAMS_SOURCE, 
		    "  Link up.  Speed %u Mbps %s\n", 
		    (unsigned) link.link_speed,
		    (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
		    ("full-duplex") : ("half-duplex\n")
		   );
	} else {
	    RTE_LOG(INFO, STREAMS_SOURCE, "  Link down.\n");
	}

	if (promiscuous) rte_eth_promiscuous_enable(port_id);
    }
}

// TODO add some validation
static int validate_port_config(void) {
    return 0;
}

int init(int promiscuous) {
    int ret = -1, i ;

    if (validate_port_config() < 0)
	goto exit;

    /* Assign socket id's */
    for (i = 0; i < RTE_MAX_LCORE; ++i) {
	if (rte_lcore_is_enabled(i) == 0)
	    continue;
	lcore_conf_[i].socket_id = rte_lcore_to_socket_id(i);
    }

    rte_srand(rte_rdtsc());

    init_pools();
    init_ports(promiscuous);
    ret = 0;

exit:
    return ret;
}
