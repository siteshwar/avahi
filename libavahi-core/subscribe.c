#include "subscribe.h"
#include "util.h"

static void elapse(AvahiTimeEvent *e, void *userdata) {
    AvahiSubscription *s = userdata;
    GTimeVal tv;
    gchar *t;
    
    g_assert(s);

    avahi_server_post_query(s->server, s->interface, s->protocol, s->key);

    if (s->n_query++ <= 8)
        s->sec_delay *= 2;

    g_message("%i. Continuous querying for %s", s->n_query, t = avahi_key_to_string(s->key));
    g_free(t);
    
    avahi_elapse_time(&tv, s->sec_delay*1000, 0);
    avahi_time_event_queue_update(s->server->time_event_queue, s->time_event, &tv);
}

struct cbdata {
    AvahiSubscription *subscription;
    AvahiInterface *interface;
};

static gpointer scan_cache_callback(AvahiCache *c, AvahiKey *pattern, AvahiCacheEntry *e, gpointer userdata) {
    struct cbdata *cbdata = userdata;

    g_assert(c);
    g_assert(pattern);
    g_assert(e);
    g_assert(cbdata);

    cbdata->subscription->callback(
        cbdata->subscription,
        e->record,
        cbdata->interface->hardware->index,
        cbdata->interface->protocol,
        AVAHI_SUBSCRIPTION_NEW,
        cbdata->subscription->userdata);

    return NULL;
}

static void scan_interface_callback(AvahiInterfaceMonitor *m, AvahiInterface *i, gpointer userdata) {
    AvahiSubscription *s = userdata;
    struct cbdata cbdata = { s, i };

    g_assert(m);
    g_assert(i);
    g_assert(s);

    avahi_cache_walk(i->cache, s->key, scan_cache_callback, &cbdata);
}

AvahiSubscription *avahi_subscription_new(AvahiServer *server, AvahiKey *key, gint interface, guchar protocol, AvahiSubscriptionCallback callback, gpointer userdata) {
    AvahiSubscription *s, *t;
    GTimeVal tv;

    g_assert(server);
    g_assert(key);
    g_assert(callback);

    g_assert(!avahi_key_is_pattern(key));
    
    s = g_new(AvahiSubscription, 1);
    s->server = server;
    s->key = avahi_key_ref(key);
    s->interface = interface;
    s->protocol = protocol;
    s->callback = callback;
    s->userdata = userdata;
    s->n_query = 1;
    s->sec_delay = 1;

    avahi_server_post_query(s->server, s->interface, s->protocol, s->key);
    
    avahi_elapse_time(&tv, s->sec_delay*1000, 0);
    s->time_event = avahi_time_event_queue_add(server->time_event_queue, &tv, elapse, s);

    AVAHI_LLIST_PREPEND(AvahiSubscription, subscriptions, server->subscriptions, s);

    /* Add the new entry to the subscription hash table */
    t = g_hash_table_lookup(server->subscription_hashtable, key);
    AVAHI_LLIST_PREPEND(AvahiSubscription, by_key, t, s);
    g_hash_table_replace(server->subscription_hashtable, key, t);

    /* Scan the caches */
    avahi_interface_monitor_walk(s->server->monitor, s->interface, s->protocol, scan_interface_callback, s);
    
    return s;
}

void avahi_subscription_free(AvahiSubscription *s) {
    AvahiSubscription *t;
    
    g_assert(s);

    AVAHI_LLIST_REMOVE(AvahiSubscription, subscriptions, s->server->subscriptions, s);

    t = g_hash_table_lookup(s->server->subscription_hashtable, s->key);
    AVAHI_LLIST_REMOVE(AvahiSubscription, by_key, t, s);
    if (t)
        g_hash_table_replace(s->server->subscription_hashtable, t->key, t);
    else
        g_hash_table_remove(s->server->subscription_hashtable, s->key);
    
    avahi_time_event_queue_remove(s->server->time_event_queue, s->time_event);
    avahi_key_unref(s->key);

    
    g_free(s);
}

void avahi_subscription_notify(AvahiServer *server, AvahiInterface *i, AvahiRecord *record, AvahiSubscriptionEvent event) {
    AvahiSubscription *s;
    AvahiKey *pattern;
    
    g_assert(server);
    g_assert(record);

    for (s = g_hash_table_lookup(server->subscription_hashtable, record->key); s; s = s->by_key_next)
        if (avahi_interface_match(i, s->interface, s->protocol))
            s->callback(s, record, i->hardware->index, i->protocol, event, s->userdata);
}

gboolean avahi_is_subscribed(AvahiServer *server, AvahiKey *k) {
    g_assert(server);
    g_assert(k);

    return !!g_hash_table_lookup(server->subscription_hashtable, k);
}