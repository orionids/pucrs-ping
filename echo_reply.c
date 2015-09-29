#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "checksum.h"
#include "echo_request.h"
#include "echo_reply.h"
#include "constants.h"
#include "io.h"

reply_response_t wait_for_icmp_reply(int sock_fd, echo_request_t req) {
  unsigned char buffer[BUFFER_LEN*2];
  unsigned char ttl;

  for (;;) {
    recv(sock_fd,(char *) &buffer, sizeof(buffer), 0x0);

    // Capture the time we received the message so we can calculate the time
    // elapsed at the end
    struct timeval now;
    if (gettimeofday(&now, NULL) < 0) {
      printf("Error getting the current time\n");
      exit(1);
    }

    // Check if destination MAC address is our interface
    int i, match = 1;
    for (i = 0; i < MAC_ADDR_LEN; i++) {
      if (buffer[i] != req.local_mac[i]) {
        match = 0;
        break;
      }
    }

    printf("match: %d\n", match);

    if (match != 1)
      continue;

    unsigned char *bufferptr = buffer;

    // Move past the 2 MAC addresses on the ethernet header
    bufferptr += 2 * MAC_ADDR_LEN;

    // Check if the packet type is an IPv4
    short ether_type;
    CONSUME_BYTES(bufferptr, &ether_type, sizeof(ether_type));
    printf("ether type: %d\n", ether_type);
    if (ether_type != 8) // IP
      continue;

    // Reference to where the IP section starts
    unsigned char *ip_start_ptr = bufferptr;

    // Check if is V4 and has 20 bytes on the header
    char ip_version_and_header_length;
    CONSUME_BYTES(bufferptr, &ip_version_and_header_length, sizeof(ip_version_and_header_length));
    printf("version and header length: %x\n", ip_version_and_header_length);
    if (ip_version_and_header_length != 0x45)
      continue;

    // Move forward 1 byte that represents some flags we don't care about
    bufferptr += 1;

    // Total length of the packet
    unsigned short total_packet_length;
    CONSUME_BYTES(bufferptr, &total_packet_length, sizeof(total_packet_length));
    total_packet_length = ntohs(total_packet_length);
    printf("  - total packet length %d\n", total_packet_length);

    // Move forward some bytes that represents some stuff we don't care about
    bufferptr += 4; // 2 for packet ID, 2 for flags and offset

    // Extract ttl
    CONSUME_BYTES(bufferptr, &ttl, sizeof(ttl));
    printf("  - ttl %d\n", ttl);

    // Check if the protocol is ICMP
    unsigned char protocol;
    CONSUME_BYTES(bufferptr, &protocol, sizeof(protocol));
    printf("  - protocol %d\n", protocol);
    if (protocol != 1)
      continue;

    // Is the checksum for the IP header correct?
    unsigned short checksum;
    unsigned char *checksum_ptr = bufferptr;
    CONSUME_BYTES(bufferptr, &checksum, sizeof(checksum));
    checksum = ntohs(checksum);
    printf("  - checksum %x\n", checksum);
    // Go back to the checksum start and zero it out in order to validate the message
    memset(checksum_ptr, 0, sizeof(checksum));
    // Calculate the checksum with the info we have
    unsigned short calculated_checksum = in_cksum((short unsigned int *)ip_start_ptr, IP_HEADER_LEN);
    calculated_checksum = ntohs(calculated_checksum);
    printf("  - calculated checksum %x\n", calculated_checksum);

    if (checksum != calculated_checksum)
      continue;

    // Source IP
    unsigned char source_ip[IP_ADDR_LEN];
    CONSUME_BYTES(bufferptr, source_ip, IP_ADDR_LEN * sizeof(unsigned char));
    printf("  - source ip %d.%d.%d.%d\n", source_ip[0], source_ip[1], source_ip[2], source_ip[3]);

    // Target IP (us)
    unsigned char target_ip[IP_ADDR_LEN];
    CONSUME_BYTES(bufferptr, target_ip, IP_ADDR_LEN * sizeof(unsigned char));
    printf("  - target ip %d.%d.%d.%d\n", target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
    // Does the IP match?
    for (i = 0; i < IP_ADDR_LEN; i++) {
      if (target_ip[i] != req.local_ip[i]) {
        match = 0;
        break;
      }
    }
    if (match != 1)
      continue;

    // Reference to where the ICMP section starts
    unsigned char *icmp_start_ptr = bufferptr;

    // Is it an echo reply with code = 0?
    unsigned char icmp_type;
    CONSUME_BYTES(bufferptr, &icmp_type, sizeof(icmp_type));
    printf("  - ICMP type: %d\n", icmp_type);
    if (icmp_type != 0)
      continue;
    unsigned char code;
    CONSUME_BYTES(bufferptr, &code, sizeof(code));
    printf("  - Code: %d\n", code);
    if (code != 0)
      continue;

    // Is the checksum for the ICMP header correct?
    checksum_ptr = bufferptr;
    CONSUME_BYTES(bufferptr, &checksum, sizeof(checksum));
    checksum = ntohs(checksum);
    printf("  - ICMP checksum %x\n", checksum);
    // Go back to the checksum start and zero it out in order to validate the message
    memset(checksum_ptr, 0, sizeof(checksum));
    // Calculate the checksum with the info we have
    calculated_checksum = in_cksum((short unsigned int *)icmp_start_ptr, total_packet_length - 20);
    calculated_checksum = ntohs(calculated_checksum);
    printf("  - ICMP calculated checksum %x\n", calculated_checksum);
    if (checksum != calculated_checksum)
      continue;

    // Identifier matches?
    unsigned short identifier;
    CONSUME_BYTES(bufferptr, &identifier, sizeof(identifier));
    identifier = ntohs(identifier);
    printf("  - ICMP identifier %x\n", identifier);
    if (identifier != req.identifier) {
      continue;
    }

    // Sequence number matches?
    unsigned short sequence_number;
    CONSUME_BYTES(bufferptr, &sequence_number, sizeof(sequence_number));
    sequence_number = ntohs(sequence_number);
    printf("  - ICMP sequence_number %x\n", sequence_number);
    if (sequence_number != req.sequence_number) {
      continue;
    }

    // It is a reply, so we calculate the time elapsed and break out of the loop
    printf("  - Packet time: %lu.%lu\n", req.sent_at.tv_sec, req.sent_at.tv_usec);
    printf("  - Now time:    %lu.%lu\n", now.tv_sec, now.tv_usec);

    long sec_diff  = now.tv_sec - req.sent_at.tv_sec;
    long usec_diff = now.tv_usec - req.sent_at.tv_usec;

    if (sec_diff > 0) {
      usec_diff += sec_diff * 100000;
    }

    printf("  - Diff: %0.3fms\n", usec_diff / 1000.0);
    break;
  }

  reply_response_t res = {
    .success = 1
  };

  return res;
}
