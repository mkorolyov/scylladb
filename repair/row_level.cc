/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "repair/repair.hh"
#include "message/messaging_service.hh"
#include "mutation_fragment.hh"
#include "xx_hasher.hh"
#include "dht/i_partitioner.hh"
#include <vector>
#include <algorithm>
#include <random>
#include <optional>

extern logging::logger rlogger;

struct shard_config {
    unsigned shard;
    unsigned shard_count;
    unsigned ignore_msb;
    sstring partitioner_name;
};

static const std::vector<row_level_diff_detect_algorithm>& suportted_diff_detect_algorithms() {
    static std::vector<row_level_diff_detect_algorithm> _algorithms = {
        row_level_diff_detect_algorithm::send_full_set,
    };
    return _algorithms;
};

static row_level_diff_detect_algorithm get_common_diff_detect_algorithm(const std::vector<gms::inet_address>& nodes) {
    std::vector<std::vector<row_level_diff_detect_algorithm>> nodes_algorithms(nodes.size());
    parallel_for_each(boost::irange(size_t(0), nodes.size()), [&nodes_algorithms, &nodes] (size_t idx) {
        return netw::get_local_messaging_service().send_repair_get_diff_algorithms(netw::messaging_service::msg_addr(nodes[idx])).then(
                [&nodes_algorithms, &nodes, idx] (std::vector<row_level_diff_detect_algorithm> algorithms) {
            std::sort(algorithms.begin(), algorithms.end());
            nodes_algorithms[idx] = std::move(algorithms);
            rlogger.trace("Got node_algorithms={}, from node={}", nodes_algorithms[idx], nodes[idx]);
        });
    }).get();

    auto common_algorithms = suportted_diff_detect_algorithms();
    for (auto& algorithms : nodes_algorithms) {
        std::sort(common_algorithms.begin(), common_algorithms.end());
        std::vector<row_level_diff_detect_algorithm> results;
        std::set_intersection(algorithms.begin(), algorithms.end(),
                common_algorithms.begin(), common_algorithms.end(),
                std::back_inserter(results));
        common_algorithms = std::move(results);
    }
    rlogger.trace("peer_algorithms={}, local_algorithms={}, common_diff_detect_algorithms={}",
            nodes_algorithms, suportted_diff_detect_algorithms(), common_algorithms);
    if (common_algorithms.empty()) {
        throw std::runtime_error("Can not find row level repair diff detect algorithm");
    }
    return common_algorithms.back();
}

static uint64_t get_random_seed() {
    static thread_local std::default_random_engine random_engine{std::random_device{}()};
    static thread_local std::uniform_int_distribution<uint64_t> random_dist{};
    return random_dist(random_engine);
}

class decorated_key_with_hash {
public:
    dht::decorated_key dk;
    repair_hash hash;
    decorated_key_with_hash(const schema& s, dht::decorated_key key, uint64_t seed)
        : dk(key) {
        xx_hasher h(seed);
        feed_hash(h, dk.key(), s);
        hash = repair_hash(h.finalize_uint64());
    }
};

class fragment_hasher {
    const schema& _schema;
    xx_hasher& _hasher;
private:
    void consume_cell(const column_definition& col, const atomic_cell_or_collection& cell) {
        feed_hash(_hasher, col.name());
        feed_hash(_hasher, col.type->name());
        feed_hash(_hasher, cell, col);
    }
public:
    explicit fragment_hasher(const schema&s, xx_hasher& h)
        : _schema(s), _hasher(h) { }

    void hash(const mutation_fragment& mf) {
        if (mf.is_static_row()) {
            consume(mf.as_static_row());
        } else if (mf.is_clustering_row()) {
            consume(mf.as_clustering_row());
        } else if (mf.is_range_tombstone()) {
            consume(mf.as_range_tombstone());
        } else if (mf.is_partition_start()) {
            consume(mf.as_partition_start());
        } else {
            throw std::runtime_error("Wrong type to consume");
        }
    }

private:

    void consume(const tombstone& t) {
        feed_hash(_hasher, t);
    }

    void consume(const static_row& sr) {
        sr.cells().for_each_cell([&] (column_id id, const atomic_cell_or_collection& cell) {
            auto&& col = _schema.static_column_at(id);
            consume_cell(col, cell);
        });
    }

    void consume(const clustering_row& cr) {
        feed_hash(_hasher, cr.key(), _schema);
        feed_hash(_hasher, cr.tomb());
        feed_hash(_hasher, cr.marker());
        cr.cells().for_each_cell([&] (column_id id, const atomic_cell_or_collection& cell) {
            auto&& col = _schema.regular_column_at(id);
            consume_cell(col, cell);
        });
    }

    void consume(const range_tombstone& rt) {
        feed_hash(_hasher, rt.start, _schema);
        feed_hash(_hasher, rt.start_kind);
        feed_hash(_hasher, rt.tomb);
        feed_hash(_hasher, rt.end, _schema);
        feed_hash(_hasher, rt.end_kind);
    }

    void consume(const partition_start& ps) {
        feed_hash(_hasher, ps.key().key(), _schema);
        if (ps.partition_tombstone()) {
            consume(ps.partition_tombstone());
        }
    }
};

class repair_row {
    frozen_mutation_fragment _fm;
    lw_shared_ptr<const decorated_key_with_hash> _dk_with_hash;
    repair_sync_boundary _boundary;
    repair_hash _hash;
    lw_shared_ptr<mutation_fragment> _mf;
public:
    repair_row() = delete;
    repair_row(frozen_mutation_fragment fm,
            position_in_partition pos,
            lw_shared_ptr<const decorated_key_with_hash> dk_with_hash,
            repair_hash hash,
            lw_shared_ptr<mutation_fragment> mf = {})
            : _fm(std::move(fm))
            , _dk_with_hash(std::move(dk_with_hash))
            , _boundary({_dk_with_hash->dk, std::move(pos)})
            , _hash(std::move(hash))
            , _mf(std::move(mf)) {
    }
    mutation_fragment& get_mutation_fragment() {
        if (!_mf) {
            throw std::runtime_error("get empty mutation_fragment");
        }
        return *_mf;
    }
    frozen_mutation_fragment& get_frozen_mutation() { return _fm; }
    const frozen_mutation_fragment& get_frozen_mutation() const { return _fm; }
    const lw_shared_ptr<const decorated_key_with_hash>& get_dk_with_hash() const {
        return _dk_with_hash;
    }
    size_t size() const {
        return _fm.representation().size();
    }
    const repair_sync_boundary& boundary() const {
        return _boundary;
    }
    const repair_hash& hash() const {
        return _hash;
    }
};

class repair_reader {
public:
using is_local_reader = bool_class<class is_local_reader_tag>;

private:
    schema_ptr _schema;
    dht::partition_range _range;
    // Used to find the range that repair master will work on
    dht::selective_token_range_sharder _sharder;
    // Seed for the repair row hashing
    uint64_t _seed;
    // Local reader or multishard reader to read the range
    flat_mutation_reader _reader;
    // Current partition read from disk
    lw_shared_ptr<const decorated_key_with_hash> _current_dk;

public:
    repair_reader(
            seastar::sharded<database>& db,
            column_family& cf,
            dht::token_range range,
            dht::i_partitioner& local_partitioner,
            dht::i_partitioner& remote_partitioner,
            unsigned remote_shard,
            uint64_t seed,
            is_local_reader local_reader)
            : _schema(cf.schema())
            , _range(dht::to_partition_range(range))
            , _sharder(remote_partitioner, range, remote_shard)
            , _seed(seed)
            , _reader(make_reader(db, cf, local_partitioner, local_reader)) {
    }

private:
    flat_mutation_reader
    make_reader(seastar::sharded<database>& db,
            column_family& cf,
            dht::i_partitioner& local_partitioner,
            is_local_reader local_reader) {
        if (local_reader) {
            return cf.make_streaming_reader(_schema, _range);
        }
        return make_multishard_streaming_reader(db, local_partitioner, _schema, [this] {
            auto shard_range = _sharder.next();
            if (shard_range) {
                return std::optional<dht::partition_range>(dht::to_partition_range(*shard_range));
            }
            return std::optional<dht::partition_range>();
        });
    }

public:
    future<mutation_fragment_opt>
    read_mutation_fragment() {
        return _reader(db::no_timeout);
    }

    lw_shared_ptr<const decorated_key_with_hash>& get_current_dk() {
        return _current_dk;
    }

    void set_current_dk(const dht::decorated_key& key) {
        _current_dk = make_lw_shared<const decorated_key_with_hash>(*_schema, key, _seed);
    }

    void clear_current_dk() {
        _current_dk = {};
    }

    void check_current_dk() {
        if (!_current_dk) {
            throw std::runtime_error("Current partition_key is unknown");
        }
    }

};
