/*
 * answer.c -- manipulating query answers and encoding them.
 *
 * Erik Rozendaal, <erik@nlnetlabs.nl>
 *
 * Copyright (c) 2001, 2002, 2003, NLnet Labs. All rights reserved.
 *
 * This software is an open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <config.h>

#include <string.h>

#include "answer.h"
#include "dns.h"

void
answer_init(answer_type *answer)
{
	answer->rrset_count = 0;
}

void
answer_add_rrset(answer_type *answer, answer_section_type section,
		 domain_type *domain, rrset_type *rrset)
{
	size_t i;
	
	assert(section >= ANSWER_SECTION && section <= ADDITIONAL_SECTION);
	assert(domain);
	assert(rrset);

	/* Don't add an RRset multiple times.  */
	for (i = 0; i < answer->rrset_count; ++i) {
		if (answer->rrsets[i] == rrset) {
			if (section < answer->section[i])
				answer->section[i] = section;
			return;
		}
	}
	
	if (answer->rrset_count >= MAXRRSPP) {
		/* XXX: Generate warning/error? */
		return;
	}
	
	answer->section[answer->rrset_count] = section;
	answer->domains[answer->rrset_count] = domain;
	answer->rrsets[answer->rrset_count] = rrset;
	++answer->rrset_count;
}

static void
encode_dname(struct query *q, domain_type *domain)
{
	while (domain->parent && query_get_dname_offset(q, domain) == 0) {
		query_put_dname_offset(q, domain, q->iobufptr - q->iobuf);
		DEBUG(DEBUG_NAME_COMPRESSION, 1,
		      (stderr, "dname: %s, number: %lu, offset: %u\n",
		       dname_to_string(domain_dname(domain)),
		       (unsigned long) domain->number,
		       query_get_dname_offset(q, domain)));
		query_write(q,
			    dname_name(domain_dname(domain)),
			    label_length(dname_name(domain_dname(domain))) + 1U);
		domain = domain->parent;
	}
	if (domain->parent) {
		DEBUG(DEBUG_NAME_COMPRESSION, 1,
		      (stderr, "dname: %s, number: %lu, pointer: %u\n",
		       dname_to_string(domain_dname(domain)),
		       (unsigned long) domain->number,
		       query_get_dname_offset(q, domain)));
		query_write_u16(q, htons(0xc000 | query_get_dname_offset(q, domain)));
	} else {
		query_write_u8(q, 0);
	}
}

int
encode_rr(struct query *q, domain_type *owner, rrset_type *rrset, uint16_t rr)
{
	uint8_t *truncation_point = q->iobufptr;
	uint16_t rdlength = 0;
	uint8_t *rdlength_pos;
	uint16_t j;
	
	assert(q);
	assert(owner);
	assert(rrset);
	assert(rr < rrset->rrslen);

	encode_dname(q, owner);
	query_write_u16(q, htons(rrset->type));
	query_write_u16(q, htons(rrset->class));
	query_write_u32(q, htonl(rrset->rrs[rr]->ttl));

	/* Reserve space for rdlength. */
	rdlength_pos = q->iobufptr;
	q->iobufptr += sizeof(rdlength);

	for (j = 0; !rdata_atom_is_terminator(rrset->rrs[rr]->rdata[j]); ++j) {
		if (rdata_atom_is_domain(rrset->type, j)) {
			encode_dname(q, rdata_atom_domain(rrset->rrs[rr]->rdata[j]));
		} else {
			query_write(q,
				    rdata_atom_data(rrset->rrs[rr]->rdata[j]),
				    rdata_atom_size(rrset->rrs[rr]->rdata[j]));
		}
	}

	if (!query_overflow(q)) {
		rdlength = htons(q->iobufptr - rdlength_pos - sizeof(rdlength));
		copy_uint16(rdlength_pos, rdlength);
		return 1;
	} else {
		q->iobufptr = truncation_point;
		query_clear_dname_offsets(q);
		assert(!query_overflow(q));
		return 0;
	}
}

static int
encode_rrset(struct query *q, uint16_t *count, domain_type *owner, rrset_type *rrset,
	       int truncate)
{
	uint16_t i;
	uint8_t *truncation_point = q->iobufptr;
	uint16_t added = 0;
	int all_added = 1;
	rrset_type *rrsig;
	
	assert(rrset->rrslen > 0);

	for (i = 0; i < rrset->rrslen; ++i) {
		if (encode_rr(q, owner, rrset, i)) {
			++added;
		} else {
			all_added = 0;
			if (truncate) {
				/* Truncate entire RRset and set truncate flag.  */
				q->iobufptr = truncation_point;
				TC_SET(q);
				added = 0;
				query_clear_dname_offsets(q);
			}
			break;
		}
	}

	if (all_added &&
	    q->dnssec_ok &&
	    zone_is_secure(rrset->zone) &&
	    rrset->type != TYPE_RRSIG &&
	    (rrsig = domain_find_rrset(owner, rrset->zone, TYPE_RRSIG)))
	{
		for (i = 0; i < rrsig->rrslen; ++i) {
			if (rrset_rrsig_type_covered(rrsig, i) == rrset->type) {
				if (encode_rr(q, owner, rrsig, i)) {
					++added;
				} else {
					all_added = 0;
					if (truncate) {
						/* Truncate entire RRset and set truncate flag.  */
						q->iobufptr = truncation_point;
						TC_SET(q);
						added = 0;
						query_clear_dname_offsets(q);
					}
					break;
				}
			}
		}
	}
	
	(*count) += added;

	return all_added;
}

void
encode_answer(struct query *q, const answer_type *answer)
{
	uint16_t counts[ADDITIONAL_SECTION + 1];
	answer_section_type section;
	size_t i;

	for (section = ANSWER_SECTION; section <= ADDITIONAL_SECTION; ++section) {
		counts[section] = 0;
		for (i = 0; !query_overflow(q) && i < answer->rrset_count; ++i) {
			if (answer->section[i] == section) {
				int truncate = (section == ANSWER_SECTION
						|| section == AUTHORITY_SECTION);
				encode_rrset(q, &counts[section],
					       answer->domains[i],
					       answer->rrsets[i],
					       truncate);
			}
		}
	}

	ANCOUNT(q) = htons(counts[ANSWER_SECTION]);
	NSCOUNT(q) = htons(counts[AUTHORITY_SECTION]);
	ARCOUNT(q) = htons(counts[ADDITIONAL_SECTION]);
}
