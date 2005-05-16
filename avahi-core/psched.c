/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "util.h"
#include "psched.h"

#define AVAHI_QUERY_HISTORY_MSEC 100
#define AVAHI_QUERY_DEFER_MSEC 100
#define AVAHI_RESPONSE_HISTORY_MSEC 700
#define AVAHI_RESPONSE_DEFER_MSEC 20
#define AVAHI_RESPONSE_JITTER_MSEC 100
#define AVAHI_PROBE_DEFER_MSEC 70

AvahiPacketScheduler *avahi_packet_scheduler_new(AvahiServer *server, AvahiInterface *i) {
    AvahiPacketScheduler *s;

    g_assert(server);
    g_assert(i);

    s = g_new(AvahiPacketScheduler, 1);
    s->server = server;
    s->interface = i;

    AVAHI_LLIST_HEAD_INIT(AvahiQueryJob, s->query_jobs);
    AVAHI_LLIST_HEAD_INIT(AvahiResponseJob, s->response_jobs);
    AVAHI_LLIST_HEAD_INIT(AvahiKnownAnswer, s->known_answers);
    AVAHI_LLIST_HEAD_INIT(AvahiProbeJob, s->probe_jobs);
    
    return s;
}

static void query_job_free(AvahiPacketScheduler *s, AvahiQueryJob *qj) {
    g_assert(qj);

    if (qj->time_event)
        avahi_time_event_queue_remove(qj->scheduler->server->time_event_queue, qj->time_event);

    AVAHI_LLIST_REMOVE(AvahiQueryJob, jobs, s->query_jobs, qj);
    
    avahi_key_unref(qj->key);
    g_free(qj);
}

static void response_job_free(AvahiPacketScheduler *s, AvahiResponseJob *rj) {
    g_assert(rj);

    if (rj->time_event)
        avahi_time_event_queue_remove(rj->scheduler->server->time_event_queue, rj->time_event);

    AVAHI_LLIST_REMOVE(AvahiResponseJob, jobs, s->response_jobs, rj);

    avahi_record_unref(rj->record);
    g_free(rj);
}

static void probe_job_free(AvahiPacketScheduler *s, AvahiProbeJob *pj) {
    g_assert(pj);

    if (pj->time_event)
        avahi_time_event_queue_remove(pj->scheduler->server->time_event_queue, pj->time_event);

    AVAHI_LLIST_REMOVE(AvahiProbeJob, jobs, s->probe_jobs, pj);

    avahi_record_unref(pj->record);
    g_free(pj);
}

void avahi_packet_scheduler_free(AvahiPacketScheduler *s) {
    AvahiQueryJob *qj;
    AvahiResponseJob *rj;
    AvahiProbeJob *pj;

    g_assert(s);

    g_assert(!s->known_answers);
    
    while ((qj = s->query_jobs))
        query_job_free(s, qj);
    while ((rj = s->response_jobs))
        response_job_free(s, rj);
    while ((pj = s->probe_jobs))
        probe_job_free(s, pj);

    g_free(s);
}

static gpointer known_answer_walk_callback(AvahiCache *c, AvahiKey *pattern, AvahiCacheEntry *e, gpointer userdata) {
    AvahiPacketScheduler *s = userdata;
    AvahiKnownAnswer *ka;
    
    g_assert(c);
    g_assert(pattern);
    g_assert(e);
    g_assert(s);

    if (avahi_cache_entry_half_ttl(c, e))
        return NULL;
    
    ka = g_new0(AvahiKnownAnswer, 1);
    ka->scheduler = s;
    ka->record = avahi_record_ref(e->record);

    AVAHI_LLIST_PREPEND(AvahiKnownAnswer, known_answer, s->known_answers, ka);
    return NULL;
}

static guint8* packet_add_query_job(AvahiPacketScheduler *s, AvahiDnsPacket *p, AvahiQueryJob *qj) {
    guint8 *d;

    g_assert(s);
    g_assert(p);
    g_assert(qj);

    if ((d = avahi_dns_packet_append_key(p, qj->key, FALSE))) {
        GTimeVal tv;

        qj->done = 1;

        /* Drop query after some time from history */
        avahi_elapse_time(&tv, AVAHI_QUERY_HISTORY_MSEC, 0);
        avahi_time_event_queue_update(s->server->time_event_queue, qj->time_event, &tv);

        g_get_current_time(&qj->delivery);

        /* Add all matching known answers to the list */
        avahi_cache_walk(s->interface->cache, qj->key, known_answer_walk_callback, s);
    }

    return d;
}

static void append_known_answers_and_send(AvahiPacketScheduler *s, AvahiDnsPacket *p) {
    AvahiKnownAnswer *ka;
    guint n;
    g_assert(s);
    g_assert(p);

    n = 0;
    
    while ((ka = s->known_answers)) {

        while (!avahi_dns_packet_append_record(p, ka->record, FALSE, 0)) {

            g_assert(!avahi_dns_packet_is_empty(p));

            avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_FLAGS, avahi_dns_packet_get_field(p, AVAHI_DNS_FIELD_FLAGS) | AVAHI_DNS_FLAG_TC);
            avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_ANCOUNT, n);
            avahi_interface_send_packet(s->interface, p);
            avahi_dns_packet_free(p);

            p = avahi_dns_packet_new_query(s->interface->hardware->mtu);
            n = 0;
        }

        AVAHI_LLIST_REMOVE(AvahiKnownAnswer, known_answer, s->known_answers, ka);
        avahi_record_unref(ka->record);
        g_free(ka);
        
        n++;
    }
    
    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_ANCOUNT, n);
    avahi_interface_send_packet(s->interface, p);
    avahi_dns_packet_free(p);
}

static void query_elapse(AvahiTimeEvent *e, gpointer data) {
    AvahiQueryJob *qj = data;
    AvahiPacketScheduler *s;
    AvahiDnsPacket *p;
    guint n;
    guint8 *d;

    g_assert(qj);
    s = qj->scheduler;

    if (qj->done) {
        /* Lets remove it  from the history */
        query_job_free(s, qj);
        return;
    }

    g_assert(!s->known_answers);
    
    p = avahi_dns_packet_new_query(s->interface->hardware->mtu);
    d = packet_add_query_job(s, p, qj);
    g_assert(d);
    n = 1;

    /* Try to fill up packet with more queries, if available */
    for (qj = s->query_jobs; qj; qj = qj->jobs_next) {

        if (qj->done)
            continue;

        if (!packet_add_query_job(s, p, qj))
            break;

        n++;
    }

    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_QDCOUNT, n);

    /* Now add known answers */
    append_known_answers_and_send(s, p);
}

static AvahiQueryJob* query_job_new(AvahiPacketScheduler *s, AvahiKey *key) {
    AvahiQueryJob *qj;
    
    g_assert(s);
    g_assert(key);

    qj = g_new(AvahiQueryJob, 1);
    qj->scheduler = s;
    qj->key = avahi_key_ref(key);
    qj->done = FALSE;
    qj->time_event = NULL;
    
    AVAHI_LLIST_PREPEND(AvahiQueryJob, jobs, s->query_jobs, qj);

    return qj;
}

static AvahiQueryJob* look_for_query(AvahiPacketScheduler *s, AvahiKey *key) {
    AvahiQueryJob *qj;

    g_assert(s);
    g_assert(key);

    for (qj = s->query_jobs; qj; qj = qj->jobs_next)
        if (avahi_key_equal(qj->key, key))
            return qj;

    return NULL;
}

gboolean avahi_packet_scheduler_post_query(AvahiPacketScheduler *s, AvahiKey *key, gboolean immediately) {
    GTimeVal tv;
    AvahiQueryJob *qj;
    
    g_assert(s);
    g_assert(key);

    avahi_elapse_time(&tv, immediately ? 0 : AVAHI_QUERY_DEFER_MSEC, 0);

    if ((qj = look_for_query(s, key))) {
        glong d = avahi_timeval_diff(&tv, &qj->delivery);

        /* Duplicate questions suppression */
        if (!qj->done || d <= AVAHI_QUERY_HISTORY_MSEC*1000) {
            g_message("WARNING! DUPLICATE QUERY SUPPRESSION ACTIVE!");
            
            if (!qj->done && d < 0) {
                /* If the new entry should be scheduled earlier,
                 * update the old entry */
                qj->delivery = tv;
                avahi_time_event_queue_update(s->server->time_event_queue, qj->time_event, &qj->delivery);
            }
                
            return FALSE;
        } else
            query_job_free(s, qj);
    }
    
    qj = query_job_new(s, key);
    qj->delivery = tv;
    qj->time_event = avahi_time_event_queue_add(s->server->time_event_queue, &qj->delivery, query_elapse, qj);
    return TRUE;
}


void avahi_packet_scheduler_incoming_query(AvahiPacketScheduler *s, AvahiKey *key) {
    AvahiQueryJob *qj;
    GTimeVal tv;
    
    g_assert(s);
    g_assert(key);

    /* This function is called whenever an incoming query was
     * receieved. We mark all matching queries that match as done. The
     * keyword is "DUPLICATE QUESTION SUPPRESION". */

    if (!(qj = look_for_query(s, key)))
        qj = query_job_new(s, key);

    qj->done = TRUE;

    /* Drop the query after some time */
    avahi_elapse_time(&tv, AVAHI_QUERY_HISTORY_MSEC, 0);
    
    if (qj->time_event)
        avahi_time_event_queue_update(s->server->time_event_queue, qj->time_event, &tv);
    else
        qj->time_event = avahi_time_event_queue_add(s->server->time_event_queue, &tv, query_elapse, qj);

    g_get_current_time(&qj->delivery);
}

static guint8* packet_add_response_job(AvahiPacketScheduler *s, AvahiDnsPacket *p, AvahiResponseJob *rj) {
    guint8 *d;

    g_assert(s);
    g_assert(p);
    g_assert(rj);

    if ((d = avahi_dns_packet_append_record(p, rj->record, rj->flush_cache, 0))) {
        GTimeVal tv;

        rj->done = 1;

        /* Drop response after some time from history */
        avahi_elapse_time(&tv, AVAHI_RESPONSE_HISTORY_MSEC, 0);
        avahi_time_event_queue_update(s->server->time_event_queue, rj->time_event, &tv);

        g_get_current_time(&rj->delivery);
    }

    return d;
}

static void send_response_packet(AvahiPacketScheduler *s, AvahiResponseJob *rj) {
    AvahiDnsPacket *p;
    guint n;

    g_assert(s);

    p = avahi_dns_packet_new_response(s->interface->hardware->mtu, TRUE);
    n = 0;

    /* If a job was specified, put it in the packet. */
    if (rj) {
        guint8 *d;
        d = packet_add_response_job(s, p, rj);
        g_assert(d);
        n++;
    }

    /* Try to fill up packet with more responses, if available */
    for (rj = s->response_jobs; rj; rj = rj->jobs_next) {

        if (rj->done)
            continue;

        if (!packet_add_response_job(s, p, rj))
            break;

        n++;
    }

    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_ANCOUNT, n);
    avahi_interface_send_packet(s->interface, p);
    avahi_dns_packet_free(p);
}

static void response_elapse(AvahiTimeEvent *e, gpointer data) {
    AvahiResponseJob *rj = data;
    AvahiPacketScheduler *s;

    g_assert(rj);
    s = rj->scheduler;

    if (rj->done) {
        /* Lets remove it  from the history */
        response_job_free(s, rj);
        return;
    }

    send_response_packet(s, rj);
}

static AvahiResponseJob* look_for_response(AvahiPacketScheduler *s, AvahiRecord *record) {
    AvahiResponseJob *rj;

    g_assert(s);
    g_assert(record);

    for (rj = s->response_jobs; rj; rj = rj->jobs_next)
        if (avahi_record_equal_no_ttl(rj->record, record))
            return rj;

    return NULL;
}

static AvahiResponseJob* response_job_new(AvahiPacketScheduler *s, AvahiRecord *record) {
    AvahiResponseJob *rj;
    
    g_assert(s);
    g_assert(record);

    rj = g_new(AvahiResponseJob, 1);
    rj->scheduler = s;
    rj->record = avahi_record_ref(record);
    rj->done = FALSE;
    rj->time_event = NULL;
    rj->flush_cache = FALSE;
    rj->querier_valid = FALSE;
    
    AVAHI_LLIST_PREPEND(AvahiResponseJob, jobs, s->response_jobs, rj);

    return rj;
}

gboolean avahi_packet_scheduler_post_response(AvahiPacketScheduler *s, AvahiRecord *record, gboolean flush_cache, gboolean immediately, const AvahiAddress *querier) {
    AvahiResponseJob *rj;
    GTimeVal tv;
    
    g_assert(s);
    g_assert(record);

    g_assert(!avahi_key_is_pattern(record->key));
    
    avahi_elapse_time(&tv, immediately ? 0 : AVAHI_RESPONSE_DEFER_MSEC, immediately ? 0 : AVAHI_RESPONSE_JITTER_MSEC);
    
    /* Don't send out duplicates */
    
    if ((rj = look_for_response(s, record))) {
        
        glong d = avahi_timeval_diff(&tv, &rj->delivery);

        /* If there's already a matching packet in our history or in
         * the schedule, we do nothing. */
        
        if ((!!record->ttl == !!rj->record->ttl) &&
            (rj->flush_cache || !flush_cache) &&
            ((!rj->done && d >= 0) || (rj->done && d <= AVAHI_RESPONSE_HISTORY_MSEC*1000))) {
            
            g_message("Duplicate suppresion active.");
            return FALSE;
        }

        /* If the old job was not yet done but scheduled earlier than
         * our new one, we chedule our new job at the same time. */ 
        if (!rj->done && d > 0)
            tv = rj->delivery;

        /* If the old job had the flush_cache bit enabled, we must
           enable it on our new one, too */
        if (!rj->done && rj->flush_cache)
            flush_cache = TRUE;

        /* For known answer suppresion we have record for which host this data was intended */
        if (querier && !rj->done && (!rj->querier_valid || avahi_address_cmp(&rj->querier, querier) != 0))
            querier = NULL;
        
        /* The old job wasn't good enough, so let's drop it */
        response_job_free(s, rj);
    }

    /* Create a new job and schedule it */
    rj = response_job_new(s, record);
    rj->flush_cache = flush_cache;
    rj->delivery = tv;
    rj->time_event = avahi_time_event_queue_add(s->server->time_event_queue, &rj->delivery, response_elapse, rj);

    if ((rj->querier_valid = !!querier))
        rj->querier = *querier;

    return TRUE;
}

void response_job_set_elapse_time(AvahiPacketScheduler *s, AvahiResponseJob *rj, guint msec, guint jitter) {
    GTimeVal tv;

    g_assert(s);
    g_assert(rj);

    avahi_elapse_time(&tv, msec, jitter);

    if (rj->time_event)
        avahi_time_event_queue_update(s->server->time_event_queue, rj->time_event, &tv);
    else
        rj->time_event = avahi_time_event_queue_add(s->server->time_event_queue, &tv, response_elapse, rj);
}

void avahi_packet_scheduler_incoming_response(AvahiPacketScheduler *s, AvahiRecord *record, gboolean flush_cache) {
    AvahiResponseJob *rj = NULL;
    
    g_assert(s);
    g_assert(record);

    /* This function is called whenever an incoming response was
     * receieved. We drop all scheduled responses which match
     * here. The keyword is "DUPLICATE ANSWER SUPPRESION". */

    if ((rj = look_for_response(s, record))) {
    
        if (!rj->done) {

            if (rj->flush_cache && !flush_cache)
                /* The incoming response didn't have flush_cache
                 * set, but our scheduled has => we still have to
                 * send our response */
                return;

            
            if (!!record->ttl != !!rj->record->ttl) {
                /* Either one was a goodbye packet but other was
                 * none => we still have to send our response */
                return;
            }
        }
        
        /* The two responses match, so let's mark the history
         * entry as done or update it */
    }

    /* No matching job was found. Add the query to the history */
    if (!rj) 
        rj = response_job_new(s, record);
    else {
        avahi_record_unref(rj->record);
        rj->record = avahi_record_ref(record);
    }
    
    rj->done = TRUE;
    rj->flush_cache = rj->flush_cache || flush_cache;
                    
    /* Drop response after 500ms from history */
    response_job_set_elapse_time(s, rj, AVAHI_RESPONSE_HISTORY_MSEC, 0);

    g_get_current_time(&rj->delivery);
}


void avahi_packet_scheduler_incoming_known_answer(AvahiPacketScheduler *s, AvahiRecord *record, const AvahiAddress *querier) {
    AvahiResponseJob *rj;
    
    g_assert(s);
    g_assert(record);
    g_assert(querier);

    /* Check whether a matching job has been scheduled */
    if (!(rj = look_for_response(s, record)) || rj->done)
        return;

    /* Chech whether another querier demanded the original job */
    if (!rj->querier_valid || avahi_address_cmp(&rj->querier, querier) != 0)
        return;

    /* Check whether one of them is a goodbye packet, while the other is not */
    if (!!record->ttl != !!rj->record->ttl)
        return;

    /* Check whether the known answer has a good TTL */
    if (record->ttl <= rj->record->ttl/2)
        return;

    g_message("Known answer suppression active!");
    response_job_free(s, rj);
}

void avahi_packet_scheduler_flush_responses(AvahiPacketScheduler *s) {
    AvahiResponseJob *rj;
    
    g_assert(s);

    /* Send all scheduled responses, ignoring the scheduled time */
    
    for (rj = s->response_jobs; rj; rj = rj->jobs_next)
        if (!rj->done)
            send_response_packet(s, rj);
}

static AvahiProbeJob* probe_job_new(AvahiPacketScheduler *s, AvahiRecord *record) {
    AvahiProbeJob *pj;
    
    g_assert(s);
    g_assert(record);

    pj = g_new(AvahiProbeJob, 1);
    pj->scheduler = s;
    pj->record = avahi_record_ref(record);
    pj->time_event = NULL;
    pj->chosen = FALSE;
    
    AVAHI_LLIST_PREPEND(AvahiProbeJob, jobs, s->probe_jobs, pj);

    return pj;
}

static guint8* packet_add_probe_query(AvahiPacketScheduler *s, AvahiDnsPacket *p, AvahiProbeJob *pj) {
    guint size;
    guint8 *ret;
    AvahiKey *k;

    g_assert(s);
    g_assert(p);
    g_assert(pj);

    g_assert(!pj->chosen);
    
    /* Estimate the size for this record */
    size =
        avahi_key_get_estimate_size(pj->record->key) +
        avahi_record_get_estimate_size(pj->record);

    /* Too large */
    if (size > avahi_dns_packet_space(p))
        return NULL;

    /* Create the probe query */
    k = avahi_key_new(pj->record->key->name, pj->record->key->class, AVAHI_DNS_TYPE_ANY);
    ret = avahi_dns_packet_append_key(p, k, FALSE);
    g_assert(ret);

    /* Mark this job for addition to the packet */
    pj->chosen = TRUE;

    /* Scan for more jobs whith matching key pattern */
    for (pj = s->probe_jobs; pj; pj = pj->jobs_next) {
        if (pj->chosen)
            continue;

        /* Does the record match the probe? */
        if (k->class != pj->record->key->class || !avahi_domain_equal(k->name, pj->record->key->name))
            continue;
        
        /* This job wouldn't fit in */
        if (avahi_record_get_estimate_size(pj->record) > avahi_dns_packet_space(p))
            break;

        /* Mark this job for addition to the packet */
        pj->chosen = TRUE;
    }

    avahi_key_unref(k);
            
    return ret;
}

static void probe_elapse(AvahiTimeEvent *e, gpointer data) {
    AvahiProbeJob *pj = data, *next;
    AvahiPacketScheduler *s;
    AvahiDnsPacket *p;
    guint n;

    g_assert(pj);
    s = pj->scheduler;

    p = avahi_dns_packet_new_query(s->interface->hardware->mtu);

    /* Add the import probe */
    if (!packet_add_probe_query(s, p, pj)) {
        g_warning("Record too large! ---");
        avahi_dns_packet_free(p);
        return;
    }

    n = 1;
    
    /* Try to fill up packet with more probes, if available */
    for (pj = s->probe_jobs; pj; pj = pj->jobs_next) {

        if (pj->chosen)
            continue;
        
        if (!packet_add_probe_query(s, p, pj))
            break;
        
        n++;
    }

    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_QDCOUNT, n);

    n = 0;

    /* Now add the chosen records to the authorative section */
    for (pj = s->probe_jobs; pj; pj = next) {

        next = pj->jobs_next;

        if (!pj->chosen)
            continue;

        if (!avahi_dns_packet_append_record(p, pj->record, FALSE, 0)) {
            g_warning("Bad probe size estimate!");

            /* Unmark all following jobs */
            for (; pj; pj = pj->jobs_next)
                pj->chosen = FALSE;
            
            break;
        }

        probe_job_free(s, pj);
        
        n ++;
    }
    
    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_NSCOUNT, n);

    /* Send it now */
    avahi_interface_send_packet(s->interface, p);
    avahi_dns_packet_free(p);
}

gboolean avahi_packet_scheduler_post_probe(AvahiPacketScheduler *s, AvahiRecord *record, gboolean immediately) {
    AvahiProbeJob *pj;
    GTimeVal tv;
    
    g_assert(s);
    g_assert(record);
    g_assert(!avahi_key_is_pattern(record->key));
    
    avahi_elapse_time(&tv, immediately ? 0 : AVAHI_PROBE_DEFER_MSEC, 0);

    /* Create a new job and schedule it */
    pj = probe_job_new(s, record);
    pj->delivery = tv;
    pj->time_event = avahi_time_event_queue_add(s->server->time_event_queue, &pj->delivery, probe_elapse, pj);

    return TRUE;
}