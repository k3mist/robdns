#include "db-zone.h"
#include "db-entry.h"
#include "db-rrset.h"
#include "zonefile-rr.h"
#include "domainname.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>



/****************************************************************************
 ****************************************************************************/
struct DBZone
{
	uint64_t hash;
	struct DBZone *next;
    struct DomainPointer domain;
    unsigned label_count;

    struct {
        unsigned shortest;
        unsigned longest;
    } wildcard;
    struct {
        unsigned shortest;
        unsigned longest;
    } delegation;

    unsigned entry_count;
    unsigned entry_mask;
    struct DBEntry **records;
    unsigned char domain_buffer[256];
};



/****************************************************************************
 ****************************************************************************/
static unsigned
domain_count_labels(const struct DomainPointer *domain)
{
    unsigned label_count = 0;
    unsigned i;

    for (i=0; i<domain->length; i += domain->name[i] + 1)
        label_count++;

    return label_count;
}


/****************************************************************************
 ****************************************************************************/
void
zone_create_record(
    struct DBZone *zone,
    const struct DB_XDomain *xdomain,
    unsigned type,
    unsigned ttl,
    unsigned rdlength,
    const unsigned char *rdata)
{
    uint64_t hash_index;
    unsigned zone_label_count;

    /* Find the number of labels in the zone domain name*/
    zone_label_count = domain_count_labels(&zone->domain);

    /* */
    if (xdomain_is_wildcard(xdomain)) {
        if (zone->wildcard.shortest >= zone_label_count)
            zone->wildcard.shortest = zone_label_count;
        if (zone->wildcard.longest <= zone_label_count)
            zone->wildcard.longest = zone_label_count;
    }
    if (type == TYPE_NS) {
        if (zone->delegation.shortest >= zone_label_count)
            zone->delegation.shortest = zone_label_count;
        if (zone->delegation.longest <= zone_label_count)
            zone->delegation.longest = zone_label_count;
    }

    
    /* Find the hash index. This is the final hash value of all the labels.
     * If we are processing an '@' name (i.e. "example.com" record within
     * the "example.com" domain, this will equal the hash value of the zone*/
    hash_index = xdomain->hash;
    if (zone_label_count == xdomain->label_count) {
        ; //assert(hash_index == zone->hash);
    }

    /* Now finish the creation of the label */
    entry_create_self(
                &zone->records[hash_index & zone->entry_mask],
                xdomain, 
                zone_label_count, 
                type, ttl, rdlength, rdata);
}
void zone_create_record2(struct DBZone *zone,
    struct DomainPointer domain,
    struct DomainPointer origin,
    unsigned type,
    unsigned ttl,
    unsigned rdlength,
    const unsigned char *rdata
    )
{
    struct DB_XDomain xdomain[1];
    xdomain_reverse3(xdomain, &domain, &origin);
    zone_create_record(zone, xdomain,
        type, ttl, rdlength, rdata);

}

/****************************************************************************
 ****************************************************************************/
const struct DBEntry *
zone_lookup_wildcard(const struct DBZone *zone, const struct DB_XDomain *xdomain)
{
    unsigned i;
    struct DB_XDomain wdomain;

    wdomain.label_count = xdomain->label_count;
    memcpy(wdomain.labels, xdomain->labels, sizeof(wdomain.labels[0])*wdomain.label_count);

    for (i=zone->wildcard.longest; i>=zone->wildcard.shortest; i--) {
        uint64_t hash;
        const struct DBEntry *record;

        if (wdomain.label_count <= i)
            continue;
        
        wdomain.labels[i].name = (const unsigned char *)"\1*";
        hash = xdomain_label_hash(&wdomain, i);
        wdomain.labels[i].hash = hash;
        wdomain.hash = hash;

        record = entry_find(
                    zone->records[hash & zone->entry_mask],
                    &wdomain,
                    zone->label_count,
                    i+1);
        if (record)
            return record;
    }
    return 0;
}


/****************************************************************************
 ****************************************************************************/
const struct DBEntry *
zone_lookup_delegation(const struct DBZone *zone, const struct DB_XDomain *xdomain)
{
    unsigned i;

    /* Walk backwards looking for a "cut" (i.e. an NS record pointing
     * to a different domain */
    for (i=zone->delegation.longest; i>=zone->delegation.shortest; i--) {
        const struct DBEntry *record;
        uint64_t hash;

        hash = xdomain->labels[i].hash;

        record = entry_find(
                    zone->records[hash & zone->entry_mask],
                    xdomain,
                    zone->label_count,
                    i);
        if (!entry_is_delegation(record))
            continue;

        if (record)
            return record;
    }
    return 0;
}

/****************************************************************************
 ****************************************************************************/
const struct DBEntry *
zone_lookup_exact(const struct DBZone *zone, const struct DB_XDomain *xdomain)
{
    uint64_t hash_index;

    /* Find the hash index. This is the final hash value of all the labels.
     * If we are processing an '@' name (i.e. "example.com" record within
     * the "example.com" domain, this will equal the hash value of the zone*/
    hash_index = xdomain->hash;
    if (zone->label_count == xdomain->label_count) {
        assert(hash_index == zone->hash);
    }

    return entry_find(
                zone->records[hash_index & zone->entry_mask],
                xdomain,
                zone->label_count,
                xdomain->label_count);
}
const struct DBEntry *
zone_lookup_exact2(const struct DBZone *zone, const unsigned char *name, unsigned length)
{
    struct DB_XDomain xdomain[1];
    xdomain_reverse2(xdomain, name, length);
    return zone_lookup_exact(zone, xdomain);
}

/****************************************************************************
 ****************************************************************************/
const struct DBEntry *
zone_lookup_self(const struct DBZone *zone)
{
    struct DB_XDomain xdomain[1];

    xdomain_reverse3(xdomain, &zone->domain, 0);

    return zone_lookup_exact(zone, xdomain);
}

/****************************************************************************
 ****************************************************************************/
void
zone_name_from_record(const struct DBZone *zone, const struct DBEntry *record, struct DomainPointer *name, struct DomainPointer *origin)
{
    origin->name = zone->domain.name;
    origin->length = zone->domain.length;

    if (record)
        *name = entry_name(record);
    else {
        name->name = 0;
        name->length = 0;
    }
}
void
zone_name(const struct DBZone *zone, struct DomainPointer *origin)
{
    origin->name = zone->domain.name;
    origin->length = zone->domain.length;
}



/****************************************************************************
 * Given a zone, find the "SOA" record associated with that zone.
 * TODO: right now, this just does a name lookup, which is slow. In the
 *  future I plan to copy the SOA record to the zone itself, to optimize
 *  lookups.
 ****************************************************************************/
const struct DBrrset *
zone_get_soa_rr(const struct DBZone *zone)
{
    const struct DBrrset *rrset;
    const struct DBEntry *record;

    record = zone_lookup_self(zone);
    rrset = rrset_first(record, TYPE_SOA);

    return rrset;
}


/****************************************************************************
 ****************************************************************************/
uint64_t pow2(uint64_t x)
{
    uint64_t result = 1;

    while (result < x)
        result *= 2;

    return result;
}

/****************************************************************************
 ****************************************************************************/
struct DBZone *
zone_create_self(
    const struct DB_XDomain *xdomain,
    struct DBZone *next,
    uint64_t filesize)
{
	struct DBZone *zone;
	uint64_t hash = xdomain->hash;

	/* Fill in the zone */
    zone = (struct DBZone*)malloc(sizeof(*zone));
    memset(zone, 0, sizeof(*zone));
    zone->hash = hash;
    zone->domain.name = zone->domain_buffer;
	xdomain_copy(xdomain, &zone->domain);
    zone->wildcard.longest = 0;
    zone->wildcard.shortest = UINT_MAX;
    zone->delegation.longest = 0;
    zone->delegation.shortest = UINT_MAX;

    /* Allocate space for records */
    zone->entry_count = (unsigned)pow2(filesize/64);
    //fprintf(stderr, "%u zone entry count\n", zone->entry_count);
    xdomain_err(xdomain, ": initial entries %u\n", zone->entry_count);
    zone->entry_mask = zone->entry_count - 1;
    zone->records = (struct DBEntry **)malloc(zone->entry_count * sizeof(zone->records[0]));
    if (zone->records == NULL) {
        xdomain_err(xdomain, ": allocation failed\n");
        exit(1);
    }
    memset(zone->records, 0, zone->entry_count * sizeof(zone->records[0]));
    
    /* Insert into our hash table */
    zone->next = next;
    zone->label_count = domain_count_labels(&zone->domain);

    return zone;
}

/****************************************************************************
 ****************************************************************************/
struct DBZone *
zone_follow_chain(struct DBZone *zone, const struct DB_XDomain *xdomain, unsigned max_labels)
{
    /* Resolve hash colisions */
	for (; zone != NULL; zone = zone->next) {
		if (xdomain_is_equal(xdomain, &zone->domain, max_labels))
			break;
	}
    return zone;
}

/****************************************************************************
 ****************************************************************************/
const struct DBEntry *
zone_entry_by_index(const struct DBZone *zone, unsigned index)
{
    if (index >= zone->entry_count)
        return 0;

    return zone->records[index];
}
