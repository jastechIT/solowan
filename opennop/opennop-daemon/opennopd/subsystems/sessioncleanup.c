/*

  sessioncleanup.c

  This file is part of OpenNOP-SoloWAN distribution.
  No modifications made from the original file in OpenNOP distribution.

  Copyright (C) 2014 OpenNOP.org (yaplej@opennop.org)

    OpenNOP is an open source Linux based network accelerator designed 
    to optimise network traffic over point-to-point, partially-meshed and 
    full-meshed IP networks.

  References:

    OpenNOP: http://www.opennop.org

  License:

    OpenNOP is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenNOP is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // for sleep function

#include <sys/time.h> 

#include <netinet/ip.h> // for tcpmagic and TCP options
#include <netinet/tcp.h> // for tcpmagic and TCP options

#include <linux/types.h>

#include "sessioncleanup.h"
#include "sessionmanager.h"
#include "opennopd.h"
#include "packet.h"
#include "tcpoptions.h"
#include "csum.h"
#include "logger.h"

int rawsock = 0; // Used to send keep-alive messages.

/*
 * This sends a keep alive message to the specified IP.
 * It does not tag the packet with the Accelerator ID this
 * prevents the keepalive messages from recreating dead sessions
 * in remote Accelerators.
 */
void sendkeepalive
(__u32 saddr, __u16 source, __u32 seq,
__u32 daddr, __u16 dest, __u32 ack_seq){
	char packet[BUFSIZE];
	struct iphdr *iph = NULL;
	struct tcphdr *tcph = NULL;
	struct sockaddr_in sin, din;
	__u16 tcplen;

	memset(packet, 0, BUFSIZE);

	sin.sin_family = AF_INET;
	din.sin_family = AF_INET;
	
	sin.sin_port = source;
	din.sin_port = dest;
	
	sin.sin_addr.s_addr = saddr;
	din.sin_addr.s_addr = daddr;

	iph = (struct iphdr *)packet;
	iph->ihl = 5; // IP header length.
	iph->version = 4; // IP version 4.
	iph->tos = 0; // No TOS.
	iph->tot_len=htons(sizeof(struct iphdr) + sizeof(struct tcphdr)); // L3 + L4 header length.
	iph->id = 0; // What?
	iph->frag_off = 0; // No fragmenting.
	iph->ttl = 64; // Set a TTL.
	iph->protocol = IPPROTO_TCP; // TCP protocol.
	iph->check = 0; // No IP checksum yet.
	iph->saddr = saddr; // Source IP.
	iph->daddr = daddr; // Dest IP.
	
	tcph = (struct tcphdr *) (((u_int32_t *)iph) + iph->ihl);
	tcph->check = 0; // No TCP checksum yet.
	tcph->source = source; // Source TCP Port.
	tcph->dest = dest; // Destination TCP Port.
	tcph->seq = htonl(seq - 1); // Current SEQ minus one is used for TCP keepalives.
	tcph->ack_seq = htonl( ack_seq - 1); // Ummm not sure yet.
	tcph->res1 = 0; // Not sure.
	tcph->doff = 5; // TCP Offset.  At least 5 if there are no TCP options.
	tcph->fin = 0; // FIN flag.
	tcph->syn = 0; // SYN flag.
	tcph->rst = 0; // RST flag.
	tcph->psh = 0; // PSH flag.
	tcph->ack = 1; // ACK flag.
	tcph->urg = 0; // URG flag.
	
	__set_tcp_option((__u8 *)iph,32,6,localID); // Add the Accelerator ID to this packet.
	
	tcplen = ntohs(iph->tot_len) - iph->ihl*4;
	tcph->check = 0;
	tcph->check = tcp_sum_calc(tcplen,
		(unsigned short *)&iph->saddr,
		(unsigned short *)&iph->daddr,
		(unsigned short *)tcph);
 	iph->check = 0;
 	iph->check = ip_sum_calc(iph->ihl*4,
		(unsigned short *)iph);
	
	
	if(sendto(rawsock, packet, ntohs(iph->tot_len), 0, (struct sockaddr *)&din, sizeof(din)) < 0){
		printf("sendto() error\n");
	}
	
	return;
}

/*
 * This function goes through All the session of a single list
 * looking for idle sessions.  When it locates one it
 * send a keepalive message to both client/server.  If they
 * fail to respond to the keepalive messages twice the session
 * is removed from its list. 
 */
void cleanuplist (struct session_head *currentlist){
	struct session *currentsession = NULL;
	struct timeval tv; // Used to get the system time.
	__u64 currenttime; // The current time in seconds.
	
	gettimeofday(&tv,NULL); // Get the time from hardware.
	currenttime = tv.tv_sec - 60; // Set the currenttime minus one minuet.
	
	if (currentlist->next != NULL){ // Make sure there is something in the list.
		currentsession = currentlist->next; // Make the first session of the list the current.
	}
	else{ // No sessions in this list.
		return; 
	}
	
	if (currentsession != NULL){ // Make sure there is a session to work on.
	
		while (currentsession != NULL){ // Do this for all the sessions in the list.
			
			if (currentsession->deadcounter > 2){ // This session has failed to respond it is dead.
				
				if (currentsession->next != NULL){ // Check that next sessions isnt NULL;
					currentsession = currentsession->next; // Advance to the next session.
					clearsession(currentsession->prev); // Clear the previous session.
				}
				else{ // This was the last session of the list.
					clearsession(currentsession);
					currentsession = NULL;
				}
			}
			else{ // Session needs checked for idle time.
				
				if (currentsession->lastactive < currenttime){ // If this session is not active.
					++currentsession->deadcounter; // Increment the deadcounter.
					sendkeepalive(currentsession->largerIP, currentsession->largerIPPort, currentsession->largerIPseq,
									currentsession->smallerIP, currentsession->smallerIPPort, currentsession->smallerIPseq);
									
					sendkeepalive(currentsession->smallerIP, currentsession->smallerIPPort, currentsession->smallerIPseq,
									currentsession->largerIP, currentsession->largerIPPort, currentsession->largerIPseq);
					 
				}
	
				if (currentsession->next != NULL){ // Check if there are more sessions.
					currentsession = currentsession->next; // Advance to the next session.
				}
				else{ // Reached end of the session list.
					currentsession = NULL;
				}
			}	
		}
	}
}

/*
 * This function runs every 5 or so minuets.
 * It checks each bucket for idle sessions, and
 * will remove the dead ones using the previous function.
 */
void *cleanup_function(void *data){
	int one = 1;
	const int *val = &one;
	char message [LOGSZ];
	__u32 i = 0;
	
	rawsock = socket(PF_INET, SOCK_RAW, IPPROTO_TCP);
	
	if (rawsock < 0){
		sprintf(message, "Initialization: Error opening raw socket.\n");
		logger(LOG_INFO, message);
		exit(EXIT_FAILURE);
	}
	
	if(setsockopt(rawsock, IPPROTO_IP, IP_HDRINCL, val, sizeof(one)) < 0){
		sprintf(message, "Initialization: Error setting socket options.\n");
		logger(LOG_INFO, message);
		exit(EXIT_FAILURE);
	}
	
	while (servicestate >= STOPPING) {
		sleep(300); // Sleeping for 5 minuets.
		
		for (i = 0; i < SESSIONBUCKETS; i++){  // Process each bucket.
		
			if (getsessionhead(i)->next != NULL){
    			cleanuplist(getsessionhead(i));
    		}
		}
	}
	
	/*
	 * Free any allocated resources before exiting
	 */
	close(rawsock); // close the socket used for sending keepalives.
	rawsock = 0;
        
	return NULL;
}
