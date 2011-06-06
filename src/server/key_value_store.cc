#include "server/key_value_store.hpp"

#include "btree/rget.hpp"
#include "concurrency/cond_var.hpp"
#include "concurrency/signal.hpp"
#include "concurrency/side_coro.hpp"
#include "concurrency/pmap.hpp"
#include "db_thread_info.hpp"
#include "replication/backfill.hpp"
#include "replication/master.hpp"
#include "cmd_args.hpp"
#include "arch/timing.hpp"

#include <boost/shared_ptr.hpp>

#include <math.h>

static void co_persist_stats(btree_key_value_store_t *, signal_t *); // forward decl

/* shard_store_t */

shard_store_t::shard_store_t(
    translator_serializer_t *translator_serializer,
    mirrored_cache_config_t *dynamic_config,
    int64_t delete_queue_limit) :
    btree(translator_serializer, dynamic_config, delete_queue_limit),
    dispatching_store(&btree),
    timestamper(&dispatching_store)
    { }

get_result_t shard_store_t::get(const store_key_t &key, order_token_t token) {
    on_thread_t th(home_thread());
    sink.check_out(token);
    order_token_t substore_token = substore_order_source.check_in().with_read_mode();
    // We need to let gets reorder themselves, and haven't implemented that yet.
    return btree.get(key, substore_token);
}

rget_result_t shard_store_t::rget(rget_bound_mode_t left_mode, const store_key_t &left_key, rget_bound_mode_t right_mode, const store_key_t &right_key, order_token_t token) {
    on_thread_t th(home_thread());
    sink.check_out(token);
    order_token_t substore_token = substore_order_source.check_in().with_read_mode();
    // We need to let gets reorder themselves, and haven't implemented that yet.
    return btree.rget(left_mode, left_key, right_mode, right_key, substore_token);
}

mutation_result_t shard_store_t::change(const mutation_t &m, order_token_t token) {
    on_thread_t th(home_thread());
    sink.check_out(token);
    order_token_t substore_token = substore_order_source.check_in();
    return timestamper.change(m, substore_token);
}

mutation_result_t shard_store_t::change(const mutation_t &m, castime_t ct, order_token_t token) {
    /* Bypass the timestamper because we already have a castime_t */
    on_thread_t th(home_thread());
    sink.check_out(token);
    order_token_t substore_token = substore_order_source.check_in();
    return dispatching_store.change(m, ct, substore_token);
}

/* btree_key_value_store_t */

void prep_for_serializer(
        btree_key_value_store_dynamic_config_t *dynamic_config,
        btree_key_value_store_static_config_t *static_config,
        int i) {
    /* Prepare the file */
    standard_serializer_t::create(
        &dynamic_config->serializer,
        &dynamic_config->serializer_private[i],
        &static_config->serializer);
}

void create_existing_serializer(
        btree_key_value_store_dynamic_config_t *dynamic_config,
        standard_serializer_t **serializers,
        int i) {

    /* Go to an appropriate thread to run the serializer on */
    on_thread_t thread_switcher(i % get_num_db_threads());
    serializers[i] = new standard_serializer_t(
        &dynamic_config->serializer,
        &dynamic_config->serializer_private[i]);
}

void prep_for_shard(
        translator_serializer_t **pseudoserializers,
        mirrored_cache_static_config_t *static_config,
        int i) {

    // TODO I'm not sure this needs to switch threads at all, though it might
    // increase parallelism that it does. - rntz
    on_thread_t thread_switcher(i % get_num_db_threads());
    btree_slice_t::create(pseudoserializers[i], static_config);
}

void destroy_serializer(standard_serializer_t **serializers, int i) {
    on_thread_t thread_switcher(serializers[i]->home_thread());
    delete serializers[i];
}

void btree_key_value_store_t::create(btree_key_value_store_dynamic_config_t *dynamic_config,
                                     btree_key_value_store_static_config_t *static_config) {

    int n_files = dynamic_config->serializer_private.size();
    rassert(n_files > 0);
    rassert(n_files <= MAX_SERIALIZERS);

    /* Wipe out contents of files and initialize with an empty serializer */
    pmap(n_files, boost::bind(&prep_for_serializer,
        dynamic_config, static_config, _1));

    /* Create serializers so we can initialize their contents */
    standard_serializer_t *serializers[n_files];
    pmap(n_files, boost::bind(&create_existing_serializer,
        dynamic_config, serializers, _1));

    {
        /* Prepare serializers for multiplexing */
        std::vector<serializer_t *> serializers_for_multiplexer(n_files);
        for (int i = 0; i < n_files; i++) serializers_for_multiplexer[i] = serializers[i];
        // Add one slice for the metadata slice
        serializer_multiplexer_t::create(serializers_for_multiplexer, static_config->btree.n_slices + 1);

        /* Create pseudoserializers */
        serializer_multiplexer_t multiplexer(serializers_for_multiplexer);

        /* Initialize the btrees. */
        pmap(multiplexer.proxies.size(), boost::bind(&prep_for_shard, multiplexer.proxies.data(), &static_config->cache, _1));
    }

    /* Shut down serializers */
    pmap(n_files, boost::bind(&destroy_serializer, serializers, _1));
}

void create_existing_shard(
        translator_serializer_t **pseudoserializers,
        shard_store_t **shards,
        mirrored_cache_config_t *dynamic_config,
        int64_t delete_queue_limit,
        int i) {

    // TODO try to align slices with serializers so that when possible, a slice is on the
    // same thread as its serializer
    on_thread_t thread_switcher(i % get_num_db_threads());

    shards[i] = new shard_store_t(pseudoserializers[i], dynamic_config, delete_queue_limit);
}

static mirrored_cache_config_t partition_cache_config(const mirrored_cache_config_t &orig, float share) {
    mirrored_cache_config_t shard = orig;
    shard.max_size = std::max((long long) floorf(orig.max_size * share), 1LL);
    shard.max_dirty_size = std::max((long long) floorf(orig.max_dirty_size * share), 1LL);
    shard.flush_dirty_size = std::max((long long) floorf(orig.flush_dirty_size * share), 1LL);
    shard.io_priority_reads = std::max((int) floorf(orig.io_priority_reads * share), 1);
    shard.io_priority_writes = std::max((int) floorf(orig.io_priority_writes * share), 1);
    return shard;
}

btree_key_value_store_t::btree_key_value_store_t(btree_key_value_store_dynamic_config_t *dynamic_config)
    : hash_control(this) {

    /* Start serializers */
    n_files = dynamic_config->serializer_private.size();
    rassert(n_files > 0);
    rassert(n_files <= MAX_SERIALIZERS);

    for (int i = 0; i < n_files; i++) serializers[i] = NULL;
    pmap(n_files, boost::bind(&create_existing_serializer, dynamic_config, serializers, _1));
    for (int i = 0; i < n_files; i++) rassert(serializers[i]);

    /* Multiplex serializers so we have enough proxy-serializers for our slices */
    std::vector<serializer_t *> serializers_for_multiplexer(n_files);
    for (int i = 0; i < n_files; i++) serializers_for_multiplexer[i] = serializers[i];
    multiplexer = new serializer_multiplexer_t(serializers_for_multiplexer);

    btree_static_config.n_slices = multiplexer->proxies.size() - 1; // subtract 1 for metadata slice

    // calculate what share of the resources we have go to the metadata shard
    float resource_total = 1 + (METADATA_SHARD_RESOURCE_QUOTIENT / btree_static_config.n_slices);
    float shard_share = 1 / (btree_static_config.n_slices * resource_total);
    float metadata_shard_share = METADATA_SHARD_RESOURCE_QUOTIENT / resource_total;

    /* Divide resources among the several slices and the metadata slice */
    mirrored_cache_config_t per_slice_config = partition_cache_config(dynamic_config->cache, shard_share);
    mirrored_cache_config_t metadata_slice_config = partition_cache_config(dynamic_config->cache, metadata_shard_share);
    int64_t per_slice_delete_queue_limit = dynamic_config->total_delete_queue_limit * shard_share;
    int64_t metadata_slice_delete_queue_limit = dynamic_config->total_delete_queue_limit * metadata_shard_share;

    /* Load btrees */
    translator_serializer_t **pseudoserializers = multiplexer->proxies.data();
    pmap(btree_static_config.n_slices,
         boost::bind(&create_existing_shard,
                     pseudoserializers, shards,
                     &per_slice_config, per_slice_delete_queue_limit, _1));

    // Load metadata btree
    // TODO (rntz) code duplication with create_existing_shard
    {
        int i = btree_static_config.n_slices;
        on_thread_t switcher(i % get_num_db_threads());
        metadata_shard = new shard_store_t(pseudoserializers[i],
            &metadata_slice_config, metadata_slice_delete_queue_limit);
    }

    /* Initialize the timestampers to the timestamp value on disk */
    repli_timestamp_t t = get_replication_clock();
    set_timestampers(t);

    // Unpersist stats & create the stat persistence coro
    // TODO (rntz) should this really be in the constructor? what if it errors?
    // But how else can I ensure the first unpersist happens before the first persist?
    persistent_stat_t::unpersist_all(this);
    stat_persistence_side_coro_ptr.reset(
        new side_coro_handler_t(boost::bind(&co_persist_stats, this, _1)));
}

static void set_one_timestamper(shard_store_t **shards, int i, repli_timestamp_t t) {
    // TODO: Do we really need to wait for the operation to finish before returning?
    on_thread_t th(shards[i]->timestamper.home_thread());
    shards[i]->timestamper.set_timestamp(t);
}

void btree_key_value_store_t::set_timestampers(repli_timestamp_t t) {
    pmap(btree_static_config.n_slices, boost::bind(&set_one_timestamper, shards, _1, t));
}

void destroy_shard(
        shard_store_t **shards,
        int i) {

    on_thread_t thread_switcher(shards[i]->home_thread());

    delete shards[i];
}

btree_key_value_store_t::~btree_key_value_store_t() {
    /* Shut down btrees */
    pmap(btree_static_config.n_slices, boost::bind(&destroy_shard, shards, _1));
    // TODO (rntz) hackish reuse of destroy_shard
    destroy_shard(&metadata_shard, 0);

    /* Destroy proxy-serializers */
    delete multiplexer;

    /* Shut down serializers */
    pmap(n_files, boost::bind(&destroy_serializer, serializers, _1));
}

/* Function to check if any of the files seem to contain existing databases */

struct check_existing_fsm_t
    : public standard_serializer_t::check_callback_t
{
    int n_unchecked;
    btree_key_value_store_t::check_callback_t *callback;
    bool is_ok;
    check_existing_fsm_t(const std::vector<std::string>& filenames, btree_key_value_store_t::check_callback_t *cb)
        : callback(cb)
    {
        int n_files = filenames.size();
        n_unchecked = n_files;
        is_ok = true;
        for (int i = 0; i < n_files; i++)
            standard_serializer_t::check_existing(filenames[i].c_str(), this);
    }
    void on_serializer_check(bool ok) {
        is_ok = is_ok && ok;
        n_unchecked--;
        if (n_unchecked == 0) {
            callback->on_store_check(is_ok);
            delete this;
        }
    }
};

void btree_key_value_store_t::check_existing(const std::vector<std::string>& filenames, check_callback_t *cb) {
    new check_existing_fsm_t(filenames, cb);
}


void btree_key_value_store_t::set_replication_clock(repli_timestamp_t t) {

    /* Update the value on disk */
    shards[0]->btree.set_replication_clock(t);
}

repli_timestamp btree_key_value_store_t::get_replication_clock() {
    return shards[0]->btree.get_replication_clock();   /* Read the value from disk */
}

void btree_key_value_store_t::set_last_sync(repli_timestamp_t t) {
    shards[0]->btree.set_last_sync(t);   /* Write the value to disk */
}

repli_timestamp btree_key_value_store_t::get_last_sync() {
    return shards[0]->btree.get_last_sync();   /* Read the value from disk */
}

void btree_key_value_store_t::set_replication_master_id(uint32_t t) {
    shards[0]->btree.set_replication_master_id(t);
}

uint32_t btree_key_value_store_t::get_replication_master_id() {
    return shards[0]->btree.get_replication_master_id();
}

void btree_key_value_store_t::set_replication_slave_id(uint32_t t) {
    shards[0]->btree.set_replication_slave_id(t);
}

uint32_t btree_key_value_store_t::get_replication_slave_id() {
    return shards[0]->btree.get_replication_slave_id();
}

/* Hashing keys and choosing a slice for each key */

/* The following hash function was developed by Paul Hsieh, its source
 * is taken from <http://www.azillionmonkeys.com/qed/hash.html>.
 * According to the site, the source is licensed under LGPL 2.1.
 */
#define get16bits(d) ((((uint32_t)(((const uint8_t *)(d))[1])) << 8)\
                       +(uint32_t)(((const uint8_t *)(d))[0]) )

uint32_t btree_key_value_store_t::hash(const store_key_t &key) {
    const char *data = key.contents;
    int len = key.size;
    uint32_t hash = len, tmp;
    int rem;
    if (len <= 0 || data == NULL) return 0;

    rem = len & 3;
    len >>= 2;

    for (;len > 0; len--) {
        hash  += get16bits (data);
        tmp    = (get16bits (data+2) << 11) ^ hash;
        hash   = (hash << 16) ^ tmp;
        data  += 2*sizeof (uint16_t);
        hash  += hash >> 11;
    }

    switch (rem) {
        case 3: hash += get16bits (data);
                hash ^= hash << 16;
                hash ^= data[sizeof (uint16_t)] << 18;
                hash += hash >> 11;
                break;
        case 2: hash += get16bits (data);
                hash ^= hash << 11;
                hash += hash >> 17;
                break;
        case 1: hash += *data;
                hash ^= hash << 10;
                hash += hash >> 1;
                break;
        default: { }    // this space intentionally left blank
    }

    /* Force "avalanching" of final 127 bits */
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash;
}

uint32_t btree_key_value_store_t::slice_num(const store_key_t &key) {
    return hash(key) % btree_static_config.n_slices;
}

get_result_t btree_key_value_store_t::get(const store_key_t &key, order_token_t token) {
    return shards[slice_num(key)]->get(key, token);
}

typedef merge_ordered_data_iterator_t<key_with_data_provider_t,key_with_data_provider_t::less> merged_results_iterator_t;

rget_result_t btree_key_value_store_t::rget(rget_bound_mode_t left_mode, const store_key_t &left_key, rget_bound_mode_t right_mode, const store_key_t &right_key, order_token_t token) {

    boost::shared_ptr<merged_results_iterator_t> merge_iterator(new merged_results_iterator_t());
    for (int s = 0; s < btree_static_config.n_slices; s++) {
        merge_iterator->add_mergee(shards[s]->rget(left_mode, left_key, right_mode, right_key, token));
    }
    return merge_iterator;
}

/* set_store_interface_t interface */

perfmon_duration_sampler_t pm_store_change_1("store_change_1", secs_to_ticks(1.0));

mutation_result_t btree_key_value_store_t::change(const mutation_t &m, order_token_t token) {
    block_pm_duration timer(&pm_store_change_1);
    return shards[slice_num(m.get_key())]->change(m, token);
}

/* set_store_t interface */

perfmon_duration_sampler_t pm_store_change_2("store_change_2", secs_to_ticks(1.0));

mutation_result_t btree_key_value_store_t::change(const mutation_t &m, castime_t ct, order_token_t token) {
    block_pm_duration timer(&pm_store_change_2);
    return shards[slice_num(m.get_key())]->change(m, ct, token);
}

/* btree_key_value_store_t interface */

void btree_key_value_store_t::delete_all_keys_for_backfill() {
    for (int i = 0; i < btree_static_config.n_slices; ++i) {
        shards[i]->btree.delete_all_keys_for_backfill();
    }
}

// metadata interface
static store_key_t key_from_string(const std::string &key) {
    guarantee(key.size() <= MAX_KEY_SIZE);
    store_key_t sk;
    bool b = str_to_key(key.data(), &sk);
    rassert(b, "str_to_key on key of length < MAX_KEY_SIZE failed");
    return sk;
    (void) b;                   // avoid unused variable warning on release build
}

bool btree_key_value_store_t::get_meta(const std::string &key, std::string *out) {
    store_key_t sk = key_from_string(key);
    // TODO (rntz) should we be worrying about order tokens?
    get_result_t res = metadata_shard->get(sk, order_token_t::ignore);
    // This should only be tripped if a gated store was involved, which it wasn't.
    guarantee(!res.is_not_allowed);
    if (!res.value) return false;

    // Get the data & copy it into the outstring
    const const_buffer_group_t *bufs = res.value->get_data_as_buffers();
    out->assign("");
    out->reserve(bufs->get_size());
    size_t nbufs = bufs->num_buffers();
    for (unsigned i = 0; i < nbufs; ++i) {
        const_buffer_group_t::buffer_t buf = bufs->get_buffer(i);
        out->append((const char *) buf.data, (size_t) buf.size);
    }
    return true;
}

void btree_key_value_store_t::set_meta(const std::string &key, const std::string &value) {
    store_key_t sk = key_from_string(key);
    boost::shared_ptr<buffered_data_provider_t>
        datap(new buffered_data_provider_t((const void*) value.data(), value.size()));

    // TODO (rntz) code dup with run_storage_command :/
    mcflags_t mcflags = 0;      // default, no flags
    // TODO (rntz) what if it's a large value, and needs the LARGE_VALUE flag? how do we determine this?
    exptime_t exptime = 0;      // indicates never expiring

    set_result_t res = metadata_shard->sarc(sk, datap, mcflags, exptime,
        add_policy_yes, replace_policy_yes, // "set" semantics: insert if not present, overwrite if present
        NO_CAS_SUPPLIED, // not a CAS operation
        // TODO (rntz) do we need to worry about ordering?
        order_token_t::ignore);

    // TODO (rntz) consider error conditions more thoroughly
    // For now, we assume "too large" or "not allowed" can't happen.
    guarantee(res == sr_stored);
}

static void pulse_shared_ptr(boost::shared_ptr<cond_t> cvar) {
    cvar->pulse();
}

static void co_persist_stats(btree_key_value_store_t *store, signal_t *shutdown) {
    for (;;) {
        // Could do this without a shared_ptr, but it would be more complicated.
        boost::shared_ptr<cond_t> wakeup(new cond_t());
        cond_link_t linkme(shutdown, wakeup.get());
        call_with_delay(STAT_PERSIST_FREQUENCY_MS, boost::bind(pulse_shared_ptr, wakeup), NULL);
        wakeup->wait_eagerly();
        if (shutdown->is_pulsed()) break;

        // Persist stats
        persistent_stat_t::persist_all(store);
    }
}
